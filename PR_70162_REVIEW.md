# PR #70162 Code Review: [Enhancement] Add Range-Split Parallel Compaction for Non-Overlapping Output

**Author:** meegoo
**Branch:** `meegoo/compaction-9e2e-squashed`
**Type:** Enhancement
**Reviewer:** Claude (AI Review)
**Date:** 2026-03-12

---

## 1. Overview

This PR introduces a new **RANGE_SPLIT** subtask type for StarRocks lake (shared-data) parallel compaction. Instead of dividing compaction work by segment index (NORMAL/LARGE_ROWSET_PART), this strategy partitions the compaction by **sort key ranges**, so that each subtask processes a non-overlapping key range across all input rowsets.

### Key Benefits
- **Non-overlapping output rowsets** (`overlapped=false`): Improves zone-map pruning during queries
- **Eliminates multi-way merge on read**: Since output segments are non-overlapping, subsequent queries skip merging
- **Reuses core algorithm**: Extracts `calculate_range_split_boundaries()` into a shared utility for both tablet splitting (resharding) and compaction

### Feature Gate
- Controlled by `enable_lake_compaction_range_split` (default: `false`)
- Only activated when all segments have `sort_key_min`/`sort_key_max` metadata

---

## 2. Architecture & Data Flow

```
                           _create_subtask_groups()
                                    |
                    [config::enable_lake_compaction_range_split?]
                           /                    \
                         NO                     YES
                          |                       |
              Existing strategy         _can_use_range_split()
         (NORMAL/LARGE_ROWSET_PART)          /         \
                                           NO          YES
                                           |             |
                                       Fallback   _create_range_split_groups()
                                                         |
                                        _collect_segment_key_bounds()
                                                         |
                                   calculate_range_split_boundaries()
                                          (tablet_splitter.cpp)
                                                         |
                                         SubtaskGroup[RANGE_SPLIT] x N
                                                         |
                                      submit_subtasks_from_groups()
                                                         |
                                    execute_subtask_range_split() per group
                                                         |
                            CompactionTask (Horizontal/Vertical)
                            with range filter (start_key/end_key)
                                                         |
                                         on_subtask_complete()
                                                         |
                                       get_merged_txn_log()
                                     (all-or-nothing merge)
                                                         |
                                    Single non-overlapping output rowset
```

---

## 3. Changed Files Analysis

### 3.1 `gensrc/proto/lake_types.proto`

**Changes:** Added 6 new fields to `TxnLogPB`.

| Field | Type | Location | Purpose |
|-------|------|----------|---------|
| `range_split_lower_bound` | `TuplePB` | `OpCompaction` (field 15) | Lower sort key bound for RANGE_SPLIT subtask |
| `range_split_upper_bound` | `TuplePB` | `OpCompaction` (field 16) | Upper sort key bound for RANGE_SPLIT subtask |
| `range_split_lower_inclusive` | `bool` | `OpCompaction` (field 17) | Whether lower bound is inclusive |
| `range_split_upper_inclusive` | `bool` | `OpCompaction` (field 18) | Whether upper bound is inclusive |
| `is_range_split` | `bool` | `OpParallelCompaction` (field 7) | Flag indicating range-split strategy was used |

**Review Notes:**
- All fields are `optional`, following the proto style guideline (never use `required`).
- Field ordinals are sequential and do not conflict with existing fields.
- Wire-compatible with older versions (optional fields default to absent).

**Concern:** The `range_split_lower_bound` and `range_split_upper_bound` fields are stored in the txn log but appear to be **only written, never read** during the merge process. They serve as audit/debugging metadata. This is acceptable but could be documented more explicitly in the proto comments.

---

### 3.2 `be/src/common/config.h`

**Changes:** Added one configuration parameter.

```cpp
CONF_mBool(enable_lake_compaction_range_split, "false");
```

- `m` prefix = mutable at runtime (can be changed via SQL `SET` or admin API)
- Default `false` = feature is opt-in, conservative rollout

**Concern (Documentation):** Per CLAUDE.md guidelines, new config parameters must be documented in `docs/en/administration/management/BE_configuration.md`. This documentation update is **missing** from the PR.

