# Shared-Data: Concurrent ADD PARTITION During Metadata-Only ALTER

- Status: active
- Owner: meegoo
- Last Updated: 2026-06-10

## Summary

Allow partition creation (manual `ALTER TABLE ... ADD PARTITION`, automatic creation
during load, and the dynamic-partition scheduler) to proceed concurrently with two
families of shared-data ALTER operations that are metadata-only and provably safe:

1. **Path 1 — Fast Schema Evolution V2 add/drop column** (synchronous, FE-catalog-only;
   `SchemaChangeHandler.updateCatalogForFastSchemaEvolution`).
2. **Path 2 — Lake ADD/DROP INDEX fast path (IDG)** (`LakeTableIndexFastPathJobBase`
   subclasses: `LakeTableAddIndexJob`, `LakeTableDropIndexJob`, including the
   bloom-filter-columns property fast path).

Today both are blocked by a coarse table-level state guard. Worse, the automatic
partition-creation path **cancels** any running schema-change job — including the
index fast-path jobs that have no real conflict with partition creation.

Out of scope (explicitly): `LakeTableSchemaChangeJob` (full rewrite with shadow
tablets), the `LakeTableAlterMetaJobBase` family (FSE V1 and meta-property jobs —
candidate follow-up), rollup jobs, shared-nothing `SchemaChangeJobV2`, and
DROP PARTITION concurrency (the jobs' owned-partition maps assume owned partitions
stay alive; see Risks).

## Background: Where Partition Creation Is Blocked Today

Three guard points reject or work around partition creation when
`OlapTable.state != NORMAL`:

| # | Path | Location | Today's behavior |
|---|------|----------|------------------|
| G1 | Manual `ALTER TABLE ... ADD PARTITION` | `AlterJobExecutor.visitAlterTableStatement` — `fe/fe-core/.../alter/AlterJobExecutor.java:164` | Throws `InvalidOlapTableStateException` for any non-NORMAL state |
| G2 | Automatic creation during load (expression partitioning) | `FrontendServiceImpl.cancelConflictingAlterJobs` — `fe/fe-core/.../service/FrontendServiceImpl.java:2745` | **Cancels** the running SCHEMA_CHANGE/ROLLUP job; if uncancellable (`FINISHED_REWRITING`), polls up to `auto_partition_wait_alter_finish_timeout_ms` (default 5000 ms) |
| G3 | Dynamic-partition scheduler | `fe/fe-core/.../clone/DynamicPartitionScheduler.java:387` | Sets `skipAddPartition = true` for non-NORMAL state (TABLET_RESHARD excepted) |

The guards exist because the classic schema-change job model is genuinely unsafe
against concurrent partition creation:

- `SchemaChangeJobV2.onFinished` iterates the table's **live** partition list and
  asserts every partition has a shadow index
  (`Preconditions.checkNotNull(shadowIdx, ...)`, `SchemaChangeJobV2.java:949`) — a
  partition added mid-job crashes the finish.
- `SchemaChangeJobV2` registers shadow index metas into the **table-level**
  `indexMetaIdToMeta` while running (`addShadowIndexToCatalog`,
  `SchemaChangeJobV2.java:550`), so `LocalMetastore.createPhysicalPartition`
  (`LocalMetastore.java:1700`), which builds one `MaterializedIndex` per table-level
  index meta in state `NORMAL`, would materialize the shadow index incorrectly in a
  concurrently created partition.

Neither hazard applies to Path 1 or Path 2 (evidence below), but the guards cannot
tell the difference: both job families set the same `OlapTableState.SCHEMA_CHANGE`
(Path 2) or transit through `UPDATING_META` (Path 1).

## Why Paths 1 and 2 Are Safe

### Path 1: FSE V2 synchronous add/drop column

`applyFastSchemaEvolutionMetaChangeInternal` (`SchemaChangeHandler.java:3620-3729`):

