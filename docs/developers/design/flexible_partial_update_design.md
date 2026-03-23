# Flexible Partial Update 设计文档

## 1. 背景与动机

### 1.1 当前现状

StarRocks Primary Key 表当前支持 Partial Update（部分列更新），但要求 **同一批次（batch）中所有行更新的列集合必须相同**。列集合在 Load/INSERT 时通过以下方式确定：

- Stream Load: 通过 `columns` header 和 `partial_update=true` 指定
- INSERT: 通过 `INSERT INTO table(col1, col2, ...) VALUES (...)` 指定
- UPDATE: 通过 SET 子句隐式确定

内部通过 `TPartialUpdateMode` 枚举（ROW_MODE / COLUMN_UPSERT_MODE / COLUMN_UPDATE_MODE / AUTO_MODE）控制更新模式，更新列集合记录在 `RowsetTxnMetaPB.partial_update_column_ids` 中，对整个 Rowset 统一生效。

### 1.2 问题

实际业务场景中，CDC（Change Data Capture）或事件驱动的更新常遇到以下情况：

```
Row 1: pk=1, col_a=10, col_b=20          -- 只更新 col_a, col_b
Row 2: pk=2, col_c=30                     -- 只更新 col_c
Row 3: pk=3, col_a=40, col_d=50, col_e=60 -- 更新 col_a, col_d, col_e
```

同一批数据中，不同行需要更新不同的列。当前方案下用户必须：
1. 按列组合拆分为多个 Load 任务 —— 增加系统开销和复杂度
2. 补全所有列（union of all columns）+ 使用默认值填充 —— 导致大量不必要的写放大

### 1.3 目标

支持 **Flexible Partial Update**：单次 Load/INSERT 中不同行可以更新不同的列子集，系统自动识别每行需要更新的列。

## 2. 业界方案参考

### 2.1 Apache Doris — Flexible Column Update

Doris 在 3.1 版本引入了 Flexible Partial Update，核心设计：

| 维度 | 方案 |
|------|------|
| **触发方式** | Stream Load 设置 `flexible_partial_update=true`，支持 JSON 格式 |
| **列存在性判断** | JSON 输入天然通过 key 的有无判断哪些列存在；对于非 JSON 格式使用特殊标记值 |
| **写入路径** | "加载时补全缺失字段 + 全行写入"（load-time missing field completion followed by full-row writing） |
| **底层实现** | 基于 Unique Key Model + Merge-on-Write，`VerticalSegmentWriter` 中增加 `_append_block_with_flexible_partial_content` |
| **放大效应** | 100 列表更新 10 列 → 约 9x 读放大 + 10x 写放大 |

**核心限制**：Doris 的方案本质上仍然是"按行补全后全行写入"，写放大严重。

### 2.2 ClickHouse — 多种方案

| 方案 | 机制 | 特点 |
|------|------|------|
| **CoalescingMergeTree** | 非 key 列标记为 Nullable，NULL 表示"未更新"，merge 时取 latest non-null | 需要所有列 Nullable；merge 前查询需解析多版本 |
| **Lightweight Update（Patch Parts）** | 只写变更列的 patch part，merge 时合并 | 25.7 新特性，只写变更列，写放大最小 |
| **ReplacingMergeTree** | 全行插入 + 去重 | 不支持部分列更新 |

**ClickHouse Lightweight Update 的 Patch Part 机制**与 StarRocks 的 Delta Column Group（DCG）概念高度相似——都是只写变更列的增量文件，merge/compaction 时合并。

### 2.3 设计选型

综合考虑 StarRocks 已有架构和性能要求：

| 方案 | 写放大 | 读放大 | 实现复杂度 | 选择 |
|------|--------|--------|------------|------|
| **A: Doris 式全行补全写入** | 高 | 低 | 低 | 不选 |
| **B: 基于 DCG 的 per-row 变更列记录** | 低 | 中 | 中 | **推荐** |
| **C: NULL-coalescing（类 ClickHouse）** | 低 | 高（merge 前） | 低 | 不选（侵入性大） |

**推荐方案 B**：扩展现有 Column Mode Partial Update + Delta Column Group 机制，增加 per-row 的列存在性 bitmap，使同一 segment 中不同行可以有不同的更新列集合。

