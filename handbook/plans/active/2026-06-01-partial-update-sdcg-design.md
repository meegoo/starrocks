# Sparse Delta Column Group (SDCG) —— 稀疏列上的 Partial Update 设计

- Status: research / pre-design
- Owner: TBD
- Last Updated: 2026-06-01

---

## 0. Executive Summary

本文是 **StarRocks 主键表 partial update 的下一代设计提案**,目标场景是 **CDC + 半结构化数据 + 高频小批量更新**——这是 ClickHouse Lightweight Update、Doris Flexible Partial Update、Apache Paimon partial-update merge engine、Apache Hudi PartialUpdateAvroPayload、Apache Pinot partial upsert 等系统正在共同探索的赛道。

核心提案是 **SDCG (Sparse Delta Column Group)**:在 StarRocks 既有 DCG (`.cols`) 之外引入 **稀疏增量文件 `.spcols`**——每列各自携带 rowid 集合,与 base segment 通过物理 rowid 对齐,无需 mask、无需占位值。读路径深度融入 StarRocks 已有的 `SparseRange<>` row-range + late materialization + page-level zone map 三大基础设施;写路径在 dense/sparse 之间按密度阈值自动选择;并复用现有 `VariantColumnMerger` 把 Variant **path 级 partial update** 作为 StarRocks 独家差异化能力。

核心赌注:**StarRocks 在 OLAP 多模数据(结构化 + Variant)+ 实时 CDC(异构列) 双战场上,单一稀疏 patch 模型(CK 路线) 不够用,单一 bitmap 模型(Doris 路线)代价不可接受,必须双态 + path 级 + 谓词下推保真**——这是目前业界无人完整覆盖的设计空间。

---

## 1. 背景与动机

### 1.1 StarRocks 现状的两个痛点

源码确认的事实(`be/src/storage/`):

1. **`AUTO_MODE` 形同虚设** —— `be/src/storage/delta_writer.cpp:132` 注释:
   ```cpp
   // In the current implementation, UNKNOWN_MODE and AUTO_MODE can be considered as ROW_MODE
   ```
   FE 默认下发 `AUTO_MODE`,BE 一律走 ROW_MODE(读完整旧行 + 写完整新行)。"自动"是空头支票。

2. **DCG 在小批量更新上写放大严重** —— `be/src/storage/rowset_column_update_state.cpp:319` `read_from_source_segment_and_update` **全量扫源 segment**,即使只更新 100 行也要把 100 万行的列读出来再写回。

3. **DCG 存在即关闭 segment-level zone map filter** —— `be/src/storage/rowset/segment.cpp:288-308`:
   ```cpp
   bool Segment::_use_segment_zone_map_filter(...) {
       ...
       return st.ok() && dcgs.size() == 0;   // 有 DCG 直接返回 false
   }
   ```
   有 partial update 历史的表,所有列的 zone map pushdown 全部失效,查询读 IO 可能 100× 放大。

### 1.2 目标场景特征

- **CDC 同步**:上游消息只带 PK + 变更列(2–10 列),目标表上百列
- **多源宽表**:多条 CDC 流融合,**单批次内不同行更新不同列**
- **半结构化高频更新**:Variant/JSON 内某条 path 高频改,其余 path 不动
- **运维诉求**:写吞吐 ≥ 10k QPS、查询 P99 不抖、上游 DB 配置零侵入

### 1.3 现有方案为何不够

| 方案 | 致命问题 |
|---|---|
| StarRocks ROW_MODE | 读整行 + 写整行,小批量场景 N× 写放大 |
| StarRocks COLUMN_MODE | 读源 segment 整列,M× 写放大;关闭 zone map |
| Doris Flexible (skip_bitmap) | base 每行恒带 bitmap,永久存储税;写时需读历史(高 IOPS) |
| ClickHouse Patch Parts | 单批不支持异构列;无 Variant path 级;source part merge 后退化 hash join |
| Snowflake/Databricks/Iceberg | COW 整文件重写,根本不是轻量更新 |

---

## 2. 业界方案调研

### 2.1 ClickHouse Lightweight Update (Patch Parts)

**源码**: `src/Storages/MergeTree/PatchParts/` (clone at `https://github.com/ClickHouse/ClickHouse`)

- **物理形态**: 独立 MergeTree part,分区名 `patch-<hash(column_names)>-<orig_partition_id>`
- **系统列**: `_part`、`_part_offset`、`_block_number`、`_block_offset`、`_data_version` (`PatchPartsUtils.cpp:109-137`)
- **两种 apply mode** (`PatchPartInfo.h:68-74`):
  ```cpp
  enum class PatchMode { Merge, Join };
  ```
  - Merge: 按 `(_part, _part_offset)` 排序合并,几乎零开销
  - Join: 按 `(_block_number, _block_offset)` hash 关联,慢 39%–121%,patch 必须装内存
- **多版本仲裁**: ReplacingMergeTree on `_data_version` (`applyPatches.cpp:148-180`)
- **物化**: `apply_patches_on_merge` 在 background merge 时把 patch 物化进 base
- **限制**: 单 UPDATE 的 SET 列固定,**任何接口都不支持单批次 per-row 异构列**

ClickHouse Cloud (SharedMergeTree + ClickPipes) 与 OSS 共用同一存储语义,在 partial update 上没有 cloud-only 增强。ClickPipes Postgres CDC 在 TOAST 列场景下需要 `REPLICA IDENTITY FULL`,这会显著放大 WAL。

### 2.2 Apache Doris Flexible Partial Update (3.1+)

**源码**: `be/src/storage/segment/vertical_segment_writer.cpp:738` `_append_block_with_flexible_partial_content`

- **隐藏列**: `__DORIS_SKIP_BITMAP_COL__` per row, bit=1 表示该列在该行未更新
- **必须**: `enable_unique_key_merge_on_write=true` + `enable_unique_key_skip_bitmap_column=true` + VerticalSegmentWriter
- **入口**: 仅 Stream Load / Routine Load / Flink Connector + JSON

**关键事实(源码验证,修正了我之前的误解)**: Doris flexible 是 **写时合并**,不是读时合并。

写路径(`partial_update_info.cpp:566-708`):
1. **批内折叠**: `MemTable::_aggregate_for_flexible_partial_update_*` (`memtable.cpp:594`) 把同 batch 同 PK 多版本折叠
2. **PK 索引点查**: `_generate_flexible_read_plan` (`vertical_segment_writer.cpp:948`) 找历史 RowLocation
3. **历史回填**: `fill_non_primary_key_cell_for_column_store` 按 skip_bitmap 决定取历史值还是输入值
4. **写完整行**: `vertical_segment_writer.cpp:868` 写出 full_block

源码注释 `vertical_segment_writer.cpp:842` 自证:
```cpp
// this column is not needed in read path for merge-on-write table
```

**skip_bitmap 在 MoW 读路径完全不用**——读取就是普通 MoW(PK 索引直达完整行)。

**代价**: 写时高 IOPS,文档明示需开 row store 缓解;并发冲突走 publish-期 transient rewrite (`base_tablet.cpp:1489` `create_transient_rowset_writer`),热点 PK 高并发下 publish 阶段抖动严重。

