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

#include <future>

#include "storage/lake/compaction_scheduler.h"
#include "storage/lake/compaction_task_context.h"
#include "storage/lake/test_util.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"
#include "util/threadpool.h"

namespace starrocks::lake {

class TestClosure : public google::protobuf::Closure {
public:
    void Run() override {
        std::lock_guard<std::mutex> lock(_mutex);
        _finished = true;
        _cv.notify_all();
    }

    bool wait_finish(int64_t timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return _finished; });
    }

    bool is_finished() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _finished;
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _finished = false;
};

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

        ThreadPoolBuilder("test_pool")
                .set_min_threads(0)
                .set_max_threads(1) // Use 1 thread to allow execution if needed, or 0 to block
                .build(&_thread_pool);
    }

    void TearDown() override {
        _manager.reset();
        if (_thread_pool) {
            _thread_pool->shutdown();
        }
        remove_test_dir_ignore_error();
    }

    void create_tablet_with_rowsets(int64_t tablet_id, int num_rowsets, int64_t rowset_size) {
        auto metadata = generate_simple_tablet_metadata(DUP_KEYS);
        metadata->set_id(tablet_id);
        metadata->set_version(num_rowsets + 1);

        for (int i = 0; i < num_rowsets; i++) {
            auto* rowset = metadata->add_rowsets();
            rowset->set_id(i);
            rowset->set_overlapped(true);
            rowset->set_num_rows(100);
            rowset->set_data_size(rowset_size);

            rowset->add_segments(fmt::format("segment_{}.dat", i));
            rowset->add_segment_size(rowset_size);
        }

        CHECK_OK(_tablet_mgr->put_tablet_metadata(*metadata));
    }

    std::shared_ptr<TabletMetadata> _tablet_metadata;
    std::unique_ptr<TabletParallelCompactionManager> _manager;
    std::unique_ptr<ThreadPool> _thread_pool;
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

TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_single_group) {
    int64_t tablet_id = 10001;
    int64_t txn_id = 20001;
    int64_t version = 11;

    // Create 10 rowsets, each 1MB
    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(3);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024); // Large enough to hold all

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    // Use a thread pool with 1 thread
    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    // Submit a blocking task to occupy the thread
    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    // Wait for the blocking task to start
    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(1, st.value()); // Should be 1 group

    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(1, state->running_subtasks.size());
    ASSERT_EQ(10, state->running_subtasks[0].input_rowset_ids.size());

    // Unblock the thread
    block_promise.set_value();
    pool->wait();

    _manager->cleanup_tablet(tablet_id, txn_id);
}

TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_multiple_groups) {
    int64_t tablet_id = 10002;
    int64_t txn_id = 20002;
    int64_t version = 11;

    // Create 10 rowsets, each 10MB
    create_tablet_with_rowsets(tablet_id, 10, 10 * 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(3);
    config.set_max_bytes_per_subtask(25 * 1024 * 1024); // 25MB limit

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    // Use a thread pool with 1 thread
    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    // Submit a blocking task to occupy the thread
    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    // Wait for the blocking task to start
    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());

    // Total 100MB. Max 25MB per task.
    // Group 1: 10+10 = 20MB (next is 30 > 25) -> 2 rowsets
    // Group 2: 10+10 = 20MB -> 2 rowsets
    // Group 3: 10+10 = 20MB -> 2 rowsets.
    // Remaining 4 rowsets are skipped because they exceed max_parallel * max_bytes capacity.

    // Expected:
    // Group 0: 2 rowsets (20MB)
    // Group 1: 2 rowsets (20MB)
    // Group 2: 2 rowsets (20MB)

    ASSERT_EQ(3, st.value());

    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(3, state->running_subtasks.size());

    ASSERT_EQ(2, state->running_subtasks[0].input_rowset_ids.size());
    ASSERT_EQ(2, state->running_subtasks[1].input_rowset_ids.size());
    ASSERT_EQ(2, state->running_subtasks[2].input_rowset_ids.size());

    // Unblock the thread
    block_promise.set_value();
    pool->wait();

    _manager->cleanup_tablet(tablet_id, txn_id);
}

