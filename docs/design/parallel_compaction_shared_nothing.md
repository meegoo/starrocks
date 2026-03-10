# Shared-Nothing Parallel Compaction 设计方案

> 基于当前 Lake Parallel Compaction 框架，将 subtask 级别的并行 compaction 能力扩展到 Shared-Nothing 路径。

## 1. 背景与动机

### 1.1 现状

**Shared-Nothing（本地存储）** 的 compaction 当前有两种并行模式：

| 并行模式 | 说明 | 局限性 |
|----------|------|--------|
| **跨 Tablet 并行** | `CompactionManager` 调度不同 Tablet 的 compaction 任务并行执行 | 单 Tablet 内无法加速 |
| **同 Tablet 多 Level 并行** | `SizeTieredCompactionPolicy` 利用 `is_compacting` 标志，允许同一 Tablet 的不同 level 同时执行 | 每个 level 仍然串行执行；大 level（如 base compaction）耗时长无法拆分 |

**Lake（共享存储）** 已实现完整的 subtask 级别并行：

- `TabletParallelCompactionManager` 将单次 compaction 拆分为多个 subtask
- 支持 NORMAL（多 rowset 分组）和 LARGE_ROWSET_PART（segment 范围拆分）两种模式
- 各 subtask 独立执行后合并 TxnLog

### 1.2 目标

将 Lake 的 subtask 级别并行能力**复用**到 Shared-Nothing 路径，使单次 compaction（尤其是大的 base compaction）可以被拆分为多个 subtask 并行执行，显著降低大 compaction 的延迟。

---

## 2. 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                    CompactionManager (调度器)                          │
│  _schedule() → pick_candidate() → submit_compaction_task()           │
└───────────────┬──────────────────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────────────────┐
│                submit_compaction_task() 内部决策                       │
│                                                                       │
│  tablet->create_compaction_task()                                     │
│       │                                                               │
│       ├─→ 返回单个 CompactionTask → 直接执行（现有流程）                │
│       │                                                               │
│       └─→ 返回 nullptr + 同时产出 ParallelCompactionPlan              │
│            → 创建 LocalParallelCompactionManager                      │
│            → 拆分为 subtask 组 → 提交到线程池并行执行                    │
│            → 所有 subtask 完成后原子性 commit                           │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心数据结构

### 3.1 `LocalSubtaskGroup`（新增）

类比 Lake 的 `SubtaskGroup`，表示一个 subtask 的工作单元。

```cpp
// be/src/storage/local_parallel_compaction_manager.h

enum class LocalSubtaskType {
    NORMAL,           // 多个完整 rowset 组成的分组
    SEGMENT_RANGE     // 单个大 rowset 按 segment 范围拆分
};

struct LocalSubtaskGroup {
    LocalSubtaskType type = LocalSubtaskType::NORMAL;

    // NORMAL: 多个完整 rowset
    std::vector<RowsetSharedPtr> rowsets;

    // SEGMENT_RANGE: 单个 rowset + segment 范围
    RowsetSharedPtr large_rowset;
    int32_t segment_start = 0;
    int32_t segment_end = 0;

    int64_t total_bytes = 0;
};
```

### 3.2 `LocalSubtaskInfo`（新增）

```cpp
struct LocalSubtaskInfo {
    int32_t subtask_id = 0;
    std::vector<RowsetSharedPtr> input_rowsets;
    int64_t input_bytes = 0;
    int64_t start_time = 0;

    LocalSubtaskType type = LocalSubtaskType::NORMAL;
    // SEGMENT_RANGE 类型
    int32_t segment_start = 0;
    int32_t segment_end = 0;

    // 输出 rowset（subtask 完成后设置）
    RowsetSharedPtr output_rowset;
    Status status;
};
```

### 3.3 `LocalParallelCompactionState`（新增）

类比 Lake 的 `TabletParallelCompactionState`，管理单个 Tablet 的并行 compaction 状态。