### 2.3 其他系统快速归纳

| 系统 | 异构列表达 | 物化时机 | StarRocks 可借鉴点 |
|---|---|---|---|
| **Apache Paimon** | null 哨兵 + sequence-group | compaction | 多 sequence group 让不同列组各自定序 |
| **Apache Hudi** | null 哨兵 (PartialUpdateAvroPayload) | MoR compaction | payload class 插件式扩展 |
| **Apache Pinot** | per-column merge strategy (OVERWRITE/INCREMENT/...) | 实时 in-memory | 列级合并函数 |
| **Apache Kudu** | UPSERT 缺列 = 保留历史 | per-column DeltaFile | 列级差分存储 |
| **Snowflake / Databricks / Iceberg** | COW 整文件重写 | 写时 | 不入选(非轻量) |
| **SingleStore** | 行存 in-place / 列存 segment 合并 | 即时/异步 | 不入选 |

四种主流"异构列"表达模式:
- **A 显式 bitmap** (Doris): 强表达,base 永久开销
- **B null 哨兵 + sequence group** (Paimon/Hudi): 协议简单,丢失真 null 语义
- **C per-column merge strategy** (Pinot): 列级聚合表达强,内存型
- **D 独立 patch 文件** (CK): 写轻,跨语句异构,单语句固定

SDCG 路线 = D + 在 patch 内挂 per-col rowid 集合 = **patch 文件方案的稀疏度维度从"行稀疏"扩展到"行×列双稀疏"**,获得 A 的表达力但不付永久 base 开销。

---

## 3. StarRocks 现状源码地图

### 3.1 Partial Update 入口

**Thrift 定义** (`gensrc/thrift/Types.thrift:568`):
```thrift
enum TPartialUpdateMode {
    UNKNOWN_MODE = 0;
    ROW_MODE = 1;
    COLUMN_UPSERT_MODE = 2;
    AUTO_MODE = 3;
    COLUMN_UPDATE_MODE = 4;
}
```

**FE 入口**:
- `fe/fe-core/src/main/java/com/starrocks/sql/InsertPlanner.java:456` — 下发 AUTO_MODE
- `fe/fe-core/src/main/java/com/starrocks/load/streamload/StreamLoadKvParams.java:232`
- `fe/fe-core/src/main/java/com/starrocks/load/loadv2/BrokerLoadJob.java:291`

**BE 入口**:
- `be/src/storage/delta_writer.cpp:132` — AUTO_MODE 退化到 ROW_MODE 的位置
- `be/src/storage/tablet_updates.cpp:1314-1324` — COLUMN vs ROW 路径分支

### 3.2 ROW Mode 路径

入口: `TabletUpdates::_apply_normal_rowset_commit` (`tablet_updates.cpp:1346`)

读历史:
- `be/src/storage/rowset_update_state.cpp:373` `_prepare_partial_update_states`
- `be/src/storage/rowset_update_state.cpp:279` `plan_read_by_rssid`
- 走 `tablet->updates()->get_rss_rowids_by_pk` 拿到 (rssid, rowid)
- 然后**实际读历史列值**填补缺失列

写出: 完整新行,旧行 delete bitmap 标删

### 3.3 COLUMN Mode 路径 (DCG)

入口: `TabletUpdates::_apply_column_partial_update_commit` (`tablet_updates.cpp:1133`)

核心类:
- `be/src/storage/rowset_column_update_state.h:68` `ColumnPartialUpdateState`
- `be/src/storage/rowset_column_update_state.h:140` `RowsetColumnUpdateState`
- `be/src/storage/delta_column_group.h:35` `DeltaColumnGroup`

写路径(关键代码):
- `rowset_column_update_state.cpp:180` `_prepare_partial_update_states`:仅 PK 索引点查,**不读列值**
- `rowset_column_update_state.cpp:230` `_resolve_conflict`:发现 latest_applied_version 变化时仅重查 PK index
- `rowset_column_update_state.cpp:319` `read_from_source_segment_and_update`:**全量扫源 segment 整列**(写放大根源)
- `rowset_column_update_state.cpp:450` `_update_source_chunk_by_upt`:用 `Chunk::update_rows` 覆盖
- `rowset_column_update_state.cpp:735-825` `finalize`:写 `.cols` + 生成 DCG 元数据

DCG 文件格式 (`delta_column_group.h:64`):
```
$1_$2_$3_$4.cols
  $1 = rowsetid, $2 = segment id, $3 = version, $4 = seq suffix
```

DCG 内含**稠密整列数据**——源 segment 的所有 N 行,更新行有新值,未更新行有从 base 读出的原值。

DCG 元数据:
```cpp
class DeltaColumnGroup {
    int64_t _version;
    std::vector<std::vector<ColumnUID>> _column_uids;   // 列组分桶
    std::vector<std::string> _column_files;
    ...
};
```

合并(`delta_column_group.cpp:65`): `merge_by_version` 仅合并同 version 的 DCG。

### 3.4 读路径

`be/src/storage/rowset/segment_iterator.cpp`:

- 入口: `_new_dcg_column_iterator` (line 1138)
- DCG 查找: `_get_dcg_segment` (line 1120) —— 按版本降序找最新含该列的 DCG
- DCG 缓存: `_dcg_segments` map (line 1127)
- Zone map 关闭点: `segment.cpp:288-308` `_use_segment_zone_map_filter`

### 3.5 并发模型

StarRocks 主键表:
- `be/src/storage/tablet_updates.cpp` 的 `_index_lock` 保证单 tablet 的 apply 串行
- `_resolve_conflict` 在版本变化时只重查 PK index、重建 rss_rowid 映射,**无须重写**

对比 Doris 的 publish-期 transient rewrite,StarRocks 现状的冲突解决**已经更轻**。SDCG 沿用此模型。

### 3.6 Variant 类型现状

`TYPE_VARIANT = 55` (`be/src/types/logical_type.h:73`),完整基础设施已就绪:

| 资产 | 路径 |
|---|---|
| 类型定义 | `be/src/types/variant.h/.cpp`、`variant_value.h/.cpp` |
| 列存 | `be/src/column/variant_column.h/.cpp` |
| Path 解析 | `be/src/column/variant_path_parser.h/.cpp` |
| **Variant 合并(关键)** | `be/src/column/variant_merger.h/.cpp` `VariantColumnMerger` |
| 编解码/转换 | `be/src/column/variant_encoder.*`、`variant_converter.*` |

`VariantColumnMerger` 注释明示其设计目的:
> "For shredded columns, run an explicit schema pre-alignment check before append."
> "For overlapping shredded paths with numeric type conflicts, widen to a common type."

→ **path 级 partial update 所需的合并算法已就绪**,SDCG 不需要从零造合并器。

### 3.7 Lake 路径

存在并行实现 `be/src/storage/lake/column_mode_partial_update_handler.{h,cpp}`,与本地 `RowsetColumnUpdateState` 是平行类。任何 SDCG 改动两边都要做——是抽公共 helper 的关键动机。

---

## 4. SDCG v1.2 设计

### 4.1 总览

