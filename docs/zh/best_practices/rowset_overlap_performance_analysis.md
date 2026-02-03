---
displayed_sidebar: docs
keywords: ['rowset', 'overlap', 'compaction', 'query performance', '查询性能']
---

# Overlapping Rowset vs Non-overlapping Rowset 对查询性能的影响分析

本文档详细分析了 StarRocks 中 Overlapping Rowset（重叠Rowset）和 Non-overlapping Rowset（非重叠Rowset）的区别，以及它们对查询性能的具体影响。

## 概述

在 StarRocks 的存储引擎中，每次数据导入都会生成一个 Rowset，一个 Rowset 可以包含多个 Segment 文件。根据 Segment 之间数据主键范围是否存在重叠，Rowset 可以分为两种类型：

- **Overlapping Rowset（重叠Rowset）**：Rowset 内部的多个 Segment 之间存在主键范围重叠
- **Non-overlapping Rowset（非重叠Rowset）**：Rowset 内部的多个 Segment 之间主键范围互不重叠

## 技术定义

### Overlapping Rowset 的判定条件

根据 StarRocks 源代码中 `rowset_meta.h` 的定义，一个 Rowset 被判定为 Overlapping 需要同时满足以下条件：

1. Rowset 包含超过 1 个 Segment
2. Rowset 是 singleton delta（即 start_version == end_version，表示是单次导入产生的）
3. segments_overlap 标志不是 NONOVERLAPPING

```cpp
// 源码位置: be/src/storage/rowset/rowset_meta.h
bool is_segments_overlapping() const {
    return num_segments() > 1 && is_singleton_delta() && segments_overlap() != NONOVERLAPPING;
}
```

### Non-overlapping Rowset 的特点

Non-overlapping Rowset 通常由以下方式产生：

1. **Compaction 后的 Rowset**：Compaction 过程会对数据进行排序合并，产生有序的 Non-overlapping Rowset
2. **单 Segment 的 Rowset**：只有一个 Segment 的 Rowset 天然不存在重叠问题
3. **排序导入**：通过 Stream Load 等方式导入已排序的数据

## 对查询性能的影响

### 1. 读放大效应

#### Overlapping Rowset 的问题

当查询需要读取 Overlapping Rowset 时，由于 Segment 之间存在主键重叠，查询引擎需要：

- **多路归并**：需要同时打开所有相关的 Segment，进行多路归并排序
- **更多的 I/O 操作**：每个 Segment 都需要单独读取，增加磁盘 I/O 次数
- **更高的内存消耗**：需要为每个 Segment 维护独立的迭代器和缓冲区

```cpp
// 源码位置: be/src/storage/lake/horizontal_compaction_task.cpp
// Overlapping Rowset 需要按 segment 数量计算输入
total_input_segs += rowset->is_overlapped() ? rowset->num_segments() : 1;
```

#### Non-overlapping Rowset 的优势

对于 Non-overlapping Rowset，查询引擎可以：

- **Union 迭代**：使用 Union Iterator 顺序读取各个 Segment，无需归并
- **更少的 I/O**：可以利用数据的有序性进行更高效的数据跳过
- **更低的内存开销**：只需维护一个活跃的迭代器

```cpp
// 源码位置: be/src/storage/lake/rowset.cpp
if (segment_iterators.size() > 1 && !is_overlapped()) {
    // union non-overlapped segment iterators - 更高效
    auto iter = new_union_iterator(std::move(segment_iterators));
    return std::vector<ChunkIteratorPtr>{iter};
}
```

### 2. Compaction Score 的计算差异

Compaction Score 用于评估 Tablet 的数据健康程度和 Compaction 优先级：

| Rowset 类型 | Compaction Score 计算方式 |
|------------|-------------------------|
| Overlapping | Score = Segment 数量 |
| Non-overlapping | Score = 1 |

```cpp
// 源码位置: be/src/storage/rowset/rowset_meta.h
uint32_t get_compaction_score() const {
    uint32_t score = 0;
    if (!is_segments_overlapping()) {
        score = 1;  // Non-overlapping 只计 1 分
    } else {
        score = num_segments();  // Overlapping 按 segment 数计分
        CHECK(score > 0);
    }
    return score;
}
```

这意味着：
- 10 个 Overlapping Segment 的 Rowset，Score = 10
- 10 个 Non-overlapping Segment 的 Rowset，Score = 1

### 3. 查询时的 Iterator 数量影响

在计算查询所需的读取迭代器数量时：

```cpp
// 源码位置: be/src/storage/lake/rowset.cpp
if (segment_num > 1 && !is_overlapped()) {
    return 1;  // Non-overlapping 只需 1 个迭代器
} else {
    return segment_num;  // Overlapping 需要多个迭代器
}
```

