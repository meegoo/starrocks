# Lake Compaction Optimization - 与 TabletParallelCompactionManager 的具体结合方案

## 1. 现有框架分析

### 1.1 TabletParallelCompactionManager 做了什么

当前 `TabletParallelCompactionManager` 是一个 **FE-驱动的、请求级别的** 并行 compaction 管理器，它在一次 FE `CompactRequest` 的生命周期内完成以下工作：

```
FE CompactRequest (txn_id=100, tablets=[T1, T2, ...])
  │
  ├─ 对每个 tablet:
  │   ├─ pick_rowsets_for_compaction()     → 选择要 compact 的 rowsets
  │   ├─ _create_subtask_groups()          → 将 rowsets 分成并行组
  │   ├─ create_and_register_tablet_state()→ 创建 TabletParallelCompactionState
  │   └─ submit_subtasks_from_groups()     → 提交到线程池执行
  │       ├─ execute_subtask() 或 execute_subtask_segment_range()
  │       │   └─ TabletManager::compact()  → 实际执行 compaction
  │       └─ on_subtask_complete()         → subtask 完成回调
  │           └─ 所有 subtask 完成后:
  │               └─ get_merged_txn_log()  → 合并所有结果
  │                   └─ callback->finish_task() → 发 RPC 响应给 FE
  │
  └─ CompactionTaskCallback 收集所有 tablet 结果 → 发送 RPC 响应
```

**关键约束**:
- 生命周期绑定到一次 FE RPC 请求 (由 `txn_id` 唯一标识)
- State key = `(tablet_id, txn_id)` → 请求结束后清理
- 所有 subtask 必须完成后才合并和上报
- `CompactionTaskCallback` 耦合了 RPC 响应发送

### 1.2 哪些可以直接复用

`TabletParallelCompactionManager` 中**大量的静态工具方法**已经非常好地抽象了 rowset 分组和过滤逻辑：

| 方法 | 是否 static | 可直接复用 | 说明 |
|------|-----------|-----------|------|
| `_filter_compactable_rowsets()` | ✅ static | ✅ | 过滤大的 non-overlapped rowsets |
| `_calculate_rowset_stats()` | ✅ static | ✅ | 计算 rowset 统计信息 |
| `_calculate_grouping_config()` | ✅ static | ✅ | 计算最优分组配置 |
| `_group_rowsets_into_subtasks()` | ✅ static | ✅ | 按配置将 rowsets 分组 |
| `_filter_invalid_groups()` | ✅ static | ✅ | 过滤无效组 |
| `_is_large_rowset_for_split()` | ✅ static | ✅ | 判断大 rowset 是否需要拆分 |
| `_split_large_rowset()` | ✅ static | ✅ | 拆分大 rowset |
| `_group_small_rowsets()` | ✅ static | ✅ | 分组小 rowsets |
| `_is_group_valid_for_compaction()` | ✅ static | ✅ | 验证组是否有效 |
| `split_rowsets_into_groups()` | public | ✅ | 完整的分组流程 |
| `pick_rowsets_for_compaction()` | public | 需扩展 | 需要支持排除 compacting rowsets |
| `_create_subtask_groups()` | private | 需暴露 | 包含大 rowset 拆分逻辑 |
| `execute_subtask()` | private | 需解耦 | 耦合了 `(tablet_id, txn_id)` state |
| `get_merged_txn_log()` | public | 部分复用 | TxnLog 合并逻辑可复用 |
| `mark/unmark_rowsets_compacting()` | private | ✅ 机制复用 | compacting 标记逻辑 |

### 1.3 不能直接复用的原因

| 方法/机制 | 原因 |
|----------|------|
| `create_parallel_tasks()` 整体流程 | 耦合 FE txn_id 和 callback |
| `on_subtask_complete()` | 等待所有 subtask 完成后才 merge 上报 |
| `TabletParallelCompactionState` | 生命周期绑定 RPC 请求 |
| `CompactionTaskCallback` | 耦合 RPC 响应发送 |

---

## 2. 集成方案：双模式架构

### 2.1 核心思路