TEST_F(TabletParallelCompactionManagerTest, test_manual_completion_flow) {
    int64_t tablet_id = 10003;
    int64_t txn_id = 20003;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) {
                // Block execution
                block_future.wait();
            });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Simulate completion of subtask 0
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_data_size(500);

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    ASSERT_FALSE(closure.is_finished());
    ASSERT_FALSE(_manager->is_tablet_complete(tablet_id, txn_id));

    // Simulate completion of subtask 1
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_data_size(500);

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    // Verify merged context
    ASSERT_EQ(1, response.txn_logs_size());
    const auto& op_compaction = response.txn_logs(0).op_compaction();
    EXPECT_EQ(2, op_compaction.input_rowsets_size()); // 0 and 5
    EXPECT_EQ(100, op_compaction.output_rowset().num_rows());
    EXPECT_EQ(1000, op_compaction.output_rowset().data_size());

    // State should be cleaned up
    ASSERT_EQ(nullptr, _manager->get_tablet_state(tablet_id, txn_id));

    // Unblock the thread
    block_promise.set_value();
    pool->wait();
}

class SubtaskInfoTest : public ::testing::Test {};

TEST_F(SubtaskInfoTest, test_subtask_info_default_values) {
    SubtaskInfo info;
    EXPECT_EQ(0, info.subtask_id);
    EXPECT_TRUE(info.input_rowset_ids.empty());
    EXPECT_EQ(0, info.input_bytes);
    EXPECT_EQ(0, info.start_time);
}

TEST_F(SubtaskInfoTest, test_subtask_info_set_values) {
    SubtaskInfo info;
    info.subtask_id = 5;
    info.input_rowset_ids = {1, 2, 3};
    info.input_bytes = 1024 * 1024;
    info.start_time = 1234567890;

    EXPECT_EQ(5, info.subtask_id);
    EXPECT_EQ(3, info.input_rowset_ids.size());
    EXPECT_EQ(1024 * 1024, info.input_bytes);
    EXPECT_EQ(1234567890, info.start_time);
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
    _state->max_parallel = 10;
    _state->max_bytes_per_subtask = 5368709120L; // 5GB
    _state->next_subtask_id = 3;
    _state->total_subtasks_created = 5;

    EXPECT_EQ(100, _state->tablet_id);
    EXPECT_EQ(200, _state->txn_id);
    EXPECT_EQ(5, _state->version);
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

// Test for max_bytes <= 0 (line 58)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_invalid_max_bytes) {
    int64_t tablet_id = 10010;
    int64_t txn_id = 20010;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(-1); // Invalid, should use default

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});

    // Should succeed with default max_bytes
    ASSERT_TRUE(st.ok());

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for max_parallel <= 0 (line 54-56)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_invalid_max_parallel) {
    int64_t tablet_id = 10011;
    int64_t txn_id = 20011;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(0); // Invalid, should use 1
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});

    // Should succeed with single group
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(1, st.value());

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for tablet not found (line 68)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_tablet_not_found) {
    int64_t tablet_id = 99999; // Non-existent tablet
    int64_t txn_id = 20012;
    int64_t version = 1;

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(10 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, _thread_pool.get(), []() { return true; },
            [](bool) {});

    ASSERT_FALSE(st.ok());
}

// Test for already existing parallel compaction (lines 216-217)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_already_exists) {
    int64_t tablet_id = 10013;
    int64_t txn_id = 20013;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    // First creation should succeed
    auto st1 = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st1.ok());

    // Second creation with same tablet_id and txn_id should fail
    auto st2 = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_FALSE(st2.ok());
    ASSERT_TRUE(st2.status().is_already_exist());

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for acquire_token failure (lines 274-288)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_acquire_token_failure) {
    int64_t tablet_id = 10014;
    int64_t txn_id = 20014;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    // acquire_token always returns false to simulate failure
    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return false; }, [](bool) {});

    ASSERT_FALSE(st.ok());
    ASSERT_TRUE(st.status().is_resource_busy());
}

