# Lake Compaction Optimization - 基于 Parallel Compaction 框架的具体修改方案

## 1. 总体概述

本方案基于 RFC 文档的设计，在现有 `TabletParallelCompactionManager` 框架上进行扩展，实现两个核心目标：
1. **解决慢 Tablet 阻塞问题**：每个 Tablet 独立执行 compaction，快 Tablet 不等慢 Tablet
2. **解决单 Tablet 串行处理问题**：单 Tablet 可并行执行多个 compaction 任务（已有框架支持）

### 1.1 与现有框架的关系

| 模块 | 现有实现 | 本方案扩展 |
|------|---------|-----------|
| `TabletParallelCompactionManager` | 单 tablet 内并行 subtask，受 FE CompactRequest 驱动 | 复用 subtask 分组/合并逻辑，但改为 BE 自主触发 |
| `CompactionScheduler` (BE) | 处理 FE RPC，管理任务队列 | 新增 `PUBLISH_AUTONOMOUS` 类型处理 |
| `CompactionPolicy` | 选择 rowset 进行 compaction | 扩展：支持排除正在 compacting 的 rowset、支持数据量限制 |
| `CompactionScheduler` (FE) | 选 partition → 创建 CompactionJob → 等待完成 | 新增定期扫描 → 触发 batch publish 的路径 |
| `CompactionJob` (FE) | 包含一个 partition 所有 tablet | 新增 `PUBLISH_AUTONOMOUS` 类型，支持 partial publish |

---

## 2. BE 侧修改

### 2.1 新增 `LakeCompactionManager` 类

**文件**: `be/src/storage/lake/lake_compaction_manager.h/cpp`

这是 BE 侧自主 compaction 的核心调度器，采用事件驱动模型。

```
┌─────────────────────────────────────────────────────────────────────┐
│                   LakeCompactionManager                              │
│                                                                      │
│  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────┐   │
│  │ Priority Queue    │    │ CompactingRowsets │    │ Token Limiter │   │
│  │ (score-sorted)    │    │ (per-tablet map)  │    │ (concurrency) │   │
│  └────────┬─────────┘    └──────────────────┘    └──────┬───────┘   │
│           │                                              │           │
│           ▼                                              │           │
│  ┌──────────────────┐                                    │           │
│  │ Worker Threads    │◄──────────────────────────────────┘           │
│  │ (execute tasks)   │                                               │
│  └────────┬─────────┘                                               │
│           │                                                          │
│           ▼                                                          │
│  ┌──────────────────┐                                               │
│  │ CompactionResult  │                                               │
│  │ Manager           │                                               │
│  └──────────────────┘                                               │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.1.1 类定义

```cpp
// be/src/storage/lake/lake_compaction_manager.h

class LakeCompactionManager {
public:
    static LakeCompactionManager* instance();
    
    explicit LakeCompactionManager(TabletManager* tablet_mgr);
    ~LakeCompactionManager();

    // ====== 事件触发接口 ======
    
    // 异步触发 tablet compaction 评估（非阻塞）
    // 触发时机：
    //   1. publish_version 完成后
    //   2. 手动 compact 命令
    //   3. 单任务数据量达到阈值后自触发
    void update_tablet_async(int64_t tablet_id);
    
    // 批量触发（用于 PUBLISH_AUTONOMOUS 请求中的 tablet_ids）
    void update_tablets_async(const std::vector<int64_t>& tablet_ids);

    // ====== Compaction 结果管理 ======
    
    // 获取某个 tablet 的本地 compaction 结果
    // 过滤条件：base_version <= visible_version
    std::vector<CompactionResultPB> get_results_for_tablet(
        int64_t tablet_id, int64_t visible_version);
    
    // 保存 compaction 结果
    Status save_compaction_result(const CompactionResultPB& result);
    
    // 清理已 publish 的结果
    void cleanup_published_results(int64_t tablet_id, int64_t published_version);

    // ====== Compacting Rowset 跟踪 ======
    
    // 获取某 tablet 正在 compacting 的 rowset IDs
    std::unordered_set<uint32_t> get_compacting_rowsets(int64_t tablet_id);
    
    // ====== 生命周期 ======
    void start();
    void stop();

