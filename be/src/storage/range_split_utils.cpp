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

#include "storage/range_split_utils.h"

#include <algorithm>
#include <set>

namespace starrocks {

StatusOr<std::vector<OrderedRangeInfo>> RangeSplitUtils::build_ordered_ranges(
        const std::vector<SegmentKeyBound>& seg_bounds, const std::vector<uint32_t>& source_ids) {
    if (seg_bounds.empty()) {
        return std::vector<OrderedRangeInfo>{};
    }

    DCHECK(source_ids.empty() || source_ids.size() == seg_bounds.size());
    bool track_sources = !source_ids.empty();

    // Step 1: Collect all unique boundary points and sort them
    auto comparator = [](const VariantTuple* a, const VariantTuple* b) { return a->compare(*b) < 0; };
    std::set<const VariantTuple*, decltype(comparator)> ordered_boundaries(comparator);
    for (const auto& bound : seg_bounds) {
        ordered_boundaries.insert(&bound.min_key);
        ordered_boundaries.insert(&bound.max_key);
    }

    if (ordered_boundaries.size() < 2) {
        return std::vector<OrderedRangeInfo>{};
    }

    // Step 2: Build ordered ranges between adjacent boundary points
    std::vector<OrderedRangeInfo> ordered_ranges;
    ordered_ranges.reserve(ordered_boundaries.size());
    const VariantTuple* last_boundary = nullptr;
    for (const auto* boundary : ordered_boundaries) {
        if (last_boundary != nullptr) {
            OrderedRangeInfo range;
            range.min_key = *last_boundary;
            range.max_key = *boundary;
            ordered_ranges.push_back(std::move(range));
        }
        last_boundary = boundary;
    }

    if (ordered_ranges.empty()) {
        return ordered_ranges;
    }

    // Step 3: Estimate data distribution by distributing segment data proportionally
    // across overlapping ranges.
    //
    // For overlap detection:
    //   - Non-last ranges are treated as [min, max) to avoid double-counting on boundaries
    //   - The last range is treated as [min, max] to include the rightmost boundary
    for (size_t seg_idx = 0; seg_idx < seg_bounds.size(); seg_idx++) {
        const auto& seg_bound = seg_bounds[seg_idx];

        std::vector<OrderedRangeInfo*> overlapping;
        for (auto& range : ordered_ranges) {
            bool is_last = (&range == &ordered_ranges.back());
            if (is_last) {
                // Last range: [min, max] (inclusive both sides)
                if (!(range.max_key.compare(seg_bound.min_key) < 0 ||
                      range.min_key.compare(seg_bound.max_key) > 0)) {
                    overlapping.push_back(&range);
                }
            } else {
                // Non-last ranges: [min, max) (exclusive right)
                if (!(range.max_key.compare(seg_bound.min_key) <= 0 ||
                      range.min_key.compare(seg_bound.max_key) >= 0)) {
                    overlapping.push_back(&range);
                }
            }
        }

        if (overlapping.empty()) {
            continue;
        }

        auto count = static_cast<int64_t>(overlapping.size());

        // Distribute num_rows
        int64_t rows_per_range = seg_bound.num_rows / count;
        int64_t rows_remainder = seg_bound.num_rows % count;

        // Distribute data_size
        int64_t size_per_range = seg_bound.data_size / count;
        int64_t size_remainder = seg_bound.data_size % count;

        for (int64_t i = 0; i < count; i++) {
            int64_t delta_rows = rows_per_range + (i < rows_remainder ? 1 : 0);
            int64_t delta_size = size_per_range + (i < size_remainder ? 1 : 0);

            overlapping[i]->estimated_num_rows += delta_rows;
            overlapping[i]->estimated_data_size += delta_size;

            if (track_sources) {
                auto& stats = overlapping[i]->source_stats[source_ids[seg_idx]];
                stats.first += delta_rows;
                stats.second += delta_size;
            }
        }
    }

    return ordered_ranges;
}

StatusOr<std::vector<VariantTuple>> RangeSplitUtils::calculate_split_boundaries(
        const std::vector<OrderedRangeInfo>& ordered_ranges, int32_t target_split_count,
        int64_t target_value_per_split, bool use_num_rows) {
    if (ordered_ranges.empty() || target_split_count <= 1) {
        return std::vector<VariantTuple>{};
    }

    // Calculate total value and count non-empty ranges
    int64_t total_value = 0;
    size_t non_empty_ranges = 0;
    for (const auto& r : ordered_ranges) {
        int64_t val = use_num_rows ? r.estimated_num_rows : r.estimated_data_size;
        total_value += val;
        if (val > 0) {
            non_empty_ranges++;
        }
    }

    int32_t actual_split_count =
            std::min(target_split_count, static_cast<int32_t>(ordered_ranges.size()));
    if (actual_split_count <= 1) {
        return std::vector<VariantTuple>{};
    }

    // Need enough non-empty ranges to form actual_split_count splits
    if (non_empty_ranges < static_cast<size_t>(actual_split_count)) {
        return std::vector<VariantTuple>{};
    }

    int64_t actual_target = total_value / actual_split_count;
    if (target_value_per_split > 0) {
        actual_target = std::min(actual_target, target_value_per_split);
    }
    actual_target = std::max<int64_t>(1, actual_target);

    std::vector<VariantTuple> boundaries;
    int64_t accumulated = 0;
    size_t remaining_non_empty = non_empty_ranges;

    for (size_t i = 0; i < ordered_ranges.size(); i++) {
        const auto& range = ordered_ranges[i];
        int64_t val = use_num_rows ? range.estimated_num_rows : range.estimated_data_size;
        bool is_non_empty = val > 0;

        accumulated += val;

        bool is_last_range = (i == ordered_ranges.size() - 1);
        int32_t remaining_splits = actual_split_count - 1 - static_cast<int32_t>(boundaries.size());

        if (!is_last_range && remaining_splits > 0 && is_non_empty &&
            (accumulated >= actual_target ||
             remaining_non_empty <= static_cast<size_t>(remaining_splits))) {
            // Advance boundary across trailing empty ranges to maximize natural gaps
            const VariantTuple* boundary = &range.max_key;
            for (size_t j = i + 1; j < ordered_ranges.size(); j++) {
                int64_t next_val = use_num_rows ? ordered_ranges[j].estimated_num_rows
                                                : ordered_ranges[j].estimated_data_size;
                if (next_val > 0 || j == ordered_ranges.size() - 1) {
                    break;
                }
                boundary = &ordered_ranges[j].max_key;
                // Skip these empty ranges in the outer loop
                i = j;
            }

            boundaries.push_back(*boundary);
            accumulated = 0;

            if (static_cast<int32_t>(boundaries.size()) >= actual_split_count - 1) {
                break;
            }
        }

        if (is_non_empty) {
            remaining_non_empty--;
        }
    }

    return boundaries;
}

} // namespace starrocks
