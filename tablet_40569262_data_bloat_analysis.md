# Tablet 40569262 Data Bloat Analysis Report

## 1. Problem Description

Tablet 40569262 has a total data size of 41.8GB, but actual valid data files account for only ~3.4GB. Analysis reveals two compounding issues: delete vector metadata leakage preventing 38.7GB of space from being reclaimed, and a compaction scheduling policy defect that leaves high-deletion-rate rowsets uncompacted indefinitely.

## 2. Raw Data

### 2.1 Tablet Overview

| Metric | Value |
|---|---|
| TabletId | 40569262 |
| BackendId | 248902028 |
| DataSize | 41.8GB |
| RowCount | 101,799,619 |
| MinVersion | 0 |

### 2.2 Delvec Metadata Statistics

```
$ jq '.delvec_meta.delvecs | length' 40569262.json
12964

$ jq '.delvec_meta.version_to_file | length' 40569262.json
9419

$ jq '[.delvec_meta.delvecs[].value.version] | unique | length' 40569262.json
9419

$ jq '[.delvec_meta.version_to_file[].value.size] | add' 40569262.json
41569279586
```

| Metric | Value |
|---|---|
| delvecs entry count | 12,964 |
| version_to_file entry count | 9,419 |
| Unique versions referenced by delvecs | 9,419 |
| Total delvec file size | 41,569,279,586 bytes (38.7GB) |

### 2.3 version_to_file Sample

```json
"delvec_meta": {
    "version_to_file": [
        {
            "key": 1222323,
            "value": {
                "name": "0000000006493062_8f614b47-5caf-489d-9ef4-8370013028bc.delvec",
                "size": 8999771
            }
        },
        {
            "key": 310069,
            "value": {
                "name": "0000000001f56740_a1d5b5d1-f47a-4c03-b7dc-fd8865f86d09.delvec",
                "size": 6455139
            }
        }
    ]
}
```

### 2.4 delvecs Sample

```json
"delvecs": [
    {
        "key": 717882,
        "value": {
            "version": 723899,
            "offset": 1274140,
            "size": 79,
            "crc32c": 1274320653,
            "crc32c_gen_version": 723899
        }
    },
    {
        "key": 786793,
        "value": {
            "version": 793435,
            "offset": 1823723,
            "size": 261,
            "crc32c": 782734388,
            "crc32c_gen_version": 793435
        }
    }
]
```

### 2.5 Current Rowsets

```
$ jq '[.rowsets[] | {id: .id, seg_count: (.segments | length)}] | .[] |
  "\(.id) ~ \(.id + .seg_count - 1)"' 40569262.json

"712562 ~ 712562"
"712563 ~ 712564"
"727581 ~ 727581"
"893003 ~ 893003"
"921892 ~ 921892"
"940995 ~ 940995"
"1030482 ~ 1030482"
"1049077 ~ 1049077"
"1115866 ~ 1115866"
"1235992 ~ 1235992"
"1238573 ~ 1238573"
"1278777 ~ 1278777"
"1321583 ~ 1321583"
"1340735 ~ 1340735"
"1343202 ~ 1343202"
"1343437 ~ 1343437"
"1343679 ~ 1343679"
"1343890 ~ 1343890"
"1343966 ~ 1343966"
"1344127 ~ 1344127"
"1344260 ~ 1344260"
"1344321 ~ 1344321"
"1344482 ~ 1344482"
"1344514 ~ 1344514"
"1344521 ~ 1344521"
"1344533 ~ 1344533"
"1344545 ~ 1344545"
"1344553 ~ 1344553"
"1344561 ~ 1344561"
"1344565 ~ 1344565"
"1344566 ~ 1344566"
"1344567 ~ 1344567"
"1344568 ~ 1344568"
```

There are 34 valid segment IDs in total (33 rowsets, where rowset 712563 contains 2 segments).

### 2.6 Rowset Details (with Deletion Statistics)