**性能影响**：

| 场景 | Overlapping Rowset | Non-overlapping Rowset |
|------|-------------------|----------------------|
| 10 个 Segment | 10 个迭代器 | 1 个迭代器 |
| 内存占用 | 高 | 低 |
| CPU 开销 | 高（归并排序） | 低（顺序读取） |
| I/O 模式 | 随机读取 | 顺序读取 |

### 4. 主键表的特殊影响

对于主键表（Primary Key Table），Overlapping Rowset 的影响更为显著：

```cpp
// 源码位置: be/src/storage/lake/primary_key_compaction_policy.h
// IO count = overlapped segment count + delvec files
// 同样的数据量，更多的 IO 意味着更大的开销
double io_count() const {
    // 对于已经足够大的 non-overlapped rowset，返回 0 表示不需要 compaction
    if (!rowset_meta_ptr->overlapped() && stat.num_dels == 0) {
        int64_t rowset_size = static_cast<int64_t>(rowset_meta_ptr->data_size());
        if (rowset_size >= large_rowset_threshold) {
            return 0;
        }
    }
    // ...
}
```

主键表中：
- **Overlapping Rowset** 需要额外处理 Delete Vector（删除向量）
- 每次查询都需要检查数据是否被删除
- 增加了额外的 I/O 和 CPU 开销

## 性能对比量化

### 典型场景分析

假设一个 Tablet 有 100 个 Segment，分布在 10 个 Rowset 中：

**场景 A：全部是 Overlapping Rowset**

| 指标 | 值 |
|------|-----|
| 总 Segment 数 | 100 |
| 查询迭代器数 | 100 |
| Compaction Score | 100 |
| 内存占用（相对） | 100% |
| 查询延迟（相对） | 高 |

**场景 B：全部是 Non-overlapping Rowset**

| 指标 | 值 |
|------|-----|
| 总 Segment 数 | 100 |
| 查询迭代器数 | 10 |
| Compaction Score | 10 |
| 内存占用（相对） | ~10% |
| 查询延迟（相对） | 低 |

### SQL Profile 中的诊断

在慢查询分析中，可以通过以下指标判断是否受 Overlapping Rowset 影响：

```
SegmentsReadCount / TabletCount
```

- 如果该值很大（例如几十以上），说明可能是 Compaction 不及时导致的 Overlapping Rowset 过多

## 最佳实践

### 1. 导入策略优化

- **增大批次大小**：避免频繁的小批量导入，减少 Rowset 数量
- **延长导入间隔**：建议导入间隔不低于 10 秒
- **预排序数据**：导入前对数据进行排序，减少 Segment 间重叠

### 2. Compaction 配置优化

```sql
-- 查看 Compaction 配置
ADMIN SHOW FRONTEND CONFIG LIKE "%lake_compaction%";

-- 调整 Compaction 任务数
ADMIN SET FRONTEND CONFIG ("lake_compaction_max_tasks" = "128");
```

建议：
- 将 `compact_threads` 设置为 CPU 核心数的 25%
- 将 `max_cumulative_compaction_num_singleton_deltas` 设置为 100

### 3. 监控和告警

定期检查 Compaction Score：

```sql
-- 查看分区的 Compaction Score
SHOW PARTITIONS FROM <table_name>;

-- 通过系统视图查看
SELECT * FROM information_schema.partitions_meta 
ORDER BY Max_CS DESC 
LIMIT 10;
```

告警阈值建议：
- Max_CS < 10：健康
- Max_CS 10-100：正常
- Max_CS > 100：需要关注
- Max_CS > 500：需要人工介入

### 4. 强制触发 Compaction

当发现 Overlapping Rowset 过多时，可以手动触发 Compaction：

```sql
-- 触发整个表的 Compaction
ALTER TABLE <table_name> COMPACT;

-- 触发特定分区的 Compaction
ALTER TABLE <table_name> COMPACT <partition_name>;
```

## 总结

| 特性 | Overlapping Rowset | Non-overlapping Rowset |
|------|-------------------|----------------------|
| 数据有序性 | Segment 间无序 | Segment 间有序 |
| 查询性能 | 较差 | 较好 |
| 内存占用 | 高 | 低 |
| I/O 模式 | 多路归并 | 顺序读取 |
| Compaction Score | 高（按 segment 计数） | 低（计为 1） |
| 产生原因 | 频繁小批量导入 | Compaction 后 |

**核心结论**：Non-overlapping Rowset 在查询性能上显著优于 Overlapping Rowset。通过合理的导入策略和及时的 Compaction，可以有效减少 Overlapping Rowset 的比例，从而提升整体查询性能。