- The entire mutation — bump per-index-meta `schemaVersion`/`schemaId`, rebuild full
  schema, record a pre-FINISHED `SchemaChangeJobV2` for history only — happens inside
  **one table WRITE-lock critical section with one edit-log entry**
  (`applyFastSchemaEvolutionMetaChange` takes the lock at `:3566`; the in-memory apply
  runs in the `logModifyTableAddOrDrop` WAL callback). `state` is set to
  `UPDATING_META` at `:3625` and restored to `NORMAL` in `finally` at `:3727`.
- Consequence 1: there is **no asynchronous window** and no rollback machinery —
  failure before the edit-log append means nothing was applied.
- Consequence 2: `UPDATING_META` is observable **only by lock-free readers**. Any
  partition-creation path that proceeds will serialize behind the WRITE lock inside
  `LocalMetastore.addPartitions`, by which time the state is `NORMAL` again. This
  holds for every `UPDATING_META` producer in the codebase
  (`SchemaChangeHandler.java:3625/3727`, `SchemaChangeJobV2.java:923/1011`,
  `OnlineOptimizeJobV2.java:451/457`, `:690/709`) — all set-and-restore within a
  single WRITE-lock section — so tolerating `UPDATING_META` for partition-only
  statements is safe in both run modes.
- Copy/commit race: `addPartitions` builds tablets from a shadow copy taken under a
  READ lock, then revalidates under WRITE lock via `checkIfMetaChange`
  (`LocalMetastore.java:1131`), which compares index-meta count, per-meta
  `schemaHash`, distribution type, and `light_weight_tablet_creation`. FSE V2 changes
  `schemaVersion`/`schemaId` but **not** `schemaHash`, so a V2 change landing between
  copy and commit does not abort the ADD PARTITION — and does not need to: the new
  partition's tablets carrying the pre-alter schema file is exactly the state of
  every pre-existing tablet under FSE V2 (V2 never rewrites tablet schema files; CN
  reconciles by schema id / column unique ids per rowset). With
  `light_weight_tablet_creation` the tablets carry even less state. Uniform either
  way.

### Path 2: Lake ADD/DROP INDEX fast path

`LakeTableIndexFastPathJobBase` and subclasses satisfy four invariants:

1. **Owned-set snapshot.** `partitionToTablets` is populated once in `runPendingJob`
   (`LakeTableIndexFastPathJobBase.java:181`); the field doc declares the dispatch is
   "driven by a stable snapshot even if the table is concurrently modified" (`:95-97`).
   Every later phase iterates only the owned set: `runRunningJob:275`,
   `lakePublishVersion:359`, `runFinishedRewritingJob:323`, `dispatchAllTasks:564`,
   `replay`/`replayUpdateNextVersion:530`. A partition created after the snapshot is
   simply outside the job's scope — no phase can crash on it.
2. **Zero table-level meta mutation before FINISHED.** No shadow index, no new index
   meta, no schema version bump. `createPhysicalPartition` therefore reads a clean,
   consistent schema while the job runs — the corruption vector that justifies the
   guard for `SchemaChangeJobV2` does not exist here.
3. **Idempotent, table-level-only catalog flip at FINISHED.**
   `LakeTableAddIndexJob.applyCatalogMutation` (`:102`) appends to
   `table.getIndexes()` with a duplicate check; `LakeTableDropIndexJob`'s (`:135`)
   removes by name/id. Neither touches partition state. Both run (and replay) under
   the table WRITE lock, serialized against `addPartitions`' commit section.
4. **Cancel is FE-only cleanup.** `cancelImpl` (`:384`) drains the agent-task queue,
   restores `NORMAL`, persists CANCELLED. BE-side txn logs (`only_add_index` /
   `OpDropIndex` tombstones, keyed by the watershed txn) are never published and are
   vacuumed as orphans. The catalog flip happens only at FINISHED, so a cancelled job
   leaves the table at the pre-alter schema — which is exactly the schema any
   concurrently created partition was built with.

**Window-state matrix** (partition created while the job runs):

