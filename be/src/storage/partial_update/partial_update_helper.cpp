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

#include "storage/partial_update/partial_update_helper.h"

#include <algorithm>

namespace starrocks {

RowidGroupingResult group_by_source_rowid(const std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
    RowidGroupingResult result;
    result.sorted_pairs = pairs;
    // Stable sort by source_rowid so equal rowids preserve input (write) order,
    // which the overlay applies oldest -> newest, last-write-wins.
    std::stable_sort(result.sorted_pairs.begin(), result.sorted_pairs.end(),
                     [](const std::pair<uint32_t, uint32_t>& lhs, const std::pair<uint32_t, uint32_t>& rhs) {
                         return lhs.first < rhs.first;
                     });

    size_t i = 0;
    const size_t n = result.sorted_pairs.size();
    while (i < n) {
        const uint32_t rowid = result.sorted_pairs[i].first;
        size_t j = i + 1;
        while (j < n && result.sorted_pairs[j].first == rowid) {
            ++j;
        }
        result.groups.push_back(RowidGroup{rowid, i, j});
        i = j;
    }
    return result;
}

} // namespace starrocks
