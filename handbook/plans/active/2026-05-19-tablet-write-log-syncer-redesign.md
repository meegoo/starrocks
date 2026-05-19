# TabletWriteLogHistorySyncer Redesign

- Status: active
- Owner: lake storage / observability
- Last Updated: 2026-05-19

## Summary

`TabletWriteLogHistorySyncer` was patched in `dde326d7` to fix an empirically-observed
~82% data-loss rate at 500 ops/sec. The fix migrated the persistent table to PRIMARY
KEY mode and switched the sync watermark from `finish_time > MAX` to
`finish_time >= MAX - 120s` with PK-side dedup. While that resolves the most obvious
symptom, a review found seven additional gaps in the implementation. This plan
redesigns the syncer around **per-BE persistent watermarks + adaptive overlap +
full observability**, plus follow-up BE-side work to push down predicates and assert
PK uniqueness at the source.

The goal: a sync pipeline that is correct under multi-BE clock skew, can self-heal
from transient failures up to a bounded interruption, and is observable enough that
operators can detect data loss before users do.

## Background

### Symptom
- 60-minute, 500 ops/sec bench: 133k LOAD entries in BE in-memory ring vs. only
  24k in `_statistics_.tablet_write_log_history` → ~82% loss.
- Original sync SQL used `finish_time > (SELECT MAX(finish_time) FROM history)`.

### Stated root cause in `dde326d7`
- `finish_time` is second-precision; strict `>` drops every entry in the second
  matching the previous batch's max.

### Real root cause (reanalyzed)
The same-second-boundary explanation accounts for ~1.7% loss, not 82%. The
amplifying factors are:

1. **Cluster-wide MAX across heterogeneous BEs.** `SELECT MAX(finish_time) FROM
   history` is a cluster-level scalar. If a fast BE pulls the cluster MAX to T,
   a slow BE that still has entries with `finish_time < T` in its local ring
   has them all skipped on the next round.
2. **Sync jitter + ring overflow.** BE ring capacity (`tablet_write_log_buffer_size`,
   default 100,000) holds ~200s at 500 ops/sec. If sync interval drifts (FE GC, slow
   SQL, scheduler lag) and the gap exceeds ring capacity, the oldest entries are
   evicted before sync can pull them.
3. **Bootstrap empty-set + freshness buffer.** First sync at `T=60s` with
   `freshness=60s` produces an empty range; MAX stays at the sentinel until the
   second pass, and ring may already have overflowed by then.

The `dde326d7` PK + overlap fix mitigates all three but leaves observability,
performance, and edge cases unaddressed.

## Identified Issues After `dde326d7`

| # | Severity | Issue |
|---|---|---|
| 1 | High (perf) | BE `schema_be_tablet_write_log_scanner.cpp` lacks predicate pushdown; every sync ships the entire ring (~100k rows) to FE for filtering. |
| 2 | Medium (correctness) | 120s overlap vs. 200s ring capacity leaves only 80s of safety margin; two consecutive sync failures (~180s) can push data outside the recoverable window. |
| 3 | Medium (correctness) | Cluster-wide MAX still lets fast BEs "blind" slow ones beyond the 120s overlap. |
| 4 | Medium (correctness) | PK uniqueness on `(be_id, finish_time, txn_id, tablet_id, log_type)` depends on a BE-side convention (one `add_*_log` per event); no test or assertion enforces it. |
| 5 | Medium (perf) | Even with adequate ring sizes, each round re-scans 120s of overlap; transmission cost scales with `overlap × ops_sec × num_BEs`. |
| 6 | Low (perf) | `SELECT MAX(finish_time) FROM history` runs each round against the partitioned PK history table; cheap but not zero. |
| 7 | High (operability) | No metrics for sync lag, rows inserted/skipped, BE ring overflow count, or sync failures. Operators cannot detect data loss. |

Plus three schema-property fixes still pending from review:

- Missing `'replication_num' = '1'` (single-BE clusters can't CREATE TABLE).
- Hard-coded `'enable_persistent_index' = 'true'` (conflicts with shared-data PK
  cloud-native index defaults).
- Hard-coded `partition_live_number = 7` (not user-configurable).

## Design

### Phase 1: FE-side (one PR)

#### 1.1 New persistent cursor table

```sql
CREATE TABLE _statistics_.tablet_write_log_sync_state (
  be_id BIGINT NOT NULL,
  max_finish_time DATETIME NOT NULL,
  overlap_seconds INT NOT NULL,
  failure_count INT NOT NULL,
  last_round_started_at DATETIME NOT NULL,
  last_round_rows BIGINT NOT NULL
)
PRIMARY KEY (be_id)
DISTRIBUTED BY HASH(be_id) BUCKETS 3
PROPERTIES ('replication_num' = '1');
```

- One row per BE. Survives FE restart → fixes #6 (no more `MAX(finish_time)` full
  scan against history).