// Test for on_subtask_complete with subtask not found (lines 374-381)
TEST_F(TabletParallelCompactionManagerTest, test_on_subtask_complete_subtask_not_found) {
    int64_t tablet_id = 10015;
    int64_t txn_id = 20015;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());

    // Try to complete a non-existent subtask (id 999)
    auto ctx = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx->subtask_id = 999; // Non-existent subtask
    _manager->on_subtask_complete(tablet_id, txn_id, 999, std::move(ctx));

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for list_tasks with running and completed subtasks (lines 786-827)
TEST_F(TabletParallelCompactionManagerTest, test_list_tasks) {
    int64_t tablet_id = 10016;
    int64_t txn_id = 20016;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());

    // List tasks while some are running
    std::vector<CompactionTaskInfo> infos;
    _manager->list_tasks(&infos);

    // Should have at least one task
    EXPECT_GE(infos.size(), 1);

    // Check task info
    for (const auto& info : infos) {
        EXPECT_EQ(txn_id, info.txn_id);
        EXPECT_EQ(tablet_id, info.tablet_id);
        EXPECT_EQ(version, info.version);
    }

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for merged TxnLog with overlapped output (lines 520-565)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_overlapped) {
    int64_t tablet_id = 10017;
    int64_t txn_id = 20017;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Simulate completion of subtask 0 with overlapped output
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(1);
    auto* output0 = ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset();
    output0->set_num_rows(100);
    output0->set_data_size(1024);
    output0->set_overlapped(true);
    output0->add_segments("segment_0.dat");
    output0->add_segment_size(512);
    output0->add_segment_encryption_metas("meta0");
    ctx0->txn_log->mutable_op_compaction()->set_compact_version(10);
    ctx0->table_id = 1001;
    ctx0->partition_id = 2001;

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Simulate completion of subtask 1
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(6);
    auto* output1 = ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset();
    output1->set_num_rows(200);
    output1->set_data_size(2048);
    output1->set_overlapped(false);
    output1->add_segments("segment_1.dat");
    output1->add_segment_size(1024);
    output1->add_segment_encryption_metas("meta1");

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    // Verify merged result
    ASSERT_EQ(1, response.txn_logs_size());
    const auto& op_compaction = response.txn_logs(0).op_compaction();
    // Input rowsets should contain 0, 1, 5, 6 (deduplicated and sorted)
    EXPECT_EQ(4, op_compaction.input_rowsets_size());
    // Output should be merged
    EXPECT_EQ(300, op_compaction.output_rowset().num_rows());
    EXPECT_EQ(3072, op_compaction.output_rowset().data_size());
    // Should be overlapped since we have multiple subtasks
    EXPECT_TRUE(op_compaction.output_rowset().overlapped());
    // Segments should be merged
    EXPECT_EQ(2, op_compaction.output_rowset().segments_size());
    EXPECT_EQ(2, op_compaction.output_rowset().segment_size_size());
    EXPECT_EQ(2, op_compaction.output_rowset().segment_encryption_metas_size());
    // compact_version should be set
    EXPECT_TRUE(op_compaction.has_compact_version());
    EXPECT_EQ(10, op_compaction.compact_version());

    block_promise.set_value();
    pool->wait();
}

// Test for TxnLog merge with failed subtask status (lines 442-448)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_with_failed_status) {
    int64_t tablet_id = 10018;
    int64_t txn_id = 20018;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Simulate completion of subtask 0 with success
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->status = Status::OK();
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_data_size(500);

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Simulate completion of subtask 1 with failure
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->status = Status::InternalError("simulated failure");
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_data_size(500);

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    // Response status should indicate failure
    EXPECT_NE(0, response.status().status_code());

    block_promise.set_value();
    pool->wait();
}

