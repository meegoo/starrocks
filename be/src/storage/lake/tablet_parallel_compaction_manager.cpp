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

#include "storage/lake/tablet_parallel_compaction_manager.h"

#include <algorithm>
#include <set>

#include "common/config.h"
#include "common/logging.h"
#include "gen_cpp/lake_types.pb.h"
#include "gutil/strings/join.h"
#include "gutil/strings/substitute.h"
#include "storage/lake/compaction_policy.h"
#include "storage/lake/compaction_scheduler.h"
#include "storage/lake/compaction_task.h"
#include "storage/lake/rowset.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/update_manager.h"
#include "storage/lake/versioned_tablet.h"
#include "storage/memtable_flush_executor.h"
#include "storage/rows_mapper.h"
#include "storage/storage_engine.h"
#include "util/defer_op.h"
#include "util/threadpool.h"

namespace starrocks::lake {

TabletParallelCompactionManager::TabletParallelCompactionManager(TabletManager* tablet_mgr) : _tablet_mgr(tablet_mgr) {}

TabletParallelCompactionManager::~TabletParallelCompactionManager() = default;

StatusOr<int> TabletParallelCompactionManager::create_parallel_tasks(
        int64_t tablet_id, int64_t txn_id, int64_t version, const TabletParallelConfig& config,
        std::shared_ptr<CompactionTaskCallback> callback, bool force_base_compaction, ThreadPool* thread_pool,
        const AcquireTokenFunc& acquire_token, const ReleaseTokenFunc& release_token) {
    // Validate configuration
    int32_t max_parallel = config.has_max_parallel_per_tablet() ? config.max_parallel_per_tablet()
                                                                : config::lake_compaction_max_parallel_per_tablet;
    int64_t max_bytes = config.has_max_bytes_per_subtask() ? config.max_bytes_per_subtask()
                                                           : config::lake_compaction_max_bytes_per_subtask;

    if (max_parallel <= 0) {
        max_parallel = 1;
    }
    if (max_bytes <= 0) {
        max_bytes = config::lake_compaction_max_bytes_per_subtask;
    }

    LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " txn=" << txn_id << " version=" << version
              << " max_parallel=" << max_parallel << " max_bytes=" << max_bytes;

    // Get tablet metadata
    ASSIGN_OR_RETURN(auto tablet, _tablet_mgr->get_tablet(tablet_id, version));
    const auto& metadata = tablet.metadata();
    if (!metadata) {
        return Status::NotFound(strings::Substitute("Tablet $0 metadata not found", tablet_id));
    }

    // Step 1: Get all rowsets to compact using standard pick_rowsets()
    ASSIGN_OR_RETURN(auto policy, CompactionPolicy::create(_tablet_mgr, metadata, force_base_compaction));
    auto all_rowsets_or = policy->pick_rowsets();
    if (!all_rowsets_or.ok() || all_rowsets_or.value().empty()) {
        return Status::NotFound(strings::Substitute("No rowsets to compact for tablet $0", tablet_id));
    }

    auto& all_rowsets = all_rowsets_or.value();
    size_t total_rowsets_count = all_rowsets.size();

    // Calculate total bytes
    int64_t total_bytes = 0;
    for (const auto& r : all_rowsets) {
        total_bytes += r->data_size();
    }

    LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " total_rowsets=" << total_rowsets_count
              << " total_bytes=" << total_bytes << " max_bytes_per_subtask=" << max_bytes;

    // Step 2: Split rowsets into groups with balanced data distribution
    // Strategy:
    // 1. Calculate optimal number of subtasks based on total bytes and max_bytes
    // 2. If data exceeds max_parallel capacity, skip excess data for later compaction
    // 3. Distribute data evenly across subtasks to avoid imbalanced groups
    std::vector<std::vector<RowsetPtr>> valid_groups;

    // If total bytes is small enough or max_parallel is 1, use a single group
    if (total_bytes <= max_bytes || max_parallel <= 1) {
        std::vector<uint32_t> group_ids;
        for (const auto& r : all_rowsets) {
            group_ids.push_back(r->id());
        }
        LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " using single group: " << all_rowsets.size()
                  << " rowsets, " << total_bytes << " bytes, ids=[" << JoinInts(group_ids, ",") << "]";
        valid_groups.push_back(std::move(all_rowsets));
    } else {
        // Calculate optimal number of subtasks
        // Use ceiling division to get the number of subtasks needed
        int32_t num_subtasks = static_cast<int32_t>((total_bytes + max_bytes - 1) / max_bytes);

        // Check if we need to skip some data due to max_parallel limit
        bool skip_excess_data = num_subtasks > max_parallel;
        if (skip_excess_data) {
            num_subtasks = max_parallel;
            LOG(INFO) << "Parallel compaction: tablet=" << tablet_id
                      << " data exceeds max_parallel capacity, will skip excess data for later compaction"
                      << ", max_parallel=" << max_parallel
                      << ", would need=" << ((total_bytes + max_bytes - 1) / max_bytes);
        }

        // Calculate target bytes per subtask for even distribution
        // Only consider the data we're actually going to process
        int64_t processable_bytes = skip_excess_data ? (static_cast<int64_t>(max_parallel) * max_bytes) : total_bytes;
        int64_t target_bytes_per_subtask = processable_bytes / num_subtasks;

        LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " planning " << num_subtasks
                  << " subtasks with target_bytes_per_subtask=" << target_bytes_per_subtask
                  << " (processable_bytes=" << processable_bytes << ")";

        // Split rowsets into groups with balanced distribution
        std::vector<RowsetPtr> current_group;
        int64_t current_bytes = 0;
        size_t skipped_rowsets = 0;
        int64_t skipped_bytes = 0;

        for (size_t rowset_idx = 0; rowset_idx < all_rowsets.size(); rowset_idx++) {
            auto& rowset = all_rowsets[rowset_idx];
            int64_t rowset_bytes = rowset->data_size();

            // Check if we should skip this rowset due to max_parallel/max_bytes limits
            if (skip_excess_data) {
                // Case 1: We've already finalized max_parallel groups, skip all remaining
                if (static_cast<int32_t>(valid_groups.size()) >= max_parallel) {
                    skipped_rowsets++;
                    skipped_bytes += rowset_bytes;
                    continue;
                }
                // Case 2: We're building the last allowed group (valid_groups.size() == max_parallel - 1)
                // and adding this rowset would exceed max_bytes
                if (static_cast<int32_t>(valid_groups.size()) == max_parallel - 1 && !current_group.empty() &&
                    current_bytes + rowset_bytes > max_bytes) {
                    skipped_rowsets++;
                    skipped_bytes += rowset_bytes;
                    continue;
                }
            }

            // Check if we should start a new group
            // Condition: current group is not empty AND adding this rowset exceeds target
            // AND we haven't created all needed groups yet
            bool should_start_new_group = !current_group.empty() &&
                                          current_bytes + rowset_bytes > target_bytes_per_subtask &&
                                          static_cast<int32_t>(valid_groups.size()) < num_subtasks - 1;

            if (should_start_new_group) {
                // Save current group
                std::vector<uint32_t> group_ids;
                for (const auto& r : current_group) {
                    group_ids.push_back(r->id());
                }
                LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " group " << valid_groups.size() << ": "
                          << current_group.size() << " rowsets, " << current_bytes << " bytes, ids=["
                          << JoinInts(group_ids, ",") << "]";

                valid_groups.push_back(std::move(current_group));
                current_group.clear();
                current_bytes = 0;
            }

            current_group.push_back(std::move(rowset));
            current_bytes += rowset_bytes;
        }

        // Add the last group if not empty
        if (!current_group.empty()) {
            std::vector<uint32_t> group_ids;
            for (const auto& r : current_group) {
                group_ids.push_back(r->id());
            }
            LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " group " << valid_groups.size() << ": "
                      << current_group.size() << " rowsets, " << current_bytes << " bytes, ids=["
                      << JoinInts(group_ids, ",") << "]";
            valid_groups.push_back(std::move(current_group));
        }

        if (skipped_rowsets > 0) {
            LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " skipped " << skipped_rowsets << " rowsets ("
                      << skipped_bytes << " bytes) for later compaction due to max_parallel limit";
        }
    }

    if (valid_groups.empty()) {
        return Status::NotFound("No valid rowset groups for parallel compaction");
    }

    LOG(INFO) << "Parallel compaction: tablet=" << tablet_id << " created " << valid_groups.size() << " groups from "
              << total_rowsets_count << " rowsets";

    // Step 3: Create tablet state
    std::string state_key = make_state_key(tablet_id, txn_id);
    std::unique_ptr<TabletParallelState> state;
    {
        std::lock_guard<std::mutex> lock(_states_mutex);
        auto it = _tablet_states.find(state_key);
        if (it != _tablet_states.end()) {
            return Status::AlreadyExist(
                    strings::Substitute("Parallel compaction already exists for tablet $0 txn $1", tablet_id, txn_id));
        }
        state = std::make_unique<TabletParallelState>();
        state->tablet_id = tablet_id;
        state->txn_id = txn_id;
        state->version = version;
        state->max_parallel = max_parallel;
        state->max_bytes_per_subtask = max_bytes;
        state->callback = std::move(callback);
        state->release_token = release_token;
        _tablet_states[state_key] = std::move(state);
    }

    TabletParallelState* state_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(_states_mutex);
        state_ptr = _tablet_states[state_key].get();
    }

