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

#include "storage/lake/lake_compaction_manager.h"

#include "common/config.h"
#include "common/logging.h"
#include "gutil/strings/substitute.h"
#include "storage/lake/compaction_policy.h"
#include "storage/lake/compaction_result_manager.h"
#include "storage/lake/tablet_manager.h"
#include "util/defer_op.h"

namespace starrocks::lake {

// Singleton instance
static LakeCompactionManager* g_lake_compaction_manager = nullptr;
static std::once_flag g_init_once;
static std::mutex g_mgr_mutex;

Status LakeCompactionManager::create_instance(TabletManager* tablet_mgr) {
    std::lock_guard<std::mutex> lock(g_mgr_mutex);
    if (g_lake_compaction_manager != nullptr) {
        return Status::OK();
    }

    if (tablet_mgr == nullptr) {
        return Status::InvalidArgument("tablet_mgr cannot be null");
    }

    g_lake_compaction_manager = new LakeCompactionManager(tablet_mgr);
    RETURN_IF_ERROR(g_lake_compaction_manager->init());

    // Recover from local results after initialization
    // This implements Design Doc Section 6.1.1 Mechanism 1: Local Result Recovery
    if (config::enable_lake_autonomous_compaction) {
        auto st = g_lake_compaction_manager->recover_from_local_results();
        if (!st.ok()) {
            LOG(WARNING) << "Failed to recover from local compaction results: " << st;
            // Continue anyway - recovery failure shouldn't prevent startup
        }
    }

    return Status::OK();
}

LakeCompactionManager* LakeCompactionManager::instance() {
    // Return nullptr instead of FATAL if not initialized
    // This allows callers to check and handle the case gracefully
    return g_lake_compaction_manager;
}

LakeCompactionManager::LakeCompactionManager(TabletManager* tablet_mgr)
        : _tablet_mgr(tablet_mgr), _result_mgr(std::make_unique<CompactionResultManager>()) {}

LakeCompactionManager::~LakeCompactionManager() {
    stop();
}

Status LakeCompactionManager::init() {
    if (_stopped.load()) {
        return Status::InternalError("LakeCompactionManager already stopped");
    }

    if (_tablet_mgr == nullptr) {
        return Status::InvalidArgument("_tablet_mgr cannot be null");
    }

    // Validate configuration parameters (P8 fix)
    if (config::lake_compaction_max_concurrent_tasks <= 0) {
        return Status::InvalidArgument(
            fmt::format("Invalid lake_compaction_max_concurrent_tasks: {}",
                       config::lake_compaction_max_concurrent_tasks));
    }

    if (config::lake_compaction_max_tasks_per_tablet <= 0) {
        return Status::InvalidArgument(
            fmt::format("Invalid lake_compaction_max_tasks_per_tablet: {}",
                       config::lake_compaction_max_tasks_per_tablet));
    }

    if (config::lake_compaction_max_bytes_per_task <= 0) {
        return Status::InvalidArgument(
            fmt::format("Invalid lake_compaction_max_bytes_per_task: {}",
                       config::lake_compaction_max_bytes_per_task));
    }

    if (config::lake_compaction_score_threshold < 0) {
        return Status::InvalidArgument(
            fmt::format("Invalid lake_compaction_score_threshold: {}",
                       config::lake_compaction_score_threshold));
    }

    // Create thread pool for executing compaction tasks
    RETURN_IF_ERROR(ThreadPoolBuilder("lake_auto_compact")
                            .set_min_threads(0)
                            .set_max_threads(config::lake_compaction_max_concurrent_tasks)
                            .set_max_queue_size(1000)
                            .build(&_thread_pool));

    // Start background scheduler thread (P2 fix - correct method name)
    _schedule_thread = std::make_unique<std::thread>(
        [this]() { this->_schedule_thread_func(); }
    );

    LOG(INFO) << "LakeCompactionManager initialized with max_concurrent_tasks="
              << config::lake_compaction_max_concurrent_tasks;

    return Status::OK();
}

void LakeCompactionManager::stop() {
    if (_stopped.exchange(true)) {
        return;
    }

    LOG(INFO) << "Stopping LakeCompactionManager";

    // Wake up scheduler thread
    _queue_cv.notify_all();

    // Wait for scheduler thread to exit
    if (_schedule_thread && _schedule_thread->joinable()) {
        _schedule_thread->join();
    }

    // Shutdown thread pool
    if (_thread_pool) {
        _thread_pool->shutdown();
    }

    LOG(INFO) << "LakeCompactionManager stopped";
}

void LakeCompactionManager::update_tablet_async(int64_t tablet_id) {
    if (_stopped.load()) {
        return;
    }

    // Calculate compaction score
    auto score_or = _calculate_score(tablet_id);
    if (!score_or.ok()) {
        VLOG(2) << "Failed to calculate score for tablet " << tablet_id << ": " << score_or.status();
        return;
    }

    double score = score_or.value();
    
    // Check for pending results - tablets with pending results should always be scheduled
    bool has_pending = _result_mgr->has_pending_results(tablet_id);
    
    // Only add to queue if score exceeds threshold OR has pending results
    if (score < config::lake_compaction_score_threshold && !has_pending) {
        VLOG(2) << "Tablet " << tablet_id << " score " << score << " below threshold "
                << config::lake_compaction_score_threshold << " and no pending results";
        return;
    }

    if (has_pending) {
        LOG(INFO) << "Tablet " << tablet_id << " has pending compaction results, scheduling for publish";
    }

    _update_tablet_state(tablet_id, score);
}

void LakeCompactionManager::update_tablets_async(const std::vector<int64_t>& tablet_ids) {
    if (_stopped.load()) {
        return;
    }

    LOG(INFO) << "Batch updating " << tablet_ids.size() << " tablets for compaction scheduling";

    for (int64_t tablet_id : tablet_ids) {
        update_tablet_async(tablet_id);
    }
}

// ============== Startup Recovery Implementation ==============

Status LakeCompactionManager::recover_from_local_results() {
    if (_recovered.exchange(true)) {
        LOG(INFO) << "LakeCompactionManager already recovered, skipping";
        return Status::OK();
    }

    LOG(INFO) << "Starting LakeCompactionManager recovery from local compaction results...";

    // Step 1: Call CompactionResultManager to scan and validate local results
    // This implements Design Doc Section 6.1.1 Mechanism 1:
    // "After BE restart, CompactionResultManager scans the local result directory"
    auto stats_or = _result_mgr->recover_on_startup(_tablet_mgr);
    if (!stats_or.ok()) {
        LOG(WARNING) << "Failed to recover compaction results: " << stats_or.status();
        return stats_or.status();
    }

    auto stats = stats_or.value();

    // Step 2: Update tablet states for tablets with valid pending results
    // This ensures these tablets are prioritized in the scheduling queue
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        for (int64_t tablet_id : stats.recovered_tablet_ids) {
            auto& state = _tablet_states[tablet_id];
            state.tablet_id = tablet_id;
            state.has_pending_results = true;
        }
    }