| | Job finishes | Job cancelled / fails |
|---|---|---|
| **ADD INDEX** | FE catalog gains the index; the new partition's tablets have no `.idx` payloads — the same state IDG already produces for segments loaded concurrently into *owned* partitions after the watershed. BE checks index presence per segment and falls back to scan; future writes/compaction build the index inline. Performance-only, self-healing. | New partition (pre-alter schema, no index) matches the rolled-back table. Consistent. |
| **DROP INDEX** | New partition was created with the old index definition but holds no data and no `.idx` files; the stale reference in its creation-time schema is dead metadata (compaction uses the latest schema). Nothing to tombstone. | New partition (still containing the index) matches the rolled-back table. Consistent. |

**Version chains:** `commitVersionMap` covers owned partitions only; loads into the
new partition publish independently and never interact with the job's watershed or
reserved commit versions.

**Replay:** journal order serializes `AddPartitionsInfoV2` against job state
transitions. Job replay touches only `commitVersionMap` partitions and the
idempotent catalog flip; partition replay has no dependency on job state. Both
interleavings (partition logged before FINISHED; after FINISHED) replay cleanly.

**`checkIfMetaChange` interaction:** the fast path mutates `TableIndexes`/
`bfColumns`, none of which `checkIfMetaChange` compares — so a job finishing inside
an ADD PARTITION's copy/commit window neither aborts the ADD PARTITION nor needs to.

## Design

### A. Capability method on `AlterJobV2`

```java
// AlterJobV2.java
/**
 * Whether this job tolerates partitions being created on the target table while
 * the job is running. A job may return true only when it (a) never iterates the
 * table's live partition list after its initial snapshot, (b) registers no
 * table-level shadow meta before FINISHED, and (c) cancels without per-partition
 * cleanup. Default false.
 */
public boolean allowConcurrentPartitionCreation() {
    return false;
}
```

- Override returning `true` in `LakeTableIndexFastPathJobBase` only (covers
  `LakeTableAddIndexJob`, `LakeTableDropIndexJob`, and the BF-columns property jobs
  built on them).
- Behavioral method, not serialized state: replayed/restored jobs keep the override
  via their concrete class. No edit-log format change, no rolling-upgrade
  negotiation (only the leader evaluates guards).
- `LakeTableAlterMetaJobBase` (FSE V1, persistent-index/file-bundling property jobs)
  is structurally identical (owned `physicalPartitionIndexMap`, table-level
  `updateCatalog`) and is the designated follow-up: flipping it later is a one-line
  override plus tests, after validating CN behavior for stale tablet-schema-file vs
  FE schema on freshly created partitions.

### B. Shared guard helper

```java
// AlterJobMgr.java (static; both call sites already depend on GlobalStateMgr)
public static boolean unfinishedAlterJobsAllowConcurrentPartitionCreation(long tableId) {
    List<AlterJobV2> jobs = new ArrayList<>();
    jobs.addAll(GlobalStateMgr.getCurrentState().getSchemaChangeHandler()
            .getUnfinishedAlterJobV2ByTableId(tableId));
    jobs.addAll(GlobalStateMgr.getCurrentState().getRollupHandler()
            .getUnfinishedAlterJobV2ByTableId(tableId));
    return !jobs.isEmpty()
            && jobs.stream().allMatch(AlterJobV2::allowConcurrentPartitionCreation);
}
```

Empty job list with non-NORMAL state is an anomaly (e.g. stale state after a crash);
the helper returns `false` so legacy behavior applies. `getUnfinishedAlterJobV2ByTableId`
already exists on `AlterHandler` (`AlterHandler.java:109`).

TOCTOU note: the check is lock-free, but the checked set can only shrink — a new
*unsafe* job cannot start while `state != NORMAL` (G1 blocks all other ALTERs), and a
safe job finishing early only makes the situation safer. The deeper serialization is
the table WRITE lock inside `addPartitions` plus `checkIfMetaChange`.

### C. G1 — `AlterJobExecutor.visitAlterTableStatement`

Replace the unconditional throw at `AlterJobExecutor.java:164` with:

```java
OlapTable.OlapTableState state = olapTable.getState();
if (state != OlapTable.OlapTableState.NORMAL) {
    boolean addPartitionOnly = statement.getAlterClauseList().stream()
            .allMatch(c -> AlterOpType.getOpType(c) == AlterOpType.ADD_PARTITION);
    boolean tolerable = Config.enable_concurrent_add_partition_during_alter
            && addPartitionOnly
            && (state == OlapTable.OlapTableState.UPDATING_META
                || (state == OlapTable.OlapTableState.SCHEMA_CHANGE
                    && AlterJobMgr.unfinishedAlterJobsAllowConcurrentPartitionCreation(table.getId())));
    if (!tolerable) {
        throw new AlterJobException("", InvalidOlapTableStateException.of(state, olapTable.getName()));
    }
    LOG.info("allow ADD PARTITION on table {} concurrent with alter, state={}",
            olapTable.getName(), state);
}
```

- The `UPDATING_META` branch implements Path 1 (no job object exists to check; safety
  rests on the always-within-WRITE-lock argument above).
- The `SCHEMA_CHANGE` + safe-jobs branch implements Path 2.
- Mixed-clause statements, all other states (`ROLLUP`, `RESTORE`, `OPTIMIZE`, ...),
  and all non-partition clauses keep today's rejection.
- Downstream is already state-clean: `visitAddPartitionClause` only calls
  `DynamicPartitionUtil.checkAlterAllowed` (dynamic-partition-property check,
  state-independent — `DynamicPartitionUtil.java:409`) and
  `LocalMetastore.addPartitions`, which contains no state assertion (verified; its
  consistency mechanism is `checkIfMetaChange`).

### D. G2 — `FrontendServiceImpl.cancelConflictingAlterJobs`

Insert a skip at the top of the SCHEMA_CHANGE branch (`FrontendServiceImpl.java:2762`):

```java
if (olapTable.getState() == OlapTable.OlapTableState.SCHEMA_CHANGE) {
    if (Config.enable_concurrent_add_partition_during_alter
            && AlterJobMgr.unfinishedAlterJobsAllowConcurrentPartitionCreation(olapTable.getId())) {
        LOG.info("skip cancelling alter job for automatic partition creation,"
                + " jobs tolerate concurrent partition creation. txn_id={} table={}",
                txnId, olapTable.getName());
    } else {
        cancelAlterJob(state, db, olapTable, ShowAlterStmt.AlterType.COLUMN, errMsg);
    }
}
```

- The ROLLUP branch is unchanged (rollup jobs are out of scope and stay cancelled).
- The `FINISHED_REWRITING` poll-wait fallback (`waitForAlterJobCompletion`) remains
  for the unsafe-job path; when the skip fires, neither cancel nor wait happens and
  `addPartitions` proceeds directly.
- `UPDATING_META` already passes through this function untouched today (it is neither
  ROLLUP nor SCHEMA_CHANGE) and serializes on the table lock — no change needed; add
  a regression test.
- This is the headline behavioral fix: a load that auto-creates a partition no longer
  kills a user's running ADD/DROP INDEX job (current behavior documented in
  `docs/en/faq/Others.md:249`).

### E. G3 — `DynamicPartitionScheduler` (optional, same mechanism)

At `DynamicPartitionScheduler.java:387`, do not set `skipAddPartition = true` when
the config is enabled and `unfinishedAlterJobsAllowConcurrentPartitionCreation`
returns true (or state is `UPDATING_META`). Only the add half is relaxed; the drop
half keeps its existing state handling (DROP stays out of scope). Low risk, shares
the helper; can ship in the same PR or trail as a small follow-up.

### F. Config

```java
// Config.java, next to auto_partition_wait_alter_finish_timeout_ms (:2772)
/**
 * If true, partition creation (manual ALTER TABLE ADD PARTITION, automatic
 * creation during load, dynamic-partition scheduler) is allowed to proceed
 * concurrently with alter jobs that declare allowConcurrentPartitionCreation()
 * (currently the shared-data ADD/DROP INDEX fast-path jobs), and with the
 * transient UPDATING_META state of fast schema evolution, instead of rejecting
 * the DDL or cancelling the alter job. Set to false to restore the legacy
 * exclusive behavior.
 */
@ConfField(mutable = true)
public static boolean enable_concurrent_add_partition_during_alter = true;
```