| Rowset ID | Segments | num_rows | data_size | num_dels | del% | version |
|---|---|---|---|---|---|---|
| 712562 | 1 | 17,729,787 | 397MB | 15,420,598 | 87% | 718549 |
| 712563 | 2 | 80,909,727 | 1,497MB | 41,423,289 | 51% | 718550 |
| 727581 | 1 | 5,472,780 | 119MB | 3,604,236 | 66% | 733691 |
| 893003 | 1 | 3,486,866 | 68MB | 469,815 | 13% | 900271 |
| 921892 | 1 | 9,037,015 | 182MB | 673,324 | 7% | 929365 |
| 940995 | 1 | 2,569,718 | 56MB | 632,930 | 25% | 948503 |
| 1030482 | 1 | 2,192,381 | 50MB | 272,783 | 12% | 1038386 |
| 1049077 | 1 | 5,596,401 | 130MB | 534,965 | 10% | 1057287 |
| 1115866 | 1 | 5,396,958 | 108MB | 743,403 | 14% | 1124261 |
| 1235992 | 1 | 4,641,600 | 101MB | 591,484 | 13% | 1245347 |
| 1238573 | 1 | 22,110,370 | 499MB | 1,317,431 | 6% | 1247931 |
| 1278777 | 1 | 3,916,642 | 70MB | 539,324 | 14% | 1288310 |
| 1321583 | 1 | 1,011,088 | 22MB | 252,073 | 25% | 1332112 |
| 1340735 | 1 | 557,181 | 12MB | 23,788 | 4% | 1351839 |
| 1343202 | 1 | 3,552,297 | 68MB | 20,347 | 1% | 1354324 |
| 1343437~1344568 | 1 each | (small rowsets) | <1MB~0.4MB | <6K | low | 1354557~1355692 |

### 2.7 SSTable Metadata

```json
"sstable_meta": {
    "sstables": [
        {
            "filename": "6149d96c-7279-4ee9-be9d-6480b5323df1.sst",
            "filesize": 1362584047,
            "max_rss_rowid": 5769008665206783
        }
    ]
}
```

### 2.8 Space Distribution Summary

| Component | Size | Proportion |
|---|---|---|
| delvec files | 38.7 GB | **93%** |
| Data files (sum of rowsets data_size) | ~3.4 GB | 8% |
| SSTable (PK Index) | ~1.3 GB | 3% |

---

## 3. Issue One: Orphaned Delvec Entries Preventing Space Reclamation

### 3.1 Data Comparison

| Metric | Actual Value | Expected Value |
|---|---|---|
| delvecs entry count | **12,964** | ≤34 (current segment count) |
| version_to_file entry count | **9,419** | ≤34 |
| Orphaned delvec entries (estimated) | **~12,930** | 0 |

The keys in delvecs are segment_ids. For example, key=717882 and key=786793 **do not belong to any current rowset**:
- Rowset 712563's segment range is [712563, 712564]
- Rowset 727581's segment is [727581]
- 717882 and 786793 fall between these two, belonging to old rowsets that have been removed by compaction

These orphaned delvec entries reference 9,419 distinct versions, preventing the corresponding .delvec files in version_to_file from being garbage collected.

### 3.2 Relevant Data Structures

Proto definition (`gensrc/proto/lake_types.proto`):

```protobuf
message DelvecMetadataPB {
    // version → delvec file metadata (filename, file size)
    map<int64, FileMetaPB> version_to_file = 1;
    // segment_id → delvec location within file (version, offset, size, crc32c)
    map<uint32, DelvecPagePB> delvecs = 2;
}
```

Lookup flow: `segment_id → delvecs[segment_id].version → version_to_file[version].name → read delvec data using offset+size`

GC mechanism (`meta_file.cpp:501-519`): Only cleans up version_to_file entries whose versions are not referenced by any delvecs entry. There is no cleanup logic for delvecs itself based on rowset validity.

### 3.3 Root Cause: _finalize_delvec Re-inserts Deleted Delvec Entries After Compaction Removal

`MetaFileBuilder` stores delvec information in two places:

1. **`_delvecs`** (member variable, `meta_file.h:88`): Written by `append_delvec()`, temporarily holds delvecs generated in the current publish batch
2. **`_tablet_meta->delvec_meta().delvecs()`** (protobuf metadata): Persisted delvec metadata

Bug trigger condition: **Within the same publish batch, a Write TxnLog is applied before a Compaction TxnLog, and the Write generates a delvec for a segment that is about to be compacted.**

### 3.4 Step-by-Step Code Trace

**Precondition:** Publish batch from base_version → new_version, containing Write TxnLog T1 and Compaction TxnLog T2 (compacting rowset R, which contains segment S). T1 < T2, applied in order (`txn_log_applier.cpp:149-173`).

---

**Step 1: Apply Write T1** (`update_manager.cpp:214-354`, `publish_primary_key_tablet`)