```cpp
struct LocalParallelCompactionState {
    int64_t tablet_id = 0;
    TabletSharedPtr tablet;
    TabletSchemaCSPtr tablet_schema;
    CompactionType compaction_type;
    Version output_version;  // 整体输出版本范围
    double compaction_score = 0;

    // 所有 subtask 共享的输入 rowset（用于最终 commit 时替换）
    std::vector<RowsetSharedPtr> all_input_rowsets;

    // 运行中的 subtask
    std::unordered_map<int32_t, LocalSubtaskInfo> running_subtasks;
    // 已完成的 subtask
    std::vector<LocalSubtaskInfo> completed_subtasks;

    int32_t next_subtask_id = 0;
    int32_t total_subtasks_created = 0;

    mutable std::mutex mutex;

    bool is_complete() const {
        return running_subtasks.empty() && total_subtasks_created > 0;
    }
};
```

### 3.4 `LocalParallelCompactionManager`（新增）

核心管理器，负责拆分、执行、合并。

```cpp
// be/src/storage/local_parallel_compaction_manager.h

class LocalParallelCompactionManager {
public:
    // 判断是否应该使用并行 compaction
    // 返回 true 表示应该并行执行
    static bool should_parallel_compact(const TabletSharedPtr& tablet,
                                        const std::vector<RowsetSharedPtr>& input_rowsets,
                                        CompactionType type);

    // 创建并行 compaction subtask
    // 返回 subtask 数量，0 表示不适合并行
    StatusOr<int> create_parallel_tasks(
        const TabletSharedPtr& tablet,
        std::vector<RowsetSharedPtr> input_rowsets,
        Version output_version,
        CompactionType compaction_type,
        double compaction_score,
        ThreadPool* thread_pool);

    // subtask 完成回调
    void on_subtask_complete(int64_t tablet_id, int32_t subtask_id);

    // 所有 subtask 完成后的原子 commit
    Status commit_parallel_compaction(int64_t tablet_id);

    // 清理状态
    void cleanup(int64_t tablet_id);

    // 监控
    void list_tasks(std::vector<CompactionTaskInfo>* infos);

private:
    // 拆分 rowset 为 subtask 组
    std::vector<LocalSubtaskGroup> _create_subtask_groups(
        const TabletSharedPtr& tablet,
        const std::vector<RowsetSharedPtr>& rowsets,
        int32_t max_parallel,
        int64_t max_bytes_per_subtask);

    // 判断大 rowset 是否应拆分
    static bool _is_large_rowset_for_split(const RowsetSharedPtr& rowset,
                                            int64_t max_bytes_per_subtask);

    // 按 segment 范围拆分大 rowset
    static std::vector<LocalSubtaskGroup> _split_large_rowset(
        const RowsetSharedPtr& rowset,
        int64_t target_bytes_per_subtask);

    // 将小 rowset 分组
    static std::vector<LocalSubtaskGroup> _group_small_rowsets(
        std::vector<RowsetSharedPtr> rowsets,
        int64_t target_bytes_per_subtask);

    // 执行单个 subtask（NORMAL 类型）
    void _execute_subtask(int64_t tablet_id, int32_t subtask_id);

    // 执行单个 subtask（SEGMENT_RANGE 类型）
    void _execute_subtask_segment_range(int64_t tablet_id, int32_t subtask_id);

    std::unordered_map<int64_t, std::shared_ptr<LocalParallelCompactionState>> _tablet_states;
    mutable std::mutex _states_mutex;
};
```

---

## 4. 详细修改方案

### 4.1 新增文件

| 文件 | 说明 |
|------|------|
| `be/src/storage/local_parallel_compaction_manager.h` | 核心管理器头文件 |
| `be/src/storage/local_parallel_compaction_manager.cpp` | 核心管理器实现 |
| `be/test/storage/local_parallel_compaction_manager_test.cpp` | 单元测试 |

### 4.2 修改文件

#### 4.2.1 `be/src/storage/compaction_manager.h/.cpp`

**改动点**：在 `CompactionManager` 中集成 `LocalParallelCompactionManager`。