**不改动现有 `TabletParallelCompactionManager` 的接口和语义**，而是：

1. 将其中的 **rowset 选择和分组逻辑** 提升为可共享的工具
2. 新增 `AutonomousCompactionManager` 作为自主模式的调度器
3. 两个 manager 共享底层执行资源（线程池、Limiter、TabletManager）

```
                    CompactionScheduler (BE)
                   /          |          \
                  /           |           \
    FE compact()        FE PUBLISH_       BE 自主触发
    (现有模式)          AUTONOMOUS         (event-driven)
         |                   |                |
         v                   |                v
  TabletParallel             |        Autonomous
  CompactionMgr              |        CompactionMgr
  (不变)                     |        (新增)
         |                   |                |
         +------- 共享工具层 --------+        |
         |      (CompactionUtils)    |        |
         |                           |        |
         +---------- 共享执行层 ----------+---+
                TabletManager::compact()
                Thread Pool & Limiter
```

### 2.2 具体修改

#### Step 1: 提取共享工具到 `CompactionUtils`

从 `TabletParallelCompactionManager` 中提取**纯函数/工具方法**到一个独立的工具类。这些方法本身已经是 static 的，提取非常自然。

**新文件**: `be/src/storage/lake/compaction_utils.h`

```cpp
#pragma once

#include "storage/lake/rowset.h"
#include "storage/lake/compaction_policy.h"

namespace starrocks::lake {

class TabletManager;

// 复用自 TabletParallelCompactionManager 中已有的 static 方法
// 只是将它们暴露为公共工具供多个 manager 使用
class CompactionUtils {
public:
    struct RowsetStats {
        int64_t total_bytes = 0;
        int64_t total_segments = 0;
        bool has_delete_predicate = false;
    };

    struct GroupingConfig {
        int32_t num_subtasks = 1;
        int64_t target_bytes_per_subtask = 0;
        size_t target_rowsets_per_subtask = 2;
        bool skip_excess_data = false;
        int64_t max_segments_per_subtask = 0;
    };

    // 选择 rowsets，支持排除正在 compacting 的 rowsets
    static StatusOr<std::vector<RowsetPtr>> pick_rowsets(
        TabletManager* tablet_mgr, int64_t tablet_id, int64_t version,
        bool force_base_compaction,
        const std::unordered_set<uint32_t>& excluded_rowset_ids = {});

    // 过滤不需要 compact 的大 rowsets
    static std::vector<RowsetPtr> filter_compactable_rowsets(
        int64_t tablet_id, std::vector<RowsetPtr> all_rowsets);

    // 计算 rowset 统计信息
    static RowsetStats calculate_rowset_stats(const std::vector<RowsetPtr>& rowsets);

    // 计算分组配置
    static GroupingConfig calculate_grouping_config(
        int64_t tablet_id, const std::vector<RowsetPtr>& rowsets,
        const RowsetStats& stats, int32_t max_parallel, int64_t max_bytes);

    // 将 rowsets 分成并行组
    static std::vector<std::vector<RowsetPtr>> split_rowsets_into_groups(
        int64_t tablet_id, std::vector<RowsetPtr> all_rowsets,
        int32_t max_parallel, int64_t max_bytes, bool is_pk_table);

    // 限制总数据量，如果 rowsets 总大小超过 max_bytes，截断
    static std::vector<RowsetPtr> limit_rowsets_by_bytes(
        std::vector<RowsetPtr> rowsets, int64_t max_bytes);
};

} // namespace starrocks::lake
```

**实现要点**: 这些方法的实现直接复用 `TabletParallelCompactionManager` 中已有的 static 方法。`TabletParallelCompactionManager` 内部改为调用 `CompactionUtils` 的方法，避免代码重复。

**`pick_rowsets` 扩展**: 在调用 `CompactionPolicy::pick_rowsets()` 后，过滤掉 `excluded_rowset_ids` 中的 rowsets。

