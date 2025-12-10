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

#include <gtest/gtest.h>

#include "storage/lake/compaction_task_context.h"
#include "storage/lake/test_util.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"

namespace starrocks::lake {

class TabletParallelStateTest : public ::testing::Test {
protected:
    void SetUp() override { _state = std::make_unique<TabletParallelState>(); }

    std::unique_ptr<TabletParallelState> _state;
};

TEST_F(TabletParallelStateTest, test_can_create_subtask) {
    _state->max_parallel = 3;

    // Initially no running subtasks, should be able to create
    EXPECT_TRUE(_state->can_create_subtask());

    // Add running subtasks up to max
    for (int i = 0; i < 3; i++) {
        SubtaskInfo info;
        info.subtask_id = i;
        _state->running_subtasks[i] = std::move(info);
    }
    EXPECT_FALSE(_state->can_create_subtask());

    // Remove one, should be able to create again
    _state->running_subtasks.erase(0);
    EXPECT_TRUE(_state->can_create_subtask());
}

TEST_F(TabletParallelStateTest, test_is_rowset_compacting) {
    EXPECT_FALSE(_state->is_rowset_compacting(1));
    EXPECT_FALSE(_state->is_rowset_compacting(2));

    _state->compacting_rowsets.insert(1);
    _state->compacting_rowsets.insert(3);

    EXPECT_TRUE(_state->is_rowset_compacting(1));
    EXPECT_FALSE(_state->is_rowset_compacting(2));
    EXPECT_TRUE(_state->is_rowset_compacting(3));
}

TEST_F(TabletParallelStateTest, test_is_complete) {
    // No subtasks created yet
    EXPECT_FALSE(_state->is_complete());

    // Create subtasks
    _state->total_subtasks_created = 2;
    SubtaskInfo info1, info2;
    info1.subtask_id = 0;
    info2.subtask_id = 1;
    _state->running_subtasks[0] = std::move(info1);
    _state->running_subtasks[1] = std::move(info2);

    // Still running
    EXPECT_FALSE(_state->is_complete());

    // Complete one
    _state->running_subtasks.erase(0);
    EXPECT_FALSE(_state->is_complete());

    // Complete all
    _state->running_subtasks.erase(1);
    EXPECT_TRUE(_state->is_complete());
}

class TabletParallelCompactionManagerTest : public TestBase {
public:
    TabletParallelCompactionManagerTest() : TestBase(kTestDirectory) { clear_and_init_test_dir(); }

protected:
    constexpr static const char* kTestDirectory = "test_tablet_parallel_compaction_manager";

    void SetUp() override {
        _tablet_metadata = generate_simple_tablet_metadata(DUP_KEYS);
        CHECK_OK(_tablet_mgr->put_tablet_metadata(*_tablet_metadata));
        _manager = std::make_unique<TabletParallelCompactionManager>(_tablet_mgr.get());
    }

    void TearDown() override {
        _manager.reset();
        remove_test_dir_ignore_error();
    }

    std::shared_ptr<TabletMetadata> _tablet_metadata;
    std::unique_ptr<TabletParallelCompactionManager> _manager;
};

TEST_F(TabletParallelCompactionManagerTest, test_get_tablet_state_not_exist) {
    int64_t tablet_id = 12345;
    int64_t txn_id = 67890;

    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    EXPECT_EQ(nullptr, state);
}

TEST_F(TabletParallelCompactionManagerTest, test_is_tablet_complete_not_exist) {
    int64_t tablet_id = 12345;
    int64_t txn_id = 67890;

    // Non-existent tablet should return true (considered complete)
    EXPECT_TRUE(_manager->is_tablet_complete(tablet_id, txn_id));
}

TEST_F(TabletParallelCompactionManagerTest, test_cleanup_tablet) {
    int64_t tablet_id = 12345;
    int64_t txn_id = 67890;

    // Cleanup non-existent tablet should not crash
    _manager->cleanup_tablet(tablet_id, txn_id);
}

TEST_F(TabletParallelCompactionManagerTest, test_get_merged_txn_log_not_exist) {
    int64_t tablet_id = 12345;
    int64_t txn_id = 67890;

    auto result = _manager->get_merged_txn_log(tablet_id, txn_id);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.status().is_not_found());
}

TEST_F(TabletParallelCompactionManagerTest, test_metrics_initial_value) {
    EXPECT_EQ(0, _manager->running_subtasks());
    EXPECT_EQ(0, _manager->completed_subtasks());
}

TEST_F(TabletParallelCompactionManagerTest, test_on_subtask_complete_not_exist) {
    int64_t tablet_id = 12345;
    int64_t txn_id = 67890;
    int32_t subtask_id = 0;

    auto context = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, 1, false, true, nullptr);

    // Should not crash when state not exist
    _manager->on_subtask_complete(tablet_id, txn_id, subtask_id, std::move(context));
}

class SubtaskInfoTest : public ::testing::Test {};

TEST_F(SubtaskInfoTest, test_subtask_info_default_values) {
    SubtaskInfo info;
    EXPECT_EQ(0, info.subtask_id);
    EXPECT_TRUE(info.input_rowset_ids.empty());
    EXPECT_EQ(0, info.input_bytes);
    EXPECT_EQ(0, info.start_time);
    EXPECT_EQ(0, info.progress);
}

