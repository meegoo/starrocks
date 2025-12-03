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

#include "storage/lake/autonomous_compaction_publisher.h"

#include <unordered_map>
#include <unordered_set>

#include "common/logging.h"
#include "gutil/strings/substitute.h"
#include "storage/lake/compaction_result_manager.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/transactions.h"

namespace starrocks::lake {

AutonomousCompactionPublisher::AutonomousCompactionPublisher(TabletManager* tablet_mgr)
        : _tablet_mgr(tablet_mgr), _result_mgr(std::make_unique<CompactionResultManager>()) {}

AutonomousCompactionPublisher::~AutonomousCompactionPublisher() = default;

Status AutonomousCompactionPublisher::process_request(const CompactRequest* request, CompactResponse* response) {
    if (!request->publish_autonomous()) {
        return Status::InvalidArgument("Not a PUBLISH_AUTONOMOUS request");
    }

    LOG(INFO) << "Processing PUBLISH_AUTONOMOUS request for "
              << request->tablet_ids_size() << " tablets, txn_id="
              << request->txn_id() << ", version=" << request->version()
              << ", visible_version=" << request->visible_version();

    int64_t txn_id = request->txn_id();
    int64_t base_version = request->version(); // This is the base version
    int64_t new_version = request->version() + 1; // Increment to get new version
    int64_t visible_version = request->has_visible_version() ? request->visible_version() : base_version;

    // Phase 1: Load compaction results for all tablets
    std::unordered_map<int64_t, std::vector<CompactionResultPB>> tablet_results;
    std::unordered_map<int64_t, std::vector<std::string>> tablet_result_files;

    for (int64_t tablet_id : request->tablet_ids()) {
        auto results_or = load_tablet_results(tablet_id, visible_version);
        if (!results_or.ok()) {
            LOG(WARNING) << "Failed to load results for tablet " << tablet_id << ": " << results_or.status();
            // Continue with other tablets
            continue;
        }

        auto results = results_or.value();
        if (!results.empty()) {
            tablet_results[tablet_id] = std::move(results);
            
            // Get result file paths for cleanup
            auto files_or = _result_mgr->list_result_files(tablet_id);
            if (files_or.ok()) {
                tablet_result_files[tablet_id] = std::move(files_or.value());
            }
        }
    }

    LOG(INFO) << "Loaded compaction results for " << tablet_results.size() << " tablets out of "
              << request->tablet_ids_size() << " total tablets";

    // Phase 2: Build TxnLogs for tablets with results
    std::unordered_map<int64_t, TxnLogPB> tablet_txn_logs;
    for (const auto& [tablet_id, results] : tablet_results) {
        auto txn_log_or = build_merged_txn_log(tablet_id, results);
        if (!txn_log_or.ok()) {
            LOG(WARNING) << "Failed to build TxnLog for tablet " << tablet_id << ": " << txn_log_or.status();
            response->add_failed_tablets(tablet_id);
            continue;
        }

        tablet_txn_logs[tablet_id] = std::move(txn_log_or.value());
    }

    // Phase 3: Publish all tablets
    int success_count = 0;
    int failed_count = 0;

    for (int64_t tablet_id : request->tablet_ids()) {
        bool has_compaction = tablet_txn_logs.count(tablet_id) > 0;
        const TxnLogPB* txn_log = has_compaction ? &tablet_txn_logs[tablet_id] : nullptr;

        auto st = publish_tablet(tablet_id, txn_id, base_version, new_version, has_compaction, txn_log);
        if (!st.ok()) {
            LOG(WARNING) << "Failed to publish tablet " << tablet_id << ": " << st;
            response->add_failed_tablets(tablet_id);
            failed_count++;
        } else {
            success_count++;
            
            // Clean up result files for successful tablets
            if (has_compaction && tablet_result_files.count(tablet_id) > 0) {
                cleanup_tablet_results(tablet_id, tablet_result_files[tablet_id]);
            }
        }
    }

    LOG(INFO) << "PUBLISH_AUTONOMOUS completed: " << success_count << " succeeded, " << failed_count << " failed, "
              << tablet_results.size() << " with compaction, "
              << (request->tablet_ids_size() - tablet_results.size()) << " without compaction";

    // Set response status
    if (failed_count == request->tablet_ids_size()) {
        // All failed
        Status::InternalError("All tablets failed to publish").to_protobuf(response->mutable_status());
    } else if (failed_count > 0) {
        // Partial success
        Status::OK().to_protobuf(response->mutable_status());
    } else {
        // All succeeded
        Status::OK().to_protobuf(response->mutable_status());
    }

    return Status::OK();
}

StatusOr<std::vector<CompactionResultPB>> AutonomousCompactionPublisher::load_tablet_results(int64_t tablet_id,
                                                                                               int64_t max_version) {
    // Load results with version filtering
    ASSIGN_OR_RETURN(auto results, _result_mgr->load_results(tablet_id, max_version));

    if (results.empty()) {
        VLOG(2) << "No compaction results found for tablet " << tablet_id;
    } else {
        LOG(INFO) << "Loaded " << results.size() << " compaction results for tablet " << tablet_id
                  << ", max_version=" << max_version;
    }

    return results;
}

StatusOr<TxnLogPB> AutonomousCompactionPublisher::build_merged_txn_log(int64_t tablet_id,
                                                                        const std::vector<CompactionResultPB>& results) {
    if (results.empty()) {
        return Status::InvalidArgument("No results to merge");
    }

    TxnLogPB txn_log;
    txn_log.set_tablet_id(tablet_id);

    // Merge all results into one TxnLog
    auto* op_compaction = txn_log.mutable_op_compaction();

    // Collect all input rowsets (deduplicate)
    std::unordered_set<uint32_t> input_rowset_set;
    for (const auto& result : results) {
        for (uint32_t rid : result.input_rowset_ids()) {
            input_rowset_set.insert(rid);
        }
    }

    // Add input rowsets to TxnLog
    for (uint32_t rid : input_rowset_set) {
        op_compaction->add_input_rowsets(rid);
    }

    // Add all output rowsets to TxnLog
    for (const auto& result : results) {
        if (result.has_output_rowset()) {
            auto* output_rowset = op_compaction->add_output_rowsets();
            *output_rowset = result.output_rowset();
        }
    }

    // Handle SSTable metadata for primary key tables (P3 fix - proper merging of multiple sstables)
    for (const auto& result : results) {
        for (const auto& input_sst : result.input_sstables()) {
            op_compaction->add_input_sstables()->CopyFrom(input_sst);
        }
        for (const auto& output_sst : result.output_sstables()) {
            op_compaction->add_output_sstables()->CopyFrom(output_sst);
        }
    }

    // Set base version for conflict checking
    if (results[0].has_base_version()) {
        op_compaction->set_compact_version(results[0].base_version());
    }

    LOG(INFO) << "Built merged TxnLog for tablet " << tablet_id << ": " << input_rowset_set.size()
              << " input rowsets, " << results.size() << " output rowsets";

    return txn_log;
}

Status AutonomousCompactionPublisher::publish_tablet(int64_t tablet_id, int64_t txn_id, int64_t base_version,
                                                      int64_t new_version, bool has_compaction, const TxnLogPB* txn_log) {
    // Build TxnInfoPB
    TxnInfoPB txn_info;
    txn_info.set_txn_id(txn_id);
    txn_info.set_commit_time(time(nullptr));
    txn_info.set_combined_txn_log(false);
    txn_info.set_txn_type(TXN_COMPACTION);
    txn_info.set_force_publish(true); // Always set force_publish for autonomous compaction

    if (has_compaction && txn_log != nullptr) {
        // Write TxnLog to storage
        RETURN_IF_ERROR(_tablet_mgr->put_txn_log(*txn_log));
        LOG(INFO) << "Wrote TxnLog for tablet " << tablet_id << ", txn_id=" << txn_id;
    } else {
        // No compaction for this tablet, but still need to update version
        LOG(INFO) << "No compaction for tablet " << tablet_id << ", updating version only";
    }

    // Call publish_version
    std::span<const TxnInfoPB> txn_infos(&txn_info, 1);
    RETURN_IF_ERROR(publish_version(_tablet_mgr, tablet_id, base_version, new_version, txn_infos));

    LOG(INFO) << "Published tablet " << tablet_id << " from version " << base_version << " to " << new_version
              << ", has_compaction=" << has_compaction;

    return Status::OK();
}

void AutonomousCompactionPublisher::cleanup_tablet_results(int64_t tablet_id, const std::vector<std::string>& file_paths) {
    int deleted_count = 0;
    for (const auto& file_path : file_paths) {
        auto st = _result_mgr->delete_result(file_path);
        if (st.ok()) {
            deleted_count++;
        }
    }

    LOG(INFO) << "Cleaned up " << deleted_count << " result files for tablet " << tablet_id;
}

} // namespace starrocks::lake