```cpp
StatusOr<std::vector<RowsetPtr>> CompactionUtils::pick_rowsets(
    TabletManager* tablet_mgr, int64_t tablet_id, int64_t version,
    bool force_base_compaction,
    const std::unordered_set<uint32_t>& excluded_rowset_ids) {

    ASSIGN_OR_RETURN(auto tablet, tablet_mgr->get_tablet(tablet_id, version));
    const auto& metadata = tablet.metadata();
    ASSIGN_OR_RETURN(auto policy, CompactionPolicy::create(tablet_mgr, metadata, force_base_compaction));
    ASSIGN_OR_RETURN(auto rowsets, policy->pick_rowsets());

    if (!excluded_rowset_ids.empty()) {
        std::erase_if(rowsets, [&](const RowsetPtr& r) {
            return excluded_rowset_ids.count(r->id()) > 0;
        });
    }
    return rowsets;
}
```

**`limit_rowsets_by_bytes`**: 支持 RFC 中的"单任务数据量限制"。

```cpp
std::vector<RowsetPtr> CompactionUtils::limit_rowsets_by_bytes(
    std::vector<RowsetPtr> rowsets, int64_t max_bytes) {
    if (max_bytes <= 0) return rowsets;

    std::vector<RowsetPtr> result;
    int64_t total = 0;
    for (auto& r : rowsets) {
        if (total + r->data_size() > max_bytes && !result.empty()) {
            break;
        }
        total += r->data_size();
        result.push_back(std::move(r));
    }
    return result;
}
```

#### Step 2: 让 `TabletParallelCompactionManager` 使用 `CompactionUtils`

修改 `TabletParallelCompactionManager`，将其内部的 static 方法调用改为通过 `CompactionUtils` 调用。这是一个纯重构，不改变行为。

```cpp
// Before:
auto compactable_rowsets = _filter_compactable_rowsets(tablet_id, std::move(all_rowsets));

// After:
auto compactable_rowsets = CompactionUtils::filter_compactable_rowsets(tablet_id, std::move(all_rowsets));
```

`TabletParallelCompactionManager` 的外部接口不变，FE-driven 模式完全不受影响。

#### Step 3: 新增 `AutonomousCompactionManager`

**新文件**: `be/src/storage/lake/autonomous_compaction_manager.h/cpp`

这是自主 compaction 模式的核心。与 `TabletParallelCompactionManager` 的关键区别：

| 维度 | TabletParallelCompactionManager | AutonomousCompactionManager |
|------|------|------|
| 触发方式 | FE RPC | 事件驱动 (publish_version/手动/自触发) |
| State key | `(tablet_id, txn_id)` | `tablet_id` (无 txn) |
| State 生命周期 | 一次 RPC 请求 | 持续存在，跨 round |
| Subtask 完成后 | 等所有完成 → merge → RPC 响应 | 立即保存到本地结果 |
| compacting_rowsets | 请求结束后清除 | 持续跟踪，跨 round |
| 并行度控制 | FE 配置 `max_parallel_per_tablet` | BE 配置 `lake_compaction_max_tasks_per_tablet` |