```
SDCG v1.2 组件图
├─ 物理存储
│   ├─ .cols       (dense, 现状,向后兼容)
│   └─ .spcols     (sparse, 新增, per-col rowid + value)
├─ 元数据
│   ├─ DeltaColumnGroupPB 扩展
│   │   ├─ FileKind ∈ {DENSE_COLS, SPARSE_PERCOL}
│   │   └─ ExtendedColumnRef = (column_uid, optional<variant_path>)
│   └─ 反向索引 col_uid → DCG[] (iterator 启动时构建)
├─ 写路径 (helper 共享)
│   ├─ density 阈值二选一
│   ├─ rowid 集合 hash 等价类分组
│   ├─ inline-PB (K ≤ 4)
│   └─ ColumnPatchSink (SQL UPDATE 入口)
├─ 读路径 (helper 共享)
│   ├─ LayeredOverlayIterator (K 路归并)
│   ├─ Dense pruning (遇 dense 截断后续 sparse)
│   ├─ Presence bitmap fast-path
│   ├─ 向量化 update_rows apply
│   ├─ Effective ZoneMap (segment + page 级)
│   ├─ Late materialization 协同
│   └─ Read-time merge cache
├─ Variant path 级
│   └─ 复用 VariantColumnMerger
├─ 后台收敛
│   ├─ Sparse → sparse 合并 worker
│   ├─ Sparse → dense promotion
│   └─ 读频驱动优先级
└─ 双引擎共享
    └─ be/src/storage/partial_update/ helper 模块
```

### 4.2 物理文件格式

#### `.cols` (dense, 现状不变)

`$rowsetid_$segid_$version_$suffix.cols`,Segment v2 文件,**整列稠密**——源 segment 所有 N 行的该列值(更新行新值,未更新行原值)。

#### `.spcols` (sparse, 新增)

`$rowsetid_$segid_$version_$suffix.spcols`,Segment v2 文件,**列各自携带 rowid 集合**:

```
SegmentFooter
   ColumnMeta for "source_rowid": UInt32, sorted, has zone map + bloom filter
   ColumnMeta for "col_a":
       - sub-meta: presence_bitmap (Roaring) 引用
       - data:     [va_at_row_100, va_at_row_200, ...]  (K_a rows)
       - zone_map: 仅反映 K_a 个值
       - bloom_filter: 仅反映 K_a 个值
   ColumnMeta for "col_b":
       - sub-meta: presence_bitmap
       - data:     [vb_at_row_305, ...]                   (K_b rows)
   ...
```

关键性质:
- 每列独立的 rowid 集合 → 单批内 row 100 改 col_a、row 305 改 col_b 天然表达
- 无 mask、无占位值
- 列存压缩对每列的 (rowid, value) 各自优化

#### Roaring Presence Bitmap

每列一个 Roaring bitmap (复用 `be/src/util/bitmap_value.h`),记录该列覆盖的 rowid 集合。用于:
- 快速 `rangeCardinality` 判断 page 是否被 overlay 命中
- 与 `SparseRange<>` 求交,做 row-range 级裁剪
- 多版本合并时的快速交集/并集

#### 列分组优化

writer 检测 rowid 集合等价类(hash-bucket):

```
group_1 (rowid 完全相同的列共享 source_rowid 列):
   source_rowid: [100, 305]
   col_a:        [va100, va305]
   col_b:        [vb100, vb305]
group_2:
   source_rowid: [200]
   col_c:        [vc200]
```

退化场景:同列集合 batch update → 单组,等价于现有稠密路径(完全兼容)。

### 4.3 元数据扩展

`DeltaColumnGroupPB` (`gen_cpp/olap_common.pb`、`gen_cpp/lake_types.pb`) 新增:

```protobuf
message DeltaColumnGroupPB {
    repeated DeltaColumnGroupColumnIdsPB column_ids = 1;
    repeated string column_files = 2;
    repeated string encryption_metas = 3;
    int64 file_size = 4;

    // === SDCG v1.2 新增 ===
    repeated FileKind file_kinds = 5;          // DENSE_COLS | SPARSE_PERCOL
    repeated int64 sparse_row_counts = 6;       // 稀疏文件真实 K
    repeated ExtendedColumnRefPB extended_refs = 7;  // 含 variant_path
    optional InlineSparsePatchPB inline_patch = 8;   // K ≤ inline_threshold 时
}

enum FileKind {
    DENSE_COLS = 0;          // 默认,向后兼容
    SPARSE_PERCOL = 1;
}

message ExtendedColumnRefPB {
    int32 column_uid = 1;
    optional string variant_path = 2;
}
```

C++ 端:

```cpp
// be/src/storage/delta_column_group.h
class DeltaColumnGroup {
    int64_t _version = 0;
    std::vector<std::vector<ColumnUID>> _column_uids;
    std::vector<std::string> _column_files;
    std::vector<std::string> _encryption_metas;
    int64_t _file_size = 0;
    // 新增
    std::vector<FileKind> _file_kinds;
    std::vector<int64_t> _sparse_row_counts;
    std::vector<std::vector<ExtendedColumnRef>> _extended_refs;
    std::optional<InlineSparsePatch> _inline_patch;
};

struct ExtendedColumnRef {
    ColumnUID column_uid;
    std::optional<VariantPath> variant_path;
    bool operator==(...) const;
    uint64_t hash() const;
};
```

### 4.4 写路径

#### 4.4.1 入口统一

两种数据源进同一 writer 流程:

| 入口 | 行级列子集表达 |
|---|---|
| Stream Load JSON | 每条 JSON 自带 key 集合 → 每行已知列集合 |
| SQL UPDATE | 单语句 SET 列固定,所有 WHERE 命中行同列 |
| MERGE 语句(可选) | 多 WHEN MATCHED 分支映射到多列组 |

#### 4.4.2 决策树

```
RowsetColumnUpdateState::finalize() 改造:
    
    // 1. PK 索引点查 (现状不变)
    _prepare_partial_update_states(...);
    
    // 2. 计算每个源 segment 的命中密度
    for each source segment S:
        K_S = 命中 S 的更新行数
        density = K_S / S.num_rows
        
    // 3. 列分组等价类 (新增)
    col_groups = group_columns_by_rowid_set(update_columns, partial_update_states);
    
    // 4. 路径选择 (per source segment per column group)
    for each (source_segment, col_group):
        if density < dense_threshold:
            if K_S ≤ inline_threshold:
                # inline 到 PB
                write_inline_sparse_patch(...);
            else:
                # 独立稀疏文件
                write_sparse_percol_file(...);
        else:
            # 走现状稠密路径
            write_dense_cols_file(...);   // 调用 read_from_source_segment_and_update
```

#### 4.4.3 稀疏写入实现

```cpp
Status write_sparse_percol_file(
        uint32_t source_segment_id,
        const std::vector<RowidPairs>& source_to_upt,  // 已排序 source_rowid
        const std::vector<ColumnUID>& cols,
        Rowset* rowset) {
    // 完全不访问源 segment
    auto writer = create_spcols_writer(...);
    
    // source_rowid 列
    writer->append_column("source_rowid", extract_rowids(source_to_upt));
    
    // 每个更新列:从 .upt 文件按 upt_rowid 拉值
    for (auto col_uid : cols) {
        auto upt_iter = rowset->get_update_file_iterator(...);
        auto values = read_selective(upt_iter, source_to_upt.upt_rowids);
        writer->append_column(col_uid, values);
    }
    
    // presence bitmap 也写入
    writer->finalize_with_presence_bitmap();
    
    return Status::OK();
}
```