```cpp
// compaction_manager.h 新增成员
class CompactionManager {
    // ... 现有成员 ...
private:
    std::unique_ptr<LocalParallelCompactionManager> _parallel_mgr;
};
```

**改动点**：修改 `submit_compaction_task()` 以支持并行模式。

```cpp
// compaction_manager.cpp
void CompactionManager::submit_compaction_task(const CompactionCandidate& candidate) {
    // 现有逻辑: 提交到线程池
    _compaction_pool->submit_func([...] {
        auto compaction_task = tablet->create_compaction_task();
        if (compaction_task != nullptr) {
            // 现有流程：单任务执行
            compaction_task->start();
        } else {
            // 新增流程：尝试并行 compaction
            // create_compaction_task() 返回 nullptr 可能有多种原因
            // 需要检查是否适合并行执行
        }
    });
}
```

更好的方案是修改 `_schedule()` 中 submit 后的逻辑：

```cpp
void CompactionManager::_schedule() {
    while (!_stop.load()) {
        _wait_to_run();
        CompactionCandidate candidate;
        if (!pick_candidate(&candidate)) continue;

        // 先尝试并行模式
        if (_try_submit_parallel_compaction(candidate)) {
            continue;
        }
        // 回退到单任务模式
        submit_compaction_task(candidate);
    }
}

bool CompactionManager::_try_submit_parallel_compaction(const CompactionCandidate& candidate) {
    auto tablet = candidate.tablet;
    // 判断是否启用并行 compaction
    if (!config::enable_parallel_compaction) return false;

    // 通过 policy 获取选中的 rowsets
    // ... (需要从 policy 中获取)
    // 调用 _parallel_mgr->create_parallel_tasks(...)
    // 如果返回 > 0，说明已经提交了并行 subtask
    return true;
}
```

#### 4.2.2 `be/src/storage/compaction_policy.h`

**改动点**：扩展 `CompactionPolicy` 接口，支持获取选中的 rowset 列表（不立刻创建 task）。

```cpp
class CompactionPolicy {
public:
    virtual ~CompactionPolicy() = default;

    virtual bool need_compaction(double* score, CompactionType* type) = 0;
    virtual std::shared_ptr<CompactionTask> create_compaction(TabletSharedPtr tablet) = 0;

    // 新增：获取选中的 rowsets（不创建 task，不设置 is_compacting）
    virtual bool get_selected_rowsets(std::vector<RowsetSharedPtr>* rowsets,
                                     CompactionType* type) {
        return false;  // 默认不支持
    }
};
```

#### 4.2.3 `be/src/storage/size_tiered_compaction_policy.h/.cpp`

**改动点**：实现 `get_selected_rowsets()` 方法。

```cpp
// size_tiered_compaction_policy.h
class SizeTieredCompactionPolicy : public CompactionPolicy {
public:
    // 新增
    bool get_selected_rowsets(std::vector<RowsetSharedPtr>* rowsets,
                             CompactionType* type) override;
    // ... 现有接口 ...
};
```

```cpp
// size_tiered_compaction_policy.cpp
bool SizeTieredCompactionPolicy::get_selected_rowsets(
        std::vector<RowsetSharedPtr>* rowsets, CompactionType* type) {
    if (_compaction_type == INVALID_COMPACTION || _rowsets.empty()) {
        return false;
    }
    *rowsets = _rowsets;
    *type = _compaction_type;
    return true;
}
```

#### 4.2.4 `be/src/storage/compaction_task.h`

**改动点**：修改 `_commit_compaction()` 支持多输出 rowset 提交（或抽取为可复用函数）。

```cpp
// 新增静态方法，支持原子性多输出提交
static Status commit_parallel_compaction(
    const TabletSharedPtr& tablet,
    const std::vector<RowsetSharedPtr>& output_rowsets,
    const std::vector<RowsetSharedPtr>& input_rowsets);
```

#### 4.2.5 `be/src/common/config.h`

**改动点**：新增配置参数。