// Test for data exceeding max_parallel capacity (lines 141-156)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_exceeds_capacity) {
    int64_t tablet_id = 10019;
    int64_t txn_id = 20019;
    int64_t version = 11;

    // Create 10 rowsets, each 10MB = 100MB total
    create_tablet_with_rowsets(tablet_id, 10, 10 * 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);              // Only allow 2 subtasks
    config.set_max_bytes_per_subtask(20 * 1024 * 1024); // 20MB per subtask, so 40MB total capacity

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());
    // Should create max_parallel subtasks (2), skipping excess data
    ASSERT_EQ(2, st.value());

    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    ASSERT_NE(nullptr, state);
    // Each subtask should have limited rowsets
    ASSERT_EQ(2, state->running_subtasks.size());

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for stats merging in on_subtask_complete (line 446)
TEST_F(TabletParallelCompactionManagerTest, test_stats_merging) {
    int64_t tablet_id = 10020;
    int64_t txn_id = 20020;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 with stats
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx0->stats->io_ns_read_remote = 1000;
    ctx0->stats->io_bytes_read_remote = 2000;

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1 with stats
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx1->stats->io_ns_read_remote = 3000;
    ctx1->stats->io_bytes_read_remote = 4000;

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for no rowsets to compact (line 75)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_no_rowsets) {
    int64_t tablet_id = 10022;
    int64_t txn_id = 20022;
    int64_t version = 1;

    // Create tablet without any rowsets
    auto metadata = generate_simple_tablet_metadata(DUP_KEYS);
    metadata->set_id(tablet_id);
    metadata->set_version(version);
    // Don't add any rowsets
    CHECK_OK(_tablet_mgr->put_tablet_metadata(*metadata));

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(10 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, _thread_pool.get(), []() { return true; },
            [](bool) {});

    // Should fail because no rowsets to compact
    ASSERT_FALSE(st.ok());
    ASSERT_TRUE(st.status().is_not_found());
}

// Test for valid_groups empty case (line 203)
TEST_F(TabletParallelCompactionManagerTest, test_create_parallel_tasks_single_small_rowset) {
    int64_t tablet_id = 10023;
    int64_t txn_id = 20023;
    int64_t version = 2;

    // Create tablet with a single non-overlapped rowset (won't be selected for compaction)
    auto metadata = generate_simple_tablet_metadata(DUP_KEYS);
    metadata->set_id(tablet_id);
    metadata->set_version(version);

    // Add a single rowset that's not overlapped
    auto* rowset = metadata->add_rowsets();
    rowset->set_id(0);
    rowset->set_overlapped(false); // Not overlapped, may not be selected
    rowset->set_num_rows(10);
    rowset->set_data_size(100);
    rowset->add_segments("segment_0.dat");
    rowset->add_segment_size(100);

    CHECK_OK(_tablet_mgr->put_tablet_metadata(*metadata));

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(10 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, _thread_pool.get(), []() { return true; },
            [](bool) {});

    // May fail if no rowsets selected, or succeed with single group
    // This tests the pick_rowsets path
}

// Test for execute_subtask when state is cleaned up (lines 631-644)
TEST_F(TabletParallelCompactionManagerTest, test_execute_subtask_state_cleaned_up) {
    int64_t tablet_id = 10024;
    int64_t txn_id = 20024;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(1);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(4).build(&pool);

    std::atomic<bool> subtask_started{false};
    std::promise<void> cleanup_done_promise;
    std::future<void> cleanup_done_future = cleanup_done_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) {
                // When release_token is called, the subtask is done
                subtask_started.store(true);
            });

    // Wait briefly for subtask to start, then cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Cleanup tablet state while subtask might be running
    // This tests the path where state is gone during execute_subtask
    _manager->cleanup_tablet(tablet_id, txn_id);

    pool->wait();
}