---

### 3.3 `be/src/storage/lake/compaction_task_context.h`

**Changes:** Added 9 new fields to `CompactionTaskContext`.

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `has_range_split` | `bool` | `false` | Master flag: this task uses range filtering |
| `range_start_key` | `vector<OlapTuple>` | empty | Lower key bound(s) for TabletReaderParams |
| `range_end_key` | `vector<OlapTuple>` | empty | Upper key bound(s) for TabletReaderParams |
| `range_lower_inclusive` | `bool` | `true` | GE vs GT for start |
| `range_upper_inclusive` | `bool` | `false` | LE vs LT for end |
| `has_lower_bound` | `bool` | `false` | Explicit: is lower bound finite? |
| `has_upper_bound` | `bool` | `false` | Explicit: is upper bound finite? |
| `is_first_range` | `bool` | `false` | Is this the leftmost range? |
| `is_last_range` | `bool` | `false` | Is this the rightmost range? |

**Design Note:** The `has_lower_bound`/`has_upper_bound` flags decouple bound presence from the OlapTuple content. This avoids relying on "empty OlapTuple = unbounded" semantics in the segment iterator, which is fragile and could change. Good defensive design.

**Minor Concern:** `is_first_range` and `is_last_range` are set in `execute_subtask_range_split()` but are **never read** by the compaction tasks (horizontal/vertical). They could be removed from `CompactionTaskContext` or documented as reserved for future use (e.g., statistics, logging).

---

### 3.4 `be/src/storage/lake/horizontal_compaction_task.cpp`

**Changes:** Added range filtering block (18 lines).

```cpp
if (_context->has_range_split) {
    if (_context->has_lower_bound && !_context->range_start_key.empty()) {
        reader_params.start_key = _context->range_start_key;
        reader_params.range = ... GE or GT ...;
    }
    if (_context->has_upper_bound && !_context->range_end_key.empty()) {
        reader_params.end_key = _context->range_end_key;
        reader_params.end_range = ... LE or LT ...;
    }
}
```

**Review:** Correct. The code sets `start_key`/`end_key` on `TabletReaderParams` only when bounds are present. The reader will use these to filter segments via zone-map / seek operations.

---

### 3.5 `be/src/storage/lake/vertical_compaction_task.cpp`

**Changes:** Identical range filtering block as horizontal, with an important comment:

> "Must apply to ALL column groups (key and non-key) so that segment iterators produce the same row subsets."

**Review:** This is a **critical correctness detail**. In vertical compaction, key columns are read first with `heap_merge_iterator` which records `RowSourceMasks`. Non-key columns are then read with `mask_merge_iterator` replaying those masks. If only the key group had range filtering, non-key iterators would read from row 0, producing misaligned data. The comment correctly explains this.

---

### 3.6 `be/src/storage/lake/tablet_splitter.h` (NEW public API)

**Changes:** Extracted and generalized the range split algorithm into a reusable public API.

New types:
- `SegmentSplitInfo`: Segment-level key bounds + statistics
- `SourceStats`: Per-source (rowset) statistics tracking
- `RangeSplitResult`: Output of boundary calculation (boundaries, per-range sizes, per-range row counts)

New function:
```cpp
StatusOr<RangeSplitResult> calculate_range_split_boundaries(
    const std::vector<SegmentSplitInfo>& segments,
    int32_t target_split_count,
    int64_t target_value_per_split,
    bool use_num_rows,
    bool track_sources = false,
    const TabletRange* tablet_range = nullptr);
```

**Review:** Excellent refactoring. This function is now shared between:
1. **Tablet splitting (resharding)** in `get_tablet_split_ranges()`
2. **Range-split compaction** in `_create_range_split_groups()`

---

### 3.7 `be/src/storage/lake/tablet_splitter.cpp` (Core Algorithm)

**Changes:** ~240 lines of new algorithm code + refactoring of `get_tablet_split_ranges()`.

#### Algorithm Steps:

1. **Collect boundary points**: Extract `min_key` and `max_key` from all segments into a sorted set.
2. **Build ordered ranges**: Create adjacent ranges between consecutive boundary points.
3. **Distribute data**: For each segment, find overlapping ranges via binary search, then distribute `num_rows`/`data_size` proportionally.
4. **Greedy split**: Walk candidate ranges, accumulate values, place boundaries when accumulated >= target. Skip trailing empty ranges to maximize natural gaps.
5. **Estimate per-range stats**: Walk candidate ranges against boundaries to produce per-range size/row estimates.

**Binary Search Optimization:** The old code used O(N*M) linear scans for segment-to-range overlap. The new code uses binary search for the first overlapping range, reducing to O(N*log(M)).

**Tablet Range Filtering:** When `tablet_range` is provided, ranges outside the tablet range are excluded from candidates, and boundaries are only placed at positions where `strictly_contains()` returns true.

**Potential Concern - Data Distribution Accuracy:** The proportional distribution (Step 3) assumes uniform data density within each segment's key range. For segments with skewed data, this could produce unbalanced subtasks. However, this is a reasonable approximation and matches the existing behavior in tablet splitting.

**Refactoring of `get_tablet_split_ranges()`:** The old function had ~200 lines of inline algorithm code. It now delegates to `calculate_range_split_boundaries()` and constructs `TabletRangeInfo` from the result. This is a clean separation of concerns.

#### Edge Cases Handled:
- Empty segments → return empty result
- `target_split_count <= 1` → return empty result
- Fewer non-empty ranges than target splits → return empty result
- `tablet_range` filtering → skip out-of-range boundaries

---

### 3.8 `be/src/storage/lake/tablet_parallel_compaction_manager.h`

**Changes:**
- New enum value: `SubtaskType::RANGE_SPLIT`
- New fields in `SubtaskGroup` for range split (rowsets, bounds, flags)
- New fields in `SubtaskInfo` for range bounds
- New fields in `TabletParallelCompactionState` (is_range_split, expected count)
- New private methods: `_can_use_range_split`, `_collect_segment_key_bounds`, `_create_range_split_groups`, `_variant_tuple_to_olap_tuple`
- New public method: `execute_subtask_range_split`
- `FRIEND_TEST` declarations for unit testing

**Concern:** `#include <gtest/gtest_prod.h>` is included unconditionally (not behind `#ifdef NDEBUG` or similar). This means the production binary depends on gtest headers. This is a **common pattern in StarRocks** (many files use `FRIEND_TEST`), so it's consistent with the codebase style.

---

### 3.9 `be/src/storage/lake/tablet_parallel_compaction_manager.cpp`

This is the largest change (~850 lines modified). Key sections:

#### 3.9.1 Merge Logic (`get_merged_txn_log`)

The merge path is now split into two branches:

```
if (state->is_range_split) {
    // All-or-nothing: ALL subtasks must succeed
    // Produces non-overlapping output (overlapped=false)
} else {
    // Existing logic: partial success OK
    // Handles NORMAL + LARGE_ROWSET_PART subtasks
}
```

**All-or-Nothing Semantics:** For range split, if ANY subtask fails, the entire compaction fails. This is correct because:
- Each subtask processes a disjoint key range
- Missing any range would cause data loss
- Unlike NORMAL subtasks, range-split subtasks cannot be individually retried

**Incomplete Submission Detection:** The code checks `completed_subtasks.size() != expected_range_split_count` to detect cases where `submit_func` failed after some subtasks were already submitted. This prevents data loss from partial merges.

**Segment Index Renumbering:** Each subtask assigns `segment_idx=0` to its output segments. During merge, these are renumbered sequentially:
```cpp
uint32_t seg_idx_base = static_cast<uint32_t>(merged_output->segment_metas_size());
sm->set_segment_idx(seg_idx_base + static_cast<uint32_t>(i));
```
This prevents RSSID (RowSet Segment ID) collisions in PK tables where `RSSID = rowset_id + segment_idx`.

**Output Rowset Properties:**
- `overlapped=false`: Key benefit - enables zone-map pruning
- `next_compaction_offset` cleared: Must not be set for non-overlapped rowsets