写入代价: O(K × num_updated_cols)。**完全不读源 segment 列值**——这是相比现状最大节省。

#### 4.4.4 ColumnPatchSink (SQL UPDATE 入口)

FE 改造 `UpdatePlanner`:

```java
// fe/fe-core/.../UpdatePlanner.java
public Plan plan(UpdateStmt stmt) {
    int setColCount = stmt.getSetClauses().size();
    double estimatedSelectivity = estimateSelectivity(stmt.getWhere());
    
    if (enableSparseDcgUpdate && 
        setColCount <= sparseDcgUpdateMaxColumns &&
        estimatedSelectivity <= sparseDcgUpdateDensityThreshold) {
        return buildColumnPatchSinkPlan(stmt);   // 新路径
    }
    return buildLegacyReadModifyWritePlan(stmt);  // 现状路径
}
```

`ColumnPatchSink` 流程:
1. SELECT 阶段:`SELECT pk_columns, set_exprs FROM t WHERE pred` (**不读其他列**)
2. BE 写入:接收 chunk → PK 索引点查 → 按 (rssid, rowid) 分组 → 调 `SparseColsWriter`
3. 生成 SDCG 元数据
4. 走 publish

对比现状 ROW_MODE:省掉"先 SELECT 整行再写整行"的 N× 写放大。

### 4.5 读路径

#### 4.5.1 整体流程

```
SegmentIterator init:
    1. dcg_loader.load(...)  → _dcgs (现状)
    2. 构建反向索引 col_uid → ordered DCG list[]   (新)
    3. 对每个读取列:
       - 查反向索引
       - 应用 dense pruning (找到最新 dense 即截断后续 sparse)
       - 构造 LayeredOverlayIterator (无 DCG → 原生 column iterator)
       
SegmentIterator next_batch:
    - 各列 iterator 按 row range 取数据
    - LayeredOverlayIterator 内部:
        - 整 page presence bitmap 不交 → fast path (原生扫描)
        - 整 page 有交 → 读 base + 向量化 update_rows apply
        - 多版本 → 按版本降序应用 (有 merge cache 时直接用 cached overlay)
```

#### 4.5.2 LayeredOverlayIterator

```cpp
class LayeredOverlayIterator : public ColumnIterator {
public:
    Status next_batch(size_t* n, Column* dst, const SparseRange<>& range) override {
        // 1. base iterator 读出整段
        RETURN_IF_ERROR(_base_iter->next_batch(n, dst, range));
        
        // 2. 对每个 DCG 层 (按版本降序,已经过 dense pruning)
        for (auto& layer : _layers) {
            // 2a. presence bitmap 与 range 求交
            auto hit_rowids = layer.presence_bitmap & range_to_bitmap(range);
            if (hit_rowids.empty()) continue;  // fast path
            
            // 2b. 把命中 rowid 转 local index (在 dst 内的位置)
            auto local_idx = compute_local_indices(hit_rowids, range);
            
            // 2c. 读 overlay value 列
            auto overlay_values = layer.read_values_for(hit_rowids);
            
            // 2d. 向量化覆盖 (复用 Column::update_rows,与写路径同款)
            dst->update_rows(*overlay_values, local_idx.data());
        }
        return Status::OK();
    }
    
    // 实现现有 zone map row range 接口
    Status get_row_ranges_by_zone_map(...) override {
        // 用 effective zone map 评估,产出 page 级 row range
    }
    
private:
    std::unique_ptr<ColumnIterator> _base_iter;
    std::vector<OverlayLayer> _layers;  // 已剪枝 + 版本降序
};
```

#### 4.5.3 Dense Pruning

```cpp
auto collect_layers(ColumnUID uid) {
    auto& dcgs = _col_to_dcgs[uid];  // 已按版本降序
    std::vector<OverlayLayer> layers;
    for (auto& dcg_ref : dcgs) {
        layers.push_back(make_layer(dcg_ref));
        if (dcg_ref.is_dense()) {
            // dense 替换 base 之上的所有 sparse → 停止
            break;
        }
    }
    return layers;
}
```

#### 4.5.4 Effective ZoneMap

```cpp
// Segment 级 (改造 segment.cpp:288)
bool Segment::_use_segment_zone_map_filter(...) {
    return true;  // 总是允许;具体判断由 SegmentZoneMapPruner 用 effective ZM 做
}

// 新增 helper
ZoneMap Segment::get_effective_zone_map(uint32_t ucid, ...) {
    auto layers = collect_layers(ucid);
    
    if (layers.empty()) return base_zone_map(ucid);
    
    // 最新 dense 替换 base
    if (layers[0].is_dense()) {
        ZoneMap zm = layers[0].zone_map();
        // 叠加更新的 sparse
        for (auto& l : remaining_sparse_above_dense) {
            zm.union_with(l.zone_map());
        }
        return zm;
    }
    
    // 无 dense: base ∪ all sparse
    ZoneMap zm = base_zone_map(ucid);
    for (auto& l : layers) zm.union_with(l.zone_map());
    return zm;
}
```

**正确性**: `(min, max)` 区间或 distinct value zonemap 在 union 后,predicate `P` 的"非命中"判定永远是 over-include 方向——**只多读不漏读,等价于 P 的下推不会漏行**。Bloom filter 同样安全。

Page 级 ZM 下推:同思路下沉到 `ColumnReader::get_row_ranges_by_zone_map` (复用 `segment_iterator.cpp:1106`),让 OverlayColumnIterator 实现该接口。

#### 4.5.5 读优化叠加

| 优化 | 复用源码 | 效果 |
|---|---|---|
| Presence bitmap fast-path | Roaring `rangeCardinality` | 未命中 page 走原生 base iterator,零 overlay 开销 |
| 向量化 `update_rows` | `column.h:198` `update_rows` (写路径同款) | 命中 page 一次 gather-blend,非 per-row 分支 |
| 列裁剪 | `_column_access_paths` | 未读列零成本 |
| Read-time merge cache | `_dcg_segments` map 扩展 | K 路归并每 segment 一次,跨 query 摊销 |
| Late materialization 协同 | `_predicate_late_materialization_scan_strategy` | overlay 只对存活行做 |
| 字典共享 | LowCardinality 编码 | 枚举列免解码 |
| Page cache | StarRocks 现有 page cache | `.spcols` page 一并缓存 |

### 4.6 Variant Path 级 Partial Update

**目标**: 业界(CK / Doris / Snowflake / Databricks / Iceberg) **没有任何系统支持**——StarRocks 独家。

**机制**:
- `ExtendedColumnRef = (column_uid, optional<variant_path>)` 把 path 当虚拟列
- writer:按 `(uid, path)` 计算 rowid 集合等价类,各自落 `.spcols`
- reader:在 OverlayColumnIterator 之上挂一层 path overlay,复用 `VariantColumnMerger` 做类型选举和 shredded 合并