    // Step 4: Create subtasks for each group
    int subtasks_created = 0;
    int submitted_rowsets_count = 0;
    int64_t submitted_bytes = 0;

    for (size_t group_idx = 0; group_idx < valid_groups.size(); group_idx++) {
        auto& rowsets = valid_groups[group_idx];

        // Collect rowset IDs and calculate input bytes
        std::vector<uint32_t> rowset_ids;
        int64_t input_bytes = 0;
        for (const auto& rowset : rowsets) {
            rowset_ids.push_back(rowset->id());
            input_bytes += rowset->data_size();
        }
        submitted_rowsets_count += rowset_ids.size();
        submitted_bytes += input_bytes;

        int32_t subtask_id = static_cast<int32_t>(group_idx);

        // Mark rowsets as compacting and create subtask info
        {
            std::lock_guard<std::mutex> lock(state_ptr->mutex);
            mark_rowsets_compacting(state_ptr, rowset_ids);

            SubtaskInfo info;
            info.subtask_id = subtask_id;
            info.input_rowset_ids = rowset_ids;
            info.input_bytes = input_bytes;
            info.start_time = ::time(nullptr);
            state_ptr->running_subtasks[subtask_id] = std::move(info);
            state_ptr->total_subtasks_created++;
        }

        _running_subtasks++;

        // Acquire limiter token before submitting
        if (!acquire_token()) {
            LOG(WARNING) << "Parallel compaction: failed to acquire limiter token for subtask " << subtask_id
                         << " tablet " << tablet_id << ", will skip remaining groups";
            // Failed to acquire token, revert state changes
            std::lock_guard<std::mutex> lock(state_ptr->mutex);
            unmark_rowsets_compacting(state_ptr, rowset_ids);
            state_ptr->running_subtasks.erase(subtask_id);
            state_ptr->total_subtasks_created--;
            _running_subtasks--;

            if (subtasks_created == 0) {
                cleanup_tablet(tablet_id, txn_id);
                return Status::ResourceBusy("Failed to acquire limiter token for parallel compaction");
            }
            break;
        }

        // Submit task to thread pool
        auto submit_st = thread_pool->submit_func([this, tablet_id, txn_id, subtask_id, rowsets = std::move(rowsets),
                                                   version, force_base_compaction, release_token]() mutable {
            execute_subtask(tablet_id, txn_id, subtask_id, std::move(rowsets), version, force_base_compaction,
                            release_token);
        });

        if (!submit_st.ok()) {
            LOG(WARNING) << "Parallel compaction: failed to submit subtask " << subtask_id << " for tablet "
                         << tablet_id << ": " << submit_st;
            // Failed to submit, revert state changes and release token
            {
                std::lock_guard<std::mutex> lock(state_ptr->mutex);
                unmark_rowsets_compacting(state_ptr, rowset_ids);
                state_ptr->running_subtasks.erase(subtask_id);
                state_ptr->total_subtasks_created--;
            }
            _running_subtasks--;
            release_token(false);

            if (subtasks_created == 0) {
                cleanup_tablet(tablet_id, txn_id);
                return submit_st;
            }
            break;
        }

        subtasks_created++;

        LOG(INFO) << "Parallel compaction: created subtask " << subtask_id << " for tablet " << tablet_id
                  << ", txn_id=" << txn_id << ", rowsets=" << rowset_ids.size()
                  << " (ids: " << JoinInts(rowset_ids, ",") << "), input_bytes=" << input_bytes;
    }