    // ====== 监控 ======
    int64_t pending_tablets() const;
    int64_t running_tasks() const;

private:
    struct TabletCompactionState {
        int64_t tablet_id = 0;
        double score = 0.0;
        int running_tasks = 0;
        std::unordered_set<uint32_t> compacting_rowsets;
        mutable std::mutex mutex;
    };

    // 优先级队列条目
    struct PriorityEntry {
        int64_t tablet_id;
        double score;
        bool operator<(const PriorityEntry& other) const {
            return score < other.score; // max-heap
        }
    };

    // 计算 tablet compaction score
    StatusOr<double> compute_compaction_score(int64_t tablet_id);
    
    // 调度线程主循环
    void schedule_loop();
    
    // 执行单个 tablet 的 compaction
    void execute_compaction(int64_t tablet_id);
    
    // 任务完成回调
    void on_task_complete(int64_t tablet_id, const CompactionResultPB& result,
                          int64_t processed_bytes);

    TabletManager* _tablet_mgr;
    
    // 优先级队列
    std::priority_queue<PriorityEntry> _priority_queue;
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    
    // Per-tablet state
    std::unordered_map<int64_t, std::shared_ptr<TabletCompactionState>> _tablet_states;
    std::mutex _states_mutex;
    
    // 结果管理
    std::unique_ptr<CompactionResultManager> _result_mgr;
    
    // 工作线程池
    std::unique_ptr<ThreadPool> _worker_pool;
    
    // 调度线程
    std::unique_ptr<std::thread> _schedule_thread;
    
    // 全局并发控制
    std::atomic<int64_t> _running_tasks{0};
    std::atomic<bool> _stopped{false};
};
```

#### 2.1.2 关键方法实现思路

**`update_tablet_async()`** - 事件驱动入口

```
1. 计算 tablet 的 compaction score
2. 如果 score > lake_compaction_score_threshold:
   a. 将 tablet 加入优先级队列
3. 通知调度线程有新工作
```

**`schedule_loop()`** - 调度线程

```
while (!_stopped):
    1. 等待队列非空（或超时）
    2. 从优先级队列取出最高 score 的 tablet
    3. 检查全局并发限制 (running_tasks < lake_compaction_max_concurrent_tasks)
    4. 检查该 tablet 并发限制 (tablet.running_tasks < lake_compaction_max_tasks_per_tablet)
    5. 向 worker_pool 提交 execute_compaction(tablet_id)
```

**`execute_compaction()`** - 执行单个 compaction 任务

```
1. 获取 tablet metadata
2. 调用 CompactionPolicy::pick_rowsets()，传入排除集合(compacting_rowsets)
3. 如果启用数据量限制，裁剪 rowsets 使总大小不超过 lake_compaction_max_bytes_per_task
4. 标记选中的 rowsets 为 compacting
5. 执行 compaction (复用现有 CompactionTask)
6. 保存 CompactionResultPB 到本地
7. 回调 on_task_complete()
```

**`on_task_complete()`** - 任务完成处理

```
1. 取消标记 compacting rowsets
2. running_tasks--
3. 如果 processed_bytes >= lake_compaction_max_bytes_per_task * 0.8:
   a. 自动触发下一轮: update_tablet_async(tablet_id)
4. 更新 metrics
```

### 2.2 新增 `CompactionResultManager` 类

**文件**: `be/src/storage/lake/compaction_result_manager.h/cpp`

负责 compaction 结果的本地持久化。

```cpp
// be/src/storage/lake/compaction_result_manager.h

class CompactionResultManager {
public:
    explicit CompactionResultManager(TabletManager* tablet_mgr);
    
    // 保存结果到本地存储
    Status save_result(const CompactionResultPB& result);
    
    // 加载指定 tablet 的所有结果
    std::vector<CompactionResultPB> load_results(int64_t tablet_id);
    
    // 加载结果并按 base_version 过滤
    std::vector<CompactionResultPB> load_results(
        int64_t tablet_id, int64_t max_base_version);
    
    // 删除指定 tablet 已 publish 的结果
    void cleanup_results(int64_t tablet_id, int64_t published_version);
    