**关键复用**: `VariantColumnMerger` 已有的"shredded path 合并 + 数值类型扩宽 + 冲突识别"逻辑直接用,只需扩展输入接口接受 sparse rowid + path tuple。

### 4.7 并发模型

**沿用现状,不引入新机制**:
- `_index_lock` 保证 apply 串行
- `_resolve_conflict` 在 latest_applied_version 变化时重查 PK index、重建 (rssid, rowid) 映射 (`rowset_column_update_state.cpp:230`)
- 不需要 Doris 的 publish-期 transient rewrite (StarRocks 的冲突解决本来就更轻)

**SDCG 沿用的不变式**:
- conflict resolution 必须在 `.spcols` 写出之前完成
- 一旦 .spcols 写出,其内部 source_rowid 已基于最新 PK 映射

### 4.8 后台收敛

#### 4.8.1 Sparse → sparse 合并

```
触发: 同 (rssid) 上同 column_uid 的 sparse DCG ≥ N (e.g. 8)
动作: 把多个 .spcols 按版本顺序合并成单一 .spcols
      - rowid 集合并集
      - 同 rowid 取最新值
      - 输出 presence bitmap 合并
合并后: 仍是 SPARSE_PERCOL kind,版本设为最大版本
```

#### 4.8.2 Sparse → dense promotion

```
触发: 单 segment 累积总 K / segment 行数 ≥ promote_threshold (e.g. 0.3)
   或: sparse 文件数 ≥ hard_promote_count (e.g. 16)
动作: 把所有 sparse DCG + base 合并出新 dense .cols
      → 等价于现状 dense 路径的一次性付清
```

#### 4.8.3 主 compaction 协同

不变。主 compaction (cumulative / base) 时把 base + 所有 DCG 物化成新 base segment,DCG 被 GC (复用 `delta_column_group.h:146` `garbage_collection`)。

#### 4.8.4 读频驱动优先级

```
每个 segment 维护统计:
  sparse_version_count, scan_frequency, read_overlay_us

promotion priority = scan_frequency × sparse_version_count × overlay_cost
→ 热点优先 promote,冷数据保持 sparse
```

### 4.9 双引擎统一: Helper 模块

#### 4.9.1 目录结构

```
be/src/storage/partial_update/      # 新增模块
├── helper.h
├── sparse_writer.cpp               # SparseColsWriter
├── dense_writer.cpp                # DenseColsWriter (封装现状)
├── layered_overlay_iterator.cpp   # 读路径核心
├── effective_zone_map.cpp          # ZM/BF 合成
├── rowid_classifier.cpp            # 列等价类
├── extended_column_ref.cpp         # variant path 支持
└── promotion_worker.cpp            # 后台收敛

调用方:
├── be/src/storage/rowset_column_update_state.cpp   # 本地引擎
└── be/src/storage/lake/column_mode_partial_update_handler.cpp  # Lake
```

#### 4.9.2 模块边界 (`be/module_boundary_manifest.json`)

```json
{
  "name": "PartialUpdate",
  "include_prefixes": [
    "storage/partial_update/", "storage/", "column/", "types/",
    "common/", "base/", "gutil/", "gen_cpp/"
  ],
  "target_deps": [
    "ColumnCore", "TypesCore", "Common", "Base", "Gutil", "StarRocksGen"
  ],
  "core_tests": ["partial_update_test"]
}
```

storage/ 和 storage/lake/ 都 depend on `PartialUpdate`。

#### 4.9.3 抽象与引擎相关

Helper 持有(引擎无关):
- 列等价类划分算法
- 稀疏 `.spcols` 文件读写格式
- LayeredOverlayIterator K 路归并
- Effective ZoneMap 合成
- ExtendedColumnRef + variant path 处理

调用方持有(引擎相关):
- 文件系统访问(local: FileSystem;lake: LakeIoOptions + 对象存储)
- PK 索引访问(local: PrimaryIndex;lake: LakeMetaStore 间接)
- Conflict resolution 时机
- Compaction 调度

---

## 5. 性能影响分析

### 5.1 与 StarRocks 现状对比矩阵

| # | 维度 | 现状 | SDCG | 变化 | 风险 |
|---|---|---|---|---|---|
| 1 | 写: 小批量稀疏 | ROW M×N / DCG M×cols | sparse K×cols | **显著改善** | — |
| 2 | 写: 大批量稠密 | dense `.cols` | 同 dense 路径 | 持平 | — |
| 3 | 写: 批内异构 | ❌ 不支持 | ✅ | 新能力 | — |
| 4 | 写: SQL UPDATE 点更 | ROW_MODE 重 | ColumnPatchSink 轻 | **显著改善** | — |
| 5 | 读: 无 DCG | base 直读 | 同 (_dcgs 空早返回) | 持平 | — |
| 6 | 读: 单 dense DCG | iterator 替换 | 同 | 持平 | — |
| 7 | 读: 单 sparse DCG | n/a (现状没此模式) | Overlay (向量化) | **+0–2%**(优化后) | ⚠️ 小 |
| 8 | 读: 多版本 sparse | n/a | K 路归并 + cache | **+2–8%**(优化后) | ⚠️ 需后台收敛兜底 |
| 9 | 谓词下推 (有 DCG) | **全关 ZM filter** | Effective ZM | **改善 5–100×** | — |
| 10 | Compaction | cumul + base | + sparse / promotion | **+1–3% CPU** | 🟡 轻 |
| 11 | 元数据存储 | DCG PB | + FileKind + variant_path | **+<100B/DCG** | 🟡 轻 |
| 12 | PK index 压力 | column mode 已点查 | 同 | 持平 | — |
| 13 | 内存占用 | iterator 状态 | + 反向索引 + K 路堆 | **+几 MB/query** | 🟡 轻 |
| 14 | Variant path | ❌ | ✅ | 新能力 | — |

### 5.2 与 Doris Flexible 对比

| 维度 | Doris Flexible | SDCG |
|---|---|---|
| 每行额外开销 | bitmap × num_cols (永久) | 仅 .spcols 中,base 零侵入 |
| 占位值/默认值 | 必须填 | 不存在(rowid 集合稀疏) |
| 写时读历史 | ✅ 必须 | ❌ |
| 需要 row store | 强需要 | 不需要 |
| 写并发冲突处理 | publish 期 transient rewrite | PK 索引重查 (零 IO) |
| 同列单批多版本 | bitmap 显式 | rowid 集合允许重复,后写赢 |
| Variant path 级 | ❌ | ✅ |

### 5.3 与 ClickHouse Patch Parts 对比

