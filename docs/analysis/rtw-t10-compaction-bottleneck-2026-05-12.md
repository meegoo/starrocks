# rtw_t10 Compaction Bottleneck Analysis (2026-05-12)

**Test environment**
- StarRocks shared-data mode (lake), 6× CN nodes (16c61g / `mem_limit=90%` → ~53 GB BE process limit)
- BE binary: PR-1' branch `claude/pr1prime-on-good-base` (base `6d1320c7ccf` + cherry-pick `29c8a4a` + commit `ba3328b`)
- Target table `rtw_t10`: 10T tier in bigseed template — 1.34 B rows in partition `p1`, 100 GB / tablet, BUCKETS=2, PRIMARY KEY + ORDER BY (separate sort key)
- Data state at drain start: 2 rowsets × 1378 segments / tablet, compaction score (cs) = 5348 on p1
- Drain trigger: S6 bench at 200 TPS killed at 00:38 after early-stage OOM cascade; no further user ingest during drain

## TL;DR

p1's compaction takes **5.5 hours** of wall time to converge from cs=5348 to cs=6. The other nine partitions converge in under 2 hours. Five distinct phases drive the timeline, and only the **last** phase (a base-level PK-index merge) actually moves cs below the OOM/overlap floor — the four LARGE_ROWSET_PART segment-index splits between hours 1 and 4 are largely wasted work because they produce more overlapping rowsets than they consolidate. With the right configs the same drain should take ~3.5 hours; with bucket=20 and aligned sort key, ~30 minutes.

## Phase-by-phase timeline

All times in CST. Data sourced from BE `cn.INFO.log.20260511-101100`, 793 collector snapshots, and `SHOW PROC '/compactions'`.

| Phase | Window | Wall time | What ran | cs change |
|---|---|---|---|---|
| 1 | 00:00 – 02:36 | **156 min** | Round 1 (txn 17187) — vertical_compaction + pk_index merge, **3× OOM retries** (00:41, 00:42, 01:01) | 5348 → 1158 |
| 2 | 02:36 – 04:00 | 84 min | Round 2 (txn 17615) — vertical 70min + publish 14min, LARGE_ROWSET_PART output | 1158 → 1659 ↑ |
| 3 | 04:00 – 04:57 | 57 min | Round 3 (txn 17997) — same LARGE_ROWSET_PART path | 1659 → **2158** ↑ |
| 4 | 04:57 – 05:28 | 31 min | **16 quick PK-index L0/L1 merges**, ~2min each, fixed −148.5 cs / round | 2158 → 34.5 ⬇ |
| 5 | 05:28 – 05:55 | 27 min | **1 base-level PK-index merge** (txn 18384) — 80 GB input across 16 sub-tasks | 34.5 → **6** ⬇ |

Versions (`COMPACT_VERSION`): 6 → 7 → 8 → 9..23 → 24.

## Real cost of one vertical-compaction round on p1

From the round 2 BE trace (`lake_service.cpp:550 Published txns=17615.tablets=23795`, cost = 875 655 921 µs = 14.6 min publish):

```
compaction_replace_index_latency_us      628 628 353  (10.5 min)   ← PK index replace
pindex_memtable_replace_us               444 366 454  ( 7.4 min)   sub-step
compaction_get_next_latency_us           162 829 130  ( 2.7 min)   read input
pindex_init_sst_open_us                   38 429 453  (32 s)        load existing SSTs
pindex_init_sst_total_bytes           53 669 991 163  (50 GB)       PK index size
pindex_init_fileset_cnt                          768
pindex_init_sst_cnt                            1 215
total_segment_cnt (post-compaction)              109
output_rowset count (LARGE_ROWSET_PART)            3 / tablet
```

The 670 M-row tablet keeps a ~50 GB persistent index spread over ~1 200 SSTs. Each compaction has to load, merge, and replace this whole structure, then write 1 GB+ of new SST data. `compaction_replace_index_latency_us` alone is **73 %** of the publish wall time.

## Phase 4: 26 "quick" PK-index compactions, mostly no-ops

Each round in phase 4 fires a `LakePersistentIndexParallelCompactMgr` job with 16 sub-tasks. Trace from a representative round at 05:00:25 (`lake_persistent_index_parallel_compact_mgr.cpp:354`):

```
SubCompactionTask 0  input  19 SST /  600 MB   output    1.1 KB     ← no-op (re-tier)
SubCompactionTask 1  input  38 SST / 1.22 GB   output    1.4 KB     ← no-op
SubCompactionTask 2  input  57 SST / 1.83 GB   output    2.7 KB     ← no-op
SubCompactionTask 3  input  76 SST / 2.45 GB   output  859 MB       ← real merge
SubCompactionTask 4  input  67 SST / 2.15 GB   output    1.2 KB     ← no-op
…
                                       (typically 2-3 real, 13-14 no-ops)
total_latency_us         107 606 133   (108 s)
output sstables            56
```

