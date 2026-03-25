# Shared-Data Compaction Observability Enhancement Proposal

## 背景

本文聚焦 **shared-data（存算分离）模式**的 compaction 可观测性，对比业界竞品，分析当前差距并给出改进方案。

> Shared-nothing 模式不再维护，不在本方案范围内。

---

## 一、竞品 Compaction 可观测性对比

### 1. ClickHouse（业界标杆）

三层系统表提供完整的过去-现在视图：

| 层级 | 系统表 | 核心字段 |
|------|--------|---------|
| **实时运行** | `system.merges` | `progress`(0~1)、`elapsed`、`num_parts`、`source_part_names`、`bytes_read/written`、`rows_read/written`、`memory_usage`、`merge_type` |
| **当前快照** | `system.parts` | 每个 part 的行数、大小、merge level、创建/修改时间、partition、状态（active/inactive/deleting） |
| **历史审计** | `system.part_log` | 持久化 MergeTree 表，事件类型（`NewPart`/`MergeParts`/`MutatePart`/`RemovePart` 等）、`duration_ms`、`read_rows`/`read_bytes`、`merged_from`（源 part 数组）、`peak_memory_usage`、`error`/`exception`、`ProfileEvents` map |

Prometheus 指标：`MergedRows`、`MergedUncompressedBytes`、`MergeExecuteMilliseconds`、`Merge`（活跃数）、`MaxPartCountForPartition`。

### 2. Snowflake

以"数据质量"视角提供可观测性：

| 能力 | 具体实现 |
|------|---------|
| **聚簇健康度** | `SYSTEM$CLUSTERING_INFORMATION()`: `average_depth`、`average_overlaps`、`partition_depth_histogram`、`clustering_errors` |
| **历史记录** | `AUTOMATIC_CLUSTERING_HISTORY`: credits、bytes/rows reclustered、时间戳 |
| **剪枝效果** | `TABLE_PRUNING_HISTORY`: 按小时聚合扫描分区数 vs 剪枝分区数 |
| **成本预估** | `SYSTEM$ESTIMATE_AUTOMATIC_CLUSTERING_COSTS()` |

### 3. Databricks (Delta Lake)

| 能力 | 具体实现 |
|------|---------|
| **操作历史** | `DESCRIBE HISTORY`: `operationMetrics`（numFilesAdded/Removed、文件大小分布 p25/p50/p75/max/min） |
| **自动优化追踪** | `system.storage.predictive_optimization_operations_history`: 操作类型、状态、预估 DBU 成本 |
| **返回统计** | OPTIMIZE 命令直接返回 removed/added files 统计 |

### 4. Apache Doris

| 能力 | 具体实现 |
|------|---------|
| **HTTP REST API** | `/api/compaction/show`、`/api/compaction/run`、`/api/compaction_score?top_n=N` |
| **Prometheus Metrics** | `tablet_base/cumulative_max_compaction_score`、`total_rowset_num` |
| **SHOW PROC** | `SHOW PROC '/cluster_health/tablet_health'`、`SHOW TABLET` 带 CompactionStatus URL |

**现状**：以 HTTP API + SHOW PROC 为主，无持久化历史系统表。

---

## 二、StarRocks Shared-Data 现状评估

### 已有能力

#### 2.1 系统表：`information_schema.be_cloud_native_compactions`

来自 BE 端 `CompactionScheduler::list_tasks()`，展示**当前活跃**的 compaction 任务：

| 列名 | 类型 | 说明 |
|------|------|------|
| BE_ID | BIGINT | 执行节点 |
| TXN_ID | BIGINT | 事务 ID |
| TABLET_ID | BIGINT | tablet ID |
| VERSION | BIGINT | 数据版本 |
| SKIPPED | BOOLEAN | 是否跳过 |
| RUNS | INT | 执行次数 |
| START_TIME | DATETIME | 开始时间 |
| FINISH_TIME | DATETIME | 结束时间 |
| PROGRESS | INT | 进度 0~100 |
| STATUS | VARCHAR | 状态/错误信息 |
| PROFILE | VARCHAR | JSON profile |

**局限**：仅显示 BE 内存中的活跃任务，任务完成后即消失，无法查询历史。

#### 2.2 SHOW PROC '/compactions'