    // 启动时扫描并加载所有本地结果
    Status recover_on_startup();

private:
    // 结果文件路径: {storage_root}/lake/compaction_results/
    //   tablet_{id}_result_{timestamp}_{random}.pb
    std::string get_result_dir();
    std::string generate_result_path(int64_t tablet_id);
    
    TabletManager* _tablet_mgr;
    
    // 内存缓存: tablet_id -> results
    std::unordered_map<int64_t, std::vector<CompactionResultPB>> _results_cache;
    mutable std::mutex _cache_mutex;
};
```

**存储路径**: `{storage_root}/lake/compaction_results/tablet_{id}_result_{timestamp}_{random}.pb`

**恢复机制**: BE 启动时，`CompactionResultManager::recover_on_startup()` 扫描目录加载所有 result 文件。不需要 BE 主动全量扫描 tablet——FE 的 `PUBLISH_AUTONOMOUS` 请求会带上 tablet_ids，自然触发 compaction 调度。

### 2.3 Protobuf 修改

**文件**: `gensrc/proto/lake_types.proto`

```protobuf
// 新增: Compaction 结果消息
message CompactionResultPB {
    int64 tablet_id = 1;
    int64 base_version = 2;       // compaction 基于的版本
    repeated uint32 input_rowset_ids = 3;
    RowsetMetadataPB output_rowset = 4;
    int64 finish_time_ms = 5;
    int64 input_bytes = 6;        // 输入数据总大小
    int64 output_bytes = 7;       // 输出数据大小
}
```

**文件**: `gensrc/proto/lake_service.proto`

```protobuf
// 修改: CompactRequest 新增类型
message CompactRequest {
    // ... 现有字段 ...
    
    // 新增: compact 类型
    enum CompactType {
        NORMAL = 0;              // 现有模式
        PUBLISH_AUTONOMOUS = 1;   // 自主 compaction publish 模式
    }
    optional CompactType compact_type = 20 [default = NORMAL];
}

// 修改: CompactResponse 新增字段
message CompactResponse {
    // ... 现有字段 ...
    
    // PUBLISH_AUTONOMOUS 模式下返回
    repeated int64 tablets_with_compaction = 20;  // 有 compaction 结果的 tablets
    repeated int64 tablets_without_compaction = 21; // 无 compaction 结果的 tablets
}
```

### 2.4 扩展 `CompactionPolicy`

**文件**: `be/src/storage/lake/compaction_policy.h/cpp`

当前 `CompactionPolicy` 的 `pick_rowsets()` 不支持排除正在 compacting 的 rowsets。需要扩展：

```cpp
// 新增方法
class CompactionPolicy {
public:
    // 现有接口
    virtual StatusOr<std::vector<RowsetPtr>> pick_rowsets() = 0;
    