```cpp
// be/src/storage/lake/autonomous_compaction_manager.h

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "common/status.h"

namespace starrocks {
class ThreadPool;
}

namespace starrocks::lake {

class TabletManager;
class CompactionResultStore;

// 单 tablet 的自主 compaction 状态
// 与 TabletParallelCompactionState 的关键区别:
//   - 无 txn_id (BE 自主执行时没有事务)
//   - 无 callback (结果保存到本地，不发 RPC)
//   - 生命周期跨多轮 compaction
struct AutonomousTabletState {
    int64_t tablet_id = 0;

    // 正在被 compaction 的 rowset IDs (引用计数)
    // 机制与 TabletParallelCompactionState::compacting_rowsets 完全一致
    std::unordered_map<uint32_t, int> compacting_rowsets;

    // 当前运行中的任务数
    int running_tasks = 0;

    mutable std::mutex mutex;

    bool is_rowset_compacting(uint32_t rowset_id) const {
        auto it = compacting_rowsets.find(rowset_id);
        return it != compacting_rowsets.end() && it->second > 0;
    }

    std::unordered_set<uint32_t> get_compacting_rowset_ids() const {
        std::unordered_set<uint32_t> ids;
        for (const auto& [id, count] : compacting_rowsets) {
            if (count > 0) ids.insert(id);
        }
        return ids;
    }
};

class AutonomousCompactionManager {
public:
    explicit AutonomousCompactionManager(TabletManager* tablet_mgr, ThreadPool* thread_pool);
    ~AutonomousCompactionManager();

    // ============ 事件触发接口 ============

    // 将 tablet 加入调度队列进行 compaction 评估
    // 调用时机:
    //   1. publish_version 完成后 (transactions.cpp)
    //   2. PUBLISH_AUTONOMOUS 请求中的 tablet_ids
    //   3. 手动 compact 命令
    //   4. 单任务数据量达到阈值后自触发
    void schedule_tablet(int64_t tablet_id);

    // 批量调度
    void schedule_tablets(const std::vector<int64_t>& tablet_ids);

    // ============ PUBLISH_AUTONOMOUS 处理 ============

    // 当 FE 发送 PUBLISH_AUTONOMOUS 时调用
    // 对每个 tablet:
    //   - 有本地结果: 合并 → 构建 TxnLog → 写 S3 → publish
    //   - 无本地结果: 不写 TxnLog → force_publish 更新版本号
    struct PublishResult {
        std::vector<int64_t> tablets_with_compaction;
        std::vector<int64_t> tablets_without_compaction;
    };

    StatusOr<PublishResult> handle_publish_autonomous(
        int64_t txn_id, int64_t visible_version, int64_t new_version,
        const std::vector<int64_t>& tablet_ids);

    // ============ 生命周期 ============
    void start();
    void stop();

    // ============ 监控 ============
    int64_t pending_tablets() const { return _pending_count.load(); }
    int64_t running_tasks() const { return _running_tasks.load(); }

private:
    // 优先级队列条目
    struct ScheduleEntry {
        int64_t tablet_id;
        double score;
        int64_t schedule_time;

        bool operator<(const ScheduleEntry& o) const {
            return score < o.score; // max-heap
        }
    };

    // 调度线程主函数
    void schedule_loop();

    // 对单个 tablet 执行一轮 compaction
    // 这是自主模式的核心执行路径
    void execute_autonomous_compaction(int64_t tablet_id);

    // 获取或创建 tablet state
    std::shared_ptr<AutonomousTabletState> get_or_create_state(int64_t tablet_id);

    // 计算 compaction score
    StatusOr<double> compute_score(int64_t tablet_id);

    // 构建 TxnLog 从多个 CompactionResult
    // 复用了 TabletParallelCompactionManager::get_merged_txn_log 中的合并思路
    Status build_txn_log_from_results(int64_t tablet_id, int64_t txn_id,
                                       TxnLogPB* txn_log);

    TabletManager* _tablet_mgr;
    ThreadPool* _thread_pool; // 与 CompactionScheduler 共享

    // 调度队列
    std::priority_queue<ScheduleEntry> _schedule_queue;
    std::unordered_set<int64_t> _in_queue; // 去重
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;

    // Per-tablet state (持续存在)
    std::unordered_map<int64_t, std::shared_ptr<AutonomousTabletState>> _tablet_states;
    std::mutex _states_mutex;

    // 本地结果存储
    std::unique_ptr<CompactionResultStore> _result_store;

    // 调度线程
    std::unique_ptr<std::thread> _schedule_thread;

    // 全局并发控制
    std::atomic<int64_t> _running_tasks{0};
    std::atomic<int64_t> _pending_count{0};
    std::atomic<bool> _stopped{false};
};

} // namespace starrocks::lake
```

#### Step 4: `execute_autonomous_compaction` 的实现

这是核心执行路径。它复用 `CompactionUtils` 进行 rowset 选择和分组，但使用不同的结果处理路径。