Net effect: cs drops by an extremely consistent **148.5** per round, no matter what cs starts at. The amount of real work per round is tiny — 1-3 sub-tasks merging at most a few hundred MB — but the orchestration cost (open all SSTs, decide what to merge, write the new manifest) repeats 26 times. Phase 4 burns **31 min** to drop cs from 2158 to 34.5.

## Phase 5: one big base-level merge does what 26 quick rounds could not

At 05:28:54, a different style of PK-index compaction starts (trace of `lake_persistent_index_parallel_compact_mgr.cpp:354 completed` finishing 20.6 minutes later):

```
input filesets    20      output sstables  234
total_latency_us   1 238 960 040   (20.6 min)

Per sub-task:
  compact_input_bytes        4-6 GB
  compact_input_sst_cnt      77-119
  task_latency_us            525-658 s   (8-11 min each)
  16 sub-tasks total ≈ 80 GB input / 16 GB output
```

This is what finally consolidates the index and lets cs cross the overlap floor. **Until this fires, cs cannot drop below ~34.5** because the three LARGE_ROWSET_PART output rowsets each register their segments separately under `io_count()`.

Cs trajectory: 34.5 (stable for 27 min waiting for this round to complete) → **6** the moment txn 18384 publishes.

## Why the LARGE_ROWSET_PART splits make cs go up

`be/src/storage/lake/tablet_parallel_compaction_manager.cpp:1735-1758` (`_split_large_rowset`):

```cpp
group.type        = SubtaskType::LARGE_ROWSET_PART;
group.segment_start = segment_start;   // by segment array index, NOT by key range
group.segment_end   = segment_end;
```

Three sub-tasks each take ~445 segments of the input rowset and emit their own output rowset. The three output rowsets cover overlapping primary-key ranges (because consecutive segments in the input were never key-sorted across), so they are all marked `overlapped=true`.

cs is summed over rowsets in `RowsetCandidate::io_count()` (`be/src/storage/lake/primary_key_compaction_policy.cpp:24-64`):

```cpp
if (rowset_meta_ptr->overlapped()) {
    cnt = effective_segment_count;   // every overlapped output adds in full
}
```

Result observed in this run:

| Round | input rowsets | output rowsets | input segs | output segs | cs after publish |
|---|---|---|---|---|---|
| 1 (17187) | 1 | 1 (45 segs) | 1378 | 45 | 1158 |
| 2 (17615) | 1 | 3 (54 segs total) | 45 | 91 | 1659 |
| 3 (17997) | 1 | 3 (~50 segs) | 90 | 108 | 2158 |

The real PK-index `enable_lake_compaction_range_split` path (`tablet_parallel_compaction_manager.cpp:2438 _create_range_split_groups`) would have split by sort-key range instead and emitted non-overlapping output, but it's gated off by config:

```
enable_lake_compaction_range_split = false   (default)
```

`_can_use_range_split()` requires every segment to carry `sort_key_min` / `sort_key_max` metadata, which rtw_t10 does have because of the `ORDER BY` clause.

## Schema interactions that amplify the cost

### BUCKETS=2 makes each tablet ~100 GB

```sql
DISTRIBUTED BY HASH(contact_id) BUCKETS 2
```

→ 670 M rows / tablet × 2 tablets = 1.34 B rows in p1, ~100 GB / tablet (against ~5-10 GB / tablet for a reasonable lake-mode design). The 50 GB PK index per tablet is a direct consequence.

### ORDER BY disables load spill

`be/src/storage/lake/delta_writer.cpp:384` (`should_enable_load_spill`):

```cpp
return config::enable_load_spill &&
       (_tablet_schema->keys_type() != KeysType::PRIMARY_KEYS ||
        ((... merge_condition empty ...) &&
         !is_partial_update() &&
         !_tablet_schema->has_separate_sort_key()));   // ← false for rtw_t10
```

`rtw_t10`'s `ORDER BY(account_id, audience_id)` differs from `PRIMARY KEY(account_id, contact_id, id)`, so `has_separate_sort_key() == true` and load spill is bypassed. INSERT goes through `TabletWriterSink::flush_chunk`, which calls `_writer->flush(segment)` after every memtable flush — each ~100 MB memtable produces its own segment file, hence the 1378 segments observed per rowset on p1.

## Bottlenecks (ranked by elapsed-time impact)

