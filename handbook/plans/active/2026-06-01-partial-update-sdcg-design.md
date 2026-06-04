# Sparse Delta Column Group (SDCG) —— 稀疏列上的 Partial Update 设计

- Status: design / **v1.3 — 经代码级验证修订**
- Owner: TBD
- Last Updated: 2026-06-04
- 验证记录: 2026-06-03/04,14+4 个并行 agent 对照最新 main(b5d9a6080)逐条核验本文事实与架构,详见 §9 Decision Log 与 Appendix C

---

## 0. Executive Summary

本文是 **StarRocks 主键表 partial update 的下一代设计提案**,目标场景是 **CDC + 半结构化数据 + 高频小批量更新**——这是 ClickHouse Lightweight Update、Doris Flexible Partial Update、Apache Paimon partial-update merge engine、Apache Hudi PartialUpdateAvroPayload、Apache Pinot partial upsert 等系统正在共同探索的赛道。

核心提案是 **SDCG (Sparse Delta Column Group)**:在 StarRocks 既有 DCG (`.cols`) 之外引入 **稀疏增量文件 `.spcols`**——每个 rowid 等价类一个文件,文件内各列共享同一 rowid 集合,与 base segment 通过物理 rowid 对齐,无需 mask、无需占位值。读路径通过新建的 `LayeredOverlayIterator` 做逐行回落叠加,并与 `SparseRange<>` row-range、late materialization、zone map 三大基础设施协同;写路径在 dense/sparse 之间按密度阈值自动选择(由 **BE 作唯一权威**)。

核心赌注:**StarRocks 在 OLAP 多模数据 + 实时 CDC(异构列)双战场上,单一稀疏 patch 模型(CK 路线)不够用,单一 bitmap 模型(Doris 路线)代价不可接受,必须双态 + 谓词下推保真**。

**v1.3 关键修订(相对 v1.2)**:
1. 读路径合并顺序的权威规则改为**版本升序 apply(老→新,last-write-wins)**——v1.2 正文的"按版本降序应用"是错误顺序(旧值赢),只有附录 B.2 是对的(修订前内部自相矛盾)。
2. dense/sparse 是**每文件**(每列组)属性,剪枝谓词严格按列解析 `file_kinds[file_idx]`,绝不塌缩成 DCG 级标志。
3. 现有"首命中 + dense 遮蔽一切"DCG 读模型与 sparse **不兼容**,由 `LayeredOverlayIterator` 完全取代;只有 DENSE 文件可终止图层遍历。
4. DCG GC(`garbage_collection`)的覆盖谓词对 sparse 会**静默丢数据**,v1 采用 **P-conservative 策略**:只有 DENSE 覆盖可释放旧层,sparse 仅由收敛动作(merge/promotion/compaction)的提交显式删除。
5. Lake 收敛走**另一个 proto**(`DeltaColumnGroupVerPB`)与另一套代码(`append_dcg`/`merge_dcg_meta`),两者都是 dense-only 语义,必须密度感知改造;Lake proto SDCG 字段从 **tag 6** 起(tag 5 已被 `shared_files` 占用)。
6. **Variant path 级更新从 P1 降为"仅预留 schema"**——`VariantColumnMerger` 做的是整列行拼接 + 列级 schema 调和,不是逐行 path 打补丁;该能力是一个独立的从零设计课题。
7. `.spcols` 物理格式采用**每 rowid 等价类一个文件**(Spike A 结论):Segment v2 / `SegmentWriter` **零改动**;单文件多组(各列行数不同)在现有 ordinal 体系下结构性不可行。
8. `PartialUpdate` 模块切分已完成原型(Spike B):`be/src/storage/partial_update/` + manifest 条目落地,`check_be_module_boundaries.py --mode full` 与 `render_be_agents.py --check` 双绿,无依赖环。

---

## 1. 背景与动机

### 1.1 StarRocks 现状的三个痛点(已验证)

源码确认的事实(`be/src/storage/`,对照 2026-06 最新 main 复核):

1. **`AUTO_MODE` 形同虚设** —— `be/src/storage/delta_writer.cpp:132` 注释:
   ```cpp
   // In the current implementation, UNKNOWN_MODE and AUTO_MODE can be considered as ROW_MODE
   ```
   注意:`:132` 只是注释所在(位于 sort-key-conflict 辅助函数内);**实际退化由"缺席"实现**——列模式选择分支(`delta_writer.cpp:307-318`)只匹配 `COLUMN_UPSERT_MODE/COLUMN_UPDATE_MODE`,AUTO_MODE 永不命中,端到端走 ROW 路径。**"AUTO_MODE 真正自动化"的实现点是 `:307-318`,不是 `:132`。**
   FE 侧:`InsertPlanner.java:456` 对 INSERT/SQL **默认**下发 AUTO_MODE;Stream Load 在用户传 `partial_update_mode=auto` 时下发(`StreamLoadKvParams.java:232`);Broker Load **默认是 UNKNOWN_MODE**(`BrokerLoadJob.java:287`),仅显式传 auto 才发 AUTO_MODE(`:291`)——两者同样退化为 ROW_MODE,结论不变。

2. **DCG 在小批量更新上写放大严重** —— `be/src/storage/rowset_column_update_state.cpp:319` `read_from_source_segment_and_update` **全量扫源 segment**(整段 chunk 循环 `:317-359`),即使只更新 100 行也要把 100 万行的列读出来再写回。

3. **DCG 存在即关闭 segment 级 zone map filter(范围比 v1.2 描述的窄)** —— `be/src/storage/rowset/segment.cpp:307`:
   ```cpp
   return st.ok() && dcgs.size() == 0;   // 有 DCG 即返回 false
   ```
   **修正(v1.3)**:实际损失比"所有列 zone map 全部失效"窄两档:
   - **key 列不受影响**:`segment.cpp:322` 是 `tablet_column.is_key() || parent->_use_segment_zone_map_filter(...)`,key 列不可被 partial update,其 ZM 恒有效;
   - **page 级 ZM/BF 今天对 dense DCG 就在工作**:page 级过滤跑在 `_column_iterators` 上(`segment_iterator.cpp:893/:1106`),dense DCG 列的迭代器指向 dense `.cols` 段,其自身 ZM 正确描述当前列值,**不受 DCG 开关门控**。
   真正被关掉的只有**非 key 列的 segment 级跳过**。SDCG 的收益因此重新表述为:(a) 用 effective ZM 重开 segment 级跳过;(b) 把 page 级下推**扩展到新的 sparse 层**(sparse 层没有 dense 整列可以倚靠)。收益倍数需按此修正基线在 staging 重新量化。

### 1.2 目标场景特征

- **CDC 同步**:上游消息只带 PK + 变更列(2–10 列),目标表上百列
- **多源宽表**:多条 CDC 流融合,**单批次内不同行更新不同列**
- **半结构化高频更新**:Variant/JSON 内某条 path 高频改,其余 path 不动(v1.3:预留能力,见 §4.6)
- **运维诉求**:写吞吐 ≥ 10k QPS、查询 P99 不抖、上游 DB 配置零侵入

### 1.3 现有方案为何不够

| 方案 | 致命问题 |
|---|---|
| StarRocks ROW_MODE | 读整行 + 写整行,小批量场景 N× 写放大 |
| StarRocks COLUMN_MODE | 读源 segment 整列,M× 写放大;关闭非 key 列 segment 级 zone map |
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

**关键事实(源码验证)**: Doris flexible 是 **写时合并**,不是读时合并。

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

SDCG 路线 = D + 在 patch 内挂 per-group rowid 集合 = **patch 文件方案的稀疏度维度从"行稀疏"扩展到"行×列双稀疏"**,获得 A 的表达力但不付永久 base 开销。

---

## 3. StarRocks 现状源码地图(v1.3 全部引用已对照最新 main 复核)

### 3.1 Partial Update 入口

**Thrift 定义** (`gensrc/thrift/Types.thrift:568-574`,`olap_file.proto:88-92` 同序镜像):
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
- `fe/fe-core/src/main/java/com/starrocks/sql/InsertPlanner.java:456` — 默认 AUTO_MODE(经 `checkIfUseColumnUpsertMode` 可改 COLUMN_UPSERT_MODE,`:457-458`)
- `fe/fe-core/src/main/java/com/starrocks/load/streamload/StreamLoadKvParams.java:232`(`auto` 分支;`column`→`:229`,`row`→`:235`)
- `fe/fe-core/src/main/java/com/starrocks/load/loadv2/BrokerLoadJob.java:287`(默认 UNKNOWN_MODE)/`:291`(显式 auto)

**BE 入口**:
- `be/src/storage/delta_writer.cpp:132` — AUTO_MODE≈ROW_MODE 的注释;**实际模式分派/未来 AUTO 实现点:`:307-318`**
- `be/src/storage/tablet_updates.cpp:1314-1324` — COLUMN vs ROW 路径分支(谓词是 `rowset->is_column_mode_partial_update()`,即 `num_update_files>0`,与枚举值无关)

### 3.2 ROW Mode 路径

入口: `TabletUpdates::_apply_normal_rowset_commit` (`tablet_updates.cpp:1346`)

读历史:
- `be/src/storage/rowset_update_state.cpp:373` `_prepare_partial_update_states`(真实读历史列值)
- `be/src/storage/rowset_update_state.cpp:279` `plan_read_by_rssid`
- 走 `tablet->updates()->get_rss_rowids_by_pk` 拿到 (rssid, rowid),然后读历史列值填补缺失列

写出: 完整新行,旧行 delete bitmap 标删

### 3.3 COLUMN Mode 路径 (DCG)

入口: `TabletUpdates::_apply_column_partial_update_commit` (`tablet_updates.cpp:1133`)

核心类:
- `be/src/storage/rowset_column_update_state.h:68` `ColumnPartialUpdateState`
- `be/src/storage/rowset_column_update_state.h:140` `RowsetColumnUpdateState`
- `be/src/storage/delta_column_group.h:35` `DeltaColumnGroup`

写路径(关键代码):
- `rowset_column_update_state.cpp:180` `_prepare_partial_update_states`:仅 PK 索引点查,**不读列值**
- `rowset_column_update_state.cpp:230` `_resolve_conflict`:latest_applied_version 变化时仅重查 PK index,重建 `(source_rowid, upt_rowid)` 对(`rowset_column_update_state.h:80-105`,按 source_rowid 排序 `:96-100`)
- `rowset_column_update_state.cpp:319` `read_from_source_segment_and_update`:**全量扫源 segment 整列**(写放大根源;M=源段行数也在此 `Segment::open` 后才可得,`:332`)
- `rowset_column_update_state.cpp:450` `_update_source_chunk_by_upt`:**整读 `.upt`**(`read_chunk_from_update_file`,`:407-421`)后内存 `append_selective`(`:480`)——注意:**当前没有 `.upt` 的选择性/位置读 API**(见 §4.4)
- `rowset_column_update_state.cpp:672-859` `finalize`(v1.2 误引 735-825):`.cols` 写出循环 `:769-825`,DCG 元数据构建 `:827-832`;K_S 在 `:747-753` 免费可得;`{uids[]}↔files[]` 位置映射约定 `:763-765`
- `rowset_column_update_state.cpp:390-405` `_prepare_delta_column_group_writer`:`SegmentWriter` + `init(false)`(无 key 列);本地路径 encryption_metas 当前留空(`:830`),Lake 路径有真实加密(见 3.7)

DCG 文件格式 (`delta_column_group.h:64`):
```
$1_$2_$3_$4.cols
  $1 = rowsetid, $2 = segment id, $3 = version, $4 = seq suffix
```

DCG 内含**稠密整列数据**——源 segment 的所有 N 行,更新行有新值,未更新行有从 base 读出的原值。

DCG 元数据(`delta_column_group.h:113-119`):
```cpp
class DeltaColumnGroup {
    int64_t _version;
    std::vector<std::vector<ColumnUID>> _column_uids;   // 列组分桶,与 _column_files 平行
    std::vector<std::string> _column_files;
    ...
};
```