```cpp
void AutonomousCompactionManager::execute_autonomous_compaction(int64_t tablet_id) {
    auto state = get_or_create_state(tablet_id);
    std::unordered_set<uint32_t> excluded;
    {
        std::lock_guard lock(state->mutex);
        excluded = state->get_compacting_rowset_ids();
    }

    // 1. 获取 tablet 当前版本
    auto metadata_or = _tablet_mgr->get_latest_metadata(tablet_id);
    if (!metadata_or.ok()) return;
    int64_t version = metadata_or.value()->version();

    // 2. 选择 rowsets (排除正在 compacting 的)
    //    ★ 复用 CompactionUtils::pick_rowsets
    auto rowsets_or = CompactionUtils::pick_rowsets(
        _tablet_mgr, tablet_id, version, false, excluded);
    if (!rowsets_or.ok() || rowsets_or.value().empty()) return;

    auto rowsets = std::move(rowsets_or.value());

    // 3. 限制单任务数据量
    //    ★ 复用 CompactionUtils::limit_rowsets_by_bytes
    int64_t max_bytes = config::lake_compaction_max_bytes_per_task;
    rowsets = CompactionUtils::limit_rowsets_by_bytes(std::move(rowsets), max_bytes);

    // 4. 标记 rowsets 为 compacting
    std::vector<uint32_t> rowset_ids;
    int64_t total_bytes = 0;
    for (const auto& r : rowsets) {
        rowset_ids.push_back(r->id());
        total_bytes += r->data_size();
    }
    {
        std::lock_guard lock(state->mutex);
        for (uint32_t rid : rowset_ids) {
            state->compacting_rowsets[rid]++;
        }
        state->running_tasks++;
    }

    // 5. 执行 compaction
    //    ★ 复用 TabletManager::compact() — 与 TabletParallelCompactionManager
    //      的 execute_subtask() 调用的是同一个底层方法
    auto context = std::make_unique<CompactionTaskContext>(
        0 /* 无 txn_id */, tablet_id, version,
        false, true /* skip_write_txnlog */, nullptr);

    auto task_or = _tablet_mgr->compact(context.get(), std::move(rowsets));
    if (task_or.ok()) {
        auto exec_st = task_or.value()->execute(
            []() { return Status::OK(); }, nullptr);
        if (!exec_st.ok()) {
            context->status = exec_st;
        }
    } else {
        context->status = task_or.status();
    }

    // 6. 保存结果到本地
    if (context->status.ok() && context->txn_log != nullptr) {
        CompactionResultPB result;
        result.set_tablet_id(tablet_id);
        result.set_base_version(version);
        for (uint32_t rid : rowset_ids) {
            result.add_input_rowset_ids(rid);
        }
        if (context->txn_log->has_op_compaction()) {
            *result.mutable_output_rowset() =
                context->txn_log->op_compaction().output_rowset();
        }
        result.set_finish_time_ms(butil::gettimeofday_ms());
        result.set_input_bytes(total_bytes);
        _result_store->save(result);
    }

    // 7. 取消标记
    {
        std::lock_guard lock(state->mutex);
        for (uint32_t rid : rowset_ids) {
            auto it = state->compacting_rowsets.find(rid);
            if (it != state->compacting_rowsets.end()) {
                if (--it->second <= 0) state->compacting_rowsets.erase(it);
            }
        }
        state->running_tasks--;
    }

    _running_tasks--;

    // 8. 自触发: 如果处理的数据量接近阈值，立即触发下一轮
    if (total_bytes >= max_bytes * 0.8) {
        schedule_tablet(tablet_id);
    }
}
```

**关键点**: 步骤 5 调用的 `TabletManager::compact()` 与 `TabletParallelCompactionManager::execute_subtask()` 中调用的是**完全相同的方法**。差异仅在于上下文管理和结果处理。

#### Step 5: 单 Tablet 内并行

当单个 tablet 数据量很大时，也需要在一个 tablet 内并行。这时可以复用 `TabletParallelCompactionManager` 的分组逻辑：