Default `true`: the change strictly removes failure modes (failed DDLs, cancelled
alter jobs); the mutable flag is the kill switch. Docs to update per repo rule:
`docs/en/administration/management/FE_configuration.md` and the `zh` counterpart
(`ja` if the page exists); also narrow the FAQ entry at `docs/en/faq/Others.md:249`
to the still-affected job types.

## Failure / Rollback Matrix (Path 2, per job phase, with a concurrently added partition)

| Job phase at failure/cancel | BE state | FE state after cancel | New-partition interaction |
|---|---|---|---|
| PENDING (snapshot not yet persisted) | nothing dispatched | state NORMAL, no catalog change | none — partition not in owned set |
| WAITING_TXN | nothing dispatched | same | none |
| RUNNING | some tablets hold unpublished txn logs (watershed txn) | same; orphan txn logs vacuumed | none |
| FINISHED_REWRITING | owned partitions' `nextVersion` bumped | see Risk R2 (pre-existing gap) | none — `commitVersionMap` never contains the new partition |
| FINISHED | published; catalog flipped (idempotent) | n/a (success) | new partition lacks `.idx` payloads — benign per IDG semantics |

In every phase, the job holds no reference to the concurrently created partition, so
rollback never needs to know it exists; conversely the partition was created from the
pre-alter schema, which is the post-rollback schema. FSE V2 (Path 1) has no phases:
single-edit-log atomicity.

## Behavior Changes (user-visible)

1. `ALTER TABLE ... ADD PARTITION` no longer fails with
   `Table[...] is under SCHEMA_CHANGE` while a lake ADD/DROP INDEX fast-path job is
   running.
2. Loads that trigger automatic partition creation no longer cancel a running lake
   ADD/DROP INDEX job.
3. `ADD PARTITION` no longer transiently fails when racing the millisecond
   `UPDATING_META` window of FSE V2 add/drop column (or any other `UPDATING_META`
   producer).
4. Dynamic-partition scheduler creates partitions instead of skipping a cycle while a
   safe job runs (if G3 ships).
5. `SHOW ALTER TABLE` / `CANCEL ALTER TABLE` semantics unchanged.

## Test Plan

Unit tests (`fe/fe-core/src/test/java/com/starrocks/...`):

1. **Guard logic** (extend `alter/AlterTest` or a new
   `alter/ConcurrentAddPartitionDuringAlterTest`):
   - SCHEMA_CHANGE + safe job → ADD PARTITION passes G1; ADD COLUMN still rejected;
     mixed ADD PARTITION + other clause rejected; config=false rejected.
   - SCHEMA_CHANGE + `LakeTableSchemaChangeJob` (unsafe) → rejected.
   - SCHEMA_CHANGE + zero unfinished jobs → rejected (anomaly conservatism).
2. **Job lifecycle with concurrent partition** (extend
   `alter/LakeTableIndexFastPathJobBaseTest`, `alter/SchemaChangeHandlerLakeIndexFastPathTest`):
   - partition added between PENDING and RUNNING → job finishes; new partition absent
     from `commitVersionMap`; `applyCatalogMutation` applied; no exception.
   - cancel at RUNNING with a concurrently added partition → state NORMAL; partition
     intact; catalog unchanged.
3. **Auto-create path** (extend `service/FrontendServiceImplCreatePartitionTest`):
   - safe job running → job NOT cancelled, partitions created, response OK.
   - unsafe job running → legacy cancel still fires.
4. **FSE V2 / UPDATING_META race**: two-thread test using the lock test helpers
   (`common/lock/LockTestUtils`, `LockThread`) — thread A inside FSE V2's WRITE-lock
   section, thread B passes G1 during `UPDATING_META` and completes after A releases.
5. **Replay**: journal sequences (create job → add partition → finish) and
   (create job → add partition → cancel) replay cleanly on a fresh FE
   (extend the fast-path job replay tests).