合并(`delta_column_group.cpp:65-87`): `merge_by_version` **仅合并同 version 的 DCG,且只做文件列表拼接**;其唯一调用方是 Linked Schema Change(`schema_change.cpp:1161`、`tablet_updates.cpp:4110`)。**它不是任何跨版本收敛原语**(§4.8 的 sparse 合并是净新逻辑)。

GC(`delta_column_group.cpp:245-285` `DeltaColumnGroupListHelper::garbage_collection`):按"列 UID 被更新 DCG 列出即覆盖"释放旧 DCG——**dense-only 假设,对 sparse 不安全**,v1.3 必改(见 §4.8.5)。唯一生产调用点:`update_manager.cpp:309`(由 `tablet_updates.cpp:2836` `_remove_expired_versions` 触发,**与 compaction 物化无关**);物理删除在 `update_manager.cpp:316-327`。

### 3.4 读路径

`be/src/storage/rowset/segment_iterator.cpp`:

- 入口: `_new_dcg_column_iterator` (`:1138-1153`) —— **位置式整列替换**:直接用 dense `.cols` 段的列迭代器顶替 base 列迭代器(`:1273-1283`)
- DCG 查找: `_get_dcg_segment` (`:1120-1136`) —— 按版本降序找**首个**含该列的 DCG(首命中即终止;正确性依赖 dense 全行覆盖)
- DCG 段缓存: `_dcg_segments` map(声明 `:469`;v1.2 误引 1127)
- DCG 列表在 iterator init 时整载(`:824`)
- Zone map 关闭点: `segment.cpp:288-308`(key 列豁免在 `:322`)
- Page 级 row-range ZM:`segment_iterator.cpp:893/:1106`、page 剪枝 `:1764-1805`(**base ordinal 空间**)
- Page 级 BF:`:3507-3524`(per-page 行区间求交,`column_reader.cpp:432-472`)
- Late materialization:`PredicateLateMaterializationScanStrategy`(`segment_iterator.cpp`,策略字段/分支贯穿 `:168` 起)
- `Column::update_rows`:`be/src/column/column.h:190-198`,**无脑按下标覆盖**(无版本仲裁)——这是 §4.5 顺序规则的根据
- DCG 版本排序保证:RocksDB key 编码 `INT64_MAX - version`(`tablet_meta_manager.cpp:740/:765`),scan 出来天然新→老(`:1238-1252`);缓存插入保持新前(`update_manager.cpp:415`)

### 3.5 并发模型(v1.3:按引擎分述)

**本地引擎**:
- per-tablet apply 串行的主保证是 **`do_apply()` 单线程**("only 1 thread at max is running this method",`tablet_updates.cpp:999-1006`,`_apply_running` 门 `:947-953`);
- `_index_lock`(列模式 apply 在 `:1169` 加锁)保护 PK 索引在 `_resolve_conflict` 重查期间不被 compaction/GC/pk_dump 并发改写(`:1286/:2383/:5768/:5888` 等);
- `_resolve_conflict` 只重映射 `(source_rowid, upt_rowid)` 的 source 侧(值由不可变 `.upt` 按 conflict-invariant 的 `upt_rowid` 取,`rowset_column_update_state.cpp:456-482`),**零数据重写**;
- compaction-vs-partial-update 竞态由 `_check_conflict_with_partial_update`(`tablet_updates.cpp:2154-2193`)处理:过期 compaction 被取消,不动更新。

**Lake 引擎(2026-05 起结构已分叉,v1.2 未覆盖)**:
- 列模式 publish 已被 #71217/#71652 重写为**并行**:`batch_get_rss_rowids_from_pkindex(..., need_lock=false)`(`column_mode_partial_update_handler.cpp:118-119`)→ `LakePrimaryIndex::batch_parallel_get_rss_rowids`(`lake_primary_index.cpp:483`),专用 `lake_partial_update_thread_pool`(`exec_env.cpp:725-734`),受 `config::enable_pk_index_parallel_execution` 门控(默认 true,`config.h:471`);
- **lake 列模式 handler 内没有 `_resolve_conflict`**:它在固定 `base_version` 读 PK,冲突语义由 publish 的版本串行与 `CompactionUpdateConflictChecker`(`handler.cpp:452`)承担。

对比 Doris 的 publish-期 transient rewrite,StarRocks 两个引擎的冲突解决都更轻。**SDCG 的并发不变式按引擎分别表述(§4.7),helper 不内置 resolve 步骤;helper 代码必须对 lake 并行路径线程安全。**

### 3.6 Variant 类型现状(v1.3:修正复用范围)

`TYPE_VARIANT = 55` (`be/src/types/logical_type.h:73`),基础设施:

| 资产 | 路径 | 对 SDCG 的真实可用性 |
|---|---|---|
| 类型定义 | `be/src/types/variant.h/.cpp`、`variant_value.h/.cpp` | 行值不可变(`variant_value.h:63-236`),**无任何逐行 path 变更原语** |
| 列存 | `be/src/column/variant_column.h/.cpp` | 仅 `append` 族(`variant_column.h:85-86`),**无 `update_rows`** |
| Path 解析 | `be/src/column/variant_path_parser.h/.cpp` | 可用 |
| Path 读取三态 | `be/src/exprs/variant_path_reader.h:23-26` | `kMissing/kNull/kValue` 三态是 path 级补丁必须表达的语义 |
| 合并器 | `be/src/column/variant_merger.h/.cpp` `VariantColumnMerger` | **做整列垂直行拼接 + 列级 shredded schema 调和**(`merge_into` 最终 `dst->append(src,0,src.size())`,`variant_merger.cpp:552/558/565`);**不做逐行逐 path 打补丁** |
| 类型选举 | `arbitrate_type_conflicts`/`choose_common_type`(`variant_merger.cpp:389/:275`) | **可复用**:未来 compaction 把 path patch 提升进 shredded schema 时的类型扩宽/冲突仲裁 |

**v1.3 结论**:v1.2 的"path 级 partial update 所需的合并算法已就绪,不需要从零造合并器"**不成立**。真正需要的原语是"取既有行的 variant 值 → 解析 path → 替换/插入/删除子树 → 重编码 metadata 字典+value"——代码库中不存在,且 shredded schema 是列级而非行级(`column_array_serde.cpp:520`、`variant_column.cpp:990/:1006`),path patch 与 shredding 演化的交互是独立设计课题。详见 §4.6。

### 3.7 Lake 路径

并行实现 `be/src/storage/lake/column_mode_partial_update_handler.{h,cpp}`,与本地 `RowsetColumnUpdateState` 是平行类,但**数据结构已部分共享**(handler.h `#include rowset_column_update_state.h`,复用 `ColumnPartialUpdateState`/`RowidPairs`/`split_rowid_pairs`);DCG 读取已抽象为 `DeltaColumnGroupLoader`(`delta_column_group.h:122`,Local/Lake 两实现)。

**Lake DCG 元数据模型与本地不同(v1.3 必须吃透)**:
- 每个 segment(rssid)只有**一个** `DeltaColumnGroupVerPB`(`lake_types.proto:95-103`),其 repeated 字段是**同条消息内按 entry 堆叠的平行数组**(`unique_column_ids[i]/column_files[i]/versions[i]/encryption_metas[i]/shared_files[i]` 描述第 i 个文件);entry 顺序新→老(`meta_file.cpp:113-163` 先放新 entry 再搬旧 entry);
- `LakeDeltaColumnGroupLoader::load` 把它折叠成**一个**内存 DCG(`column_mode_partial_update_handler.cpp:55-63`),`get_lake_dcg_segment`(`update_manager.cpp:1266-1298`)的真实逐列解析靠 `get_column_idx` 的**首命中**;
- 收敛在 `MetaFileBuilder::append_dcg`(`meta_file.cpp:113-163`):**从所有旧 entry 剥离被更新列 UID,列空了就 orphan 文件**——dense-only;
- `merge_dcg_meta`(`tablet_merger.cpp:320-380`)是 **tablet 分裂/合并(SPLIT)路径**而非主 compaction,不重写 segment,对列重叠返回 `NotSupported`;
- 主 compaction 物化是另一条路(基于 segment 重建 + DCG GC);
- 加密:lake 写 `.cols` 时产出真实 encryption_meta(`handler.cpp:145-149/:416-419`);
- 新文件扩展名有硬编码白名单:`filenames.h:219`(`extract_uuid_from`)与 `:242`(`gen_filename_from`)——**`.spcols` 必须注册**,否则跨集群迁移/orphan 清理静默丢文件;
- 缓存:`LakeIOOptions`(`options.h:74-82`)的 `use_page_cache` 与 `fill_data_cache` 是两个独立开关,当前列模式读只设 `fill_data_cache=true`。

任何 SDCG 改动两边都要做——抽公共 helper 的关键动机(§4.9;原型已落地)。

---

## 4. SDCG v1.3 设计

### 4.1 总览

```
SDCG v1.3 组件图
├─ 物理存储
│   ├─ .cols      (dense, 现状, 向后兼容)
│   └─ .spcols    (sparse, 新增) —— 每 rowid 等价类一个 Segment v2 文件
│                  文件内: source_rowid 保留列 + 该组的更新值列, 行数统一为 K
├─ 元数据
│   ├─ DeltaColumnGroupPB    扩展 (local, tag 5-9)
│   ├─ DeltaColumnGroupVerPB 扩展 (lake,  tag 6-9; tag 5 已被 shared_files 占用)
│   ├─ ExtendedColumnRef 预留 (nested in DeltaColumnGroupColumnIdsPB, variant_path 仅占位)
│   └─ 反向索引 col_uid → 有序图层栈 (iterator 启动时构建, 净新)
├─ 写路径 (helper 共享)
│   ├─ 密度决策: BE 唯一权威 (K/M 阈值 + K 绝对值交叉点)
│   ├─ rowid 集合 hash 等价类分组 → 每组一个 .spcols
│   ├─ .upt 按列位置读 (排序→fetch_values_by_rowid→回置)
│   └─ inline-PB (字节预算, 非行数)
├─ 读路径 (helper 共享)
│   ├─ LayeredOverlayIterator (净新; sparse 层按版本升序 apply, last-write-wins)
│   ├─ Per-column dense pruning (file_kinds[file_idx]; 只有 DENSE 终止遍历)
│   ├─ Presence pre-filter (PB 内 min/max/count) + Roaring bitmap fast-path
│   ├─ 向量化 update_rows apply
│   ├─ Effective ZoneMap —— segment 级 (P0, 证明安全) / page 级+BF+DELETE (P1 独立 hardened 工作流)
│   ├─ Late materialization 协同
│   └─ Read-time merge cache
├─ Variant path 级 —— 仅预留 schema; BE 拒绝/回退非空 variant_path (独立设计轨)
├─ 后台收敛
│   ├─ Sparse → sparse 合并 worker (净新逻辑, 非 merge_by_version)
│   ├─ Sparse → dense promotion (文件数/密度/meta 字节三触发)
│   └─ P-conservative GC: 仅 DENSE 覆盖可释放旧层
└─ 双引擎共享
    └─ be/src/storage/partial_update/ helper 模块 (原型已落地, 边界校验双绿)
```

### 4.2 物理文件格式(Spike A 已定)

#### `.cols` (dense, 现状不变)

`$rowsetid_$segid_$version_$suffix.cols`,Segment v2 文件,**整列稠密**——源 segment 所有 N 行的该列值(更新行新值,未更新行原值)。

#### `.spcols` (sparse, 新增) —— **每 rowid 等价类一个文件**

