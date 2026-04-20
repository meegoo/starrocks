# Data Cache Populate: Why Large Tablets Don't Need Extra Parallelization

> 中文版本：[2026-04-14-intra-tablet-warmup-parallelism.md](./2026-04-14-intra-tablet-warmup-parallelism.md)

- Status: **archived** (decision record, not implementing)
- Last Updated: 2026-04-16
- Original motivation: attempted to solve "CACHE SELECT becomes a long-tail bottleneck on 100GB tablets because a single scanner reads serially"
- Conclusion: **Existing mechanisms already provide sufficient parallelism. The real bottleneck is resource limits, not parallelism.**

## 0. TL;DR

If a developer sees "data cache warmup on large tablets seems slow" and wants to build a new parallelization subsystem — read this first. The conclusion:

1. In Lake mode, every data cache populate path is already naturally parallel (scan DOP, tablet internal parallel, async write-back).
2. Since v3.4, CACHE SELECT inherits tablet internal parallel by default — no new splitting mechanism needed.
3. The real bottleneck is **resource limits** (object storage bandwidth, local SSD IOPS, cache capacity). Adding parallelism would only hit the limits sooner.
4. If users find CACHE SELECT slow, tuning two session variables is enough (`tablet_internal_parallel_mode='force_split'` + `pipeline_dop`).
5. The only code change worth considering is **auto-setting `force_split` in `DataCacheSelectExecutor` (~5-10 lines of Java)** — not a new thread pool, new scan range field, or new TabletReader grouping logic.

## 1. The Problem That Looked Real

Lake tablets can reach 100GB. At a single S3 connection throughput of 150-300 MB/s, reading one serially takes 5-11 minutes. CACHE SELECT waits synchronously — if it's truly "one tablet, one scanner serial", a single large tablet becomes the long-tail bottleneck for the whole warmup.

The reasoning looks sound, but two premises need verification:

- (P1) Is the parallelism in existing populate paths actually sufficient?
- (P2) Is CACHE SELECT really "one tablet, one scanner"?

Code review shows **(P1) yes, (P2) no**.

## 2. Current State of Parallelization Across Populate Paths

Every path in StarRocks Lake mode that writes to the local data cache:

| Trigger | Code Entry | Populate Timing | Parallelism Source |
|---------|-----------|-----------------|-------------------|
| **Write (TabletWriter)** | `fs_starlet.cpp:383`, `WritableFileOptions.skip_fill_local_cache=false` (default) | Starlet fills local cache on write | Multi-tablet × multi-segment-writer |
| **Compaction output** | Same (compaction uses TabletWriter too) | Same | Compaction worker pool + multi-tablet concurrency |
| **Compaction input reads** | `horizontal_compaction_task.cpp:62`, `lake_io_opts.fill_data_cache=true` | Piggyback populate on read | Multiple segment iterators within compaction + range-split subtasks |
| **Normal query** | `cache_input_stream.cpp:_populate_to_cache` + `options.async=true` (default since 3.3+) | Read remote on miss + async write to cache | Scan DOP (fragment × tablet × tablet-internal-parallel morsels × per-column IO) |
| **CACHE SELECT** | `segment_iterator.cpp:2048-2091` `cache_file_only` path | Explicit touch_cache, no decoding | **Inherits normal query's tablet internal parallel** (see §3) |
| **Peer cache → local** | `cache_input_stream.cpp:147-163` | On local miss, query peer; populate local on peer hit | Same as normal query |

**Key facts:**

1. **Newly written data from writes and compaction is already in local cache on the producing CN** (starlet write-through, `fs_starlet.cpp:383`: `fslib_opts.skip_fill_local_cache = opts.skip_fill_local_cache`, default `false`).
2. **Normal query populate is async + best-effort**, piggybacking on the read (`cache_input_stream.cpp:442-501`).
3. **`datacache_max_concurrent_inserts` defaults to 1.5M**, and `ResourceBusy` is swallowed by `_can_ignore_populate_error` — the query is unaffected (`cache_input_stream.cpp:503-509`).

## 3. CACHE SELECT Already Has Intra-Tablet Parallelism

This is the easiest thing to misjudge. Evidence chain:

### 3.1 `DataCacheSelectExecutor` Does NOT Disable Tablet Internal Parallel

`fe/fe-core/.../datacache/DataCacheSelectExecutor.java:148-161` overrides only these 7 session variables:

```java
sessionVariable.setCatalog(...);
sessionVariable.setEnableScanDataCache(true);
sessionVariable.setEnablePopulateDataCache(true);
sessionVariable.setDataCachePopulateMode(DataCachePopulateMode.ALWAYS.modeName());
sessionVariable.setEnableDataCacheAsyncPopulateMode(false);
sessionVariable.setEnableDataCacheIOAdaptor(false);
sessionVariable.setDataCacheEvictProbability(100);
sessionVariable.setDataCachePriority(...);
sessionVariable.setDatacacheTTLSeconds(...);
sessionVariable.setEnableCacheSelect(true);
```

**Does NOT include** `enable_tablet_internal_parallel` or `enable_lake_tablet_internal_parallel` — inherited from the user's session.

