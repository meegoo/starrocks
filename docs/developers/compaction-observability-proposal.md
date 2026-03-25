# Shared-Data Compaction Observability Enhancement Proposal

## 背景

本文聚焦 **shared-data（存算分离）模式**的 compaction 可观测性改进方案。

核心发现：StarRocks 已有 `tablet_write_log` 机制提供 tablet 级 compaction 历史，但**默认关闭**且信息不完整。方案的核心思路是**增强并复用 `tablet_write_log`**，而非从零新建。

---

## 一、现状盘点

### 1.1 `tablet_write_log`（已有，默认关闭）

**两层存储架构**：

| 层 | 位置 | 容量 | 保留策略 |
|----|------|------|---------|
| BE 内存 | `TabletWriteLogManager` ring buffer | 10 万条（`tablet_write_log_buffer_size`） | FIFO 淘汰 |
| FE 持久化 | `_statistics_.tablet_write_log_history` | 无限制 | 按天分区，7 天自动清理 |

**同步机制**：`TabletWriteLogHistorySyncer` 每 60 秒从 `information_schema.be_tablet_write_log` 拉取增量数据写入内部表。

**当前 Schema**（17 列）：

```
BE_ID, BEGIN_TIME, FINISH_TIME, TXN_ID, TABLET_ID, TABLE_ID, PARTITION_ID,
LOG_TYPE,          -- "LOAD" 或 "COMPACTION"
INPUT_ROWS, INPUT_BYTES, OUTPUT_ROWS, OUTPUT_BYTES,
INPUT_SEGMENTS,    -- 仅 COMPACTION
OUTPUT_SEGMENTS,
LABEL,             -- 仅 LOAD
COMPACTION_SCORE,  -- 仅 COMPACTION（当前始终为 0）
COMPACTION_TYPE    -- "vertical" 或 "horizontal"
```

**记录时机**：
- `vertical_compaction_task.cpp:127` — vertical compaction 完成时
- `horizontal_compaction_task.cpp:157` — horizontal compaction 完成时
- `delta_writer.cpp:925` — load 提交时

**关键问题**：
1. ❌ **默认关闭**：`enable_tablet_write_log = false`
2. ❌ `compaction_score` 始终为 0（硬编码）
3. ❌ 仅记录成功的 compaction，**失败的不记录**
4. ❌ 无 read local/remote 分解（shared-data 核心关注点）
5. ❌ 无内存峰值使用
6. ❌ 无排队等待时间

### 1.2 `SHOW PROC '/compactions'` + `CompactionRecord`（FE 端）

FE 端 `CompactionScheduler` 维护的 partition 级历史：

| 字段 | 说明 |
|------|------|
| partitionName | `db.table.partitionId` 字符串 |
| txnId | 事务 ID |
| startTs / commitTs / finishTs | 时间戳 |
| errorMessage | 错误信息 |
| executionProfile | CompactionProfile JSON |

**关键问题**：
1. ❌ **默认仅 20 条**（`lake_compaction_history_size = 20`）
2. ❌ 纯内存，FE 重启丢失
3. ❌ 无结构化 ID（db_id, table_id, partition_id）
4. ❌ CompactionProfile 精度截断到秒/MB

### 1.3 `information_schema.be_cloud_native_compactions`（BE 端实时视图）

仅显示**当前活跃**的 compaction 任务，任务完成即消失。有 PROGRESS(0~100) 和 PROFILE。

### 1.4 已有两层 vs 竞品对比

| 层级 | ClickHouse 等价 | StarRocks 现状 | 问题 |
|------|----------------|---------------|------|
| 实时运行 | `system.merges` | `be_cloud_native_compactions` ✅ | 缺少 table_id/partition_id |
| 当前快照 | `system.parts` | ❌ 无 | — |
| 历史审计 | `system.part_log` | `tablet_write_log` ⚠️ | 默认关闭，信息不完整 |
| 调度视图 | — | `SHOW PROC '/compactions'` ⚠️ | 20 条内存队列 |

---

## 二、改进方案

### 核心思路

```
tablet_write_log（tablet 级别，BE 侧）     CompactionRecord（partition 级别，FE 侧）
         │                                          │
    ┌────┴────┐                                ┌────┴────┐
    │ 增强字段 │                                │ 增强字段 │
    │ 默认开启 │                                │ 扩大容量 │
    └────┬────┘                                │ 持久化   │
         │                                     └────┬────┘
         ▼                                          ▼
  be_tablet_write_log (实时)              compaction_history (系统表)
  tablet_write_log_history (持久化)        ← 两个视角互补 →
```