**Spike A 结论(阻断级前提已验证)**:Segment v2 在 footer(`SegmentFooterPB.num_rows`,`segment.proto:212`)、writer 等长校验(`segment_writer.cpp:321-323`)、以及**整个 ordinal 体系**(`seek_to_ordinal`≡物理位置,`segment_iterator.cpp:1928`、`column_iterator.cpp:85-92`)三处都硬编码"单文件单行数"。v1.2 草图中"单文件多组、各列行数不同"**结构性不可行**(需自定义 footer + 每列 ordinal 平移层,波及全表共享读路径)。因此:

```
一个 rowid 等价类(组)= 一个 .spcols 文件(Segment v2, 零格式改动):
  ColumnMeta "source_rowid"  : 保留 uid, UInt32, 升序, K 行, 开 zone map(min/max rowid)
  ColumnMeta "col_a"         : K 行, 与 source_rowid 同序对齐
  ColumnMeta "col_b"         : K 行
  ...
SegmentFooter.num_rows = K   (writer/reader 全部走现成路径)
```

- 单批内同组列共享一个文件;不同 rowid 集合 → 不同文件,全部挂在同一 DCG 版本的 `column_files`(repeated,天然支持多文件);
- **退化即最优**:经典 CDC(每批同列集合同行集合)= 1 个等价类 = 1 个文件,与现状 dense 单组文件数相同;
- 写入构造与 dense 完全同款(`SegmentWriter` + `init(false)`,encryption 流程同 `handler.cpp:145-149`),只换 schema 和 chunk;
- **source_rowid 保留 uid**:必须避开真实列 uid 与现有哨兵 uid(`FULL_ROW_COLUMN`/op 列等),且**不进入** `DeltaColumnGroup::column_uids()` 的 uid→file 映射(`get_column_idx` 永不解析到它);`SegmentWriter::_verify_footer`(`segment_writer.cpp:478-485`)的 uid 唯一性 CHECK 顺带兜底。落地前 30 分钟 grep + 一个 `_verify_footer` UT 确认无碰撞。

#### Presence Bitmap(v1.3:双层放置,字节上限)

每个 `.spcols` 文件一个 Roaring bitmap(= 该文件 source_rowid 列的集合,二者按构造一致,可互校验/重建)。底层用 `be/src/types/bitmap_value.h`(**v1.2 误写 util/ 路径**)的 `BitmapValue`(Roaring64Map,`bitmap_value_detail.h:107`):交并差有 `operator&=/|=/-=`(`:135/:143/:147`),**没有现成 `rangeCardinality`**——需新增包装(参照 `DeletionBitmap::get_range_cardinality`,`deletion_bitmap.cpp:48-52`,底层 `roaring64_bitmap_range_cardinality`)或用 `bitmap_subset_in_range_internal`(`:216`)+`cardinality()`。

放置策略(调和"读路径要零 IO"与"lake 元数据膨胀"两个事实):
- **PB 内恒存轻量 pre-filter**:per-file `sparse_row_counts`(K)+ `min/max source_rowid`——page/range 与 `[min,max]` 不相交即零成本走原生 fast path;
- **完整 Roaring**:序列化后 ≤ `sdcg_presence_bitmap_inline_max_bytes`(默认 4096)则内联进 DCG PB(典型小 K 远小于此,读路径零 IO);超限则存于 `.spcols` 文件内(读侧经 read-time merge cache 一次加载、跨查询摊销)。Lake 的 `dcg_meta` 内嵌于每版本重传的 `TabletMetadataPB`(`lake_types.proto:215`),该字节上限 + §4.8 的 meta 字节硬顶共同防膨胀;
- **GC 不依赖 bitmap**(P-conservative,§4.8.5),所以"文件内存放"不损害 GC。

#### 列分组优化

writer 检测 rowid 集合等价类(hash-bucket,算法见附录 B.1)。退化场景:同列集合 batch update → 单组单文件,与现有稠密路径文件数等同(完全兼容)。

### 4.3 元数据扩展(v1.3:双消息、双起始 tag、平行数组纪律)

**本地** `DeltaColumnGroupPB`(`gensrc/proto/olap_common.proto:60-65`,proto2,现用 tag 1-4;`encryption_metas` 是 `repeated bytes`,v1.2 误写 string):

```protobuf
enum DeltaColumnFileKind { DENSE_COLS = 0; SPARSE_PERCOL = 1; }  // 0=默认 ⇒ 旧 meta 全 dense

message DeltaColumnGroupPB {
    repeated DeltaColumnGroupColumnIdsPB column_ids = 1;
    repeated string column_files = 2;
    repeated bytes  encryption_metas = 3;
    optional int64  file_size = 4;
    // === SDCG v1.3 ===
    repeated DeltaColumnFileKind file_kinds = 5;     // 与 column_files 平行; 空 ⇒ 全 DENSE
    repeated int64  sparse_row_counts = 6;           // 平行; DENSE 槽写 0
    repeated SparsePresencePB presences = 7;         // 平行; 每文件 min/max/count + 可选内联 roaring
    optional InlineSparsePatchPB inline_patch = 8;   // 字节预算内的微批内联(§4.4)
    optional int64  source_segment_num_rows = 9;     // 源段物理布局指纹(§4.7 读时不变式)
}

message SparsePresencePB {
    optional uint32 min_source_rowid = 1;
    optional uint32 max_source_rowid = 2;
    optional int64  row_count = 3;
    optional bytes  roaring = 4;     // 仅当 ≤ sdcg_presence_bitmap_inline_max_bytes
}
```

**Lake** `DeltaColumnGroupVerPB`(`gensrc/proto/lake_types.proto:95-103`,proto2):**tag 5 已被 `shared_files` 占用,SDCG 字段从 6 起,绝不复用 5**:

```protobuf
message DeltaColumnGroupVerPB {
    repeated DeltaColumnGroupColumnIdsPB unique_column_ids = 1;
    repeated string column_files = 2;
    repeated int64  versions = 3;
    repeated bytes  encryption_metas = 4;
    repeated bool   shared_files = 5;
    // === SDCG v1.3 (tag 6+) ===
    repeated DeltaColumnFileKind file_kinds = 6;     // per entry; 空 ⇒ 全 DENSE
    repeated int64  sparse_row_counts = 7;
    repeated SparsePresencePB presences = 8;
    optional int64  source_segment_num_rows = 9;
}
```

**ExtendedColumnRef(Variant path 预留)**:不在两条 DCG 消息上做扁平平行数组(单 entry 可携多 ref,扁平数组无法表达),改为**嵌入两引擎共享的 `DeltaColumnGroupColumnIdsPB`**:

```protobuf
message ExtendedColumnRefPB { optional int32 column_uid = 1; optional string variant_path = 2; }
// DeltaColumnGroupColumnIdsPB 内新增: repeated ExtendedColumnRefPB extended_refs = <next free tag>;
```

仅占位;**BE 对任何带非空 variant_path 的 DCG 拒绝读取或回退整列路径**(§4.6)。

**平行数组纪律(强制)**:`file_kinds/sparse_row_counts/presences` 长度为 0(=legacy,全 DENSE)或恒等于 `column_files_size()`;必须同步扩展 lake 的三处校验:
- `validate_dcg_shape`(`tablet_merger.cpp:262-281`):加 0-or-equal-length 检查;**放宽跨 entry 重复 UID 检查**(`:272-279`)——sparse 链有意重复 UID;新规则:两个 **DENSE** entry 重复 UID = Corruption;较老一侧为 SPARSE = 合法;
- `normalize_dcg_optional_fields`(`:283-290`):把 kinds 补齐为 DENSE、counts 补 0,使下游下标访问统一;
- `verify_dcg_entry_consistency`(`:292-318`):同名文件跨 meta 时同时断言 kind 与 K 一致。

**向后兼容**(两消息均 proto2,`olap_common.proto:36`/`lake_types.proto:15`):新字段 optional/repeated、默认缺席;`DENSE_COLS=0` 使旧 meta 在新 BE 上全 dense;旧 BE 读新 meta 保留未知字段。`save()` 在**全 dense 时省略新字段**,旧表 meta 字节恒等(零回归)。`OldDeltaColumnGroupPB` 回退路径(`delta_column_group.cpp:97-118`)经 fallback 也是 dense。

C++ 端(`delta_column_group.h`):

```cpp
std::vector<DeltaColumnFileKind> _file_kinds;     // 与 _column_files 平行; 空 ⇒ 全 DENSE
std::vector<int64_t>             _sparse_row_counts;
// 兼容访问器: 缺席即 dense —— 零回归铰链
DeltaColumnFileKind file_kind(size_t idx) const {
    return idx < _file_kinds.size() ? _file_kinds[idx] : DENSE_COLS;
}
bool is_file_dense(size_t idx) const { return file_kind(idx) == DENSE_COLS; }
```

`init()` 加可选尾参(既有调用方零改动);`load/save/serialize/merge_by_version`(`delta_column_group.cpp:89-135/:155-173/:175-243/:65-87`)同步携带新数组并保持平行。

### 4.4 写路径(v1.3:三处假设修正后的版本)

#### 4.4.1 入口统一

| 入口 | 行级列子集表达 |
|---|---|
| Stream Load JSON | 每条 JSON 自带 key 集合 → 每行已知列集合 |
| SQL UPDATE | 单语句 SET 列固定,所有 WHERE 命中行同列;**列模式 SQL UPDATE 今天已存在**(见 4.4.4) |
| MERGE 语句(可选) | 多 WHEN MATCHED 分支映射到多列组 |

#### 4.4.2 决策树(BE 唯一权威)

```
RowsetColumnUpdateState::finalize() (672-859) 改造, 决策插入点 ≈ :771 的 col/rss 循环:

    // 1. PK 索引点查 (现状不变, K_S 已在 :747-753 免费可得)
    // 2. M = 源段行数: footer-only Segment open (廉价的 footer 读,
    //    不是 read_from_source_segment 的整列扫描; RowsetStats 只有 per-rowset 行数,
    //    per-segment M 必须显式取得 —— v1.2 误以为免费)
    // 3. 列分组等价类 (附录 B.1)
    // 4. 路径选择 per (source_segment, col_group):
    if (K/M < sdcg_dense_threshold && K < sdcg_sparse_max_rows) {
        if (估算补丁字节 <= inline 字节预算)  write_inline_sparse_patch(...);
        else                                  write_sparse_percol_file(...);   // 每组一个 .spcols
    } else {
        write_dense_cols_file(...);   // 现状路径 (read_from_source_segment_and_update)
    }
```

双因子规则:除密度 K/M(默认 0.3)外,**K 绝对值也参与**(默认 `sdcg_sparse_max_rows=50000`)——`fetch_values_by_rowid` 的默认实现是逐 ordinal 随机 seek(`column_iterator.cpp:85-92`,接口自述"非高性能"),K 大到一定程度顺序整扫 `.upt` 反而更快;**Lake(对象存储随机读昂贵)默认更低的 sparse 上限/更倾向顺序扫描-再-gather**。

#### 4.4.3 稀疏写入实现(修正 `.upt` 读取方式)

**事实修正**:`get_update_file_iterator` 返回顺序 ChunkIterator(`segment_iterator.cpp:1993/2013/2030` 只有 `do_get_next`),**没有 read_selective**;现状代码是整读 `.upt` 再 `append_selective`(`rowset_column_update_state.cpp:461-484`)。位置读必须按列走 `ColumnIterator::fetch_values_by_rowid`(`column_iterator.h:218-220`),且**要求 ordinal 升序**——而 `(source_rowid, upt_rowid)` 对按 **source_rowid** 排序(`rowset_column_update_state.h:96-100`),upt_rowid 乱序。