The write ingests new data, and the primary key index detects conflicting rows in old segment S:

```cpp
// update_manager.cpp:268-269
RETURN_IF_ERROR(_do_update(rowset_id, segment_id, ..., index, &new_deletes));
// new_deletes[S] = {list of deleted row numbers}
```

Retrieves segment S's old delvec, merges new deletions, and generates an updated delvec:

```cpp
// update_manager.cpp:316-319
RETURN_IF_ERROR(get_del_vec(tsid, base_version, builder, false, &old_del_vec));
old_del_vec->add_dels_as_new_version(new_delete.second, metadata->version(), &(new_del_vecs[idx].second));
```

Calls `append_delvec` to store it in MetaFileBuilder's member variable `_delvecs`:

```cpp
// update_manager.cpp:353-354
for (auto&& each : new_del_vecs) {
    builder->append_delvec(each.second, each.first);  // each.first = S
}

// meta_file.cpp:47-59  append_delvec implementation
void MetaFileBuilder::append_delvec(const DelVectorPtr& delvec, uint32_t segment_id) {
    // ... serialize to _buf, record offset/size ...
    _delvecs[segment_id].set_offset(offset);   // ← writes to member variable _delvecs
    _delvecs[segment_id].set_size(size);
}
```

**State at this point:**
- `_delvecs[S]` = new delvec page ✅
- `_tablet_meta->delvec_meta().delvecs()[S]` = old entry, unmodified

---

**Step 2: Apply Compaction T2** (`meta_file.cpp:282-408`, `apply_opcompaction`)

Compaction removes input rowset R and its segment S's delvec entry:

```cpp
// meta_file.cpp:306
delete_delvec_sid_range.emplace_back(it->id(), it->id() + std::max(it->segments_size(), 1) - 1);
// range includes segment S

// meta_file.cpp:330-334
auto delvecs = _tablet_meta->mutable_delvec_meta()->mutable_delvecs();
int delvec_erase_cnt = delete_from_protobuf_map<T_DELVEC>(delvecs, delete_delvec_sid_range, ...);
// Deletes S from _tablet_meta->delvec_meta().delvecs() ✅
```

**Critical defect: `apply_opcompaction` does not clean up corresponding entries in the member variable `_delvecs`.**

**State at this point:**
- `_delvecs[S]` = new delvec page ✅ **still present**
- `_tablet_meta->delvec_meta().delvecs()[S]` ❌ **already deleted**
- Rowset R has been removed from `_tablet_meta->rowsets()`

---

**Step 3: finalize() → _finalize_delvec()** (`meta_file.cpp:458-519`)

**Phase 1 (line 462-473)**: Iterates over existing entries in `_tablet_meta->delvec_meta().delvecs()`. If a matching segment_id is found in `_delvecs`, updates it and erases from `_delvecs`:

```cpp
for (auto&& each_delvec : *(_tablet_meta->mutable_delvec_meta()->mutable_delvecs())) {
    auto iter = _delvecs.find(each_delvec.first);
    if (iter != _delvecs.end()) {
        // update existing entry ...
        _delvecs.erase(iter);  // remove from _delvecs
    }
}
```

**S was already deleted from `_tablet_meta->delvec_meta().delvecs()` in Step 2, so it does not appear in this iteration. Therefore `_delvecs[S]` is not erased and remains.**

**Phase 2 (line 476-483)**: Inserts **remaining** entries in `_delvecs` as new entries into `_tablet_meta->delvec_meta().delvecs()`:

```cpp
for (auto&& each_delvec : _delvecs) {
    each_delvec.second.set_version(version);
    (*_tablet_meta->mutable_delvec_meta()->mutable_delvecs())[each_delvec.first] = each_delvec.second;
    //                                                         ^^^^^^^^^^^^^^^^
    //                                                         S is re-inserted! 🐛
}
```

**S is re-inserted as a "new entry" into metadata. But S's corresponding rowset R no longer exists — an orphan is born.**

**Phase 4 (line 501-519)**: GC checks `version_to_file`:

```cpp
std::set<int64_t> refered_versions;
for (const auto& item : _tablet_meta->delvec_meta().delvecs()) {
    refered_versions.insert(item.second.version());  // S was just inserted, its version is in the set
}
// Only deletes version_to_file entries not referenced → S's version will not be deleted
```

---

**Step 4: put_metadata Persists**

