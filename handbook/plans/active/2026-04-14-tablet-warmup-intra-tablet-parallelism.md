# Data Cache Warmup: Intra-Tablet Parallelism

- Status: active (design / RFC)
- Owner: TBD
- Last Updated: 2026-04-14

## Summary

Today, Data Cache Warmup is driven by `CACHE SELECT`, which translates to an
`INSERT INTO BLACKHOLE() SELECT ...` query and reuses the normal scan path to
populate the block cache (see `DataCacheSelectExecutor.cacheSelect`,
`fe/fe-core/src/main/java/com/starrocks/datacache/DataCacheSelectExecutor.java:45`).
Parallelism is bounded by the query's scan parallelism, which in shared-data
mode is effectively **per-tablet**: one tablet is assigned to one scan
operator, which serially walks rowsets/segments and pulls bytes through the
scanner into the block cache.

Because a single shared-data tablet can grow to ~100 GB and contains hundreds
of segments behind one or more rowsets, a single warmup scanner pulling that
tablet from object storage is the dominant tail in any warmup job. The goal of
this plan is to introduce **intra-tablet parallelism** so a single 100 GB
tablet can be warmed by N concurrent workers fetching disjoint segment (or
file-range) sub-units in parallel.

This document is the initial design and trade-off study; it does not yet
prescribe code.

## Problem Statement

### Current behaviour

1. FE: `CACHE SELECT` builds an `INSERT INTO BLACKHOLE()` plan and dispatches
   one execution per compute resource
   (`DataCacheSelectExecutor`, lines 45-104). Tablet selection is implicit
   through the optimizer.
2. FE planner: the lake scan node assigns each tablet to a single scan range /
   fragment instance. The shared-nothing `enable_tablet_internal_parallel`
   knob (`SessionVariable`, see also
   `test/sql/test_tablet_internal_parallel/`) splits a tablet by row-key
   ranges, but it is OLAP-only and is not active for cloud-native (lake)
   tablets.
3. BE: the lake connector
   (`be/src/connector/lake_connector.cpp`) opens the tablet, iterates
   rowsets, and reads segments serially through the segment iterator. The
   block cache is populated as a side-effect of remote reads on the
   `RemoteFileSystem` path.
4. There is **no dedicated warmup task type, no warmup RPC, and no warmup
   thread pool** on BE. There is no message in
   `gensrc/proto/internal_service.proto` for explicit tablet warmup.

### Why this hurts at 100 GB / tablet

- Wall-clock warmup time for a warehouse is `max(per-tablet read time)` over
  all tablets, not the average. A single 100 GB tablet that streams at, say,
  ~250 MB/s per scanner takes >6 minutes; meanwhile the rest of the cluster
  is idle waiting for the long-pole tablet.
- Object-storage throughput per tablet is throttled by the number of
  concurrent connections we issue. A single scanner uses a small, bounded
  fan-out; we leave aggregate bandwidth on the table.
- Warmup bytes go through the full scan stack (column readers, predicate
  evaluation, chunk assembly, sink). For warmup we only need the bytes in
  cache — this stack is overhead.

### Natural sub-units inside a tablet

- **Tablet → Rowset → Segment → Data file → Block.**
- Segment counts: a 100 GB tablet with default ~100-256 MB segments has on
  the order of 400-1000 segment files. Rowsets number in the dozens.
- The block cache itself stores fixed-size blocks (configurable, default
  ~1 MB). Cache key = `hash(filename) + mtime + blockId` (see
  `docs/en/data_source/data_cache.md`). Population is therefore safely
  idempotent at block granularity.
- Precedent: `Rowset` already has a constructor that takes
  `(rowset_index, segment_start, segment_end)` for "large rowset split
  compaction" (`be/src/storage/lake/rowset.h:55-64`). The same shape works
  for warmup sub-tasks.

## Goals & Non-Goals

**Goals**

- Allow N workers to warm one tablet concurrently, where N is bounded by a
  config knob (per CN and per tablet).
- Keep warmup idempotent and safe under retries; no double-billed cache
  population.
- Surface progress and bytes-cached metrics per sub-task.
- Work for cloud-native (lake) tablets first; OLAP can come later.

**Non-Goals**

- Replace `CACHE SELECT` semantics or the FE `DataCacheSelectExecutor`
  user-facing surface.