    // Step 3: NO FULL TABLET SCAN (Design Doc Section 6.3.1)
    // "Why No BE Startup Self-Check Is Needed?"
    // - Score-Driven Scheduling: Partition score remains high → FE schedules it again
    // - Request-Triggered Scheduling: PUBLISH_AUTONOMOUS brings tablet_ids → BE queues them on demand
    // - On-Demand Execution: BE only processes partitions FE determines require compaction
    //
    // We only trigger scheduling for tablets that have valid pending results
    // FE will handle the rest through its periodic scheduling

    _recovered_tablets.store(stats.recovered_tablet_ids.size());

    LOG(INFO) << "LakeCompactionManager recovery completed: "
              << "valid_results=" << stats.valid_results
              << ", cleaned=" << stats.invalid_results_cleaned
              << ", tablets_with_pending_results=" << stats.recovered_tablet_ids.size();

    // Schedule tablets with pending results for immediate processing
    // These tablets need to publish their results
    for (int64_t tablet_id : stats.recovered_tablet_ids) {
        update_tablet_async(tablet_id);
    }

    return Status::OK();
}

// ============== Query Methods Implementation ==============

bool LakeCompactionManager::has_pending_results(int64_t tablet_id) const {
    std::lock_guard<std::mutex> lock(_state_mutex);
    auto it = _tablet_states.find(tablet_id);
    if (it != _tablet_states.end()) {
        return it->second.has_pending_results;
    }
    return _result_mgr->has_pending_results(tablet_id);
}