The orphaned entry is persisted along with the tablet metadata, and all subsequent versions inherit this orphan.

### 3.5 Cumulative Effect

Every time a publish batch contains "Write before Compaction, where the Write generates a delvec for a segment about to be compacted," a new orphaned entry is created. This tablet's version has exceeded 1.35 million. Over prolonged operation, ~12,930 orphaned entries have accumulated, referencing 9,419 version_to_file entries, corresponding to 38.7GB of .delvec files that can never be garbage collected.

### 3.6 Fix Direction

When `apply_opcompaction` deletes metadata delvec entries, it should also clean up entries in the `_delvecs` member variable for the same segment range:

```cpp
// In meta_file.cpp apply_opcompaction, add after delete_from_protobuf_map:
for (const auto& range : delete_delvec_sid_range) {
    for (auto it = _delvecs.begin(); it != _delvecs.end();) {
        if (it->first >= range.first && it->first <= range.second) {
            it = _delvecs.erase(it);
        } else {
            ++it;
        }
    }
}
```

---

## 4. Issue Two: Compaction Scheduling Fails to Clean Up High-Deletion-Rate Rowsets

### 4.1 Symptoms

Multiple rowsets have extremely high deletion rates but have not been cleaned up by compaction for an extended period:

| Rowset ID | data_size | num_rows | num_dels | del% |
|---|---|---|---|---|
| **712562** | 397MB | 17,729,787 | 15,420,598 | **87%** |
| **727581** | 119MB | 5,472,780 | 3,604,236 | **66%** |
| **712563** | 1,497MB | 80,909,727 | 41,423,289 | **51%** |
| 940995 | 56MB | 2,569,718 | 632,930 | 25% |
| 1321583 | 22MB | 1,011,088 | 252,073 | 25% |

Rowsets 712562 (87%), 727581 (66%), and 712563 (51%) have more than half their rows deleted. Rowset 712562 has only 13% valid data, yet compaction has never been triggered for it.

### 4.2 Compaction Selection Logic Analysis

Code location: `be/src/storage/lake/primary_key_compaction_policy.cpp`

**Gate 1: min_input_segment_check (line 82-93)**

```cpp
bool min_input_segment_check(const std::shared_ptr<const TabletMetadataPB>& tablet_metadata) {
    int64_t total_segment_cnt = 0;
    for (const auto& rowset : tablet_metadata->rowsets()) {
        total_segment_cnt += rowset.overlapped() ? rowset.segments_size() : 1;
        if (total_segment_cnt >= config::lake_pk_compaction_min_input_segments) {  // default 5
            return true;
        }
    }
    return false;
}
```

This tablet has 33 rowsets, all with `overlapped=false`, counting as 33 ≥ 5. **Passes.**

**Gate 2: Score Calculation (primary_key_compaction_policy.h:47-64)**

```cpp
double io_count() const {
    double cnt = rowset_meta_ptr->overlapped() ? std::max(segments_size(), 1) : 1;
    if (stat.num_dels > 0) {
        cnt *= config::update_compaction_delvec_file_io_amp_ratio;  // default 2
    }
    return cnt;
}

double delete_bytes() const {
    return (double)stat.bytes * ((double)stat.num_dels / (double)stat.num_rows);
}

double read_bytes() const { return (double)stat.bytes - delete_bytes() + 1; }

void calculate_score() { score = (io_count() * 1024 * 1024) / read_bytes(); }
```

Calculated results for each rowset:

| Rowset ID | data_size | del% | io_count | delete_bytes | read_bytes | **score** |
|---|---|---|---|---|---|---|
| 712562 | 397MB | 87% | 1×2=2 | 345MB | 52MB | 0.038 |
| 727581 | 119MB | 66% | 1×2=2 | 78MB | 41MB | 0.049 |
| 712563 | 1,497MB | 51% | 1×2=2 | 764MB | 733MB | 0.0027 |
| 1238573 | 499MB | 6% | 1×2=2 | 30MB | 469MB | 0.0043 |
| 921892 | 182MB | 7% | 1×2=2 | 14MB | 169MB | 0.012 |
| Small rowsets (~0.3MB) | ~0.3MB | low | 1 | ~0 | ~0.3MB | **~3.3** |