```cpp
void AutonomousCompactionManager::execute_autonomous_compaction(int64_t tablet_id) {
    // ... 步骤 1-3 同上 ...

    auto stats = CompactionUtils::calculate_rowset_stats(rowsets);
    int32_t max_tasks_per_tablet = config::lake_compaction_max_tasks_per_tablet;

    // 判断是否需要并行
    if (stats.total_bytes > max_bytes && max_tasks_per_tablet > 1 &&
        !stats.has_delete_predicate && stats.total_segments >= 4) {

        // ★ 复用 CompactionUtils 的分组逻辑
        auto groups = CompactionUtils::split_rowsets_into_groups(
            tablet_id, std::move(rowsets), max_tasks_per_tablet,
            max_bytes, is_pk_table);

        if (groups.size() > 1) {
            // 并行执行多个组
            for (auto& group : groups) {
                submit_autonomous_subtask(tablet_id, state, std::move(group), version);
            }
            return;
        }
    }

    // 单任务执行 (同上)
    // ...
}
```

#### Step 6: PUBLISH_AUTONOMOUS 处理

在 BE 的 `CompactionScheduler` 中新增对 `PUBLISH_AUTONOMOUS` 类型的处理，委托给 `AutonomousCompactionManager`。

```cpp
// compaction_scheduler.cpp

void CompactionScheduler::compact(...) {
    // 新增: PUBLISH_AUTONOMOUS 类型
    if (request->has_compact_type() &&
        request->compact_type() == CompactRequest::PUBLISH_AUTONOMOUS) {
        process_publish_autonomous(request, response, done);
        return;
    }
    // ... 现有逻辑 ...
}

void CompactionScheduler::process_publish_autonomous(
    const CompactRequest* request, CompactResponse* response,
    ::google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);

    std::vector<int64_t> tablet_ids(
        request->tablet_ids().begin(), request->tablet_ids().end());

    auto result = _autonomous_mgr->handle_publish_autonomous(
        request->txn_id(), request->version(), /* new_version from request */,
        tablet_ids);

    if (result.ok()) {
        // 填充响应
        for (int64_t tid : result.value().tablets_with_compaction) {
            response->add_tablets_with_compaction(tid);
        }
        for (int64_t tid : result.value().tablets_without_compaction) {
            response->add_tablets_without_compaction(tid);
        }
    }
    result.status().to_protobuf(response->mutable_status());

    // ★ 触发所有 tablet 继续 compaction
    _autonomous_mgr->schedule_tablets(tablet_ids);
}
```

`handle_publish_autonomous` 的 TxnLog 构建逻辑复用了 `get_merged_txn_log()` 中的合并思路（union input_rowsets, concat output_rowsets），但不需要 SST compaction 和 LCRM 合并（因为每个结果已经是完整的 compaction output）。

#### Step 7: publish_version 后触发调度

```cpp
// transactions.cpp
Status publish_version(...) {
    // ... 现有逻辑 ...

    // publish 成功后，触发自主 compaction 调度
    if (config::lake_enable_autonomous_compaction) {
        if (auto* scheduler = StorageEngine::instance()->lake_compaction_scheduler()) {
            if (auto* mgr = scheduler->autonomous_compaction_mgr()) {
                mgr->schedule_tablet(tablet_id);
            }
        }
    }

    return Status::OK();
}
```

#### Step 8: CompactionResultStore (本地结果存储)

```cpp
// be/src/storage/lake/compaction_result_store.h

class CompactionResultStore {
public:
    explicit CompactionResultStore(const std::string& root_path);

    Status save(const CompactionResultPB& result);

    // 获取某 tablet 的结果，按 base_version 过滤
    std::vector<CompactionResultPB> get_results(
        int64_t tablet_id, int64_t max_base_version);

    // 清理已 publish 的结果
    void cleanup(int64_t tablet_id, int64_t published_version);

    // 启动时恢复
    Status recover();

private:
    std::string _root_path;
    // 内存索引: tablet_id → results
    std::unordered_map<int64_t, std::vector<CompactionResultPB>> _index;
    std::mutex _mutex;
};
```

---

## 3. FE 侧修改

### 3.1 新增 PUBLISH_AUTONOMOUS 调度

FE `CompactionScheduler` 新增一个与现有 `startCompaction()` 平行的方法：