    // 新增：支持排除集合和数据量限制
    virtual StatusOr<std::vector<RowsetPtr>> pick_rowsets_with_exclusion(
        const std::unordered_set<uint32_t>& excluded_rowsets,
        int64_t max_bytes = 0  // 0 表示不限制
    );
};
```

**实现思路**:
- 在 `PrimaryCompactionPolicy::pick_rowsets()` 和 `SizeTieredCompactionPolicy::pick_rowsets()` 中，增加过滤逻辑，跳过 `excluded_rowsets` 中的 rowset
- 如果设置了 `max_bytes`，累加选中 rowset 的大小，超过阈值时停止选择

**注意**: 现有 `TabletParallelCompactionManager::pick_rowsets_for_compaction()` 已经有类似逻辑（通过 `compacting_rowsets` 过滤），可以复用该机制。

### 2.5 扩展 BE `CompactionScheduler` - 处理 PUBLISH_AUTONOMOUS

**文件**: `be/src/storage/lake/compaction_scheduler.cpp`

在 `compact()` 方法中新增对 `PUBLISH_AUTONOMOUS` 类型的处理：

```cpp
void CompactionScheduler::compact(RpcController* controller, 
                                   const CompactRequest* request,
                                   CompactResponse* response, 
                                   Closure* done) {
    if (request->compact_type() == CompactRequest::PUBLISH_AUTONOMOUS) {
        process_publish_autonomous(request, response, done);
        return;
    }
    // ... 现有逻辑 ...
}
```

**`process_publish_autonomous()` 实现思路**:

```
对于请求中的每个 tablet_id:
    1. 从 CompactionResultManager 获取本地结果
       - 过滤条件: result.base_version <= request.visible_version
    2. 如果有结果:
       a. 合并同一 tablet 的多个结果为一个 TxnLog
          - input_rowsets = union(all results' input_rowsets)
          - output_rowsets = concat(all results' output_rowsets)
       b. 写 TxnLog 到 S3
       c. 调用 publish_version (正常路径)
       d. 记录到 tablets_with_compaction
    3. 如果没有结果:
       a. 不生成 TxnLog
       b. 调用 publish_version (force_publish 路径)
          - BE 发现 txn_log 不存在 + force_publish=true
          - 执行 ignore_txn_log，仅更新版本号
       c. 记录到 tablets_without_compaction
    4. 将 tablet_ids 加入 LakeCompactionManager 的调度队列
       (触发下一轮自主 compaction)
```

**TxnLog 合并逻辑** (复用 `TabletParallelCompactionManager::get_merged_txn_log()` 的思路):

```cpp
void build_txn_log_for_tablet(
    const std::vector<CompactionResultPB>& results,
    TxnLogPB* txn_log) {
    auto* op_compaction = txn_log->mutable_op_compaction();
    
    // 合并所有 input_rowsets (去重)
    std::unordered_set<uint32_t> input_set;
    for (const auto& result : results) {
        for (uint32_t rid : result.input_rowset_ids()) {
            input_set.insert(rid);
        }
    }
    for (uint32_t rid : input_set) {
        op_compaction->add_input_rowsets(rid);
    }
    
    // 合并所有 output_rowsets
    for (const auto& result : results) {
        auto* output = op_compaction->add_output_rowsets();
        *output = result.output_rowset();
    }
}
```

### 2.6 修改 `publish_version` 触发 compaction

**文件**: `be/src/storage/lake/transactions.cpp`

在 `publish_version` 完成后，触发 tablet 的 compaction 评估：

```cpp
Status publish_version(...) {
    // ... 现有逻辑 ...
    
    // 新增：publish 成功后触发 compaction
    if (auto* mgr = LakeCompactionManager::instance()) {
        mgr->update_tablet_async(tablet_id);
    }
    
    return Status::OK();
}
```

### 2.7 新增 BE 配置参数

**文件**: `be/src/common/config.h`

```cpp
// 自主 compaction 模式开关
CONF_mBool(lake_enable_autonomous_compaction, "false");

// Compaction score 阈值，超过该值才触发 compaction
CONF_mDouble(lake_compaction_score_threshold, "10.0");

// BE 全局最大并发 compaction 任务数
CONF_mInt32(lake_compaction_max_concurrent_tasks, "32");

// 单 tablet 最大并发任务数
CONF_mInt32(lake_compaction_max_tasks_per_tablet, "3");

// 单个 compaction 任务最大处理数据量 (10GB)
CONF_mInt64(lake_compaction_max_bytes_per_task, "10737418240");
```

---

## 3. FE 侧修改

### 3.1 扩展 `CompactionScheduler` (FE)

**文件**: `fe/fe-core/src/main/java/com/starrocks/lake/compaction/CompactionScheduler.java`

#### 3.1.1 新增自主 compaction publish 调度逻辑

在 `scheduleNewCompaction()` 中新增 autonomous 模式的分支：

```java
private void scheduleNewCompaction() {
    // === Phase 0: 处理 autonomous compaction publish ===
    if (Config.lake_enable_autonomous_compaction) {
        scheduleAutonomousPublish();
    }
    
    // === Phase 1: 处理已完成的 jobs (现有逻辑) ===
    // ... 现有代码不变 ...
    
    // === Phase 2: 启动新 jobs ===
    if (Config.lake_enable_autonomous_compaction) {
        // autonomous 模式下，不再创建 NORMAL CompactionJob
        // 而是由 scheduleAutonomousPublish() 处理
        return;
    }
    // ... 现有代码不变 ...
}
```

**`scheduleAutonomousPublish()` 方法**:

```java
private void scheduleAutonomousPublish() {
    // 遍历所有有统计信息的 partition
    for (PartitionStatistics stats : compactionManager.getAllStatistics()) {
        PartitionIdentifier partition = stats.getPartition();
        
        // 跳过已有 running compaction 的 partition
        if (runningCompactions.containsKey(partition)) {
            continue;
        }
        
        // 判断是否需要触发 publish
        if (!shouldTriggerPublish(stats)) {
            continue;
        }
        
        // 创建 PUBLISH_AUTONOMOUS 类型的 CompactionJob
        CompactionJob job = startAutonomousPublish(partition, stats);
        if (job != null) {
            runningCompactions.put(partition, job);
        }
    }
}
```

**Publish 触发策略** (`shouldTriggerPublish()`):

```java
private boolean shouldTriggerPublish(PartitionStatistics stats) {
    long timeSinceLastPublish = System.currentTimeMillis() - stats.getLastPublishTime();
    double score = stats.getCompactionScore().getMax();
    long versionDelta = stats.getCurrentVersion() - stats.getCompactionVersion();
    
    // 策略1: 高 score 优先
    if (score > Config.lake_compaction_high_score_threshold 
        && versionDelta >= Config.lake_compaction_min_version_delta_for_high_score) {
        return true;
    }
    
    // 策略2: 版本 delta 达到阈值
    if (versionDelta >= Config.lake_compaction_version_delta_threshold) {
        return true;
    }
    
    // 策略3: 超过最大时间间隔
    if (timeSinceLastPublish > Config.lake_compaction_max_interval_ms) {
        return true;
    }
    
    return false;
}
```

#### 3.1.2 `startAutonomousPublish()` 方法

```java
private CompactionJob startAutonomousPublish(PartitionIdentifier partitionId, 
                                              PartitionStatistics stats) {
    // 1. 验证 DB/Table/Partition 存在
    Database db = ...;
    OlapTable table = ...;
    PhysicalPartition partition = ...;
    
    // 2. 收集所有 tablet 按 BE 分组
    Map<Long, List<Long>> beToTablets = collectPartitionTablets(partition, computeResource);
    
    // 3. 开始事务
    long txnId = beginTransaction(partitionId, computeResource);
    long currentVersion = partition.getVisibleVersion();
    
    // 4. 创建 CompactionJob (PUBLISH_AUTONOMOUS 类型)
    CompactionJob job = new CompactionJob(db, table, partition, txnId,
            true /* allowPartialSuccess */, computeResource, warehouseName);
    
    // 5. 构建 PUBLISH_AUTONOMOUS 请求发送到各 BE
    List<CompactionTask> tasks = createAutonomousPublishTasks(
        currentVersion, beToTablets, txnId, table);
    for (CompactionTask task : tasks) {
        task.sendRequest();
    }
    job.setTasks(tasks);
    
    return job;
}
```

#### 3.1.3 构建 PUBLISH_AUTONOMOUS 请求

```java
private List<CompactionTask> createAutonomousPublishTasks(
        long currentVersion, Map<Long, List<Long>> beToTablets, 
        long txnId, OlapTable table) {
    List<CompactionTask> tasks = new ArrayList<>();
    
    for (Map.Entry<Long, List<Long>> entry : beToTablets.entrySet()) {
        CompactRequest request = new CompactRequest();
        request.tabletIds = entry.getValue();
        request.txnId = txnId;
        request.version = currentVersion;
        request.timeoutMs = Config.lake_compaction_publish_timeout_seconds * 1000L;
        request.allowPartialSuccess = true;
        request.compactType = CompactRequest.CompactType.PUBLISH_AUTONOMOUS;
        
        CompactionTask task = new CompactionTask(entry.getKey(), service, request);
        tasks.add(task);
    }
    return tasks;
}
```

### 3.2 扩展 `CompactionJob`

**文件**: `fe/fe-core/src/main/java/com/starrocks/lake/compaction/CompactionJob.java`

无需大改，但需要在 `getResult()` 中对 `PUBLISH_AUTONOMOUS` 类型做特殊处理——该类型下始终允许 partial success。

### 3.3 扩展 `PartitionStatistics`

**文件**: `fe/fe-core/src/main/java/com/starrocks/lake/compaction/PartitionStatistics.java`

新增字段:

```java
// 上次 publish 时间（用于时间间隔策略）
private volatile long lastPublishTime = 0;

public long getLastPublishTime() { return lastPublishTime; }
public void setLastPublishTime(long time) { this.lastPublishTime = time; }
```

### 3.4 扩展 `CompactionMgr`

**文件**: `fe/fe-core/src/main/java/com/starrocks/lake/compaction/CompactionMgr.java`

新增方法:

```java
// 获取所有 partition 的统计信息（用于 autonomous 扫描）
public Collection<PartitionStatistics> getAllStatistics() {
    return partitionStatisticsHashMap.values();
}
```

在 `handleCompactionFinished()` 中更新 `lastPublishTime`:

```java
public void handleCompactionFinished(PartitionIdentifier partition, ...) {
    PartitionStatistics stats = getStatistics(partition);
    if (stats != null) {
        stats.setLastPublishTime(System.currentTimeMillis());
    }
    // ... 现有逻辑 ...
}
```

### 3.5 新增 FE 配置参数

**文件**: `fe/fe-core/src/main/java/com/starrocks/common/Config.java`

```java
// 是否启用自主 compaction 模式
@ConfField(mutable = true)
public static boolean lake_enable_autonomous_compaction = false;

// 版本 delta 阈值，超过则触发 publish
@ConfField(mutable = true)
public static int lake_compaction_version_delta_threshold = 10;

// 高 score 阈值
@ConfField(mutable = true)
public static double lake_compaction_high_score_threshold = 50.0;

// 高 score 场景下的最小版本 delta
@ConfField(mutable = true)
public static int lake_compaction_min_version_delta_for_high_score = 5;

// 最大 publish 间隔 (30 分钟)
@ConfField(mutable = true)
public static long lake_compaction_max_interval_ms = 1800000;

// PUBLISH_AUTONOMOUS RPC 超时 (5 分钟)
@ConfField(mutable = true)
public static int lake_compaction_publish_timeout_seconds = 300;
```

---

## 4. 完整执行流程

### 4.1 正常流程

```
                     FE                                      BE
                      │                                       │
                      │  1. publish_version (数据加载完成)      │
                      │──────────────────────────────────────►│
                      │                                       │
                      │                                       │ 2. publish 成功
                      │                                       │    → LakeCompactionManager
                      │                                       │      ::update_tablet_async()
                      │                                       │
                      │                                       │ 3. 计算 score > 阈值
                      │                                       │    → 加入优先级队列
                      │                                       │
                      │                                       │ 4. 调度线程取出 tablet
                      │                                       │    → pick_rowsets (排除 compacting)
                      │                                       │    → 执行 compaction
                      │                                       │    → 保存 CompactionResultPB
                      │                                       │
                      │                                       │ 5. 如果数据量 >= 80% 阈值
                      │                                       │    → 自触发下一轮
                      │                                       │
   6. 定期扫描 partition │                                       │
      shouldTriggerPublish() == true                           │
                      │                                       │
   7. 开始事务          │                                       │
      beginTransaction()                                      │
                      │                                       │
   8. 发送 PUBLISH_AUTONOMOUS │                                │
      CompactRequest   │──────────────────────────────────────►│
      (所有 tablets)   │                                       │
                      │                                       │ 9. 收集本地 compaction results
                      │                                       │    base_version <= visible_version
                      │                                       │
                      │                                       │ 10a. 有结果的 tablets:
                      │                                       │     → 合并结果 → 构建 TxnLog
                      │                                       │     → 写 S3 → publish_version
                      │                                       │
                      │                                       │ 10b. 无结果的 tablets:
                      │                                       │     → 不写 TxnLog
                      │                                       │     → publish_version (force_publish)
                      │                                       │     → 仅更新版本号
                      │                                       │
                      │◄──────────────────────────────────────│ 11. 返回结果
                      │  CompactResponse                       │
                      │                                       │
  12. 提交事务          │                                       │ 12. 触发所有 tablets
      (forceCommit)   │                                       │     下一轮 compaction
                      │                                       │
  13. publish 可见      │                                       │
      → 下一轮扫描     │                                       │
```

### 4.2 跨版本 Compaction 正确性

```
第一次 Publish (v10 → v11):
  Tablet A: 有 compaction 结果 (base_version=v10)
    → 应用 compaction，更新到 v11
  Tablet B: 无结果 (正在 compacting，base_version=v10)  
    → 不应用 compaction，但更新到 v11
    → v11 metadata 内容与 v10 完全一致

第二次 Publish (v11 → v12):
  Tablet B 的 compaction 终于完成 (base_version=v10)
  问题：能从 v11 metadata 中找到 v10 选中的 rowsets 吗?
  回答：能 ✅
  原因：
    - Tablet B 的 v11 内容与 v10 一致
    - compaction 选中的 input_rowsets 存在于 v10
    - 因此也存在于 v11
    - 应用 compaction 时可以成功移除这些 rowsets
```

---

## 5. 具体文件修改清单

### 5.1 新增文件

| 文件 | 说明 |
|------|------|
| `be/src/storage/lake/lake_compaction_manager.h` | BE 自主 compaction 调度器头文件 |
| `be/src/storage/lake/lake_compaction_manager.cpp` | BE 自主 compaction 调度器实现 |
| `be/src/storage/lake/compaction_result_manager.h` | Compaction 结果管理器头文件 |
| `be/src/storage/lake/compaction_result_manager.cpp` | Compaction 结果管理器实现 |
| `be/test/storage/lake/lake_compaction_manager_test.cpp` | 调度器单测 |
| `be/test/storage/lake/compaction_result_manager_test.cpp` | 结果管理器单测 |

### 5.2 修改文件

| 文件 | 修改内容 |
|------|---------|
| **BE Protobuf** | |
| `gensrc/proto/lake_types.proto` | 新增 `CompactionResultPB` 消息 |
| `gensrc/proto/lake_service.proto` | `CompactRequest` 新增 `compact_type` 字段；`CompactResponse` 新增 autonomous 相关字段 |
| **BE C++** | |
| `be/src/common/config.h` | 新增 autonomous compaction 配置参数 |
| `be/src/storage/lake/compaction_policy.h` | 新增 `pick_rowsets_with_exclusion()` 方法 |
| `be/src/storage/lake/compaction_policy.cpp` | 实现排除逻辑和数据量限制 |
| `be/src/storage/lake/compaction_scheduler.h` | 新增 `process_publish_autonomous()` 声明 |
| `be/src/storage/lake/compaction_scheduler.cpp` | 新增 `PUBLISH_AUTONOMOUS` 处理逻辑 |
| `be/src/storage/lake/lake_service.cpp` | `compact()` 入口分发 `PUBLISH_AUTONOMOUS` |
| `be/src/storage/lake/transactions.cpp` | `publish_version` 完成后触发 `update_tablet_async()` |
| `be/src/storage/lake/tablet_manager.h/cpp` | 可能需要暴露计算 compaction score 的方法 |
| **FE Java** | |
| `fe/fe-core/.../common/Config.java` | 新增 autonomous compaction 配置参数 |
| `fe/fe-core/.../lake/compaction/CompactionScheduler.java` | 新增 `scheduleAutonomousPublish()` 和相关方法 |
| `fe/fe-core/.../lake/compaction/CompactionJob.java` | 支持 `PUBLISH_AUTONOMOUS` 类型 |
| `fe/fe-core/.../lake/compaction/CompactionMgr.java` | 新增 `getAllStatistics()` 方法 |
| `fe/fe-core/.../lake/compaction/PartitionStatistics.java` | 新增 `lastPublishTime` 字段 |
| `fe/fe-core/.../proto/CompactRequest.java` (生成) | 新增 `compactType` 字段 |
| **测试** | |
| `fe/fe-core/src/test/.../CompactionSchedulerTest.java` | 新增 autonomous 模式测试 |
| `be/test/storage/lake/compaction_policy_test.cpp` | 新增排除逻辑测试 |
| `be/test/storage/lake/compaction_scheduler_test.cpp` | 新增 PUBLISH_AUTONOMOUS 测试 |

---

## 6. 与现有 Parallel Compaction 框架的复用关系

### 6.1 直接复用的模块

| 现有模块 | 复用方式 |
|---------|---------|
| `CompactionTask` (Horizontal/Vertical) | 自主 compaction 的执行引擎完全复用 |
| `CompactionPolicy` | 复用 rowset 选择算法，扩展排除逻辑 |
| `TabletParallelCompactionManager` 的 subtask 分组逻辑 | 当单 tablet 需要并行时复用 `_create_subtask_groups()` |
| `TxnLog` 合并逻辑 | 复用 `get_merged_txn_log()` 中的合并思路 |
| `Limiter` | 复用内存限制和并发控制机制 |
| `force_publish` / `partial_success` | 完全复用现有的 `ignore_txn_log` 和 `observe_empty_compaction` 逻辑 |

### 6.2 扩展的模块

| 模块 | 扩展内容 |
|------|---------|
| `CompactionPolicy` | 新增 `pick_rowsets_with_exclusion()` |
| `CompactionScheduler` (BE) | 新增 `PUBLISH_AUTONOMOUS` 处理 |
| `CompactionScheduler` (FE) | 新增 autonomous publish 调度 |

### 6.3 新增的模块

| 模块 | 说明 |
|------|------|
| `LakeCompactionManager` | 事件驱动的 tablet 级调度器 |
| `CompactionResultManager` | 本地结果持久化 |

---

## 7. 配置参数完整列表

### 7.1 BE 配置 (新增)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `lake_enable_autonomous_compaction` | `false` | 是否启用 BE 自主 compaction |
| `lake_compaction_score_threshold` | `10.0` | Score 阈值 |
| `lake_compaction_max_concurrent_tasks` | `32` | 全局最大并发任务数 |
| `lake_compaction_max_tasks_per_tablet` | `3` | 单 tablet 最大并发任务数 |
| `lake_compaction_max_bytes_per_task` | `10737418240` (10GB) | 单任务最大数据量 |

### 7.2 FE 配置 (新增)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `lake_enable_autonomous_compaction` | `false` | 是否启用自主 compaction 模式 |
| `lake_compaction_version_delta_threshold` | `10` | 版本 delta 阈值 |
| `lake_compaction_high_score_threshold` | `50.0` | 高 score 阈值 |
| `lake_compaction_min_version_delta_for_high_score` | `5` | 高 score 最小版本 delta |
| `lake_compaction_max_interval_ms` | `1800000` (30min) | 最大 publish 间隔 |
| `lake_compaction_publish_timeout_seconds` | `300` (5min) | Publish RPC 超时 |

---

## 8. 异常处理和恢复

### 8.1 BE 崩溃恢复

1. **本地结果恢复**: `CompactionResultManager::recover_on_startup()` 扫描结果目录
2. **Score 驱动调度**: Partition score 仍然很高 → FE 继续调度
3. **PUBLISH_AUTONOMOUS 触发调度**: FE 请求包含所有 tablet_ids → BE 按需恢复调度
4. **数据安全**: 未完成的 compaction 不影响原始数据；S3 写入是原子的

### 8.2 FE 崩溃恢复

1. **无状态设计**: FE 不存储中间调度状态
2. **未完成事务处理**: FE 重启后检查未完成事务，过期事务回滚
3. **BE 本地结果保留**: FE 回滚事务不删除 BE 本地结果，下次 publish 可复用

### 8.3 网络分区

- FE 无法到达 BE：PUBLISH_AUTONOMOUS RPC 超时 → 事务回滚 → 下一轮重试
- BE 无法到达 S3：compaction 任务失败 → 结果不保存 → 下次重新执行

---

## 9. 渐进式实施计划

### Phase 1: 基础框架 (必须)

1. Protobuf 修改（`CompactionResultPB`, `compact_type`）
2. `CompactionResultManager` 实现
3. `LakeCompactionManager` 核心调度逻辑
4. `CompactionPolicy` 排除逻辑
5. BE 配置参数

### Phase 2: Publish 机制 (必须)

1. BE `PUBLISH_AUTONOMOUS` 处理
2. FE `scheduleAutonomousPublish()` 逻辑
3. FE 配置参数
4. `PartitionStatistics` 扩展

### Phase 3: 优化和测试 (必须)

1. 完整单元测试
2. 集成测试
3. 性能测试
4. 异常恢复测试

### Phase 4: 高级特性 (可选)

1. 自适应并发控制
2. 监控 metrics
3. 运维命令支持