**Key finding: score = io_count / read_bytes. The larger the read_bytes, the lower the score.** For all non-overlapped single-segment rowsets, io_count is at most 2 (`1 × delvec_amp_ratio`), while read_bytes scales linearly with data_size. Small rowsets (read_bytes < 1MB) can achieve a score of ~3.3, which is **87x higher** than rowset 712562 (score 0.038) and **1,222x higher** than rowset 712563 (score 0.0027).

**Gate 3: Size-Tiered Level Grouping (line 22-75)**

Rowsets are sorted by `read_bytes` in descending order, then grouped into tiers using `size_tiered_level_multiple=5`. Adjacent rowsets are placed in different tiers when read_bytes differs by more than 5x:

- **Level A**: 712563 (read_bytes ≈ 733MB) — occupies a tier alone
- **Level B**: 1238573 (469MB), 921892 (169MB), 1049077 (118MB), 1115866 (93MB), 1235992 (87MB), 1278777 (40MB), 1343202 (67MB), and other medium rowsets
- **Level C**: 712562 (52MB), 727581 (41MB) may share a tier with some medium rowsets, or form a separate tier
- **Level D**: Trailing 14 small rowsets (each <1MB) — clustered into one tier

The level with the **highest cumulative score** is selected. Because small rowsets have extremely high scores (due to tiny read_bytes), Level D's **cumulative score far exceeds all other levels**, and it is always selected as the compaction target.

Special handling (line 67-71): If the highest-scoring level contains only 1 non-overlapped rowset, it is merged with the second-highest-scoring level. However, Level D has 14 rowsets, so this logic is not triggered.

**Gate 4: Input Rowset Selection (line 129-150)**

```cpp
if (cur_compaction_result_bytes > std::max(
        config::update_compaction_result_bytes,           // default 1GB
        compaction_data_size_threshold))                  // tablet_data_size * 0.5 ≈ 1.7GB
    break;
```

Since Level D (small rowsets) always wins the level selection, the rowsets selected for compaction are always small rowsets. The levels containing 712562, 727581, and 712563 have far lower cumulative scores than Level D and never get a compaction opportunity.

### 4.3 Root Cause Summary

High-deletion-rate large rowsets (712562/727581/712563) cannot be compacted for an extended period because:

1. **The score formula inherently penalizes large rowsets**: `score = io_count / read_bytes`. For non-overlapped single-segment rowsets, io_count is at most 2, while read_bytes grows linearly with data_size. Even at 87% deletion rate (712562), the score remains far lower than KB-sized small rowsets.

2. **Size-tiered grouping isolates rowsets**: Large, medium, and small rowsets are assigned to different tiers. Compaction selects only the single tier with the highest cumulative score, preventing cross-tier merging.

3. **Small rowsets continuously monopolize compaction opportunities**: Frequent writes continuously produce small rowsets. These small rowsets have extremely high scores, and their tier is always the top compaction priority. High-deletion-rate large rowsets are perpetually "starved."

4. **Vicious cycle**: Large rowsets cannot compact → deletions continue to accumulate → delvecs continue to grow → small rowsets compact into new medium rowsets → deletions begin accumulating on the new rowsets.

### 4.4 Improvement Direction

The current compaction strategy is oriented toward IO read amplification (`io_count / read_bytes`) and lacks consideration for space reclamation. Possible improvements include:

- Adding extra score weighting for rowsets with deletion rates exceeding a threshold (e.g., 50%), giving high-deletion-rate rowsets higher priority
- Or introducing an independent space reclaim compaction strategy specifically for high-deletion-rate rowsets, which does not compete with regular compaction under the same scoring system

---

## 5. Summary

This tablet suffers from two compounding issues that together cause 93% of its 41.8GB data to be wasted space:

| Issue | Root Cause | Impact |
|---|---|---|
| **Delvec Orphan Bug** | When `apply_opcompaction` deletes entries from metadata delvecs, it does not clean up the corresponding entries in MetaFileBuilder's member variable `_delvecs`, causing `_finalize_delvec` to re-insert deleted entries | ~12,930 orphaned entries referencing 9,419 version_to_file entries; 38.7GB of delvec files cannot be GC'd |
| **Compaction Scheduling Defect** | The score formula `io_count / read_bytes` gives large rowsets extremely low scores; size-tiered grouping isolates large and small rowsets; small rowsets continuously monopolize compaction opportunities | Rowsets with 87%/66%/51% deletion rates (712562/727581/712563) cannot be compacted for extended periods; deletions continue accumulating; space cannot be reclaimed |