来自 FE 端 `CompactionScheduler` 的内存循环队列（`SynchronizedCircularQueue<CompactionRecord>`）：

| 列名 | 说明 |
|------|------|
| Partition | `db.table.partitionId` 字符串 |
| TxnID | 事务 ID |
| StartTime | 开始时间 |
| CommitTime | 提交时间 |
| FinishTime | 完成时间 |
| Error | 错误信息 |
| Profile | JSON profile |

**严重局限**：
- **默认仅保留 20 条**（`lake_compaction_history_size = 20`），极易被覆盖
- **纯内存存储**，FE 重启即全部丢失
- 仅 partition 级别，无 tablet 级别明细
- 无 table_id/partition_id/db_id 等结构化字段，只有 partitionName 字符串
- 无输入/输出 rowset/行/字节统计

#### 2.3 CompactionProfile（JSON）

`CompactionJob.getExecutionProfile()` 聚合所有 tablet 的 `CompactStat`：

| 字段 | 说明 | 精度问题 |
|------|------|---------|
| `sub_task_count` | tablet 子任务数 | — |
| `read_local_sec` / `read_local_mb` | 本地读 | 截断到秒/MB |
| `read_remote_sec` / `read_remote_mb` | 远程读 | 截断到秒/MB |
| `read_segment_count` | 输入 segment 数 | — |
| `write_segment_count` / `write_segment_mb` | 输出 segment | 截断到 MB |
| `write_remote_sec` | 远程写时间 | 截断到秒 |
| `in_queue_sec` | 排队等待时间 | — |

**精度问题**：时间截断到秒、大小截断到 MB。对于小 compaction 任务（毫秒级、KB 级），profile 全部显示为 0。

#### 2.4 Prometheus Metrics

```
starrocks_be_compaction_bytes_total{type="base|cumulative|update"}
starrocks_be_compaction_deltas_total{type="base|cumulative|update"}
starrocks_be_update_compaction_outputs_total
starrocks_be_update_compaction_outputs_bytes_total
starrocks_be_update_compaction_duration_us
starrocks_be_compaction_mem_bytes
starrocks_be_tablet_base_max_compaction_score
starrocks_be_tablet_cumulative_max_compaction_score
```

#### 2.5 Compaction 调度架构

```
FE (CompactionScheduler, 每秒一次)
  │
  ├─ CompactionMgr.choosePartitionsToCompact()
  │   └─ Selector.select() → Sorter.sort()
  │   └─ 按 compaction score 排序
  │
  ├─ startCompaction(partition)
  │   ├─ beginTransaction(LAKE_COMPACTION)
  │   ├─ 按 tablet → CN/BE 映射分组
  │   └─ 发送 CompactRequest RPC
  │
  └─ scheduleNewCompaction() — 检查完成的 job
      ├─ ALL_SUCCESS → commitTransaction → 等待 visible → 记录 CompactionRecord
      ├─ PARTIAL_SUCCESS → 根据配置决定提交或中止
      └─ NONE_SUCCESS → abortTransaction → 记录 CompactionRecord
```

### 差距分析

| 差距 | 对比竞品 | 严重程度 |
|------|---------|---------|
| **历史记录极度不足**：仅 20 条内存队列，FE 重启丢失 | ClickHouse `part_log`（持久化）、Databricks `DESCRIBE HISTORY` | 🔴 严重 |
| **CompactionRecord 信息贫乏**：无 input/output 统计、无结构化 ID | ClickHouse `part_log` 包含完整 I/O 统计 | 🔴 严重 |
| **CompactionProfile 精度丢失**：截断到秒/MB | ClickHouse `duration_ms` + 精确字节数 | 🟡 中 |
| **无 tablet 级历史**：只有 partition 聚合视图 | ClickHouse 每个 part 有独立日志 | 🟡 中 |
| **无数据质量指标**：无 segment 大小分布、无 rowset overlap 统计 | Snowflake `clustering depth`、Databricks 文件大小 p25/p50/p75 | 🟡 中 |
| **FE/BE 信息割裂**：FE 有调度视角但无执行细节，BE 有执行细节但无调度上下文 | ClickHouse 统一在 system 表中 | 🟡 中 |
| **无 compaction 健康诊断**：无法自动判断 compaction 是否跟上写入 | Snowflake `clustering_errors` | 🟢 低 |