- Change the on-disk block cache layout or eviction policy.
- Cross-CN tablet sharding (one tablet's primary CN still owns it).

## Design Options

The two real questions are **(A) where to introduce parallelism** and
**(B) what code path issues the IO**. They are independent.

### (A) Sub-task granularity

| Option | Unit | Sub-tasks per 100 GB tablet | Notes |
|---|---|---|---|
| A1 | Rowset | 10s | Coarse; uneven if one rowset is huge. |
| A2 | Segment | 100s-1000s | Best alignment with current `Rowset(seg_start, seg_end)` API; segment files are immutable and independently readable. |
| A3 | File byte-range | 1000s-10000s | Finest; matches block-cache block size. Requires range-aware fetcher. |

**Recommendation: A2 (segment-level).** It maps to existing storage
abstractions, gives 100x+ parallelism for the worst case, and aligns
naturally with the block-cache key (per-file). A3 is a future refinement
that helps only when an individual segment is itself a tail (rare under
default segment-flush sizing).

### (B) Execution path

| Option | Path | Pros | Cons |
|---|---|---|---|
| B1 | Keep `INSERT INTO BLACKHOLE()`; teach the planner to split tablets into multiple scan ranges by segment range | Reuses entire scan/scheduler stack; user surface unchanged. | Couples warmup parallelism to query DOP; still pays for column-reader/chunk/sink overhead; lake scan node does not currently emit sub-tablet ranges. |
| B2 | New explicit `WarmupTablet` RPC on BE, dispatched by FE in addition to (or instead of) `CACHE SELECT` | Bypasses scan stack; a dedicated `warmup_thread_pool` controls concurrency; clean per-sub-task metrics; can prefetch raw blocks instead of materialising chunks. | New surface (proto + RPC + executor); needs progress reporting and crash recovery. |
| B3 | Hybrid: keep `CACHE SELECT` as the user entry point; FE expands one tablet into K scan ranges (segment ranges) and dispatches them like ordinary instances. | Smallest user-visible change; reuses `Coordinator`. | Inherits scan-stack overhead; uneven splits unless the planner reads segment metadata. |

**Recommendation: start with B2, layered behind the existing
`CACHE SELECT` user surface.** `DataCacheSelectExecutor` decides per
tablet whether to (a) ship a normal scan (small tablets) or (b) issue
`WarmupTablet` RPCs in parallel to the owning CN (large tablets, gated
by `tablet_size > warmup_split_threshold_bytes`). This isolates the new
machinery while preserving today's API.

### Proposed pieces (B2 + A2)

1. **Proto** (`gensrc/proto/internal_service.proto`):

   ```proto
   message PWarmupTabletSubtask {
       optional uint32 rowset_index = 1;
       optional int32  segment_start = 2;   // inclusive
       optional int32  segment_end   = 3;   // exclusive
   }

   message PWarmupTabletRequest {
       optional int64 tablet_id = 1;
       optional int64 version   = 2;
       repeated PWarmupTabletSubtask subtasks = 3;
       optional int32 priority  = 4;
       optional int64 ttl_seconds = 5;
       optional string trace_id = 6;
   }

   message PWarmupTabletResponse {
       optional StatusPB status = 1;
       optional int64 bytes_read   = 2;
       optional int64 bytes_cached = 3;
       optional int64 bytes_skipped = 4;  // already in cache
       optional int64 elapsed_ns   = 5;
   }
   ```

   Stay with `optional`/`repeated` per repo invariants.

2. **FE planner / dispatcher**:
   - Extend `DataCacheSelectExecutor` (or a new
     `TabletWarmupDispatcher`) to consult tablet size/segment count from
     the lake metadata cache.
   - For each "large" tablet, build K subtasks by even-segment partition
     across rowsets; pack into one or more `PWarmupTabletRequest`s sent
     to the tablet's primary CN.
   - Aggregate `PWarmupTabletResponse` into the existing
     `DataCacheSelectMetrics` so the user-facing output is unchanged.

3. **BE executor**:
   - New `DataCacheWarmupExecutor` owning a `PriorityThreadPool`
     `datacache_warmup_thread_pool` (config:
     `datacache_warmup_thread_pool_num_threads`,
     `datacache_warmup_max_concurrent_subtasks_per_tablet`).
   - For each subtask, build a `Rowset(tablet_metadata, rowset_index,
     seg_start, seg_end)` and walk its segment files. Two flavours:
     - **Raw fetch (preferred):** open the segment file via the lake
       filesystem with `populate_datacache=true` and stream-read its
       byte ranges into the cache without materialising chunks. The
       block cache uses `(filename, mtime, blockId)` as key, so populate
       is naturally idempotent and safe to run concurrently.
     - **Iterator fetch (fallback):** use `Rowset::read` /
       `get_each_segment_iterator` with predicates disabled and a
       `Sink::DiscardSink`. Slower but reuses well-tested code.
   - Skip-if-cached check uses the existing
     `BlockCache::contains`-style probe to avoid re-fetching warm
     blocks (cheap with `bytes_skipped` accounting for visibility).

4. **Idempotency & retry**: subtasks key off
   `(tablet_id, version, rowset_index, segment_range)`. Re-issuing the
   same subtask is safe — block-cache writes collapse on the existing
   key.

5. **Backpressure**: per-tablet concurrency cap (`<= K_per_tablet`) to
   avoid object-storage throttling; per-CN cap on
   `datacache_warmup_thread_pool`. Both default conservatively (e.g.,
   `K_per_tablet = 8`, pool size = `min(32, 2 * num_disks)`).

## Trade-offs

| Decision | Choice | Rejected alternative | Why |
|---|---|---|---|
| Sub-task unit | Segment range (A2) | Rowset (A1) / file byte-range (A3) | Best fit with existing `Rowset` ctor; 100x+ parallelism for the worst tablet without engineering a range-aware fetcher. |
| Execution path | Dedicated RPC + thread pool (B2) | Reuse query path (B1/B3) | Avoids scan-stack overhead; isolates concurrency; clean metrics; doesn't require the lake scan node to emit sub-tablet ranges. |
| User surface | Keep `CACHE SELECT` | New `WARMUP TABLET` DDL | Zero migration; advanced users can still target tablets via predicates. |
| Splitter location | FE | BE | FE already knows tablet metadata and owns dispatch fan-out; BE just executes what it's handed. |
| Read mode | Raw byte-range fetch | Full scan iterator | Saves CPU and avoids predicate / column-reader work that warmup doesn't need. |
| Concurrency knobs | Per-tablet **and** per-CN caps | Single global cap | Per-tablet cap protects against object-storage throttling; per-CN cap protects against pool starvation under multi-tablet warmup. |
| Idempotency | Rely on existing block-cache key | Build a coordination table | Block-cache key already encodes `(file, mtime, block)`; redundant writes collapse safely. |

### Risks worth flagging

- **Object-storage throttling.** A naive 32-way fan-out per tablet can
  trigger S3/OSS request-rate caps. Mitigation: per-tablet concurrency
  cap + jittered start; expose `bytes_throttled` metric.
- **Memory footprint.** Each subtask opens segment readers; 1000
  concurrent subtasks per CN would dominate heap. Mitigation: cap
  outstanding subtasks at the pool, queue the rest.
- **Skewed segment sizes.** Even segment-count partition can still be
  uneven by bytes. Mitigation: FE splitter uses `segment_size` from
  `RowsetMetadataPB` (already present) for byte-balanced partition.
- **Version drift during warmup.** Tablet metadata may advance between
  subtask dispatch and execution. Mitigation: pin
  `(tablet_id, version)` per request; on mismatch, surface as a
  retriable subtask error and let FE rebuild the plan.
- **Compaction interaction.** Warming segments that compaction is
  about to delete is wasted work. Mitigation: cheap pre-check that
  the rowset is still present in the latest tablet metadata; a stale
  hit just costs a probe.

## Open Questions

1. Should `WarmupTablet` honour the existing `enable_populate_datacache`
   session flag, or always force-populate (CACHE SELECT semantics)?
2. Do we want per-subtask cancellation hooks routed through the existing
   `CACHE SELECT` `ctrl-c` path?
3. Is it worth a small persistence layer (FE-side) so a long warmup can
   resume after FE failover, or is "user re-runs `CACHE SELECT`" good
   enough?
4. Should warmup priority share the data-cache `priority`/`TTL`
   semantics or grow its own?

## Acceptance Criteria

- A `CACHE SELECT` over a 100 GB tablet finishes in
  `~ tablet_size / (K_per_tablet * per-subtask-bandwidth)` instead of
  `~ tablet_size / per-subtask-bandwidth`.
- New BE config `datacache_warmup_thread_pool_num_threads` and
  `datacache_warmup_max_concurrent_subtasks_per_tablet` documented in
  `docs/en/administration/management/BE_configuration.md` (and zh peer).
- New metrics for `bytes_read`, `bytes_cached`, `bytes_skipped`,
  `subtasks_in_flight` documented in
  `docs/en/administration/management/monitoring/metrics.md`.
- New proto fields stay `optional`/`repeated`; no reused ordinals.
- Unit tests: subtask splitting, idempotent re-dispatch, version
  mismatch, throttling, and skip-if-cached.
- Integration: SQL test under `test/sql/test_data_cache_warmup/` that
  exercises a multi-segment tablet end-to-end.

## Decision Log

- 2026-04-14: Initial design. Chose segment-range subtasks (A2) over
  rowset (A1) and byte-range (A3); chose dedicated RPC + thread pool
  (B2) over reusing the scan path (B1/B3). Kept `CACHE SELECT` as the
  user entry point.