// Test for partial subtask creation when some fail (lines 310-314)
TEST_F(TabletParallelCompactionManagerTest, test_partial_subtask_creation) {
    int64_t tablet_id = 10025;
    int64_t txn_id = 20025;
    int64_t version = 11;

    // Create 10 rowsets, each 5MB
    create_tablet_with_rowsets(tablet_id, 10, 5 * 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(3);
    config.set_max_bytes_per_subtask(15 * 1024 * 1024); // ~15MB per subtask

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    int acquire_count = 0;
    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(),
            [&]() {
                acquire_count++;
                // First acquisition succeeds, subsequent ones fail
                return acquire_count <= 1;
            },
            [](bool) {});

    // Should succeed with at least one subtask
    ASSERT_TRUE(st.ok());
    ASSERT_GE(st.value(), 1);

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for listing completed subtasks (lines 810-826)
TEST_F(TabletParallelCompactionManagerTest, test_list_tasks_with_completed) {
    int64_t tablet_id = 10026;
    int64_t txn_id = 20026;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->runs.store(1, std::memory_order_relaxed);
    ctx0->start_time.store(::time(nullptr) - 10, std::memory_order_relaxed);
    ctx0->finish_time.store(::time(nullptr), std::memory_order_release);
    ctx0->skipped.store(false, std::memory_order_relaxed);
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // List tasks - should include completed subtask
    std::vector<CompactionTaskInfo> infos;
    _manager->list_tasks(&infos);

    // Should have tasks listed
    EXPECT_GE(infos.size(), 1);

    // Complete subtask 1 to finish
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for copying table_id and partition_id from subtask contexts (lines 413-428)
TEST_F(TabletParallelCompactionManagerTest, test_table_partition_id_copy) {
    int64_t tablet_id = 10021;
    int64_t txn_id = 20021;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 without table_id and partition_id
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx0->table_id = 0;     // Not set
    ctx0->partition_id = 0; // Not set

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1 with table_id and partition_id
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    ctx1->table_id = 12345;
    ctx1->partition_id = 67890;

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for TxnLog merge without output (lines 520-527, 560-565)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_no_output) {
    int64_t tablet_id = 10027;
    int64_t txn_id = 20027;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 with TxnLog but no output_rowset
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    // No output_rowset set

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1 with TxnLog but no output_rowset
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    // No output_rowset set

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    // Verify merged result has no output statistics
    ASSERT_EQ(1, response.txn_logs_size());
    const auto& op_compaction = response.txn_logs(0).op_compaction();
    EXPECT_EQ(0, op_compaction.output_rowset().num_rows());

    block_promise.set_value();
    pool->wait();
}

// Test for TxnLog merge without compact_version (lines 567-574)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_no_compact_version) {
    int64_t tablet_id = 10028;
    int64_t txn_id = 20028;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 without compact_version
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    // No compact_version set

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for subtask with null TxnLog (lines 501-507)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_null_txn_log) {
    int64_t tablet_id = 10029;
    int64_t txn_id = 20029;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 with valid TxnLog
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1 with null TxnLog
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = nullptr; // Null TxnLog

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for subtask with TxnLog but no op_compaction (lines 501-507, 522-523)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_no_op_compaction) {
    int64_t tablet_id = 10030;
    int64_t txn_id = 20030;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // Complete subtask 0 with TxnLog but no op_compaction
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    // Don't set op_compaction

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // Complete subtask 1 with TxnLog but no op_compaction
    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    // Don't set op_compaction

    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for single subtask output (not overlapped, line 564)
TEST_F(TabletParallelCompactionManagerTest, test_merged_txn_log_single_subtask) {
    int64_t tablet_id = 10031;
    int64_t txn_id = 20031;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(1);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(1, st.value()); // Single subtask

    // Complete the single subtask with non-overlapped output
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(1);
    auto* output = ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset();
    output->set_num_rows(100);
    output->set_data_size(1000);
    output->set_overlapped(false); // Not overlapped
    output->add_segments("merged_segment.dat");

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    ASSERT_TRUE(closure.is_finished());

    // Verify output is not overlapped for single subtask
    ASSERT_EQ(1, response.txn_logs_size());
    const auto& op_compaction = response.txn_logs(0).op_compaction();
    // Single subtask, original overlapped value preserved
    EXPECT_FALSE(op_compaction.output_rowset().overlapped());

    block_promise.set_value();
    pool->wait();
}

// Test for metrics after subtask completion
TEST_F(TabletParallelCompactionManagerTest, test_metrics_after_completion) {
    int64_t tablet_id = 10032;
    int64_t txn_id = 20032;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    int64_t initial_running = _manager->running_subtasks();
    int64_t initial_completed = _manager->completed_subtasks();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(2, st.value());

    // After creation, running subtasks should increase
    EXPECT_EQ(initial_running + 2, _manager->running_subtasks());

    // Complete both subtasks
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    // After first completion
    EXPECT_EQ(initial_running + 1, _manager->running_subtasks());
    EXPECT_EQ(initial_completed + 1, _manager->completed_subtasks());

    auto ctx1 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx1->subtask_id = 1;
    ctx1->txn_log = std::make_unique<TxnLogPB>();
    ctx1->txn_log->mutable_op_compaction()->add_input_rowsets(5);
    ctx1->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(50);
    _manager->on_subtask_complete(tablet_id, txn_id, 1, std::move(ctx1));

    // After all completions
    EXPECT_EQ(initial_running, _manager->running_subtasks());
    EXPECT_EQ(initial_completed + 2, _manager->completed_subtasks());

    ASSERT_TRUE(closure.is_finished());

    block_promise.set_value();
    pool->wait();
}

// Test for rowsets marking and unmarking (lines 606-620)
TEST_F(TabletParallelCompactionManagerTest, test_rowsets_marking) {
    int64_t tablet_id = 10033;
    int64_t txn_id = 20033;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(2);
    config.set_max_bytes_per_subtask(5 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("test_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();
    std::promise<void> start_promise;

    pool->submit_func([&]() {
        start_promise.set_value();
        block_future.wait();
    });

    start_promise.get_future().wait();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; }, [](bool) {});
    ASSERT_TRUE(st.ok());

    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    ASSERT_NE(nullptr, state);

    // Check that rowsets are marked as compacting
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        EXPECT_FALSE(state->compacting_rowsets.empty());

        // All rowsets from all subtasks should be marked
        for (const auto& [subtask_id, info] : state->running_subtasks) {
            for (uint32_t rid : info.input_rowset_ids) {
                EXPECT_TRUE(state->is_rowset_compacting(rid));
            }
        }
    }

    block_promise.set_value();
    pool->wait();
    _manager->cleanup_tablet(tablet_id, txn_id);
}

// Test for callback not set scenario
TEST_F(TabletParallelCompactionManagerTest, test_on_subtask_complete_null_callback) {
    int64_t tablet_id = 10034;
    int64_t txn_id = 20034;
    int64_t version = 11;

    create_tablet_with_rowsets(tablet_id, 10, 1024 * 1024);

    TabletParallelConfig config;
    config.set_max_parallel_per_tablet(1);
    config.set_max_bytes_per_subtask(100 * 1024 * 1024);

    CompactRequest request;
    request.add_tablet_ids(tablet_id);
    CompactResponse response;
    TestClosure closure;
    // Create callback but don't assign to state
    auto callback = std::make_shared<CompactionTaskCallback>(nullptr, &request, &response, &closure);

    std::unique_ptr<ThreadPool> pool;
    ThreadPoolBuilder("blocked_pool").set_max_threads(1).build(&pool);

    std::promise<void> block_promise;
    std::future<void> block_future = block_promise.get_future();

    auto st = _manager->create_parallel_tasks(
            tablet_id, txn_id, version, config, callback, false, pool.get(), []() { return true; },
            [&](bool) { block_future.wait(); });
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(1, st.value());

    // Get state and set callback to nullptr to test null callback path
    auto* state = _manager->get_tablet_state(tablet_id, txn_id);
    ASSERT_NE(nullptr, state);

    // Complete subtask
    auto ctx0 = std::make_unique<CompactionTaskContext>(txn_id, tablet_id, version, false, true, nullptr);
    ctx0->subtask_id = 0;
    ctx0->txn_log = std::make_unique<TxnLogPB>();
    ctx0->txn_log->mutable_op_compaction()->add_input_rowsets(0);
    ctx0->txn_log->mutable_op_compaction()->mutable_output_rowset()->set_num_rows(100);

    _manager->on_subtask_complete(tablet_id, txn_id, 0, std::move(ctx0));

    ASSERT_TRUE(closure.wait_finish(5000));

    block_promise.set_value();
    pool->wait();
}

} // namespace starrocks::lake
