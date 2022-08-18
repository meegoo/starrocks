// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/status.h"
#include "storage/olap_define.h"
#include "util/threadpool.h"

namespace brpc {
class Controller;
}

namespace google::protobuf {
class Closure;
}

namespace starrocks {

class DataDir;
class ExecEnv;
class PTabletWriterAddSegmentRequest;
class PTabletWriterAddSegmentResult;

namespace vectorized {
class DeltaWriter;
}

class SegmentFlushExecutor {
public:
    SegmentFlushExecutor() = default;
    ~SegmentFlushExecutor() = default;

    // init should be called after storage engine is opened,
    // because it needs path hash of each data dir.
    Status init(const std::vector<DataDir*>& data_dirs);

    Status submit(std::shared_ptr<starrocks::vectorized::DeltaWriter> delta_writer, brpc::Controller* cntl,
                  const PTabletWriterAddSegmentRequest* request, PTabletWriterAddSegmentResult* response,
                  google::protobuf::Closure* done);

private:
    std::unique_ptr<ThreadPool> _flush_pool;
};

} // namespace starrocks
