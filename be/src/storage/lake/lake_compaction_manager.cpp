// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE.2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage/lake/lake_compaction_manager.h"

#include <chrono>
#include <fstream>

#include "common/config.h"
#include "fs/fs_util.h"
#include "gutil/strings/join.h"
#include "gutil/strings/substitute.h"
#include "gutil/strings/util.h"
#include "storage/lake/compaction_policy.h"
#include "storage/lake/compaction_task.h"
#include "storage/lake/compaction_task_context.h"
#include "storage/lake/rowset.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/update_manager.h"
#include "storage/storage_engine.h"
#include "util/defer_op.h"
#include "util/threadpool.h"
#include "util/time.h"

namespace starrocks::lake {

LakeCompactionManager* LakeCompactionManager::instance() {
    static LakeCompactionManager instance;
    return &instance;
}

LakeCompactionManager::~LakeCompactionManager() {
    stop();
}

Status LakeCompactionManager::init(TabletManager* tablet_manager) {
    if (_tablet_manager != nullptr) {
        return Status::InternalError("LakeCompactionManager already initialized");
    }

    _tablet_manager = tablet_manager;

    // Initialize result directory
    _result_dir = config::lake_compaction_result_dir;
    RETURN_IF_ERROR(fs::create_directories(_result_dir));

    // Create worker thread pool
    RETURN_IF_ERROR(ThreadPoolBuilder("lake_auto_compact")
                            .set_min_threads(0)
                            .set_max_threads(config::lake_autonomous_compaction_threads)
                            .set_max_queue_size(1000)
                            .build(&_worker_pool));

    // Start scheduler thread
    _scheduler_thread = std::make_unique<std::thread>([this]() { _schedule_loop(); });

    LOG(INFO) << "LakeCompactionManager initialized with " << config::lake_autonomous_compaction_threads
              << " worker threads";

    return Status::OK();
}

void LakeCompactionManager::stop() {
    if (_stopped.exchange(true)) {
        return; // Already stopped
    }

    LOG(INFO) << "Stopping LakeCompactionManager...";

    // Wake up scheduler thread
    {
        std::lock_guard lock(_queue_mutex);
        _queue_cv.notify_all();
    }

    // Wait for scheduler thread to exit
    if (_scheduler_thread && _scheduler_thread->joinable()) {
        _scheduler_thread->join();
    }

    // Wait for all worker tasks to complete
    if (_worker_pool) {
        _worker_pool->wait();
        _worker_pool->shutdown();
    }

    LOG(INFO) << "LakeCompactionManager stopped";
}

void LakeCompactionManager::update_tablet_async(int64_t tablet_id) {
    if (!config::enable_lake_autonomous_compaction || _stopped.load()) {
        return;
    }

    double score = _calculate_score(tablet_id);
    if (score < config::lake_compaction_score_threshold) {
        // Score too low, not worth compacting
        return;
    }

    auto state = _get_tablet_state(tablet_id);
    std::lock_guard state_lock(state->mutex);

    CompactionCandidate candidate;
    candidate.tablet_id = tablet_id;
    candidate.score = score;
    candidate.last_update_time = MonotonicNanos();

    std::lock_guard queue_lock(_queue_mutex);

    // Check if tablet is already in queue
    if (_tablets_in_queue.count(tablet_id)) {
        // Update score if new score is higher
        if (state->queued_candidate && candidate.score > state->queued_candidate->score) {
            state->queued_candidate->score = candidate.score;
            state->queued_candidate->last_update_time = candidate.last_update_time;
        }
        return;
    }

    // Add to queue
    auto candidate_ptr = std::make_shared<CompactionCandidate>(candidate);
    state->queued_candidate = candidate_ptr;
    _candidate_queue.push(*candidate_ptr);
    _tablets_in_queue.insert(tablet_id);

    _queue_cv.notify_one();

    VLOG(1) << "Tablet " << tablet_id << " added to compaction queue with score " << score;
}

double LakeCompactionManager::_calculate_score(int64_t tablet_id) {
    auto tablet = _tablet_manager->get_tablet(tablet_id);
    if (!tablet.ok()) {
        return 0.0;
    }

    // Get tablet metadata
    auto metadata_or = tablet->get_metadata();
    if (!metadata_or.ok()) {
        return 0.0;
    }

    auto metadata = std::move(metadata_or).value();

    // Use the compaction policy to calculate score
    auto policy_or = CompactionPolicy::create_compaction_policy(_tablet_manager, metadata);
    if (!policy_or.ok()) {
        return 0.0;
    }

    auto policy = std::move(policy_or).value();
    return policy->calculate_score(metadata);
}

void LakeCompactionManager::_schedule_loop() {
    while (!_stopped.load()) {
        std::unique_lock lock(_queue_mutex);

        // Wait for new candidates or stop signal
        _queue_cv.wait(lock, [this]() { return !_candidate_queue.empty() || _stopped.load(); });

        if (_stopped.load()) {
            break;
        }

        _schedule_next_compaction();
    }

    LOG(INFO) << "LakeCompactionManager scheduler thread exited";
}

void LakeCompactionManager::_schedule_next_compaction() {
    // _queue_mutex must be held when calling this function

    if (_candidate_queue.empty()) {
        return;
    }

    auto candidate = _candidate_queue.top();
    _candidate_queue.pop();
    _tablets_in_queue.erase(candidate.tablet_id);

    int64_t tablet_id = candidate.tablet_id;

    // Get tablet state
    auto state = _get_tablet_state(tablet_id);
    {
        std::lock_guard state_lock(state->mutex);
        state->queued_candidate.reset();

        // Check if we can start a new task for this tablet
        if (state->running_tasks >= config::lake_compaction_max_tasks_per_tablet) {
            // Too many running tasks, re-add to queue if score is still high
            if (candidate.score >= config::lake_compaction_score_threshold) {
                auto candidate_ptr = std::make_shared<CompactionCandidate>(candidate);
                state->queued_candidate = candidate_ptr;
                _candidate_queue.push(candidate);
                _tablets_in_queue.insert(tablet_id);
            }
            return;
        }

        // Increment running task count
        state->running_tasks++;
    }

    _tablets_running.insert(tablet_id);

    // Submit task to worker pool
    auto status = _worker_pool->submit_func([this, tablet_id]() {
        auto st = _execute_compaction(tablet_id);
        if (!st.ok()) {
            LOG(WARNING) << "Autonomous compaction failed for tablet " << tablet_id << ": " << st;
        }

        // Decrement running task count
        auto state = _get_tablet_state(tablet_id);
        {
            std::lock_guard state_lock(state->mutex);
            state->running_tasks--;
        }

        // Check if we need to schedule another round of compaction for this tablet
        update_tablet_async(tablet_id);
    });

    if (!status.ok()) {
        LOG(WARNING) << "Failed to submit compaction task for tablet " << tablet_id << ": " << status;
        // Rollback running task count
        std::lock_guard state_lock(state->mutex);
        state->running_tasks--;
    }
}

Status LakeCompactionManager::_execute_compaction(int64_t tablet_id) {
    VLOG(1) << "Start autonomous compaction for tablet " << tablet_id;

    auto tablet_or = _tablet_manager->get_tablet(tablet_id);
    RETURN_IF_ERROR(tablet_or.status());
    auto tablet = std::move(tablet_or).value();

    // Get current metadata
    auto metadata_or = tablet.get_metadata();
    RETURN_IF_ERROR(metadata_or.status());
    auto metadata = std::move(metadata_or).value();
    int64_t base_version = metadata->version();

    // Get tablet compaction state
    auto state = _get_tablet_state(tablet_id);
    std::unordered_set<uint32_t> compacting_rowsets;
    {
        std::lock_guard state_lock(state->mutex);
        compacting_rowsets = state->compacting_rowsets;
    }

    // Pick rowsets for compaction
    std::vector<uint32_t> input_rowsets;
    int64_t input_data_size = 0;
    auto has_more_or = _pick_rowsets_with_limit(tablet_id, base_version, compacting_rowsets, &input_rowsets,
                                                  &input_data_size);
    RETURN_IF_ERROR(has_more_or.status());
    bool has_more = has_more_or.value();

    if (input_rowsets.empty()) {
        VLOG(1) << "No rowsets to compact for tablet " << tablet_id;
        return Status::OK();
    }

    // Mark rowsets as being compacted
    {
        std::lock_guard state_lock(state->mutex);
        for (uint32_t rowset_id : input_rowsets) {
            state->compacting_rowsets.insert(rowset_id);
        }
    }

    // Ensure rowsets are unmarked on exit
    DeferOp defer([&]() {
        std::lock_guard state_lock(state->mutex);
        for (uint32_t rowset_id : input_rowsets) {
            state->compacting_rowsets.erase(rowset_id);
        }
    });

    LOG(INFO) << "Tablet " << tablet_id << " compacting " << input_rowsets.size() << " rowsets, data size "
              << input_data_size << " bytes, has_more=" << has_more;

    // Create a dummy context for autonomous compaction
    // We don't have a real txn_id yet, will be assigned during publish
    auto context = std::make_unique<CompactionTaskContext>(-1 /* txn_id */, tablet_id, base_version,
                                                           false /* force_base_compaction */,
                                                           true /* skip_write_txnlog */,
                                                           nullptr /* callback */);

    // Create compaction task using tablet manager
    auto task_or = _tablet_manager->compact(context.get());
    RETURN_IF_ERROR(task_or.status());
    auto task = std::move(task_or).value();

    // Execute compaction
    ThreadPool* flush_pool = nullptr;
    if (config::lake_enable_compaction_async_write) {
        flush_pool = StorageEngine::instance()->lake_memtable_flush_executor()->get_thread_pool();
    }
    
    RETURN_IF_ERROR(task->execute(CompactionTask::kNoCancelFn, flush_pool));

    // Get compaction result
    CompactionResult result;
    result.tablet_id = tablet_id;
    result.base_version = base_version;
    result.input_rowsets = std::move(input_rowsets);
    // TODO: Extract output_rowset and other info from context->stats or task
    result.finish_time = UnixSeconds();

    // Save result to local storage
    RETURN_IF_ERROR(_save_compaction_result(result));

    LOG(INFO) << "Autonomous compaction completed for tablet " << tablet_id;

    // Trigger another round if there are more rowsets to compact
    if (has_more) {
        update_tablet_async(tablet_id);
    }

    return Status::OK();
}

StatusOr<bool> LakeCompactionManager::_pick_rowsets_with_limit(int64_t tablet_id, int64_t base_version,
                                                                 const std::unordered_set<uint32_t>& compacting_rowsets,
                                                                 std::vector<uint32_t>* input_rowsets,
                                                                 int64_t* input_data_size) {
    auto tablet = _tablet_manager->get_tablet(tablet_id);
    RETURN_IF_ERROR(tablet.status());

    auto metadata_or = tablet->get_metadata(base_version);
    RETURN_IF_ERROR(metadata_or.status());
    auto metadata = std::move(metadata_or).value();

    // Create compaction policy
    auto policy_or = CompactionPolicy::create_compaction_policy(_tablet_manager, metadata);
    RETURN_IF_ERROR(policy_or.status());
    auto policy = std::move(policy_or).value();

    // Pick rowsets using the policy
    std::vector<RowsetPtr> input_rowset_ptrs;
    RETURN_IF_ERROR(policy->pick_rowsets(metadata, &input_rowset_ptrs));

    // Filter out rowsets that are currently being compacted by other tasks
    // and limit the total data size
    *input_data_size = 0;
    bool has_more = false;

    for (const auto& rowset_ptr : input_rowset_ptrs) {
        uint32_t rowset_id = rowset_ptr->id();

        // Skip if already being compacted
        if (compacting_rowsets.count(rowset_id)) {
            continue;
        }

        int64_t rowset_size = rowset_ptr->data_size();

        // Check if adding this rowset would exceed the limit
        if (!input_rowsets->empty() && *input_data_size + rowset_size > config::lake_compaction_max_task_data_size) {
            has_more = true;
            break;
        }

        input_rowsets->push_back(rowset_id);
        *input_data_size += rowset_size;
    }

    return has_more;
}

Status LakeCompactionManager::_save_compaction_result(const CompactionResult& result) {
    // Create tablet result directory
    std::string tablet_result_dir = strings::Substitute("$0/$1", _result_dir, result.tablet_id);
    RETURN_IF_ERROR(fs::create_directories(tablet_result_dir));

    // Serialize result to protobuf
    CompactionResultPB result_pb;
    result_pb.set_tablet_id(result.tablet_id);
    result_pb.set_base_version(result.base_version);
    for (uint32_t rowset_id : result.input_rowsets) {
        result_pb.add_input_rowsets(rowset_id);
    }
    *result_pb.mutable_output_rowset() = result.output_rowset;
    result_pb.set_finish_time(result.finish_time);
    
    // For PK tables
    for (const auto& input_sstable : result.input_sstables) {
        *result_pb.add_input_sstables() = input_sstable;
    }
    if (result.output_sstable.has_filename()) {
        *result_pb.mutable_output_sstable() = result.output_sstable;
    }
    for (const auto& sst : result.ssts) {
        *result_pb.add_ssts() = sst;
    }

    // Write to file
    std::string result_file = strings::Substitute("$0/$1_$2.pb", tablet_result_dir, result.base_version, result.finish_time);
    ASSIGN_OR_RETURN(auto wf, fs::new_writable_file(result_file));
    
    std::string serialized;
    if (!result_pb.SerializeToString(&serialized)) {
        return Status::InternalError("Failed to serialize CompactionResultPB");
    }
    
    RETURN_IF_ERROR(wf->append(serialized));
    RETURN_IF_ERROR(wf->close());

    LOG(INFO) << "Saved compaction result for tablet " << result.tablet_id << " to " << result_file
              << ", input_rowsets=" << result.input_rowsets.size()
              << ", output_segments=" << result.output_rowset.segments_size();

    return Status::OK();
}

StatusOr<std::vector<LakeCompactionManager::CompactionResult>> LakeCompactionManager::_load_compaction_results(
        int64_t tablet_id, int64_t visible_version) {
    std::vector<CompactionResult> results;

    std::string tablet_result_dir = strings::Substitute("$0/$1", _result_dir, tablet_id);

    // Check if directory exists
    auto exists_or = fs::path_exist(tablet_result_dir);
    if (!exists_or.ok() || !exists_or.value()) {
        return results; // No results
    }

    // List all .pb files in the directory
    std::vector<std::string> files;
    RETURN_IF_ERROR(fs::list_dirs_files(tablet_result_dir, nullptr, &files));

    for (const auto& file : files) {
        if (!HasSuffixString(file, ".pb")) {
            continue;
        }

        std::string full_path = strings::Substitute("$0/$1", tablet_result_dir, file);
        
        // Read file content
        ASSIGN_OR_RETURN(auto rf, fs::new_random_access_file(full_path));
        ASSIGN_OR_RETURN(auto file_size, rf->get_size());
        
        std::string content;
        content.resize(file_size);
        RETURN_IF_ERROR(rf->read_at_fully(0, content.data(), file_size));

        // Deserialize
        CompactionResultPB result_pb;
        if (!result_pb.ParseFromString(content)) {
            LOG(WARNING) << "Failed to parse CompactionResultPB from " << full_path;
            continue;
        }

        // Filter by base_version
        if (result_pb.base_version() > visible_version) {
            VLOG(2) << "Skip compaction result with base_version " << result_pb.base_version()
                    << " > visible_version " << visible_version;
            continue;
        }

        // Convert to CompactionResult
        CompactionResult result;
        result.tablet_id = result_pb.tablet_id();
        result.base_version = result_pb.base_version();
        result.input_rowsets.assign(result_pb.input_rowsets().begin(), result_pb.input_rowsets().end());
        result.output_rowset = result_pb.output_rowset();
        result.finish_time = result_pb.finish_time();
        
        result.input_sstables.assign(result_pb.input_sstables().begin(), result_pb.input_sstables().end());
        if (result_pb.has_output_sstable()) {
            result.output_sstable = result_pb.output_sstable();
        }
        result.ssts.assign(result_pb.ssts().begin(), result_pb.ssts().end());

        results.push_back(std::move(result));
    }

    LOG(INFO) << "Loaded " << results.size() << " compaction results for tablet " << tablet_id
              << " with visible_version " << visible_version;

    return results;
}

Status LakeCompactionManager::_clear_compaction_results(int64_t tablet_id) {
    std::string tablet_result_dir = strings::Substitute("$0/$1", _result_dir, tablet_id);

    auto exists_or = fs::path_exist(tablet_result_dir);
    if (!exists_or.ok() || !exists_or.value()) {
        return Status::OK(); // Nothing to clear
    }

    RETURN_IF_ERROR(fs::remove_all(tablet_result_dir));

    LOG(INFO) << "Cleared compaction results for tablet " << tablet_id;

    return Status::OK();
}

StatusOr<std::vector<TxnLogPB>> LakeCompactionManager::get_and_clear_compaction_results(
        const std::vector<int64_t>& tablet_ids, int64_t visible_version) {
    std::vector<TxnLogPB> txn_logs;

    for (int64_t tablet_id : tablet_ids) {
        // Load compaction results for this tablet
        auto results_or = _load_compaction_results(tablet_id, visible_version);
        RETURN_IF_ERROR(results_or.status());
        auto results = std::move(results_or).value();

        if (results.empty()) {
            continue; // No results for this tablet
        }

        // Merge all results into a single TxnLogPB
        TxnLogPB txn_log;
        txn_log.set_tablet_id(tablet_id);
        auto* op_compaction = txn_log.mutable_op_compaction();

        for (const auto& result : results) {
            // Add input rowsets
            for (uint32_t input_rowset : result.input_rowsets) {
                op_compaction->add_input_rowsets(input_rowset);
            }

            // Note: output_rowset will be merged/combined later
            // For now, we just collect all segments
        }

        txn_logs.push_back(std::move(txn_log));

        // Clear results after loading
        RETURN_IF_ERROR(_clear_compaction_results(tablet_id));
    }

    return txn_logs;
}

std::shared_ptr<LakeCompactionManager::TabletCompactionState> LakeCompactionManager::_get_tablet_state(
        int64_t tablet_id) {
    std::lock_guard lock(_tablet_states_mutex);

    auto it = _tablet_states.find(tablet_id);
    if (it != _tablet_states.end()) {
        return it->second;
    }

    auto state = std::make_shared<TabletCompactionState>();
    _tablet_states[tablet_id] = state;
    return state;
}

} // namespace starrocks::lake