    if (subtasks_created == 0) {
        LOG(WARNING) << "Parallel compaction: failed to create any subtask for tablet " << tablet_id
                     << ", txn_id=" << txn_id << ", falling back to non-parallel mode";
        cleanup_tablet(tablet_id, txn_id);
        return Status::NotFound(strings::Substitute("No rowsets to compact for tablet $0", tablet_id));
    }

    LOG(INFO) << "Parallel compaction: successfully created " << subtasks_created << " subtasks for tablet "
              << tablet_id << ", txn_id=" << txn_id << ", total_rowsets=" << submitted_rowsets_count
              << ", total_bytes=" << submitted_bytes;

    return subtasks_created;
}

TabletParallelState* TabletParallelCompactionManager::get_tablet_state(int64_t tablet_id, int64_t txn_id) {
    std::string state_key = make_state_key(tablet_id, txn_id);
    std::lock_guard<std::mutex> lock(_states_mutex);
    auto it = _tablet_states.find(state_key);
    if (it == _tablet_states.end()) {
        return nullptr;
    }
    return it->second.get();
}

void TabletParallelCompactionManager::on_subtask_complete(int64_t tablet_id, int64_t txn_id, int32_t subtask_id,
                                                          std::unique_ptr<CompactionTaskContext> context) {
    std::string state_key = make_state_key(tablet_id, txn_id);

    TabletParallelState* state_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(_states_mutex);
        auto it = _tablet_states.find(state_key);
        if (it == _tablet_states.end()) {
            // State was cleaned up (e.g., due to timeout or cancellation from FE).
            // We must still decrement the counter since it was incremented in create_parallel_tasks.
            _running_subtasks--;
            LOG(WARNING) << "Tablet state not found for subtask completion, tablet=" << tablet_id
                         << ", txn_id=" << txn_id << ", subtask_id=" << subtask_id
                         << ". Decremented running_subtasks counter.";
            return;
        }
        state_ptr = it->second.get();
    }

    std::shared_ptr<CompactionTaskCallback> callback;
    bool all_complete = false;

    {
        std::lock_guard<std::mutex> lock(state_ptr->mutex);

        auto it = state_ptr->running_subtasks.find(subtask_id);
        if (it == state_ptr->running_subtasks.end()) {
            // Subtask was already removed (should not happen in normal flow, but handle defensively).
            // We must still decrement the counter to prevent counter leakage.
            _running_subtasks--;
            LOG(WARNING) << "Subtask not found, tablet=" << tablet_id << ", txn_id=" << txn_id
                         << ", subtask_id=" << subtask_id << ". Decremented running_subtasks counter.";
            return;
        }

        // Unmark rowsets
        unmark_rowsets_compacting(state_ptr, it->second.input_rowset_ids);

        // Move to completed
        state_ptr->completed_subtasks.push_back(std::move(context));
        state_ptr->running_subtasks.erase(it);

        _running_subtasks--;
        _completed_subtasks++;

        all_complete = state_ptr->is_complete();
        callback = state_ptr->callback;

        LOG(INFO) << "Parallel compaction subtask completed, tablet=" << tablet_id << ", txn_id=" << txn_id
                  << ", subtask_id=" << subtask_id << ", remaining=" << state_ptr->running_subtasks.size()
                  << ", completed=" << state_ptr->completed_subtasks.size();
    }

    // If all subtasks are complete, notify the callback
    if (all_complete && callback) {
        // Build merged context
        // Note: skip_write_txnlog must be true so that merged txn_log is added to response
        auto merged_context = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, state_ptr->version,
                                                                      false /* force_base_compaction */,
                                                                      true /* skip_write_txnlog */, callback);

        // Copy table_id and partition_id from one of the completed subtask contexts.
        // These values are populated in TabletManager::compact() from shard info,
        // and are needed for downstream processes (e.g., metrics, catalog operations).
        {
            std::lock_guard<std::mutex> lock(state_ptr->mutex);
            for (const auto& subtask_ctx : state_ptr->completed_subtasks) {
                if (subtask_ctx->table_id != 0) {
                    merged_context->table_id = subtask_ctx->table_id;
                }
                if (subtask_ctx->partition_id != 0) {
                    merged_context->partition_id = subtask_ctx->partition_id;
                }
                // All subtasks belong to the same tablet, so table_id and partition_id
                // should be the same. Break once we've found valid values.
                if (merged_context->table_id != 0 && merged_context->partition_id != 0) {
                    break;
                }
            }
        }

        // Merge TxnLogs
        auto merged_txn_log_or = get_merged_txn_log(tablet_id, txn_id);
        if (merged_txn_log_or.ok()) {
            merged_context->txn_log = std::make_unique<TxnLogPB>(std::move(merged_txn_log_or.value()));
        } else {
            merged_context->status = merged_txn_log_or.status();
        }

        // Set status based on subtask statuses
        {
            std::lock_guard<std::mutex> lock(state_ptr->mutex);
            for (const auto& subtask_ctx : state_ptr->completed_subtasks) {
                if (!subtask_ctx->status.ok()) {
                    merged_context->status.update(subtask_ctx->status);
                }
                if (subtask_ctx->stats) {
                    *(merged_context->stats) = *(merged_context->stats) + *(subtask_ctx->stats);
                }
            }
        }

        callback->finish_task(std::move(merged_context));

        // Cleanup
        cleanup_tablet(tablet_id, txn_id);
    }
}

