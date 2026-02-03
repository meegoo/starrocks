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

## 为什么需要多路归并

### 核心问题：数据有序性与聚合需求

多路归并（Heap Merge）并不是所有查询都需要的。它的使用取决于**表类型**和**查询场景**。

### 什么时候需要多路归并

根据源代码 `tablet_reader.cpp` 中的实现逻辑：

#### 情况 1：Compaction 过程（所有表类型）

```cpp
// 源码位置: be/src/storage/tablet_reader.cpp
if (is_compaction(params.reader_type) && keys_type == DUP_KEYS) {
    _collect_iter = new_heap_merge_iterator(seg_iters);
}
```

Compaction 需要将多个 Rowset 合并成一个有序的 Rowset，必须使用归并排序。

#### 情况 2：AGG_KEYS / UNIQUE_KEYS 表的聚合查询

```cpp
// 源码位置: be/src/storage/tablet_reader.cpp
} else if ((keys_type == AGG_KEYS || keys_type == UNIQUE_KEYS) && !skip_aggr) {
    // 需要先归并再聚合
    _collect_iter = new_heap_merge_iterator(seg_iters);
    _collect_iter = new_aggregate_iterator(std::move(_collect_iter), 0);
}
```

对于聚合表和更新表，需要将相同主键的数据归并后进行聚合计算。

#### 情况 3：需要全局排序的查询（sorted_by_keys_per_tablet）

```cpp
// 源码位置: be/src/storage/tablet_reader.cpp
if (params.sorted_by_keys_per_tablet && (keys_type == DUP_KEYS || keys_type == PRIMARY_KEYS)) {
    _collect_iter = new_heap_merge_iterator(seg_iters);
}
```

当显式要求返回排序结果时，才需要归并。

### 什么时候不需要多路归并

#### DUP_KEYS 和 PRIMARY_KEYS 的普通查询

```cpp
// 源码位置: be/src/storage/tablet_reader.cpp (第 458-472 行)
} else if (keys_type == PRIMARY_KEYS || keys_type == DUP_KEYS || 
           (keys_type == UNIQUE_KEYS && skip_aggr) ||
           (select_all_keys && seg_iters.size() == 1)) {
    //             UnionIterator  <-- 使用顺序读取，不归并！
    //                   |
    //       +-----------+-----------+
    //       |           |           |
    // SegmentIterator  ...    SegmentIterator
    _collect_iter = new_union_iterator(std::move(seg_iters));
}
```

**重要结论**：对于 `PRIMARY_KEYS` 和 `DUP_KEYS` 表的普通查询，**不需要多路归并**，直接使用 `union_iterator` 顺序读取！

### 查询场景总结

| 表类型 | 普通查询 | Compaction | 聚合查询 | 排序查询 |
|--------|----------|------------|----------|----------|
| DUP_KEYS | Union（不归并） | **Merge** | N/A | **Merge** |
| PRIMARY_KEYS | Union（不归并） | **Merge** | N/A | **Merge** |
| UNIQUE_KEYS | Union（skip_aggr）| **Merge** | **Merge** | **Merge** |
| AGG_KEYS | Union + 预聚合 | **Merge** | **Merge** | **Merge** |

### 多路归并 vs 顺序读取的工作原理

#### 多路归并（Heap Merge Iterator）

```
     输入: 3 个 Segment（需要有序输出时使用）
     
     Segment1: [1, 5, 9, 13]      当前指针 → 1
     Segment2: [2, 6, 10, 14]     当前指针 → 2  
     Segment3: [3, 7, 11, 15]     当前指针 → 3
                    ↓
              最小堆比较
                    ↓
     输出: [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15]
```

**工作流程**：
1. 为每个 Segment 创建一个迭代器
2. 将每个迭代器的当前行放入最小堆
3. 每次取出堆顶（最小值），输出到结果
4. 从对应 Segment 读取下一行，重新入堆
5. 重复直到所有数据读取完毕

**时间复杂度**：O(N * log(K))，其中 N 是总行数，K 是 Segment 数量

#### 顺序读取（Union Iterator）

```
     输入: 3 个 Segment（普通查询时使用）
     
     Segment1: [1, 5, 9]
     Segment2: [2, 6, 10]
     Segment3: [3, 7, 11]
                    ↓
              顺序拼接（不排序）
                    ↓
     输出: [1, 5, 9, 2, 6, 10, 3, 7, 11]  (按 Segment 顺序输出)
```

**工作流程**：
1. 按顺序读取第一个 Segment 的所有数据
2. 读完后切换到下一个 Segment
3. 重复直到所有 Segment 读取完毕

**时间复杂度**：O(N)，线性扫描

## Overlapping 对性能的真正影响

### 关键澄清

**对于 DUP_KEYS 和 PRIMARY_KEYS 表的普通查询，Overlapping 与否不影响是否使用归并！** 普通查询都使用 `union_iterator` 顺序读取。

Overlapping 的真正影响体现在以下方面：

### 1. Compaction 过程中的效率

在 Compaction 时，需要对 Rowset 内部的 Segment 进行处理：

```cpp
// 源码位置: be/src/storage/rowset_merger.cpp
if (rowset->rowset_meta()->is_segments_overlapping()) {
    // Overlapping: 需要先对 Rowset 内部的 Segment 做归并
    entry.segment_itr = new_heap_merge_iterator(res.value(), entry.need_rssid_rowids);
} else {
    // Non-overlapping: 可以直接顺序读取
    entry.segment_itr = new_union_iterator(res.value());
}
```