**不新建 compaction 专用的 BE 端历史表**，而是增强 `tablet_write_log` 使其覆盖 compaction 场景的全部需求。

---

### Phase 1：增强 `tablet_write_log`（高优先级）

#### 1.1 默认开启

```cpp
// be/src/common/config.h
// 改为：
CONF_mBool(enable_tablet_write_log, "true");  // 原来是 "false"
```

**理由**：tablet_write_log 的 ring buffer 开销极小（10 万条 struct，约 10MB 内存），且 FE 同步器已经有完整的持久化机制。默认关闭导致用户遇到问题时无法回溯。

#### 1.2 增加 compaction 关键字段

在 `TabletWriteLogEntry` 中新增：

```cpp
// be/src/storage/lake/tablet_write_log_manager.h
struct TabletWriteLogEntry {
    // ... 现有字段 ...

    // === 新增：shared-data I/O 分解 ===
    int64_t read_bytes_local{0};     // 本地缓存读取字节
    int64_t read_bytes_remote{0};    // 远程存储读取字节
    int64_t read_time_local_ms{0};   // 本地读耗时（毫秒）
    int64_t read_time_remote_ms{0};  // 远程读耗时（毫秒）
    int64_t write_time_remote_ms{0}; // 远程写耗时（毫秒）

    // === 新增：排队与资源 ===
    int64_t in_queue_time_ms{0};     // 在 compaction 队列中等待时间
    int64_t peak_memory_bytes{0};    // 峰值内存使用

    // === 新增：失败信息 ===
    std::string error_message;       // 失败原因（成功则为空）
    bool success{true};              // 是否成功
};
```

**数据来源**：这些字段在 `CompactionTaskContext` / `CompactStat` 中已经存在，只需在记录时传入：

```cpp
// be/src/storage/lake/vertical_compaction_task.cpp — 修改记录逻辑
TabletWriteLogManager::instance()->add_compaction_log(
    ...,
    // 新增参数：
    _context->stats->read_bytes_local,
    _context->stats->read_bytes_remote,
    _context->stats->read_time_local / 1000000,   // ns → ms
    _context->stats->read_time_remote / 1000000,
    _context->stats->write_time_remote / 1000000,
    _context->stats->in_queue_time_sec * 1000,     // sec → ms
    _context->stats->peak_memory_bytes,
    "", true  // error_message, success
);
```

**同时修改失败路径**：当前仅成功时记录。需要在 compaction 失败时也记录（带 error_message）。

#### 1.3 修复 `compaction_score` 始终为 0

当前硬编码 `0`：
```cpp
// vertical_compaction_task.cpp:133
TabletWriteLogManager::instance()->add_compaction_log(
    ..., 0, "vertical", ...);  // ← compaction_score 始终为 0
```

从 `CompactionTaskContext` 中获取实际 score：
```cpp
TabletWriteLogManager::instance()->add_compaction_log(
    ..., _context->compaction_score, "vertical", ...);
```

#### 1.4 更新系统表 Schema

```sql
-- information_schema.be_tablet_write_log 新增列
READ_BYTES_LOCAL     BIGINT,
READ_BYTES_REMOTE    BIGINT,
READ_TIME_LOCAL_MS   BIGINT,
READ_TIME_REMOTE_MS  BIGINT,
WRITE_TIME_REMOTE_MS BIGINT,
IN_QUEUE_TIME_MS     BIGINT,
PEAK_MEMORY_BYTES    BIGINT,
ERROR_MESSAGE        VARCHAR,
SUCCESS              BOOLEAN
```

**修改文件**：
- `be/src/storage/lake/tablet_write_log_manager.h/cpp` — 扩展 entry struct 和 add_compaction_log
- `be/src/storage/lake/vertical_compaction_task.cpp` — 传入新字段 + 失败路径记录
- `be/src/storage/lake/horizontal_compaction_task.cpp` — 同上
- `be/src/exec/schema_scanner/schema_be_tablet_write_log_scanner.cpp` — 填充新列
- `fe/.../BeTabletWriteLogSystemTable.java` — 新增列定义
- `fe/.../TabletWriteLogHistorySyncer.java` — 同步 SQL 增加新列

#### 1.5 典型查询场景

增强后，用户可直接用 SQL 分析 compaction：

