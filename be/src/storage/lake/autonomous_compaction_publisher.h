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

#include <unordered_set>

#include "common/status.h"
#include "gen_cpp/lake_service.pb.h"

namespace starrocks::lake {

class TabletManager;
class CompactionResultManager;
class LakeCompactionManager;

// Statistics for PUBLISH_AUTONOMOUS processing
struct PublishAutonomousStats {
    int64_t tablets_processed = 0;
    int64_t tablets_with_compaction = 0;
    int64_t tablets_published = 0;
    int64_t tablets_failed = 0;
    int64_t tablets_scheduled = 0;  // Tablets added to compaction queue
    int64_t result_files_cleaned = 0;
};

// Handles PUBLISH_AUTONOMOUS compaction requests
//
// PUBLISH_AUTONOMOUS has dual roles (Design Doc Section 6.3.4):
//
// Role 1: Result Collection + Publish
//   - Collect BE local results
//   - Build TxnLog and write to S3
//   - Execute publish_version
//
// Role 2: Scheduling Trigger (Mechanism 3 from Section 6.1.1)
//   - Add tablets in the request into BE's compaction scheduling queue
//   - Automatically reactivates compaction after BE crash
//   - Maintains continuous autonomous compaction pipeline
//
// This enables on-demand recovery without full scans after BE restart.
class AutonomousCompactionPublisher {
public:
    explicit AutonomousCompactionPublisher(TabletManager* tablet_mgr);
    ~AutonomousCompactionPublisher();

    // Process PUBLISH_AUTONOMOUS request
    // Dual roles (Section 6.3.4):
    // 1. Result Collection + Publish:
    //    - Load local compaction results for all tablets
    //    - Group results by tablet
    //    - Build merged TxnLog for each tablet with results
    //    - Call publish_version for all tablets (with and without results)
    //    - Clean up published result files
    // 2. Scheduling Trigger:
    //    - Add all tablet_ids to the compaction scheduling queue
    //    - This reactivates compaction for tablets that may need it
    Status process_request(const CompactRequest* request, CompactResponse* response);

    // Get statistics from the last process_request call
    const PublishAutonomousStats& last_stats() const { return _last_stats; }

private:
    // Load and filter compaction results for a tablet
    // Uses load_and_validate_results for proper validation
    StatusOr<std::vector<CompactionResultPB>> load_tablet_results(int64_t tablet_id, int64_t max_version);

    // Build merged TxnLog from multiple compaction results
    StatusOr<TxnLogPB> build_merged_txn_log(int64_t tablet_id, const std::vector<CompactionResultPB>& results);

    // Publish version for a tablet (with or without compaction)
    Status publish_tablet(int64_t tablet_id, int64_t txn_id, int64_t base_version, int64_t new_version,
                          bool has_compaction, const TxnLogPB* txn_log);

    // Clean up result files for a tablet
    void cleanup_tablet_results(int64_t tablet_id, const std::vector<std::string>& file_paths);

    // ============== Scheduling Trigger (Role 2) ==============
    
    // Trigger compaction scheduling for tablets in the request
    // This implements Mechanism 3: PUBLISH_AUTONOMOUS Triggers Scheduling
    // - The FE request contains all tablet_ids in the partition
    // - BE adds these tablets to its compaction scheduling queue
    // - Enables on-demand recovery without full scans
    void trigger_compaction_scheduling(const std::vector<int64_t>& tablet_ids);

    // Check which tablets need compaction (have high score or pending results)
    std::unordered_set<int64_t> identify_tablets_needing_compaction(const std::vector<int64_t>& tablet_ids);

    TabletManager* _tablet_mgr;
    std::unique_ptr<CompactionResultManager> _result_mgr;
    PublishAutonomousStats _last_stats;
};

} // namespace starrocks::lake