**LCRM (Logical Column Read Map) Handling:** Subtask LCRM files are collected as orphan files for cleanup, and the merge task produces a combined LCRM. This is the same pattern as LARGE_ROWSET_PART.

#### 3.9.2 Subtask Creation (`_create_subtask_groups`)

Range split is attempted **before** the existing NORMAL/LARGE_ROWSET_PART strategy:
```cpp
if (config::enable_lake_compaction_range_split && _can_use_range_split(rowsets)) {
    auto range_groups = _create_range_split_groups(tablet_id, rowsets, max_parallel, max_bytes_per_subtask);
    if (!range_groups.empty()) {
        return range_groups;
    }
    VLOG(1) << "Range split fallback ...";
}
```
This means range split has **higher priority** when enabled. Fallback to existing strategy is seamless.

#### 3.9.3 Subtask Submission & Execution

- `submit_subtasks_from_groups()`: Records `expected_range_split_count` before submission, sets `state->is_range_split = true`
- `execute_subtask_range_split()`: Converts `VariantTuple` bounds to `OlapTuple`, sets context fields, delegates to `_tablet_mgr->compact()`

**Review Note on execute_subtask_range_split:** The code structure closely mirrors `execute_subtask()` and `execute_subtask_large_rowset_part()`. There is significant code duplication in:
- State validation
- Context creation
- Queue time tracking
- Cancel function setup
- Flush pool configuration
- Error handling and token release

This is a common pattern in this codebase but could benefit from a shared helper. Not blocking for this PR.

#### 3.9.4 Helper Functions

- `_can_use_range_split()`: Validates all segments have sort key metadata
- `_collect_segment_key_bounds()`: Extracts `SegmentSplitInfo` from rowset metadata, estimates per-segment data size proportionally
- `_variant_tuple_to_olap_tuple()`: Converts VariantTuple to OlapTuple using `datum_to_string()`
- `_create_range_split_groups()`: Orchestrates boundary calculation and group creation

---

### 3.10 `be/test/storage/lake/tablet_parallel_compaction_manager_test.cpp`

**Changes:** +510 lines of unit tests.

| Test | What It Validates |
|------|-------------------|
| `test_can_use_range_split_empty_rowsets` | Empty input → false |
| `test_can_use_range_split_missing_segment_metas` | Missing sort_key metadata → false |
| `test_can_use_range_split_with_segment_metas` | Valid metadata → true |
| `test_collect_segment_key_bounds` | Correct extraction of bounds from rowsets |
| `test_calculate_range_split_boundaries_basic` | 3 overlapping segments → 2 boundaries |
| `test_calculate_range_split_boundaries_single_subtask` | target=1 → no boundaries |
| `test_calculate_range_split_boundaries_empty_segments` | Empty input → no boundaries |
| `test_variant_tuple_to_olap_tuple` | INT32 conversion correctness |
| `test_variant_tuple_to_olap_tuple_empty` | Empty tuple → empty OlapTuple |
| `test_create_range_split_groups` | End-to-end group creation from metadata |
| `test_get_merged_txn_log_range_split` | Successful merge: non-overlapping output, segment_idx renumbering |
| `test_get_merged_txn_log_range_split_partial_failure` | Any failure → entire compaction fails |
| `test_get_merged_txn_log_range_split_incomplete_submission` | Missing subtask → detected and fails |
| `test_get_merged_txn_log_range_split_with_lcrm` | LCRM handling: orphan files collected |
| `test_compaction_context_range_split_fields` | Context field defaults and modification |

**Test Coverage Assessment:**
- Unit tests cover the key helper functions and merge logic well
- Edge cases (empty, single, failure) are covered
- The incomplete submission test is particularly valuable (prevents data loss regression)

**Missing Test Coverage:**
- No test for `execute_subtask_range_split()` end-to-end (would require mock TabletManager)
- No test for the actual compaction with range filtering (horizontal/vertical) - this would be an integration test
- No test for multi-column sort keys (all tests use single INT32 key)
- No test for NULL values in sort keys

---

## 4. Correctness Analysis

### 4.1 Data Consistency Guarantees