std::unordered_set<uint32_t> LakeCompactionManager::get_compacting_rowsets(int64_t tablet_id) const {
    std::lock_guard<std::mutex> lock(_state_mutex);
    auto it = _tablet_states.find(tablet_id);
    if (it != _tablet_states.end()) {
        return it->second.compacting_rowsets;
    }
    return {};
}

bool LakeCompactionManager::has_partial_compaction_state(int64_t tablet_id) const {
    // A tablet has partial compaction state if:
    // 1. It has pending results (some compaction finished)
    // 2. It has running tasks (some compaction still in progress)
    std::lock_guard<std::mutex> lock(_state_mutex);
    auto it = _tablet_states.find(tablet_id);
    if (it == _tablet_states.end()) {
        return false;
    }

    const auto& state = it->second;
    return state.has_pending_results && state.running_tasks > 0;
}

StatusOr<double> LakeCompactionManager::_calculate_score(int64_t tablet_id) {
    // Get tablet metadata
    ASSIGN_OR_RETURN(auto tablet, _tablet_mgr->get_tablet(tablet_id));
    auto metadata = tablet.metadata();
    
    if (!metadata) {
        return Status::NotFound(strings::Substitute("Tablet $0 metadata not found", tablet_id));
    }

    // Calculate compaction score
    double score = compaction_score(_tablet_mgr, metadata);
    
    return score;
}

void LakeCompactionManager::_update_tablet_state(int64_t tablet_id, double score) {
    // P6 fix: Separate lock operations to avoid potential deadlock
    // Step 1: Update state
    TabletCompactionInfo info;
    bool should_add_to_queue = false;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);

        auto& state = _tablet_states[tablet_id];
        state.tablet_id = tablet_id;
        state.last_score = score;
        state.last_update_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count();

        if (!state.in_queue) {
            info.tablet_id = tablet_id;
            info.score = score;
            info.last_update_time_ms = state.last_update_time_ms;
            should_add_to_queue = true;
        }
    }

    // Step 2: Add to queue if needed (outside state lock)
    if (should_add_to_queue) {
        std::lock_guard<std::mutex> queue_lock(_queue_mutex);
        _tablet_queue.push(info);

        // Update in_queue flag
        {
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            auto it = _tablet_states.find(tablet_id);
            if (it != _tablet_states.end()) {
                it->second.in_queue = true;
            }
        }

        VLOG(2) << "Added tablet " << tablet_id << " to compaction queue with score " << info.score;

        // Wake up scheduler thread
        _queue_cv.notify_one();
    }
}

int64_t LakeCompactionManager::queue_size() const {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    return _tablet_queue.size();
}

bool LakeCompactionManager::_can_schedule_tablet(const TabletCompactionState& state) {
    // Check global concurrent task limit
    if (_running_tasks.load() >= config::lake_compaction_max_concurrent_tasks) {
        return false;
    }

    // Check per-tablet task limit
    if (state.running_tasks >= config::lake_compaction_max_tasks_per_tablet) {
        return false;
    }

    return true;
}