```sql
-- 查看某表最近 1 小时的 compaction 历史
SELECT tablet_id, begin_time, finish_time,
       input_bytes, output_bytes, input_segments, output_segments,
       read_bytes_remote, read_time_remote_ms,
       compaction_type, error_message
FROM _statistics_.tablet_write_log_history
WHERE table_id = 12345
  AND log_type = 'COMPACTION'
  AND finish_time > NOW() - INTERVAL 1 HOUR
ORDER BY finish_time DESC;

-- 分析写放大
SELECT table_id,
       COUNT(*) AS compaction_count,
       SUM(input_bytes) / SUM(output_bytes) AS write_amp,
       AVG(TIMESTAMPDIFF(SECOND, begin_time, finish_time)) AS avg_duration_sec,
       SUM(read_bytes_remote) / SUM(read_bytes_local + read_bytes_remote) AS remote_read_ratio
FROM _statistics_.tablet_write_log_history
WHERE log_type = 'COMPACTION'
  AND finish_time > NOW() - INTERVAL 1 DAY
GROUP BY table_id
ORDER BY write_amp DESC;

-- 查看失败的 compaction
SELECT * FROM _statistics_.tablet_write_log_history
WHERE log_type = 'COMPACTION'
  AND success = false
  AND finish_time > NOW() - INTERVAL 1 DAY;
```

---

### Phase 2：增强 FE 端 Compaction 调度历史（中优先级）

`tablet_write_log` 是 tablet 级 BE 视角。FE 端的 partition 级调度视角同样重要。

#### 2.1 增强 `CompactionRecord`

```java
// fe/.../lake/compaction/CompactionRecord.java — 扩展字段
public class CompactionRecord {
    // === 现有字段 ===
    private final long txnId;
    private final long startTs;
    private final long commitTs;
    private final long finishTs;
    private final String partitionName;
    private final String errorMessage;
    private final String executionProfile;

    // === 新增结构化字段 ===
    private final long dbId;
    private final String dbName;
    private final long tableId;
    private final String tableName;
    private final long partitionId;
    private final String warehouse;

    // === 新增状态 ===
    private final String state;  // SUCCESS / PARTIAL_SUCCESS / FAILED

    // === 新增统计（精确值，从 CompactStat 聚合）===
    private final int inputTablets;
    private final long inputSegments;
    private final long inputBytes;
    private final long outputSegments;
    private final long outputBytes;
    private final long readBytesLocal;
    private final long readBytesRemote;
    private final long readTimeLocalMs;   // 毫秒精度
    private final long readTimeRemoteMs;
    private final long writeTimeRemoteMs;
    private final long inQueueTimeMs;
}
```

#### 2.2 新增 `information_schema.compaction_history` 系统表

数据源：`CompactionScheduler.history` 循环队列（FE 内存）。

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
    start_time          DATETIME,
    commit_time         DATETIME,
    finish_time         DATETIME,
    duration_ms         BIGINT,
    state               VARCHAR,     -- SUCCESS/PARTIAL_SUCCESS/FAILED
    error_message       VARCHAR,
    -- 聚合统计
    input_tablets       INT,
    input_segments      BIGINT,
    input_bytes         BIGINT,
    output_segments     BIGINT,
    output_bytes        BIGINT,
    -- I/O 分解
    read_bytes_local    BIGINT,
    read_bytes_remote   BIGINT,
    read_time_local_ms  BIGINT,
    read_time_remote_ms BIGINT,
    write_time_remote_ms BIGINT,
    in_queue_time_ms    BIGINT,
    -- 扩展
    profile             VARCHAR
);
```

与 `tablet_write_log` 的关系：

| 维度 | `compaction_history` | `tablet_write_log_history` |
|------|---------------------|---------------------------|
| 粒度 | partition（聚合） | tablet（明细） |
| 视角 | FE 调度 | BE 执行 |
| 关联键 | txn_id | txn_id |
| 失败记录 | ✅ 含调度失败 | ✅ Phase 1 新增 |
| 独有信息 | warehouse、commit_time、state | peak_memory、compaction_type |

```sql
-- 联合查询：找到某次失败 compaction 的 tablet 级明细
SELECT h.tablet_id, h.input_bytes, h.output_bytes, h.error_message
FROM _statistics_.tablet_write_log_history h
JOIN information_schema.compaction_history c ON h.txn_id = c.txn_id
WHERE c.state = 'FAILED'
  AND c.table_name = 'my_table';