```cpp
Status write_sparse_percol_file(group) {
    // 完全不读源 segment(写放大节省的来源); 值从不可变 .upt 取
    auto writer = make_segment_writer_like_dense(...);          // 同 :390-405 流程
    writer->append_column(SOURCE_ROWID_UID, group.sorted_source_rowids);  // 升序

    for (col_uid : group.cols) {
        auto it = upt_segment->new_column_iterator(col_uid);    // 按列开 .upt 迭代器
        // a. 把本组 upt_rowids 升序排序(记住逆置换)
        // b. fetch_values_by_rowid(sorted_upt_rowids) → tmp
        // c. 按逆置换回置到 source_rowid 序(与 source_rowid 列严格对齐 ——
        //    对齐错误是静默数据损坏, 用 split_rowid_pairs/append_selective 同款骨架, 勿重造)
        writer->append_column(col_uid, permuted_values);
    }
    writer->finalize(...);            // K 行等长, segment_writer.cpp:321 校验自然通过
    emit_presence(group);             // min/max/count 进 PB; roaring 按字节上限内联或入文件
}
```

写入代价: O(K × num_updated_cols),零源段读。**同批同列同 source_rowid 重复更新**:按 upt_rowid(=写入顺序)取最后写(与 §5.2 "后写赢"一致),写入 `.spcols` 前去重,文件内 source_rowid 严格唯一。

**inline-PB(字节预算,非行数)**:DCG PB 常驻内存且每次 iterator init 整载(`segment_iterator.cpp:824`),lake 还内嵌于每版本重传的 TabletMetadataPB——所以内联阈值以**字节**计(`sdcg_inline_patch_max_bytes`,默认 512),只收定宽短值,**绝不内联 Varchar 长值/Variant**;在 memtable 层合并使多个微批共享同一 PB。注意 `_resolve_conflict` 重映射后内联补丁中的 source_rowid 必须同步重写(普通文件路径写出在 resolve 之后,天然满足;内联路径要显式处理)。

#### 4.4.4 SQL UPDATE 通道(v1.3:重新定义 FE 改动)

**事实修正**:`UpdatePlanner` 没有任何 selectivity 估算;列模式 SQL UPDATE **今天已存在**——partial update 时 FE 已只 SELECT PK+SET 列(`UpdatePlanner.java:117-122`)并下发 `COLUMN_UPDATE_MODE`(`:146-149`)。真正的闸门在 `UpdateAnalyzer`:`column` 模式直通(`UpdateAnalyzer.java:108-113`);`auto` 模式要求 **SET 列 ≤3 且 <30% 且无 WHERE 谓词**(`:114-127`,`checkIfUsePartialUpdate` `:57-63`)。

因此 FE 改动 = **放开 auto 模式的"无 WHERE"限制**(允许带谓词的列模式 partial update),不是新建 ColumnPatchSink;dense/sparse 决策完全在 BE finalize(4.4.2),FE 估算至多作 hint。原 v1.2 的 `estimateSelectivity` 方案废弃。

### 4.5 读路径

#### 4.5.1 整体流程

```
SegmentIterator init:
    1. dcg_loader.load(...) → _dcgs (现状, :824)
    2. 构建反向索引 col_uid → 有序图层栈 (净新; 见 4.5.3 的按列解析)
    3. 对每个读取列:
       - 无图层 → 原生 column iterator (零开销早返回)
       - 仅一个 dense 层 → 现状位置式替换 (:1273-1283, 行为不变)
       - 含 sparse 层 → LayeredOverlayIterator (净新)

SegmentIterator next_batch:
    - LayeredOverlayIterator: 先取 base(或层栈底部 dense), 再把 sparse 层
      按版本升序 (老→新) 逐层 update_rows 覆盖 → last-write-wins
```

**v1.3 权威顺序规则(以"谁赢"表述,唯一权威,替换 v1.2 的自相矛盾表述)**:

1. 对读取列 uid,按 DCG **新→老**解析图层栈:每个 DCG 经 `get_column_idx(uid)→file_idx` 取**该列所在文件**,读 `file_kinds[file_idx]`,把该文件作为一层;**遇到该列的 DENSE 文件,把它纳入后停止**(dense 行完整,取代 base 与所有更老层)。
2. base 的取法:层栈以 dense 文件收底 → **该 dense 文件的列迭代器就是 base**(沿用现状位置式读;dense 无 presence bitmap、行完整,不进 overlay 循环);否则 base = 原生段列。
3. 其上的 **SPARSE 层按版本升序(老先 apply、新后 apply)** 经 `Column::update_rows` 覆盖——`update_rows` 是无脑覆盖(`column.h:190-198`),升序保证**最新版本最后落、按行获胜**,与现有引擎"含该列的最新 DCG 获胜"(`segment_iterator.cpp:1120-1133`)、版本编码 `INT64_MAX-version`(`tablet_meta_manager.cpp:740`)语义一致。
4. 等价实现二选一并删除另一种:`_layers` 存**升序**正向遍历,或存降序用 `rbegin/rend`(附录 B.2 即后者,为规范实现)。
5. 同版本两层命中同行不应发生(写侧去重保证),DCHECK 兜底;tie-break 按文件后缀升序。

> 为什么现在才需要顺序:现有引擎从未做过叠加循环——dense 全覆盖使"首命中即终止"天然正确;SDCG 把仲裁机制从**早终止**换成**层叠覆盖**,顺序首次成为正确性问题。验收必含"同一行被 ≥3 个 sparse 版本更新读到最新值"的 UT(单凭它即可捕获 v1.2 的顺序 bug)。

#### 4.5.2 LayeredOverlayIterator(净新组件)

```cpp
class LayeredOverlayIterator : public ColumnIterator {
    Status next_batch(size_t* n, Column* dst, const SparseRange<>& range) override {
        // 1. base: 原生段列 或 层栈底部 dense 文件的列迭代器
        RETURN_IF_ERROR(_base_iter->next_batch(n, dst, range));

        // 2. sparse 层按版本升序 apply (_layers 升序存放)
        for (auto& layer : _layers) {
            // 2a. pre-filter: range ∩ [min,max source_rowid] 为空 → 零成本跳过
            // 2b. presence roaring ∩ range 为空 → fast path 跳过
            auto hit = layer.presence_intersect(range);
            if (hit.empty()) continue;
            // 2c. base-rowid → 该层局部下标 (0..K-1): 经层内 source_rowid 列翻译
            //     (sparse 文件内值在 ordinal 0..K-1, 绝不能拿 base rowid 当 ordinal ——
            //      这是与 dense 路径的本质差别)
            auto local = layer.translate_to_local(hit);
            auto vals  = layer.fetch_values(local);            // fetch_values_by_rowid, 升序
            dst->update_rows(*vals, offsets_in_dst(hit, range)); // 向量化覆盖 (column.h:198)
        }
        return Status::OK();
    }
};
```

真实复用:`Column::update_rows`、`SparseRange<>`(自带 `&`/`|`)、Roaring 位运算、zone-map 原语、`Segment::open`/`new_dcg_segment` 打开 `.spcols`(就是普通 Segment v2 文件)。**净新**:本迭代器、反向索引、按列 dense-pruning、坐标翻译。

#### 4.5.3 Per-column Dense Pruning(修正 v1.2 的 DCG 级谓词)

```cpp
// 错误 (v1.2): if (dcg_ref.is_dense()) break;        // dense/sparse 不是 DCG 级属性!
// 正确 (v1.3): 严格按列、按文件
auto collect_layers(ColumnUID uid) {
    std::vector<OverlayLayer> layers;                  // 收集后反转为升序存放
    for (auto& dcg : dcgs_new_to_old) {
        auto idx = dcg->get_column_idx(uid);           // (file_idx, col_idx), h:51-62
        if (idx.first < 0) continue;
        layers.push_back(make_layer(dcg, idx));        // 携带 (file_idx, kind, K, presence)
        if (dcg->is_file_dense(idx.first)) break;      // 只有该列的 DENSE 文件可终止
    }
    return layers;
}
```

承重不变式(显式断言,勿默认):**DENSE_COLS 文件对其源 segment 行完整(N 行全在)**——dense-pruning 的安全性完全系于此;实现处加 DCHECK(dense 文件行数 == 源段 num_rows)。任何未来"部分 dense"种类**不得**终止遍历。

同时注意:**现有 `_get_dcg_segment` 首命中模型对 sparse 不兼容**(sparse 只覆盖 K 行,首命中会对未覆盖行静默遮蔽旧值)——读路径凡涉 sparse 一律走本节的层栈解析,`get_column_idx` 自身不感知覆盖,绝不可单独用作 sparse 的解析依据。Lake 侧 `get_lake_dcg_segment`(`update_manager.cpp:1266-1298`)同理(单段返回是 dense-only,需改为返回该列的有序层列表;在 `:1270` 加 TODO 标注,改造属于本迭代器工作项)。

#### 4.5.4 Effective ZoneMap(v1.3:拆成两个机制)

**事实修正**:SR ZoneMap **只有四元组** `(min, max, has_null, has_not_null)`(`segment.proto:147-156`、`zone_map_detail.h:23-57`)——v1.2 的"distinct value zonemap"不存在;`ZoneMap::union_with` 也不存在,**须从零实现**。

**(一)Segment 级(P0,可证明不漏行)**:

正确性论证(替换 v1.2 的笼统"over-include"断言):SR 下推的每种谓词的 `zone_map_filter` 都是四元组格上的**单调函数**——逐一核验:Eq(`column_predicate_cmp.cpp:493-498`,保留当且仅当 value∈[min,max],union 加宽区间只会多保留)、Lt/Gt/Le/Ge(`:291-294/:341-344/:442-446`,单边比较,加宽安全)、Ne(`:556-564`,仅 min==max==value 才剪,union 使其更难成立=剪枝力只减)、IsNull/IsNotNull(`column_null_predicate.cpp:66-69/:153-156`,需要 has_null/has_not_null 取 OR)。union = 格的 join ⇒ "保留集"只增不减 ⇒ **不漏行**。

```cpp
ZoneMap effective_zone_map(uid) {
    layers = collect_layers(uid);                       // 4.5.3, 升序
    ZoneMap zm = layers 以 dense 收底 ? dense_layer.zone_map()   // dense 替代 base
                                       : base_zone_map(uid);     // 全 sparse: base 仍有效
    for (auto& l : sparse_layers) zm = union_with(zm, l.zone_map());
    return zm;
}
// union_with 的强制语义 (最易出 bug 的一块, 独立 UT 矩阵 + 随机性质测试):
//   min' = min(各非全 null 操作数的 min); max' 同理取 max;
//   has_null' / has_not_null' = OR;
//   has_not_null==false 的操作数其 min/max **非法**, 必须跳过
//     (zone_map_index.cpp:149-152 的四态约定; column_reader.cpp:410-430 解析即如此)
//   类型感知比较走 TypeInfo::cmp + delegate_type (column_reader.cpp:413/:699)
```

落点:`segment.cpp:288-308` 改为总是允许,实际判定交给以 effective ZM 评估的 `SegmentZoneMapPruner`(`:310-336`,key 列分支 `:322` 保留)。

**(二)Page 级 + Bloom Filter + DELETE 谓词(P1,独立 hardened 工作流,差分测试门禁)**:

- **Page 级不能塌缩成单一 merged ZoneMapPB**:page 剪枝产出 base ordinal 空间的 `SparseRange`(`segment_iterator.cpp:1764-1805`);`.spcols` 的页 ZM 在自己 0..K 的 ordinal 空间(`column_reader.cpp:676-729` 用**本 reader** 的页索引;`scalar_column_iterator.cpp:445-466` 的无 ZM 回退返回 `{0, num_rows()}`——对 `.spcols` 即 K,**坐标空间错误**)。正确算法:base 页剪枝得 `base_keep`;**被任何 overlay 覆盖的 base 行强制反剪**(base 值已过期),再用该层自身页 ZM 在层内 ordinal 空间二次限定后翻译回 base 空间。坐标翻译(presence/source_rowid:层内 ordinal↔base rowid)是承重正确性细节,必须显式编码。
- **Bloom filter 不是"OR of bitsets"**:SR 没有段级 BF;BF 是 per-page 行区间求交(`column_reader.cpp:432-472`,`*row_ranges = row_ranges->intersection(bf_row_ranges)`)。P0 安全姿势:**与 overlay presence 重叠的 base 行排除出 base-BF 剪枝**;NGram/LIKE BF(`column_predicate_cmp.cpp:680`)同样处理。
- **DELETE 谓词(v1.2 完全遗漏,真实漏行/复活风险)**:删除谓词按页 ZM 评估并标记 `_delete_partial_satisfied_pages`(`column_reader.cpp:683-685/:724-726`、`scalar_column_iterator.cpp:730-736`);若 DELETE 谓词引用被部分更新的列,base 页 ZM 不再反映覆盖行当前值,标记可能朝**不安全方向**出错(已删行复活)。安全规则:**与 overlay presence 重叠的页强制 DEL_PARTIAL_SATISFIED**(行级对有效值重判)。验收含"DELETE WHERE <被更新列> 后对照全表扫描"差分测试。