```cpp
// 是否启用 shared-nothing 并行 compaction
CONF_mBool(enable_local_parallel_compaction, "false");

// 每个 subtask 的最大数据量 (bytes)
CONF_mInt64(local_compaction_max_bytes_per_subtask, "1073741824");  // 1GB

// 单个 tablet 最大并行 subtask 数
CONF_mInt32(local_compaction_max_parallel_per_tablet, "4");

// 触发并行 compaction 的最小数据量阈值
CONF_mInt64(local_compaction_parallel_threshold_bytes, "2147483648");  // 2GB

// 触发大 rowset segment 拆分的最小 segment 数
CONF_mInt32(local_compaction_min_segments_for_split, "4");
```

---

## 5. 核心逻辑详解

### 5.1 判断是否使用并行 compaction

```
should_parallel_compact():
  1. config::enable_local_parallel_compaction == true
  2. 输入 rowsets 总数据量 >= local_compaction_parallel_threshold_bytes
  3. 输入 rowsets 总 segment 数 >= local_compaction_min_segments_for_split
  4. 非 Primary Key 表（PK 表有 delete/partial update 语义，需要特殊处理）
     或 PK 表但满足特定条件
  5. 没有 delete predicate（或仅在 base compaction 时允许）
```

### 5.2 Subtask 拆分策略

复用 Lake 的拆分逻辑，但适配 Shared-Nothing 的 Rowset 格式：

```
_create_subtask_groups(rowsets, max_parallel, max_bytes_per_subtask):
  1. 计算总数据量和总 segment 数
  2. 如果总量 <= max_bytes_per_subtask 或 max_parallel <= 1：返回空（不拆分）

  3. 遍历 rowsets，识别大 rowset:
     - data_size > max_bytes_per_subtask
     - segments >= local_compaction_min_segments_for_split
     - is_segments_overlapping() == true
     → 使用 _split_large_rowset() 按 segment 范围拆分

  4. 剩余小 rowset:
     → 使用 _group_small_rowsets() 按数据量分组
     → 每组至少 2 个 rowset（否则不值得 compact）

  5. 合并大 rowset 拆分组和小 rowset 分组
  6. 按 max_parallel 限制总 subtask 数
```

### 5.3 Subtask 执行

每个 subtask 独立执行一个小的 compaction 操作：

```
_execute_subtask(tablet_id, subtask_id):
  1. 从 state 获取 subtask info
  2. 确定输出版本范围:
     - NORMAL: output_version = [first_input.start, last_input.end]
     - SEGMENT_RANGE: output_version 需要特殊处理（见 5.4）

  3. 创建 CompactionTaskFactory → 创建 HorizontalCompactionTask 或 VerticalCompactionTask
  4. 调用 run_impl() 但跳过 commit（只做 merge，不修改 tablet metadata）
  5. 将输出 rowset 保存到 subtask info
  6. 调用 on_subtask_complete()
```

**关键改动**：需要让 `HorizontalCompactionTask::run_impl()` 和 `VerticalCompactionTask::run_impl()` 支持"只 merge，不 commit"模式。

```cpp
// 方案：增加 skip_commit 标志
class CompactionTask {
    bool _skip_commit = false;  // 新增
public:
    void set_skip_commit(bool skip) { _skip_commit = skip; }
};

// HorizontalCompactionTask::run_impl()
Status HorizontalCompactionTask::run_impl() {
    // ... shortcut + merge ...
    RETURN_IF_ERROR(_validate_compaction(statistics));
    if (!_skip_commit) {
        RETURN_IF_ERROR(_commit_compaction());
    }
    return Status::OK();
}
```

### 5.4 输出版本处理

这是 Shared-Nothing 并行 compaction 的核心难点。

**方案 A：保持单输出版本（推荐）**

所有 subtask 共享同一个 `output_version`，但每个 subtask 产出独立的 output rowset。最终 commit 时，使用一个"合并" rowset 替换所有 input rowsets：

```
Input:  [2-3] [4-5] [6-7] [8-9] [10-11]
Split:  Subtask1: [2-3][4-5] → output_rowset_1
        Subtask2: [6-7][8-9][10-11] → output_rowset_2

Commit: 将 output_rowset_1 和 output_rowset_2 合并为一个新的 rowset
        或：直接用多个 output rowset 替换 input rowsets
```