```java
// CompactionScheduler.java

private void scheduleNewCompaction() {
    // ... 现有的 Phase 1: 处理完成的 jobs ...

    if (Config.lake_enable_autonomous_compaction) {
        // ★ 新增: 触发 autonomous publish
        scheduleAutonomousPublish();
        return; // autonomous 模式不走下面的 startCompaction 路径
    }

    // ... 现有的 Phase 2: startCompaction ...
}

private void scheduleAutonomousPublish() {
    List<PartitionStatisticsSnapshot> candidates =
        compactionManager.choosePartitionsToCompact(
            runningCompactions.keySet(), disabledIds);

    for (PartitionStatisticsSnapshot snapshot : candidates) {
        if (runningCompactions.containsKey(snapshot.getPartition())) {
            continue;
        }
        if (!shouldTriggerPublish(snapshot)) {
            continue;
        }
        CompactionJob job = startAutonomousPublish(snapshot);
        if (job != null) {
            runningCompactions.put(snapshot.getPartition(), job);
        }
    }
}
```

`startAutonomousPublish()` 与现有 `startCompaction()` 结构类似，但发送的是 `PUBLISH_AUTONOMOUS` 类型的请求，且始终设置 `allowPartialSuccess = true`。

### 3.2 收集结果和 commit

当 `PUBLISH_AUTONOMOUS` 请求返回后，FE 的处理流程与现有 partial success 完全一致：

```
CompactResponse 返回:
  tablets_with_compaction: [T1, T2, T3]    → 有 TxnLog
  tablets_without_compaction: [T4, T5]      → 无 TxnLog (force_publish)

FE 行为:
  1. commitCompaction(forceCommit=true)
     → CompactionTxnCommitAttachment(forceCommit=true)
  2. PublishVersionDaemon 发送 publishVersion:
     → T1,T2,T3: 有 TxnLog → 应用 compaction → 更新版本
     → T4,T5: 无 TxnLog + force_publish → 仅更新版本号
  3. 所有 tablet 版本统一更新 (v10 → v11)
```

这完全复用了现有的 `force_publish` 和 `ignore_txn_log` 机制，无需修改 BE 的 `publish_version` 逻辑。

---

## 4. 两种模式的对比

```
═══════════════════════════════════════════════════════════════════

  FE-Driven Mode (现有, 不变)

  FE: 选 partition → 创建 txn → 发 CompactRequest
                                        │
  BE: TabletParallelCompactionManager   │
      ├─ pick_rowsets_for_compaction()  ←┘
      ├─ _create_subtask_groups()         ← CompactionUtils
      ├─ submit_subtasks_from_groups()
      │   └─ execute_subtask()
      │       └─ TabletManager::compact() ← 共享
      └─ on_subtask_complete()
          └─ get_merged_txn_log()
              └─ callback->finish_task()  → RPC 响应给 FE
                                        │
  FE: commitCompaction → publishVersion ←┘

═══════════════════════════════════════════════════════════════════

  Autonomous Mode (新增)

  BE: AutonomousCompactionManager
      ├─ schedule_tablet() ←─────── publish_version 完成 / 自触发
      ├─ compute_score()
      ├─ pick_rowsets(excluded)          ← CompactionUtils
      ├─ limit_rowsets_by_bytes()        ← CompactionUtils
      ├─ split_rowsets_into_groups()     ← CompactionUtils
      │   └─ TabletManager::compact()    ← 共享
      └─ save CompactionResultPB         → 本地存储
                                        │
  FE: 定期扫描 → shouldTriggerPublish() │
      → 发 PUBLISH_AUTONOMOUS           │
                                        │
  BE: handle_publish_autonomous()       ←┘
      ├─ 有结果的 tablet: 构建 TxnLog → 写 S3
      └─ 无结果的 tablet: 不写 TxnLog
                                        │
  FE: commitCompaction(forceCommit)     ←┘
      → publishVersion (force_publish)
      → 所有 tablet 版本统一更新

═══════════════════════════════════════════════════════════════════
```

---

## 5. 共享组件关系图