- Per-BE watermark → fixes #3 (fast BEs no longer mask slow BEs).

#### 1.2 Single-statement sync that joins the cursor table

```sql
INSERT INTO _statistics_.tablet_write_log_history (col_list...)
SELECT log.col_list...
FROM information_schema.be_tablet_write_log log
LEFT JOIN _statistics_.tablet_write_log_sync_state ss
       ON log.be_id = ss.be_id
WHERE log.finish_time >= COALESCE(
        DATE_SUB(ss.max_finish_time, INTERVAL ss.overlap_seconds SECOND),
        '0001-01-01 00:00:00')
  AND log.finish_time < NOW() - INTERVAL 30 SECOND;
```

- Single statement covers all BEs; no FE-side loop.
- Each BE filters by its own watermark and overlap.
- BEs without a cursor row use the sentinel + default overlap (first sync).

#### 1.3 Upsert the cursor table after each successful sync

```sql
INSERT INTO _statistics_.tablet_write_log_sync_state
SELECT
    be_id,
    MAX(finish_time)             AS max_finish_time,
    {computed_overlap}           AS overlap_seconds,
    0                            AS failure_count,
    NOW()                        AS last_round_started_at,
    COUNT(*)                     AS last_round_rows
FROM _statistics_.tablet_write_log_history
WHERE finish_time >= NOW() - INTERVAL 10 MINUTE
GROUP BY be_id;
```

PK-mode INSERT is upsert; the row for each BE is updated in place.

#### 1.4 Adaptive overlap

```java
private static final int OVERLAP_BASE_SEC = 120;
private static final int OVERLAP_MAX_SEC  = 600;

int computeOverlap(int failureCount, int syncIntervalSec) {
    int byFailure  = (1 + failureCount) * syncIntervalSec;
    int byInterval = syncIntervalSec * 3;
    return Math.min(OVERLAP_MAX_SEC,
                    Math.max(OVERLAP_BASE_SEC, Math.max(byFailure, byInterval)));
}
```

- Success path: overlap stays at base (`120s`).
- Failure path: grows linearly with `failure_count`, capped at `OVERLAP_MAX_SEC`.
- Once a sync succeeds, `failure_count = 0` resets overlap. Closes #2's safety
  margin issue with bounded recovery.

#### 1.5 Failure-path cursor update

When the INSERT throws:

```sql
INSERT INTO _statistics_.tablet_write_log_sync_state
SELECT
    be_id,
    max_finish_time,
    LEAST({OVERLAP_MAX_SEC}, overlap_seconds + {SYNC_INTERVAL}) AS overlap_seconds,
    failure_count + 1 AS failure_count,
    NOW() AS last_round_started_at,
    last_round_rows
FROM _statistics_.tablet_write_log_sync_state;
```

Or maintain `failure_count` in-memory and persist on next success.

#### 1.6 Metrics

Register in `MetricRepo`:

| Metric | Type | Description |
|---|---|---|
| `tablet_write_log_sync_rows_inserted_total` | Counter | Cumulative successful inserts. |
| `tablet_write_log_sync_rows_skipped_dup_total` | Counter | `rows_pulled - rows_inserted` (PK dedup). |
| `tablet_write_log_sync_failures_total` | Counter | Sync round failures. |
| `tablet_write_log_sync_last_success_ts` | Gauge | Unix epoch of last successful round. |
| `tablet_write_log_sync_lag_seconds_max` | Gauge | `NOW - MIN(max_finish_time)` over all BEs. |
| `tablet_write_log_sync_overlap_seconds_max` | Gauge | Largest current overlap (catches stuck-in-recovery). |
| `tablet_write_log_sync_be_failure_count_max` | Gauge | Max `failure_count` across BEs. |

Document in `docs/en/administration/management/monitoring/metrics.md`.

#### 1.7 Schema property fixes

Roll the three pending fixes into the same PR:

- Add `'replication_num' = '1'`.
- Remove `'enable_persistent_index' = 'true'` (rely on `enable_persistent_index_by_default`
  / `enable_cloud_native_persistent_index_by_default`).
- Replace hard-coded `partition_live_number = 7` with
  `Config.tablet_write_log_history_retained_days` (default 7, `@ConfField(mutable = true)`).
  Doc update: `docs/en/administration/management/FE_configuration.md`.

### Phase 2: BE-side (separate PRs, can land independently)

#### 2.1 Predicate pushdown for `be_tablet_write_log` (#1 + #5)

`schema_be_tablet_write_log_scanner.cpp` currently scans the full ring:

```cpp
// TODO: Support predicate pushdown filtering
_logs = lake::TabletWriteLogManager::instance()->get_logs();
```

`TabletWriteLogManager::get_logs(table_id, partition_id, tablet_id, log_type,
start_finish_time, ...)` already accepts the filter args. The scanner needs to:

1. Inspect `_param->_conjunct_ctxs` for `finish_time >= <const>`, `be_id = <const>`,
   etc.
2. Translate them to the matching `get_logs` arguments.
3. Fall back to full scan if conjuncts can't be pushed.

Effect: per-round payload drops from ~100k rows to ~30k (new rows only).

#### 2.2 Duplicate-event assertion in `add_*_log` (#4)

```cpp
void TabletWriteLogManager::_add_log(TabletWriteLogEntry entry) {
    std::lock_guard<std::mutex> lock(_mutex);
#ifndef NDEBUG
    auto key = entry.dedup_key();  // (be_id, finish_time/s, txn_id, tablet_id, log_type)
    if (_recent_keys.contains(key)) {
        LOG(WARNING) << "Duplicate tablet write log entry: " << entry.to_string();
        DCHECK(false);
    }
    _recent_keys.insert(key);  // bounded LRU
#endif
    _log_buffer.push_back(std::move(entry));
    ...
}
```

Release builds expose `tablet_write_log_dedup_violations_total` instead of
crashing.

#### 2.3 Expose BE ring overflow as metric

`TabletWriteLogManager::_total_logs_dropped` already exists but isn't a metric:

```cpp
REGISTER_GAUGE_STARROCKS_METRIC(tablet_write_log_dropped_total,
    [] { return TabletWriteLogManager::instance()->total_dropped(); });
```

FE-side alerting: scrape `information_schema.be_metrics` → alert if any BE has
`tablet_write_log_dropped_total > 0`.

## Acceptance Criteria

### Phase 1 (FE PR)

- [ ] `tablet_write_log_sync_state` table is created by `TableKeeper` on first
      run, schema matches §1.1.
- [ ] Single-statement sync per §1.2; per-BE watermarks join into the WHERE clause.
- [ ] Cursor table is upserted on success (§1.3) and updated on failure (§1.5).
- [ ] `computeOverlap` follows §1.4; covered by unit tests for (success, single
      failure, sustained failure, recovery) trajectories.
- [ ] All seven metrics in §1.6 are registered, visible in `/metrics`,
      documented in the monitoring page.
- [ ] All three pending schema fixes (§1.7) applied; FE config doc updated.
- [ ] No schema migration of `tablet_write_log_history` itself (i.e. the column
      set defined in `dde326d7` stays put).
- [ ] Mock-based unit test simulates a 5-BE cluster with one slow BE and verifies
      the slow BE's data is not dropped.
- [ ] Integration test on a 3-node cluster: kill FE leader mid-sync, verify
      sync resumes without data loss after failover.

### Phase 2 (BE PRs)

- [ ] Pushdown: per-round payload size ≤ 1.5× the new-row count at 500 ops/sec.
- [ ] Dedup assertion: debug build crashes on contrived double-`add_log`; release
      build increments `tablet_write_log_dedup_violations_total`.
- [ ] Ring overflow metric: visible in BE `/metrics`; alert rule sample added to
      ops docs.

### Cross-cutting

- [ ] Schema is **frozen** after Phase 1 lands. Any future column needs are
      handled via `ALTER TABLE ADD COLUMN` through the existing
      `EXPECTED_COLUMNS` mechanism.

## Risks

- **Cursor table availability.** If `tablet_write_log_sync_state` is unavailable,
  sync falls back to the sentinel — equivalent to full ring scan, identical to
  current behavior. Acceptable.
- **PK still depends on BE not double-recording.** Mitigated by §2.2 assertion,
  but Phase 1 lands without it. Risk window: until Phase 2 ships, undetected BE
  double-`add_log` would silently dedup. Mitigation: keep
  `tablet_write_log_sync_rows_skipped_dup_total` visible — a sudden spike is the
  signal.
- **Downgrade path.** Old FE versions don't know about `sync_state`. They will
  fall back to `MAX(finish_time) FROM history` (the old SQL still works against
  the new history schema). Cursor table is harmless to ignore.

## Decision Log

- 2026-05-19: Picked per-BE watermark over per-tablet because tablet count can
  reach 10⁵+; per-BE cardinality stays bounded.
- 2026-05-19: Picked LEFT JOIN single-statement over per-BE loop because large
  clusters (>100 BEs) would otherwise need 100+ INSERT statements per round.
- 2026-05-19: Chose persistent cursor table over FE-memory-only because FE
  restart would otherwise force a full re-scan / re-bootstrap.
- 2026-05-19: Adaptive overlap capped at 600s; beyond that the BE ring (200s
  default capacity) can no longer back the recovery — alerting must take over.
- 2026-05-19: Phase 1 explicitly does NOT alter `tablet_write_log_history`
  schema beyond the three property fixes; the column set from `dde326d7`
  is final.
- 2026-05-19: BE pushdown deferred to Phase 2 because the scanner-side
  conjunct parsing is non-trivial and the FE-only fix is correct (just less
  efficient).