### 3.2 `enable_lake_tablet_internal_parallel` Defaults to `true` (3.4+)

From `release-3.4.md:119`:

> By setting the default value of `enable_lake_tablet_internal_parallel` to `true`, Parallel Scan for Cloud-native tables in shared-data clusters is enabled by default to increase per-query internal parallelism.

### 3.3 The Splitting Mechanism Flows End-to-End

```
FE sends enable_tablet_internal_parallel=true
  ↓
LakeDataSourceProvider::_could_tablet_internal_parallel (lake_connector.cpp:1250-1296)
  ↓ when conditions met, split
PhysicalSplitMorselQueue (rowid-range split, one morsel per pipeline driver)
  ↓
Each morsel creates its own LakeDataSource → TabletReader → SegmentIterator
  ↓
SegmentIterator::_init() → _get_row_ranges_by_rowid_range()
  ↓  _scan_range &= rowid_range_option  (segment_iterator.cpp:3517)
cache_file_only path uses the filtered _scan_range to call get_io_range_vec
  ↓  (segment_iterator.cpp:2065)
get_io_range_vec(_scan_range, ...) returns only the page byte ranges for this morsel's rowids
  ↓  (scalar_column_iterator.cpp:738-779)
touch_cache(offset, size) populates only this morsel's subset of data
```

**Different morsels' rowid ranges map to different page byte ranges → different cache blocks → parallel populate across morsels with no conflict and no duplicate IO.**

### 3.4 Conclusion

In 3.4+ Lake mode, CACHE SELECT on a large tablet **already auto-splits into rowid-range morsels and warms up in parallel**. The "one tablet, one scanner" premise is wrong.

## 4. The Real Bottleneck: Resource Limits

Large tablets can legitimately be "slow", but the root cause is not parallelism:

| Symptom | Real Root Cause | Existing Mitigation |
|---------|----------------|--------------------|
| High first-query latency | Cold data, remote IO bandwidth | Pre-warm via CACHE SELECT |
| Cache pollution (big query evicts hot data) | Limited cache capacity | SLRU eviction + `DataCachePopulateMode` (AUTO/NEVER/ALWAYS) + `datacache_evict_probability` + priorities |
| Partial populate loss | `datacache_max_concurrent_inserts`, capacity, SSD write bandwidth throttling | `_can_ignore_populate_error` swallows `ResourceBusy`/`CapacityLimitExceeded`/`AlreadyExist`; query doesn't fail |
| Still cold on repeat queries | Cumulative effect of the above (populates dropped) | CACHE SELECT provides synchronous, full-coverage warmup |
| Object storage bandwidth saturated | Physical limit | Horizontally scale CN count |

**Adding parallelism solves none of these** — more parallelism just hits the same resource ceilings faster.

## 5. A Small Remaining Gap in CACHE SELECT (But No New Mechanism Needed)

The trigger condition in `_could_tablet_internal_parallel` (`lake_connector.cpp:1254-1256`):

```cpp
bool force_split = tablet_internal_parallel_mode == TTabletInternalParallelMode::type::FORCE_SPLIT;
if (!force_split && num_total_scan_ranges >= pipeline_dop) {
    return false;
}
```

Reasonable for normal queries (when tablet count ≥ DOP, CPU parallelism is already filled), but too conservative for CACHE SELECT:

|  | Normal Query | CACHE SELECT |
|---|-------------|--------------|
| What matters | CPU utilization | IO wall time (single tablet is the long tail) |
| DOP is already filled but single tablet is huge | Doesn't matter | **Still needs splitting** |

### 5.1 Knobs Users Can Turn Today

If CACHE SELECT is genuinely slow, no code changes are needed:

- `SET tablet_internal_parallel_mode = 'force_split'` — bypasses the `num_total_scan_ranges >= pipeline_dop` check
- `SET pipeline_dop = <larger>` — raises the overall parallelism ceiling
- `SET enable_lake_tablet_internal_parallel = true` — only needed on 3.3 or when explicitly disabled

### 5.2 Optional Small Improvement (if manual tuning is too much)

The only code change worth doing: make `DataCacheSelectExecutor` auto-set `force_split` (~5-10 lines of Java).

```java
// DataCacheSelectExecutor.buildCacheSelectConnectContext()
sessionVariable.setTabletInternalParallelMode("force_split");
```

Side effect: CACHE SELECT splits morsels more aggressively. Benefit: removes the tablet-count-≥-DOP conservative gate under CACHE SELECT.

**This is not in scope for this plan. If anyone wants to do it, open a separate ~10-line PR.**

## 6. What NOT to Do

Wrong turns taken during investigation, recorded here so future readers don't repeat them:

- ❌ **Add a `cache_warmup_thread_pool`**: The pipeline driver executor + scan task queue is already the generic infrastructure for this. A CACHE-SELECT-only pool would be reinventing the wheel.
- ❌ **Add `TInternalScanRange.cache_warmup_parallelism` thrift field**: `enable_tablet_internal_parallel` + `tablet_internal_parallel_mode` already express the same intent.
- ❌ **Add another parallel-grouping layer inside `TabletReader::init_collector`**: rowid-range morsel splitting + pipeline driver parallelism already do this. Adding another layer would conflict with the morsel mechanism.
- ❌ **Split scan ranges on the FE side**: The FE has no segment metadata (`LakeTablet.java` only holds aggregate `dataSize`; fetching metadata requires an RPC). Also, splitting one tablet into multiple scan ranges causes Coordinator anchoring issues and SST warmup duplication.
- ❌ **Parallel re-warmup for compaction output**: Starlet's default `skip_fill_local_cache=false` already fills cache on the compaction worker CN during writes. There is no "cache gap after compaction" problem.

## 7. Guidance for Anyone Facing Similar Symptoms

If you see "CACHE SELECT seems slow" or "warmup on large tablets has a long tail", troubleshoot in this order:

1. **Confirm version**: is it 3.4+ and is `enable_lake_tablet_internal_parallel` true? If not, enable it first.
2. **Check the profile**: in the query profile, is morsel/driver count > 1 per tablet? If it's 1, the split didn't trigger.
3. **Confirm it hit the "tablet count ≥ DOP" gate**: just `SET tablet_internal_parallel_mode = 'force_split'` and re-run to see the effect.
4. **Physical bottleneck**: if splitting already happened and it's still slow, look at object storage egress bandwidth, CN local SSD write bandwidth, and `datacache_max_concurrent_inserts` metrics. These are capacity problems, not code problems.
5. **Were populates dropped?**: check the `skip_write_cache_count` and `write_cache_fail_count` counters. If many were dropped, you hit a capacity/concurrency/bandwidth throttle — scale out or reduce load.

**Don't jump to "write a new mechanism" as the first reaction.** At least 90% of "slow warmup" is configuration + capacity, not code.

## 8. Key Code References

| Component | File | Key Lines |
|-----------|------|-----------|
| FE CACHE SELECT entry | `fe/fe-core/.../datacache/DataCacheSelectExecutor.java` | 45-104 (execution), 148-161 (session overrides) |
| FE CACHE SELECT analyzer | `fe/fe-core/.../sql/analyzer/DataCacheStmtAnalyzer.java` | 128-171 |
| FE LakeTablet (aggregate metadata only) | `fe/fe-core/.../lake/LakeTablet.java` | 42-211 |
| FE OlapScanNode.addScanRangeLocations | `fe/fe-core/.../planner/OlapScanNode.java` | 553-709 |
| `enable_lake_tablet_internal_parallel` default true | 3.4 release notes | - |
| BE starlet write-through cache | `be/src/fs/fs_starlet.cpp` | 383 |
| BE `WritableFileOptions.skip_fill_local_cache` default false | `be/src/fs/fs.h` | 281 |
| BE compaction input fills cache | `be/src/storage/lake/horizontal_compaction_task.cpp` | 62-63 |
| BE `CacheInputStream` async populate | `be/src/io/cache_input_stream.cpp` | 442-509 |
| BE `_can_ignore_populate_error` (swallows ResourceBusy) | `be/src/io/cache_input_stream.cpp` | 503-509 |
| BE `cache_file_only` path | `be/src/storage/rowset/segment_iterator.cpp` | 2048-2091 |
| BE SST warmup atomic guard | same | 2080-2086 |
| BE `_get_row_ranges_by_rowid_range` | `be/src/storage/rowset/segment_iterator.cpp` | 3511-3517 |
| BE `get_io_range_vec` (maps rowid range to bytes) | `be/src/storage/rowset/scalar_column_iterator.cpp` | 738-779 |
| BE LakeDataSource sets `cache_file_only` | `be/src/connector/lake_connector.cpp` | 329-331 |
| BE Lake tablet internal parallel trigger logic | `be/src/connector/lake_connector.cpp` | 1250-1296 |
| BE `datacache_max_concurrent_inserts` default 1.5M | `be/src/common/config.h` | 1465 |
| BE `datacache_evict_probability` / SLRU config | `be/src/common/config.h` | 1456-1520 |

## 9. Wrong Turns During the Investigation (for Reference)

1. **Original plan B2 (FE splits scan ranges)**: Built on the wrong premise that "FE holds tablet metadata" — FE only has aggregate `dataSize`, no rowset/segment list. Making it work would require a new metadata RPC, which is architecturally intrusive.
2. **Revised plan B3+ (BE self-splits + FE hint + new thread pool)**: Solved the metadata problem but didn't know rowid-range morsels already existed — ended up reinventing a parallel executor.
3. **Plan C (tune tablet internal parallel's trigger condition)**: Close to the right answer, but didn't fully realize existing session variables could already do it.
4. **Final conclusion (this document)**: No new code is needed. Existing mechanisms suffice; tuning session variables is enough. If manual tuning is unfriendly, a 5-10 line Java change in `DataCacheSelectExecutor` to auto-set `force_split` is the only thing worth doing.

The meta-lesson from this winding path: **always thoroughly understand existing code before proposing a design — otherwise you'll build a new mechanism for a problem that's already solved.**