TEST_F(SubtaskInfoTest, test_subtask_info_set_values) {
    SubtaskInfo info;
    info.subtask_id = 5;
    info.input_rowset_ids = {1, 2, 3};
    info.input_bytes = 1024 * 1024;
    info.start_time = 1234567890;
    info.progress = 50;

    EXPECT_EQ(5, info.subtask_id);
    EXPECT_EQ(3, info.input_rowset_ids.size());
    EXPECT_EQ(1024 * 1024, info.input_bytes);
    EXPECT_EQ(1234567890, info.start_time);
    EXPECT_EQ(50, info.progress);
}

class TabletParallelStateFieldsTest : public ::testing::Test {
protected:
    void SetUp() override { _state = std::make_unique<TabletParallelState>(); }

    std::unique_ptr<TabletParallelState> _state;
};

TEST_F(TabletParallelStateFieldsTest, test_default_values) {
    EXPECT_EQ(0, _state->tablet_id);
    EXPECT_EQ(0, _state->txn_id);
    EXPECT_EQ(0, _state->version);
    EXPECT_EQ(0, _state->table_id);
    EXPECT_EQ(0, _state->partition_id);
    EXPECT_EQ(3, _state->max_parallel);
    EXPECT_EQ(10737418240L, _state->max_bytes_per_subtask); // 10GB
    EXPECT_EQ(0, _state->next_subtask_id);
    EXPECT_EQ(0, _state->total_subtasks_created);
    EXPECT_TRUE(_state->compacting_rowsets.empty());
    EXPECT_TRUE(_state->running_subtasks.empty());
    EXPECT_TRUE(_state->completed_subtasks.empty());
    EXPECT_EQ(nullptr, _state->callback);
}

TEST_F(TabletParallelStateFieldsTest, test_set_fields) {
    _state->tablet_id = 100;
    _state->txn_id = 200;
    _state->version = 5;
    _state->table_id = 300;
    _state->partition_id = 400;
    _state->max_parallel = 10;
    _state->max_bytes_per_subtask = 5368709120L; // 5GB
    _state->next_subtask_id = 3;
    _state->total_subtasks_created = 5;

    EXPECT_EQ(100, _state->tablet_id);
    EXPECT_EQ(200, _state->txn_id);
    EXPECT_EQ(5, _state->version);
    EXPECT_EQ(300, _state->table_id);
    EXPECT_EQ(400, _state->partition_id);
    EXPECT_EQ(10, _state->max_parallel);
    EXPECT_EQ(5368709120L, _state->max_bytes_per_subtask);
    EXPECT_EQ(3, _state->next_subtask_id);
    EXPECT_EQ(5, _state->total_subtasks_created);
}

TEST_F(TabletParallelStateFieldsTest, test_compacting_rowsets_operations) {
    _state->compacting_rowsets.insert(1);
    _state->compacting_rowsets.insert(2);
    _state->compacting_rowsets.insert(3);

    EXPECT_EQ(3, _state->compacting_rowsets.size());
    EXPECT_TRUE(_state->compacting_rowsets.count(1) > 0);
    EXPECT_TRUE(_state->compacting_rowsets.count(2) > 0);
    EXPECT_TRUE(_state->compacting_rowsets.count(3) > 0);
    EXPECT_FALSE(_state->compacting_rowsets.count(4) > 0);

    _state->compacting_rowsets.erase(2);
    EXPECT_EQ(2, _state->compacting_rowsets.size());
    EXPECT_FALSE(_state->compacting_rowsets.count(2) > 0);
}

TEST_F(TabletParallelStateFieldsTest, test_running_subtasks_operations) {
    SubtaskInfo info1;
    info1.subtask_id = 0;
    info1.input_bytes = 100;

    SubtaskInfo info2;
    info2.subtask_id = 1;
    info2.input_bytes = 200;

    _state->running_subtasks[0] = std::move(info1);
    _state->running_subtasks[1] = std::move(info2);

    EXPECT_EQ(2, _state->running_subtasks.size());
    EXPECT_EQ(100, _state->running_subtasks[0].input_bytes);
    EXPECT_EQ(200, _state->running_subtasks[1].input_bytes);

    _state->running_subtasks.erase(0);
    EXPECT_EQ(1, _state->running_subtasks.size());
    EXPECT_TRUE(_state->running_subtasks.find(0) == _state->running_subtasks.end());
}

TEST_F(TabletParallelStateFieldsTest, test_completed_subtasks_operations) {
    auto ctx1 = std::make_unique<CompactionTaskContext>(100, 101, 1, false, true, nullptr);
    auto ctx2 = std::make_unique<CompactionTaskContext>(100, 102, 1, false, true, nullptr);

    _state->completed_subtasks.push_back(std::move(ctx1));
    _state->completed_subtasks.push_back(std::move(ctx2));

    EXPECT_EQ(2, _state->completed_subtasks.size());
    EXPECT_EQ(101, _state->completed_subtasks[0]->tablet_id);
    EXPECT_EQ(102, _state->completed_subtasks[1]->tablet_id);
}

} // namespace starrocks::lake