#### 4.5.5 读优化叠加

| 优化 | 复用/新建 | 说明 |
|---|---|---|
| Presence pre-filter | PB 内 min/max/count(新) | range 不相交零成本跳层 |
| Roaring fast-path | `types/bitmap_value.h` + 新增 range-cardinality 包装 | 未命中 page 走原生扫描 |
| 向量化 `update_rows` | `column.h:198`(写路径同款) | 命中 page 一次 gather-blend |
| 列裁剪 | `_column_access_paths` | 未读列零成本 |
| Read-time merge cache | `_dcg_segments`(`segment_iterator.cpp:469`)扩展 | 层栈/位图装配每 segment 一次,跨 query 摊销 |
| Late materialization 协同 | `PredicateLateMaterializationScanStrategy` | overlay 只对存活行做 |
| Page/data cache | 本地 page cache;**Lake 需显式选择 `LakeIOOptions`** | lake 当前列模式读仅 `fill_data_cache=true`、未开 `use_page_cache`(`options.h:74-82`)——`.spcols` 读取必须显式指定缓存策略,merge cache 在 lake 上更重要 |

### 4.6 Variant Path 级 Partial Update(v1.3:降级为预留)

**裁决**:从 P1 移出,**仅预留 schema**(`ExtendedColumnRefPB.variant_path`,§4.3),BE 对带非空 variant_path 的 DCG **拒绝或回退整列路径**,直到独立设计轨交付。理由(源码级):

1. v1.2 的承重复用前提不成立:`VariantColumnMerger` 是**整列垂直行拼接 + 列级 shredded schema 调和**(`variant_merger.h:35`;`merge_into→dst->append(src,0,src.size())`,`variant_merger.cpp:552/558/565`;测试 `variant_merger_test.cpp:358-362` 证实两个 1 行输入 merge 出 **2 行**),不存在"把 path 补丁合入既有行"的操作。
2. 逐行 path 变更原语不存在:`VariantRowValue` 不可变(`variant_value.h:63-236`,单块 `[metadata][value]`,只有构造/序列化/to_json);全 BE 无 `set_path/update_path/merge_into_row`;`VariantColumn` 无 `update_rows`。真实操作 = 解码行→path 子树替换/插入/删除→重导出 metadata 字典+value 重编码——净新设计。
3. 语义空洞:path 级须表达 `kMissing/kNull/kValue` 三态(`variant_path_reader.h:23-26`;`:42/:74` 警告 shredding 后 base remain 失效)——(rowid→值)模型表达不了 delete-path;
4. shredding schema 是列/段级(`column_array_serde.cpp:520`;`align_schema_from` 对类型冲突直接拒绝,`variant_column.cpp:1006`),path patch 命中 shredded 路径是 schema 演化事件而非值覆盖;
5. 依赖未建组件(LayeredOverlayIterator 本身是 P0 净新)。

**合法保留的复用**:`arbitrate_type_conflicts/choose_common_type`(类型选举/数值扩宽)用于**未来 compaction 将 path patch 提升进 shredded schema**。差异化叙述收窄为可辩护形式:"**对 PK base 行持久化的、稀疏的、path 粒度 partial UPDATE**"(Snowflake/Databricks 的按 path shredding 是存储粒度,不是更新粒度)。独立轨的前置:per-row decode→splice→re-encode 原型 + 三态编码 + shredding 演化方案 + microbenchmark。

### 4.7 并发模型(v1.3:写时不变式 + 读时不变式,按引擎)

**写时不变式(成立,机制现成)**:`.spcols` 写出前其 source_rowid 必须基于最新 PK 映射。
- 本地:`_check_and_resolve_conflict`(`rowset_column_update_state.cpp:258`,read_version 守卫 `:273`)在 finalize 内先行;**成立的真正原因**:source_rowid 是 `(source_rowid, upt_rowid)` 对中冲突敏感的一半,值由不可变 `.upt` 按 conflict-invariant 的 upt_rowid 取(`:456-482`)——重映射 source 侧即正确,零数据重写。finalize+commit 全程持 `_index_lock`(`tablet_updates.cpp:1169-1274`),对 compaction 提交原子;过期 compaction 由 `_check_conflict_with_partial_update`(`:2154`)取消。
- Lake:handler 内无 resolve;不变式由 publish 的 base_version 串行 + `CompactionUpdateConflictChecker`(`handler.cpp:452`)承担。**这是 caller-owned 不变式,helper 不内置 resolve 步骤**;且 lake 路径已并行化(§3.5),helper 必须线程安全,`enable_pk_index_parallel_execution` 开/关都要验证。

**读时不变式(v1.2 缺失,新增)**:`.spcols` **位置绑定到一个特定 base segment 的物理行布局**(dense `.cols` 自含、sparse 不自含——结构性新依赖)。
- 与该段**原子 GC**:tsid-keyed 的 DCG 生命周期(`garbage_collection` 按 TabletSegmentId)与 `_check_conflict_with_partial_update` 已守住 compaction 竞态——复用,但要**明示**依赖;
- **防过期寻址**:PB 持久化 `source_segment_num_rows` 指纹;overlay/merge 打开时断言 `max(source_rowid) < segment.num_rows()`;
- **任何重写 base 段物理 rowid 而不失效其绑定 `.spcols` 的操作都是禁区**(布局变更型 ALTER、§4.8 promotion/合并的实现都必须以"新文件提交 + 旧文件同批失效"的方式进行);风险登记新增对应行。

**Inline-PB 特例**:内联补丁直接活在 tablet meta 里,`_resolve_conflict` 重映射后必须同步重写内联的 source_rowid(§4.4.3)。

### 4.8 后台收敛(v1.3:v1 触发只用文件数+密度;GC 重新设计)

#### 4.8.1 Sparse → sparse 合并(净新 worker)

```
触发: 同 (rssid, column-group) 的 sparse 文件数 ≥ sdcg_sparse_compaction_max_files (默认 8)
动作: 逐 rowid latest-version-wins + presence 并集 → 重写单一 .spcols (版本=最大版本)
提交: 新文件落地与旧输入(meta key + 文件)删除在同一 write batch
      —— GC 不再负责回收 sparse 输入 (见 4.8.5), 收敛动作自己删
```

**不是** `merge_by_version` 的复用(后者只拼接同版本文件列表、只接在 schema change 上,§3.3);与 schema-change 路径的并发互斥需要显式锁定(同一 DCG 版本不可同时被两者触碰)。

#### 4.8.2 Sparse → dense promotion

```
触发(三选一, 全部可从现有 DCG 元数据零 IO 计算):
  - 累积 K/M ≥ sdcg_promotion_threshold (默认 0.3)
  - sparse 文件数 ≥ sdcg_promotion_hard_count (默认 16)
  - per-segment dcg_meta 字节数 ≥ 上限 (lake 防 TabletMetadataPB 膨胀的硬顶)
动作: base + 全部层 按 §4.5 语义物化出新 dense .cols → 旧层在提交批内删除
```

v1.2 的读频驱动公式(`scan_frequency × sparse_version_count × overlay_cost`)依赖**不存在的统计**,降为 P2(先加 instrumentation)。

#### 4.8.3 主 compaction 协同

不变:cumulative/base compaction 把 base + 所有层(按 §4.5 语义)物化成新 base segment;物化读必须走 LayeredOverlayIterator(compaction 输入读取经 overlay)。落地前需验证 lake 主 compaction 物化路径对层叠语义的吃入(标记为实现期验证项)。

#### 4.8.4 Lake 收敛(H5 方案,P0 必做——v1.2 完全缺失)

- **`append_dcg`(`meta_file.cpp:113-163`)按文件种类分流**:
  - 新文件覆盖列 c 为 **SPARSE** ⇒ c 是层:**不**进剥离过滤器、**不**从旧 entry 剥离、**不** orphan;新 entry 前置(数组本就新前),首命中序自然成立;
  - 新文件覆盖列 c 为 **DENSE** ⇒ 维持今天剥离 + orphan(且现在会正确连带 orphan 旧 **sparse** entry 的 `.spcols`——dense 取代一切旧层,语义正确);
  - **混合新 entry 按文件构建过滤器**(dense-uid 集合才进剥离过滤器),旧 entry 搬运时同步搬 `file_kinds/sparse_row_counts/presences` 维持平行;
- **`merge_dcg_meta`(`tablet_merger.cpp:320-380`,tablet SPLIT 合并,非 compaction)**:同名文件去重不变(consistency 校验加 kind/K);重叠列规则改为——双方该列均 DENSE 仍 `NotSupported`(真冲突);**任一侧 SPARSE 即合法层,照常并入**;合并后按 `versions` **降序稳定重排全部平行数组**(新增 `sort_dcg_entries_by_version_desc` helper),保层叠读序;`source_segment_num_rows` 断言相等或取存在侧;
- **vacuum 已链安全**(全部按 `column_files()` 枚举:`vacuum.cpp:303-311/:947-955/:1463-1468`,无任何"列不再被列出即可删"推断)——唯一这么推断的地方就是 `append_dcg` 的 orphan 步,上文已修;加一条"链上 `.spcols` 不在删除集"UT;
- **`.spcols` 注册进 `filenames.h` 白名单**(`:219/:242`)先于任何 lake 写路径;data-cache 失效路径(`vacuum.cpp:1463-1468` 走 `column_files`)自动覆盖。

#### 4.8.5 本地 GC:P-conservative(H4 方案,P0 必做)

现状谓词(`delta_column_group.cpp:253-281` 平铺 `column_set`)对 sparse 会静默丢数据(§3.3)。v1 策略:

> **列 uid 的"覆盖"只能由更新的 DENSE 文件建立;sparse 文件永不被 GC 覆盖路径释放**,仅由收敛动作(4.8.1/4.8.2/4.8.3)的提交批显式删除。

```cpp
// 核心循环改造 (delta_column_group.cpp garbage_collection):
std::unordered_set<uint32_t> dense_covered;       // 仅 DENSE 文件喂入
for (dcg : list /*新→老*/) {
    if (dcg->version() > min_readable_version) continue;
    bool freeable = 所有 (file, uids) 的每个 uid 均 ∈ dense_covered;
    if (freeable)  free(dcg);
    else for (f : files) if (dcg->is_file_dense(f))       // P-conservative 闸门
             dense_covered.insert(uids_of(f).begin(), ...);  // 按文件、非按 DCG
}
```

性质:
- **零遗留回归**:`_file_kinds` 缺席 ⇒ `is_file_dense` 恒真 ⇒ 完全还原今天的 `column_set` 行为,旧表字节级等价;
- 顺带修复一个现状边角:旧 **dense** 层也不再被新 **sparse** 层错误释放(新 sparse 只覆盖部分行,旧 dense 仍是未覆盖行的来源)——今天的代码会错删,闸门连带修正;
- 文件累积有界:GC 不再是 sparse 的收敛机制,但 4.8.1(8 文件合并)与 4.8.2(16 文件/0.3 密度/meta 字节促升)把稳态 per-segment 文件数压在 ≤16;
- P-bitmap(按 presence 并集超集判定)评估后否决 v1:GC 持锁且有 10ms 预算(`update_manager.cpp:297-303`),不能开文件;PB 内全量位图为此付出的 meta 代价不值(等价判定本就是 4.8.1 合并 worker 的活)。schema 已为 P-bitmap 留好演进空间(P-hybrid)。
- 范围确认:坏谓词**只有一个**生产调用点(`update_manager.cpp:309`);其余 DCG 删除全是整表无条件清理(drop/rebuild/migration/snapshot,`tablet_meta_manager.cpp:1451` 族)或版本桶合并(schema change),均与覆盖谓词无关;lake 不用此函数。