---

## 三、改进方案

### Phase 1：增强 Compaction 历史记录（高优先级）

这是当前最大的可观测性瓶颈。**仅 20 条内存历史，FE 重启即丢失**。

#### 1.1 增强 `CompactionRecord`，丰富记录内容

**当前** `CompactionRecord` 字段：
```java
txnId, startTs, commitTs, finishTs, partitionName, errorMessage, executionProfile
```

**增强为**：
```java
// === 结构化标识 ===
txnId              long
dbId               long
dbName             String
tableId            long
tableName          String
partitionId        long
partitionName      String      // 保留向后兼容
warehouse          String      // 执行 warehouse

// === 时间线 ===
startTs            long
commitTs           long
finishTs           long
durationMs         long        // finishTs - startTs，方便查询

// === 状态 ===
state              String      // SUCCESS / PARTIAL_SUCCESS / FAILED / CANCELLED
errorMessage       String

// === 输入输出统计（从 CompactStat 聚合）===
inputTablets       int         // 参与的 tablet 数
inputSegments      long        // 输入 segment 数
inputBytes         long        // 输入字节数（精确，非截断）
outputSegments     long        // 输出 segment 数
outputBytes        long        // 输出字节数（精确）
writeAmplification double      // inputBytes / outputBytes

// === 资源消耗（精确值）===
readBytesLocal     long        // 本地读字节
readBytesRemote    long        // 远程读字节
readTimeLocalMs    long        // 本地读耗时（毫秒）
readTimeRemoteMs   long        // 远程读耗时（毫秒）
writeTimeRemoteMs  long        // 远程写耗时（毫秒）
inQueueTimeMs      long        // 排队等待时间（毫秒）

// === 完整 Profile（保留 JSON 用于扩展）===
executionProfile   String      // 原始 JSON profile
```

**修改文件**：
- `fe/fe-core/.../lake/compaction/CompactionRecord.java` — 扩展字段
- `fe/fe-core/.../lake/compaction/CompactionJob.java` — `CompactionRecord.build()` 传入新字段
- `fe/fe-core/.../lake/compaction/CompactionProfile.java` — 保留毫秒/字节精度的版本

#### 1.2 将 `SHOW PROC '/compactions'` 升级为 `information_schema.compaction_history` 系统表

**目标**：通过标准 SQL 查询 compaction 历史，支持 WHERE/ORDER BY/JOIN。

```sql
SELECT * FROM information_schema.compaction_history
WHERE table_name = 'my_table'
  AND state = 'FAILED'
  AND start_time > NOW() - INTERVAL 1 HOUR
ORDER BY duration_ms DESC
LIMIT 10;
```

**Schema**：

```sql
CREATE VIEW information_schema.compaction_history AS
SELECT
    txn_id              BIGINT,
    db_id               BIGINT,
    db_name             VARCHAR,
    table_id            BIGINT,
    table_name          VARCHAR,
    partition_id        BIGINT,
    warehouse           VARCHAR,
    -- 时间线
    start_time          DATETIME,
    commit_time         DATETIME,
    finish_time         DATETIME,
    duration_ms         BIGINT,
    -- 状态
    state               VARCHAR,        -- SUCCESS/PARTIAL_SUCCESS/FAILED/CANCELLED
    error_message       VARCHAR,
    -- 输入输出统计
    input_tablets       INT,
    input_segments      BIGINT,
    input_bytes         BIGINT,
    output_segments     BIGINT,
    output_bytes        BIGINT,
    write_amplification DOUBLE,
    -- 资源消耗
    read_bytes_local    BIGINT,
    read_bytes_remote   BIGINT,
    read_time_local_ms  BIGINT,
    read_time_remote_ms BIGINT,
    write_time_remote_ms BIGINT,
    in_queue_time_ms    BIGINT,
    -- 扩展
    profile             VARCHAR         -- 完整 JSON profile
);
```

**实现方式**：FE 端 SchemaTable + ProcNode 适配器，数据源为 `CompactionScheduler.history` 循环队列。

**修改文件**：
- `fe/fe-core/.../catalog/system/information/CompactionHistorySystemTable.java`（新增）
- `fe/fe-core/.../catalog/system/SystemTable.java`（注册）
- `gensrc/thrift/Descriptors.thrift`（新增 TSchemaTableType）

