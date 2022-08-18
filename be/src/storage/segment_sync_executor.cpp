// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#include "storage/segment_sync_executor.h"

#include <fmt/format.h>

#include <memory>

#include "gen_cpp/data.pb.h"
#include "gen_cpp/doris_internal_service.pb.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "storage/delta_writer.h"
#include "util/brpc_stub_cache.h"
#include "util/raw_container.h"

namespace starrocks {

class SegmentSyncTask final : public Runnable {
public:
    SegmentSyncTask(SyncToken* sync_token, std::unique_ptr<SegmentPB> segment, bool eos)
            : _sync_token(sync_token), _segment(std::move(segment)), _eos(eos) {}

    ~SegmentSyncTask() override = default;

    void run() override { _sync_token->_sync_segment(std::move(_segment), _eos); }

private:
    SyncToken* _sync_token;
    std::unique_ptr<SegmentPB> _segment;
    bool _eos;
};

class SyncChannel {
public:
    SyncChannel(const DeltaWriterOptions* opt, const std::string& host, int32_t port)
            : _opt(opt), _host(host), _port(port) {
        _closure = new RefCountClosure<PTabletWriterAddSegmentResult>();
        _closure->ref();
    };
    ~SyncChannel() {
        if (_closure != nullptr && _closure->unref()) {
            delete _closure;
        }
    }

    Status sync_segment(SegmentPB* segment, bool eos, std::vector<std::unique_ptr<PTabletInfo>>* sync_tablet_infos);

    std::string debug_string() { return fmt::format("SyncChannnel [host={}, port={}]", _host, _port); }

private:
    Status _init();

    const DeltaWriterOptions* _opt;
    const std::string _host;
    const int32_t _port;

    RefCountClosure<PTabletWriterAddSegmentResult>* _closure = nullptr;
    doris::PBackendService_Stub* _stub = nullptr;

    bool _inited = false;
    Status _st = Status::OK();
};

Status SyncChannel::_init() {
    if (_inited) {
        return Status::OK();
    }
    _inited = true;

    _stub = ExecEnv::GetInstance()->brpc_stub_cache()->get_stub(_host, _port);
    if (_stub == nullptr) {
        auto msg = fmt::format("Connect {}:{} failed.", _host, _port);
        LOG(WARNING) << msg;
        return Status::InternalError(msg);
    }
    return Status::OK();
}

Status SyncChannel::sync_segment(SegmentPB* segment, bool eos,
                                 std::vector<std::unique_ptr<PTabletInfo>>* sync_tablet_infos) {
    RETURN_IF_ERROR(_st);

    // 1. init sync channel
    _st = _init();
    RETURN_IF_ERROR(_st);

    // 2. generate segment syc request
    PTabletWriterAddSegmentRequest request;
    request.set_allocated_id(const_cast<starrocks::PUniqueId*>(&_opt->load_id));
    request.set_tablet_id(_opt->tablet_id);
    request.set_eos(eos);
    request.set_txn_id(_opt->txn_id);
    request.set_index_id(_opt->index_id);
    if (segment != nullptr) {
        request.set_allocated_segment(segment);
    }

    _closure->ref();
    _closure->cntl.Reset();
    //_closure->cntl.set_timeout_ms();
    _stub->tablet_writer_add_segment(&_closure->cntl, &request, &_closure->result, _closure);

    request.release_id();
    if (segment != nullptr) {
        request.release_segment();
    }

    _closure->join();
    if (_closure->cntl.Failed()) {
        _st = Status::InternalError(_closure->cntl.ErrorText());
        LOG(WARNING) << "Failed to send rpc to [" << _host << ":" << _port << "] err=" << _st;
        return _st;
    }
    _st = _closure->result.status();
    if (!_st.ok()) {
        LOG(WARNING) << "Failed to send rpc to [" << _host << ":" << _port << "] err=" << _st;
        return _st;
    }

    VLOG(2) << "Sync tablet " << _opt->tablet_id << " segment id " << (segment == nullptr ? -1 : segment->segment_id())
            << " eos " << eos << " to [" << _host << ":" << _port << "] res " << _closure->result.DebugString();

    for (size_t i = 0; i < _closure->result.tablet_vec_size(); ++i) {
        sync_tablet_infos->emplace_back(std::make_unique<PTabletInfo>());
        sync_tablet_infos->back()->Swap(_closure->result.mutable_tablet_vec(i));
    }

    return _st;
}

SyncToken::SyncToken(std::unique_ptr<ThreadPoolToken> sync_pool_token, const DeltaWriterOptions* opt)
        : _sync_token(std::move(sync_pool_token)), _status(), _opt(opt) {
    // first replica is primary replica, skip it
    for (size_t i = 1; i < opt->replicas.size(); ++i) {
        _sync_channels.emplace_back(
                std::move(std::make_unique<SyncChannel>(opt, opt->replicas[i].host(), opt->replicas[i].port())));
    }
}

SyncToken::~SyncToken() {}

Status SyncToken::submit(std::unique_ptr<SegmentPB> segment, bool eos) {
    RETURN_IF_ERROR(status());
    // Does not acount the size of SegmentSyncTask into any memory tracker
    SCOPED_THREAD_LOCAL_MEM_SETTER(nullptr, false);
    auto task = std::make_shared<SegmentSyncTask>(this, std::move(segment), eos);
    return _sync_token->submit(std::move(task));
}

void SyncToken::cancel() {
    _sync_token->shutdown();
}

Status SyncToken::wait() {
    _sync_token->wait();
    std::lock_guard l(_status_lock);
    return _status;
}

void SyncToken::_sync_segment(std::unique_ptr<SegmentPB> segment, bool eos) {
    // If previous flush has failed, return directly
    if (!status().ok()) return;

    LOG(INFO) << "load_id " << _opt->load_id << " tablet_id " << _opt->tablet_id << " eos " << eos;
    if (segment) {
        auto fs = FileSystem::CreateUniqueFromString(segment->path()).value();
        LOG(INFO) << "sync segment " << segment->DebugString();
        auto rfile = std::move(fs->new_random_access_file(segment->path()).value());
        auto buff = segment->mutable_data();
        raw::stl_string_resize_uninitialized(buff, segment->data_size());
        auto st = rfile->read_fully(buff->data(), segment->data_size());
        if (!st.ok()) {
            LOG(WARNING) << "Failed to read segment " << segment->path() << " err " << st;
            return set_status(st);
        }
    }
    for (auto& channel : _sync_channels) {
        auto st = channel->sync_segment(segment.get(), eos, &_sync_tablet_infos);
        if (!st.ok()) {
            LOG(WARNING) << "Failed to sync segment " << channel->debug_string() << " err " << st;
            return set_status(st);
        }
    }
}

Status SegmentSyncExecutor::init(const std::vector<DataDir*>& data_dirs) {
    int data_dir_num = static_cast<int>(data_dirs.size());
    int min_threads = std::max<int>(1, config::flush_thread_num_per_store);
    int max_threads = data_dir_num * min_threads;
    return ThreadPoolBuilder("segment_sync") // mem table flush
            .set_min_threads(min_threads)
            .set_max_threads(max_threads)
            .build(&_sync_pool);
}

std::unique_ptr<SyncToken> SegmentSyncExecutor::create_sync_token(const DeltaWriterOptions* opt,
                                                                  ThreadPool::ExecutionMode execution_mode) {
    return std::make_unique<SyncToken>(_sync_pool->new_token(execution_mode), opt);
}

} // namespace starrocks