| 维度 | CK Patch | SDCG |
|---|---|---|
| 稀疏维度 | 仅行稀疏 | 行 + 列双稀疏 |
| 寻址 | `_part_offset` (Merge) / hash join (Join) | 物理 rowid + PK 索引重查 |
| 与 source part 生命周期耦合 | 弱 (hash join fallback) | 较强,但 PK 索引避免 fallback |
| 列集合切分 | 不同分区 (爆炸风险) | 同 DCG 内多列组 (紧凑) |
| 批内 per-row 异构 | ❌ | ✅ |
| Variant path 级 | ❌ | ✅ |
| 谓词下推 | apply 阶段复杂 | Effective ZM |
| CDC 上游负担 | 普通场景 OK;TOAST/MINIMAL 受限 | 普通场景 OK + 支持变更列子集流 |
| 生产验证 | ✅ 25.x 上线 | ❌ 设计中 |
| 运维复杂度 | 低 | 中 |

### 5.4 何时 SDCG 会比现状慢

诚实列出:

1. **多版本 sparse 未及时收敛** —— sparse compaction worker 跟不上时,K 路归并 + merge cache miss,读延迟 10–50%。**必须靠后台收敛兜底**。
2. **单 sparse DCG 命中 page 内部** —— 一次向量化 `update_rows` apply,5–15% (优化后接近 0–2%)。
3. **极短 PK 点查** —— iterator 初始化的反向索引构建。可针对 PK 点查走 fast path 绕过。
4. **FE 估算错路径(SQL UPDATE)** —— WHERE 选择性估算偏低导致走 sparse 但实际 K 很大。BE 侧加保护性切换。

---

## 6. 实现计划

### 6.1 优先级与路径

**P0 (MVP, Lake 优先)**:
1. Helper 模块骨架 (`be/src/storage/partial_update/`)
2. `.spcols` 文件格式 + Segment v2 集成
3. DCG PB 扩展 (FileKind, ExtendedColumnRef, sparse_row_counts)
4. SparseColsWriter (跳过 read_from_source_segment)
5. LayeredOverlayIterator + 反向索引 + Dense Pruning
6. Effective ZoneMap (segment level)
7. Presence Bitmap fast-path
8. Lake 接入 (`column_mode_partial_update_handler.cpp`)
9. Feature flag `enable_sparse_dcg` (默认 false)
10. UT + 端到端 lake 验证

**P1 (功能完整)**:
11. 本地引擎接入 (`rowset_column_update_state.cpp`)
12. SQL UPDATE 通道 (FE ColumnPatchSink)
13. Effective ZoneMap (page level)
14. Sparse compaction worker
15. Sparse → dense promotion
16. Variant path 级支持(复用 VariantColumnMerger)
17. Late materialization 协同
18. Read-time merge cache
19. AUTO_MODE 真正自动化
20. inline-PB (K ≤ 4)

**P2 (优化与差异化)**:
21. MERGE 语句承载批内异构(可选)
22. 字典共享编码
23. 读频驱动 promotion
24. 自适应阈值调优
25. 监控/observability metrics

### 6.2 关键源码改动清单

| 改动 | 文件 | P |
|---|---|---|
| Helper 模块新建 | `be/src/storage/partial_update/*` | P0 |
| DCG PB 扩展 | `gen_cpp/olap_common.pb`、`gen_cpp/lake_types.pb` | P0 |
| DCG class 扩展 | `be/src/storage/delta_column_group.{h,cpp}` | P0 |
| SparseColsWriter | helper 模块 | P0 |
| LayeredOverlayIterator | helper 模块 | P0 |
| 反向索引 | `be/src/storage/rowset/segment_iterator.cpp:481` 改 `_dcgs` 结构 | P0 |
| 有效 ZM segment 级 | `be/src/storage/rowset/segment.cpp:288`、新增 `get_effective_zone_map` | P0 |
| 有效 ZM page 级 | `be/src/storage/rowset/segment_iterator.cpp:1106` 的 `get_row_ranges_by_zone_map` | P1 |
| Lake 接入 | `be/src/storage/lake/column_mode_partial_update_handler.cpp` | P0 |
| 本地接入 | `be/src/storage/rowset_column_update_state.cpp` 改 `finalize` | P1 |
| SQL UPDATE | `fe/fe-core/.../UpdatePlanner.java`、新增 `ColumnPatchSink` | P1 |
| AUTO_MODE 实现 | `be/src/storage/delta_writer.cpp:132` | P1 |
| Variant path | helper + `be/src/column/variant_merger.cpp` 集成 | P1 |
| Promotion worker | 新增 worker;`be/src/storage/compaction_manager.cpp` 注册 | P1 |
| Module manifest | `be/module_boundary_manifest.json` 新增 PartialUpdate | P0 |
| Configs | `be/src/common/config.h` 新增 sparse_threshold 等 | P0 |
| FE configs | `fe/fe-core/.../GlobalVariable.java` 新增 enable_sparse_dcg_update | P1 |

### 6.3 配置项设计草案

```cpp
// be/src/common/config.h
CONF_mDouble(sdcg_dense_threshold, "0.3");         // K/M >= 此值走 dense
CONF_mInt32(sdcg_inline_threshold, "4");           // K <= 此值 inline 到 PB
CONF_mInt32(sdcg_sparse_compaction_max_files, "8"); // 触发稀疏合并阈值
CONF_mDouble(sdcg_promotion_threshold, "0.3");      // 累积 K/M 升级为 dense
CONF_mInt32(sdcg_promotion_hard_count, "16");      // sparse 文件数上限
CONF_mBool(sdcg_enable_effective_zone_map, "true");
CONF_mBool(enable_sparse_dcg, "false");            // 顶层 feature flag
```

```java
// FE session var
SET enable_sparse_dcg_update = false;
SET sparse_dcg_update_max_columns = 32;
SET sparse_dcg_update_density_threshold = 0.1;
```

### 6.4 灰度策略

1. **Stage 0**: Helper 模块只编译,所有调用方仍走老路径
2. **Stage 1**: Lake 集群 `enable_sparse_dcg=true`,小流量 partial update 走 sparse 路径
3. **Stage 2**: Lake 全量 partial update 走 sparse;监控读延迟、compaction 健康度
4. **Stage 3**: 本地引擎灰度开启
5. **Stage 4**: 默认开启,AUTO_MODE 真正决策路径
6. **Stage 5**: SQL UPDATE 通道开启

每个 stage 失败应能一键回退到老路径(feature flag)。

### 6.5 验收标准

**功能**:
- [ ] Stream Load JSON 单批内不同行更新不同列,读取正确
- [ ] SQL UPDATE 走 ColumnPatchSink,与 ROW_MODE 结果一致
- [ ] Variant 列 path 级 update + path 级查询正确
- [ ] Effective ZM 谓词下推不漏行(随机谓词 + 全表对比)
- [ ] 并发 partial update 冲突解决正确(版本变化场景)
- [ ] dense / sparse 混合 DCG 读取正确
- [ ] 主 compaction 后 DCG 正确清理
- [ ] Lake 与本地引擎结果一致

**性能(对标现状)**:
- [ ] 小批量 partial update 写吞吐 ≥ 10× ROW_MODE
- [ ] 静态表(无 DCG)读延迟无回退
- [ ] 含 DCG 表的高选择性查询有 zone map 下推
- [ ] 多版本 sparse 长尾(连续 100 次稀疏 update 后)读延迟 ≤ 现状 + 10%
- [ ] CDC 场景 PK 索引争用不退化