bool TabletParallelCompactionManager::is_tablet_complete(int64_t tablet_id, int64_t txn_id) {
    auto* state = get_tablet_state(tablet_id, txn_id);
    if (state == nullptr) {
        return true;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->is_complete();
}

void TabletParallelCompactionManager::cleanup_tablet(int64_t tablet_id, int64_t txn_id) {
    std::string state_key = make_state_key(tablet_id, txn_id);
    int32_t subtask_count = 0;

    {
        std::lock_guard<std::mutex> lock(_states_mutex);
        auto it = _tablet_states.find(state_key);
        if (it != _tablet_states.end()) {
            // Get subtask count before erasing state
            std::lock_guard<std::mutex> state_lock(it->second->mutex);
            subtask_count =
                    static_cast<int32_t>(it->second->completed_subtasks.size() + it->second->running_subtasks.size());
            _tablet_states.erase(it);
        }
    }

    // Clean up any remaining rows mapper files (outside of lock)
    // This is safe even if files don't exist or were already deleted by light_publish
    if (subtask_count > 0) {
        delete_lake_rows_mapper_files(tablet_id, txn_id, subtask_count);
    }

    LOG(INFO) << "Cleaned up parallel compaction state for tablet " << tablet_id << ", txn_id=" << txn_id
              << ", subtask_count=" << subtask_count;
}

StatusOr<TxnLogPB> TabletParallelCompactionManager::get_merged_txn_log(int64_t tablet_id, int64_t txn_id) {
    auto* state = get_tablet_state(tablet_id, txn_id);
    if (state == nullptr) {
        return Status::NotFound(strings::Substitute("Tablet state not found for tablet $0 txn $1", tablet_id, txn_id));
    }

    TxnLogPB merged_log;
    merged_log.set_tablet_id(tablet_id);
    merged_log.set_txn_id(txn_id);

    auto* op_compaction = merged_log.mutable_op_compaction();

    // Variables to be used outside the lock
    int32_t subtask_count = 0;
    int64_t version = 0;
    bool has_any_output = false;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        // Cache version for use outside the lock
        version = state->version;

        // Sort completed_subtasks by subtask_id to ensure consistent ordering
        // This is critical because rows_mapper files are merged by subtask_id order,
        // so segments must also be merged in the same order.
        std::sort(state->completed_subtasks.begin(), state->completed_subtasks.end(),
                  [](const std::unique_ptr<CompactionTaskContext>& a, const std::unique_ptr<CompactionTaskContext>& b) {
                      return a->subtask_id < b->subtask_id;
                  });

        // Collect all input rowsets (deduplicate and keep sorted order)
        // Use std::set instead of unordered_set to maintain ascending order by rowset id.
        // This is important because NonPrimaryKeyTxnLogApplier::apply_compaction_log
        // expects input_rowsets to be in the same order as they appear in metadata.
        std::set<uint32_t> input_rowset_set;
        for (const auto& ctx : state->completed_subtasks) {
            if (ctx->txn_log != nullptr && ctx->txn_log->has_op_compaction()) {
                for (uint32_t rid : ctx->txn_log->op_compaction().input_rowsets()) {
                    input_rowset_set.insert(rid);
                }
            }
        }

        // Add input rowsets to merged log (in sorted order)
        for (uint32_t rid : input_rowset_set) {
            op_compaction->add_input_rowsets(rid);
        }

        // Merge all output rowsets from subtasks into a single output rowset
        // Each subtask produces its own output_rowset, we need to combine all segments
        auto* merged_output_rowset = op_compaction->mutable_output_rowset();
        int64_t total_num_rows = 0;
        int64_t total_data_size = 0;
        bool any_overlapped = false;

        for (const auto& ctx : state->completed_subtasks) {
            if (ctx->txn_log != nullptr && ctx->txn_log->has_op_compaction()) {
                const auto& sub_compaction = ctx->txn_log->op_compaction();
                if (sub_compaction.has_output_rowset()) {
                    const auto& sub_output = sub_compaction.output_rowset();
                    has_any_output = true;

                    // Merge segments from this subtask's output rowset
                    for (const auto& segment : sub_output.segments()) {
                        merged_output_rowset->add_segments(segment);
                    }

                    // Merge segment sizes
                    for (uint64_t seg_size : sub_output.segment_size()) {
                        merged_output_rowset->add_segment_size(seg_size);
                    }

                    // Merge segment encryption metas
                    for (const auto& enc_meta : sub_output.segment_encryption_metas()) {
                        merged_output_rowset->add_segment_encryption_metas(enc_meta);
                    }

                    // Accumulate statistics
                    total_num_rows += sub_output.num_rows();
                    total_data_size += sub_output.data_size();

                    // If any subtask output is overlapped, the merged result is overlapped
                    if (sub_output.overlapped()) {
                        any_overlapped = true;
                    }
                }

                // Note: SST compaction is skipped during subtask execution.
                // It will be executed once after all subtasks complete (see below).
            }
        }

        // Set merged statistics if we have any output
        if (has_any_output) {
            merged_output_rowset->set_num_rows(total_num_rows);
            merged_output_rowset->set_data_size(total_data_size);
            // Parallel compaction outputs are overlapped since they come from different subtasks
            merged_output_rowset->set_overlapped(any_overlapped || state->completed_subtasks.size() > 1);
        }

        // Set compact_version (all subtasks should use the same base version)
        if (!state->completed_subtasks.empty() && state->completed_subtasks[0]->txn_log != nullptr &&
            state->completed_subtasks[0]->txn_log->has_op_compaction()) {
            const auto& first_compaction = state->completed_subtasks[0]->txn_log->op_compaction();
            if (first_compaction.has_compact_version()) {
                op_compaction->set_compact_version(first_compaction.compact_version());
            }
        }

        subtask_count = static_cast<int32_t>(state->completed_subtasks.size());

        // Record subtask_count in txn_log for light_publish to read multiple rows_mapper files.
        // Each subtask generates its own rows_mapper file with subtask-specific filename
        // ({tablet_id}_{txn_id}_{subtask_id}.crm), and light_publish will read them in order
        // instead of requiring a single merged file.
        if (subtask_count > 0) {
            op_compaction->set_subtask_count(subtask_count);
        }

        LOG(INFO) << "Merged TxnLog for tablet " << tablet_id << ", txn_id=" << txn_id
                  << ", input_rowsets=" << input_rowset_set.size() << ", subtasks=" << subtask_count
                  << ", output_segments=" << merged_output_rowset->segments_size() << ", output_rows=" << total_num_rows
                  << ", output_size=" << total_data_size;
    }
    // Lock released here

    // Execute SST compaction once after all subtasks complete.
    // This is more efficient than having each subtask independently try to compact SST files,
    // which could lead to conflicts and redundant work.
    // SST compaction was skipped in each subtask's execute_index_major_compaction().
    RETURN_IF_ERROR(execute_sst_compaction(tablet_id, version, &merged_log));

    return merged_log;
}