The design ensures no data loss through:
1. **All-or-nothing merge**: If any range subtask fails, nothing is merged
2. **Incomplete submission detection**: If `submit_func` fails mid-way, the merge detects missing subtasks
3. **Non-overlapping ranges**: Adjacent ranges use `[lower, upper)` semi-open intervals (first range unbounded below, last range unbounded above)
4. **Both column groups filtered**: Vertical compaction applies range filter to key AND non-key groups

### 4.2 Potential Edge Cases

1. **Segments with identical min/max keys**: If multiple segments have the same key range, the proportional distribution will split evenly. This is correct but could produce very small ranges.

2. **Single-row segments**: A segment where `min_key == max_key` will contribute to exactly one range boundary pair. The algorithm handles this correctly (the boundary set will have fewer unique points).

3. **Primary key tables with deletes**: The code does NOT account for delvec (delete vector) in `_collect_segment_key_bounds()`, unlike `get_tablet_split_ranges()`. This means data size estimates may be inflated for PK tables with many deletes. The compaction task itself will handle deletes correctly during execution, so this only affects load balancing between subtasks, not correctness.

---

## 5. Performance Considerations

### 5.1 Algorithm Complexity
- **Boundary collection**: O(S * log S) where S = total segments (set insertion)
- **Data distribution**: O(S * log R) where R = number of ranges (binary search)
- **Greedy split**: O(R) linear scan
- **Overall**: O(S * log S) dominated by sorting

### 5.2 Memory Overhead
- Each `SubtaskGroup` stores a copy of all rowset pointers (`range_split_rowsets`)
- `VariantTuple` copies for boundaries
- This is small compared to the data being compacted

### 5.3 Query Performance Impact
- Non-overlapping output enables zone-map pruning: queries on sort key columns can skip entire segments
- Eliminates merge-on-read for overlapping rowsets
- Most significant for time-series or monotonically increasing key patterns

---

## 6. Issues & Recommendations

### 6.1 MUST FIX

1. **Missing Documentation**: The `enable_lake_compaction_range_split` config parameter needs to be documented in:
   - `docs/en/administration/management/BE_configuration.md`
   - `docs/zh/administration/management/BE_configuration.md` (if exists)

### 6.2 SHOULD FIX

2. **Unused fields in CompactionTaskContext**: `is_first_range` and `is_last_range` are set but never read by the compaction tasks. They should either be:
   - Removed from `CompactionTaskContext` (keep only in `SubtaskGroup`), or
   - Documented with a comment explaining future use

3. **Proto fields written but never read**: `range_split_lower_bound/upper_bound` in `OpCompaction` are written in `execute_subtask_range_split()` but never read during merge. Add a comment clarifying they serve as audit/debugging metadata.

### 6.3 NICE TO HAVE

4. **Delvec adjustment in `_collect_segment_key_bounds()`**: For PK tables, data size estimates could be more accurate by accounting for delete vectors (as done in `get_tablet_split_ranges()`). This would improve load balancing.

5. **Code deduplication**: `execute_subtask_range_split()` shares ~50% of its code with `execute_subtask()` and `execute_subtask_large_rowset_part()`. A shared helper could reduce maintenance burden.

6. **Multi-column sort key testing**: All unit tests use single INT32 keys. Adding a test with composite sort keys (e.g., INT32 + VARCHAR) would increase confidence.

---

## 7. Summary

| Aspect | Assessment |
|--------|------------|
| **Design** | Sound. Range-split compaction is a well-motivated optimization. The all-or-nothing semantics are correct for non-overlapping output. |
| **Refactoring** | Excellent. Extracting `calculate_range_split_boundaries()` into a shared utility avoids duplication and improves testability. |
| **Correctness** | High confidence. Edge cases are handled, data loss prevention is robust, vertical compaction alignment is addressed. |
| **Testing** | Good coverage of unit-level logic. Integration tests with actual data would strengthen confidence. |
| **Code Quality** | Consistent with codebase style. Comments are helpful and accurate. |
| **Documentation** | Missing BE config documentation (required by project guidelines). |

**Overall Verdict:** Approve with minor changes (documentation required).