**运维**:
- [ ] 监控覆盖: sparse DCG 数、密度分布、overlay 耗时、promotion 频次
- [ ] feature flag 可热切换
- [ ] tablet meta 不显著膨胀

---

## 7. Open Questions / 决策需求

### 7.1 待决策(需 owner 拍板)

1. **inline-PB 阈值 K**:4 / 8 / 16 ——影响 tablet meta 大小与小文件爆炸防护
2. **promotion 阈值 K/M**:0.1 / 0.3 / 0.5 ——影响读快写慢权衡
3. **Variant path 级是否 P1 必做**:不必做的话 ExtendedColumnRef 也要先预留 schema 位
4. **Lake 优先还是本地优先**:本设计假设 Lake 优先,但本地用户基数大,可能并行
5. **SQL UPDATE 入口何时打开**:作为 P1 还是更早

### 7.2 待调研

1. **Roaring Bitmap on rowid 的实际压缩率** —— 在生产 CDC 负载上跑数据
2. **Effective ZoneMap over-include 率** —— 坏分布下下推退化到什么程度
3. **PK 索引在高频小批量下的争用** —— SDCG 不改善这条,但需评估是否成为瓶颈
4. **Sparse compaction 的资源开销** —— 与现有 compaction 抢 IO 的程度
5. **Variant path 集合的稀疏度分布** —— path 数和 path 命中分布的实际形态
6. **极端场景下的 fail-safe** —— sparse DCG 损坏时的恢复路径

### 7.3 设计预留(暂不实现,但要兼容)

1. **更细粒度的 path 级稀疏** —— 单 path 内的 array element 级更新
2. **跨 segment 的 sparse 合并** —— 当前设计 sparse DCG 绑定单 source segment,未来可能扩展
3. **Sparse DCG 的列 reorder** —— 优化读时多列同时 overlay 的内存局部性

---

## 8. 风险登记

| 风险 | 等级 | 缓解 |
|---|---|---|
| Multi-version sparse 不收敛导致读退化 | 高 | sparse compaction + promotion 双层兜底 |
| Lake 一致性模型与 SDCG 的交互未充分推演 | 高 | Lake 优先,小流量先跑通 |
| 小文件爆炸 (高频小批量) | 中 | inline-PB + memtable 合并 + sparse compaction |
| PK 索引压力 | 中 | 现状已有此压力,SDCG 不恶化 |
| 反向索引内存占用 | 低 | 短查询走 fast path 绕过 |
| Effective ZM over-include 严重退化 | 低 | 极端分布下损失可量化,设监控告警 |
| 文件格式变更后老 DCG 兼容性 | 中 | DCG PB 默认值保证向后兼容(FileKind=DENSE_COLS) |
| Variant 列在并发场景下的 path 类型选举死锁 | 中 | 复用 VariantColumnMerger 现有锁模型 |

---

## 9. Decision Log

- **2026-06-01**: 初稿。基于对 ClickHouse Patch Parts、Doris Flexible Partial Update、Apache Paimon、Apache Hudi、Apache Pinot 的源码与文档深入调研,以及对 StarRocks DCG 现状(`be/src/storage/rowset_column_update_state.cpp`、`delta_column_group.cpp`、`segment_iterator.cpp`)的代码级分析。

---

## 10. References

### 10.1 StarRocks 源码(本仓库)

- 分区策略与 partial update 模式: `gensrc/thrift/Types.thrift:568` `TPartialUpdateMode`
- ROW mode 入口: `be/src/storage/tablet_updates.cpp:1346` `_apply_normal_rowset_commit`
- COLUMN mode 入口: `be/src/storage/tablet_updates.cpp:1133` `_apply_column_partial_update_commit`
- AUTO_MODE 退化点: `be/src/storage/delta_writer.cpp:132`
- DCG 元数据: `be/src/storage/delta_column_group.h:35` `DeltaColumnGroup`
- DCG 合并: `be/src/storage/delta_column_group.cpp:65` `merge_by_version`
- 稠密写入: `be/src/storage/rowset_column_update_state.cpp:319` `read_from_source_segment_and_update`
- 冲突解决: `be/src/storage/rowset_column_update_state.cpp:230` `_resolve_conflict`
- 读路径 DCG overlay: `be/src/storage/rowset/segment_iterator.cpp:1120` `_get_dcg_segment`
- Zone map filter 关闭: `be/src/storage/rowset/segment.cpp:288-308`
- Late materialization: `be/src/storage/rowset/segment_iterator.cpp:262`
- Row range zone map: `be/src/storage/rowset/segment_iterator.cpp:1106`
- Column update_rows: `be/src/column/column.h:198`
- Variant 类型: `be/src/types/logical_type.h:73` `TYPE_VARIANT = 55`
- Variant 合并器: `be/src/column/variant_merger.h` `VariantColumnMerger`
- Lake 并行实现: `be/src/storage/lake/column_mode_partial_update_handler.cpp`

### 10.2 ClickHouse 源码

- Patch part 设计文档: `src/Storages/MergeTree/PatchParts/PatchPartInfo.h:8-59`
- Apply 实现: `src/Storages/MergeTree/PatchParts/applyPatches.{h,cpp}`
- Partition by column hash: `src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:79-85`
- 系统列定义: `src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:109-137`
- MaterializedPostgreSQL: `src/Storages/PostgreSQL/MaterializedPostgreSQLConsumer.cpp:444-489`

### 10.3 Apache Doris 源码

- Flexible partial update 入口: `be/src/storage/segment/vertical_segment_writer.cpp:738` `_append_block_with_flexible_partial_content`
- 关键 TODO: `be/src/storage/segment/vertical_segment_writer.cpp:842`(skip_bitmap 不在 MoW 读路径使用)
- 历史回填: `be/src/storage/partial_update_info.cpp:596` `fill_non_primary_key_cell_for_column_store`
- 批内折叠: `be/src/load/memtable/memtable.cpp:594`
- Publish 期 transient rewrite: `be/src/storage/tablet/base_tablet.cpp:1489` `create_transient_rowset_writer`
- 版本严格串行: `be/src/storage/task/engine_publish_version_task.cpp:216`

### 10.4 公开文档