#### 1.3 大幅增加历史容量 + 持久化

**短期**：
- 将 `lake_compaction_history_size` 默认值从 **20 → 10000**
- 添加新配置 `lake_compaction_history_ttl_seconds`（默认 86400 = 1 天），按时间淘汰

**中期**：
- 将 CompactionRecord 持久化到 FE Image/Journal，FE 重启后可恢复
- 在 `CompactionMgr.save()` / `CompactionMgr.load()` 中增加历史记录的序列化

**长期**：
- 考虑写入内部表（类似 `_statistics_` 表），支持更长时间的历史查询

---

### Phase 2：增强实时运行视图（中优先级）

#### 2.1 增强 `be_cloud_native_compactions` 系统表

当前 BE 端 `CompactionTaskInfo` 信息有限。增强：

```sql
-- 新增列
ALTER TABLE information_schema.be_cloud_native_compactions ADD COLUMNS (
    TABLE_ID            BIGINT,         -- 表 ID（当前缺失）
    PARTITION_ID        BIGINT,         -- 分区 ID（当前缺失）
    INPUT_SEGMENTS      BIGINT,         -- 输入 segment 数
    INPUT_BYTES         BIGINT,         -- 输入字节数
    OUTPUT_BYTES        BIGINT,         -- 已输出字节数（实时）
    MEMORY_BYTES        BIGINT,         -- 当前内存使用
    COMPACTION_TYPE     VARCHAR         -- BASE/CUMULATIVE/UPDATE
);
```

**修改文件**：
- `be/src/storage/lake/compaction_task_context.h` — 扩展 `CompactionTaskInfo`
- `be/src/exec/schema_scanner/schema_be_cloud_native_compactions_scanner.cpp` — 填充新列
- `fe/fe-core/.../BeCloudNativeCompactionsSystemTable.java` — 新增列定义

#### 2.2 新增 FE 端运行任务视图

`be_cloud_native_compactions` 是 BE 视角，缺少 FE 调度上下文。新增 FE 端视图：

```sql
-- information_schema.running_compactions
SELECT
    txn_id              BIGINT,
    db_name             VARCHAR,
    table_name          VARCHAR,
    partition_id        BIGINT,
    warehouse           VARCHAR,
    start_time          DATETIME,
    elapsed_seconds     DOUBLE,
    -- 任务分发信息
    num_tasks           INT,            -- CompactionTask 数
    num_tablets          INT,            -- 参与的 tablet 数
    -- 聚合进度（从 be_cloud_native_compactions 获取）
    completed_tablets   INT,
    failed_tablets      INT,
    allow_partial_success BOOLEAN
);
```

数据源：`CompactionScheduler.runningCompactions` ConcurrentHashMap。

---

### Phase 3：数据质量与诊断（低优先级）

#### 3.1 Partition Compaction 健康度视图

**灵感来源**：Snowflake `SYSTEM$CLUSTERING_INFORMATION`

```sql
-- information_schema.partition_compaction_status
SELECT
    db_name                 VARCHAR,
    table_name              VARCHAR,
    partition_id            BIGINT,
    -- 来自 PartitionStatistics
    current_version         BIGINT,
    compaction_version      BIGINT,
    versions_behind         BIGINT,         -- current_version - compaction_version
    -- Compaction Score 分位数
    compaction_score_min    DOUBLE,
    compaction_score_p25    DOUBLE,
    compaction_score_median DOUBLE,
    compaction_score_p75    DOUBLE,
    compaction_score_max    DOUBLE,
    -- 调度状态
    next_compaction_time    DATETIME,
    punishment_factor       INT,            -- 惩罚因子（partial success 累计）
    last_compaction_time    DATETIME,
    compaction_priority     VARCHAR         -- DEFAULT/MANUAL_COMPACT
);
```

数据源：`CompactionMgr.partitionStatisticsHashMap`，无需 RPC，FE 本地数据。

#### 3.2 新增 Prometheus Metrics

