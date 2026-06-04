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

#include <gtest/gtest.h>

namespace starrocks {

TEST(PartialUpdateHelperTest, EmptyInput) {
    auto result = group_by_source_rowid({});
    EXPECT_TRUE(result.sorted_pairs.empty());
    EXPECT_TRUE(result.groups.empty());
}

TEST(PartialUpdateHelperTest, SingleRowid) {
    auto result = group_by_source_rowid({{7, 100}});
    ASSERT_EQ(1u, result.groups.size());
    EXPECT_EQ(7u, result.groups[0].source_rowid);
    EXPECT_EQ(0u, result.groups[0].begin);
    EXPECT_EQ(1u, result.groups[0].end);
}

TEST(PartialUpdateHelperTest, GroupsAreAscendingAndContiguous) {
    // Unsorted input with duplicate source rowids.
    auto result = group_by_source_rowid({{5, 1}, {2, 2}, {5, 3}, {2, 4}, {9, 5}});
    ASSERT_EQ(3u, result.groups.size());

    EXPECT_EQ(2u, result.groups[0].source_rowid);
    EXPECT_EQ(0u, result.groups[0].begin);
    EXPECT_EQ(2u, result.groups[0].end);

    EXPECT_EQ(5u, result.groups[1].source_rowid);
    EXPECT_EQ(2u, result.groups[1].begin);
    EXPECT_EQ(4u, result.groups[1].end);

    EXPECT_EQ(9u, result.groups[2].source_rowid);
    EXPECT_EQ(4u, result.groups[2].begin);
    EXPECT_EQ(5u, result.groups[2].end);
}

TEST(PartialUpdateHelperTest, StableOrderWithinGroupPreservesWriteOrder) {
    // For equal source rowids, value_token input order must be preserved so the
    // overlay can apply oldest -> newest with last-write-wins semantics.
    auto result = group_by_source_rowid({{3, 11}, {3, 22}, {3, 33}});
    ASSERT_EQ(1u, result.groups.size());
    ASSERT_EQ(3u, result.sorted_pairs.size());
    EXPECT_EQ(11u, result.sorted_pairs[0].second);
    EXPECT_EQ(22u, result.sorted_pairs[1].second);
    EXPECT_EQ(33u, result.sorted_pairs[2].second);
}

} // namespace starrocks
