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


---

## .upt 批内异构列(Flexible Ingestion)调研与设计原文(2026-06-04)

> 主文档 §5.6 的依据。三份报告:① `.upt` 全生命周期现状;② 导入前端现状;③ Option A vs B 设计裁决。

### 调研 ①:.upt 全生命周期(现状)

All paths verified in the current worktree. Lines cited are current.

# 1. WRITE: where .upt files are produced

**Dispatch to the update writer.** `DeltaWriter` builds a partial tablet schema and, for column mode, the factory picks the update writer:
- `be/src/storage/delta_writer.cpp:268-320` — when keys_type==PRIMARY_KEYS and `partial_cols_num < real_num_columns`, it builds `writer_context.referenced_column_ids` from the load slots (`:269-281`), sets `is_partial_update=true`, `partial_update_mode=_opt.partial_update_mode`, and `partial_update_schema = TabletSchema::create(_tablet_schema, referenced_column_ids)` (`:304,:315-320`). For COLUMN_UPSERT_MODE/COLUMN_UPDATE_MODE it forces `num_short_key_columns=1` and sort-key = key columns (`:307-313`).
- `be/src/storage/rowset/rowset_factory.cpp:76-89` — `create_rowset_writer` returns a `HorizontalUpdateRowsetWriter` iff `partial_update_mode` is COLUMN_UPSERT_MODE or COLUMN_UPDATE_MODE; otherwise `HorizontalRowsetWriter`. (`be/src/storage/rowset/horizontal_update_rowset_writer.h:28-29` "used for column mode partial update".)

**Which method writes update files (vs normal segments).** `HorizontalUpdateRowsetWriter` overrides the chunk-write methods to emit `.upt` files instead of `.dat` segments:
- `be/src/storage/rowset/horizontal_update_rowset_writer.cpp:44-58` `_create_update_file_writer()` opens a `SegmentWriter` on path `Rowset::segment_upt_file_path(...)` using `_context.tablet_schema` (the partial schema).
- `:86-116` `flush_chunk()` (header comment `:85` "flush and generate `.upt` file") — creates one update-file writer, appends the chunk, finalizes, and records `_num_uptfile++` plus encryption meta. This is the path the memtable sink calls.
- `:60-83` `add_chunk()` / `:119-134` `flush()` are the streaming variants (also roll a new `.upt` when the current one exceeds `config::max_segment_file_size`, `:63-74`).
- The sink wires one flush → one `.upt`: `be/src/storage/memtable_rowset_writer_sink.h:35-37` `flush_chunk` → `_rowset_writer->flush_chunk(chunk, seg_info)`.

**Filename pattern / extension — LOCAL:** `be/src/storage/rowset/rowset.cpp:186-188` `segment_upt_file_path` = `"$0/$1_$2.upt"` = `<dir>/<rowset_id>_<update_file_id>.upt`. So extension is `.upt`, indexed by `update_file_id` (0..N-1).

**Filename / location — LAKE: there is NO `.upt` extension.** `be/src/storage/lake/filenames.h` knows only `.dat/.del/.delvec/.cols` (whitelist `:219`; `gen_segment_filename` `:150-151` → `.dat`; `gen_cols_filename` `:258-259` → `.cols`). Lake's column-mode update payload is written as ordinary rowset **`.dat` segments** in `op_write.rowset().segments()`, and the per-rowset column metadata is put on the txn log: `be/src/storage/lake/delta_writer.cpp:842-858` copies `rowset_txn_meta`, adds `partial_update_column_ids` / `partial_update_column_unique_ids`, and `set_partial_update_mode`. At apply, the lake handler opens those segments as the "update files" via `_rowset_ptr->get_each_segment_iterator(...)` (`be/src/storage/lake/column_mode_partial_update_handler.cpp:101-102, :232-233, :244`). So in lake, the "upt_id" is just the segment index of the op_write rowset; the file is a `.dat`, not a `.upt`. (The SDCG plan §7.3/§12.2 calling for registering a `.spcols` whitelist entry in `filenames.h` is consistent with this: lake-side update/overlay files must be explicitly whitelisted or they get silently dropped by vacuum/migration.)

**Can ONE load produce MULTIPLE .upt files?** Yes — one per memtable flush. Each full memtable triggers `flush_chunk`, which creates a fresh update-file writer and bumps `_num_uptfile` (`horizontal_update_rowset_writer.cpp:96-110`). The streaming `add_chunk` path also rolls a new file at `max_segment_file_size` (`:63-74`). They are NOT a per-row-subset split — they are size/flush-driven and all share the identical partial schema.

**Where num_update_files is recorded.** `be/src/storage/rowset/rowset_writer.cpp:183-189` — at build, for PRIMARY_KEYS, `_rowset_meta_pb->set_num_update_files(_num_uptfile)`, plus `updatefile_encryption_metas`, `total_update_row_size`, `num_rows_upt`. Proto field: `gensrc/proto/olap_file.proto:168` `optional uint32 num_update_files = 57`. Accessors: `be/src/storage/rowset/rowset_meta.h:230 get_num_update_files()`, `:144 is_column_mode_partial_update() { return num_update_files() > 0; }`.

# 2. SCHEMA of a .upt file — PER-ROWSET, includes PK columns

