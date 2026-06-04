# SDCG v1.3 配套材料 —— 修订历史、验证记录、Spike 报告与 H4/H5 改造方案

- Parent: [2026-06-01-partial-update-sdcg-design.md](./2026-06-01-partial-update-sdcg-design.md) (v1.3)
- Date: 2026-06-04
- 内容: ① v1.2→v1.3 修订历史与验证结论;② Spike A (.spcols 物理格式)、Spike B (PartialUpdate 模块切分原型)、H4 (本地 GC 密度感知 diff 级方案)、H5 (lake 收敛密度感知 diff 级方案) 的完整原始报告。**主设计文档只保留最终方案,本文承载全部过程信息。**

---

## 一、修订历史与验证记录(v1.2 → v1.3)

### 1.1 验证方式

- **2026-06-01**: v1.2 初稿(research / pre-design)。基于 ClickHouse Patch Parts、Doris Flexible、Paimon、Hudi、Pinot 的源码/文档调研与 StarRocks DCG 现状代码分析。
- **2026-06-03**: 14 个并行 agent 对照最新 main(commit b5d9a6080,含 #71217/#71652 lake publish 并行化)做两轮审查——7 簇事实核验(约 40 条 `file:line` 引用逐条比对)+ 7 维架构对抗审查(读序/ZoneMap/写路径/并发/Variant/Lake 对等/收敛与兼容)。
- **2026-06-04**: 4 个 agent 完成两个 P0 spike(`.spcols` 格式、模块切分,后者真实落仓并通过仓库边界 harness)与 H4/H5 diff 级方案;v1.3 定稿。

**总体结论**:v1.2 的全部基础前提成立(AUTO_MODE 退化、列模式写放大、zone map 关闭),但存在 6 处会导致**错误结果或数据丢失**的设计硬伤与十余处事实引用偏差;全部已在 v1.3 修入。

### 1.2 六处正确性硬伤(H1–H6)

| # | 硬伤 | v1.2 的问题 | v1.3 的修正 |
|---|---|---|---|
| **H1** | 读路径合并顺序 | 正文 §4.5.2 对版本降序的 `_layers` 正向遍历 + 措辞"按版本降序应用"——`Column::update_rows` 是无脑覆盖,该顺序使**旧值赢**(同一行被多 sparse 版本更新即出错,恰是 CDC 目标场景);与附录 B.2 的 `rbegin/rend`(正确)自相矛盾 | 权威规则唯一化:**sparse 层版本升序 apply(老先新后,last-write-wins)**;守门 UT:同一行 ≥3 个 sparse 版本读最新值 |
| **H2** | dense 剪枝粒度 | `is_dense()` 被当作 DCG 级属性;实际 `file_kinds` 是**每文件**(每列组)属性,一个 DCG 版本可同时含 dense 与 sparse 文件 → 过剪/欠剪 | 剪枝谓词严格按列:`get_column_idx(uid)→file_idx`,读 `file_kinds[file_idx]`,只在**该列的 DENSE 文件**处终止 |
| **H3** | sparse 破坏首命中读模型 | 现有 `_get_dcg_segment` 返回首个(最新)含该列的 DCG——正确性完全依赖 dense 全行覆盖;sparse 只覆盖 K 行,首命中会对未覆盖行**静默遮蔽**旧值 → 错误结果 | 凡涉 sparse 一律走 LayeredOverlayIterator 层栈解析 + presence 逐行回落;只有 DENSE 终止遍历(行完整性加 DCHECK) |
| **H4** | 本地 GC 丢数据 | `garbage_collection`(`delta_column_group.cpp:245-285`)按"列 UID 被更新 DCG 列出即覆盖"释放旧 DCG——dense-only 假设;新 sparse 补丁会释放覆盖**不同行**的旧同列层;且 GC 在 `min_readable_version` 推进时触发,与 compaction 物化无关 | **P-conservative**:仅 DENSE 文件建立覆盖;sparse 仅由收敛动作提交批显式删除;`file_kinds` 缺席 ⇒ 全 dense ⇒ 旧表字节级零回归;顺带修复"新 sparse 错删旧 dense"的现状边角 |
| **H5** | Lake 收敛 dense-only 且另一套 proto | v1.2 只改了本地 `DeltaColumnGroupPB`;Lake 实际用 `DeltaColumnGroupVerPB`(`lake_types.proto:95`,**tag 5 已被 shared_files 占用**,且每 segment 单消息、entry 平行数组);`append_dcg` 按列剥离+orphan、`merge_dcg_meta` 对列重叠 `NotSupported`——均为 dense-only,sparse 补丁会丢未触及行 | Lake proto 自 **tag 6** 起扩展;`append_dcg` 按文件种类分流(sparse 不剥离不 orphan;dense 照旧并连带清理旧 sparse);`merge_dcg_meta`(tablet split 路径)允许 sparse 重叠并按版本降序重排;三校验函数学习新数组并放宽 sparse 链重复 UID;vacuum 经核实已链安全 |
| **H6** | Variant 复用前提为假 | "复用 `VariantColumnMerger`,path 级合并算法已就绪"——实际该类做**整列垂直行拼接 + 列级 schema 调和**(`merge_into→dst->append(src,0,src.size())`),不做逐行 path 打补丁;`VariantRowValue` 不可变,全 BE 无逐行 path 变更原语;三态语义(kMissing/kNull/kValue)与 shredding 演化均未设计 | **降为仅预留 schema**(`ExtendedColumnRefPB.variant_path` 占位,BE 拒绝/回退非空 path),移入独立设计轨;可合法复用的只有类型选举(`arbitrate_type_conflicts/choose_common_type`)用于未来 compaction 提升 |

### 1.3 事实性引用修正表(v1.2 → 实际)

| v1.2 写法 | 实际(已核验) |
|---|---|
| AUTO_MODE 退化/实现点 `delta_writer.cpp:132` | `:132` 只是注释(位于 sort-key-conflict 辅助函数内);实际退化由列模式分支 `:307-318` 不匹配 AUTO_MODE 实现,未来 AUTO 自动化的实现点也是 `:307-318` |
| "FE 默认下发 AUTO_MODE" | 对 INSERT/SQL 成立(`InsertPlanner.java:456`);Broker Load 默认 `UNKNOWN_MODE`(`BrokerLoadJob.java:287`),仅显式 `auto` 才发;结论不变(同样退化 ROW) |
| `finalize` 在 `rowset_column_update_state.cpp:735-825` | 实际 `:672-859`;`.cols` 写出循环 `:769-825`,DCG 元数据构建 `:827-832` |
| `_dcg_segments` 在 `segment_iterator.cpp:1127` | 声明在 `:469` |
| "DCG 存在 ⇒ 所有列 zone map 全失效,IO 100× 放大" | 仅**非 key 列的 segment 级跳过**被关(key 列豁免 `segment.cpp:322`);page 级 ZM/BF 对 dense DCG 今天就工作(不受 DCG 门控);收益须按修正基线重新量化 |
| Roaring 在 `be/src/util/bitmap_value.h`,有 `rangeCardinality` | 实际在 `be/src/types/bitmap_value.h`;**没有** `rangeCardinality`,须新增包装(参照 `DeletionBitmap::get_range_cardinality`)或 `bitmap_subset_in_range_internal`+`cardinality` |
| manifest 字段 `name/include_prefixes/target_deps/core_tests` | 真 schema 是 `id/doc_label/summary/owned_targets/owned_globs/allowed_include_prefixes/allowed_target_deps/allowed_test_targets/allowed_test_link_deps/remediation`;v1.2 的字段会被加载器**静默忽略**(空模块) |
| "ZoneMap 或 distinct value zonemap";`ZoneMap::union_with` 既有 | SR ZM 只有 `(min,max,has_null,has_not_null)` 四元组;`union_with` 不存在,须从零实现(全 null 操作数 min/max 非法须跳过) |
| `encryption_metas` 是 `repeated string` | 实际 `repeated bytes` |
| `.upt` 迭代器支持 `read_selective` | 段级 ChunkIterator 只有顺序读;位置读须按列 `fetch_values_by_rowid`(要求升序 ordinal,而 upt_rowid 按 source_rowid 排序后乱序 → 必须排序+回置);大 K 随机 seek 劣于顺序扫 |
| "M(源段行数)在 finalize 免费可得" | 只有 per-rowset 行数;per-segment M 须 footer-only open 取得(廉价,但须显式) |
| "FE 加 selectivity 估算选路径;ColumnPatchSink 取代重 ROW_MODE SQL UPDATE" | FE 无估算;列模式 SQL UPDATE 已存在且只读 PK+SET;真实限制是 auto 模式禁 WHERE(`UpdateAnalyzer.java:114-127`)→ FE 改动 = 放开该限制,BE 是密度决策唯一权威 |
| "inline-PB 阈值 K=4/8/16(行数)" | 改为**字节预算**(PB 常驻 + lake 每版本重传);绝不内联 Variant/长 varchar |
| "并发模型:_index_lock 串行,双引擎沿用" | 本地串行主靠 `do_apply` 单线程(`_apply_running`),`_index_lock` 保护索引重查;**lake 列模式已并行化**(#71217,`need_lock=false` + 专用线程池)且 handler 内无 resolve → 不变式按引擎分述,helper 须线程安全 |
| "sparse 合并复用 `merge_by_version`" | 该函数只拼接**同版本**文件列表、只接在 schema change 上;跨版本 latest-per-rowid 合并是**净新逻辑** |
| "promotion 读频优先级公式" | 依赖的 `scan_frequency/read_overlay_us` 等统计不存在;v1 触发只用文件数+密度,读频降 P2 |
| "主 compaction 顺带 GC,复用 garbage_collection" | 见 H4:GC 在 min_readable 推进即触发、与物化无关;对 sparse 不安全 |
| 单文件多组 `.spcols`(各列行数不同) | Segment v2 结构性不可行(footer 单 num_rows + writer 等长校验 + ordinal 体系);改为每等价类一文件(Spike A) |

### 1.4 Open Question 裁决记录

| 问题 | 裁决 | 依据 |
|---|---|---|
| inline 阈值 | 字节预算 512B(非行数) | PB 常驻 + `segment_iterator.cpp:824` 每查询整载 + lake 每版本重传 |
| promotion 阈值 | K/M 0.3 或 16 文件或 meta 字节硬顶 | 全部可零 IO 计算;读频统计不存在 |
| Variant path 级 P1? | 否,仅预留 schema | H6;依赖两个未建迭代器 + 三态/演化未设计 |
| Lake-first or 本地-first | Lake-first | #71217 后 lake handler 是干净的 per-(column_batch,rssid) 并行 writer 循环,低改动插入;**非**因"lake 冲突更轻"(其实并发部件更多);代价是 MVP 必含 meta_file/tablet_merger 改造 |
| SQL UPDATE 何时开 | P1,改动重定义为放开 auto 模式 WHERE 限制 | FE 无估算、列模式已存在 |
| `.spcols` 格式 | 每等价类一文件 | Spike A |
| GC 策略 | P-conservative(留 P-bitmap 演进位) | H4;GC 持锁 10ms 预算不能开文件 |

---

## SPIKE A: Can Segment v2 / SegmentWriter carry the .spcols layout?

### Bottom line
**Recommend Option (a): ONE `.spcols` Segment-v2 file PER rowid-equivalence-group.** Every column inside one file shares the same K rows, so `SegmentWriter` works **completely unchanged** — `num_rows` is uniform within the file, which is exactly what Segment v2 hardcodes. The "different row counts in one file" layout sketched in doc §4.2 is **structurally impossible without invasive Segment-v2 surgery**; per-group files sidestep that entirely and `DeltaColumnGroupPB.column_files` (repeated) already models multiple files per DCG version. This is gating-P0-unblocking: implementation can start on Option (a).

---

### 1. Where `num_rows` lives, and why per-column divergent row counts are blocked

**One `num_rows` per segment, baked into the footer and every consumer:**

- `SegmentFooterPB.num_rows` is a single `uint32` for the whole file — `gensrc/proto/segment.proto:212`.
- Writer side: `SegmentWriter::_num_rows` is one value; `finalize_columns()` *enforces* every appended column has the same count: `be/src/storage/rowset/segment_writer.cpp:321-323` returns `InternalError("num rows written $0 is not equal to segment num rows $1")` if `_num_rows != _num_rows_written`. So you cannot append col_a with K_a rows then col_b with K_b rows into one writer — the second `append_chunk`/finalize diverges and fails.
- `_write_footer()` writes the single value: `segment_writer.cpp:398` `_footer.set_num_rows(_num_rows)`.
- Read side: `Segment::_open` sets `_num_rows = footer.num_rows()` (`be/src/storage/rowset/segment.cpp:283`); `Segment::num_rows()` (`segment.h:196`) returns it. Consumers assume it is THE row count: `new_column_iterator_or_default` builds default/EMPTY iterators sized to `num_rows()` (`segment.cpp:524,548,597`), zone-map filter stats use it (`segment.cpp:345`), virtual-column factory uses it (`segment_iterator.cpp:1210`).
- `ColumnMetaPB.num_rows` (`segment.proto:192`) is per-column BUT the comment says it is "required by array/struct/map reader to create child reader" — it is a child-count helper, not a license for top-level columns to disagree with the footer. The ordinal index of every top-level column is built assuming positions `0..footer.num_rows-1`.

**Ordinal index is the real blocker.** Columns are read by *ordinal* (physical position in the file), not by a stored rowid. The dense `.cols` path works precisely because the dense file has exactly N rows in 1:1 ordinal correspondence with the base segment, so `seek_to_ordinal(base_rowid)` lands on the right value:
- `SegmentIterator::_seek_columns` → `_column_iterators[f->id()]->seek_to_ordinal(pos)` (`segment_iterator.cpp:1928`), `pos` = base segment rowid.
- `ColumnIterator::fetch_values_by_rowid` default impl literally does `seek_to_ordinal(rowids[i]); next_batch(...)` (`be/src/storage/rowset/column_iterator.cpp:85-92`) — rowid IS ordinal.

So a single file with col_a at K_a positions and col_b at K_b positions has no coherent ordinal space — `seek_to_ordinal` would mean different physical rows for different columns. **Per-column divergent row counts in one Segment-v2 file are not representable without a custom ordinal-translation layer per column (Option b territory).**

### How the dense `.cols` file is read today (schema + column lookup by uid)

- `SegmentIterator::_get_dcg_segment(ucid)` (`segment_iterator.cpp:1120`) iterates `_dcgs` newest→oldest, calls `dcg->get_column_idx(ucid)` (`delta_column_group.h:51`) to find which `.cols` file holds that uid, then `Segment::new_dcg_segment(dcg, idx, schema)`.
- `Segment::new_dcg_segment` (`segment.cpp:637-651`) builds a **partial TabletSchema from the DCG's `column_ids()[idx]`** via `TabletSchema::create_with_uid(...)`, then `Segment::open` on the `.cols` file. The `.cols` segment's `_column_readers` are keyed by `unique_id` (`segment.cpp:611-612` `_column_readers.find(id)`), so lookup is by uid.
- The DCG column iterator then participates in normal `seek_to_ordinal` / `next_batch` over the full base ordinal range — i.e. the dense file is logically a full-height replacement column. This is the invariant Option (a) must preserve via per-column rowid translation in the overlay iterator (sparse values sit at local positions 0..K-1, NOT at base-rowid positions, so the reader must translate base-rowid → local-index before `fetch_values_by_rowid`).

---

### 2. How the dense writer is created today (both engines)

**Local** — `RowsetColumnUpdateState::_prepare_delta_column_group_writer` (`be/src/storage/rowset_column_update_state.cpp:390-405`):
- builds `.cols` path via `Rowset::delta_column_group_path(...)`, deletes any stale file, opens a `WritableFile`, constructs `SegmentWriter(wfile, segid, partial_tschema, SegmentWriterOptions{})`, `init(false)` (`has_key=false`).
- The `partial_tschema` is `TabletSchema::create(tschema, selective_update_column_uids)` (`rowset_column_update_state.cpp:782`).
- Encryption: local path passes empty `SegmentWriterOptions` (no encryption_meta) and `init({}, ...)` defaults — encryption_metas in DCG metadata stay `{}` (see `finalize` line 830 `init(..., dcg_column_files, {}, ...)`).
- Column uids land in DCG metadata via `dcg_column_ids[rssid].push_back(selective_unique_update_column_ids)` paired with `dcg_column_files[rssid].push_back(file_name(...))` (`rowset_column_update_state.cpp:819-820`); the `{uids[]} ↔ files[]` positional mapping is documented at lines 763-765.
- Then `DeltaColumnGroup::init(version, dcg_column_ids, dcg_column_files, {}, file_size)` (`rowset_column_update_state.cpp:828-831`).

**Lake** — `ColumnModePartialUpdateHandler::_prepare_delta_column_group_writer` (`be/src/storage/lake/column_mode_partial_update_handler.cpp:133-154`):
- path via `params.tablet->segment_location(gen_cols_filename(_txn_id))`, schema `TabletSchema::create_with_uid(params.tablet_schema, selective_unique_update_column_ids)` (line 349).
- Encryption IS produced per file here: if `config::enable_transparent_data_encryption`, `KeyCache::create_encryption_meta_pair_using_current_kek()` → `writer_options.encryption_meta` (lines 145-149). The meta is harvested via `delta_column_group_writer->encryption_meta()` and stored alongside the file: `dcg_column_file_with_encryption_metas[rssid].emplace_back(file_name(...), encryption_meta())` (lines 416-419), committed via `builder->append_dcg(rssid, files_with_metas, column_ids)` (line 442).

**Takeaway for SDCG:** the writer-creation lambda is the natural seam. A `SparseColsWriter` reuses the *exact same* `SegmentWriter` construction (same options, same encryption flow, same `init(false)`); only the schema and the appended chunk differ. The `{uids[]} ↔ files[] ↔ encryption_metas[]` positional vectors already accommodate N files per DCG version — adding more sparse files is just more entries.

---

### 3. Format options evaluated

| Option | Segment-v2 impact | Files per batch | Verdict |
|---|---|---|---|
| **(a) one .spcols per rowid-equivalence-group** | **zero** — uniform K per file, `source_rowid` is just another column | more files when many distinct rowid sets; collapses to 1 file when all updated cols share rowids (the common CDC case) | **RECOMMENDED** |
| (b) one file, multi-group, custom footer ext for per-col K | high — need a new footer field, per-column ordinal-base offsets, and a custom ColumnIterator that translates global→per-group ordinal; breaks the `seek_to_ordinal == position` contract that `fetch_values_by_rowid`/`_seek_columns` rely on | fewest files | rejected for P0: invasive, touches the shared Segment-v2 read path used by ALL tables (regression blast radius), violates `_num_rows`/footer invariant |
| (c) one file, pad all groups to max K | zero code, but stores placeholder values | 1 file | rejected: violates the design's explicit "no placeholder values" invariant |
| (d) reuse `.upt` writer infra | `.upt` files are written by the same `SegmentWriter` (RowsetWriter path) and are already uniform-height per file — no advantage over (a); `.upt` is the *source* of values, not the overlay store | — | subsumed by (a) |

**Why (a) is genuinely cheap on file count under the design's grouping:** doc §4.2's "列分组优化" already groups columns into rowid-equivalence classes (Appendix B.1 `classify_columns_by_rowid_set`). The degenerate-but-dominant case — a batch where every updated column shares the same set of updated rows (classic CDC: same rows change the same column set) — produces exactly ONE equivalence class → ONE `.spcols` file. Files multiply only when the batch is genuinely heterogeneous (row 100 changed col_a, row 305 changed col_b), which is precisely the case the design wants to support and where extra files are the honest cost of avoiding placeholders. And `DeltaColumnGroupPB.column_files` being `repeated` means K files per DCG version is already first-class — no metadata stretch.

---

### 4. Where the presence bitmap lives (recommended)

**Recommendation: presence bitmap as a sidecar `bytes` field in `DeltaColumnGroupPB` / `DeltaColumnGroupVerPB`, one Roaring blob per `.spcols` file (per equivalence group).** Rationale tied to the two consumers:

- **GC / coverage checks (H4: cheap coverage without opening files):** GC and dense-pruning decisions need "which base rowids does this layer cover?" If the bitmap is a column *inside* the `.spcols` file, answering requires opening the segment and reading a page. Putting it in the DCG metadata PB (which is already loaded eagerly by `DeltaColumnGroupLoader::load` — see `segment.cpp:299` and the lake loader at `column_mode_partial_update_handler.cpp:55-63`) makes coverage a pure in-memory metadata operation. Roaring blobs for sparse K are tiny (doc §4.5.5; far under the §5.1 "+<100B/DCG" budget for typical K).
- **Read path (build overlay layer without reading value columns):** `LayeredOverlayIterator` (doc §4.5.2 / Appendix B.2) intersects `presence_bitmap & range` to decide fast-path skip BEFORE touching value columns. Having the bitmap already in memory from DCG-load means the per-page skip check costs zero IO. This is the whole point of the presence-bitmap fast-path (doc §4.5.5).
- The **`source_rowid` reserved column inside the file is still the source of truth** for translating base-rowid → local-index (0..K-1) when a page IS hit; the sidecar bitmap is the redundant fast index. They agree by construction (bitmap = set of values in source_rowid column). For robustness, the bitmap can be regenerated from the source_rowid column if a metadata/file mismatch is ever detected.

(Storing it as its own column in the file is the fallback if PB-size pressure ever appears, but it loses the no-open GC property — not recommended for P0.)

---

### 5. THE recommendation — concrete spec

**Option (a): one Segment-v2 `.spcols` file per rowid-equivalence-group. `source_rowid` is a reserved-uid column; presence bitmap is a DCG-PB sidecar. No SegmentWriter or SegmentFooterPB changes for P0.**

**5.1 Writer API — reuses existing infra unchanged.**
- Build a partial `TabletSchema` for the group = `[source_rowid_column] + [update value columns of this equivalence class]`. All have K rows.
- Construct `SegmentWriter` identically to the dense path (`rowset_column_update_state.cpp:400-403` / lake `column_mode_partial_update_handler.cpp:137-152`): same `SegmentWriterOptions`, same encryption flow (lake creates the meta pair, local leaves empty), `init(false)` (no key columns — `.spcols` has no short-key index, same as `.cols`).
- `append_chunk(chunk)` once with a K-row chunk: column 0 = sorted source_rowids (`UInt32`), columns 1..M = the selected values pulled from `.upt` by upt_rowid (doc §4.4.3 `write_sparse_percol_file`). **No source-segment read** — satisfies the invariant; values come from `.upt` via `rowset->get_update_file_iterator` (the existing `_update_source_chunk_by_upt` machinery at `rowset_column_update_state.cpp:450-478` already reads upt by rowid).
- `finalize(&size, &index, &footer_pos)` — unchanged; the uniform K makes `finalize_columns`' equality check (`segment_writer.cpp:321`) pass trivially.

**5.2 Schema for the reserved `source_rowid` column.**
- Type `TYPE_INT`/`UInt32` (matches `rowid_t`), nullable=false, sorted ascending. Give it `need_zone_map=true` (min/max rowid → page-level range pruning when intersecting with `SparseRange`) and optionally a bloom filter (doc §4.2 sketch).
- **Reserved unique_id concern (verified):** `SegmentWriter::_verify_footer` (`segment_writer.cpp:478-485`) CHECKs that footer column unique_ids are unique. Pick a reserved uid in a range that cannot collide with real table column uids (StarRocks already reserves sentinel uids, e.g. the `__row__`/`FULL_ROW_COLUMN` and op-column conventions). The reserved uid must NOT appear in `DeltaColumnGroup::column_ids()` (the uid→file index built by `get_column_idx` at `delta_column_group.h:51`), so the overlay iterator never tries to resolve a real column to the source_rowid column. Add it only to the on-disk partial schema, not to the DCG uid mapping.

**5.3 Footer / meta changes.**
- **Segment footer: none.** This is the key win — `.spcols` is a vanilla Segment-v2 file.
- **DCG metadata:** add the §4.3 fields to `DeltaColumnGroupPB` (`gensrc/proto/olap_common.proto`) and `DeltaColumnGroupVerPB` (`gensrc/proto/lake_types.proto`): `file_kinds` (DENSE_COLS=0 default → backward compatible), `sparse_row_counts` (the K per file), `presence_bitmaps` (repeated bytes, Roaring, one per file), and the optional `extended_refs`. **Tag discipline:** `olap_common.proto DeltaColumnGroupPB` tags 1-4 are used → new fields start at 5; `lake_types.proto DeltaColumnGroupVerPB` tags 1-5 are used (tag 5 = `shared_files`) → new fields start at 6. All optional/repeated, never required, never reuse ordinals (per gensrc rules). Mirror into `DeltaColumnGroup` C++ class (`delta_column_group.h:113-119` private members + `init()` signature, plus `save()`/`load()`).

**5.4 Read-path open procedure.**
- Reuse `_get_dcg_segment` / `new_dcg_segment` flow (`segment_iterator.cpp:1120`, `segment.cpp:637`) to open the `.spcols` segment — it is just a Segment-v2 file, `Segment::open` works as-is. Build the partial schema from the value-column uids only (exclude the reserved source_rowid uid from the uid map; load the source_rowid column via its reserved uid separately).
- The `LayeredOverlayIterator` (the new component, doc §4.5.2): for each layer, intersect the in-memory presence bitmap with the requested `SparseRange`. On a hit, read the `source_rowid` column to map base-rowids → local indices 0..K-1, `fetch_values_by_rowid(local_indices)` on the value column (this works because in the sparse file the values ARE at ordinal 0..K-1), then `dst->update_rows(values, local_offsets)` (vectorized blend, `column.h:198`). This is the only place that must NOT use raw base-rowid as ordinal — it must translate first. The dense path keeps using base-rowid==ordinal directly.

**5.5 File-count implication.** Under the design's equivalence-class grouping: homogeneous batch (all cols same rowids) → 1 `.spcols` file (identical file count to today's dense single-group case). Heterogeneous batch with G distinct rowid sets → G files, each a clean uniform-K Segment-v2 file, all hung off one DCG version via the already-repeated `column_files`. Small-file blow-up at high G is mitigated by the design's inline-PB path (K ≤ threshold) and sparse-compaction worker (doc §4.8.1) — out of scope for this spike but the format does not stand in the way.

**Invariants preserved:** no source-segment read (values from `.upt`); no placeholder values (each file holds exactly its K real rows); backward compatible (FileKind defaults to DENSE_COLS so old DCGs parse unchanged).

### Risks / alternatives noted
- Option (a)'s file multiplication under extreme heterogeneity is real but bounded and addressed by inline-PB + sparse compaction (separate spikes).
- If a future requirement forces single-file multi-group (to cap file count hard), Option (b) becomes necessary — but it requires a custom ColumnIterator with per-group ordinal bases and a footer extension, touching the shared read path. Defer until file-count data from a real CDC load justifies the regression risk. P0 should ship (a).
- One concrete follow-up to validate during implementation: confirm the chosen reserved source_rowid uid does not collide with any existing sentinel uid (`FULL_ROW_COLUMN`, op column, shadow columns) across both engines — a 30-min grep + a `_verify_footer` unit test.

---

## SPIKE B: PartialUpdate 模块切分原型(已落仓,边界校验双绿)

## What I built and verified

Prototyped a real `storage/partial_update` BE library, wired it into CMake exactly like `rowset/`, added a manifest entry using the REAL schema, and ran the repo's boundary harness. Both load-bearing checks PASS; the dependency graph has no cycle.

---

## 1. Existing CMake / target-link graph (actual edges)

**ADD_BE_LIB macro:** `be/CMakeLists.txt:796-813` — `FUNCTION(ADD_BE_LIB LIB_NAME)`. Builds a STATIC lib (or SHARED under `ENABLE_MULTI_DYNAMIC_LIBS`), appends to `ALL_BE_LIBS`. It does NOT add link deps itself.

**Subdirectory wiring (two different levels):**
- `add_subdirectory(rowset)` lives INSIDE `be/src/storage/CMakeLists.txt:20` (nested under storage).
- `add_subdirectory(${SRC_DIR}/storage)` lives at the top level `be/CMakeLists.txt:965`.
- So `partial_update/` is wired the same way `rowset/` is: a nested `add_subdirectory` inside `be/src/storage/CMakeLists.txt`.

**Rowset target:** `be/src/storage/rowset/CMakeLists.txt:7-13` — `ADD_BE_LIB(Rowset ...)`. It has ZERO `target_link_libraries` anywhere in the tree (verified by grep). It is consumed only via the executable-level link group.

**Storage target:** `be/src/storage/CMakeLists.txt:312-316` — `ADD_BE_LIB(Storage ${STORAGE_FILES})` then `target_link_libraries(Storage PUBLIC ExecCore)`. The `STORAGE_FILES` list aggregates both engines' partial-update files: `rowset_column_update_state.cpp` (line 49) and `lake/column_mode_partial_update_handler.cpp` (line 276).

**Relevant explicit edges (the only ones the harness tracks):**
- `Storage -> ExecCore` — `be/src/storage/CMakeLists.txt:316`
- `ExecCore -> ExprCore, RuntimeCore` — `be/src/exec/CMakeLists.txt:36`
- `Exec -> ExecCore, SpillCore, ExecSchemaScannerCore, ExecSchemaScanners` — `be/src/exec/CMakeLists.txt:495`
- `Runtime -> RuntimeCore, ExecCore` — `be/src/runtime/CMakeLists.txt:147`
- `Rowset` — no explicit out-edges.

**Who "links" Storage / Rowset:** Nobody via `target_link_libraries`. Both appear in the executable-level group list `STARROCKS_LINK_LIBS` (`be/CMakeLists.txt:537-577`, `Storage` at line 549, `Rowset` at line 550) wrapped in `-Wl,--start-group`/`--end-group` (lines 528-529). That group is an archive-ordering device at the final binary, not a per-target dependency, and the harness does not parse it.

---

## 2. Files created (honest stubs)

- `be/src/storage/partial_update/partial_update_helper.h` — `namespace starrocks`, declares `group_by_source_rowid(const std::vector<std::pair<uint32_t,uint32_t>>&)` returning per-`source_rowid` equivalence classes (`RowidGroup{source_rowid, begin, end}`). Pure, no storage/fs/runtime includes; only `<cstdint>/<utility>/<vector>`.
- `be/src/storage/partial_update/partial_update_helper.cpp` — stable-sort + contiguous-run grouping. Stable order preserves write order so the overlay can apply oldest→newest, last-write-wins (matches the SDCG semantics). Only includes its own header + `<algorithm>`.
- `be/src/storage/partial_update/CMakeLists.txt` — `ADD_BE_LIB(PartialUpdate partial_update_helper.cpp)` + `target_link_libraries(PartialUpdate PUBLIC Rowset)`, mirroring `rowset/CMakeLists.txt` (sets LIBRARY/EXECUTABLE_OUTPUT_PATH, then ADD_BE_LIB).
- `be/test/storage/partial_update/partial_update_helper_test.cpp` — 4 gtest cases (empty, single, ascending+contiguous grouping, stable-within-group). Registered in `DW_TEST_FILES` so it compiles with the standard BE test suite.

I deliberately kept the stub's actual `#include` surface minimal so it compiles and passes include checks, while the MANIFEST honestly declares the broader planned dependency surface.

## 3. Files modified

- `be/src/storage/CMakeLists.txt` — added `add_subdirectory(partial_update)` (after `add_subdirectory(rowset)`); changed `target_link_libraries(Storage PUBLIC ExecCore)` → `... ExecCore PartialUpdate` (Storage now depends on PartialUpdate).
- `be/CMakeLists.txt` — added `PartialUpdate` to `STARROCKS_LINK_LIBS` right after `Rowset` (line ~551) so the static lib links into the binary, mirroring Rowset.
- `be/test/CMakeLists.txt` — added the test source to `DW_TEST_FILES`.
- `be/module_boundary_manifest.json` — new `partialupdate` entry (below).
- `be/AGENTS.md` — regenerated by `render_be_agents.py --write` (8 lines added: the generated `### PartialUpdate` block).

---

## 4. Final manifest entry (REAL schema)

```json
{
  "id": "partialupdate",
  "doc_label": "PartialUpdate",
  "summary": "SDCG (Sparse Delta Column Group) partial-update write and overlay helpers, lifted out of the Storage aggregate. Near the top of the storage stack: it may use Rowset/segment types and lower core layers, but Storage depends on it, not the reverse.",
  "owned_targets": ["PartialUpdate"],
  "owned_globs": ["be/src/storage/partial_update/**"],
  "allowed_include_prefixes": ["storage/partial_update/", "storage/rowset/", "column/", "types/", "common/", "base/", "gutil/", "gen_cpp/", "fs/", "serde/", "runtime/", "util/"],
  "allowed_target_deps": ["Rowset"],
  "allowed_test_targets": ["partial_update_test"],
  "allowed_test_link_deps": ["PartialUpdate", "Rowset", "Storage", "Common", "Base", "Gutil", "StarRocksGen"],
  "remediation": "Keep PartialUpdate limited to SDCG sparse-overlay write/read helpers that depend only on Rowset/segment types and lower core layers; move broad Storage-engine coupling the other way (Storage depends on PartialUpdate)."
}
```

Style matches `fscore`/`spillcore`/`execcore` (owned_globs `**`, prefix lists, deps, test fields, remediation). `Rowset` is in `allowed_target_deps` because that is the only `target_link_libraries` edge PartialUpdate declares; `storage/rowset/` is in `allowed_include_prefixes` because `rowset.h` is owned by the `Rowset` target (so it would resolve via the owner check at `check_be_module_boundaries.py:351-353`, where `Rowset` is now an allowed owner).

**Harness tolerance of a not-yet-standalone test target — confirmed from the code:** `check_test_links_for_module` (lines 410-425) iterates `module.allowed_test_targets` and does `cmake_state.test_target_links.get(test_target, [])`, which returns `[]` for an absent target → zero violations. Also, the parser only records test links from literal `target_link_libraries` commands in `be/test/**` (lines 207-222); `ADD_BE_TEST` (`be/CMakeLists.txt:913-928`) is a custom function whose internal `TARGET_LINK_LIBRARIES` is NOT expanded by the static parser, so even a real `ADD_BE_TEST(partial_update_test)` would record no edges. Declaring `partial_update_test` is therefore safe today; the compiled test currently rides in the `starrocks_test` aggregate.

---

## 5. Commands run and outcomes

| Command | Outcome |
|---|---|
| `python3 build-support/check_be_module_boundaries.py --mode full` (before) | `OK: ... clean for all modules.` EXIT 0 (clean baseline) |
| `python3 build-support/render_be_agents.py --check` (before) | `OK: ... matches ...` EXIT 0 |
| `python3 build-support/check_be_module_boundaries.py --mode full` (after) | `OK: BE module boundaries clean for all modules.` EXIT 0 ✅ |
| `python3 build-support/render_be_agents.py --check` (after manifest edit) | EXIT 1 — `be/AGENTS.md is out of date ... run --write` (correctly demands regen) |
| `python3 build-support/render_be_agents.py --write` | `Updated be/AGENTS.md` — regenerated the generated section (+8 lines: the `### PartialUpdate (partialupdate)` block) |
| `python3 build-support/render_be_agents.py --check` (re-verify) | `OK: ... matches ...` EXIT 0 ✅ |
| `python3 build-support/check_be_module_boundaries.py --mode changed --base main` | `OK: ... clean for base, ..., partialupdate, ...` EXIT 0 ✅ (partialupdate selected because manifest is in DEFAULT_CHANGED_FULL_CHECK_PATHS) |
| `python3 build-support/check_be_module_boundaries.py --mode changed --base main --enforce-baseline-shrink` | EXIT 1 — **pre-existing, NOT my change** (see below) |
| `--base HEAD --enforce-baseline-shrink` | EXIT 0 ✅ |
| parser instrumentation (import check_be_module_boundaries, parse_cmake_state) | `PartialUpdate` sources=`[partial_update_helper.cpp]`, links=`['Rowset']`; `Storage` links=`['ExecCore','PartialUpdate']`; `Rowset` links=`None` |

Invocation/flags taken straight from `build-support/check_be_module_boundaries.py:670-681` (argparse) and the documented commands in `be/AGENTS.md`.

**The `--enforce-baseline-shrink` EXIT 1 is a worktree branch-divergence artifact, not caused by this spike:** `build-support/be_module_boundary_baseline.json` does not exist on `main` (`git show main:... → exists on disk, but not in 'main'`). When the guard loads main's baseline via git, `_load_baseline_from_git` hits a missing-marker and returns an empty baseline (`check_be_module_boundaries.py:626-631`), so it reports every current baseline entry (the two deferred allocator entries `columncore`/`runtimecore`) as a "new addition." I did NOT touch the baseline file (`git status` shows it unmodified), and `--base HEAD` passes cleanly. This guard would be green against a real PR base that already contains the baseline file.

---

## 6. Cycle verdict — NO CYCLE

Graph after the change, restricted to harness-tracked `target_link_libraries` edges:

```
Storage ──► ExecCore ──► ExprCore, RuntimeCore
Storage ──► PartialUpdate ──► Rowset ──► (no out-edges)
```

- `Storage` reaches `PartialUpdate`: **yes** (the new edge).
- `PartialUpdate` reaches `Storage`: **no** — `PartialUpdate -> Rowset`, and `Rowset` has zero `target_link_libraries` edges (verified by grep and by the parser: `Rowset links: None`). So the path dead-ends at Rowset.
- Therefore `Storage -> PartialUpdate -> Rowset` is acyclic. The would-be back-edge `Rowset -> Storage` does not exist.

The only "cycle" in the tree is the intentional executable-level `-Wl,--start-group ... --end-group` archive group in `be/CMakeLists.txt:537-564`, which is a static-archive symbol-resolution mechanism on the final binary, not a CMake target dependency. It is invisible to the harness and is not a real per-library cycle.

**Cheap-validation caveat (as requested):** I did NOT run a BE build or `cmake` configure (both require thirdparty/STARROCKS_THIRDPARTY and the full toolchain). A standalone `cmake -P` cannot validate these `add_subdirectory`/`target_link_libraries` relationships in isolation because they depend on the whole project's target graph and thirdparty discovery. Validation here is: (a) the python boundary harness (passes), (b) the render harness (passes), and (c) static re-use of the harness's own CMake parser to confirm the captured edges. The C++ stub is written to be standard-conformant and self-contained, but its actual compilation is left to a real BE build.

---

## Test plan for the eventual real change
1. `./run-be-ut.sh --build-target partial_update_test --module partial_update_test --without-java-ext` once a standalone `ADD_BE_TEST(storage/partial_update/partial_update_helper_test)` target is added (or run the existing aggregate that now includes the source).
2. `python3 build-support/check_be_module_boundaries.py --mode full` and `python3 build-support/render_be_agents.py --check` (both green now).
3. On a real PR: `--mode changed --base origin/main --enforce-baseline-shrink` (will be green when the base already carries the baseline file).
4. Full `./build.sh --be` to confirm the static link order in the `--start-group` block resolves `PartialUpdate <-> Rowset <-> Storage` symbols (the group handles mutual archive references at link time).

---

# H4 — Make DCG Garbage Collection Density-Aware (local engine)

## 1. The GC and its full caller chain (verified)

### 1.1 The buggy predicate
`be/src/storage/delta_column_group.cpp:245-285` `DeltaColumnGroupListHelper::garbage_collection`. The loop walks `dcg_list` front-to-back, maintaining a flat `std::unordered_set<uint32_t> column_set` (line 253). For each DCG whose `version() <= min_readable_version` (line 255), it frees the DCG iff *every* column UID of that DCG is already in `column_set` (lines 259-269); otherwise it inserts that DCG's UIDs into `column_set` and advances (lines 275-281). Freed DCG version → `garbage_dcgs`; freed files → `garbage_files` (lines 271-273).

This encodes the dense-only assumption: "a column UID seen in any newer DCG fully replaces that column in all older DCGs." For a dense `.cols` file that is true (it materializes all N source rows of the column). For a sparse `.spcols` it is false — a newer patch of `col_a` on rows {100,200} does not contain row {500,600} from an older `col_a` patch, yet the predicate would free the older one and silently destroy {500,600}.

### 1.2 List version-ordering is guaranteed newest-first at the GC site
The predicate is only correct if "already in `column_set`" means "seen in a *newer* DCG," i.e. the list must be newest-version-first. Confirmed end to end:
- RocksDB key encodes `INT64_MAX - version` (`be/src/storage/tablet_meta_manager.cpp:740` and `:765`), comment "sorted by version in reverse order in RocksDB."
- `scan_delta_column_group` does `iterate_range(lower=end_version, upper=begin_version)` + `push_back` (`tablet_meta_manager.cpp:1238-1252`), yielding newest-first.
- The cache loads from this scan (`update_manager.cpp:189`/`:423`), and `set_cached_delta_column_group` inserts new DCGs at `begin()` (`update_manager.cpp:415`) — keeping newest-first.
- Schema-change/linked paths also preserve newest-first via `insert(begin())` (`schema_change.cpp:1167` "reverse order by version"; `tablet_updates.cpp:4115-4116`).

So in production the newest-first invariant holds and the new per-column predicate can rely on it. (Caveat for tests: the existing UT `be/test/storage/delta_column_group_test.cpp:80-84` builds the list oldest-first via `push_back(i=1..20)` — it only "passes" because every DCG in those cases shares identical UIDs, so order is irrelevant. The new per-file-kind tests must build newest-first to match production; see §5.)

### 1.3 The single production trigger and who unlinks files
- `tablet_updates.cpp:2836` inside `TabletUpdates::_remove_expired_versions` (the function spanning ~2755-2854; min_readable_version computed at `:2801`) calls `update_manager->clear_delta_column_group_before_version(...)`.
- `UpdateManager::clear_delta_column_group_before_version` (`update_manager.cpp:291-329`) iterates the in-memory `_delta_column_group_cache` for the tablet (`:306-312`), calling `garbage_collection` per segment list, then: deletes the DCG meta keys from RocksDB via `TabletMetaManager::delete_delta_column_group` (`:316-322`), commits the write batch (`:323`), and **unlinks the physical files** via `fs->delete_file(filename)` (`:324-327`). So the helper only *selects*; the unlink happens here.
- This fires on min_readable_version advancement, independent of compaction materialization — exactly the danger called out in the task: sparse files can be selected for deletion before any dense materialization exists.

### 1.4 Other DCG-deletion sites are NOT the flawed coverage predicate
`grep` for `clear_delta_column_group` shows the rest are the bulk variant `TabletMetaManager::clear_delta_column_group(store, batch, tablet_id)` (`tablet_meta_manager.cpp:1451`), which deletes *all* DCG keys for a tablet during meta rebuild/drop/migration/snapshot: `tablet_manager.cpp:1838`, `schema_change.cpp:1182`, `snapshot_loader.cpp:705`, `engine_storage_migration_task.cpp:556`, `tablet_updates.cpp:4155/4367/4666/5081/5218`. None use the coverage set, so none can selectively destroy a sparse layer while keeping its supersessor. The schema-change linked path (`schema_change.cpp:1161` region, `tablet_updates.cpp:4110`) uses `merge_into_by_version` + `insert(begin())` (`delta_column_group.cpp:54-63`, `:65-87`) — version-bucketed merge, also not coverage-based. **The flawed predicate has exactly one caller: `update_manager.cpp:309`.**

### 1.5 Lake engine: out of scope for H4, confirmed
No `DeltaColumnGroupListHelper::garbage_collection` usage anywhere under `be/src/storage/lake/`. Lake DCG cleanup is object-store orphan reclamation against live metadata, not this coverage predicate. H4 is correctly scoped to the local engine.

## 2. Coverage predicate policy — three candidates

### (P-conservative) — RECOMMENDED for v1
Rule: a column UID is "covered" (eligible to free older layers of that column) **only by a newer DENSE file**. Sparse files never establish coverage and are never freed by the GC coverage path. Sparse files are removed only when a convergence action (sparse→sparse merge §4.8.1, sparse→dense promotion §4.8.2, or main compaction §4.8.3) rewrites them and its commit explicitly deletes the inputs (the same mechanism `merge_by_version` already uses: rename/replace at commit, the superseded keys deleted in the same write batch).

Correctness: fully correct. The only way GC frees a file is dense coverage, which is value-complete for that column over all N rows — identical to today's guarantee. Sparse files survive min_readable_version advancement until a materializing action supersedes them, which is exactly the read-path requirement (LayeredOverlayIterator needs every un-superseded sparse layer present).

File-accumulation cost: between convergence events, sparse files for a segment accumulate unboundedly *by GC alone*. But the design's convergence triggers bound it: sparse→sparse merge at `sdcg_sparse_compaction_max_files=8` (doc §4.8.1 / §6.3) collapses N files to 1; promotion at `sdcg_promotion_hard_count=16` or `sdcg_promotion_threshold` K/M=0.3 (§4.8.2 / §6.3) writes a dense `.cols` that *does* establish coverage and lets the next GC reclaim everything below it. So the steady-state file count per segment is bounded by max(compaction trigger, promotion trigger) ≈ ≤16, independent of GC. GC stops being the convergence mechanism for sparse data; the background workers are. This is the correct separation of concerns: GC reclaims only what is provably dead.

### (P-bitmap)
Rule: coverage by union of newer sparse presence bitmaps — free an older sparse layer of `col_a` only if the Roaring union of all newer layers' `col_a` presence bitmaps is a superset of the older layer's bitmap. Correct in principle, but presence bitmaps live in `.spcols` footers, so the GC (which runs holding `_delta_column_group_cache_lock`, with a hard 10ms budget — `update_manager.cpp:297-303`) would need to open files. That is unacceptable under the lock. Mitigation = persist cheap pre-filters in `DeltaColumnGroupPB`:
- Cheapest: per-file `row_count` (already proposed as `sparse_row_counts` tag 6) + per-file `min/max source_rowid`. Enables a necessary-condition prune (range disjoint ⇒ definitely not covered) but range-overlap is not sufficient for superset, so it can only ever *defer* freeing, never safely *confirm* it. Net value over P-conservative for GC: near zero.
- Full bitmap bytes in PB: makes the superset test exact and lock-safe, but inflates tablet meta by the serialized Roaring size per file (10s–100s of bytes to KBs for wide rowid sets) on the hot RocksDB meta path, and duplicates data already in the footer. The doc's own meta-budget line (§5.1 row 11: "+<100B/DCG") would be blown.
Verdict: not worth it for v1. The superset test that P-bitmap would perform is exactly what the sparse→sparse merge worker already performs when it unions bitmaps and rewrites — so P-bitmap duplicates convergence logic into the GC for marginal earlier reclamation.

### (P-hybrid)
P-conservative now; add P-bitmap later *only if* production shows convergence workers lag and sparse files pile up between merges in a way GC could safely trim. Schema is forward-compatible: `file_kinds` (tag 5) ships in v1; presence-bitmap pre-filter fields can be added as later optional tags without touching v1 readers.

**Recommendation: P-conservative for v1, schema laid out so P-bitmap is a pure additive follow-up (= P-hybrid).** Rationale: zero risk of data loss, zero legacy regression, file count already bounded by the two convergence triggers, and it keeps GC a pure "reclaim provably-dead" operation rather than a second convergence engine.

## 3. Exact code changes

### 3.1 Proto (additive, backward-compatible)
`gensrc/proto/olap_common.proto:60-65` — extend `DeltaColumnGroupPB` (tags 1-4 used; add tag 5):
```proto
enum DeltaColumnFileKind { DENSE_COLS = 0; SPARSE_PERCOL = 1; }   // 0 default = legacy
message DeltaColumnGroupPB {
    repeated DeltaColumnGroupColumnIdsPB column_ids = 1;
    repeated string column_files = 2;
    repeated bytes  encryption_metas = 3;
    optional int64  file_size = 4;
    repeated DeltaColumnFileKind file_kinds = 5;  // parallel to column_files; ABSENT ⇒ all DENSE
}
```
Lake `DeltaColumnGroupVerPB` (`gensrc/proto/lake_types.proto:95-103`) already uses tag 5 for `shared_files`; for lake use tag 6 if/when needed. Lake is out of H4 scope, so no lake proto change now.

`file_kinds` is `repeated`, default-empty. **ABSENT (size 0) is the legacy contract: treat every file as DENSE_COLS.** This is the zero-regression hinge.

### 3.2 `delta_column_group.h`
Add storage + accessor mirroring `_column_files`:
```cpp
// new member
std::vector<DeltaColumnFileKind> _file_kinds;  // parallel to _column_files; empty ⇒ all DENSE
// new accessors
const std::vector<DeltaColumnFileKind>& file_kinds() const { return _file_kinds; }
// per-file kind with legacy fallback:
DeltaColumnFileKind file_kind(size_t idx) const {
    return idx < _file_kinds.size() ? _file_kinds[idx] : DENSE_COLS;
}
bool is_file_dense(size_t idx) const { return file_kind(idx) == DENSE_COLS; }
```
Extend `init(...)` (`delta_column_group.h:39-41`) with an optional trailing `const std::vector<DeltaColumnFileKind>& file_kinds = {}` (keeps all existing callers compiling). The GC gets FileKind visibility from this member; it is populated from the PB at load time.

### 3.3 `delta_column_group.cpp` — load / save / serialize / merge
- `load(version, data, length)` (`:89-135`): after reading `column_files`, read `dcg_pb.file_kinds()` into `_file_kinds`. **Do not** synthesize entries when absent — leave `_file_kinds` empty so `file_kind(idx)` falls back to DENSE. (OldDeltaColumnGroupPB path at `:111-118` also stays DENSE by fallback.)
- `save()` (`:155-173`): emit `dcg_pb.add_file_kinds(...)` for each file **only if any is non-DENSE** (omit entirely when all dense, to preserve byte-identical meta for legacy/dense tablets and honor the <100B budget).
- `DeltaColumnGroupListSerializer::serialize_delta_column_group_list` (`:175-199`) and `_deserialize_delta_column_group_list` (`:222-243`): same add/read of `file_kinds`, same "omit if all dense."
- `merge_by_version` (`:65-87`): when appending another DCG's files, append its kinds too (with DENSE fallback if its `_file_kinds` is shorter), keeping `_file_kinds` parallel to `_column_files`. Important: the existing sparse→sparse merge worker (§4.8.1) is what produces a merged `.spcols`; its commit is what deletes the inputs, so the merged DCG must carry SPARSE kind and the input keys are removed in that worker's write batch, not by `garbage_collection`.

### 3.4 The core GC loop (pseudo-diff) — per-column coverage replacing the flat set
Replace `std::unordered_set<uint32_t> column_set` with a set of UIDs that are *densely* covered by a newer layer. The freeing decision becomes per-DCG, but each DCG is freeable only if all its columns are dense-covered AND (P-conservative) the DCG itself contributes no still-needed sparse layer.

```cpp
void DeltaColumnGroupListHelper::garbage_collection(...)
    // dense_covered[uid] == true  iff some NEWER layer wrote uid in a DENSE file.
    std::unordered_set<uint32_t> dense_covered;
    auto it = dcg_list.begin();
    while (it != dcg_list.end()) {
        const auto& dcg = *it;
        if (dcg->version() > min_readable_version) { ++it; continue; }

        const auto& all_cids = dcg->column_ids();   // vector<vector<uid>>, parallel to files
        bool need_free = true;
        for (size_t f = 0; f < all_cids.size(); ++f) {
            for (uint32_t uid : all_cids[f]) {
                if (dense_covered.count(uid) == 0) { need_free = false; break; }
            }
            if (!need_free) break;
        }

        if (need_free) {
            garbage_dcgs->emplace_back(tsid, dcg->version());
            auto files = dcg->column_files(tablet_path);
            garbage_files->insert(garbage_files->end(), files.begin(), files.end());
            it = dcg_list.erase(it);
        } else {
            // This DCG survives. Record dense coverage ONLY from its DENSE files.
            for (size_t f = 0; f < all_cids.size(); ++f) {
                if (dcg->is_file_dense(f)) {                 // P-conservative gate
                    for (uint32_t uid : all_cids[f]) dense_covered.insert(uid);
                }
                // SPARSE files contribute NO coverage -> older sparse of same uid is retained.
            }
            ++it;
        }
    }
```
Key differences from today:
1. `column_set` → `dense_covered`, and it is fed **only from DENSE files** (the `is_file_dense(f)` gate). A sparse layer of `col_a` no longer marks `col_a` covered, so an older `col_a` layer (dense or sparse) is not freed by a newer sparse one.
2. Coverage is accumulated **per file**, not per DCG — handles the mixed case where one DCG version holds both a dense `col_x` file and a sparse `col_y` file (§4 / §5d): `col_x` establishes coverage, `col_y` does not.
3. **Legacy/dense behavior is byte-identical**: when `_file_kinds` is empty, `is_file_dense(f)` returns true for every file ⇒ every survivor feeds `dense_covered` ⇒ the loop reduces exactly to today's `column_set` logic. Zero regression. (Note one subtle correctness improvement that is also a behavior preservation: a freed DCG must itself be fully covered before it can free; for an all-dense list this is unchanged.)

There is one nuance worth calling out for review: in P-conservative a *dense* file in an older DCG also is not freed by a newer *sparse* file of the same uid — which is correct, because the newer sparse layer overlays only some rows on top of the older dense base+layers, so the older dense values are still the source for the non-overlaid rows. Today's code would wrongly free that older dense file too once the uid appears in any newer DCG; the gate fixes that as a side benefit.

### 3.5 Behavior when `file_kinds` is ABSENT
Single rule, enforced by `file_kind(idx)`’s fallback: absent ⇒ DENSE ⇒ identical to current production. No migration, no meta rewrite, no flag needed for old tablets. New sparse writers populate `_file_kinds`; only then does the gate change anything.

## 4. Interaction analysis
- The min_readable_version trigger (`tablet_updates.cpp:2836`) stays unchanged. With the gate, sparse files survive it (no dense coverage exists for them) until a convergence action supersedes them. This is the intended behavior and removes the silent-data-loss bug.
- No other path frees DCGs by the flawed predicate (§1.4): the only caller of `garbage_collection` is `update_manager.cpp:309`. The bulk `clear_delta_column_group(tablet_id)` calls are unconditional full-tablet deletes during drop/rebuild/migration/snapshot and are correct as-is. The schema-change linked-rowset path (`schema_change.cpp:1161`, `tablet_updates.cpp:4110`) uses `merge_into_by_version`, not coverage.
- Convergence workers (sparse→sparse merge, promotion) must delete their input DCG keys/files in their own commit write batch (mirroring how `clear_delta_column_group_before_version` deletes meta + unlinks files at `update_manager.cpp:316-327`), since the GC will no longer reclaim sparse inputs. That is the design's intent (§4.8) and is a precondition for P-conservative's bounded file count — flag it as a dependency for the worker implementation, not part of H4 itself.

## 5. UT plan (extend `be/test/storage/delta_column_group_test.cpp`, suite `TestDeltaColumnGroup`)
All new cases MUST build lists **newest-first** (insert at front, or push descending versions) to match production ordering (§1.2) — the existing `testGC` ordering is misleading and should not be copied.

(a) **Legacy dense unchanged** `testGC_legacy_dense_unchanged`: build DCGs with `_file_kinds` empty (use existing `init(...)` without kinds). Reproduce the existing `testGC` test2 scenario (all `{1,2,3}`) and assert identical counts (after GC at min_readable=N: 1 survivor, N-1 freed). Proves the absent-kinds fallback reduces to today. Also round-trip `save()`/`load()` and assert the serialized bytes contain no `file_kinds` (omit-if-all-dense).

(b) **Sparse retained under newer sparse, same column** `testGC_sparse_not_freed_by_sparse`: newest→oldest versions v3,v2,v1, each a single SPARSE file for uid `{7}`. GC at min_readable >= v3. Assert `dcgs.size()==3`, `garbage_dcgs.size()==0`, `garbage_files.size()==0`. This is the exact data-loss case from the task; today’s code would free v2 and v1.

(c) **Sparse freed under newer dense** `testGC_sparse_freed_by_dense`: v3 = DENSE file for uid `{7}`; v2,v1 = SPARSE files for uid `{7}`. GC at min_readable >= v3. Assert v2,v1 freed (`dcgs.size()==1`, `garbage_dcgs.size()==2`), v3 retained. Proves dense coverage still reclaims older layers.

(d) **Mixed dense/sparse files inside ONE DCG version** `testGC_mixed_files_per_kind`: v2 holds two files — file0 DENSE for uid `{10}`, file1 SPARSE for uid `{20}`. v1 (older) holds SPARSE file for `{10}` and SPARSE file for `{20}`. GC at min_readable >= v2. Assert: the v1 `{10}` layer is freed (dense-covered by v2.file0) but the v1 `{20}` layer is retained (only sparse-covered) — i.e. v1 survives because not all its columns are dense-covered, and `dense_covered` after processing v2 contains `{10}` only. Verifies per-file (not per-DCG) coverage bookkeeping and the `is_file_dense(idx)` gate. Add an assertion on the surviving v1’s file list to confirm the `{20}` file is still present.

(e) (regression guard) **Dense-then-sparse not over-freed** `testGC_old_dense_kept_under_newer_sparse`: v2 = SPARSE for uid `{5}`, v1 = DENSE for uid `{5}`. GC at min_readable >= v2. Assert v1 retained (newer sparse does not cover older dense). Documents the side-benefit correctness fix and guards against regressing to the flat-set behavior.

Build/run: `./run-be-ut.sh --build-target delta_column_group_test --module delta_column_group_test --without-java-ext` (proto regen via `./build.sh --be` after editing `olap_common.proto`).


---

# H5 PLAN — density-aware lake (shared-data) DCG convergence

## 0. Ground truth from the current code (read this first)

**Lake DCG storage is fundamentally different from local.** Per segment (rssid) there is exactly **one** `DeltaColumnGroupVerPB` (`gensrc/proto/lake_types.proto:95-103`), and its repeated fields are **parallel arrays of per-version entries** stacked inside that one message:
- `unique_column_ids[i]` / `column_files[i]` / `versions[i]` / `encryption_metas[i]` / `shared_files[i]` together describe entry `i` (one `.cols` file written at `versions[i]`).
- `validate_dcg_shape` (`tablet_merger.cpp:262-281`) enforces `unique_column_ids_size() == column_files_size() == versions_size()`, optional arrays `<=` that, and **no duplicate column UID across entries**.

**The loader collapses a segment to ONE in-memory DCG.** `LakeDeltaColumnGroupLoader::load` (`column_mode_partial_update_handler.cpp:55-63`) does a single `push_back` of one `DeltaColumnGroup` built from the one `DeltaColumnGroupVerPB`. So in `get_lake_dcg_segment` (`update_manager.cpp:1266-1298`) the `for (dcg : ctx.dcgs)` loop has **one iteration**; the real per-column resolution is `DeltaColumnGroup::get_column_idx` (`delta_column_group.h:51-62`) scanning `_column_uids[idx]` and returning the **first entry** whose uid set contains the column. The "new ver to old ver" comment at `update_manager.cpp:1269` is about entry order **within** the single DCG, not multiple DCGs.

**Entry order within the array is newest-first.** `append_dcg` (`meta_file.cpp:113-163`) builds `new_dcg_ver` by appending the NEW files first (step 1, lines 122-135) then the surviving OLD entries (step 2, lines 139-160), and replaces the map value (line 162). So index 0..k-1 = newest, then older. First-hit in `get_column_idx` therefore yields the newest writer of a column — correct for today's dense semantics where any single `.cols` fully supersedes.

**`merge_dcg_meta` is the tablet-SPLIT/RESHARD merge path, not main compaction.** `merge_tablet` (`tablet_merger.cpp:683-754`) combines sibling tablet metadatas over a key range: it `clear_dcg_meta()` (line 708), preserves rowsets via `merge_rowsets`, and remaps rssids via `map_rssid`. **It does not rewrite or materialize segments**, so every `.cols`/`.spcols` file referenced by an input meta must survive verbatim into the output meta. Main-compaction materialization (design doc 4.8.3, line 649-651) is a *different* path that rebuilds base segments and GCs DCGs via `delta_column_group.h:146 garbage_collection`; it does not run through `merge_dcg_meta`.

---

## 1. Proto extension — `DeltaColumnGroupVerPB`, start at tag 6 (5 = shared_files is taken)

Add to `gensrc/proto/lake_types.proto:95-103`. Use **parallel arrays** (consistent with the existing 5 parallel fields and the local `DeltaColumnGroupPB` design at design-doc lines 340-355), not a per-file wrapper message (see "wrapper evaluated" below).

```proto
message DeltaColumnGroupVerPB {
    repeated DeltaColumnGroupColumnIdsPB unique_column_ids = 1;
    repeated string column_files = 2;
    repeated int64 versions = 3;
    repeated bytes encryption_metas = 4;
    repeated bool shared_files = 5;
    // === SDCG additions (tags 6+) ===
    repeated FileKind file_kinds = 6;                 // per entry; absent => DENSE_COLS (back-compat)
    repeated int64 sparse_row_counts = 7;             // per entry; K for SPARSE_PERCOL, 0/ignored for DENSE
    repeated ExtendedColumnRefPB extended_refs = 8;   // OPTIONAL, see note; per entry parallel array
    optional int64 source_segment_num_rows = 9;       // base segment row count for overlay alignment
}
enum FileKind { DENSE_COLS = 0; SPARSE_PERCOL = 1; }   // DENSE_COLS=0 so default/old metas read as dense
message ExtendedColumnRefPB { optional int32 column_uid = 1; optional string variant_path = 2; }
```

**Parallel-array invariant for each new array:**
- `file_kinds`: length is 0 **or** `== column_files_size()`. Length 0 means "all DENSE_COLS" (old BE wrote no kinds). Index `i` is the kind of `column_files(i)`.
- `sparse_row_counts`: length is 0 or `== column_files_size()`. Index `i` valid only when `file_kinds(i) == SPARSE_PERCOL`; for DENSE entries the slot is present-but-ignored (write 0).
- `extended_refs`: this is the one field where a flat parallel array is awkward, because a single sparse `.spcols` may carry **multiple** `(uid, variant_path)` refs (one entry's `unique_column_ids[i]` can list several uids). **Recommendation: do NOT add a flat `repeated ExtendedColumnRefPB` here.** Instead nest it inside the per-entry `DeltaColumnGroupColumnIdsPB` (add `repeated ExtendedColumnRefPB extended_refs` *inside* that message, which already groups per-entry uids), or defer it entirely to the LayeredOverlayIterator/variant-path work which owns variant semantics. Tag 8 stays reserved. For H5's metadata-convergence scope, `file_kinds` + `sparse_row_counts` + `source_segment_num_rows` are sufficient.
- `source_segment_num_rows`: scalar, not an array. It is the row count of the **base** segment this DCG overlays; needed so the overlay iterator can size the presence range. Per-rssid, so one scalar in the per-segment `DeltaColumnGroupVerPB` is correct.

**Wrapper-message alternative (evaluated, rejected for now):** a `repeated DcgEntryPB { string file; FileKind kind; int64 sparse_rows; DeltaColumnGroupColumnIdsPB ids; ... }` would be cleaner long-term but breaks wire/round-trip compat with every existing reader/writer of the 5 current parallel fields (`append_dcg`, `merge_dcg_meta`, the three validators, vacuum loops, `DeltaColumnGroup::load`). Migrating to a wrapper is a separate refactor; H5 should extend the existing parallel-array shape to keep the blast radius to the functions listed below.

**Validators that must learn the new arrays** (`tablet_merger.cpp`):
- `validate_dcg_shape` (262-281): add the "0-or-equal-length" checks for `file_kinds` and `sparse_row_counts`; **remove/relax the cross-entry duplicate-UID check** (272-279) — sparse chaining intentionally repeats a UID across entries (col_a at v2, v3, ...). Replace it with: "duplicate UID across DENSE entries is corruption; duplicate UID where at least the older is SPARSE is legal."
- `normalize_dcg_optional_fields` (283-290): pad `file_kinds` to `DENSE_COLS` and `sparse_row_counts` to `0` up to `column_files_size()` so downstream index access is uniform.
- `verify_dcg_entry_consistency` (292-318): when two metas reference the **same filename**, also assert `file_kinds(i)==file_kinds(j)` and `sparse_row_counts(i)==sparse_row_counts(j)`.

---

## 2. `append_dcg` rewrite (`meta_file.cpp:113-163`)

The signature must carry per-file kind + K. Today it takes `file_with_encryption_metas` and `unique_column_id_list`; add a parallel `const std::vector<FileKind>& file_kinds` and `const std::vector<int64_t>& sparse_row_counts` (and optionally `source_segment_num_rows`). The caller is `publish_column_mode_partial_update` -> `ColumnModePartialUpdateHandler::execute` (`update_manager.cpp:872-873`); the handler currently produces dense `.cols`, so until the writer emits sparse files all calls pass `DENSE_COLS` and behavior is byte-identical.

**Core semantic change to step 2 (the strip/orphan loop, lines 136-160):** today every NEW uid is unconditionally added to `need_to_remove_cuids_filter` and stripped from all older entries (141-143), orphaning a file when its uid set empties (151-159). New rule, decided **per updated column c by the kind of the NEW file covering c**:

- NEW file covering c is **SPARSE** => c is a *layer*, not a replacement. **Do NOT** put c into the strip filter; **do NOT** strip c from older entries; **do NOT** orphan. Older entries (dense or sparse) of c stay in the array and remain reachable. The new sparse entry is prepended (already-newest), so first-hit ordering naturally puts the newest layer first.
- NEW file covering c is **DENSE** => dense fully supersedes base + all older layers of c. Keep today's strip (put c in the filter, remove from older entries), AND this now also correctly strips older **SPARSE** entries of c. The orphan step must orphan an older entry's file when its uid set empties — unchanged, but now it can orphan a `.spcols` file too (correct: a superseding dense makes older sparse layers of c dead).

**Mixed new entry (some files dense, some sparse):** the filter must be built per-file, not in one pass. Split into a dense-uid filter and a sparse-uid set:

```
// step 1 (build new entries) — additionally:
for each new file i:
    new_dcg_ver.add_file_kinds(file_kinds[i]);
    new_dcg_ver.add_sparse_row_counts(file_kinds[i]==SPARSE_PERCOL ? sparse_row_counts[i] : 0);
    for uid in unique_column_id_list[i]:
        if file_kinds[i] == DENSE_COLS:  dense_remove_filter.insert(uid);   // supersede
        // SPARSE: do NOT add to any remove filter

// step 2 (carry forward older entries) — per OLD entry j:
//   strip ONLY uids in dense_remove_filter from old entry j's column_ids
auto* mcids = dcg_ver.mutable_unique_column_ids(j)->mutable_column_ids();
mcids->erase(remove_if(... dense_remove_filter.count(cuid) > 0 ...));
if (!mcids->empty()) {
    carry forward column_files(j), versions(j), encryption_metas(j), shared_files(j),
                 file_kinds(j), sparse_row_counts(j);   // <-- copy the NEW arrays too
} else {
    orphan column_files(j) (with shared_files(j) flag);   // dense superseded this old file entirely
}
// also: set source_segment_num_rows once if provided.
```

Subtlety to call out in the diff: a **dense** new write must still keep older entries that cover *other* (non-superseded) columns, exactly as today — the per-uid `erase_if` already handles that. The only new code is (a) tagging new entries with kind/K, (b) building the filter from dense uids only, (c) carrying the two new arrays forward for surviving old entries so the parallel-array invariant holds. The DCHECKs at 137-138 must extend to the new arrays (after normalization).

---

## 3. `merge_dcg_meta` (`tablet_merger.cpp:320-380`) — tablet-split merge, NOT compaction

Because `merge_tablet` does not materialize segments (see §0), **(a) "materialize so dcg_meta empties" is not an option here** — there is no rewrite in this path; that is the separate main-compaction path. So this is option **(b): preserve version-ordered chaining through the merge.** Concretely:

- **Step 1 exact-dedup (346-355):** unchanged; same filename from two children is the identical file (cross-published shared `.cols`/`.spcols`). Extend `verify_dcg_entry_consistency` to also compare kind + K (see §1).
- **Step 2 overlap check (357-367):** today returns `NotSupported` on *any* shared column UID. This is wrong for sparse chains. New rule:
  - If the existing entry and the incoming entry share a UID and **both are DENSE** for that UID => still `NotSupported` (two independent dense rewrites of the same column on two split children is a genuine conflict — same as today).
  - If **either side is SPARSE** for the shared UID => it is a legal layer; **append the incoming entry anyway** (fall through to step 3). The two layers must end up in the merged array **ordered by `versions(i)`** so the reader's first-hit (newest-first) order is preserved.
- **Step 3 append (369-374):** must also copy `file_kinds(i)` and `sparse_row_counts(i)`. Critically, after merging all entries for a target rssid, **sort the merged entry arrays by descending `versions`** (stable, all 5+2 parallel arrays together) so layered read order survives. Today step 3 appends in child-iteration order, which for disjoint dense columns doesn't matter but for sparse chains does. Add a `sort_dcg_entries_by_version_desc(DeltaColumnGroupVerPB*)` helper and call it per target after the per-context loop, then `normalize_dcg_optional_fields` + `validate_dcg_shape` once more.
- `source_segment_num_rows`: when merging two metas for the same target rssid, assert equal (same base segment) or take the present one.

This keeps every input file reachable (no orphaning in this path) and preserves the overlay order. The relaxed `validate_dcg_shape` from §1 (allowing repeated UID when sparse) is a prerequisite, otherwise the validator at 331 rejects the legal chain.

---

## 4. Read-side — FLAG ONLY (owned by LayeredOverlayIterator plan)

`get_lake_dcg_segment` (`update_manager.cpp:1266-1298`) and `get_column_idx` (`delta_column_group.h:51-62`) implement **first-hit = newest = full value**. This is the dense assumption. With sparse layers, the first hit only provides values for the rows in that layer's `source_rowid` set; the reader must walk **all** entries for the column oldest->newest and overlay by physical rowid, stopping the walk only when it reaches a DENSE entry for that column (dense supersedes everything older).

**Mark, do not implement here:** `get_lake_dcg_segment` returning a single segment per column is incompatible with layering — it must return the ordered list of (segment, kind, K) layers for the column so the LayeredOverlayIterator can stack them. This is explicitly the LayeredOverlayIterator plan's deliverable. H5's job is only to make the metadata *carry* the layers (file_kinds/sparse_row_counts/ordering) so the reader plan has correct, ordered input. Add a TODO at `update_manager.cpp:1270` noting the single-segment return is dense-only.

---

## 5. Orphan / vacuum — already chain-safe, two call sites to audit

All vacuum/GC enumerations iterate `dcg.column_files()` over **every** entry index and never infer deletability from a column being absent from a uid list:
- `vacuum.cpp:303-311` (shared-file delay-delete): loops `min(column_files_size(), shared_files_size())` — file-driven, kind-agnostic. Sparse files chained under older entries are still listed in `column_files`, so they are *protected* exactly like dense. OK.
- `vacuum.cpp:947-955` (delete by latest metadata): loops all `column_files_size()`. OK — only deletes files that are no longer referenced by the latest meta, which is correct: once `append_dcg`/compaction drops an entry, its file is gone from `column_files` and becomes collectable.
- `vacuum.cpp:1463-1468` (drop local cache): loops `column_files()`. OK.

**The one place that previously inferred deletability from "column no longer listed" is `append_dcg`'s own orphan step (`meta_file.cpp:151-159`)** — that is exactly what §2 fixes (only orphan on DENSE supersede, never on a sparse layer). No other site infers deletability from column membership. **Conclusion: as long as a chained sparse file stays in `column_files` (which §2/§3 guarantee), vacuum will not collect it.** Net new vacuum code required: none. Add one test asserting a chained `.spcols` is NOT in the delete set while its entry survives.

---

## 6. UT plan

Mirror H4 (local dense) cases, plus lake-specific:

**A. append_dcg unit (meta_file_test):**
1. dense-only (regression): write col_a dense v2, col_a dense v3 -> v2 entry orphaned, single entry remains. Byte-identical to pre-change.
2. sparse layer keeps old: col_a sparse v2, then col_a sparse v3 -> array has TWO entries for col_a, neither orphaned, v3 first.
3. dense supersedes sparse: col_a sparse v2, col_a sparse v3, then col_a **dense** v4 -> both sparse entries' uid sets empty -> both `.spcols` orphaned, one dense entry remains.
4. mixed new entry: new write with file1=DENSE(col_a) + file2=SPARSE(col_b) against older sparse col_a + sparse col_b -> col_a old entry stripped/orphaned, col_b old entry preserved.
5. disjoint columns: dense col_a then sparse col_b -> both kept, no orphan.

**B. proto round-trip old<->new BE:**
- Serialize a new `DeltaColumnGroupVerPB` with `file_kinds`/`sparse_row_counts`/`source_segment_num_rows`, parse with old proto (fields ignored) — confirm no corruption and old BE treats all as dense (FileKind default 0).
- Serialize from old BE (no new fields), parse with new BE -> `normalize_dcg_optional_fields` pads kinds to DENSE_COLS, K to 0; `get_column_idx` behavior unchanged.

**C. validator rejection (tablet_merger validators):**
- `validate_dcg_shape`: `file_kinds_size()` neither 0 nor `column_files_size()` -> Corruption.
- `sparse_row_counts_size()` mismatch -> Corruption.
- Duplicate UID across two **DENSE** entries -> Corruption; duplicate UID where older is SPARSE -> OK (the relaxed rule).
- `verify_dcg_entry_consistency`: same filename but differing kind or K -> Corruption.

**D. merge_dcg_meta (split merge):**
- Two children, disjoint dense columns -> appended, no error (regression).
- Two children, same column both DENSE -> `NotSupported` (regression).
- Two children, same column where one/both SPARSE -> entries merged, ordered by version desc, both files reachable.
- After merge, `validate_dcg_shape` passes on the relaxed rule.

**E. publish sequence (integration, mirrors task spec):** sparse(v2 col_a) -> sparse(v3 col_a) -> dense(v4 col_a):
- After v2: 1 entry (sparse, v2).
- After v3: 2 entries (sparse v3, sparse v2), no orphans.
- After v4 (dense): 1 entry (dense v4), both `.spcols` in `orphan_files`. Assert metadata "chains then collapses."

**F. vacuum reachability:** with the v3 state from E (two sparse entries), run the vacuum delete-set computation and assert neither `.spcols` is scheduled for deletion; after v4 collapse, assert both old `.spcols` ARE collectable.

---

## Files to touch (no code written in this plan)
- `gensrc/proto/lake_types.proto:95-103` — extend `DeltaColumnGroupVerPB` at tag 6+, add `FileKind`, `ExtendedColumnRefPB` (extended_refs deferred/nested per §1).
- `be/src/storage/lake/meta_file.cpp:113-163` — `append_dcg` per-file dense/sparse logic + carry new arrays.
- `be/src/storage/lake/meta_file.h` — `append_dcg` signature (add file_kinds / sparse_row_counts).
- `be/src/storage/lake/tablet_merger.cpp:262-318` — three validators (relax duplicate-UID, learn new arrays).
- `be/src/storage/lake/tablet_merger.cpp:320-380` — `merge_dcg_meta` allow sparse overlap + version-desc sort.
- `be/src/storage/delta_column_group.h/.cpp` (load at `.cpp:137-153`) — load new arrays into `_file_kinds`/`_sparse_row_counts` (per design-doc 360-373).
- `be/src/storage/lake/update_manager.cpp:1266-1298` — TODO marker only (read-side owned by LayeredOverlayIterator plan).
- `be/src/storage/lake/column_mode_partial_update_handler.cpp:55-63` — loader unchanged for H5 (single DCG per segment still holds; layering is read-side).

