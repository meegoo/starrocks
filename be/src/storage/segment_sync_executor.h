// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/status.h"
#include "gen_cpp/internal_service.pb.h"
#include "storage/olap_define.h"
#include "util/ref_count_closure.h"
#include "util/spinlock.h"
#include "util/threadpool.h"

namespace starrocks {

class DataDir;
class ExecEnv;
class SegmentPB;
class SyncChannel;
class PTabletInfo;

namespace vectorized {
class DeltaWriterOptions;
}

using DeltaWriterOptions = starrocks::vectorized::DeltaWriterOptions;

class SyncToken {
public:
    SyncToken(std::unique_ptr<ThreadPoolToken> sync_pool_token, const DeltaWriterOptions* opt);
    ~SyncToken();

    Status submit(std::unique_ptr<SegmentPB> segment, bool eos);

    // error has happpens, so we cancel this token
    // And remove all tasks in the queue.
    void cancel();

    // wait all tasks in token to be completed.
    Status wait();

    Status status() const {
        std::lock_guard l(_status_lock);
        return _status;
    }

    void set_status(const Status& status) {
        if (status.ok()) return;
        std::lock_guard l(_status_lock);
        if (_status.ok()) _status = status;
    }

    const std::vector<std::unique_ptr<PTabletInfo>>* tablet_infos() const { return &_sync_tablet_infos; }

private:
    friend class SegmentSyncTask;

    void _sync_segment(std::unique_ptr<SegmentPB> segment, bool eos);

    std::unique_ptr<ThreadPoolToken> _sync_token;

    mutable SpinLock _status_lock;
    // Records the current flush status of the tablet.
    // Note: Once its value is set to Failed, it cannot return to OK.
    Status _status;

    const DeltaWriterOptions* _opt;

    std::vector<std::unique_ptr<SyncChannel>> _sync_channels;

    std::vector<std::unique_ptr<PTabletInfo>> _sync_tablet_infos;
};

class SegmentSyncExecutor {
public:
    SegmentSyncExecutor() = default;
    ~SegmentSyncExecutor() = default;

    // init should be called after storage engine is opened,
    // because it needs path hash of each data dir.
    Status init(const std::vector<DataDir*>& data_dirs);

    // NOTE: we use SERIAL mode here to ensure all segment from one tablet are synced in order.
    std::unique_ptr<SyncToken> create_sync_token(
            const DeltaWriterOptions* opt,
            ThreadPool::ExecutionMode execution_mode = ThreadPool::ExecutionMode::SERIAL);

private:
    std::unique_ptr<ThreadPool> _sync_pool;
};

} // namespace starrocks
