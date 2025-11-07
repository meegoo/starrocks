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

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/statusor.h"
#include "gen_cpp/lake_types.pb.h"
#include "util/threadpool.h"

namespace starrocks {
class ThreadPool;
namespace lake {

class TabletManager;
class Tablet;

// Manages autonomous compaction for lake tablets.
// This class implements event-based compaction scheduling similar to shared-nothing mode,
// supporting multiple concurrent compaction tasks per tablet.
class LakeCompactionManager {
public:
    static LakeCompactionManager* instance();

    LakeCompactionManager() = default;
    ~LakeCompactionManager();

    // Initialize the compaction manager with the given tablet manager
    Status init(TabletManager* tablet_manager);

    // Stop the compaction manager and wait for all tasks to complete
    void stop();

    // Update tablet compaction score and potentially trigger compaction
    // This is called when:
    // 1. Version publish completes
    // 2. Data load completes
    // 3. Previous compaction completes
    void update_tablet_async(int64_t tablet_id);

    // Get compaction results for tablets and generate TxnLogPB for publish
    // This is called by FE's PUBLISH_AUTONOMOUS CompactionJob
    StatusOr<std::vector<TxnLogPB>> get_and_clear_compaction_results(const std::vector<int64_t>& tablet_ids,
                                                                       int64_t visible_version);

private:
    struct CompactionCandidate {
        int64_t tablet_id = 0;
        double score = 0.0;
        int64_t last_update_time = 0;

        bool operator<(const CompactionCandidate& other) const {
            // Max heap: higher score has higher priority
            if (score != other.score) {
                return score < other.score;
            }
            // If scores are equal, earlier update time has higher priority
            return last_update_time > other.last_update_time;
        }
    };

    struct TabletCompactionState {
        std::mutex mutex;
        // Rowsets currently being compacted by all tasks
        std::unordered_set<uint32_t> compacting_rowsets;
        // Number of running compaction tasks for this tablet
        int running_tasks = 0;
        // Candidate in the queue (nullptr if not in queue)
        std::shared_ptr<CompactionCandidate> queued_candidate;
    };

    struct CompactionResult {
        int64_t tablet_id = 0;
        int64_t base_version = 0; // The visible version when compaction started
        std::vector<uint32_t> input_rowsets;
        RowsetMetadataPB output_rowset;
        int64_t finish_time = 0;
        // For PK tables
        std::vector<PersistentIndexSstablePB> input_sstables;
        PersistentIndexSstablePB output_sstable;
        std::vector<FileMetaPB> ssts;
    };

    // Calculate compaction score for a tablet
    double _calculate_score(int64_t tablet_id);

    // Main scheduling loop
    void _schedule_loop();

    // Schedule next compaction task from the queue
    void _schedule_next_compaction();

    // Execute a compaction task for the given tablet
    Status _execute_compaction(int64_t tablet_id);

    // Pick rowsets for compaction with size limit
    // Returns true if there are more rowsets to compact after this batch
    StatusOr<bool> _pick_rowsets_with_limit(int64_t tablet_id, int64_t base_version,
                                             const std::unordered_set<uint32_t>& compacting_rowsets,
                                             std::vector<uint32_t>* input_rowsets, int64_t* input_data_size);

    // Save compaction result to local storage
    Status _save_compaction_result(const CompactionResult& result);

    // Load compaction results from local storage for a tablet
    StatusOr<std::vector<CompactionResult>> _load_compaction_results(int64_t tablet_id, int64_t visible_version);

    // Clear compaction results for a tablet
    Status _clear_compaction_results(int64_t tablet_id);

    // Get or create tablet compaction state
    std::shared_ptr<TabletCompactionState> _get_tablet_state(int64_t tablet_id);

private:
    TabletManager* _tablet_manager = nullptr;
    std::unique_ptr<ThreadPool> _worker_pool;
    std::unique_ptr<std::thread> _scheduler_thread;
    std::atomic<bool> _stopped{false};

    // Priority queue for compaction candidates
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    std::priority_queue<CompactionCandidate> _candidate_queue;
    std::unordered_set<int64_t> _tablets_in_queue;

    // Track tablets with running compaction tasks
    std::unordered_set<int64_t> _tablets_running;

    // Per-tablet compaction state
    std::mutex _tablet_states_mutex;
    std::unordered_map<int64_t, std::shared_ptr<TabletCompactionState>> _tablet_states;

    // Local storage for compaction results
    std::string _result_dir;
};

} // namespace lake
} // namespace starrocks