### 4.9 双引擎统一: Helper 模块(v1.3:原型已落地,Spike B 双绿)

#### 4.9.1 现状(已在仓库)

```
be/src/storage/partial_update/            # 已创建
├── partial_update_helper.h/.cpp          # rowid 等价类分组的纯函数内核 (可编译、有 UT)
└── CMakeLists.txt                        # ADD_BE_LIB(PartialUpdate) + 链接 Rowset
be/test/storage/partial_update/partial_update_helper_test.cpp   # 4 个 gtest 用例
be/module_boundary_manifest.json          # 新增 partialupdate 条目 (真实 schema)
be/AGENTS.md                              # render_be_agents.py --write 再生
```

校验:`check_be_module_boundaries.py --mode full` ✅、`render_be_agents.py --check` ✅、harness 自带 CMake 解析器确认边 `Storage→{ExecCore, PartialUpdate}`、`PartialUpdate→Rowset`、`Rowset→∅` ——**无环**(`--start-group` 链接组只是最终二进制的归档符号解析,非目标依赖,harness 不解析)。

#### 4.9.2 模块边界条目(真实 schema —— v1.2 的 name/include_prefixes/target_deps/core_tests 字段不存在,会被加载器静默忽略)

```json
{
  "id": "partialupdate",
  "doc_label": "PartialUpdate",
  "summary": "SDCG (Sparse Delta Column Group) partial-update write and overlay helpers, lifted out of the Storage aggregate. Near the top of the storage stack: it may use Rowset/segment types and lower core layers, but Storage depends on it, not the reverse.",
  "owned_targets": ["PartialUpdate"],
  "owned_globs": ["be/src/storage/partial_update/**"],
  "allowed_include_prefixes": ["storage/partial_update/", "storage/rowset/", "column/", "types/",
                               "common/", "base/", "gutil/", "gen_cpp/", "fs/", "serde/", "runtime/", "util/"],
  "allowed_target_deps": ["Rowset"],
  "allowed_test_targets": ["partial_update_test"],
  "allowed_test_link_deps": ["PartialUpdate", "Rowset", "Storage", "Common", "Base", "Gutil", "StarRocksGen"],
  "remediation": "Keep PartialUpdate limited to SDCG sparse-overlay write/read helpers that depend only on Rowset/segment types and lower core layers; move broad Storage-engine coupling the other way (Storage depends on PartialUpdate)."
}
```

诚实定位:这是**近栈顶模块**,不是薄 core helper——`SegmentWriter/ColumnIterator/zone_map` 在 `storage/rowset`(target Rowset)、Roaring 在 `types/`、还要 `fs/serde/runtime/util`。`storage/` 本身今天不是受管模块,所以边界对 Storage 侧调用方向是半强制(harness 只管 PartialUpdate 的出边);真要强管 Storage→helper 边需先给 Storage 立模块,显式列为 P0 范围外。manifest 变更后必须 `render_be_agents.py --write` + `check_be_module_boundaries.py --mode full`。

#### 4.9.3 抽象与引擎相关

Helper 持有(引擎无关):
- 列等价类划分(已落仓的纯函数内核起步)
- `.spcols` 写读(SparseColsWriter / 层装配 / 坐标翻译)
- LayeredOverlayIterator 与按列 dense-pruning
- Effective ZoneMap union(含四态 null 语义)
- presence pre-filter / roaring 包装

调用方持有(引擎相关):
- 文件系统访问(local FileSystem;lake `load_segment` + `LakeIOOptions`)
- PK 索引访问(local 串行 `get_rss_rowids_by_pk` + `_index_lock`;lake 并行 `batch_get_rss_rowids_from_pkindex`)
- **冲突解决时机**(§4.7;helper 无 resolve 步骤)
- 收敛调度与提交批(本地 RocksDB batch;lake MetaFileBuilder)

---

## 5. 性能影响分析

### 5.1 与 StarRocks 现状对比矩阵(v1.3 校准)

| # | 维度 | 现状 | SDCG | 变化 | 风险 |
|---|---|---|---|---|---|
| 1 | 写: 小批量稀疏 | ROW M×N / DCG M×cols | sparse K×cols(零源段读;.upt 按列位置读) | **显著改善** | — |
| 2 | 写: 大批量稠密 | dense `.cols` | 同 dense 路径(双因子判定兜底) | 持平 | — |
| 3 | 写: 批内异构 | ❌ 不支持 | ✅(等价类分组,每组一文件) | 新能力 | — |
| 4 | 写: SQL UPDATE 点更 | **列模式已存在**,但 auto 模式禁 WHERE | 放开 WHERE 限制 + sparse | 改善(范围比 v1.2 表述窄) | — |
| 5 | 读: 无 DCG | base 直读 | 同(空层栈早返回) | 持平 | — |
| 6 | 读: 单 dense DCG | iterator 位置替换 | 同(沿用现状分支) | 持平 | — |
| 7 | 读: 单 sparse 层 | n/a | Overlay(pre-filter + 向量化) | **+0–2%**(优化后) | ⚠️ 小 |
| 8 | 读: 多版本 sparse | n/a | 升序层叠 + merge cache | **+2–8%**(优化后) | ⚠️ 需收敛兜底 |
| 9 | 谓词下推(有 DCG) | 非 key 列 segment 级跳过被关;page 级对 dense 本就工作 | segment 级重开(P0)+ page 级扩展到 sparse 层(P1) | **改善**(倍数按修正基线在 staging 量化) | — |
| 10 | Compaction | cumul + base | + sparse 合并 / promotion | **+1–3% CPU** | 🟡 轻 |
| 11 | 元数据存储 | DCG PB | + kinds/counts/presence pre-filter(全 dense 时省略,字节级零回归);位图按字节上限内联 | 受控 | 🟡 lake 防膨胀三触发兜底 |
| 12 | PK index 压力 | column mode 已点查 | 同 | 持平 | — |
| 13 | 内存占用 | iterator 状态 | + 反向索引 + 层栈 | **+几 MB/query** | 🟡 轻 |
| 14 | Variant path | ❌ | schema 预留(本期不实现) | — | — |

### 5.2 与 Doris Flexible 对比

| 维度 | Doris Flexible | SDCG |
|---|---|---|
| 每行额外开销 | bitmap × num_cols (永久) | 仅 .spcols 中,base 零侵入 |
| 占位值/默认值 | 必须填 | 不存在(rowid 集合稀疏) |
| 写时读历史 | ✅ 必须 | ❌ |
| 需要 row store | 强需要 | 不需要 |
| 写并发冲突处理 | publish 期 transient rewrite | PK 索引重查(本地)/版本串行(lake) |
| 同列单批多版本 | bitmap 显式 | 写侧去重后写赢 |
| Variant path 级 | ❌ | schema 预留 |

### 5.3 与 ClickHouse Patch Parts 对比

| 维度 | CK Patch | SDCG |
|---|---|---|
| 稀疏维度 | 仅行稀疏 | 行 + 列双稀疏 |
| 寻址 | `_part_offset` (Merge) / hash join (Join) | 物理 rowid + PK 索引重查 |
| 与 source part 生命周期耦合 | 弱 (hash join fallback) | 强(读时位置绑定;指纹断言 + 原子 GC 守护) |
| 列集合切分 | 不同分区 (爆炸风险) | 同 DCG 版本内多文件 (紧凑) |
| 批内 per-row 异构 | ❌ | ✅ |
| 谓词下推 | apply 阶段复杂 | Effective ZM(segment 级可证明安全) |
| CDC 上游负担 | 普通场景 OK;TOAST/MINIMAL 受限 | 普通场景 OK + 支持变更列子集流 |
| 生产验证 | ✅ 25.x 上线 | ❌ 设计中 |
| 运维复杂度 | 低 | 中 |

### 5.4 何时 SDCG 会比现状慢

诚实列出:

1. **多版本 sparse 未及时收敛** —— 合并 worker 跟不上时层叠 + merge cache miss,读延迟 10–50%。**必须靠后台收敛兜底**(且 GC 改为 P-conservative 后,收敛 worker 是唯一回收 sparse 的机制,监控其滞后)。
2. **单 sparse 层命中 page 内部** —— 一次向量化 `update_rows`,5–15%(优化后接近 0–2%)。
3. **极短 PK 点查** —— 反向索引构建开销;对 PK 点查走 fast path 绕过。
4. **大 K 走了 sparse** —— `fetch_values_by_rowid` 随机 seek 劣于顺序扫;双因子判定(K 绝对值上限)+ BE 保护性切换兜底;lake 对象存储下阈值更保守。

---

## 6. 实现计划(v1.3 重排)

### 6.1 优先级与路径

**P0(MVP,Lake-first;目标:结构化列 sparse 正确读写 + 不丢数据)**

已完成(本轮 spike 落仓):
1. ✅ `.spcols` 物理格式裁决(每等价类一文件,Segment v2 零改动)—— Spike A
2. ✅ Helper 模块骨架 + manifest 条目 + 边界校验双绿、无环 —— Spike B(`be/src/storage/partial_update/`)

待做:
3. `.spcols` 注册进 lake `filenames.h` 白名单(`:219/:242`)+ 往返 UT
4. DCG PB 双消息扩展(local tag 5-9;lake tag 6-9)+ `DeltaColumnGroup` C++ 镜像 + lake 三校验函数学习新数组 + 平行数组 UT
5. Roaring range-cardinality 包装(参照 `DeletionBitmap`)+ presence pre-filter
6. `SparseColsWriter`(按列 `.upt` 位置读:排序→fetch→回置;footer-only 取 M;写侧同行去重;source_rowid 保留 uid 防碰撞确认)
7. `LayeredOverlayIterator` + 反向索引 + per-column dense pruning(§4.5 权威顺序规则)+ `source_segment_num_rows` 指纹断言
8. **密度感知本地 GC(P-conservative,§4.8.5)** —— 不修则丢数据
9. **Lake `append_dcg`/`merge_dcg_meta` 密度感知改造(§4.8.4)** —— 不修则丢数据
10. Effective ZoneMap **segment 级**:`ZoneMap::union_with`(四态 null 语义 + 独立 UT 矩阵 + 随机性质测试)+ 重开 `segment.cpp:288` 判定
11. Lake 接入:插入 #71217 的 per-(column_batch,rssid) 并行 writer 循环;`enable_sparse_dcg=false` 顶层 flag;`enable_pk_index_parallel_execution` 开/关双验证
12. UT 关键面:多版本同行(≥3)读最新值;sparse-over-dense-over-base vs 全量重建逐行相等;H4 五用例(注意**测试列表必须新→老构建**,既有 `delta_column_group_test.cpp:80-84` 的旧序具有误导性);H5 七组用例(append_dcg 五例 + proto 往返 + 校验器拒绝 + split-merge + publish 链 sparse→sparse→dense + vacuum 可达性)

**P1(功能完整)**
13. 本地引擎接入(`finalize` 672-859 改造,决策点 ≈ :771)
14. SQL UPDATE:放开 auto 模式无-WHERE 限制(`UpdateAnalyzer.java:114-127`)
15. **Page 级 effective ZM(坐标翻译)+ BF 覆盖排除 + DELETE 谓词处理** —— 独立 hardened 工作流,随机差分测试门禁
16. Sparse→sparse 合并 worker(净新;与 schema-change 互斥)+ promotion(三触发)
17. Read-time merge cache、late materialization 协同、inline-PB(字节预算 + memtable 合并 + resolve 后内联重写)
18. AUTO_MODE 真正自动化(实现点 `delta_writer.cpp:307-318`)