**The .upt schema = partial tablet schema = PK columns + declared update columns.** The update-file `SegmentWriter` is constructed with `_context.tablet_schema` = the partial schema (`horizontal_update_rowset_writer.cpp:54-55`), and reopened with `_schema` (the rowset's partial schema) at read time (`rowset.cpp:955,:987`). The partial schema is `TabletSchema::create(_tablet_schema, referenced_column_ids)` where `referenced_column_ids` is built from the load slots and **includes the PK slots** (CDC payload is PK + changed cols), `delta_writer.cpp:269-281,:304`.

**Where the declared update-column set is recorded — PER-ROWSET (txn_meta), not per-file.** `be/src/storage/rowset/rowset_writer.cpp:194-204`: it asserts `referenced_column_ids.size() == tablet_schema->columns().size()` (i.e. the partial schema covers exactly the referenced columns), then for every column it does `_rowset_txn_meta_pb->add_partial_update_column_ids(referenced_column_ids[i])` and `add_partial_update_column_unique_ids(unique_id())`. These land in `RowsetTxnMetaPB`: `gensrc/proto/olap_file.proto:94-110` (`partial_update_column_ids=1`, `partial_update_column_unique_ids=2`, `partial_update_mode=6`, `auto_increment_partial_update_column_id=5`), embedded as `RowsetMetaPB.txn_meta=55` (`:164`). There is exactly ONE such list per rowset — it applies to ALL rows and ALL .upt files. Lake mirrors this on `op_write.txn_meta` (`lake/delta_writer.cpp:846-847`). This is the structural reason today's column-mode is "one fixed declared column set per load."

**Does the .upt contain the PK columns?** Yes. The PK columns are stored in the .upt and re-read at apply to do the PK-index point query: `be/src/storage/rowset_column_update_state.cpp:102-148` builds a PK-only schema (`:104-107`), opens each update file via `get_update_file_iterator(pkey_schema, idx, ...)` (`:126`), reads the rows, and PK-encodes them (`:142-143`). The read/update column split is derived from `num_key_columns`: keys are cid < num_key_columns, update cols are the rest — `rowset_column_update_state.cpp:705-732`, and `get_read_update_columns_ids` `:501-515`.

**Where the BE builds the partial tablet schema for the .upt writer.** Local: `delta_writer.cpp:304,:315-320` (build) consumed in `horizontal_update_rowset_writer.cpp:54`. Lake: the per-batch partial schema is rebuilt at apply from txn_meta uids (`lake/column_mode_partial_update_handler.cpp:349-350`).

# 3. READ at apply: how finalize consumes .upt + multi-file interplay

**Apply entry.** Local column-mode apply: `be/src/storage/tablet_updates.cpp:1133` `_apply_column_partial_update_commit` → `state.load(...)` (`:1162`) then `RowsetColumnUpdateState::finalize(...)`.

**The iterator.** `Rowset::get_update_file_iterator(schema, update_file_id, stats)` `be/src/storage/rowset/rowset.cpp:974-1000` opens `segment_upt_file_path(_rowset_path, rowset_id(), update_file_id)` as a Segment and returns a normal `ChunkIterator` (sequential read). `get_update_file_iterators` (`:940-972`) opens all of them. `read_chunk_from_update_file` (`rowset_column_update_state.cpp:407-421`) drains an iterator into a chunk.

**The core overlay: `_update_source_chunk_by_upt`** `rowset_column_update_state.cpp:450-485`. For each `(upt_id → pairs)` it (a) reads the whole .upt chunk for the selected partial columns (`:461,:467`), (b) `split_rowid_pairs` (`:475`, defined `:427-447`) splits the sorted `(source_rowid, upt_rowid)` pairs into a sorted-source-rowid vector and the corresponding (unsorted) upt-rowid vector, aligning source rowids into the current source-chunk window, (c) `append_selective` gathers the upt rows by upt_rowid (`:478-480`), and (d) `container.chunk_ptr->update_rows(tmp_chunk, sorted_source_rowids)` overwrites the source rows (`:482`). `Column::update_rows` is positional overwrite — `be/src/column/column.h:190-198`.

**Multi-.upt interplay — UptidToRowidPairs / rss_upt_id_to_rowid_pairs.**
- `upt_id` = index of an update file within the rowset (`0..num_update_files-1`), i.e. one memtable flush's .upt (local) or one op_write segment (lake).
- `UptidToRowidPairs = std::map<uint32_t, std::vector<RowidPairs>>` (`rowset_column_update_state.h:134`), keyed by upt_id; `RowidPairs = pair<source_rowid, upt_rowid>` (`:65`).
- Per-.upt state `ColumnPartialUpdateState` (`:68-106`): `src_rss_rowids[upt_rowid]` is the PK-index lookup result for each upt row; `rss_rowid_to_update_rowid` is `map<rssid, vector<(source_rowid, upt_rowid)>>`.
- `build_rss_rowid_to_update_rowid()` (`:80-105`) walks every upt row, decodes the 64-bit rss_rowid into `rssid = value>>32` and `rowid = value & ROWID_MASK` (`:88-89`), pushes `(source_rowid, upt_rowid)` into the rssid bucket, or records it in `insert_rowids` if PK not found (`UINT64_MAX`, `:91-93`), then sorts each bucket by source_rowid (`:96-100`).
- The pairs are built in `_prepare_partial_update_states` (`rowset_column_update_state.cpp:180-228`): `get_rss_rowids_by_pk(...)` resolves every upt PK against the live PK index (`:205-211`), then per-file `split_src_rss_rowids` + `build_rss_rowid_to_update_rowid` (`:214-220`).
- `finalize` then transposes per-file maps into `rss_upt_id_to_rowid_pairs : map<rssid, map<upt_id, vector<(source_rowid, upt_rowid)>>>` (`:746-755`), so for each target source segment (rssid) it can iterate all contributing update files in upt_id order and apply them onto the freshly-read source column chunk (`:771-825`, the `_update_source_chunk_by_upt` call at `:804`). Result is written out as a DCG `.cols` file (`:807,:814,:818-832`). Lake's analog is `column_mode_partial_update_handler.cpp:309-446`.

# 4. SAME-PK-TWICE in one batch — who wins, and WHERE (PER-ROW, not per-column)

Two layers of arbitration, both per-row:

**(a) Within one memtable / one .upt file: PK aggregation collapses duplicates to one whole row (last-write-wins).** `MemTable::finalize()` `be/src/storage/memtable.cpp:266-337`: for non-DUP keys it sorts + aggregates (`_merge()` `:278`, `_aggregator->aggregate_result()` `:282,:300`), so duplicate PKs in the same memtable are merged before the chunk is flushed to a .upt. The aggregator keeps the last row per key (`be/src/storage/chunk_aggregator.h:109` "the last row of non-key column is in aggregator ... before finalize"). So a single .upt never contains the same PK twice — and the surviving row is the whole latest row (all declared columns from the last occurrence). This is inherently per-row.

**(b) Across multiple .upt files (multiple memtable flushes) for the same existing PK: last-position-wins in update_rows.** Both flushes resolve the same existing PK to the same `(rssid, source_rowid)` via the PK index (`_prepare_partial_update_states:205-211`). So `rss_upt_id_to_rowid_pairs[rssid]` ends up with the same `source_rowid` appearing under multiple upt_ids. In `_update_source_chunk_by_upt` (`:456-483`) the files are applied in ascending `upt_id` order (std::map iteration), each calling `update_rows` onto the same source row — so the **last upt_id wins for the whole row** (positional overwrite, `column.h:198`). There is no per-column merge; whichever .upt is applied last replaces every declared column for that source row. Lake is identical (`column_mode_partial_update_handler.cpp:235-258`, same ascending upt_id loop + `update_rows`).

Note `insert_rowids` (brand-new PKs) follow the same per-row treatment in `_insert_new_rows` (`:604-661`) using `append_selective` + `_fill_default_columns`.

**Why this matters for the flexible design:** because today both arbitration points are whole-row last-write-wins, the system never needs to ask "which column came from which row." The SDCG batch-heterogeneous-columns case breaks this: if row A updates {c2} and row B (same PK) updates {c3}, "later wins" must be resolved PER COLUMN (keep c2 from A, c3 from B), which neither the memtable aggregator nor `update_rows` does today.

# 5. LIFECYCLE: do .upt files persist after apply? Who deletes them?

**They persist on disk after apply (local).** At apply, `_apply_rowset_commit` only calls `rowset->rowset_meta()->clear_txn_meta()` and resets the rowset to the full schema (`be/src/storage/tablet_updates.cpp:1760-1777`). It does NOT reset `num_update_files` and does NOT physically delete the `.upt`. So `is_column_mode_partial_update()` stays true and the .upt files remain referenced by the rowset (the apply produces the DCG `.cols` overlay + any new insert `.dat`; the .upt is left in place). The reserved rowsetid range accounts for this: `tablet_updates.cpp:754-757` reserves `max(num_update_files, num_segments)` "because we may transfer them to .dat files later."

**Deletion is tied to the whole rowset's removal.** `Rowset::remove()` `be/src/storage/rowset/rowset.cpp:388-393` deletes every `segment_upt_file_path(...)` for `i in 0..num_update_files()`. `remove()` is invoked when the rowset is GC'd / compacted away (e.g. `tablet_updates.cpp:4762`, `storage_engine.cpp:1284`, `tablet.cpp:1160-1165`). The same loop also appears in link/copy paths (`rowset.cpp:502-504` link_files, `:631-634` copy_files) and snapshot validation expects them present (`tablet_updates.cpp:4949-4957`), and snapshot/replication whitelist `.upt` (`be/src/runtime/snapshot_loader.cpp:1016`, `be/src/storage/replication_txn_manager.cpp:533`) — i.e. the .upt is a first-class, persistent part of the rowset until the rowset dies.

**Do they participate in compaction?** No, not as a read input. The normal read/compaction iterator path is `Rowset::new_iterator` → `get_segment_iterators` → `Segment::new_iterator` with the DCG loader applied (`rowset.cpp:745-851`); it reads `.dat` segments + DCG `.cols`, never `.upt`. The .upt is consumed only once, at apply, to build the DCG. Compaction touches .upt only transitively: when it rewrites/drops the source rowset, `Rowset::remove()` deletes the .upt.

**Lake.** The lake "update files" are ordinary op_write `.dat` segments, so their lifecycle is the normal lake rowset/segment lifecycle (metastore-tracked, vacuumed). This is why the SDCG plan stresses registering new overlay extensions in lake `filenames.h` so vacuum/migration don't silently drop them.

# 6. Existing notions of per-row column variability the flexible design can piggyback on

The path is overwhelmingly per-rowset-fixed, but a few per-row-ish special cases already exist:

- **Auto-increment column skip (per-key, value-conditional).** `rowset_column_update_state.cpp:695-732` (and the long comment `:695-704`): the auto-increment column is forced into the partial schema at write time so an id can be allocated, so the .upt physically contains an AI column; but at apply, for keys that already exist, the writer wrote "0" and finalize **discards** the AI column from the update set (`:707-710,:725-728`), keeping the historical value. Lake mirrors this (`lake/column_mode_partial_update_handler.cpp:301`, `lake/delta_writer.cpp:870-890`, txn field `auto_increment_partial_update_column_id`). This is the closest existing precedent for "this declared column is, in effect, not applied for some rows."

- **insert vs update bifurcation per row.** Each upt row is classified per-PK into update (`rss_rowid_to_update_rowid`) vs insert (`insert_rowids`) by PK-index presence (`rowset_column_update_state.h:86-93`), and COLUMN_UPDATE_MODE drops the inserts entirely while COLUMN_UPSERT_MODE materializes them (`rowset_column_update_state.cpp:836-842`). So the apply already routes different rows down different code paths based on per-row index lookup.

- **Default / expr fill per missing column.** `_fill_default_columns` (`:517-544`) fills non-updated columns from default/expr values (`column_to_expr_value` in txn_meta, proto `:109`) — a mechanism for "value present for some columns, synthesized for others," which a flexible per-row design could reuse for the not-covered columns of a row.

- **Condition / merge update.** `merge_condition` is stored in txn_meta (`olap_file.proto:101`; set in `rowset_writer.cpp:205-206`) but column mode + condition update is explicitly rejected in lake (`lake/delta_writer.cpp:830-834`), so it is not a per-column variability lever today.

There is **no** existing per-row column bitmap / skip-mask anywhere in the column-mode path: the only per-row state is the PK-index-derived `(rssid, source_rowid)` and the insert/update split. Every declared column applies to every updated row, and arbitration is whole-row. That is precisely the invariant the SDCG "批内异构列" design must replace with per-(rowid-equivalence-class) column subsets and per-column last-write-wins.

# Key files
- `be/src/storage/rowset/horizontal_update_rowset_writer.{h,cpp}` — the .upt writer
- `be/src/storage/rowset/rowset.cpp` (`:186-188` path, `:382-393` remove, `:940-1000` read iterators)
- `be/src/storage/rowset/rowset_writer.cpp:170-233` — num_update_files + txn_meta population
- `be/src/storage/rowset_column_update_state.{h,cpp}` — local apply/overlay, pair building, same-PK arbitration
- `be/src/storage/lake/column_mode_partial_update_handler.cpp` — lake apply/overlay (update files are .dat segments)
- `be/src/storage/lake/filenames.h` — confirms NO .upt extension in lake (only .dat/.del/.delvec/.cols)
- `be/src/storage/lake/delta_writer.cpp:842-890` — lake per-rowset txn_meta column ids + auto-increment
- `be/src/storage/memtable.cpp:266-337` + `be/src/storage/memtable_rowset_writer_sink.h:35-37` — per-flush .upt + intra-memtable PK dedup
- `be/src/storage/tablet_updates.cpp:754-757,:1133,:1760-1777,:4949-4957` — reserve ids, apply, clear_txn_meta, persistence
- `gensrc/proto/olap_file.proto:94-110,:164,:168` — RowsetTxnMetaPB / num_update_files
- `be/src/column/column.h:190-198` — update_rows positional-overwrite semantics

### 调研 ②:导入前端(现状)

All paths are relative to the worktree root `/home/disk4/hujie/src/starrocks/.claude/worktrees/claude+hopeful-sagan-uBVUh/`.

## 1. Stream Load JSON path — how columns are declared, and what happens when a row omits one

### Column declaration
A partial-update stream load declares its columns via the `columns` HTTP header (parsed into `ImportColumnDesc`s) and optionally `jsonpaths`. These become slot descriptors. The JSON reader maps each JSON key to a slot by name through `_slot_desc_dict` (`be/src/exec/file_scanner/json_scanner.cpp:304` builds the dict; `:530` looks a key up). With `jsonpaths` set, the i-th path is bound positionally to the i-th slot (`be/src/exec/file_scanner/json_scanner.cpp:604-668`, `_construct_row_with_jsonpath`).

### What happens TODAY when a JSON row omits a declared column
The reader tracks, per JSON object, which chunk columns were filled, in a scratch bitmap `_parsed_columns` (declared `be/src/exec/file_scanner/json_scanner.h:156`). It is reset at the start of every row (`be/src/exec/file_scanner/json_scanner.cpp:493`), set when a key is consumed (`:565`), and then used to backfill anything missing:

- `be/src/exec/file_scanner/json_scanner.cpp:583-600` (no-jsonpath path): for every column not present in this object, if it is the `__op` column it gets a default op value, otherwise `column->append_nulls(1)`.
- `be/src/exec/file_scanner/json_scanner.cpp:615-629` and `:650-663` (jsonpath path): same — missing jsonpath / not-found extraction yields `__op` default or `append_nulls(1)`.

So an omitted column becomes **null** (not a typed default, and not an error). Strict mode (`_strict_mode`, plumbed at `be/src/exec/file_scanner/json_scanner.cpp:840`, passed as `!_strict_mode` = "invalid-as-null") governs *type-cast failures* of present values, not omission: omission is always null-fill. The downstream NOT-NULL / default-value semantics are applied later in the sink (`column_to_expr_value`, see §3), not in the reader.

### Is each JSON object's actual key set captured per row?
**Yes, transiently — but it is thrown away.** `_parsed_columns` is the only per-row "which keys were present" signal, and it:
- is reset every row (`:493`),
- is consumed only inside `_construct_row_without_jsonpath` to decide null-vs-default fill (`:583-600`),
- is **never** read outside `json_scanner.cpp` (grep: the only references are `:493/:558/:565/:584` plus the declaration). It is not attached to the chunk, not exported to the sink, and the jsonpath path does not even maintain it.

Net: the front-end *knows* per-row which columns a JSON object carried, but the columnar chunk that leaves the scanner has a fixed schema with null-filled holes, and the per-row presence information is discarded. This is exactly the gap SDCG must bridge.

## 2. Where partial_update_mode is parsed/propagated, and where the FIXED update-column set is decided

### partial_update_mode parsing/propagation
- FE parse (stream load): `fe/fe-core/src/main/java/com/starrocks/load/streamload/StreamLoadKvParams.java:220-239` maps the `partial_update_mode` header string (`column`/`auto`/`row`) to `TPartialUpdateMode` (default ROW_MODE). `getPartialUpdate()` reads the `partial_update` flag (`:215-218`).
- FE plan: `fe/fe-core/src/main/java/com/starrocks/sql/LoadPlanner.java:271` stores it; `:509`/`:529` push it onto `OlapTableSink` via `setPartialUpdateMode`.
- Thrift: carried as `TOlapTableSink.partial_update_mode` (`gensrc/thrift/DataSinks.thrift:234`), enum `TPartialUpdateMode {UNKNOWN, ROW, COLUMN_UPSERT, AUTO, COLUMN_UPDATE}` (`gensrc/thrift/Types.thrift:568-574`).
- BE sink: `be/src/exec/tablet_sink.cpp:100` reads `table_sink.partial_update_mode`; translated to the BRPC `PartialUpdateMode` per tablet-writer request at `be/src/exec/tablet_sink_index_channel.cpp:183-190`.
- BE channel -> DeltaWriter: `be/src/runtime/local_tablets_channel.cpp:122` (`options.partial_update_mode = params.partial_update_mode()`); lake equivalent `be/src/runtime/lake_tablets_channel.cpp:903`.

### Where the FIXED update-column set is decided — **FE (sink descriptor)**
The authoritative decision is in FE. `LoadPlanner.plan()` computes the destination column list:
- `fe/fe-core/src/main/java/com/starrocks/sql/LoadPlanner.java:309-318`: when `partialUpdate`, `destColumns = Load.getPartialUpateColumns(...)`; otherwise the full schema.
- `Load.getPartialUpateColumns` (`fe/fe-core/src/main/java/com/starrocks/load/Load.java:660-701`) takes the load-level `ImportColumnDesc` list (the declared `columns`), intersects it with the table base schema, force-includes all key columns (errors if a key is missing, `:669-670`), auto-increment (`:671-675`), and generated columns (`:676-678`). This produces **one** `List<Column>` for the whole load.
- `LoadPlanner.java:324` `generateTupleDescriptor(destColumns, ...)` turns that list into the tuple/slot descriptors that populate `TOlapTableSchemaParam.slot_descs` (`gensrc/thrift/Descriptors.thrift:363-372`, embedded in `TOlapTableSink.schema` at `gensrc/thrift/DataSinks.thrift:219`).

BE then mirrors that fixed set rather than choosing it:
- `be/src/storage/delta_writer.cpp:269-281`: builds `writer_context.referenced_column_ids` by looking up each sink slot's name in the tablet schema. This is one ordered vector for the whole DeltaWriter.
- `be/src/storage/delta_writer.cpp:304` `TabletSchema::create(_tablet_schema, referenced_column_ids)` produces a single `partial_update_schema`; `:316-326` set it as the writer's schema and `writer_context.is_partial_update = true`. Every row written through this DeltaWriter uses this one schema.

So FE picks the column subset; BE consumes it. BE's only "mode choice" is dense/row/auto dispatch (`be/src/storage/delta_writer.cpp:307-318`), not which columns.

## 3. How Doris-style flexibility maps onto SR's columnar pipeline — is there ANY per-row column-validity vehicle?

SR's chunk/memtable model is strictly columnar with a single fixed schema:
- The memtable holds one `const Schema* _vectorized_schema` (`be/src/storage/memtable.h:141`) and a single `_chunk` (`:132`). There is no per-row column set; every row occupies every column of that schema.
- The scanner emits a fixed-width chunk (one column per slot, holes null-filled — §1).

Per-row vehicles that DO exist, and what they carry:
- `__op` column: per-row UPSERT/DELETE selector only. `Load.LOAD_OP_COLUMN = "__op"` (`fe/fe-core/src/main/java/com/starrocks/load/Load.java:118`); enum `TOpType {UPSERT, DELETE}` (`gensrc/thrift/Types.thrift:511-513`); consumed row-by-row in the sink at `be/src/exec/tablet_sink.cpp:814-831` to drive delete filtering. It is row-granular, NOT column-granular.
- `column_to_expr_value` (default/expr values for missing columns): a load-level map (not per-row), plumbed `writer_context.column_to_expr_value` (`be/src/storage/delta_writer.cpp:318`) and persisted into `RowsetTxnMetaPB.column_to_expr_value` (`be/src/storage/rowset/rowset_writer.cpp:224-227`). It is a single set of fill values for the whole load, the opposite of per-row heterogeneity.
- auto-increment null marker / `miss_auto_increment_column`: a load-level boolean (`be/src/storage/delta_writer.cpp:353`), not a per-row column-presence signal.

There is **no Doris-style per-row skip-bitmap** anywhere. A repo-wide grep for `skip_bitmap`/`SKIP_BITMAP`/`partial_update_value_columns`/per-row column masks turned up nothing in the load path (the only stray hits were in unrelated agg/delete code and contained no such fields on inspection). The `_parsed_columns` bitmap in the JSON reader (§1) is the closest thing that exists, and it is per-object scratch that never escapes the scanner.

### TOlapTableSink / sink thrift fields for partial update sent today
`gensrc/thrift/DataSinks.thrift:209-250` (`TOlapTableSink`): the only partial-update-relevant fields are `partial_update_mode` (`:234`), `merge_condition` (`:229`), `null_expr_in_auto_increment` (`:230`), `miss_auto_increment_column` (`:231`), `auto_increment_slot_id` (`:233`), and the fixed `schema` (`:219`, a `TOlapTableSchemaParam` whose `slot_descs` already encode the narrowed column set). There is **no** field expressing per-row or per-batch varying column subsets. The column subset is entirely implicit in `schema.slot_descs`.

On the BRPC tablet-writer side the mode is forwarded as a scalar (`be/src/exec/tablet_sink_index_channel.cpp:183-190`); no per-row column descriptor accompanies the chunk.

## 4. SQL UPDATE / INSERT partial path — statement-fixed (homogeneous)

- SQL UPDATE: the update-column set is the statement's SET list, fixed for all rows. `fe/fe-core/src/main/java/com/starrocks/sql/UpdatePlanner.java:116-134` builds the olap tuple by skipping every non-key, non-generated column that is `!updateStmt.isAssignmentColumn(...)` (`:117-122`). Mode is forced to `COLUMN_UPDATE_MODE` at `:146-149`. One assignment set => one slot set => homogeneous batch.
- INSERT partial: `fe/fe-core/src/main/java/com/starrocks/sql/InsertPlanner.java:455-463` sets the mode (`AUTO_MODE`, or `COLUMN_UPSERT_MODE` via `checkIfUseColumnUpsertMode`). The column set is the INSERT target column list (then narrowed by the same `Load.getPartialUpateColumns` machinery for loads / by the insert target list for SQL), again statement-fixed. `InsertPlanner.java:456` is the FE AUTO_MODE default the design doc flags as never matching in BE.

Both are trivially homogeneous: every row in the statement updates the identical column set.

## 5. Condition update (merge_condition) and auto-increment constraints a flexible mode must respect

### merge_condition
- It is a **single column name**, not a per-row construct. Carried as `TOlapTableSink.merge_condition` (`gensrc/thrift/DataSinks.thrift:229`) and persisted as scalar `RowsetTxnMetaPB.merge_condition` (`gensrc/proto/olap_file.proto:101`; written at `be/src/storage/rowset/rowset_writer.cpp:205-207`).
- Apply resolves it to one `conditional_column` index by name (`be/src/storage/tablet_updates.cpp:1449-1456`). Constraint for a flexible mode: the merge-condition column's value must be present for every row being conditionally applied; if different rows omit the condition column, the per-row condition evaluation has no defined operand. A flexible mode must guarantee the condition column is in every row's effective subset (or define omission semantics).

### auto-increment partial update
- The auto-increment column is *force-included* into the partial schema in FE (`Load.getPartialUpateColumns` `fe/fe-core/src/main/java/com/starrocks/load/Load.java:671-675`) and BE allocates ids during write; for already-existing keys a sentinel `0` is written into `.upt` and **discarded at apply** (documented at `be/src/storage/rowset_column_update_state.cpp:680-700`, skip logic at `:705-714` and `:725-726`).
- It is recorded as a single offset/uid: `RowsetTxnMetaPB.auto_increment_partial_update_column_id` (`gensrc/proto/olap_file.proto:103`) / `_uid` (`:107`), set once at `be/src/storage/rowset/rowset_writer.cpp:208-221`.
- Hard constraint: auto-increment column in the sort key is rejected for partial update (`be/src/storage/delta_writer.cpp:339-341`, `Status::NotSupported`). A flexible mode must keep auto-increment in every row's schema (it cannot be a per-row-optional column) and preserve the "skip sentinel on existing keys" discard.

Also relevant: sort-key / short-key adjustment for column mode (`be/src/storage/delta_writer.cpp:307-313`) and the `_partial_schema_with_sort_key_conflict` check (`:297-300`, `is_partial_update_with_sort_key_conflict`) assume one column subset; per-row subsets would need this evaluated per equivalence class.

## 6. Minimal change surface to let one stream-load batch carry per-row column subsets

Each hop currently encodes ONE column set; per-row heterogeneity requires teaching each of them a per-row (or per-equivalence-class) presence signal. Minimal component list with the load-bearing artifacts:

1. **JSON reader (and other scanners)** — `be/src/exec/file_scanner/json_scanner.cpp`. Today `_parsed_columns` (`:493-600`) is computed then discarded. It (or an equivalent presence signal) must be *emitted* per row instead of null-filling silently. Other format scanners (CSV/Parquet/Avro) have no notion of "absent" at all and would need an explicit absence convention.
2. **Chunk / column model** — a per-row column-validity vehicle must be added (e.g. a hidden presence/skip-bitmap column carried alongside data, analogous to how `__op` rides as an extra column at `be/src/exec/tablet_sink.cpp:814-816`). The chunk stays the UNION of columns; the new column distinguishes "value supplied" from "value absent" per row, separate from SQL NULL.
3. **OlapTableSink + sink thrift** — `gensrc/thrift/DataSinks.thrift:209-250` (`TOlapTableSink`) and the slot machinery would need either a presence-column slot id or a new field; the BRPC tablet-writer request (`be/src/exec/tablet_sink_index_channel.cpp:183-190`, backed by `gensrc/proto/internal_service.proto` `PTabletWriterAddChunk*`) would need to carry the presence column / flag.
4. **DeltaWriter** — `be/src/storage/delta_writer.cpp:269-326`. `referenced_column_ids` and the single `partial_update_schema` (`:304`) assume one subset; it must instead retain the union schema plus the per-row presence, deferring column-subset determination to apply.
5. **Memtable / .upt writer** — `be/src/storage/memtable.h:132/:141` (single `_chunk`/`_vectorized_schema`) and `be/src/storage/rowset/horizontal_update_rowset_writer.cpp:86` (`flush_chunk`). The `.upt` must persist the presence signal so apply can reconstruct per-row subsets.
6. **Rowset txn meta proto** — `gensrc/proto/olap_file.proto:94-108` (`RowsetTxnMetaPB`). Today `partial_update_column_ids` / `partial_update_column_unique_ids` (`:95-96`) are one flat list written at `be/src/storage/rowset/rowset_writer.cpp:200-204`. Per-row heterogeneity needs either a presence encoding in the `.upt` (preferred) or new repeated/structured fields here.
7. **Apply / DCG builder** — local `be/src/storage/rowset_column_update_state.cpp:701-779` and lake `be/src/storage/lake/column_mode_partial_update_handler.cpp:286-349`. Both read the single uid list and build one column set; they must instead group rows into rowid-equivalence classes (the design's Appendix B.1) and emit one DCG/.spcols entry per class. The lake handler already loops per-(column_batch, rssid) (`:343-416`, building `partial_tschema`/`dcg_column_ids` per batch), which is the cleanest insertion point — but today every batch still derives from the same single column set.

Thrift/proto messages on the path that would need new fields: `TOlapTableSink` (`gensrc/thrift/DataSinks.thrift:209`), the BRPC `PTabletWriterAddChunk*` (internal_service.proto), and `RowsetTxnMetaPB` (`gensrc/proto/olap_file.proto:94`) — plus the storage-side `DeltaColumnGroupPB` extensions the SDCG doc already plans. Everything from the scanner's discarded `_parsed_columns` (`be/src/exec/file_scanner/json_scanner.cpp:493`) down to the single `partial_update_column_ids` in txn meta (`be/src/storage/rowset/rowset_writer.cpp:200-204`) is the chain that collapses per-row presence into one fixed subset today.

### 设计裁决:Option A(union schema + per-row presence)

# SDCG ingestion: how `.upt` carries per-row heterogeneous column sets (批内异构列)

Both trace reports are accurate against the current worktree. I re-verified every load-bearing claim used below (citations are current line numbers). I also confirmed there is **no** existing per-row presence/skip-bitmap anywhere in the load path: a repo-wide grep for `skip_bitmap|partial_update_value_columns|presence_bitmap|spcols|SPARSE_PERCOL` finds only the unrelated column-with-row full-row read path (`be/src/storage/rowset_update_state.h:33`) and zero SDCG code yet — the `be/src/storage/partial_update/` module exists but holds only `partial_update_helper.{h,cpp}` (skeleton).

---

## 0. The core invariant being replaced (verified)

Today every hop collapses to **one fixed column subset per load**, enforced at three places:
- FE picks one `List<Column>` (`fe/.../Load.java:660-701`, per report 2 §2).
- BE builds one partial schema `TabletSchema::create(_tablet_schema, referenced_column_ids)` (`be/src/storage/delta_writer.cpp:304`, `:315-320`) used by every row.
- txn_meta stores ONE flat column list for the whole rowset: `be/src/storage/rowset/rowset_writer.cpp:200-204` loops `_context.tablet_schema->columns()` and `add_partial_update_column_ids` / `add_partial_update_column_unique_ids` — landing in `RowsetTxnMetaPB.partial_update_column_ids` / `partial_update_column_unique_ids` (`gensrc/proto/olap_file.proto:95-96`). It even asserts `referenced_column_ids.size() == tablet_schema->columns().size()` (`rowset_writer.cpp:195`).

Arbitration is whole-row last-write-wins at both layers (memtable PK-aggregation `be/src/storage/memtable.cpp:266-337`; positional `update_rows` in finalize, `be/src/column/column.h:190-198` — confirmed "update data from src according to indexes", whole-column positional overwrite). Neither layer can do per-column merge. That is exactly the invariant heterogeneity breaks.

---

## 1. Option A vs Option B against traced reality — recommend A

### What each option must add, mapped to the actual code

**Option A — union schema + per-row presence descriptor**
- `.upt` schema = PK + **union** of all columns appearing in the batch (superset of every row's set). Already buildable: `delta_writer.cpp:269-281` just builds `referenced_column_ids` from the sink slots — FE would send the union slot set (§4).
- A **transient per-row presence signal**: a small dictionary of distinct column-sets (set-id → bitmask over union columns) in txn_meta + a per-row set-id, OR a raw per-row bitmap. The set-id form is strongly preferred (CDC batches have very few distinct shapes; the dictionary is tiny and the per-row column degenerates to a uint16 set-id).
- Unset cells hold null placeholders in `.upt` and are **never read**: finalize derives per-column upt_rowid lists from the descriptor and only `fetch_values_by_rowid` real cells.

**Option B — split by column-set signature**
- memtable/flush partitions rows by exact column set; each partition → its own `.upt` with that exact (today-format) schema. Format unchanged.
- Requires **per-upt-file** column lists. Today the list is **per-rowset** (`rowset_writer.cpp:200-204`, one list; `RowsetTxnMetaPB` has no per-file repeated structure — `olap_file.proto:94-110`). So B needs a new repeated/nested message keyed by upt_id.

### Why A fits better — four structural reasons

1. **Multiple .upt files are already first-class, but they are NOT the heterogeneity axis today.** `UptidToRowidPairs = map<upt_id, vector<RowidPairs>>` (`be/src/storage/rowset_column_update_state.h:134`) and `rss_upt_id_to_rowid_pairs : map<rssid, map<upt_id, pairs>>` (built `rowset_column_update_state.cpp:746-755`) exist to handle **multiple flushes of the same homogeneous schema**, applied in ascending upt_id order (`_update_source_chunk_by_upt` loops the inner map, `:456`; same in lake `column_mode_partial_update_handler.cpp:235`). Option B would overload upt_id to ALSO mean "column-set partition," conflating two orthogonal axes (flush-order vs column-shape) onto one integer — and then the cross-file winner rule must reason about both at once. Option A leaves upt_id meaning exactly what it means today (flush index) and adds presence as an independent per-row attribute.

2. **txn_meta granularity is per-rowset; A keeps it that way, B fights it.** A adds ONE dictionary of distinct sets to the per-rowset `RowsetTxnMetaPB`; the union column list stays the single existing `partial_update_column_ids`. B needs a per-upt-file list — a new nested repeated structure indexed by upt_id, plus the writer must stop asserting `referenced_column_ids.size() == columns().size()` (`rowset_writer.cpp:195`) since each file now has a different shape. A respects the existing assertion (union schema == referenced columns).

3. **Memtable folding precedent favors A.** Today folding is whole-row last-write-wins via the PK aggregator (`memtable.cpp:278-300`, aggregator keeps last row per key). For A, folding becomes **column-aware**: same PK in one memtable ⇒ union the two presence masks, take latest value per present column. This is a localized change to the aggregate step (the aggregator already iterates per-column; it gains a "skip column if neither row's mask covers it, else take latest covering row" rule). For B, two rows with the same PK but different column sets must NOT be folded in one memtable at all (they go to different partitions/files), so B must either (a) route same-PK-different-set rows to different .upt and defer ALL same-PK merge to finalize, losing the intra-memtable dedup optimization, or (b) re-implement folding across partitions. A keeps folding where it already is.

4. **`_resolve_conflict` only remaps the source side — A is immune, B is not.** `_resolve_conflict` (`rowset_column_update_state.cpp:230-256`) re-runs `index.get(...)` to rebuild `src_rss_rowids` (PK → 64-bit rss_rowid) and rebuilds `rss_rowid_to_update_rowid` (`:237`, `:244`). It never touches *which columns* an upt row carries. Under A, presence is a per-(upt_id,upt_rowid) attribute that is invariant under conflict re-resolution — re-resolving just moves the (source_rowid) target, the column mask is untouched. Under B, the column-set is baked into *which file* a row lives in, which is also conflict-invariant, but B's cross-file global per-column winner must be recomputed after every resolve because the relative apply order across files can change which file wins a column — far more fragile.

**Recommendation: Option A.** Option B is the fallback, attractive only if changing the `.upt`/SegmentWriter format is deemed unacceptable (B requires zero format change) — but the SDCG storage side (`.spcols`) already changes formats, so that argument is weak.

---

## 2. The per-column winner rule: where it is enforced

With heterogeneity, last-write-wins must be **per (PK, column)**. Two enforcement points, mirroring today's two arbitration layers:

### (a) Intra-memtable fold — column-aware aggregation
Today `MemTable::finalize()` (`memtable.cpp:266-337`) collapses duplicate PKs to one whole row via `_aggregator` (keeps last row per key). New rule: when two rows share a PK, the surviving row's value for column `c` = the value from the **latest** input row whose presence mask covers `c`; the surviving presence mask = the **union** of the two masks. Implementation: the aggregator step gains a per-column "latest-present" selection instead of "latest-row" selection. This is the only place intra-batch same-PK heterogeneity is resolved; after it, a single `.upt` still never contains the same PK twice (preserving the report-1 §4(a) invariant), but the surviving row now carries a possibly-wider union mask.

### (b) Cross-.upt-file finalize — per-column pair lists
Today finalize builds `rss_upt_id_to_rowid_pairs[rssid][upt_id] = vector<(source_rowid, upt_rowid)>` (`rowset_column_update_state.cpp:746-755`) and applies files in ascending upt_id, whole-column, via `_update_source_chunk_by_upt` → `update_rows` (`:456-483`). The fix: make the pair map **per-column**. Concretely, change the build loop so a `(source_rowid, upt_rowid)` pair for upt_id is only added to the work list **for columns its presence mask covers**:

```
rss_upt_id_to_rowid_pairs[rssid][col_uid][upt_id] += (source_rowid, upt_rowid)   // only if mask(upt_id,upt_rowid) covers col_uid
```

Then the existing finalize structure (outer loop over column batches `:771`, inner over rssid `:772`, apply per upt_id `:804`) already iterates columns and upt_ids in the right order — applying ascending upt_id within a column gives per-column last-write-wins **for free**, because `update_rows` is positional and the later upt_id overwrites the earlier one *only for the rows that actually carry that column*. A row that updated `{c2}` in upt_0 and a same-PK row that updated `{c3}` in upt_1 land in `[rssid][c2][upt_0]` and `[rssid][c3][upt_1]` respectively — c2 keeps upt_0's value, c3 takes upt_1's, no clobber. This is the central elegance of Option A: **per-column LWW is just the existing ascending-upt_id positional overwrite, restricted per column by the presence mask.**

### Interaction with `_resolve_conflict`
`_resolve_conflict` (`rowset_column_update_state.cpp:230-256`) is invoked before finalize when another rowset applied in between. It rebuilds `src_rss_rowids` and `rss_rowid_to_update_rowid` from a fresh PK-index read. Because presence is keyed by (upt_id, upt_rowid) and is NOT derived from the index, the column mask is stable across re-resolution; only the `source_rowid` target moves. The per-column pair-list build (above) runs *after* resolve (same as today's `rss_upt_id_to_rowid_pairs` build at `:746`, which is in `finalize`), so it naturally consumes the re-resolved pairs. No new conflict logic needed.

---

## 3. How finalize derives per-column lists → rowid-equivalence classes → `.spcols`

### Derivation chain (Option A)
1. `_prepare_partial_update_states` (`rowset_column_update_state.cpp:180-228`) resolves every upt PK to a 64-bit rss_rowid via `get_rss_rowids_by_pk` (`:205-211`) and builds `rss_rowid_to_update_rowid` per file (`:214-220`). **Unchanged.**
2. Load the per-row presence (set-id column read from the `.upt`, decoded to a bitmask via the txn_meta dictionary). For each (upt_id, upt_rowid), we know `(rssid, source_rowid)` and `mask`.
3. Build the per-column map: `rss_to_col_to_upt_pairs[rssid][col_uid][upt_id] = vector<(source_rowid, upt_rowid)>`, pushing a pair only when `mask` covers `col_uid`.
4. **rowid-equivalence class** for a target segment (rssid) = a maximal group of columns whose set of contributing `source_rowid`s is identical (after merging across upt_ids). Per the SDCG doc §4.1, one equivalence class = one `.spcols` file (file-internal rows = K, all columns share the same source_rowid set). Compute by hashing, per column, its sorted distinct source_rowid set; columns with the same hash join one class.
5. BE density decision per (rssid, class) — `K/M < threshold && K < cap ⇒ sparse` (SDCG §5.2). Sparse ⇒ `SparseColsWriter` writes `.spcols` (source_rowid column + value columns, K rows, zero source-segment read, SDCG §5.3). Dense ⇒ existing `read_from_source_segment_and_update` (`rowset_column_update_state.cpp:319-387`) reads the full source column and writes `.cols`. **Note for the dense fallback:** `read_from_source_segment_and_update` reads source rows with the *partial* `schema` (`:345`), then `_update_source_chunk_by_upt` overlays — it needs no change beyond the per-column pair lists feeding it (point 5 below).

### Concrete 4-row example
Batch (all NEW columns relative to a 100-col table; PK column `pk`):
- row r0: PK=A, updates {c2} = v2A   (set S1 = {c2})
- row r1: PK=B, updates {c2,c3} = (v2B,v3B)  (set S2 = {c2,c3})
- row r2: PK=A, updates {c3} = v3A   (set S3 = {c3})  ← same PK as r0, different column
- row r3: PK=C, updates {c2} = v2C   (set S1 = {c2})

Distinct sets dictionary: S1={c2}, S2={c2,c3}, S3={c3}. Union schema = PK + {c2,c3}.

Assume one memtable flush ⇒ one `.upt` (upt_id 0). Intra-memtable fold (point 2a): r0 and r2 share PK=A but disjoint columns ⇒ folded into **one** surviving row with mask = S1∪S3 = {c2,c3}, values c2=v2A, c3=v3A. After fold the `.upt` (upt_id 0) holds 3 rows:
- u0: PK=A, mask{c2,c3}, c2=v2A, c3=v3A
- u1: PK=B, mask{c2,c3}, c2=v2B, c3=v3B
- u2: PK=C, mask{c2},    c2=v2C, c3=null(placeholder, never read)

PK index resolves A→(rss0,r=100), B→(rss0,r=305), C→(rss0,r=9527) (all in source segment rss0).

Per-column pair map for rss0:
- c2: upt0 → [(100,u0),(305,u1),(9527,u2)]   (u0,u1,u2 all cover c2)
- c3: upt0 → [(100,u0),(305,u1)]              (u2's mask omits c3 → NOT added)

Equivalence classes for rss0: c2's source_rowid set = {100,305,9527}; c3's = {100,305}. **Different ⇒ two classes**:
- class X = {c2}, rows {100,305,9527} (K=3)
- class Y = {c3}, rows {100,305} (K=2)

⇒ two `.spcols` files under the same DCG version (file 0: source_rowid∈{100,305,9527}, col c2; file 1: source_rowid∈{100,305}, col c3). c3 is correctly NOT written for rowid 9527 (PK=C never touched c3) — the placeholder null in u2 was never read because the c3 pair list excluded u2. This is the "批内异构列" result: PK A and B got both columns, PK C got only c2, in one batch, one `.upt`.

If instead all 4 rows had set {c2,c3} (homogeneous), both columns share source_rowid set {100,305,9527} ⇒ ONE class ⇒ one file with c2,c3 — i.e. today's single-DCG-entry behavior (point 6).

---

## 4. Exact metadata / thrift / proto additions (minimal, lake/local symmetric)

Tag discipline per `gensrc/CLAUDE.md`: proto2 — new fields optional/repeated, never required, never reuse ordinals; thrift — optional, next free ordinal.

### A. Ingestion front-end (carry per-row presence to BE)
- **JSON reader** `be/src/exec/file_scanner/json_scanner.cpp`: today `_parsed_columns` (`:493`, set `:565`, consumed `:583-600`/`:615-663`) is computed then discarded. Emit it: write a per-row set-id (or bitmask) into a new hidden presence column instead of silently null-filling. Crucially this is **only** done when the load is a heterogeneous partial update (a new flag), so the homogeneous path is untouched.
- **Sink thrift** `gensrc/thrift/DataSinks.thrift` `TOlapTableSink` (currently to `:234` partial_update_mode): add `optional bool flexible_partial_update` (next free ordinal) and reuse a presence slot. The presence column rides as a hidden extra column exactly like `__op` does today (`be/src/exec/tablet_sink.cpp:814-816` reads `__op` as `chunk->num_columns()-1`). This is the proven vehicle; presence becomes a second such hidden column.
- **BRPC** `gensrc/proto/internal_service.proto` `PTabletWriterAddChunk*`: the presence column flows as an ordinary chunk column (no schema change needed beyond the slot existing), so the only addition is a bool/flag indicating the presence-column slot id. (Report 2 §6 item 3 identifies this hop.)

### B. DeltaWriter / memtable / .upt writer
- `be/src/storage/delta_writer.cpp:269-326`: when flexible, set `referenced_column_ids` to the **union** (already what the slot list gives if FE sends the union) and keep the single union `partial_update_schema` (`:304`). Add carrying the presence column into the writer context.
- `be/src/storage/memtable.cpp`: column-aware fold (point 2a).
- `be/src/storage/rowset/horizontal_update_rowset_writer.cpp:86` (`flush_chunk`): the `.upt` SegmentWriter now also writes the presence column (set-id). No format change — it is just another column in the partial schema.

### C. Rowset txn meta (the per-rowset dictionary)
`gensrc/proto/olap_file.proto` `RowsetTxnMetaPB` (currently to tag 8 `column_to_expr_value`, `:109`): add
```
repeated PartialUpdateColumnSetPB distinct_column_sets = 9;  // dictionary: set-id = index; each lists unique-ids it covers
optional bool flexible_partial_update = 10;                  // false ⇒ today's single-set behavior (zero-cost hinge)
```
with `message PartialUpdateColumnSetPB { repeated uint32 column_unique_ids = 1; }`. The existing `partial_update_column_ids` / `partial_update_column_unique_ids` (`:95-96`) keep meaning "the union" — so legacy readers see a normal (wider) homogeneous partial update and degrade safely. Written in `be/src/storage/rowset/rowset_writer.cpp:200-204` (extend to also populate the dictionary when flexible).

### D. Storage-side DCG (already specified by the SDCG doc — symmetric)
- Local `DeltaColumnGroupPB` (`gensrc/proto/olap_common.proto:60-64`, tags 1-4 ⇒ new from 5): `file_kinds`, `sparse_row_counts`, `presences`, `source_segment_num_rows` per SDCG §4.3. Verified next-free tag = 5.
- Lake `DeltaColumnGroupVerPB` (`gensrc/proto/lake_types.proto:95-102`, tag 5 = `shared_files` ⇒ new from 6) — verified next-free tag = 6, matching the doc.

### E. Apply / DCG builder (per-column grouping)
- Local `be/src/storage/rowset_column_update_state.cpp:746-825`: change the pair map to per-column and group into equivalence classes (points 2b, 3).
- Lake `be/src/storage/lake/column_mode_partial_update_handler.cpp:309-349`: identical change; its loop already iterates per (column_batch, rssid) building `partial_tschema` per batch (`:343-349`) — the cleanest insertion point, exactly as report 1 notes. Lake's "upt files" are op_write `.dat` segments (`get_each_segment_iterator`, `:232`) so upt_id = segment index; presence rides as a column in those segments identically.

**Net change list (minimal):** JSON reader (emit presence), sink thrift (+1 flag, reuse hidden-column vehicle), DeltaWriter (union schema + carry presence), memtable (column-aware fold), `RowsetTxnMetaPB` (+dictionary +flag), finalize/lake-handler (per-column pair lists + equivalence classes), plus the DCG proto extensions the SDCG doc already plans. Lake and local are symmetric at every hop.

---

## 5. Constraints

### Condition update (merge_condition)
`merge_condition` is a single column name (`gensrc/proto/olap_file.proto:101`, written `rowset_writer.cpp:205-206`, resolved to one column index at apply `be/src/storage/tablet_updates.cpp:1449-1456`); column-mode + condition is already rejected in lake (`be/src/storage/lake/delta_writer.cpp:830-834`). **Constraint:** the condition column must be present in **every** row's effective set (FE force-includes it into the union AND into every set-id mask), else per-row condition evaluation has no operand. Enforce in FE alongside the existing key/auto-increment force-include (`fe/.../Load.java:669-678`).

### Auto-increment
Force-included into the partial schema in FE (`fe/.../Load.java:671-675`), written as sentinel `0` for existing keys and **discarded at apply** (`be/src/storage/rowset_column_update_state.cpp:705-714,:725-728`; lake `column_mode_partial_update_handler.cpp:288,:301`). **Constraint:** AI cannot be a per-row-optional column — it must be in every set-id mask (or, equivalently, treated as always-present and then the existing discard-on-existing-key logic runs unchanged). AI-in-sort-key stays rejected (`be/src/storage/delta_writer.cpp:339-341`).

### Sort-key / short-key
Column mode forces `num_short_key_columns=1` and sort-key = key columns (`be/src/storage/delta_writer.cpp:307-313`); `is_partial_update_with_sort_key_conflict` (`:297-300`) assumes one subset. **Constraint:** under a union schema the conflict check evaluates against the union (the union is a superset, so if no key/sort conflict on the union there is none per-class). No per-class re-evaluation needed because the union dominates.

### Dense-path fallback (heterogeneous batch that turns out dense for a segment)
If a (rssid, class) is dense (K/M ≥ threshold), it must still be writable as `.cols`. `read_from_source_segment_and_update` (`rowset_column_update_state.cpp:319-387`) reads the full source column with the class's partial schema and overlays via `_update_source_chunk_by_upt`. **What it needs:** the per-column pair list (point 2b) instead of the per-rowset list — i.e. only the upt rows whose mask covers the class's columns are overlaid; unmasked source rows keep their base value (which is exactly the dense `.cols` "unupdated rows keep original value" semantics, SDCG §4.1). No structural change to the dense reader; it just consumes per-column pairs. This guarantees a heterogeneous batch never blocks the dense path.

### Strict mode / omitted vs explicit null
**This is the critical semantic distinction** and Option A handles it natively. Today omission ⇒ `append_nulls(1)` (`json_scanner.cpp:597`, `:627`, `:662`) — indistinguishable from an explicit JSON `null`. Under A:
- **omitted column** ("don't touch") ⇒ the row's presence mask does NOT cover the column ⇒ finalize never adds a pair for it ⇒ base value preserved.
- **explicit JSON null** ("set NULL") ⇒ the mask DOES cover the column, value = SQL NULL ⇒ finalize writes NULL.

The JSON reader already distinguishes these: `_parsed_columns[i]` is true iff the key was physically present (`json_scanner.cpp:565`), regardless of its value being null. So presence = `_parsed_columns`, value-nullness = the column's own null bit — two independent bits, exactly the encoding A needs. `_strict_mode` (`json_scanner.cpp:840`) continues to govern type-cast failures of *present* values only; it is orthogonal to presence. For formats with no "absent" notion (CSV/Parquet — report 2 §6 item 1), flexible mode is simply not offered (or requires an explicit convention); JSON/Debezium CDC is the target and is fully expressible.

---

## 6. Zero-cost degenerate (homogeneous) case

A homogeneous batch (every row updates the identical column set) must produce today's `.upt` byte-for-byte and skip the descriptor:
- FE computes the distinct-set dictionary; if it has exactly one entry, set `flexible_partial_update=false` and emit today's exact slot set (no presence column slot).
- The JSON reader's presence-emit branch is gated on the flexible flag ⇒ stays on today's null-fill path (`json_scanner.cpp:583-600`).
- DeltaWriter builds the same single partial schema (`delta_writer.cpp:304`) with no presence column.
- `rowset_writer.cpp:200-204` writes the same single `partial_update_column_ids` list; `distinct_column_sets` / `flexible_partial_update` (proto tags 9-10) are omitted ⇒ serialized bytes identical (proto2 omits unset optional/empty-repeated).
- finalize sees no dictionary ⇒ takes the per-rowset (not per-column) path ⇒ identical `rss_upt_id_to_rowid_pairs` (`:746-755`) and identical single-DCG-entry output.

So the homogeneous path is the exact current code; heterogeneity is purely additive and opt-in via the FE-set flag. This is the same "DENSE_COLS=0 ⇒ legacy" zero-regression hinge the SDCG doc uses on the storage side (§4.3).

---

## Files cited (all absolute paths in worktree, lines current)
- `be/src/storage/delta_writer.cpp:260-342` — partial schema build, sort-key, auto-increment-in-sort-key reject
- `be/src/storage/rowset/rowset_writer.cpp:180-228` — single per-rowset txn_meta column list, the `referenced_column_ids.size()==columns().size()` assert
- `be/src/storage/rowset_column_update_state.{h,cpp}` — `.h:64-134` (RowidPairs/UptidToRowidPairs/ColumnPartialUpdateState); `.cpp:230-256` (_resolve_conflict source-only remap), `:319-387` (dense read), `:450-485` (_update_source_chunk_by_upt), `:705-732` (AI discard), `:746-832` (rss_upt_id_to_rowid_pairs build + per-(col_batch,rssid) DCG loop)
- `be/src/storage/lake/column_mode_partial_update_handler.cpp:85-102,:225-349` — lake apply, upt=segment, per-batch partial_tschema
- `be/src/storage/memtable.cpp:266-337` — whole-row PK fold (to become column-aware)
- `be/src/exec/file_scanner/json_scanner.cpp:493,:558-600,:614-667,:840` — `_parsed_columns` presence (computed then discarded), null-fill, strict mode
- `be/src/exec/tablet_sink.cpp:814-816` — `__op` rides as last chunk column (the hidden-column precedent)
- `be/src/column/column.h:190-198` — `update_rows` positional whole-column overwrite
- `gensrc/proto/olap_file.proto:94-110` — RowsetTxnMetaPB (next free tag 9)
- `gensrc/proto/olap_common.proto:60-64` — DeltaColumnGroupPB (next free tag 5)
- `gensrc/proto/lake_types.proto:95-102` — DeltaColumnGroupVerPB (tag 5 = shared_files, next free 6)
- `gensrc/thrift/DataSinks.thrift` TOlapTableSink, `gensrc/proto/internal_service.proto` PTabletWriterAddChunk* — front-end carriers
- `fe/fe-core/src/main/java/com/starrocks/load/Load.java:660-701` — FE single-column-set + force-includes (key/AI/generated)
- `handbook/plans/active/2026-06-01-partial-update-sdcg-design.md` §4.1/§4.3/§5.2-5.3 — `.spcols` equivalence class, DCG proto extensions, density decision, SparseColsWriter

---

## 设计修订记录:类打包(packing)与 S3/本地介质分界(2026-06-04 评审)

**触发**:评审两问 —— ①"每列(实为每等价类)一个 `.spcols` 文件,极异构批文件数放大";②"注意什么必须写 S3、什么可只留本地,代价差数量级"。

**修订 ①(类打包)**:Spike A 否决的是"单文件内各列物理行数不同"(option b,Segment v2 结构性不可行)与"无 presence 的纯占位"(option c,语义不可辨)。本修订采用 **option c′ = union 行 + 物理 null 占位 + per-column presence(元数据裁决语义)**:Segment v2 仍零改动(全列统一 K_union 行),占位物理代价≈null 位图(RLE 近零),文件数硬上限 = 每(源段,批)`sdcg_max_spcols_files_per_segment_batch`(默认 1)个,waste 守卫 `sdcg_pack_max_padding_ratio`(默认 4.0)超限才拆分。原"每类一文件"成为单等价类时的自然退化;等价类概念保留为**密度决策单位**(dense 类先出列、微类先 inline,打包池里只剩有界的 sparse 类)。元数据相应扩展:`SparsePresencePB.column_presences`(per-column min/max/count[/roaring])。附带影响:打包列的 ZM 因占位 null 恒 `has_null=true`(over-include 方向,安全略松,P2 可由 presence 恢复精确)。

**修订 ②(S3/本地分界,主文档新增 §5.7)**:lake 上按介质归位 —— 必须进 S3:更新载荷(op_write `.dat`)、`.spcols`/`.cols`(每批每段 ≤1 PUT,打包即请求数控制)、`TabletMetadataPB`(**每版本整体重传**是固有放大器 ⇒ meta 内只放 ~20B/条 pre-filter,roaring 默认入 `.spcols` 文件一次性写入;此处与先前"≤4KB 内联 PB"的统一策略分化为**按引擎区分**:local RocksDB 按 key 重写无重传放大,可内联)。仅本地:page/data cache、merge cache、层栈/反向索引、可重建的 presence。inline patch 在 lake 是双面账(省独立小对象 PUT vs 随 meta 每版本重传),由 512B 单笔预算 + `sdcg_dcg_meta_max_bytes_per_segment` 硬顶强制促升收口。后台收敛在 lake 同时是 S3 经济学(8 GET+1 PUT+8 DELETE 换冷读单对象),监控按对象数+请求数告警。

---

## S3/IO 优化核实原文(2026-06-05)

> 主文档 §5.8 的依据。三份报告:① 写侧平台设施(bundling/merge commit/写穿缓存/segment_metas/aggregate publish);② 读侧与 GC 设施(缓存层/IO 合并/预取/vacuum 批量删除/dense .cols 冷读形状);③ upt-ref 层可行性(否决)。

### 核实 ①:写侧平台设施

## Item 1 — File bundling / shared files: **EXISTS** (shipped, default-on); for `.spcols`/`.cols` reuse: **DOES-NOT-EXIST today, but mechanism is generic and reusable**

**File bundling mechanism (EXISTS).** Multiple logical segment files (across tablets within a partition, within a load txn) are packed into ONE physical S3 object addressed by per-file offset.

- `be/src/fs/bundle_file.h:23-95` — `BundleWritableFileContext` (the shared physical file + mutex + active-writer refcount) and `BundleWritableFile : public WritableFile` (per-logical-file buffer that, on `close()`, appends its bytes to the shared file and records its `bundle_file_offset`). `BundleSeekableInputStream` (`bundle_file.h:97-119`) is the read side: a `(offset,size)` window over the shared object.
- API surface: `try_create_bundle_file(create_file_fn)`, `appendv(slices, info) -> StatusOr<int64_t> offset`, `increase_active_writers()` / `decrease_active_writers()` (last writer closes/uploads), `BundleWritableFile::bundle_file_offset()`. Impl: `be/src/fs/bundle_file.cpp:20-95`.
- Wiring: `be/src/runtime/lake_tablets_channel.cpp:874-907` creates one `BundleWritableFileContext` **per partition** (`_bundle_wfile_ctx_by_partition`, line 293) and passes it into every `AsyncDeltaWriter` via `.set_bundle_writable_file_context(...)`. Gate: `_is_data_file_bundle_enabled()` (`lake_tablets_channel.cpp:826-828`) reads `params.lake_tablet_params().enable_data_file_bundling()` (proto `internal_service.proto:173`).
- Writer integration: `be/src/storage/lake/general_tablet_writer.cpp:213-220` — when a bundle context is present and this is the first segment at EOS, the segment's `WritableFile` is a `BundleWritableFile`. **Important gating**: today bundling only triggers for `_segments.empty() && eos` (one segment per writer at end-of-stream). PK writer participates too (`pk_tablet_writer.cpp:37-40,163`; offset captured at `pk_tablet_writer.cpp:125-127`).
- Offset captured into metadata: `general_tablet_writer.cpp:240-243` sets `segment_file_info.bundle_file_offset`; serialized at `delta_writer.cpp:803-804` into `RowsetMetadataPB.bundle_file_offsets`. Multi-statement merge of offsets across TxnLogs: `txn_log_applier.cpp:873-1010`.

**Proto.** `RowsetMetadataPB.bundle_file_offsets` (lake_types.proto:168, tag 14) and `shared_segments` (tag 15) — a parallel `repeated bool` marking which segments are physically shared.

**FE side (EXISTS).** Property `file_bundling` — `PropertyAnalyzer.PROPERTIES_FILE_BUNDLING` (`fe/.../common/util/PropertyAnalyzer.java:278`, applied at 961-964). Global default `Config.enable_file_bundling = true` (`fe/.../common/Config.java:3761`). RPC carriers: `lake_service.proto:351` and `:457` (`enable_file_bundling`). Table-level `setFileBundling/isFileBundling`, alter support (`LakeTableAlterMetaJobTest`, `LakeTableSchemaChangeJobTest`).

**`DeltaColumnGroupVerPB.shared_files` (lake_types.proto:99-100, tag 5) — meaning.** A `repeated bool`, parallel to `column_files` (tag 2), marking which `.cols` (DCG) files are physically shared across tablets. Who sets it: the publish/cross-publish path copies it into `FileMetaPB.shared` — `meta_file.cpp:155-156`, `:468-469`; `txn_log_applier.cpp:227-228`. What `true` implies:
- **Vacuum**: shared `.cols` files are routed to the `AsyncSharedFileDeleter` (delayed, reference-counted across tablets) instead of immediate delete — `vacuum.cpp:303-311` (`collect_alive_shared_files`, iterates `dcg.shared_files`). A non-shared file goes through the normal deleter. This prevents a shared physical object from being GC'd while a sibling tablet still references it.
- **Migration/tablet-split**: when a segment is rewritten into a brand-new tablet-private file, the shared flag is cleared so it is not GC'd via the shared path — `meta_file.cpp:183-191`, `:1023-1028` (also clears `bundle_file_offsets`).

**Could `.spcols`/`.cols` ride the same mechanism?** Mechanism-wise YES — bundling is a generic `WritableFile` wrapper, not tied to row segments. But TODAY the DCG `.cols` writer does NOT use it: `column_mode_partial_update_handler.cpp:133-154` (`_prepare_delta_column_group_writer`) creates a plain `fs::new_writable_file(opts, path)` with no `BundleWritableFileContext` and no `bundle_file_offset` capture. So packing G sparse equivalence-class `.spcols` into 1 PUT (design §5.7) would require either reusing `BundleWritableFileContext` in the DCG writer or the design's own "类打包" (single-file packing). The `DeltaColumnGroupVerPB` already has the parallel `shared_files` bool and `FileMetaPB.shared` plumbing that a bundled-DCG scheme would need on the vacuum side.

---

## Item 2 — Merge commit / group commit: **EXISTS** (full subsystem, "batch write / merge commit")

- FE: `fe/.../load/batchwrite/` — `BatchWriteMgr.java`, `MergeCommitJob.java`, `MergeCommitTask.java`, `TxnStateDispatcher.java`, `CoordinatorBackendAssignerImpl.java`, `MergeCommitMetricRegistry.java`. Entry via `FrontendServiceImpl.java`. This merges many small load requests sharing a load profile into fewer backend load txns/publishes.
- BE: `be/src/runtime/batch_write/` — `isomorphic_batch_write.cpp`, `batch_write_mgr.cpp`, `txn_state_cache.cpp`, `batch_write_util.cpp`; stream-load entry `http/action/stream_load.cpp`, headers `http/http_common.h`, `runtime/stream_load/time_bounded_stream_load_pipe.cpp` (time-bounded buffering).
- Knobs (BE, `be/src/common/config.h:1895-1910`): `merge_commit_stream_load_pipe_block_wait_us`, `merge_commit_stream_load_pipe_max_buffered_bytes` (1 GiB), `merge_commit_thread_pool_num_min/max` (0/512), `merge_commit_thread_pool_queue_size`, `merge_commit_default_timeout_ms`, `merge_commit_rpc_*`, `merge_commit_txn_state_cache_capacity`, etc.
- Knobs (FE, `fe/.../common/Config.java:4159-4178`): `merge_commit_gc_check_interval_ms`, `merge_commit_idle_ms`, `merge_commit_executor_threads_num`, `merge_commit_txn_state_dispatch_retry_*`, `merge_commit_be_assigner_*`.
- This is "merge commit" (load-side batching of many small txns into fewer publishes). It is the lever the design §5.7 principle 2 calls out ("S3 请求数按每批每段计敛"). No separately-named "group_commit" subsystem — `group_commit` matches are only in third-party BDB JE (`BDBEnvironment.java`), unrelated.

---

## Item 3 — Write-through data cache (populate local cache on segment/.cols write): **PARTIAL**

- `WritableFileOptions` (`be/src/fs/fs.h:277-294`) has `skip_fill_local_cache` (default `false`).
- Starlet write path honors it: `fs_starlet.cpp:384-386` sets `fslib_opts.skip_fill_local_cache = opts.skip_fill_local_cache`. Since the lake write callers never set it, it stays `false`, i.e. the write is NOT told to skip cache fill.
- BUT: none of the BE write paths I inspected (`general_tablet_writer.cpp`, `pk_tablet_writer.cpp`, `column_mode_partial_update_handler.cpp:133-154`) contain any explicit "put bytes into local data cache after upload" call. There is no `fill_data_cache` on the WRITE side; `fill_data_cache` appears only on READ paths (e.g. `column_mode_partial_update_handler.cpp:161` `LakeIOOptions{.fill_data_cache=true}`, and `tablet_manager.cpp` read/get_tablet_metadata `CacheOptions`).
- Conclusion: whether a write actually populates starcache (so the writing node's first read avoids a GET) is delegated to starlet/starcache (`skip_fill_local_cache=false` permits it) and is NOT controlled or guaranteed by code in this repo. There is no explicit BE "write-through fill" hook. For SDCG this means: do not assume the writer node has the just-written `.spcols`/`.cols` warm in cache unless starcache's write-fill behavior is confirmed at the starlet layer; the repo offers no knob to force it.

---

## Item 4 — Per-segment num_rows (M) in lake metadata WITHOUT a footer GET: **EXISTS** (and contradicts design assumption at line 297)

This is the highest-leverage finding for SDCG.

- **Per-segment num_rows IS persisted.** `SegmentMetadataPB` (lake_types.proto:133-145) has `optional int64 num_rows = 3` plus `segment_idx`, `sort_key_min/max`, `vector_index_ids`. `RowsetMetadataPB.segment_metas` (lake_types.proto:171, tag 17) is a `repeated SegmentMetadataPB`.
- **It is set at WRITE time, for free.** `delta_writer.cpp:806-810` — for each finished segment: `add_segment_metas()`, `segment_meta->set_num_rows(f.num_rows)`, `set_segment_idx(...)`. `f.num_rows` comes from `SegmentWriter::num_rows()` (`general_tablet_writer.cpp:246`, `pk_tablet_writer.cpp:131`). Compaction (`compaction_task.cpp:87-91`), schema change (`schema_change.cpp:250-254,342-346`), spark load (`spark_load.cpp:124-128`), and parallel-compaction merge (`tablet_parallel_compaction_manager.cpp:1003-1022`) all populate `segment_metas` too. Carried through publish in `meta_file.cpp:934-966`.
- **Where the column-mode handler gets M today (the footer GET).** `column_mode_partial_update_handler.cpp:179-193`: `tablet_mgr()->load_segment(...)` then `segment->num_rows()`. `load_segment` (`tablet_manager.cpp:1344-1374`) calls `segment->open(footer_size_hint, ...)` which, per the in-code comment (`tablet_manager.cpp:1369` "segment->open will read the footer, and it is time-consuming"), parses the segment footer (`segment.cpp:91-189`, `parse_segment_footer_internal`). On a metacache HIT (`tablet_manager.cpp:1351` `metacache()->lookup_segment`) no GET occurs; on a COLD node (post-migration / first apply) it is an S3 footer GET.
- **M is reachable from already-loaded metadata, no footer GET.** `RowsetUpdateStateParams` (`rowset_update_state.h:79-85`) already carries `const TabletMetadataPtr& metadata`. The handler already has `params.container.rssid_to_rowid()` (rssid → rowset id) and `rssid_to_file()`; the segment position within the rowset maps directly to `rowset.segment_metas(pos).num_rows()` (existing accessor pattern: `meta_file.cpp:42-53` `get_segment_idx`, and zone-map filter `rowset.cpp:348-366` already reads `segment_metas(i).num_rows()`). So for the SDCG density decision (K/M), **M can be read from `params.metadata` without opening the source segment footer** — the design's line 297 claim ("RowsetStats 只有 per-rowset 行数, per-segment 须显式取" via "footer-only Segment::open") is true only for `RowsetStats`; `TabletMetadataPB.rowsets[].segment_metas[].num_rows` already has it per-segment.
- Caveat: `segment_metas` is optional/backfilled — consumers fall back to positional index / footer when absent (`rowset.cpp:348` guards on `segment_metas_size() > 0`; `meta_file.cpp:47` likewise). Old rowsets written before `segment_metas` was added won't have num_rows, so SDCG would still need a footer fallback for those. But for any segment written by current code, M is in metadata.
- Note: the source segment open is still needed at apply time to actually READ the source rows (`_read_from_source_segment`), so eliminating the footer GET helps the *sizing/decision* step, not the data read. The win is: making the density decision before deciding whether to even open/scan, on metadata you already hold.

---

## Item 5 — Aggregate/combined publish to reduce per-version TabletMetadataPB re-upload: **EXISTS** (two distinct mechanisms)

1. **Aggregate publish (bundle tablet metadata) — bundles MANY tablets' metadata into ONE S3 object.**
   - `lake_service.cpp:292` `skip_write_tablet_metadata = request->enable_aggregate_publish()`; per-tablet publish then skips its individual metadata PUT, and at `lake_service.cpp:604` a single `put_bundle_tablet_metadata(tablet_metas)` writes all of them together. Metrics: `g_aggregate_publish_version_*` (`lake_service.cpp:129-132`, `:587`, `:620`).
   - `TabletManager::put_bundle_tablet_metadata` (`tablet_manager.cpp:375-434+`): picks an anchor tablet (`pick_local_anchor_tablet_id`), serializes each tablet's `TabletMetadataPB` (schemas deduped into a shared `schemas` map, `clear_schema()` per tablet) into one buffer, records each at a `PagePointerPB{offset,size}` in `BundleTabletMetadataPB.tablet_meta_pages`, and writes ONE object at `bundle_tablet_metadata_location(anchor_tablet_id, anchor_version)`. Read side: `parse_bundle_tablet_metadata` / `get_metas_from_bundle_tablet_metadata` (`tablet_manager.cpp:591-756`), single-flight via `_bundle_tablet_metadata_group` and a real-path cache key so all tablets share one fetch (`tablet_manager.cpp:687-696`). This directly addresses "per-version TabletMetadataPB re-upload" amplification — N tablets = 1 PUT instead of N. This is exactly the "tablet meta 每版本整体重传" cost the design §5.7 row 3 worries about, at the partition level.

2. **Combined txn log + batch publish — fewer publishes per partition.**
   - `lake_use_combined_txn_log` (`Config.java:1142`) — collects many tablets' TxnLogs into one `CombinedTxnLogPB` object (`tablet.h:98` `put_combined_txn_log`; `tablet_manager.cpp:850-957`; per-partition coordinator election `lake_enable_per_partition_coordinator_txn_log`, `Config.java:1154`).
   - `lake_enable_batch_publish_version = true` with `lake_batch_publish_max/min_version_num` (`Config.java:1133-1139`) — publishes multiple versions in one pass, amortizing metadata writes across versions.

No "meta-delta" (incremental diff of TabletMetadataPB) mechanism exists — each version is still a full TabletMetadataPB; the optimizations are (a) co-locating many tablets' full metas in one object and (b) batching versions/txn-logs. For SDCG, the actionable consequence is that DCG-only updates still re-upload the full per-tablet TabletMetadataPB each version (design §5.7 row 3 / line 399), and the existing levers to dampen that are aggregate-publish bundling + keeping `dcg_meta` minimal (pre-filter only), which the design already plans.

---

### Net for SDCG §5.7
- Bundling (1), aggregate publish (5), merge commit (2) are real, shipped, default-on platform pieces SDCG can lean on — the S3-request-economics levers the design wants mostly exist. The `.spcols` PUT-coalescing (design "类打包") can either reuse `BundleWritableFileContext` (currently unused by the DCG writer) or be done as single-file packing in the new SparseColsWriter; the `shared_files`/`FileMetaPB.shared` vacuum plumbing for shared DCG files already exists.
- (4) is a concrete optimization opportunity beyond the current design: M is already in `segment_metas[].num_rows` for current-format segments, so the density decision can avoid the footer GET on cold nodes by reading `params.metadata` (with a footer fallback for legacy segments lacking `segment_metas`).
- (3) write-through cache is only PARTIAL/implicit; SDCG should not assume the writer node's first read of a just-written `.spcols`/`.cols` is cache-warm without confirming starcache's write-fill behavior at the starlet layer — there is no BE-side force-fill hook.

### Key file:line index
- Bundle write: `be/src/fs/bundle_file.h:23-119`, `be/src/fs/bundle_file.cpp:20-95`; wiring `be/src/runtime/lake_tablets_channel.cpp:293,826-828,874-907`; writer `be/src/storage/lake/general_tablet_writer.cpp:213-243`, `pk_tablet_writer.cpp:125-127,163`.
- Proto: `gensrc/proto/lake_types.proto:95-103` (DCG shared_files), `:133-145` (SegmentMetadataPB.num_rows), `:168-171` (bundle_file_offsets/shared_segments/segment_metas); `internal_service.proto:173`; `lake_service.proto:351,457`.
- shared flag vacuum/migration: `be/src/storage/lake/vacuum.cpp:223-323,509-516`; `be/src/storage/lake/meta_file.cpp:155-156,183-191,468-469,1023-1028`.
- Merge commit: `fe/.../load/batchwrite/*`, `be/src/runtime/batch_write/*`, `be/src/common/config.h:1895-1910`, `fe/.../common/Config.java:4159-4178`.
- Write cache: `be/src/fs/fs.h:277-294`, `be/src/fs/fs_starlet.cpp:384-386`.
- M / num_rows: `delta_writer.cpp:806-810`, `column_mode_partial_update_handler.cpp:156-207`, `tablet_manager.cpp:1344-1374`, `segment.cpp:91-189`, `rowset_update_state.h:79-85`, `rowset.cpp:348-366`, `update_manager.h:63-68`.
- Aggregate/combined publish: `be/src/service/service_be/lake_service.cpp:129-132,292,604`; `tablet_manager.cpp:375-434,591-756,850-957`; `fe/.../common/Config.java:1133-1154`.
- SDCG design assumptions: `handbook/plans/active/2026-06-01-partial-update-sdcg-design.md:297` (footer-only M), `:391-407` (§5.7 medium cost), `:399` (meta re-upload), `:478` (lake fill_data_cache as explicit knob).

### 核实 ②:读侧与 GC 设施

# Lake READ/GC IO infrastructure — evidence

All paths are in the current worktree. Line numbers cite the current files.

---

## Item 1 — Data cache / page cache / metadata cache layers + knobs

**LakeIOOptions** (`be/src/storage/options.h:72-89`): `fill_data_cache`, `skip_disk_cache`, `buffer_size(-1)`, `fill_metadata_cache`, `use_page_cache`, `cache_file_only`, plus `sst_warmup_fn`. All default to **false / -1**.

How each knob maps (EXISTS):
- **`fill_data_cache`** → becomes `RandomAccessFileOptions.skip_fill_local_cache = !fill_data_cache` (`be/src/storage/rowset/segment.cpp:271`, `:393`; `segment_iterator.cpp:1233`). On starlet it flows to fslib `ReadOptions.skip_fill_local_cache` (`be/src/fs/fs_starlet.cpp:332`). The **local disk data cache lives inside fslib/CacheFs**, not in BE `be/src/io`. The BE `io::CacheInputStream` (`be/src/io/cache_input_stream.cpp`) is used **only by the connector/external-table path** (parquet/orc/iceberg/cache-select per the consumer list), NOT the native lake segment read path.
- **`use_page_cache`** → `PageReadOptions.use_page_cache` → `StoragePageCache` (`be/src/cache/mem_cache/page_cache.{h,cpp}`). Page cache is keyed by **`encode_cache_key(filename, page_offset)`** (`be/src/storage/rowset/page_io.cpp:329`), gated at `page_io.cpp:330` (lookup) and `:296`/`:347` (insert). It caches **decompressed column data/index pages**, per (file, offset).
- **`skip_disk_cache`** → fslib `ReadOptions.skip_read_local_cache` (`fs_starlet.cpp:334`).
- **`buffer_size`** → `RandomAccessFileOptions.buffer_size` → fslib `ReadOptions.buffer_size` (`fs_starlet.cpp:333`). For **plain S3** (non-starlet) it maps to a read-ahead buffer on `S3InputStream` (default read-ahead 64KB from `fs_s3.cpp:451-462`, buffer disabled when `_read_ahead_size<=0`; `s3_input_stream.h:30-35,70`).

**What `fill_metadata_cache` actually caches** (EXISTS, but it is the *Segment object*, not a separate footer cache):
- The lake **Metacache** (`be/src/storage/lake/metacache.{h,cpp}`) is a **single LRU** (capacity `config::lake_metadata_cache_limit`, default 2GB, `config.h:1301`) holding a `CacheValue` variant of TabletMetadataPB / TxnLogPB / TabletSchema / DelVector / **Segment** / CombinedTxnLogPB (`metacache.h:37-39`).
- `fill_metadata_cache` gates whether `TabletManager::load_segment` inserts the opened `Segment` into the metacache keyed by `segment_info.path` (`tablet_manager.cpp:1360-1367`, via `cache_segment_if_absent`).
- The **segment footer is parsed once** inside `Segment::_open` (`segment.cpp:268-286`) into a stack-local `SegmentFooterPB`, immediately converted to per-column `ColumnReader`s held in `Segment::_column_readers` (`segment.cpp:432-447`). The footer PB itself is NOT stored; the *parsed* column readers are. So caching the Segment (via metacache) = caching the parsed footer. `mem_usage()` reflects column index mem (`segment.cpp:698-704`).

**Is there a segment-footer in-memory cache keyed by file so .spcols footers parse once?** — **PARTIAL / effectively NO for DCG.** A base segment's footer is parsed once and reused only if its `Segment` is cached (path-keyed) in the metacache. But **DCG segments are never cached** — see Item 5. There is no standalone footer cache distinct from the Segment object.

---

## Item 2 — IO coalescing / ranged GET

**Infrastructure EXISTS:** `io::SharedBufferedInputStream` (`be/src/io/shared_buffered_input_stream.h`) with `IORange`, `CoalesceOptions{max_dist_size=1MB, max_buffer_size=8MB}` (`:36-40`), `set_io_ranges()` (`:80`), `get_bytes`/`find_shared_buffer`, `release()/release_to_offset()`. It coalesces scattered page reads into merged ranged GETs.

**Wired into the native lake BASE-column scan (EXISTS, but OFF by default):**
- `SegmentIterator::_init_column_iterator_by_cid` wraps the base segment read_file in a `SharedBufferedInputStream` only when `config::io_coalesce_lake_read_enable && !is_default_column && lake_tablet_manager()!=nullptr` (`segment_iterator.cpp:1256-1268`). Default config is **`io_coalesce_lake_read_enable="false"`** (`config.h:1134`); knobs `io_coalesce_read_max_buffer_size=8MB` (`:1168`), `io_coalesce_read_max_distance_size=1MB` (`:1169`).
- Ranges are fed at scan init via `convert_sparse_range_to_io_range` → `get_io_range_vec` → `set_io_ranges` (`column_iterator.h:116-130`; driven from `segment_iterator.cpp:916-918`). `ScalarColumnIterator` releases the shared buffer at EOF (`scalar_column_iterator.cpp:227-232,261-266,303-308`).

**DCG read path does NOT coalesce (DOES-NOT-EXIST for DCG):**
- In the scan path, the DCG branch explicitly opts out: `segment_iterator.cpp:1273-1281` has the comment **`// TODO io_coalesce`** and opens the DCG file with a plain `new_random_access_file` (no SharedBufferedInputStream, `is_io_coalesce` left false).
- In the partial-update apply path, `new_lake_dcg_column_iterator` (`update_manager.cpp:1301-1324`) opens each DCG file via plain `new_random_access_file_with_bundling` with default `RandomAccessFileOptions` — no coalescing, no buffer_size, no cache flags.

**`fetch_values_by_rowid` over object-store segment (the apply path):** `ScalarColumnIterator::fetch_values_by_rowid` (`scalar_column_iterator.cpp:697-703`) → `_fetch_by_rowid_helper` seeks pages and reads needed pages via `PageIO::read_and_decompress_page`. Each page miss is one `read_at` → on the unbuffered/uncached DCG read_file this is **a per-page ranged GET** (no coalescing). With page cache off (apply path never sets `use_page_cache`, `update_manager.cpp:1416-1420` `iter_opts` leaves it default false), nothing is cached across rowids/segments.

`new_random_access_file_with_bundling` (`fs.h:147`) only remaps reads inside a bundled shared segment file; it is NOT a coalescer.

---

## Item 3 — Prefetch on the lake scan path

**No BE-side async prefetch facility on the native lake segment scan (DOES-NOT-EXIST in BE):**
- The only "prefetch" in the lake scan is **surfaced from the underlying stream's numeric statistics** (i.e. implemented inside fslib/CacheFs/starlet): `segment_iterator.cpp:3576-3611` reads `kPrefetchHitCount/kPrefetchWaitFinishNs/kPrefetchPendingNs` from `get_numeric_statistics()` and copies them into `OlapReaderStatistics` (`olap_common.h:320-322,361-363`). BE does not initiate these.
- `S3InputStream` has a synchronous read-ahead buffer (`s3_input_stream.cpp:40-122`), not an async multi-stream prefetch.
- The only BE concurrency for fetching multiple segments is **parallel segment loading** via `ExecEnv::load_segment_thread_pool()` in `Rowset::load_segments` (`rowset.cpp:698-726`), gated by `config::enable_load_segment_parallel` (default **false**, `config.h:649`). This parallelizes base-segment open across a rowset; it is NOT used by the partial-update apply path (`get_column_values` iterates `rowids_by_rssid` **serially**, `update_manager.cpp:1462-1476`) and does NOT prefetch overlay/DCG columns. There is no facility today to fetch all overlay layers' columns concurrently at iterator init.

---

## Item 4 — Vacuum delete batching (EXISTS, solid)

- **S3 batches into `DeleteObjects` up to 1000 keys:** `S3FileSystem::delete_files` (`fs_s3.cpp:980-1040`): `max_delete_keys = 1000` (`:1002`), loops building `Aws::S3::Model::Delete().WithObjects().WithQuiet(true)` and calls `client->DeleteObjects` per 1000-key chunk (`:1017-1022`). Per-error reporting at `:1026-1032`.
- **Vacuum-side batching + async pipelining:** `AsyncFileDeleter` (`be/src/storage/lake/async_file_deleter.h:33-90`) accumulates paths to `_batch_size` then `submit()` → `delete_files_callable` (async). `submit` **waits for the previous task before issuing the next** (`:72-83`) — single in-flight overlap, not unbounded fan-out. `_batch_size = config::lake_vacuum_min_batch_delete_size` (default **100**, `config.h:1708`) at all construction sites (`vacuum.cpp:484,492,554,810`).
- `do_delete_files` (`vacuum.cpp:142-181`) further chunks by the same `lake_vacuum_min_batch_delete_size` and calls `delete_files_with_retry` → `fs->delete_files` (retry on resource-busy/pattern, `:110-137`). `delete_files_async` / `delete_files_callable` dispatch onto `ExecEnv::delete_file_thread_pool()` (`vacuum.cpp:193-215`).
- Note: the vacuum batch granularity (100) is smaller than the S3 API cap (1000); raising `lake_vacuum_min_batch_delete_size` toward 1000 reduces RPC count. `AsyncSharedFileDeleter` overrides to delay-delete shared files (`async_file_deleter.h:94-121`).

---

## Item 5 — Cold-read shape for a dense .cols on lake (calibration for .spcols)

When a query/apply first touches a dense DCG `.cols`:

**DCG segment open** — `Segment::new_dcg_segment` (`segment.cpp:637-651`) calls the **static `Segment::open(_fs, info, 0, tablet_schema, nullptr)`** which uses the **default `lake_io_opts = {}`** (`segment.h:88-94`: `fill_data_cache=false, use_page_cache=false, fill_metadata_cache=false, buffer_size=-1, skip_disk_cache=false`).
- This triggers `Segment::_open` (`segment.cpp:268-286`): one `get_size()` + `parse_segment_footer` (`segment.cpp:101-189`). Footer read = **1 GET** of `footer_length_hint` bytes (default hint 16KB from `rowset.cpp:580`; for `new_dcg_segment` the hint is 0 so `parse_segment_footer_internal` falls back to a 4096-byte read, `segment.cpp:112`), then **a 2nd GET** only if `footer_length > buff.size()` (`segment.cpp:161-178`, counted as `g_open_segments_io << 2`). So **footer = 1-2 GETs**.
- **The DCG Segment is never cached:** `new_dcg_segment` results are stored only in transient per-call maps (`update_manager.cpp:1284-1296` `ctx.dcg_segments`; `segment_iterator.cpp:1128`; `tablet_updates.cpp:5279`; `meta_reader.cpp:220`). None call `metacache->cache_segment*`. Only base segments go through `TabletManager::load_segment` → `cache_segment_if_absent` (`tablet_manager.cpp:1351-1373`). **Consequence: every read re-opens + re-parses the DCG footer (cold every time).**

**Column data read** — for each needed column, a `ColumnIterator` reads only the needed pages via `PageIO::read_and_decompress_page`:
- Apply path `fetch_values_from_segment` (`update_manager.cpp:1390-1460`) opens the base segment with **default LakeIOOptions** (`Segment::open(fs, file_info, segment_id, tablet_schema)` at `:1401`, no opts), `iter_opts` never sets `use_page_cache` (`:1416-1420`), read_file is plain `new_random_access_file_with_bundling` with default opts (`:1419`). So: **footer GET(s) + one GET per needed column page, uncached, unbuffered, serial per segment** (`:1462-1476`).
- It does NOT read the whole file — only the pages covering the requested rowids (`fetch_values_by_rowid`, `scalar_column_iterator.cpp:697-703`). For a sparse rowid set this is potentially many small per-page GETs.

**So a dense .cols cold read ≈ (1-2 footer GETs) + (1 GET per needed column-page), all uncached & uncoalesced.** A `.spcols` cold read inherits exactly this shape unless SDCG adds: (a) caching the overlay Segment/footer in the metacache (path-keyed) so footers parse once, (b) page cache (`use_page_cache=true`) and/or data-cache fill on the overlay read, and (c) IO coalescing for the overlay column pages (resolve the `// TODO io_coalesce` at `segment_iterator.cpp:1275` and the plain-file open in `update_manager.cpp:1319`). Query-path scans set `fill_data_cache=true, fill_metadata_cache=true` (`tablet_reader_params.h:67`) for **base** segments, but those flags do not propagate to DCG segments because `new_dcg_segment` discards them.

---

## Net assessment for SDCG S3/IO optimization

| Capability | Status | Hook for SDCG |
|---|---|---|
| Page cache (per file,offset) | EXISTS | set `use_page_cache=true` + carry through DCG iter_opts |
| Local data cache (fslib) | EXISTS | set `fill_data_cache=true` on DCG `Segment::open` |
| Metadata/footer reuse | PARTIAL (Segment cache exists; DCG not cached) | cache `.spcols` Segment in metacache keyed by path |
| IO coalescing (SharedBufferedInputStream) | EXISTS but DCG path skips it (TODO) + off by default | enable for overlay column pages |
| Async prefetch (BE) | DOES-NOT-EXIST (only fslib-internal) | parallel-load / new prefetch for overlay layers |
| Parallel segment load | EXISTS (off by default), base only | extend to overlay fetch |
| Vacuum DeleteObjects batching ≤1000 + async | EXISTS | optionally raise `lake_vacuum_min_batch_delete_size` (100→~1000) |

### 核实 ③:upt-ref 层可行性(否决)

## Summary table

| # | Question | Verdict |
|---|---|---|
| 1 | Lake: does op_write .dat survive past publish? | **DOES-NOT-EXIST** — .dat is orphaned at the same publish; never in metadata.rowsets() |
| 2 | Local: does .upt persist with rowset until compaction? | **EXISTS** (.upt survives in version chain) but lifecycle coupling to DCG is **PARTIAL/unsafe** |
| 3 | Existing cross-rowset file-reference pattern in DCG meta? | **DOES-NOT-EXIST** — shared_files means cross-*tablet*, not cross-rowset; filename addressing is technically possible |
| 4 | Conflict/compaction guard protecting the UPDATE rowset? | **DOES-NOT-EXIST** — guards protect SOURCE segments; nothing pins the update payload for DCG refs |
| 5 | Mapping size for K pairs | **Feasible size-wise**: ~0.5KB (K=100), ~4KB (K=1k), ~39KB (K=10k) |

---

## 1. Lake — op_write .dat does NOT survive (HARD BLOCKER) — DOES-NOT-EXIST

Traced `publish_column_mode_partial_update` → `ColumnModePartialUpdateHandler::execute` → `MetaFileBuilder::apply_column_mode_partial_update`.

The decisive code is `be/src/storage/lake/meta_file.cpp:233-243`:
```cpp
void MetaFileBuilder::apply_column_mode_partial_update(const TxnLogPB_OpWrite& op_write) {
    // remove all segments that only contains partial columns.
    for (int i = 0; i < op_write.rowset().segments_size(); ++i) {
        FileMetaPB file_meta;
        file_meta.set_name(op_write.rowset().segments(i));
        ...
        _tablet_meta->mutable_orphan_files()->Add(std::move(file_meta));  // -> orphan, then vacuum
    }
}
```
Called from `column_mode_partial_update_handler.cpp:524` (`builder->apply_column_mode_partial_update(params.op_write)`) right after the DCGs are appended at `:521-523`.

Key facts:
- The op_write rowset is **never** added to `metadata.rowsets()` in the column-mode path. `apply_opwrite` (which DOES `add_rowsets()`, meta_file.cpp:165-168) is only invoked for the *insert* sub-path on a **new** rowset (`new_rows_op`), not the original op_write.
- The COLUMN_UPSERT insert path writes **brand-new** segments via `gen_segment_filename(txn_id)` (`be/src/storage/lake/update_manager.cpp:763-804`) — it re-reads the op_write payload and materializes fresh `.dat`, so it does not keep the original op_write `.dat` either.
- Orphan files are deleted by vacuum once the metadata version passes the retention/grace boundary: `be/src/storage/lake/vacuum.cpp:270-281` (`for (file : metadata.orphan_files()) deleter->delete_file(...)`), and counted as reclaimable garbage at `:349-351`.
- Readers already never read the op_write rowset's segments (they read base + DCG `.cols`); after publish the rowset doesn't exist in the version chain at all, so `num_update_files`/skip logic is moot — the payload is simply gone.

**Conclusion**: On lake the update payload lives for exactly one publish. An upt-ref DCG entry written at version V would dangle by V+1 (or as soon as vacuum runs past V). There is no version window in which it is safe to reference. This kills the upt-ref tier on lake unless you change `apply_column_mode_partial_update` to retain the op_write rowset in `metadata.rowsets()` — which would (a) reintroduce the rowset into the version chain (compaction/score/PK-index implications), (b) require a "skip in reads" mark, and (c) re-create the exact GC-coupling hazard described in item 4.

---

## 2. Local — .upt persists with rowset (EXISTS), but lifecycle coupling is PARTIAL/unsafe

`.upt` deletion is in `Rowset::remove()` at `be/src/storage/rowset/rowset.cpp:388-393`:
```cpp
for (int i = 0, sz = num_update_files(); i < sz; ++i) {
    std::string path = segment_upt_file_path(_rowset_path, rowset_id(), i);
    fs->delete_file(path); ...
}
```
`Rowset::remove()` (`:350`) is only called from `TabletUpdates::_remove_unused_rowsets` at `tablet_updates.cpp:4762`.

The column-mode update rowset **does** stay in the version chain:
- `_rowset_commit_unlocked` adds it to the rowset set and as a delta: `edit.mutable_rowsets()->Add(rowsetid)` (`tablet_updates.cpp:737/743-746`) and `edit.add_deltas(rowsetid)` (`:752`).
- The reserve-id comment at `tablet_updates.cpp:754-756` ("reserve id if .upt files exist, because we may transfer them to .dat files later") confirms the rowset (and its `.upt`) is intentionally kept post-apply.
- A rowset is moved to `_unused_rowsets` only when it drops out of `active_rowsets` (i.e., compaction removes it from every live EditVersion): `tablet_updates.cpp:2785-2796`.

So far this matches the design's claim ("both die at compaction"). **But the DCG and the .upt do NOT share a single death trigger:**

- DCG `.cols`/`.spcols` files are GC'd by **version expiry** in `UpdateManager::clear_delta_column_group_before_version` → `DeltaColumnGroupListHelper::garbage_collection`, keyed on `min_readable_version` (`update_manager.cpp:290-327`, `delta_column_group.cpp:245-285`). It deletes the DCG's `column_files()` only.
- The `.upt` is removed by **rowset-leaves-chain** (compaction), an unrelated trigger (`tablet_updates.cpp:4717-4766`).

For a vanilla `.spcols`, the file IS one of the DCG's `column_files`, so it dies with the DCG under a single trigger — airtight. For upt-ref, the referenced `.upt` belongs to a **different object** (its own rowset) on a **different lifecycle**. The dangerous ordering is: the `.upt`-bearing rowset gets compacted/removed (item 4) while a *newer* source segment's DCG still references that `.upt`. Nothing in `_remove_unused_rowsets` or `garbage_collection` checks for live DCG references into a rowset's `.upt` before deleting it.

---

## 3. Addressing — no cross-rowset reference pattern exists (DOES-NOT-EXIST); filename addressing is technically usable

- `shared_files` in `DeltaColumnGroupVerPB` (`gensrc/proto/lake_types.proto:101-102`) and `FileMetaPB.shared` (`:81-82`) are documented as **"file shared by multiple tablets"** — the tablet split/merge cross-publish mechanism (see rowset.cpp:391, meta_file.cpp:155-156). It is NOT a within-tablet cross-rowset reference. There is no precedent for a DCG entry pointing at another rowset's file.
- DCG `column_files` stores a **bare filename**, resolved as `dir_path + "/" + filename` (`be/src/storage/delta_column_group.h:65-71`). All of `.dat`/`.upt`/`.cols`/`.spcols` live in the same tablet dir, so a DCG entry *could* physically name a `.dat`/`.upt`. The lake whitelist already accepts `.dat` (`be/src/storage/lake/filenames.h:219`), so referencing it wouldn't trip orphan-cleanup.
- Stable identifier: filename is the natural key (encodes rowset_id + segment idx on local via `segment_upt_file_path`, rowset.cpp:186-187; encodes txn_id + uuid on lake). But filename stability is exactly what breaks under compaction — the file is deleted, not renamed, so a filename ref is a dangling-pointer risk, not a rename-tracking problem.

---

## 4. Conflict/compaction interplay — the dangerous case is unguarded (DOES-NOT-EXIST)

- Local guard `_check_conflict_with_partial_update` (`tablet_updates.cpp:2154-2193`) cancels a compaction whose start version predates an applied column-mode update. Lake guard `CompactionUpdateConflictChecker::conflict_check` (`column_mode_partial_update_handler.cpp:532-559`) cancels a compaction when any input rowset's **source segment** has a DCG version newer than `compact_version`.
- Both guards protect the **SOURCE** segments of a DCG (the segments whose rowids the `.spcols`/upt-ref maps). As the question notes, a compaction that rewrites the source segments correctly invalidates the mapping's source_rowids — same invariant as `.spcols`, fine.
- **The dangerous case — compacting away the UPDATE rowset itself while DCG refs into its .upt are still live — has no guard.** On lake the update rowset isn't even in the chain (item 1), so "compact it away" is already done by orphaning at publish. On local, `CompactionUpdateConflictChecker` has no analogue, and `_check_conflict_with_partial_update` only reasons about version ordering of *applied updates*, not about whether a to-be-removed rowset's `.upt` is referenced by some other segment's DCG overlay. `_remove_unused_rowsets` deletes `.upt` purely on `use_count`/active-set membership (`tablet_updates.cpp:4721-4762`), with no DCG-reference check. So the update rowset can be removed (taking its `.upt` with it) while a live DCG layer still needs it → silent data loss / read corruption.

To make upt-ref safe you would need a new pin: a reverse index "rowset X's upt_id is referenced by DCG entries {...}" plus a GC gate that refuses to remove a rowset (or free its `.upt`) until all referencing DCG layers are materialized/promoted. That is net-new machinery with the same complexity the `.spcols` single-trigger design was chosen to avoid.

---

## 5. Mapping size — feasible (the only green item)

Per (segment, batch): source_rowids as Roaring + upt_rowids as zigzag-delta-varint, over a 1M-row base segment:

| K | roaring(source) | upt delta-varint | total |
|---|---|---|---|
| 100 | ~388 B | ~100 B | **~0.5 KB** |
| 1,000 | ~2.2 KB | ~2.0 KB | **~4.1 KB** |
| 10,000 | ~20 KB | ~20 KB | **~39 KB** |

The upt_rowid mapping is shared across columns (presence per-column is separate, same as `.spcols`), so this is the whole per-(segment,batch) cost. For the high-frequency CDC case (K=100..1000) a per-(segment,batch) mapping is comfortably under a few KB — size is **not** the blocker.

Important caveat: where this mapping lives reproduces §5.7's cost problem. On lake, embedding it in `dcg_meta` (inside `TabletMetadataPB`) means it is re-uploaded every publish for its whole lifetime (O(versions × bytes)). The §4.3 escape hatch for `.spcols` is "store roaring inside the file object." For upt-ref there IS no new file object (that's the whole point), so the mapping has nowhere cheap to live on lake — it must go in the per-version-retransmitted meta. So even setting aside the item-1 hard blocker, the lake S3-economics argument that motivated `.spcols` works *against* upt-ref.

---

## Bottom line

The upt-ref tier is **not worth pursuing**:
- **Lake**: hard blocker — payload destroyed at publish (meta_file.cpp:233-242 + vacuum.cpp:270-281). No version window exists. Retaining the rowset to fix this re-imports the rowset into the chain and re-creates the item-4 GC hazard.
- **Local**: the `.upt` survives, but its lifecycle is decoupled from the DCG's (compaction-trigger vs version-expiry-trigger), and there is no guard preventing the update rowset's removal while live DCG overlays reference its `.upt`. Making it airtight needs a net-new reverse-reference pin + GC gate.
- The `.spcols` tier is safe precisely because the overlay file is one of the DCG's own `column_files` — single death trigger, file dies with the DCG. upt-ref trades exactly that property for one saved PUT, and on lake doesn't even save the PUT cleanly (mapping must ride per-version meta). Keep `.spcols`; use `inline patch` (§5.4) for the genuine zero-new-object micro-batch case, which already rides the meta PUT without referencing a foreign file's lifecycle.

Files cited: `be/src/storage/lake/meta_file.cpp:165-168,233-243`; `be/src/storage/lake/column_mode_partial_update_handler.cpp:521-559`; `be/src/storage/lake/update_manager.cpp:763-829,912-950`; `be/src/storage/lake/vacuum.cpp:270-281,349-351`; `be/src/storage/lake/filenames.h:219`; `be/src/storage/rowset/rowset.cpp:186-187,350,388-393`; `be/src/storage/tablet_updates.cpp:696-757,1204-1326,2154-2193,2785-2802,4717-4766`; `be/src/storage/update_manager.cpp:290-327`; `be/src/storage/delta_column_group.cpp:65-87,245-285`; `be/src/storage/delta_column_group.h:65-79,104`; `gensrc/proto/lake_types.proto:78-103`.