#### Overlapping Rowset 在 Compaction 时的问题

- **更多的迭代器**：每个 Segment 需要独立的迭代器
- **堆归并开销**：需要 O(N * log(K)) 的比较操作
- **更高的内存消耗**：需要为每个 Segment 维护独立的缓冲区

#### Non-overlapping Rowset 在 Compaction 时的优势

- **单个迭代器**：整个 Rowset 只需要一个 Union Iterator
- **线性扫描**：O(N) 的时间复杂度
- **更低的内存开销**：只需维护一个活跃的缓冲区

```cpp
// 源码位置: be/src/storage/lake/horizontal_compaction_task.cpp
// Compaction 时计算输入 Segment 数量
total_input_segs += rowset->is_overlapped() ? rowset->num_segments() : 1;
```

### 2. 迭代器数量的计算

```cpp
// 源码位置: be/src/storage/lake/rowset.cpp
if (segment_num > 1 && !is_overlapped()) {
    return 1;  // Non-overlapping: 整个 Rowset 算 1 个迭代器
} else {
    return segment_num;  // Overlapping: 每个 Segment 算 1 个迭代器
}
```

这影响：
- 并行度计算
- 内存预估
- Compaction 任务调度

### 3. Compaction Score 的计算差异

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

### 4. 主键表 Compaction 优先级

对于主键表（Primary Key Table），Overlapping 状态影响 Compaction 优先级的计算：

```cpp
// 源码位置: be/src/storage/lake/primary_key_compaction_policy.h
// IO count = overlapped segment count + delvec files
// 同样的数据量，更多的 IO 意味着更大的 Compaction 开销
double io_count() const {
    // 对于已经足够大的 non-overlapped rowset，返回 0 表示不需要 compaction
    if (!rowset_meta_ptr->overlapped() && stat.num_dels == 0) {
        int64_t rowset_size = static_cast<int64_t>(rowset_meta_ptr->data_size());
        if (rowset_size >= large_rowset_threshold) {
            return 0;  // 大型 non-overlapped rowset 不需要再 compaction
        }
    }
    // ...
}
```

主键表中 Non-overlapping Rowset 的优势：
- 如果已经足够大且没有删除数据，可以跳过 Compaction
- 减少不必要的 I/O 和 CPU 开销

## 性能对比量化

### Compaction 场景分析

假设一个 Tablet 有 100 个 Segment，分布在 10 个 Rowset 中：

**场景 A：全部是 Overlapping Rowset**

| 指标 | 值 |
|------|-----|
| 总 Segment 数 | 100 |
| Compaction 迭代器数 | 100 |
| Compaction Score | 100 |
| Compaction 内存占用（相对） | 100% |
| Compaction 效率（相对） | 低 |

**场景 B：全部是 Non-overlapping Rowset**

| 指标 | 值 |
|------|-----|
| 总 Segment 数 | 100 |
| Compaction 迭代器数 | 10 |
| Compaction Score | 10 |
| Compaction 内存占用（相对） | ~10% |
| Compaction 效率（相对） | 高 |

### 普通查询场景（DUP_KEYS/PRIMARY_KEYS）

对于这两种表类型的普通查询，Overlapping 状态**不影响查询性能**，因为都使用 `union_iterator`。

但是，高 Compaction Score 意味着：
- 有更多的小文件需要读取
- 文件元数据开销增加
- 可能触发导入限流（当 Score > 100）

### SQL Profile 中的诊断

在慢查询分析中，可以通过以下指标判断是否受小文件过多影响：

```
SegmentsReadCount / TabletCount
```

- 如果该值很大（例如几十以上），说明可能是 Compaction 不及时，产生了过多的小文件
- 这会增加文件元数据开销，即使不需要归并排序

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

### Overlapping 的真正影响

| 特性 | Overlapping Rowset | Non-overlapping Rowset |
|------|-------------------|----------------------|
| 数据有序性 | Segment 间数据重叠 | Segment 间数据不重叠 |
| **普通查询（DUP/PK）** | **无影响** | **无影响** |
| Compaction 效率 | 较低（需要归并） | 较高（顺序读取） |
| Compaction Score | 高（按 segment 计数） | 低（计为 1） |
| Compaction 内存 | 高 | 低 |
| 产生原因 | 频繁小批量导入 | Compaction 后 |

### 关键结论

1. **普通查询不受影响**：对于 DUP_KEYS 和 PRIMARY_KEYS 表，Overlapping 与否**不影响普通查询性能**，都使用顺序读取（union_iterator）。

2. **Compaction 是主要影响点**：Overlapping Rowset 会增加 Compaction 的开销，因为需要对 Rowset 内部的 Segment 进行归并排序。

3. **高 Compaction Score 的间接影响**：
   - 文件数量多，元数据开销增加
   - 可能触发导入限流（Score > 100）
   - 可能导致导入拒绝（Score > 2000）

4. **AGG_KEYS/UNIQUE_KEYS 不同**：这两种表类型的聚合查询需要归并，因此小文件多会直接影响查询性能。

### 最佳实践建议

通过合理的导入策略和及时的 Compaction，可以：
- 减少 Compaction 的资源消耗
- 避免导入限流和拒绝
- 减少文件元数据开销