```

#### 2.3 扩大历史容量

```java
// fe/.../common/Config.java
// 改为：
public static int lake_compaction_history_size = 10000;  // 原来是 20
```

#### 2.4 CompactionRecord 持久化到 FE Image

在 `CompactionMgr.save()` / `CompactionMgr.load()` 中增加历史记录的序列化，FE 重启后可恢复。

---

### Phase 3：数据质量与运维（低优先级）

#### 3.1 Partition Compaction 健康度视图

利用 FE 已有的 `PartitionStatistics`（无需 RPC）：

```sql
-- information_schema.partition_compaction_status
SELECT
    db_name, table_name, partition_id,
    current_version, compaction_version,
    current_version - compaction_version AS versions_behind,
    compaction_score_min, compaction_score_p25,
    compaction_score_median, compaction_score_p75, compaction_score_max,
    next_compaction_time, punishment_factor,
    compaction_priority       -- DEFAULT / MANUAL_COMPACT
```

数据源：`CompactionMgr.partitionStatisticsHashMap`。

#### 3.2 增强 `be_cloud_native_compactions`

补充当前缺失的列：

```sql
-- 新增列
TABLE_ID            BIGINT,
PARTITION_ID        BIGINT,
COMPACTION_TYPE     VARCHAR,   -- BASE/CUMULATIVE/UPDATE
INPUT_BYTES         BIGINT,
MEMORY_BYTES        BIGINT
```

#### 3.3 新增 FE 端 Prometheus Metrics

```
starrocks_fe_lake_compaction_job_duration_seconds{quantile}   Histogram
starrocks_fe_lake_compaction_job_total{state}                  Counter
starrocks_fe_lake_compaction_versions_behind                   Gauge
starrocks_fe_lake_compaction_queue_size                        Gauge
starrocks_fe_lake_compaction_write_amplification               Gauge
starrocks_fe_lake_compaction_partial_success_total             Counter
```

---

## 三、实施路径

```
Phase 1: 增强 tablet_write_log（高优先级）
├─ 1.1 默认开启 enable_tablet_write_log             [1 行改动]
├─ 1.2 新增 compaction 关键字段（I/O 分解等）         [BE 端 ~200 行]
├─ 1.3 修复 compaction_score 始终为 0                 [2 行改动]
├─ 1.4 更新系统表 Schema + 同步器                     [FE + BE ~150 行]
└─ 1.5 失败路径也记录 write_log                       [BE ~50 行]

Phase 2: 增强 FE 调度历史（中优先级）
├─ 2.1 增强 CompactionRecord 字段                     [FE ~200 行]
├─ 2.2 新增 compaction_history 系统表                  [FE ~300 行]
├─ 2.3 lake_compaction_history_size 20 → 10000        [1 行改动]
└─ 2.4 CompactionRecord 持久化到 Image                [FE ~100 行]

Phase 3: 数据质量与运维（低优先级）
├─ 3.1 partition_compaction_status 视图               [FE ~200 行]
├─ 3.2 增强 be_cloud_native_compactions               [BE + FE ~150 行]
└─ 3.3 新增 FE Prometheus metrics                     [FE ~100 行]
```

---

## 四、竞品对比（改进后）

| 能力 | ClickHouse | Snowflake | StarRocks 现状 | Phase 1 后 | Phase 1+2 后 |
|------|-----------|-----------|--------------|-----------|-------------|
| Tablet 级历史 | ✅ `part_log` | — | ❌ 关闭 | ✅ `tablet_write_log` | ✅ |
| Partition 级历史 | — | ✅ | ⚠️ 20 条 | ⚠️ 20 条 | ✅ `compaction_history` |
| I/O 分解 | ✅ | — | ❌ 截断 | ✅ 毫秒/字节精度 | ✅ |
| 失败记录 | ✅ | ✅ errors | ❌ 不记录 | ✅ | ✅ |
| 持久化 | ✅ MergeTree | ✅ Account Usage | ❌ 内存 | ✅ 7 天 | ✅ |
| SQL 查询 | ✅ | ✅ | ⚠️ SHOW PROC | ✅ | ✅ |
| 实时进度 | ✅ | ❌ | ✅ | ✅ | ✅ |
| 数据质量 | ⚠️ | ✅ depth | ❌ | ❌ | Phase 3 |

**Phase 1 即可覆盖最大痛点**：tablet 级持久化历史 + I/O 分解 + 失败记录。且实现代价最小——主要是扩展现有 `TabletWriteLogEntry` 结构体和修改记录调用点。
