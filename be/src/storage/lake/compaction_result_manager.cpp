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

#include "storage/lake/compaction_result_manager.h"

#include <fmt/format.h>

#include <chrono>
#include <random>

#include "common/logging.h"
#include "fs/fs_util.h"
#include "gutil/strings/substitute.h"
#include "storage/data_dir.h"
#include "storage/storage_engine.h"
#include "util/raw_container.h"

namespace starrocks::lake {

// P7 fix: Use thread-local static generator for better performance
thread_local std::mt19937 g_random_gen(std::random_device{}());

std::string CompactionResultManager::get_results_dir(const std::string& storage_root) {
    return fmt::format("{}/lake/compaction_results", storage_root);
}

std::string CompactionResultManager::generate_result_filename(int64_t tablet_id) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // Use static thread-local generator instead of creating new one each time
    std::uniform_int_distribution<> dis(0, 999999);
    int random_suffix = dis(g_random_gen);

    return fmt::format("tablet_{}_result_{}_{}.pb", tablet_id, timestamp, random_suffix);
}

std::vector<std::string> CompactionResultManager::get_storage_roots() {
    std::vector<std::string> roots;
    auto* engine = StorageEngine::instance();
    if (engine == nullptr) {
        return roots;
    }

    for (const auto& data_dir : engine->get_stores()) {
        roots.push_back(data_dir->path());
    }
    return roots;
}

StatusOr<std::string> CompactionResultManager::save_result(const CompactionResultPB& result) {
    if (!result.has_tablet_id()) {
        return Status::InvalidArgument("tablet_id is required");
    }

    // Use the first storage root (typically the primary data directory)
    auto roots = get_storage_roots();
    if (roots.empty()) {
        return Status::InternalError("No storage root found");
    }

    std::string results_dir = get_results_dir(roots[0]);

    // Ensure the results directory exists
    RETURN_IF_ERROR(fs::create_directories(results_dir));

    // Generate unique filename
    std::string filename = generate_result_filename(result.tablet_id());
    std::string file_path = fmt::format("{}/{}", results_dir, filename);

    // Serialize and write to file
    std::string serialized;
    if (!result.SerializeToString(&serialized)) {
        return Status::InternalError("Failed to serialize CompactionResultPB");
    }

    RETURN_IF_ERROR(fs::write_file(file_path, serialized));

    LOG(INFO) << "Saved compaction result for tablet " << result.tablet_id() << " to " << file_path
              << ", base_version=" << result.base_version();

    return file_path;
}

StatusOr<std::vector<CompactionResultPB>> CompactionResultManager::load_results(int64_t tablet_id,
                                                                                 int64_t max_version) {
    std::vector<CompactionResultPB> results;

    auto roots = get_storage_roots();
    for (const auto& root : roots) {
        std::string results_dir = get_results_dir(root);

        // Check if directory exists
        auto exists_or = fs::path_exist(results_dir);
        if (!exists_or.ok() || !exists_or.value()) {
            continue;
        }

        // List all files in the directory
        std::vector<std::string> files;
        ASSIGN_OR_RETURN(files, fs::list_dirs_files(results_dir));

        // Filter files for this tablet
        std::string tablet_prefix = fmt::format("tablet_{}_result_", tablet_id);
        for (const auto& file : files) {
            if (file.find(tablet_prefix) == 0 && file.ends_with(".pb")) {
                std::string file_path = fmt::format("{}/{}", results_dir, file);

                // Read and deserialize
                std::string content;
                ASSIGN_OR_RETURN(content, fs::read_file(file_path));

                CompactionResultPB result;
                if (!result.ParseFromString(content)) {
                    LOG(WARNING) << "Failed to parse compaction result from " << file_path << ", skipping";
                    continue;
                }

                // Filter by version if specified
                if (max_version >= 0 && result.base_version() > max_version) {
                    LOG(INFO) << "Skipping result from " << file_path << " with base_version=" << result.base_version()
                              << " > max_version=" << max_version;
                    continue;
                }

                results.push_back(std::move(result));
            }
        }
    }

    LOG(INFO) << "Loaded " << results.size() << " compaction results for tablet " << tablet_id;
    return results;
}

Status CompactionResultManager::delete_result(const std::string& file_path) {
    auto st = fs::delete_file(file_path);
    if (st.ok()) {
        LOG(INFO) << "Deleted compaction result file: " << file_path;
    } else {
        LOG(WARNING) << "Failed to delete compaction result file " << file_path << ": " << st;
    }
    return st;
}

Status CompactionResultManager::delete_tablet_results(int64_t tablet_id) {
    auto files_or = list_result_files(tablet_id);
    if (!files_or.ok()) {
        return files_or.status();
    }

    int deleted_count = 0;
    for (const auto& file_path : files_or.value()) {
        auto st = fs::delete_file(file_path);
        if (st.ok()) {
            deleted_count++;
        } else {
            LOG(WARNING) << "Failed to delete result file " << file_path << ": " << st;
        }
    }

    LOG(INFO) << "Deleted " << deleted_count << " compaction result files for tablet " << tablet_id;
    return Status::OK();
}

StatusOr<std::vector<std::string>> CompactionResultManager::list_result_files(int64_t tablet_id) {
    std::vector<std::string> result_files;

    auto roots = get_storage_roots();
    for (const auto& root : roots) {
        std::string results_dir = get_results_dir(root);

        // Check if directory exists
        auto exists_or = fs::path_exist(results_dir);
        if (!exists_or.ok() || !exists_or.value()) {
            continue;
        }

        // List all files for this tablet
        std::vector<std::string> files;
        ASSIGN_OR_RETURN(files, fs::list_dirs_files(results_dir));

        std::string tablet_prefix = fmt::format("tablet_{}_result_", tablet_id);
        for (const auto& file : files) {
            if (file.find(tablet_prefix) == 0 && file.ends_with(".pb")) {
                result_files.push_back(fmt::format("{}/{}", results_dir, file));
            }
        }
    }

    return result_files;
}

} // namespace starrocks::lake