**P2(优化)**
19. 读频驱动 promotion(先加 scan/overlay instrumentation)
20. 字典共享编码、自适应阈值、observability metrics
21. MERGE 语句承载批内异构(可选)

**独立设计轨(原 P1 项 16)**
- Variant path 级:per-row decode→splice→re-encode 原型 + 三态编码 + shredding 演化 + benchmark,先 spike 后承诺

### 6.2 关键源码改动清单(v1.3 修正)

| 改动 | 文件 | P |
|---|---|---|
| Helper 模块(已落仓) | `be/src/storage/partial_update/*`、`be/module_boundary_manifest.json`、`be/AGENTS.md` | ✅ |
| `.spcols` 扩展名注册 | `be/src/storage/lake/filenames.h:219/:242` | P0 |
| DCG PB 扩展(local) | `gensrc/proto/olap_common.proto`(tag 5-9) | P0 |
| DCG PB 扩展(lake) | `gensrc/proto/lake_types.proto`(tag 6-9) | P0 |
| DCG class 扩展 | `be/src/storage/delta_column_group.{h,cpp}`(成员/init/load/save/serialize/merge) | P0 |
| **GC 密度感知** | `delta_column_group.cpp:245-285`(P-conservative 闸门) | P0 |
| **Lake 收敛密度感知** | `be/src/storage/lake/meta_file.{h,cpp}:113-163`、`tablet_merger.cpp:262-318/:320-380` | P0 |
| SparseColsWriter | helper 模块(写入构造同 `rowset_column_update_state.cpp:390-405`/lake `:133-154`) | P0 |
| LayeredOverlayIterator + 反向索引 | helper 模块;`segment_iterator.cpp` `_dcgs`(`:469/:824`)接入 | P0 |
| 有效 ZM segment 级 | `segment.cpp:288-336` + 新 `ZoneMap::union_with` | P0 |
| Lake 读侧层栈装配 | `update_manager.cpp:1266-1298`(P0 先加 TODO/守卫,P1 改装配) | P0/P1 |
| 本地接入 | `rowset_column_update_state.cpp:672-859` | P1 |
| SQL UPDATE | `UpdateAnalyzer.java:114-127`(放开 WHERE) | P1 |
| 有效 ZM page 级 + BF + DELETE | `segment_iterator.cpp:1764-1805/:3507-3524`、`column_reader.cpp:676-729`、`scalar_column_iterator.cpp:730-736` | P1 |
| 收敛 worker | 新 worker;与 `schema_change.cpp:1161`/`tablet_updates.cpp:4110` 互斥 | P1 |
| AUTO_MODE 实现 | `be/src/storage/delta_writer.cpp:307-318` | P1 |
| Configs | `be/src/common/config.h`(同步 `docs/en|zh` BE 配置文档) | P0 |
| FE session var | `GlobalVariable.java` 等 | P1 |

### 6.3 配置项设计草案(v1.3)

```cpp
// be/src/common/config.h
CONF_mBool(enable_sparse_dcg, "false");                        // 顶层 feature flag
CONF_mDouble(sdcg_dense_threshold, "0.3");                     // K/M ≥ 此值走 dense
CONF_mInt64(sdcg_sparse_max_rows, "50000");                    // K ≥ 此值强制 dense(随机读交叉点)
CONF_mInt64(sdcg_inline_patch_max_bytes, "512");               // 内联补丁字节预算(非行数)
CONF_mInt64(sdcg_presence_bitmap_inline_max_bytes, "4096");    // roaring 内联进 PB 的上限
CONF_mInt32(sdcg_sparse_compaction_max_files, "8");            // sparse→sparse 合并触发
CONF_mDouble(sdcg_promotion_threshold, "0.3");                 // 累积 K/M 促升 dense
CONF_mInt32(sdcg_promotion_hard_count, "16");                  // sparse 文件数硬顶
CONF_mInt64(sdcg_dcg_meta_max_bytes_per_segment, "262144");    // lake meta 字节硬顶(强制促升)
CONF_mBool(sdcg_enable_effective_zone_map, "true");
```

```java
// FE session var
SET enable_sparse_dcg_update = false;
```

(v1.2 的 `sparse_dcg_update_density_threshold`/`max_columns` FE 估算路线随 §4.4.4 废弃。)

### 6.4 灰度策略

1. **Stage 0**: Helper 模块只编译(已达成),所有调用方走老路径
2. **Stage 1**: Lake 集群 `enable_sparse_dcg=true`,小流量 partial update 走 sparse;`enable_pk_index_parallel_execution` 开/关分别回归
3. **Stage 2**: Lake 全量 sparse;监控读延迟、收敛滞后(sparse 文件数分布)、dcg_meta 字节
4. **Stage 3**: 本地引擎灰度
5. **Stage 4**: 默认开启,AUTO_MODE 真正决策路径
6. **Stage 5**: SQL UPDATE 放开 WHERE

每个 stage 可一键回退(feature flag);GC/收敛改动随 flag 关闭退化为现状行为(`file_kinds` 缺席 ⇒ 全 dense ⇒ 字节级旧行为)。

### 6.5 验收标准

**功能**:
- [ ] Stream Load JSON 单批内不同行更新不同列,读取正确
- [ ] **同一行被 ≥3 个 sparse 版本更新,读到最新值**(顺序规则守门测试)
- [ ] sparse-over-dense-over-base 混合层栈 vs 全量重建逐行相等(随机化)
- [ ] **GC:新 sparse 不释放旧同列层;新 dense 释放;单 DCG 版本内 dense/sparse 混合按文件判定;legacy 全 dense 行为字节级不变**
- [ ] **Lake:publish 链 sparse(v2)→sparse(v3)→dense(v4) 元数据先链后塌;vacuum 不收链上 `.spcols`;split-merge 保版本序**
- [ ] proto 新旧 BE 双向往返;平行数组校验器拒绝畸形输入
- [ ] Effective ZM 谓词下推不漏行(随机谓词 + 全表对比;含 IS NULL / IS NOT NULL / Ne)
- [ ] **DELETE WHERE <被更新列> 后差分全表扫描一致**(P1)
- [ ] 并发 partial update 冲突解决正确(版本变化场景;`source_segment_num_rows` 指纹断言生效)
- [ ] `.spcols` 经 `gen_filename_from`/`extract_uuid_from` 往返;跨集群迁移不丢文件
- [ ] 主 compaction 后层正确物化、文件正确清理
- [ ] Lake 与本地引擎结果一致;lake 并行开关两态一致

**性能(对标现状)**:
- [ ] 小批量 partial update 写吞吐 ≥ 10× ROW_MODE
- [ ] 静态表(无 DCG)读延迟无回退
- [ ] 含 DCG 表的高选择性查询恢复 segment 级 zone map 跳过
- [ ] 连续 100 次稀疏 update 后(收敛 worker 开启)读延迟 ≤ 现状 + 10%
- [ ] CDC 场景 PK 索引争用不退化

**运维**:
- [ ] 监控覆盖:sparse 文件数分布、密度分布、overlay 耗时、收敛滞后、promotion 频次、dcg_meta 字节
- [ ] feature flag 可热切换
- [ ] tablet meta 不显著膨胀(全 dense 表字节级不变;sparse 表受三触发硬顶约束)

---

## 7. Open Questions(v1.3:已决 / 余留)

### 7.1 已决(本轮验证裁定)