void TabletParallelCompactionManager::mark_rowsets_compacting(TabletParallelState* state,
                                                              const std::vector<uint32_t>& rowset_ids) {
    // Assumes state->mutex is already held
    for (uint32_t rid : rowset_ids) {
        state->compacting_rowsets.insert(rid);
    }
}

void TabletParallelCompactionManager::unmark_rowsets_compacting(TabletParallelState* state,
                                                                const std::vector<uint32_t>& rowset_ids) {
    // Assumes state->mutex is already held
    for (uint32_t rid : rowset_ids) {
        state->compacting_rowsets.erase(rid);
    }
}

void TabletParallelCompactionManager::execute_subtask(int64_t tablet_id, int64_t txn_id, int32_t subtask_id,
                                                      std::vector<RowsetPtr> input_rowsets, int64_t version,
                                                      bool force_base_compaction,
                                                      const ReleaseTokenFunc& release_token) {
    LOG(INFO) << "Executing parallel compaction subtask " << subtask_id << " for tablet " << tablet_id
              << ", txn_id=" << txn_id << ", rowsets=" << input_rowsets.size();

    // Get tablet state and callback
    auto* state = get_tablet_state(tablet_id, txn_id);
    if (state == nullptr) {
        // State has been cleaned up (possibly due to timeout or cancellation).
        // We must still decrement the global counter to prevent counter leakage.
        // Note: Since state is gone, we cannot call on_subtask_complete or notify the callback.
        // This is acceptable because state cleanup implies the compaction request has been
        // abandoned (e.g., timeout from FE side).
        _running_subtasks--;
        // Release limiter token
        if (release_token) {
            release_token(false);
        }
        LOG(WARNING) << "Tablet state not found during subtask execution, tablet=" << tablet_id << ", txn_id=" << txn_id
                     << ", subtask_id=" << subtask_id << ". Decremented running_subtasks counter.";
        return;
    }

    // Create compaction context for this subtask
    // Note: skip_write_txnlog must be true for parallel compaction, so that txn_log is saved to context
    // and can be merged later in on_subtask_complete
    // Pass subtask_id so that rows_mapper files use subtask-specific filenames
    auto context = CompactionTaskContext::create_for_subtask(txn_id, tablet_id, version, force_base_compaction,
                                                             true /* skip_write_txnlog */, state->callback, subtask_id);

    auto start_time = ::time(nullptr);
    context->start_time.store(start_time, std::memory_order_relaxed);

    // Calculate in_queue_time_sec using the enqueue time from SubtaskInfo
    // Also store context pointer in SubtaskInfo for progress/status tracking in list_tasks()
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->running_subtasks.find(subtask_id);
        if (it != state->running_subtasks.end()) {
            int64_t enqueue_time = it->second.start_time;
            int64_t in_queue_time_sec = start_time > enqueue_time ? (start_time - enqueue_time) : 0;
            context->stats->in_queue_time_sec += in_queue_time_sec;
            // Store context pointer for real-time progress/status tracking
            it->second.context = context.get();
        }
    }

    // Create compaction task using pre-selected rowsets
    auto compaction_task_or = _tablet_mgr->compact(context.get(), std::move(input_rowsets));
    if (!compaction_task_or.ok()) {
        LOG(WARNING) << "Failed to create compaction task for tablet " << tablet_id << " subtask " << subtask_id << ": "
                     << compaction_task_or.status();
        context->status = compaction_task_or.status();
        on_subtask_complete(tablet_id, txn_id, subtask_id, std::move(context));
        // Release limiter token on early return
        if (release_token) {
            release_token(compaction_task_or.status().is_mem_limit_exceeded());
        }
        return;
    }

    auto compaction_task = compaction_task_or.value();

    // Execute compaction
    // Note: We capture 'this', tablet_id, and txn_id instead of the raw 'state' pointer
    // because the state could be cleaned up by another thread calling cleanup_tablet()
    // while this subtask is executing. By capturing stable values and using get_tablet_state()
    // inside the lambda, we safely check if the state still exists before accessing it.
    auto cancel_func = [this, tablet_id, txn_id]() {
        // Check if tablet state still exists - if not, the compaction has been cancelled/timed out
        if (get_tablet_state(tablet_id, txn_id) == nullptr) {
            return Status::Cancelled("Tablet parallel compaction state has been cleaned up");
        }
        return Status::OK();
    };

    ThreadPool* flush_pool = nullptr;
    if (config::lake_enable_compaction_async_write) {
        flush_pool = StorageEngine::instance()->lake_memtable_flush_executor()->get_thread_pool();
    }

    auto exec_st = compaction_task->execute(cancel_func, flush_pool);

    auto finish_time = std::max<int64_t>(::time(nullptr), start_time);
    auto cost = finish_time - start_time;

    if (!exec_st.ok()) {
        LOG(WARNING) << "Compaction subtask " << subtask_id << " failed for tablet " << tablet_id << ": " << exec_st
                     << ", cost=" << cost << "s";
        context->status = exec_st;
    } else {
        LOG(INFO) << "Compaction subtask " << subtask_id << " completed for tablet " << tablet_id << ", cost=" << cost
                  << "s";
    }

    context->finish_time.store(finish_time, std::memory_order_release);

    // Check if memory limit was exceeded before moving context
    bool mem_limit_exceeded = exec_st.is_mem_limit_exceeded();

    // Notify completion
    on_subtask_complete(tablet_id, txn_id, subtask_id, std::move(context));

    // Release limiter token after subtask completes
    if (release_token) {
        release_token(mem_limit_exceeded);
    }
}