但如果直接用多个 output rowset 替换 input，版本号会有问题——`modify_rowsets_without_lock` 是基于版本号映射的。

**方案 B：每个 subtask 使用子版本范围**

```
Input:  [2-3] [4-5] [6-7] [8-9] [10-11]
Split:  Subtask1: [2-3][4-5] → output [2-5]
        Subtask2: [6-7][8-9][10-11] → output [6-11]

Commit: 替换 [2-3],[4-5] 为 [2-5]
        替换 [6-7],[8-9],[10-11] 为 [6-11]
```

这是最自然的方案，每个 subtask 独立 compact 一组版本连续的 rowset，产出一个合并版本。版本号天然正确。

**方案 C：最终合并（二次 merge）**

所有 subtask 完成后，对 subtask 的输出再做一次 merge 得到单一 output rowset。但这样会增加额外开销，不推荐。

**推荐方案 B**：每个 subtask 处理版本连续的一组 rowsets，输出版本为该组的合并版本范围。Commit 时按组原子替换。

### 5.5 原子 Commit

```cpp
Status LocalParallelCompactionManager::commit_parallel_compaction(int64_t tablet_id) {
    auto state = get_state(tablet_id);
    auto tablet = state->tablet;

    std::unique_lock wrlock(tablet->get_header_lock());

    // 验证所有 input rowsets 仍然存在
    for (auto& rowset : state->all_input_rowsets) {
        if (tablet->get_rowset_by_version(rowset->version()) == nullptr) {
            return Status::InternalError("input rowset not found, concurrent modification");
        }
    }

    // 按 subtask 顺序收集所有 output rowsets
    std::vector<RowsetSharedPtr> all_outputs;
    for (auto& subtask : state->completed_subtasks) {
        if (!subtask.status.ok()) {
            return subtask.status;
        }
        all_outputs.push_back(subtask.output_rowset);
    }

    // 原子替换
    std::vector<RowsetSharedPtr> to_replace;
    tablet->modify_rowsets_without_lock(all_outputs, state->all_input_rowsets, &to_replace);
    tablet->save_meta(config::skip_schema_in_rowset_meta);
    Rowset::close_rowsets(state->all_input_rowsets);
    for (auto& rs : to_replace) {
        StorageEngine::instance()->add_unused_rowset(rs);
    }

    return Status::OK();
}
```

### 5.6 SEGMENT_RANGE 类型 Subtask 的处理

对于大 rowset 的 segment 范围拆分，需要特殊处理：

```
大 Rowset [2-5] (segments: 0,1,2,3,4,5,6,7)
拆分为:
  Subtask1: segments [0-3] → output rowset_1 版本 [2-5]
  Subtask2: segments [4-7] → output rowset_2 版本 [2-5]
```

这里两个 subtask 输出的版本号相同 `[2-5]`，这在 shared-nothing 的版本管理中是不允许的。

**解决方案**：
- 对于 SEGMENT_RANGE 拆分，所有 subtask 的输出 segment 文件组成一个**单一的 output rowset**
- 各 subtask 分别写 segment 文件，最后使用 `RowsetWriter::add_rowset()` 或自定义逻辑将它们组装成一个 rowset
- 这类似于 Lake 的 large_rowset_split → merge segments 逻辑

```cpp
// 对 SEGMENT_RANGE 类型的 subtask，输出 segment 文件后：
Status merge_segment_range_outputs(
    const std::vector<LocalSubtaskInfo>& subtasks,
    const TabletSharedPtr& tablet,
    Version output_version) {
    // 创建一个 RowsetWriter
    // 将各 subtask 的 output segment 文件链接/复制到新 rowset
    // 构建最终 rowset metadata
}
```

---

## 6. 完整执行流程