1. **inline 阈值** → **字节预算**(`sdcg_inline_patch_max_bytes=512`),非行数;绝不内联 Variant/长 varchar;memtable 级合并共享 PB。
2. **promotion 阈值** → K/M 0.3 **或** 16 文件 **或** per-segment dcg_meta 字节硬顶(lake 防膨胀);v1 不用读频统计。
3. **Variant path 级** → **仅预留 schema**(§4.6),独立设计轨,先 spike 后承诺。
4. **Lake 优先还是本地优先** → **Lake-first**,理由是结构就绪(#71217 后 lake handler 是干净的 per-(column_batch,rssid) 并行 writer 循环,SparseColsWriter 低改动插入;DCG 生命周期 metastore 追踪),**不是**"lake 冲突更轻"(lake 并发其实更多活动部件);代价是 MVP 范围必须含 §4.8.4 的 meta_file/tablet_merger 改造。
5. **SQL UPDATE 何时开** → P1,且改动重定义为放开 auto 模式 WHERE 限制(§4.4.4)。
6. **`.spcols` 物理格式** → 每等价类一文件(Spike A,§4.2)。
7. **GC 策略** → P-conservative(§4.8.5),schema 为 P-bitmap 留演进位。

### 7.2 待调研

1. **Roaring presence 在生产 CDC 负载上的实际尺寸分布**(决定内联命中率与 merge cache 压力)
2. **Effective ZoneMap over-include 率** —— 坏分布下下推退化程度
3. **PK 索引在高频小批量下的争用** —— SDCG 不改善此项,评估是否成为瓶颈
4. **收敛 worker 资源开销与滞后分布** —— P-conservative 下它是 sparse 回收唯一机制
5. **`fetch_values_by_rowid` 的 K-vs-N 真实交叉点**(本地盘 vs 对象存储分别标定,校准 `sdcg_sparse_max_rows`)
6. **lake 主 compaction 物化路径对层叠语义的吃入**(§4.8.3 标记的实现期验证项)
7. **source_rowid 保留 uid 与既有哨兵 uid 的碰撞确认**(30 分钟 grep + `_verify_footer` UT)
8. **极端场景 fail-safe** —— `.spcols` 损坏时的恢复路径(presence 可由 source_rowid 列重建是已知的自愈手段之一)

### 7.3 设计预留(暂不实现,但要兼容)

1. Variant path 级(ExtendedColumnRef 已占位)
2. P-bitmap GC(presence 进 PB 的演进位已留)
3. 跨 segment 的 sparse 合并(当前绑定单 source segment;`source_segment_num_rows` 指纹使未来扩展可检验)
4. 单文件多组格式(Option b):仅当真实负载证明文件数失控且收敛不够时再启动,需自定义 footer + ordinal 平移层,波及共享读路径

---

## 8. 风险登记(v1.3)

| 风险 | 等级 | 缓解 |
|---|---|---|
| **读序/层叠实现偏离 §4.5 权威规则**(oldest-wins 回归) | 高 | 守门 UT(多版本同行);规范实现唯一化(附录 B.2) |
| **GC/收敛误删 sparse 层**(数据丢失) | 高 | P-conservative 闸门 + H4 五用例;lake append_dcg 按文件分流 + H5 用例 |
| **`source_rowid` 指向被重写/过期的 base 段**(静默错读) | 高 | PB 指纹 `source_segment_num_rows` + 打开断言;原子 GC 依赖显式化 |
| Multi-version sparse 不收敛导致读退化 | 高 | 合并/促升三触发 + 收敛滞后监控告警 |
| Lake meta(TabletMetadataPB 内嵌 dcg_meta)膨胀 | 中 | presence/inline 字节上限 + dcg_meta 字节硬顶强制促升 |
| `.spcols` 未注册扩展名 → 迁移/orphan 丢文件 | 中 | P0 第 3 项 + 往返 UT |
| 小文件爆炸(高频小批量 + 高异构度) | 中 | inline-PB 字节预算 + memtable 合并 + sparse 合并 worker |
| `ZoneMap::union_with` 四态 null 处理出错(反向漏行) | 中 | 独立 UT 矩阵 + 随机性质测试(P0 第 10 项) |
| DELETE 谓词与 overlay 交互(已删行复活) | 中 | P1 强制 DEL_PARTIAL_SATISFIED 规则 + 差分验收 |
| 平行数组失步(meta Corruption/错位) | 中 | 三校验函数同步扩展 + 拒绝用例 |
| PK 索引压力 | 中 | 现状已有,SDCG 不恶化 |
| 反向索引内存占用 | 低 | 短查询 fast path 绕过 |
| Effective ZM over-include 严重退化 | 低 | 可量化损失,设监控 |
| 文件格式/元数据向后兼容 | 低 | proto2 + 默认 DENSE + 全 dense 省略字段(字节级等价);双向往返 UT |

---

## 9. Decision Log

- **2026-06-01**: v1.2 初稿。基于对 ClickHouse Patch Parts、Doris Flexible Partial Update、Apache Paimon、Apache Hudi、Apache Pinot 的源码与文档调研,以及对 StarRocks DCG 现状的代码级分析。
- **2026-06-03**: 14 个并行 agent 对照最新 main(b5d9a6080,含 #71217/#71652 lake 并行化)逐条核验:全部基础前提成立;发现 6 处正确性硬伤(读序、per-file dense、sparse 读模型、本地 GC、lake 收敛、Variant 复用)与十余处事实引用偏差。
- **2026-06-04**: v1.3 修订定稿。Spike A(`.spcols`=每等价类一文件,Segment v2 零改动)与 Spike B(PartialUpdate 模块原型落仓,边界校验双绿、无环)完成;H4(P-conservative GC)与 H5(lake append_dcg/merge_dcg_meta 密度感知)diff 级方案定稿;Variant path 级降为 schema 预留;Open Questions 1-7 裁定(§7.1)。

---

## 10. References

### 10.1 StarRocks 源码(本仓库,v1.3 已复核)

- Partial update 模式: `gensrc/thrift/Types.thrift:568-574` `TPartialUpdateMode`(`olap_file.proto:88-92` 镜像)
- ROW mode 入口: `be/src/storage/tablet_updates.cpp:1346` `_apply_normal_rowset_commit`
- COLUMN mode 入口: `be/src/storage/tablet_updates.cpp:1133` `_apply_column_partial_update_commit`
- AUTO_MODE 注释/实现点: `be/src/storage/delta_writer.cpp:132` / **`:307-318`**
- DCG 元数据: `be/src/storage/delta_column_group.h:35`;GC `delta_column_group.cpp:245-285`;merge_by_version `:65-87`
- 稠密写入: `be/src/storage/rowset_column_update_state.cpp:319`;finalize `:672-859`;writer 构造 `:390-405`
- 冲突解决: `rowset_column_update_state.cpp:230/:258/:273`
- 读路径 DCG: `segment_iterator.cpp:1120-1136/:1138-1153/:469/:824/:1273-1283`
- Zone map: `segment.cpp:288-336`;`segment.proto:147-156`;`zone_map_index.cpp:149-152`;`column_reader.cpp:410-472/:676-729`
- 谓词单调性: `column_predicate_cmp.cpp:291-564`;`column_null_predicate.cpp:66-69/:153-156`
- Segment v2 行数/ordinal: `segment.proto:212`;`segment_writer.cpp:321-323/:398/:478-485`;`column_iterator.cpp:85-92`;`column_iterator.h:218-220`
- Column update_rows: `be/src/column/column.h:190-198`
- Roaring: `be/src/types/bitmap_value.h`(注意非 util/);range-cardinality 参照 `be/src/connector/deletion_vector/deletion_bitmap.cpp:48-52`
- Variant: `logical_type.h:73`;`variant_merger.h:35`/`.cpp:552-580/:389/:275`;`variant_value.h:63-236`;`variant_path_reader.h:23-26`;`variant_column.cpp:990/:1006`
- Lake 平行实现: `be/src/storage/lake/column_mode_partial_update_handler.{h,cpp}`;loader `:55-63`;并行循环 `:343-439`
- Lake 元数据/收敛: `lake_types.proto:95-103/:215`;`meta_file.cpp:113-163`;`tablet_merger.cpp:262-318/:320-380/:683-754`;`vacuum.cpp:303-311/:947-955/:1463-1468`;`filenames.h:219/:242`
- Lake 并行化: #71217/#71652;`lake_primary_index.cpp:483`;`exec_env.cpp:725-734`;`config.h:471`
- FE: `InsertPlanner.java:456`;`StreamLoadKvParams.java:223-235`;`BrokerLoadJob.java:287-318`;`UpdateAnalyzer.java:57-127`;`UpdatePlanner.java:117-149`
- 模块边界: `be/module_boundary_manifest.json`;`build-support/check_be_module_boundaries.py`;`build-support/render_be_agents.py`;ADD_BE_LIB `be/CMakeLists.txt:796-813`

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
- StarRocks PR #71217 — Parallelize column mode partial update publish for lake PK tables
- StarRocks PR #71652 — Parallelize row-mode partial update publish for lake PK tables
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
| **MoW / MoR** | Merge-on-Write / Merge-on-Read |
| **Patch Part** | ClickHouse 的轻量更新载体,与 base part 独立存在,读时合并 |
| **Skip Bitmap** | Doris flexible partial update 的"该列该行未更新"位图 |
| **rowid 等价类** | 同一批内 rowid 集合完全相同的更新列集合;v1.3 中 = 一个 `.spcols` 文件 |
| **Layer / 层** | 某列的一个 dense 或 sparse 覆盖文件;读时按版本升序叠加 |
| **Effective ZoneMap** | base + 层栈合成的有效区间统计(四元组格上的 join) |
| **Presence Bitmap** | 标识 sparse 文件覆盖了哪些 source rowid 的 Roaring bitmap(与文件内 source_rowid 列等价) |
| **Promotion** | sparse 层物化为 dense `.cols` 的后台动作 |
| **P-conservative GC** | v1 GC 策略:仅 DENSE 覆盖可释放旧层 |
| **source_rowid 保留列** | `.spcols` 内第 0 列,保留 uid,base rowid↔层内 ordinal 的翻译依据 |

## Appendix B. 关键算法(v1.3 规范实现)

### B.1 列等价类划分

```python
def classify_columns_by_rowid_set(update_cols, partial_update_states):
    """rowid 集合 hash 分桶 + 桶内深比对; 每个等价类 → 一个 .spcols 文件"""
    hash_to_cols = {}
    for col in update_cols:
        rowids = partial_update_states.get_rowids(col)
        h = xxhash(sorted(rowids))
        hash_to_cols.setdefault(h, []).append(col)
    result = []
    for h, cols in hash_to_cols.items():
        subgroups = {}
        for col in cols:
            key = tuple(partial_update_states.get_rowids(col))
            subgroups.setdefault(key, []).append(col)
        result.extend(subgroups.values())
    return result
```

(纯函数内核已落仓:`be/src/storage/partial_update/partial_update_helper.{h,cpp}` + UT。)

### B.2 LayeredOverlayIterator 主循环(规范实现 —— 唯一权威,_layers 升序存放)

```cpp
Status LayeredOverlayIterator::next_batch(size_t* n, Column* dst, const SparseRange<>& range) {
    // 1. base = 原生段列, 或层栈底部 DENSE 文件的列迭代器 (dense 行完整, 不进下面的循环)
    RETURN_IF_ERROR(_base_iter->next_batch(n, dst, range));

    // 2. sparse 层按版本升序 apply: 老的先, 新的后 → 最新版本最后覆盖, last-write-wins。
    //    (update_rows 是无脑覆盖, column.h:190-198; 顺序即正确性。)
    for (auto& layer : _layers) {                       // _layers: 版本升序, 已过按列剪枝
        // 2a. pre-filter: range ∩ [min,max source_rowid] 为空 → 零成本跳过
        if (!layer.range_may_overlap(range)) continue;
        // 2b. presence roaring ∩ range
        auto hit = layer.presence_intersect(range);
        if (hit.empty()) continue;                       // ★ fast path
        // 2c. base-rowid → 层内局部下标 (0..K-1), 经 source_rowid 列翻译 (升序)
        auto local_idx = layer.translate_to_local(hit);
        // 2d. 读 overlay 值 (fetch_values_by_rowid 需升序 ordinal)
        auto vals = layer.fetch_values(local_idx);
        // 2e. 向量化覆盖到 dst 内偏移
        dst->update_rows(*vals, offsets_in_dst(hit, range).data());
    }
    return Status::OK();
}
// 不变式 (DCHECK):
//   - DENSE 层若存在必为栈底且行数 == 源段 num_rows;
//   - 同版本层不重叠同行 (写侧去重保证);
//   - max(source_rowid) < source_segment_num_rows (PB 指纹)。
```

### B.3 Effective ZoneMap 合成(segment 级;四态 null 语义)

```cpp
ZoneMap effective_zone_map(uint32_t uid) {
    auto layers = collect_layers(uid);                 // 4.5.3; 升序, dense(若有)在底
    ZoneMap zm = (!layers.empty() && layers.front().is_dense())
                         ? layers.front().zone_map()   // dense 替代 base
                         : base_zone_map(uid);         // 全 sparse: base 仍有效(只松不漏)
    for (auto& l : layers | sparse_only) {
        zm = union_with(zm, l.zone_map());
    }
    return zm;
}

ZoneMap union_with(const ZoneMap& a, const ZoneMap& b) {
    ZoneMap r;
    r.has_null     = a.has_null     || b.has_null;
    r.has_not_null = a.has_not_null || b.has_not_null;
    // 全 null 操作数 (has_not_null==false) 的 min/max 非法, 必须跳过
    //   (zone_map_index.cpp:149-152; column_reader.cpp:410-430)
    if (!a.has_not_null)      { r.min = b.min; r.max = b.max; }
    else if (!b.has_not_null) { r.min = a.min; r.max = a.max; }
    else { r.min = type_min(a.min, b.min); r.max = type_max(a.max, b.max); }  // TypeInfo::cmp + delegate_type
    return r;
}
// 正确性: 每种下推谓词的 zone_map_filter 都是 (min,max,has_null,has_not_null) 格上的单调函数,
// union 是格 join ⇒ 保留集只增不减 ⇒ 不漏行。(逐谓词核验见 §4.5.4。)
```

### B.4 P-conservative GC 核心循环(见 §4.8.5;完整 diff 计划与 UT 见配套文档 [2026-06-04-sdcg-spikes-and-fix-plans.md](./2026-06-04-sdcg-spikes-and-fix-plans.md) 的 H4 章节)

---

## Appendix C. 验证与 Spike 记录(2026-06-03/04)

完整原始报告见配套文档 [2026-06-04-sdcg-spikes-and-fix-plans.md](./2026-06-04-sdcg-spikes-and-fix-plans.md)。

- **验证规模**:14 个验证/审查 agent(7 簇事实核验 × 约 40 条引用,7 维架构对抗审查)+ 4 个 spike/方案 agent;基线 commit b5d9a6080。
- **结论分布**:基础前提 100% 成立;6 处硬伤(本文 v1.3 已全部修入);事实引用偏差十余处(已全部修正,见 §3/§10)。
- **Spike A**(.spcols 格式):每等价类一文件;Segment v2/SegmentWriter 零改动;单文件多组结构性不可行(footer num_rows + ordinal 体系双重阻断)。
- **Spike B**(模块切分):`be/src/storage/partial_update/` 原型 + manifest 条目落仓;`check_be_module_boundaries.py --mode full` 与 `render_be_agents.py --check` 双绿;依赖图 `Storage→PartialUpdate→Rowset→∅` 无环。
- **H4**(本地 GC):P-conservative 方案 + 5 用例 UT 计划;唯一生产调用点 `update_manager.cpp:309`;legacy 行为字节级保留。
- **H5**(lake 收敛):append_dcg 按文件分流、merge_dcg_meta(split 路径)允许 sparse 重叠并按版本降序重排、三校验函数扩展、vacuum 已链安全;proto 自 tag 6 起。

*本文档所有性能数字基于源码分析与推理,未经真实负载验证;每个 P0 项落地时应附 microbenchmark + 端到端报告,"显著改善/改善/回退"判定需 staging A/B 后定论。*