| Metric | 类型 | 说明 |
|--------|------|------|
| `lake_compaction_job_duration_seconds` | Histogram | compaction job 耗时分布 |
| `lake_compaction_job_total{state}` | Counter | 按状态分类的 job 计数 |
| `lake_compaction_versions_behind` | Gauge | 全局最大版本落后数 |
| `lake_compaction_queue_size` | Gauge | 等待调度的 partition 数 |
| `lake_compaction_write_amplification` | Gauge | 写放大比率 |
| `lake_compaction_in_queue_time_seconds` | Histogram | 排队等待时间分布 |
| `lake_compaction_partial_success_total` | Counter | partial success 次数 |

#### 3.3 告警规则模板

```yaml
groups:
  - name: lake_compaction_alerts
    rules:
      - alert: CompactionVersionsBehindHigh
        expr: starrocks_lake_compaction_versions_behind > 100
        for: 10m
        annotations:
          summary: "Compaction falling behind writes on {{ $labels.instance }}"

      - alert: CompactionFailureRateHigh
        expr: rate(starrocks_lake_compaction_job_total{state="FAILED"}[5m]) > 0.1
        for: 5m
        annotations:
          summary: "High compaction failure rate on {{ $labels.instance }}"

      - alert: CompactionQueueBacklog
        expr: starrocks_lake_compaction_queue_size > 1000
        for: 15m
        annotations:
          summary: "Large compaction backlog on {{ $labels.instance }}"
```

---

## 四、优先级与实施路径

```
Phase 1 (核心 — 解决最大痛点)
├─ 1.1 增强 CompactionRecord 字段        ← 基础，其他依赖此项
├─ 1.2 新增 compaction_history 系统表     ← 最高价值
└─ 1.3 增加历史容量 + 持久化              ← 20→10000，FE 重启不丢失

Phase 2 (增强实时视图)
├─ 2.1 增强 be_cloud_native_compactions   ← 补充 BE 执行细节
└─ 2.2 新增 running_compactions 视图      ← 补充 FE 调度视角

Phase 3 (数据质量与运维)
├─ 3.1 partition_compaction_status 视图   ← 利用现有 PartitionStatistics
├─ 3.2 新增 Prometheus Metrics            ← FE 端 lake compaction 指标
└─ 3.3 告警规则模板                        ← 开箱即用
```

---

## 五、竞品对比矩阵

| 能力 | ClickHouse | Snowflake | Databricks | StarRocks 现状 | 方案覆盖 |
|------|-----------|-----------|------------|--------------|---------|
| 持久化历史记录 | ✅ `part_log` | ✅ `CLUSTERING_HISTORY` | ✅ `DESCRIBE HISTORY` | ❌ 20条内存队列 | ✅ Phase 1 |
| 结构化历史查询（SQL） | ✅ | ✅ | ✅ | ❌ 仅 SHOW PROC | ✅ Phase 1.2 |
| I/O 统计（精确） | ✅ bytes/rows | ✅ bytes/rows | ✅ files/bytes | ⚠️ 截断到秒/MB | ✅ Phase 1.1 |
| 实时任务进度 | ✅ `system.merges` | ❌ 托管 | ❌ | ✅ `be_cloud_native_compactions` | ✅ Phase 2.1 |
| 数据质量指标 | ⚠️ 通过 parts 派生 | ✅ depth/overlap | ✅ p25/p50/p75 | ❌ | ✅ Phase 3.1 |
| 资源消耗追踪 | ✅ memory/merge | ✅ credits | ⚠️ DBU 预估 | ⚠️ 仅聚合 metrics | ✅ Phase 1.1 |
| 健康度/落后检测 | ⚠️ MaxPartCount | ⚠️ errors | ❌ | ❌ | ✅ Phase 3.1 |

**符号**：✅ 完善 | ⚠️ 部分 | ❌ 缺失

---

## 六、总结

StarRocks shared-data compaction 可观测性的**核心瓶颈**是：

1. **历史记录形同虚设**：默认 20 条、纯内存、FE 重启丢失、无结构化字段、精度截断
2. **FE/BE 信息割裂**：FE 有调度上下文但无执行细节，BE 有执行细节但任务完成即消失

**Phase 1 的投入产出比最高**：增强 CompactionRecord + 新建 `compaction_history` 系统表 + 扩大容量和持久化，直接解决 80% 的可观测性问题。实现复杂度不高——主要是扩展现有 `CompactionRecord` 和 `CompactionProfile`，新增一张 FE 端系统表，修改循环队列默认值。