```
┌─────────────────────────────────┐
│        CompactionUtils          │   ← 从 TabletParallelCompactionManager
│  (static 工具方法)               │      的 static 方法提取
│  ● pick_rowsets (with exclusion)│
│  ● filter_compactable_rowsets   │
│  ● calculate_rowset_stats       │
│  ● calculate_grouping_config    │
│  ● split_rowsets_into_groups    │
│  ● limit_rowsets_by_bytes       │
└──────────┬──────────┬───────────┘
           │          │
     ┌─────┘          └──────┐
     │                       │
     ▼                       ▼
┌─────────────────┐  ┌──────────────────────┐
│ TabletParallel  │  │ Autonomous           │
│ CompactionMgr   │  │ CompactionMgr        │
│ (不变)          │  │ (新增)               │
│                 │  │                      │
│ ● FE-driven    │  │ ● Event-driven       │
│ ● Per-request  │  │ ● Persistent state   │
│ ● RPC callback │  │ ● Local result store │
│ ● SST merge    │  │ ● PUBLISH_AUTONOMOUS │
│ ● LCRM merge   │  │                      │
└────────┬────────┘  └──────────┬───────────┘
         │                      │
         └──────────┬───────────┘
                    │
                    ▼
         ┌───────────────────┐
         │ TabletManager     │
         │ ::compact()       │   ← 实际执行 compaction 的底层方法
         │                   │      两种模式完全共享
         ├───────────────────┤
         │ CompactionPolicy  │   ← rowset 选择策略
         │ ::pick_rowsets()  │
         ├───────────────────┤
         │ ThreadPool        │   ← 工作线程池
         │ Limiter           │   ← 并发/内存控制
         └───────────────────┘
```

---

## 6. 文件修改清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `be/src/storage/lake/compaction_utils.h` | 从 TabletParallelCompactionManager 提取的共享工具 |
| `be/src/storage/lake/compaction_utils.cpp` | 实现 |
| `be/src/storage/lake/autonomous_compaction_manager.h` | 自主 compaction 调度器 |
| `be/src/storage/lake/autonomous_compaction_manager.cpp` | 实现 |
| `be/src/storage/lake/compaction_result_store.h` | 本地结果存储 |
| `be/src/storage/lake/compaction_result_store.cpp` | 实现 |
| `be/test/storage/lake/compaction_utils_test.cpp` | 工具类单测 |
| `be/test/storage/lake/autonomous_compaction_manager_test.cpp` | 自主模式单测 |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `be/src/storage/lake/tablet_parallel_compaction_manager.h/cpp` | 内部改为调用 CompactionUtils (纯重构，不改行为) |
| `be/src/storage/lake/compaction_scheduler.h/cpp` | 新增 `_autonomous_mgr` 成员和 `process_publish_autonomous()` |
| `be/src/storage/lake/transactions.cpp` | publish_version 后触发 `schedule_tablet()` |
| `be/src/common/config.h` | 新增 autonomous 相关配置 |
| `gensrc/proto/lake_types.proto` | 新增 `CompactionResultPB` |
| `gensrc/proto/lake_service.proto` | CompactRequest 新增 `compact_type`; CompactResponse 新增字段 |
| FE `CompactionScheduler.java` | 新增 `scheduleAutonomousPublish()` |
| FE `Config.java` | 新增 autonomous 配置参数 |
| FE `PartitionStatistics.java` | 新增 `lastPublishTime` |

---

## 7. 渐进式实施路径

### Phase 1: 提取 CompactionUtils (纯重构)

将 `TabletParallelCompactionManager` 的 static 方法提取到 `CompactionUtils`。这是一个零风险的重构步骤，不改变任何行为，可以先合入主线。

### Phase 2: AutonomousCompactionManager 基础框架

实现调度循环、AutonomousTabletState、CompactionResultStore。此阶段不接入 FE，只在 BE 内部运行，通过配置开关控制。

### Phase 3: PUBLISH_AUTONOMOUS 处理

实现 BE 侧的 `handle_publish_autonomous()` 和 FE 侧的 `scheduleAutonomousPublish()`。这是打通完整流程的关键步骤。

### Phase 4: 端到端测试和调优

集成测试、异常恢复测试、性能对比测试。