## 3. 概要设计

### 3.1 核心思路

```
┌────────────────────────────────────────────────────────┐
│          Flexible Partial Update 数据流                  │
│                                                        │
│  输入: JSON/CSV with per-row varying columns           │
│    ↓                                                   │
│  FE: 解析出所有可能列的 superset                        │
│    ↓                                                   │
│  BE Writer: 写入 segment                               │
│    ├── 数据列: 只写实际存在的列值（缺失列不写/写默认值）  │
│    └── Column Existence Bitmap: 每行一个 bitmap          │
│         标记该行哪些非 key 列有实际值                     │
│    ↓                                                   │
│  Commit: 生成 DCG + bitmap 元信息                       │
│    ↓                                                   │
│  Read/Merge: 通过 bitmap 判断每行每列是否需要            │
│              从历史版本补全                              │
└────────────────────────────────────────────────────────┘
```

### 3.2 Column Existence Bitmap

引入一个隐藏列 `__column_existence_bitmap`，类型为 `VARBINARY`（或 `BitmapValue`），存储在 segment 中。

对于表有 N 个非 key 列，bitmap 的第 i 位表示该行的第 i 个非 key 列是否有实际更新值：
- bit = 1: 该列有值，从当前 segment 读取
- bit = 0: 该列无值，需要从历史版本（或默认值）补全

```
Schema: pk, col_a, col_b, col_c, col_d

Row 1: pk=1, col_a=10, col_b=20
  bitmap = 1100  (col_a=1, col_b=1, col_c=0, col_d=0)

Row 2: pk=2, col_c=30
  bitmap = 0010  (col_a=0, col_b=0, col_c=1, col_d=0)

Row 3: pk=3, col_a=40, col_d=50
  bitmap = 1001  (col_a=1, col_b=0, col_c=0, col_d=1)
```

### 3.3 数据格式变更

#### 3.3.1 Protobuf（olap_file.proto）

```protobuf
enum PartialUpdateMode {
    UNKNOWN_MODE = 0;
    ROW_MODE = 1;
    COLUMN_UPSERT_MODE = 2;
    AUTO_MODE = 3;
    COLUMN_UPDATE_MODE = 4;
    FLEXIBLE_MODE = 5;            // 新增
}

message RowsetTxnMetaPB {
    // ... existing fields ...

    // For FLEXIBLE_MODE: 所有可能被更新的列（superset）
    repeated uint32 flexible_update_column_ids = 9;
    repeated uint32 flexible_update_column_unique_ids = 10;
    // bitmap 列在 segment 中的 column unique id
    optional uint32 column_existence_bitmap_uid = 11;
}
```

#### 3.3.2 Thrift（Types.thrift / DataSinks.thrift）

```thrift
enum TPartialUpdateMode {
    UNKNOWN_MODE = 0;
    ROW_MODE = 1;
    COLUMN_UPSERT_MODE = 2;
    AUTO_MODE = 3;
    COLUMN_UPDATE_MODE = 4;
    FLEXIBLE_MODE = 5;            // 新增
}
```

#### 3.3.3 Segment 存储格式

在 FLEXIBLE_MODE 下，segment 存储的列包括：
1. **所有 Key 列** — 必须完整
2. **Superset 中所有 value 列** — 每列的数据按行存储，缺失行填充 default value（或 NULL）
3. **`__column_existence_bitmap` 列** — BitmapColumn，记录每行实际更新了哪些列

> 注意：缺失列虽然物理上在 segment 中存储了默认值占位，但 bitmap 标记为 0，
> 在后续 merge 时会被忽略，从历史版本读取正确值。
>
> 另一种优化方案：只写实际有值的列，不同行写入不同的列子集。
> 这需要更大的 segment writer 改动，可作为后续优化。

### 3.4 FE 侧变更

#### 3.4.1 Stream Load / Routine Load

新增 HTTP header / 参数：

```
partial_update_mode: flexible
```

当 `partial_update_mode=flexible` 时：
- FE 不再要求 `columns` header 指定固定列列表
- 对于 JSON 格式：自动从每行的 key 集合推断更新列
- 对于 CSV 格式：需要额外的 `columns` header 指定 superset 列，加上特殊 NULL 标记表示缺失