Status TabletParallelCompactionManager::execute_sst_compaction(int64_t tablet_id, int64_t version,
                                                               TxnLogPB* merged_log) {
    // Get tablet metadata to check if SST compaction is applicable
    auto tablet_or = _tablet_mgr->get_tablet(tablet_id, version);
    if (!tablet_or.ok()) {
        LOG(WARNING) << "Failed to get tablet for SST compaction, tablet=" << tablet_id << ", version=" << version
                     << ": " << tablet_or.status();
        return Status::OK(); // Don't fail the entire compaction for SST compaction failure
    }

    auto tablet = tablet_or.value();
    const auto& metadata = tablet.metadata();

    // Check if this is a primary key table with cloud native persistent index
    if (!metadata || metadata->schema().keys_type() != KeysType::PRIMARY_KEYS) {
        return Status::OK();
    }

    if (!metadata->enable_persistent_index() ||
        metadata->persistent_index_type() != PersistentIndexTypePB::CLOUD_NATIVE) {
        return Status::OK();
    }

    // Execute SST compaction
    LOG(INFO) << "Executing unified SST compaction for parallel compaction, tablet=" << tablet_id
              << ", version=" << version;

    auto* update_mgr = _tablet_mgr->update_mgr();
    if (update_mgr == nullptr) {
        LOG(WARNING) << "UpdateManager is null, skip SST compaction for tablet " << tablet_id;
        return Status::OK();
    }

    auto st = update_mgr->execute_index_major_compaction(metadata, merged_log);
    if (!st.ok()) {
        LOG(WARNING) << "SST compaction failed for tablet " << tablet_id << ": " << st
                     << ". This will not fail the parallel compaction.";
        // Don't fail the entire parallel compaction for SST compaction failure
        // SST compaction can be retried in the next compaction cycle
        return Status::OK();
    }

    // Log SST compaction results
    if (merged_log->has_op_compaction() && !merged_log->op_compaction().input_sstables().empty()) {
        size_t total_input_sst_size = 0;
        for (const auto& input_sst : merged_log->op_compaction().input_sstables()) {
            total_input_sst_size += input_sst.filesize();
        }
        LOG(INFO) << "SST compaction completed for tablet " << tablet_id
                  << ", input_ssts=" << merged_log->op_compaction().input_sstables_size()
                  << ", input_size=" << total_input_sst_size;
    }

    return Status::OK();
}