```
                    ┌─────────────────────────────────────┐
                    │ CompactionManager::_schedule()       │
                    │ pick_candidate() → candidate         │
                    └─────────────┬───────────────────────┘
                                  │
                    ┌─────────────▼───────────────────────┐
                    │ _try_submit_parallel_compaction()    │
                    │                                      │
                    │ 1. tablet->need_compaction()          │
                    │ 2. policy->get_selected_rowsets()     │
                    │ 3. should_parallel_compact() ?        │
                    │    ├─ NO  → return false              │
                    │    │        (回退到单任务模式)          │
                    │    └─ YES → continue                  │
                    └─────────────┬───────────────────────┘
                                  │
                    ┌─────────────▼───────────────────────┐
                    │ _parallel_mgr->create_parallel_tasks │
                    │                                      │
                    │ 1. set_is_compacting(true) on rowsets │
                    │ 2. _create_subtask_groups()           │
                    │ 3. 创建 LocalParallelCompactionState  │
                    │ 4. 提交各 subtask 到 _compaction_pool │
                    └─────────────┬───────────────────────┘
                                  │
                    ┌─────────────▼───────────────────────┐
                    │ 各 subtask 并行执行                    │
                    │                                      │
                    │ _execute_subtask():                   │
                    │   1. CompactionTaskFactory            │
                    │      → HorizontalTask/VerticalTask   │
                    │   2. task.set_skip_commit(true)       │
                    │   3. task.run_impl()                  │
                    │   4. 保存 output_rowset               │
                    │   5. on_subtask_complete()            │
                    └─────────────┬───────────────────────┘
                                  │
                    ┌─────────────▼───────────────────────┐
                    │ 所有 subtask 完成                      │
                    │                                      │
                    │ commit_parallel_compaction():         │
                    │   1. get_header_lock()                │
                    │   2. 验证 input rowsets 仍存在          │
                    │   3. 合并 SEGMENT_RANGE 输出           │
                    │   4. modify_rowsets_without_lock()    │
                    │   5. save_meta()                      │
                    │   6. set_is_compacting(false)         │
                    │   7. update_tablet_async()            │
                    └──────────────────────────────────────┘
```

---

## 7. 锁与并发控制

### 7.1 Rowset 级别

- `is_compacting` 标志：各 subtask 共享同一组 input rowsets 的 `is_compacting` 状态
- 在 `create_parallel_tasks()` 时统一设置 `set_is_compacting(true)`
- 在 `commit` 或 `cleanup`（失败时）统一设置 `set_is_compacting(false)`

### 7.2 Tablet 锁

- `get_cumulative_lock()` / `get_base_lock()`：在并行 compaction 期间，使用 shared lock（与当前行为一致）
- `get_header_lock()`：仅在最终 commit 时获取 unique lock

### 7.3 CompactionManager 并发计数

- 每个并行 compaction 的所有 subtask 共享一个"逻辑任务"计数
- 方案 A：整个并行 compaction 只占 1 个 task 名额（推荐，简单）
- 方案 B：每个 subtask 占 1 个 task 名额（更精确但更复杂）

推荐方案 A，但在 `CompactionManager` 中新增 `_parallel_subtask_count` 计数器用于监控。

---

## 8. 失败处理

### 8.1 部分 Subtask 失败

| 策略 | 说明 |
|------|------|
| **全部失败** | 任何一个 subtask 失败，整个并行 compaction 失败，清理所有 subtask 输出 |
| **部分成功（仅 NORMAL）** | NORMAL 类型的 subtask 可以独立成功；SEGMENT_RANGE 组内必须全部成功 |

推荐初始版本使用"全部失败"策略，简单可靠。后续可以扩展为部分成功。

### 8.2 失败清理

```cpp
void cleanup_on_failure(int64_t tablet_id) {
    auto state = get_state(tablet_id);
    // 1. 删除所有 subtask 的 output rowset 文件
    for (auto& subtask : state->completed_subtasks) {
        if (subtask.output_rowset) {
            StorageEngine::instance()->add_unused_rowset(subtask.output_rowset);
        }
    }
    // 2. 清除 is_compacting 标志
    for (auto& rowset : state->all_input_rowsets) {
        rowset->set_is_compacting(false);
    }
    // 3. 更新 tablet 候选
    StorageEngine::instance()->compaction_manager()->update_tablet_async(state->tablet);
    // 4. 清理 state
    cleanup(tablet_id);
}
```