- [ClickHouse Lightweight UPDATE 文档](https://clickhouse.com/docs/sql-reference/statements/update)
- [ClickHouse PR #82004 — Lightweight Updates with patch parts](https://github.com/ClickHouse/ClickHouse/pull/82004)
- [Apache Doris Partial Column Update](https://doris.apache.org/docs/3.x/data-operate/update/partial-column-update/)
- [Doris PR #34925 — Refactor variant flush logic to support partial update](https://github.com/apache/doris/pull/34925)
- [Apache Paimon Partial Update Merge Engine](https://paimon.apache.org/docs/master/primary-key-table/merge-engine/partial-update/)
- [Apache Hudi Record Mergers](https://hudi.apache.org/docs/record_merger/)
- [Apache Pinot Stream Ingestion with Upsert](https://docs.pinot.apache.org/manage-data/data-import/upsert-and-dedup/upsert)
- [Parquet Variant Shredding Spec](https://github.com/apache/parquet-format/blob/master/VariantShredding.md)
- [PostgreSQL Logical Replication Message Formats](https://www.postgresql.org/docs/current/protocol-logicalrep-message-formats.html)
- [MySQL binlog_row_image](https://dev.mysql.com/doc/refman/8.0/en/replication-options-binary-log.html#sysvar_binlog_row_image)
- [ClickPipes TOAST Columns Handling](https://raw.githubusercontent.com/ClickHouse/clickhouse-docs/main/docs/integrations/data-ingestion/clickpipes/postgres/toast.md)

### 10.5 业界深度文章

- [How we built fast UPDATEs for the ClickHouse column store – Part 2](https://clickhouse.com/blog/updates-in-clickhouse-2-sql-style-updates)
- [How we made ClickHouse UPDATEs 1,000× faster (Part 3: Benchmarks)](https://clickhouse.com/blog/updates-in-clickhouse-3-benchmarks)
- [Apache Doris 3.1: Enhanced Semi-Structured Analytics](https://www.velodb.io/blog/apache-doris-3-1-released-better-semi)
- [The "Surgical" Update: How Apache Doris Solves the Real-Time Data Stitching Nightmare](https://medium.com/@zhaochangle/the-surgical-update-how-apache-doris-solves-the-real-time-data-stitching-nightmare-a7f416708299)
- [Introducing the Open Variant Data Type in Delta Lake and Apache Spark](https://www.databricks.com/blog/introducing-open-variant-data-type-delta-lake-and-apache-spark)
- [The Apache Iceberg™ Variant Type: Flexible Semistructured Data, Reimagined](https://www.snowflake.com/en/blog/engineering/apache-iceberg-v3-variant-type/)
- [On the performance impact of REPLICA IDENTITY FULL — Xata](https://xata.io/blog/replica-identity-full-performance)
- [Why TOAST Columns Break Postgres CDC And How To Fix It — Artie](https://www.artie.com/blogs/why-toast-columns-break-postgres-cdc-and-how-to-fix-it)

### 10.6 相关 Issues / PRs

- StarRocks Issue #20436 — Support Partial Update By Column Mode
- StarRocks Issue #61938 — Partial column updates in the primary key table do not work
- ClickHouse Issue #82033 — Lightweight Updates improvements Umbrella
- ClickHouse Issue #86779 — UPSERT for MergeTree (leveraging Lightweight Updates)
- Doris PR #39756 — Support flexible partial update in stream load with json files
- Doris PR #40190 — Forbid partial update on MoW with sync MV

---

## Appendix A. 术语表

| 术语 | 解释 |
|---|---|
| **DCG** | Delta Column Group — StarRocks 现有的列级增量数据结构,对应 `.cols` 文件 |
| **SDCG** | Sparse DCG — 本文提案的扩展,引入 `.spcols` 稀疏文件类型 |
| **MoW** | Merge-on-Write — 主键表写时合并语义(StarRocks PK 表本质属于此类) |
| **MoR** | Merge-on-Read — 读时合并语义,与 MoW 对立 |
| **Patch Part** | ClickHouse 的轻量更新载体,与 base part 独立存在,读时合并 |
| **Skip Bitmap** | Doris flexible partial update 用于编码"哪些列在该行未更新"的位图 |
| **Sequence Group** | Paimon 的多源乱序合并机制,允许列组各自定序 |
| **Variant Path** | Variant/JSON 类型内部的 JSON path 引用 |
| **Effective ZoneMap** | SDCG 中合成的"有效"区间统计,覆盖 base + overlay |
| **Presence Bitmap** | 标识 sparse DCG 覆盖了哪些 rowid 的 Roaring bitmap |
| **Promotion** | sparse DCG 转 dense DCG 的后台动作 |

## Appendix B. 关键算法草稿

### B.1 列等价类划分

```python
def classify_columns_by_rowid_set(update_cols, partial_update_states):
    """
    Input: 更新列集合 + 每列的 rowid 集合
    Output: 等价类列表,每个等价类内的列共享 rowid 集合
    """
    # 用 rowid 集合的 hash 做快速分组
    hash_to_cols = {}
    for col in update_cols:
        rowids = partial_update_states.get_rowids(col)
        h = xxhash(sorted(rowids))
        hash_to_cols.setdefault(h, []).append(col)
    
    # 桶内深度比对(碰撞概率低)
    result = []
    for h, cols in hash_to_cols.items():
        # 按实际 rowid 集合再分细
        subgroups = {}
        for col in cols:
            key = tuple(partial_update_states.get_rowids(col))
            subgroups.setdefault(key, []).append(col)
        result.extend(subgroups.values())
    
    return result
```

### B.2 LayeredOverlayIterator 主循环

```cpp
Status LayeredOverlayIterator::next_batch(
        size_t* n, Column* dst, const SparseRange<>& range) {
    // 1. base iter 取数据
    RETURN_IF_ERROR(_base_iter->next_batch(n, dst, range));
    
    // 2. 应用每一层 overlay (已按版本降序,且已做 dense pruning)
    //    覆盖顺序: 老的先 apply,新的后 apply (后写赢)
    for (auto it = _layers.rbegin(); it != _layers.rend(); ++it) {
        auto& layer = *it;
        
        // 2a. range ∩ presence_bitmap
        auto hit = layer.presence_bitmap_intersect(range);
        if (hit.empty()) continue;  // ★ fast path
        
        // 2b. 计算 local index
        std::vector<uint32_t> local_idx;
        local_idx.reserve(hit.cardinality());
        for (auto rowid : hit) {
            local_idx.push_back(range_to_local(rowid, range));
        }
        
        // 2c. 读 overlay value 子集
        auto overlay_col = layer.read_values_for_rowids(hit);
        
        // 2d. 向量化覆盖 (复用 Column::update_rows)
        dst->update_rows(*overlay_col, local_idx.data());
    }
    
    return Status::OK();
}
```

### B.3 Effective ZoneMap 合成

```cpp
ZoneMap Segment::get_effective_zone_map(
        uint32_t ucid, const SegmentReadOptions& opts) {
    auto layers = collect_layers_for_column(ucid);
    
    if (layers.empty()) {
        return _column_readers[ucid]->zone_map();
    }
    
    // 找最新 dense (dense pruning)
    auto first_dense = std::find_if(layers.begin(), layers.end(),
            [](auto& l) { return l.is_dense(); });
    
    ZoneMap result;
    
    if (first_dense != layers.end()) {
        // 最新 dense 替代 base
        result = first_dense->zone_map();
        // dense 之上的 sparse 仍要叠加
        for (auto it = layers.begin(); it != first_dense; ++it) {
            result.union_with(it->zone_map());
        }
    } else {
        // 全 sparse,base 仍然有效
        result = _column_readers[ucid]->zone_map();
        for (auto& l : layers) {
            result.union_with(l.zone_map());
        }
    }
    
    return result;
}
```

---

*本文档为研究/前期设计阶段产物,所有性能数字是基于源码分析与推理,未经真实负载验证。后续每个 P0 项目落地时,应附 microbenchmark + 端到端性能报告。所有"显著改善 / 改善 / 回退"的判定需在 staging 环境 A/B 验证后转为定论。*
