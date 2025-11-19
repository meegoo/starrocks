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

#include <memory>
#include <string>
#include <vector>

#include "common/status.h"
#include "common/statusor.h"
#include "gen_cpp/lake_types.pb.h"

namespace starrocks::lake {

// Manages local persistence of autonomous compaction results.
// Results are stored in: {storage_root}/lake/compaction_results/tablet_{id}_result_{timestamp}_{random}.pb
class CompactionResultManager {
public:
    CompactionResultManager() = default;
    ~CompactionResultManager() = default;

    // Save a compaction result to local disk
    // Returns the file path where the result was saved
    StatusOr<std::string> save_result(const CompactionResultPB& result);

    // Load all compaction results for a specific tablet
    // Filters out results with base_version > max_version if max_version >= 0
    StatusOr<std::vector<CompactionResultPB>> load_results(int64_t tablet_id, int64_t max_version = -1);

    // Delete result file by path
    Status delete_result(const std::string& file_path);

    // Delete all result files for a tablet
    Status delete_tablet_results(int64_t tablet_id);

    // List all result file paths for a tablet
    StatusOr<std::vector<std::string>> list_result_files(int64_t tablet_id);

    // Get the compaction results directory for a specific storage root
    static std::string get_results_dir(const std::string& storage_root);

private:
    // Generate a unique filename for a compaction result
    static std::string generate_result_filename(int64_t tablet_id);

    // Get all storage roots configured in the system
    static std::vector<std::string> get_storage_roots();
};

} // namespace starrocks::lake