---

## 9. 配置与监控

### 9.1 新增 BE 配置

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `enable_local_parallel_compaction` | `false` | 是否启用 shared-nothing 并行 compaction |
| `local_compaction_max_bytes_per_subtask` | `1073741824` (1GB) | 每个 subtask 最大数据量 |
| `local_compaction_max_parallel_per_tablet` | `4` | 单 Tablet 最大并行 subtask 数 |
| `local_compaction_parallel_threshold_bytes` | `2147483648` (2GB) | 触发并行的最小数据量 |
| `local_compaction_min_segments_for_split` | `4` | 大 rowset 拆分的最小 segment 数 |

### 9.2 监控指标

| 指标 | 类型 | 说明 |
|------|------|------|
| `local_parallel_compaction_running_subtasks` | Gauge | 当前运行中的 subtask 数 |
| `local_parallel_compaction_completed_subtasks` | Counter | 累计完成的 subtask 数 |
| `local_parallel_compaction_failed_tasks` | Counter | 累计失败的并行 compaction 数 |
| `local_parallel_compaction_total_bytes` | Counter | 并行 compaction 处理的总数据量 |

---

## 10. 实施计划

### Phase 1：基础框架（NORMAL 类型）

1. 新增 `LocalParallelCompactionManager` 及相关数据结构
2. 新增配置参数
3. 修改 `CompactionPolicy` 接口
4. 修改 `SizeTieredCompactionPolicy` 支持 `get_selected_rowsets()`
5. 修改 `CompactionTask` 支持 `skip_commit` 模式
6. 修改 `CompactionManager` 集成并行调度
7. 实现 NORMAL 类型的 subtask 拆分与执行
8. 实现原子 commit
9. 单元测试

### Phase 2：大 Rowset Segment 拆分

1. 实现 `_split_large_rowset()` 和 `_execute_subtask_segment_range()`
2. 实现 segment 输出合并逻辑
3. 单元测试

### Phase 3：PK 表支持

1. PK 表特殊语义处理
2. 与 PK index 并行 compaction 集成

### Phase 4：优化与监控

1. 部分成功策略
2. 自适应并行度
3. 完善监控指标
4. 性能测试与调优

---

## 11. 与 Lake 代码的复用关系

| Lake 模块 | Shared-Nothing 对应 | 复用方式 |
|-----------|---------------------|----------|
| `SubtaskGroup` | `LocalSubtaskGroup` | 结构相似，可提取公共定义 |
| `TabletParallelCompactionState` | `LocalParallelCompactionState` | 裁剪 TxnLog 相关字段 |
| `TabletParallelCompactionManager` | `LocalParallelCompactionManager` | 复用拆分算法逻辑 |
| `_split_large_rowset()` | 直接复用 | segment 拆分逻辑可共享 |
| `_group_small_rowsets()` | 直接复用 | 分组逻辑可共享 |
| `CompactionScheduler::Limiter` | `CompactionManager` 已有并发控制 | 不需要额外 Limiter |
| `OpParallelCompaction` (protobuf) | 不需要 | Shared-Nothing 无 TxnLog |
| `get_merged_txn_log()` | `commit_parallel_compaction()` | 替换为本地 modify_rowsets |

---

## 12. 风险与注意事项

1. **版本号连续性**：每个 NORMAL subtask 必须处理版本连续的 rowset 组，否则 commit 时版本不连续会导致错误
2. **并发 commit**：多个 subtask 的 commit 必须原子执行，使用 `get_header_lock()` 保护
3. **内存压力**：并行 subtask 会增加内存使用，需要受 `compaction_memory_limit_per_worker` 约束
4. **磁盘 IO**：并行读写同一磁盘可能导致 IO 竞争，需要评估对查询延迟的影响
5. **与现有并行的交互**：并行 compaction 和同 Tablet 多 Level 并行可能冲突，需要协调 `is_compacting` 状态