void TabletParallelCompactionManager::list_tasks(std::vector<CompactionTaskInfo>* infos) {
    std::lock_guard<std::mutex> lock(_states_mutex);
    for (const auto& [state_key, state_ptr] : _tablet_states) {
        std::lock_guard<std::mutex> state_lock(state_ptr->mutex);

        // Add running subtasks
        for (const auto& [subtask_id, subtask_info] : state_ptr->running_subtasks) {
            auto& info = infos->emplace_back();
            info.txn_id = state_ptr->txn_id;
            info.tablet_id = state_ptr->tablet_id;
            info.version = state_ptr->version;
            info.skipped = false;
            info.runs = 1; // Parallel subtasks run once
            info.start_time = subtask_info.start_time;
            info.finish_time = 0; // Still running
            // Get real-time progress from context if available.
            // Note: We only read progress (which uses atomic operations) for running tasks.
            // We don't read status here because context->status may be modified concurrently
            // in execute_subtask() without lock protection, which would cause a data race.
            // For running tasks, status is always shown as OK (in-progress).
            // Final status is only available after task completion (in completed_subtasks).
            if (subtask_info.context != nullptr) {
                info.progress = subtask_info.context->progress.value();
            } else {
                info.progress = 0;
            }
            info.status = Status::OK();
            // Build profile with subtask-specific info
            info.profile = fmt::format(
                    "{{\"subtask_id\":{},\"input_rowsets\":{},\"input_bytes\":{},\"is_parallel_subtask\":true}}",
                    subtask_id, subtask_info.input_rowset_ids.size(), subtask_info.input_bytes);
        }

        // Add completed subtasks that haven't been cleaned up yet
        for (const auto& ctx : state_ptr->completed_subtasks) {
            auto& info = infos->emplace_back();
            info.txn_id = ctx->txn_id;
            info.tablet_id = ctx->tablet_id;
            info.version = ctx->version;
            info.skipped = ctx->skipped.load(std::memory_order_relaxed);
            info.runs = ctx->runs.load(std::memory_order_relaxed);
            info.start_time = ctx->start_time.load(std::memory_order_relaxed);
            info.finish_time = ctx->finish_time.load(std::memory_order_acquire);
            info.progress = ctx->progress.value();
            if (info.runs > 0 && ctx->stats) {
                info.profile = ctx->stats->to_json_stats();
            }
            if (info.finish_time > 0) {
                info.status = ctx->status;
            }
        }
    }
}

} // namespace starrocks::lake
