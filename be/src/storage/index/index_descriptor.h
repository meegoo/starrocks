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

#include <fmt/format.h>

#include <cstdint>
#include <string>

#include "storage/olap_common.h"

namespace starrocks {
class IndexDescriptor {
public:
    inline static const std::string mark_word = "TENANNEMPTYMARK";
    inline static const int64_t mark_word_len = 15;

    static StatusOr<std::string> get_index_file_path(const IndexType index_type, const std::string& rowset_dir,
                                                     const std::string& rowset_id, int segment_id, int64_t index_id) {
        switch (index_type) {
        case VECTOR:
            return vector_index_file_path(rowset_dir, rowset_id, segment_id, index_id);
        case GIN:
            return inverted_index_file_path(rowset_dir, rowset_id, segment_id, index_id);
        default:
            return Status::NotSupported("Not supported");
        }
    }

    // Standalone bitmap index file path derived from segment file path and column unique id.
    // E.g., segment path "dir/rowset_0.dat" -> "dir/rowset_0_<col_uid>.bmi"
    static std::string bitmap_index_file_path(const std::string& segment_path, uint32_t column_unique_id) {
        auto dot_pos = segment_path.rfind('.');
        auto base = (dot_pos == std::string::npos) ? segment_path : segment_path.substr(0, dot_pos);
        return fmt::format("{}_{}.bmi", base, column_unique_id);
    }

    // Standalone bloom filter index file path derived from segment file path and column unique id.
    // E.g., segment path "dir/rowset_0.dat" -> "dir/rowset_0_<col_uid>.bfi"
    static std::string bloom_filter_index_file_path(const std::string& segment_path, uint32_t column_unique_id) {
        auto dot_pos = segment_path.rfind('.');
        auto base = (dot_pos == std::string::npos) ? segment_path : segment_path.substr(0, dot_pos);
        return fmt::format("{}_{}.bfi", base, column_unique_id);
    }

    static std::string inverted_index_file_path(const std::string& rowset_dir, const std::string& rowset_id,
                                                int segment_id, int64_t index_id) {
        // inverted index is a directory, it's path likes below
        // {rowset_dir}/{schema_hash}/{rowset_id}_{seg_num}_{index_id}
        return fmt::format("{}/{}_{}_{}.{}", rowset_dir, rowset_id, segment_id, index_id, "ivt");
    }

    static std::string vector_index_file_path(const std::string& rowset_dir, const std::string& rowset_id,
                                              int segment_id, int64_t index_id) {
        // {rowset_dir}/{schema_hash}/{rowset_id}_{seg_num}_{index_id}.vi
        return fmt::format("{}/{}_{}_{}.{}", rowset_dir, rowset_id, segment_id, index_id, "vi");
    }

    static const std::string get_temporary_null_bitmap_file_name() { return "null_bitmap"; }
};

} // namespace starrocks