1. **OOM retries in phase 1** — three rebuilds of round 1's vertical compaction cost ~2 hours by themselves. `compaction_max_memory_limit_percent=100` lets compaction consume the entire BE budget; bench INSERTs on top trip the 53 GB ceiling.
2. **LARGE_ROWSET_PART produces overlapping output** — drives cs back up after every vertical round, requiring phase 4-5 of pure PK-index housekeeping just to make cs converge. Range-split path exists but is disabled.
3. **Phase-4 quick PK-index merges are 90 % no-ops** — 16 sub-tasks per round, only 2-3 of them do real merging; 26 rounds = 31 min on shuffling SSTs that the eventual base merge consolidates anyway.
4. **Base-level merge fires too late** — only after cs has been pushed near the LARGE_ROWSET_PART floor; firing it once per vertical round (or after a fileset-count threshold) would skip phase 4 entirely.
5. **FE→BE dispatch starvation on heavy tablets** — txns 17615 and 17997 each sat in `PREPARE` 30-70 min with zero BE activity on tablet 23795 while small statistics-table compactions kept consuming the local slot. Suspected root cause is per-cluster slot accounting that doesn't reserve capacity per CN, but the FE side wasn't verified in this run.
6. **BUCKETS=2 / ORDER BY** — schema choices that 10× the cost of every round of compaction *and* of every INSERT.

## Configuration changes that should help (mutable, no restart)

| Setting | Default | Suggested | Effect |
|---|---|---|---|
| `compaction_max_memory_limit_percent` (BE) | 100 | 30-50 | Prevents the phase-1 OOM cascade |
| `enable_lake_compaction_range_split` (BE) | false | true | LARGE rowsets split by key range; output is non-overlapping; one round should drop cs to single digits |
| `lake_pk_index_sst_min_compaction_versions` (BE) | 2 | 5 | Lets PK-index SSTs accumulate before quick merges fire — fewer no-op rounds |
| `pk_index_compaction_score_ratio` (BE) | 1.5 | 3.0 | Same idea: defer quick merges until they have real work |
| `pk_index_parallel_compaction_task_split_threshold_bytes` (BE) | 32 MB | 128 MB | Smaller sub-task count → lower concurrent memory peak during pk_index merge |

Apply via:

```bash
for ip in 172.26.80.{101..106}; do
  curl -s -X POST "http://$ip:8040/api/update_config?compaction_max_memory_limit_percent=30"
  curl -s -X POST "http://$ip:8040/api/update_config?enable_lake_compaction_range_split=true"
  curl -s -X POST "http://$ip:8040/api/update_config?lake_pk_index_sst_min_compaction_versions=5"
  curl -s -X POST "http://$ip:8040/api/update_config?pk_index_compaction_score_ratio=3.0"
  curl -s -X POST "http://$ip:8040/api/update_config?pk_index_parallel_compaction_task_split_threshold_bytes=134217728"
done
```

## Schema changes (require rebuilding rtw_t10)

```sql
DROP TABLE rtw_t10;

CREATE TABLE rtw_t10 (...)
  PRIMARY KEY(account_id, contact_id, id)
  -- drop ORDER BY, or align it with PK columns to re-enable load spill
  PARTITION BY (mod(account_id, 10))
  DISTRIBUTED BY HASH(contact_id) BUCKETS 20;   -- 10x smaller tablets
```

Per-tablet cost shrinks roughly 10×: 670 M → 67 M rows, 100 GB → 10 GB, 50 GB PK index → 5 GB. A full p1 drain on this schema should finish in well under an hour.

## Time budget

| Scenario | Observed / estimated wall time |
|---|---|
| Today's run (default configs + bench-induced OOM) | 5.5 h |
| Default configs, no bench-induced OOM | ~3.5 h (saves the 2 h of OOM retries) |
| Above + `enable_lake_compaction_range_split=true` | ~1.5 h (single round consolidates, skip phases 4-5) |
| Above + BUCKETS=20 + aligned ORDER BY | ~30 min (per-tablet cost drops 10×) |

## Caveats and follow-ups

- The "stuck at cs=34.5" interpretation given in an earlier round of this analysis was **wrong**. Phase 5 was running the entire time, it just doesn't surface in `vertical_compaction_task.cpp:123` because base-level pk-index merges log through `lake_persistent_index_parallel_compact_mgr.cpp:354` instead.
- Phase 4's −148.5 cs/round constant has not been traced back to source; it almost certainly maps to a fixed contribution from one rowset's overlap count in `io_count()`.
- The FE-side dispatch starvation is observed but unconfirmed at the source level. Worth a separate investigation.
- The bench-induced OOM at 00:38 was caused by ingest landing on partitions that were already accumulating compaction backlog. Same outcome happens with any bench that targets p0-p9 before compaction has drained the initial expand load. Suggest a pre-bench "compaction quiesce" gate.