SQL integration (`test/sql/`): optional shared-data case — ADD INDEX on a large
table, concurrent INSERT creating new expression partitions, assert both succeed;
gated on shared-data CI availability.

## Risks and Mitigations

- **R1 — Guard TOCTOU**: lock-free state/job reads at G1/G2. Mitigated by the
  shrink-only property of the checked set and by lock-level serialization +
  `checkIfMetaChange` downstream. Same exposure already exists today for the
  NORMAL-at-entry case.
- **R2 — Pre-existing**: `LakeTableIndexFastPathJobBase.cancelImpl` lacks the
  FINISHED_REWRITING force-cancel hardening that `LakeTableAlterMetaJobBase` has
  (no-op publish to heal the version chain, `LakeTableAlterMetaJobBase.java:498-616`).
  This change *reduces* exposure (G2 stops cancelling these jobs) but the gap should
  be filed and fixed independently.
- **R3 — DROP PARTITION of an owned partition** remains unsafe
  (`runRunningJob:277` `checkNotNull`; `lakePublishVersion:361` retry-forever) and
  remains blocked by the untouched guards — only ADD is relaxed.
- **R4 — Operator escape**: `enable_concurrent_add_partition_during_alter=false`
  restores legacy behavior at all guards without restart.
- **R5 — New partition without `.idx` after ADD INDEX**: performance-only and
  self-healing via future writes/compaction; identical to the already-shipped
  semantics for segments loaded concurrently into owned partitions.

## Implementation Order

1. `AlterJobV2.allowConcurrentPartitionCreation()` + override in
   `LakeTableIndexFastPathJobBase` + `AlterJobMgr` helper (+ unit tests).
2. `Config.enable_concurrent_add_partition_during_alter` + en/zh config docs.
3. G1 (`AlterJobExecutor`) + tests.
4. G2 (`FrontendServiceImpl`) + tests.
5. G3 (`DynamicPartitionScheduler`) + tests (optional, may trail).
6. Concurrency + replay tests; FAQ doc adjustment.
7. Follow-up (separate PR): flip the capability on `LakeTableAlterMetaJobBase`
   (FSE V1 + meta-property jobs) after CN-side validation; file R2.

Single PR `[Enhancement]` is cohesive (~100-150 lines main code + ~400 lines tests);
steps 1-4 are the minimum shippable unit.

## Acceptance Criteria

- With a lake ADD/DROP INDEX fast-path job in PENDING/WAITING_TXN/RUNNING/
  FINISHED_REWRITING: manual ADD PARTITION succeeds, automatic partition creation
  succeeds without cancelling the job, and the job subsequently finishes and applies
  its catalog mutation.
- A partition created mid-job is fully usable (load + query) both after job FINISHED
  and after job CANCELLED.
- ADD COLUMN/DROP COLUMN via FSE V2 racing ADD PARTITION: neither statement fails;
  final schema and partition list reflect both.
- All non-safe alter jobs and all non-ADD-PARTITION DDL keep today's rejection /
  cancellation behavior; setting the config to false restores legacy behavior
  everywhere.
- All listed unit tests pass; `mvn checkstyle:check` clean.

## Decision Log

- 2026-06-10: Scope fixed to shared-data + ADD PARTITION only; Paths 1 (FSE V2 sync
  column changes) and 2 (lake index fast path) in scope; FSE V1 /
  `LakeTableAlterMetaJobBase` deferred as follow-up; DROP PARTITION explicitly out.
- 2026-06-10: Chose a per-job capability method (`allowConcurrentPartitionCreation`)
  over state-enum changes — `OlapTableState.SCHEMA_CHANGE` cannot distinguish safe
  from unsafe jobs, and new enum values would leak into persisted state and external
  tooling.
- 2026-06-10: Empty unfinished-job list with non-NORMAL state treated as NOT
  tolerable (conservative; indicates state anomaly).
- 2026-06-10: Config default true with mutable kill switch; rationale: the change
  only removes failure modes, and G2's current behavior (cancelling user ALTER jobs
  from a load path) is the worse default.