#### 3.4.2 INSERT 语句

```sql
-- 方式1: 通过 session variable 启用
SET partial_update_mode = 'flexible';
INSERT INTO table (pk, col_a, col_b, col_c) VALUES
  (1, 10, 20, NULL),    -- NULL 表示不更新 col_c
  (2, NULL, NULL, 30);  -- NULL 表示不更新 col_a, col_b

-- 需要区分 "更新为 NULL" 和 "不更新"
-- 方案: 引入特殊标记函数 __skip()
INSERT INTO table (pk, col_a, col_b, col_c) VALUES
  (1, 10, 20, __skip()),
  (2, __skip(), __skip(), 30);
```

> **推荐首期只支持 JSON 格式的 Stream Load**，因为 JSON 天然支持字段缺失的语义，
> 无需引入 `__skip()` 等特殊语法。CSV 和 INSERT 语句可后续版本扩展。

#### 3.4.3 LoadPlanner / InsertPlanner 变更

- `LoadPlanner` 识别 `FLEXIBLE_MODE`，将所有列（superset）加入 TupleDescriptor
- 通过 `TOlapTableSink.partial_update_mode = FLEXIBLE_MODE` 传递给 BE
- 传递 superset 列信息至 BE 用于 bitmap 构建

### 3.5 BE 侧变更

#### 3.5.1 写入路径

```
StreamLoadPipe → RowBatch (varying columns per row)
       ↓
  ChunkBuilder (JSON Parser)
    - 对每行 JSON，解析出实际存在的 key
    - 构建 column_existence_bitmap
    - 对缺失列填充 default placeholder
       ↓
  DeltaWriter
    - DeltaWriterOptions.partial_update_mode = FLEXIBLE_MODE
    - 将 bitmap 列作为 hidden column 附加到 Chunk
       ↓
  SegmentWriter
    - 正常写所有 superset 列 + bitmap 列
    - RowsetTxnMetaPB 记录 flexible mode 元信息
```

**核心类变更：**

| 类 | 变更 |
|-----|------|
| `DeltaWriter` | 支持 `FLEXIBLE_MODE`，构建 bitmap |
| `SegmentWriter` | 写入 bitmap hidden column |
| `RowsetWriterContext` | 传递 FLEXIBLE_MODE 和 superset 列信息 |
| `JsonScanner/JsonReader` | 按行解析 JSON key，生成 bitmap |

#### 3.5.2 Apply / Merge 路径（Column Mode Partial Update Handler）

扩展 `ColumnModePartialUpdateHandler`：

```
Commit 阶段:
  对每个 segment:
    1. 通过 PK index 查找每行的历史版本
    2. 读取 column_existence_bitmap
    3. 对于 bitmap=0 的列:
       - 如果行在历史版本存在 → 从历史版本读取该列值
       - 如果行不存在（新插入行） → 使用 default value
    4. 生成 Delta Column Group (DCG):
       - 按列分组，将补全后的值写入 .cols 文件
       - 或直接 rewrite segment 为完整行
```

**与现有 COLUMN_UPSERT_MODE 的关键区别：**

| 维度 | COLUMN_UPSERT_MODE | FLEXIBLE_MODE |
|------|---------------------|---------------|
| 缺失列集合 | 整个 segment 统一 | 每行不同 |
| 补全方式 | 批量读取所有缺失列 | 按行按 bitmap 读取不同列 |
| DCG 生成 | 为统一的缺失列集合生成 | 需要 per-row 差异化处理 |

**补全策略优化：**

为避免逐行逐列读取，可以进行分组优化：

```
1. 扫描所有行的 bitmap，统计列组合模式
2. 将相同 bitmap 模式的行分组
3. 对每组批量读取相同的缺失列集合
4. 批量补全
```

这将 per-row 的随机 I/O 退化为按 bitmap 模式分组的批量 I/O。

#### 3.5.3 Compaction 路径

Compaction 时遇到 FLEXIBLE_MODE 的 rowset：