void LakeCompactionManager::_schedule_thread_func() {
    LOG(INFO) << "LakeCompactionManager scheduler thread started";

    while (!_stopped.load()) {
        std::unique_lock<std::mutex> lock(_queue_mutex);

        // Wait for tablets in queue or stop signal
        _queue_cv.wait_for(lock, std::chrono::seconds(1), [this]() { return !_tablet_queue.empty() || _stopped.load(); });

        if (_stopped.load()) {
            break;
        }

        if (_tablet_queue.empty()) {
            continue;
        }

        // Get highest priority tablet
        auto info = _tablet_queue.top();
        _tablet_queue.pop();
        lock.unlock();

        // Mark as not in queue
        {
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            auto it = _tablet_states.find(info.tablet_id);
            if (it != _tablet_states.end()) {
                it->second.in_queue = false;
            }
        }

        // Try to schedule compaction for this tablet
        auto st = _schedule_tablet(info.tablet_id);
        if (!st.ok()) {
            VLOG(2) << "Failed to schedule tablet " << info.tablet_id << ": " << st;
            
            // Re-add to queue if it's a temporary failure
            if (st.is_resource_busy()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                update_tablet_async(info.tablet_id);
            }
        }
    }

    LOG(INFO) << "LakeCompactionManager scheduler thread exited";
}

Status LakeCompactionManager::_schedule_tablet(int64_t tablet_id) {
    // Get tablet state
    std::unique_lock<std::mutex> state_lock(_state_mutex);
    auto it = _tablet_states.find(tablet_id);
    if (it == _tablet_states.end()) {
        return Status::NotFound(strings::Substitute("Tablet $0 state not found", tablet_id));
    }

    auto& state = it->second;

    // Check if we can schedule more tasks for this tablet
    if (!_can_schedule_tablet(state)) {
        return Status::ResourceBusy(strings::Substitute("Tablet $0 already at max concurrent tasks", tablet_id));
    }

    // Get tablet and metadata
    ASSIGN_OR_RETURN(auto tablet, _tablet_mgr->get_tablet(tablet_id));
    auto metadata = tablet.metadata();

    // Create compaction policy
    ASSIGN_OR_RETURN(auto policy, CompactionPolicy::create(_tablet_mgr, metadata, false));

    // Pick rowsets with limit and exclusion
    ASSIGN_OR_RETURN(auto input_rowsets,
                     policy->pick_rowsets_with_limit(config::lake_compaction_max_bytes_per_task, state.compacting_rowsets));

    if (input_rowsets.empty()) {
        VLOG(2) << "No rowsets to compact for tablet " << tablet_id;
        return Status::OK();
    }

    // Collect rowset IDs
    std::vector<uint32_t> input_rowset_ids;
    for (const auto& rowset : input_rowsets) {
        input_rowset_ids.push_back(rowset->id());
    }

    // Mark rowsets as being compacted
    _mark_rowsets_compacting(tablet_id, input_rowset_ids);

    // Update running tasks count
    state.running_tasks++;
    _running_tasks++;

    state_lock.unlock();

    // Create compaction context
    auto context = std::make_unique<CompactionTaskContext>(0 /* txn_id */, tablet_id, metadata->version(),
                                                           false /* is_checker */, nullptr /* callback */);

    // Submit to thread pool
    // Note: Copy input_rowset_ids instead of moving so it remains valid for error handling below
    auto submit_st = _thread_pool->submit_func([this, ctx = std::move(context), rowset_ids = input_rowset_ids]() mutable {
        _execute_compaction(std::move(ctx), std::move(rowset_ids));
    });

    if (!submit_st.ok()) {
        // Failed to submit, revert state changes
        std::lock_guard<std::mutex> lock(_state_mutex);
        auto it = _tablet_states.find(tablet_id);
        if (it != _tablet_states.end()) {
            _unmark_rowsets_compacting(tablet_id, input_rowset_ids);
            it->second.running_tasks--;
            _running_tasks--;
        }
        return submit_st;
    }

    LOG(INFO) << "Scheduled autonomous compaction for tablet " << tablet_id << ", rowsets: " << input_rowset_ids.size();

    return Status::OK();
}

