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

#include "common/status.h"
#include "gen_cpp/lake_service.pb.h"

namespace starrocks::lake {

class TabletManager;
class CompactionResultManager;

// Handles PUBLISH_AUTONOMOUS compaction requests
// Collects local compaction results and publishes them
class AutonomousCompactionPublisher {
public:
    explicit AutonomousCompactionPublisher(TabletManager* tablet_mgr);
    ~AutonomousCompactionPublisher();

    // Process PUBLISH_AUTONOMOUS request
    // 1. Load local compaction results for all tablets
    // 2. Group results by tablet
    // 3. Build merged TxnLog for each tablet with results
    // 4. Call publish_version for all tablets (with and without results)
    // 5. Clean up published result files
    Status process_request(const CompactRequest* request, CompactResponse* response);

private:
    // Load and filter compaction results for a tablet
    StatusOr<std::vector<CompactionResultPB>> load_tablet_results(int64_t tablet_id, int64_t max_version);

    // Build merged TxnLog from multiple compaction results
    StatusOr<TxnLogPB> build_merged_txn_log(int64_t tablet_id, const std::vector<CompactionResultPB>& results);

    // Publish version for a tablet (with or without compaction)
    Status publish_tablet(int64_t tablet_id, int64_t txn_id, int64_t base_version, int64_t new_version,
                          bool has_compaction, const TxnLogPB* txn_log);

    // Clean up result files for a tablet
    void cleanup_tablet_results(int64_t tablet_id, const std::vector<std::string>& file_paths);

    TabletManager* _tablet_mgr;
    std::unique_ptr<CompactionResultManager> _result_mgr;
};

} // namespace starrocks::lake