1. 读取 bitmap 列
2. 结合 DCG 和历史版本数据，生成完整行
3. 输出到新 rowset（已 resolved，不再需要 bitmap）

#### 3.5.4 查询路径

查询时遇到 FLEXIBLE_MODE rowset（尚未 compacted）：

1. 通过 DCG 机制（已有）获取最新列值
2. bitmap 信息已在 apply 阶段消费，apply 后生成的 DCG 可直接被查询路径复用
3. 无需查询路径感知 bitmap（apply 阶段已完成补全）

### 3.6 整体数据流时序

```
                  FE                              BE
                  │                               │
  Stream Load     │   TOlapTableSink              │
  (JSON, flexible)│   partial_update_mode=FLEXIBLE│
  ───────────────>│──────────────────────────────>│
                  │                               │
                  │                    ┌──────────┤
                  │                    │ JSON Parser:
                  │                    │   parse per-row keys
                  │                    │   build bitmap
                  │                    │   fill defaults
                  │                    ├──────────┤
                  │                    │ SegmentWriter:
                  │                    │   write superset cols
                  │                    │   write bitmap col
                  │                    ├──────────┤
                  │                    │ Publish/Apply:
                  │                    │   PK lookup
                  │                    │   read bitmap
                  │                    │   group by bitmap pattern
                  │                    │   batch fill missing cols
                  │                    │   generate DCGs
                  │                    └──────────┤
                  │                               │
                  │   Txn Commit                  │
  <───────────────│<──────────────────────────────│
```

## 4. 分阶段实施计划

### Phase 1: JSON Stream Load + Column Mode

**目标**：最小可用版本，只支持 JSON 格式的 Stream Load。

1. **Thrift/Proto**: 新增 `FLEXIBLE_MODE` 枚举值
2. **FE Stream Load**: 解析 `partial_update_mode=flexible` 参数
3. **BE JsonScanner**: 按行解析 JSON key，构建 bitmap
4. **BE SegmentWriter**: 写入 bitmap hidden column
5. **BE ColumnModePartialUpdateHandler**: 扩展 apply 逻辑，支持 per-row bitmap
6. **测试**: Stream Load JSON + flexible partial update 端到端测试

### Phase 2: 性能优化

1. **Bitmap 分组优化**: 按 bitmap pattern 对行分组，批量补全
2. **稀疏列优化**: 只在 segment 中写有值的列，bitmap=0 的列不写入占位
3. **内存管理**: bitmap 列的内存开销控制

### Phase 3: 扩展输入格式

1. **CSV 格式支持**: 通过 sentinel value 标记缺失列
2. **INSERT 语句支持**: `__skip()` 函数或 session variable 语义
3. **Routine Load / Broker Load 支持**

## 5. 与现有功能的兼容性

| 功能 | 兼容性 |
|------|--------|
| 现有 Partial Update (ROW_MODE/COLUMN_MODE) | 完全兼容，FLEXIBLE_MODE 是新增模式 |
| Conditional Update (merge_condition) | 需验证，bitmap 需在条件判断后生效 |
| Auto Increment 列 | 兼容，auto increment 列始终自动填充 |
| Generated Column | 兼容，generated column 依赖列需在 bitmap 中标记 |
| Schema Change | 需处理列增减时 bitmap 的映射关系 |
| 主键索引 | 无变更 |
| 物化视图刷新 | 无变更（基于 rowset 增量） |

## 6. 风险与注意事项

1. **写放大权衡**: FLEXIBLE_MODE 的初期实现（Phase 1）中，segment 仍写入 superset 所有列的占位值，与 Doris 方案类似有一定写放大。Phase 2 的稀疏列优化可显著减少。

2. **Apply 性能**: per-row 不同的缺失列会导致 apply 阶段 I/O 模式更随机，需要 bitmap 分组优化来缓解。

3. **bitmap 存储开销**: 对于宽表（数百列），每行 bitmap 需要 ceil(N/8) 字节。100 列表每行仅 13 字节，相对于数据本身开销很小。

4. **Compaction 代价**: FLEXIBLE_MODE rowset 的 compaction 需要读取 bitmap 和可能的多个 DCG，比普通 rowset 更重，但与现有 column mode partial update 的 compaction 复杂度相当。