void LakeCompactionManager::_mark_rowsets_compacting(int64_t tablet_id, const std::vector<uint32_t>& rowset_ids) {
    // Assumes _state_mutex is already held
    auto& state = _tablet_states[tablet_id];
    for (uint32_t rid : rowset_ids) {
        state.compacting_rowsets.insert(rid);
    }
}

void LakeCompactionManager::_unmark_rowsets_compacting(int64_t tablet_id, const std::vector<uint32_t>& rowset_ids) {
    // Assumes _state_mutex is already held
    auto it = _tablet_states.find(tablet_id);
    if (it != _tablet_states.end()) {
        for (uint32_t rid : rowset_ids) {
            it->second.compacting_rowsets.erase(rid);
        }
    }
}

void LakeCompactionManager::_execute_compaction(std::unique_ptr<CompactionTaskContext> context,
                                                 std::vector<uint32_t> input_rowset_ids) {
    int64_t tablet_id = context->tablet_id;
    
    // Use the rowset IDs passed from _schedule_tablet for unmarking
    // This fixes the bug where rowsets marked in _schedule_tablet were never unmarked
    DeferOp defer([this, tablet_id, &input_rowset_ids]() {
        // Decrement running tasks count
        _running_tasks--;

        std::lock_guard<std::mutex> lock(_state_mutex);
        auto it = _tablet_states.find(tablet_id);
        if (it != _tablet_states.end()) {
            it->second.running_tasks--;
        }
        // Unmark the rowsets that were marked in _schedule_tablet
        _unmark_rowsets_compacting(tablet_id, input_rowset_ids);
    });

    VLOG(1) << "Executing autonomous compaction for tablet " << tablet_id;

    // Get tablet
    auto tablet_or = _tablet_mgr->get_tablet(tablet_id);
    if (!tablet_or.ok()) {
        LOG(WARNING) << "Failed to get tablet " << tablet_id << ": " << tablet_or.status();
        _failed_tasks++;
        return;
    }

    auto tablet = tablet_or.value();
    auto metadata = tablet.metadata();

    // Perform compaction using tablet_mgr->compact()
    context->version = metadata->version();
    auto compaction_task_or = _tablet_mgr->compact(context.get());
    
    if (!compaction_task_or.ok()) {
        LOG(WARNING) << "Failed to create compaction task for tablet " << tablet_id << ": " << compaction_task_or.status();
        // Note: unmarking is done in DeferOp, no need to do it here
        _failed_tasks++;
        return;
    }

    auto compaction_task = compaction_task_or.value();
    
    // Execute compaction
    auto cancel_func = []() { return Status::OK(); };
    auto exec_st = compaction_task->execute(cancel_func, nullptr);

    // Note: unmarking is done in DeferOp, no need to do it here

    if (!exec_st.ok()) {
        LOG(WARNING) << "Compaction failed for tablet " << tablet_id << ": " << exec_st;
        _failed_tasks++;
        return;
    }

    // Save compaction result locally
    CompactionResultPB result;
    result.set_tablet_id(tablet_id);
    result.set_base_version(metadata->version());
    result.set_finish_time_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    
    for (uint32_t rid : input_rowset_ids) {
        result.add_input_rowset_ids(rid);
    }

    // Get output rowset from txn log
    // Note: In real implementation, we need to extract this from compaction_task
    // For now, this is a placeholder
    // *result.mutable_output_rowset() = ...; // TODO: Extract from compaction task

    auto save_st = _result_mgr->save_result(result);
    if (!save_st.ok()) {
        LOG(WARNING) << "Failed to save compaction result for tablet " << tablet_id << ": " << save_st.status();
        _failed_tasks++;
        return;
    }

    _completed_tasks++;

    LOG(INFO) << "Autonomous compaction completed for tablet " << tablet_id 
              << ", result saved to " << save_st.value();

    // Re-evaluate the tablet for potential follow-up compaction
    update_tablet_async(tablet_id);
}

} // namespace starrocks::lake


