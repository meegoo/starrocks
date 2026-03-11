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

#include <set>
#include <unordered_map>
#include <vector>

#include "common/statusor.h"
#include "storage/variant_tuple.h"

namespace starrocks {

// Segment-level key bound information for range split boundary calculation.
// Represents one segment's sort key range and data statistics.
struct SegmentKeyBound {
    VariantTuple min_key;
    VariantTuple max_key;
    int64_t num_rows = 0;
    int64_t data_size = 0;
};

// A range between two adjacent boundary points with estimated data statistics.
struct OrderedRangeInfo {
    VariantTuple min_key;
    VariantTuple max_key;
    int64_t estimated_num_rows = 0;
    int64_t estimated_data_size = 0;
    // Per-source statistics (e.g., per-rowset). Key semantics defined by caller.
    std::unordered_map<uint32_t, std::pair<int64_t, int64_t>> source_stats; // source_id -> (num_rows, data_size)
};

// Utilities for splitting data ranges by sort key boundaries.
// Shared by tablet resharding and parallel compaction range split.
class RangeSplitUtils {
public:
    // Build ordered ranges between adjacent boundary points from segment key bounds,
    // and estimate data distribution across those ranges.
    //
    // Each SegmentKeyBound optionally carries a source_id (e.g., rowset_id) for
    // per-source statistics tracking. Use source_ids.empty() to skip per-source tracking.
    //
    // Returns ordered ranges with estimated num_rows and data_size.
    static StatusOr<std::vector<OrderedRangeInfo>> build_ordered_ranges(
            const std::vector<SegmentKeyBound>& seg_bounds,
            const std::vector<uint32_t>& source_ids = std::vector<uint32_t>{});

    // Calculate split boundaries using a greedy algorithm.
    // Merges adjacent ordered ranges until reaching the target value per split.
    //
    // Parameters:
    //   ordered_ranges: ordered ranges from build_ordered_ranges()
    //   target_split_count: desired number of splits (N splits => N-1 boundaries)
    //   target_value_per_split: target data size (or row count) per split
    //   use_num_rows: if true, use num_rows as the split metric; otherwise use data_size
    //
    // Returns N-1 boundary VariantTuples for N splits, or empty if splitting is not possible.
    //
    // Improvements over the simple greedy approach:
    //   - Guarantees split_count boundaries when possible by forcing splits when
    //     remaining non-empty ranges <= remaining splits needed
    //   - Advances boundaries across empty ranges to maximize natural gaps
    static StatusOr<std::vector<VariantTuple>> calculate_split_boundaries(
            const std::vector<OrderedRangeInfo>& ordered_ranges, int32_t target_split_count,
            int64_t target_value_per_split, bool use_num_rows = false);
};

} // namespace starrocks
