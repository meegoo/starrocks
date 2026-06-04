// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace starrocks {

// SDCG (Sparse Delta Column Group) prototype helper.
//
// This is an intentionally minimal, dependency-light unit that exists to prove
// out the storage/partial_update module boundary. It carries no storage/rowset,
// fs, or runtime logic yet; it only models a pure data-shaping step that the
// real sparse-overlay write path needs.
//
// A sparse layer for one updated column is described as a list of
// (source_rowid, value_token) pairs. When several sparse columns are written in
// the same partial-update transaction, the write path groups source rowids into
// equivalence classes so that rows touched by an identical set of layers can be
// emitted together. The grouping below is the pure, testable core of that step:
// it partitions the input pairs into contiguous runs that share the same
// source_rowid, returning, for each distinct source_rowid (in ascending order),
// the [begin, end) index range into a stably-sorted copy of the input.
struct RowidGroup {
    uint32_t source_rowid = 0;
    // [begin, end) into the sorted pairs returned by group_by_source_rowid.
    size_t begin = 0;
    size_t end = 0;
};

struct RowidGroupingResult {
    // Input pairs sorted by source_rowid (stable: equal rowids keep input order).
    std::vector<std::pair<uint32_t, uint32_t>> sorted_pairs;
    // Equivalence classes over source_rowid, in ascending source_rowid order.
    std::vector<RowidGroup> groups;
};

// Partition `pairs` (source_rowid, value_token) into per-source_rowid groups.
// Pure function: no I/O, no global state. Deterministic and stable.
RowidGroupingResult group_by_source_rowid(const std::vector<std::pair<uint32_t, uint32_t>>& pairs);

} // namespace starrocks
