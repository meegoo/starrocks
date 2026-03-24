# Compaction Observability Enhancement Proposal

## 背景

本文对比 ClickHouse、Snowflake、Databricks (Delta Lake) 和 Apache Doris 的 compaction 可观测性能力，识别 StarRocks 的差距并给出具体改进方案。

---

## 一、竞品 Compaction 可观测性对比

### 1. ClickHouse

ClickHouse 在 merge（等价于 compaction）的可观测性是业界标杆：

| 能力 | 具体实现 |
|------|---------|
| **实时 merge 监控** | `system.merges` 表：显示所有正在运行的 merge，包含 `elapsed`、`progress`(0~1)、`num_parts`、`source_part_names`、`result_part_name`、`total_size_bytes_compressed/uncompressed`、`bytes_read/written`、`rows_read/written`、`memory_usage`、`thread_id`、`merge_type` |
| **Part 生命周期追踪** | `system.parts` 表：每个 part 的创建时间、修改时间、行数、大小、merge level、partition 信息、part 状态（active/inactive/deleting） |
| **历史 merge 日志** | `system.part_log` 表：记录每次 part 的创建、merge、下载、移除事件，包含 duration、rows/bytes read/written、错误信息，可做历史趋势分析 |
| **Merge 配置诊断** | `system.merge_tree_settings` 表：所有 MergeTree 相关配置，可运行时查询 |
| **Metrics 指标** | `ClickHouseMetrics_Merge`（活跃数）、`MemoryTrackingForMerges`（内存）、`MergedRows`（行数）、`ReplicasMaxQueueMerges`（排队数）、`UncompressedBytesMergedPerSec`（吞吐） |
| **内置 Merge Dashboard** | 24.10 版本新增 `/merges` HTTP dashboard，可视化所有 merge 状态 |

**核心优势**：三层表（实时 `merges` + 快照 `parts` + 历史 `part_log`）提供完整的过去-现在视图，每个 merge 有精确进度百分比和资源消耗。

### 2. Snowflake

Snowflake 的 Automatic Clustering（等价于 compaction）采用托管模式：

| 能力 | 具体实现 |
|------|---------|
| **聚簇状态查询** | `SYSTEM$CLUSTERING_INFORMATION(table, columns)` 函数：返回 `average_overlaps`、`average_depth`、`partition_depth_histogram`、`total_partition_count`、`clustering_errors` |
| **聚簇深度查询** | `SYSTEM$CLUSTERING_DEPTH(table, columns)` 函数：返回指定列的聚簇深度 |
| **历史记录** | `ACCOUNT_USAGE.AUTOMATIC_CLUSTERING_HISTORY` 视图：每次 reclustering 的 credits 消耗、bytes/rows updated、时间戳 |
| **成本预估** | `SYSTEM$ESTIMATE_AUTOMATIC_CLUSTERING_COSTS()` 函数：预估启用自动聚簇的计算成本 |
| **状态检查** | `SHOW TABLES` 的 `AUTO_CLUSTERING_ON` 列 |

**核心优势**：以"数据质量"视角（clustering depth/overlap）而非"任务执行"视角提供可观测性；成本预估功能独特。

### 3. Databricks (Delta Lake)

| 能力 | 具体实现 |
|------|---------|
| **操作历史** | `DESCRIBE HISTORY table` 命令：记录所有操作（含 OPTIMIZE），包含 `operationMetrics`（numFilesAdded/Removed、bytes、文件大小分布 p25/p50/p75/max/min）、`operationParameters`（zOrderBy 等） |
| **返回统计** | OPTIMIZE 命令直接返回：removed/added files 统计、batches 数、partitions optimized 数 |
| **Auto Compaction** | 自动 compaction 后同样记录到 Delta Log，可通过 DESCRIBE HISTORY 审计 |
| **Predictive Optimization** | Unity Catalog 表的预测性优化，自动判断何时 OPTIMIZE 最具成本效益 |
| **Spark UI 集成** | 通过 Spark UI 查看 OPTIMIZE 的 execution plan、stages、tasks |

**核心优势**：一切操作记录在 Delta Log 中，`DESCRIBE HISTORY` 提供完整审计轨迹；文件大小分布统计（p25/p50/p75）对诊断非常有用。

### 4. Apache Doris

| 能力 | 具体实现 |
|------|---------|
| **基础 metrics** | FE/BE HTTP 端口暴露 Prometheus 格式指标 |
| **2025 路线图** | 规划统一资源管理框架，提供实时资源监控系统表，增强 compaction 可观测性 |
| **最新改进** | 3.0.5 版本加速 compaction 任务生成，新增多个 metrics |

**现状**：Doris 的 compaction 可观测性仍在早期阶段，2025 路线图才开始规划系统表。

---

## 二、StarRocks 现状评估

### 已有能力

| 类别 | 当前实现 |
|------|---------|
| **Metrics** | 30+ 指标：请求计数、字节/行处理量、任务队列深度、score、吞吐率、耗时 |
| **系统表** | `information_schema.be_compactions`（shared-nothing 汇总）、`information_schema.be_cloud_native_compactions`（shared-data 任务明细，含 progress 和 JSON profile） |
| **PROC** | `SHOW PROC '/compactions'`（cloud-native 事务历史） |
| **REST API** | 5 个 HTTP 端点：show/run/running/repair 等 |
| **配置参数** | 60+ 参数（BE + FE） |
| **Profile** | Cloud-native compaction 有 `CompactionProfile`（read/write local/remote 时间和大小） |

### 差距分析

| 差距 | 对比竞品 | 严重程度 |
|------|---------|---------|
| **缺少 compaction 历史记录表** | ClickHouse `system.part_log`、Databricks `DESCRIBE HISTORY` | 🔴 高 |
| **缺少 part/rowset 生命周期视图** | ClickHouse `system.parts` | 🔴 高 |
| **shared-nothing 模式缺少单任务进度** | ClickHouse `system.merges` 的 progress 列 | 🔴 高 |
| **缺少数据质量指标** | Snowflake 的 clustering depth/overlap 概念 | 🟡 中 |
| **缺少文件大小分布统计** | Databricks 的 p25/p50/p75 文件大小 | 🟡 中 |
| **缺少可视化 Dashboard** | ClickHouse 的内置 /merges dashboard | 🟡 中 |
| **缺少成本/资源归因** | Snowflake 的 credit 追踪、ClickHouse 的 memory_usage per merge | 🟡 中 |
| **缺少 compaction 健康诊断** | 自动判断 compaction 是否跟上写入速度 | 🟡 中 |
| **REST API 信息分散** | 需要查多个接口才能看到全貌 | 🟢 低 |

---

## 三、改进方案

### Phase 1：核心系统表（高优先级，预计 4~6 周）

#### 1.1 新增 `information_schema.compaction_history` 系统表

**目标**：等价于 ClickHouse `system.part_log` + Databricks `DESCRIBE HISTORY`

```sql
CREATE VIEW information_schema.compaction_history AS
SELECT
    be_id              BIGINT,         -- 执行节点
    table_id           BIGINT,         -- 表 ID
    table_name         VARCHAR,        -- 表名
    db_name            VARCHAR,        -- 数据库名
    partition_id       BIGINT,         -- 分区 ID
    tablet_id          BIGINT,         -- tablet ID
    compaction_type    VARCHAR,        -- BASE/CUMULATIVE/UPDATE/FULL
    start_time         DATETIME,       -- 开始时间
    finish_time        DATETIME,       -- 结束时间
    duration_ms        BIGINT,         -- 耗时（毫秒）
    state              VARCHAR,        -- SUCCESS/FAILED/CANCELLED
    -- 输入输出统计
    input_rowsets       INT,           -- 输入 rowset 数
    input_rows          BIGINT,        -- 输入行数
    input_bytes         BIGINT,        -- 输入字节数
    input_segments      INT,           -- 输入 segment 数
    output_rowsets      INT,           -- 输出 rowset 数
    output_rows         BIGINT,        -- 输出行数
    output_bytes        BIGINT,        -- 输出字节数
    output_segments     INT,           -- 输出 segment 数
    -- 资源消耗
    peak_memory_bytes   BIGINT,        -- 峰值内存
    read_bytes_local    BIGINT,        -- 本地读取
    read_bytes_remote   BIGINT,        -- 远程读取
    write_bytes_local   BIGINT,        -- 本地写入
    write_bytes_remote  BIGINT,        -- 远程写入
    -- 诊断信息
    compaction_score    DOUBLE,        -- compaction score
    error_message       VARCHAR,       -- 错误信息
    profile             JSON           -- 详细 profile（扩展字段）
);
```

**实现要点**：
- BE 端增加 `CompactionHistoryTracker`，环形缓冲区保存最近 N 条（可配置，默认 10000）compaction 记录
- 通过 SchemaScanner 暴露给 FE
- 配置参数：`compaction_history_capacity`（默认 10000）、`compaction_history_retention_seconds`（默认 86400）

**对应文件**：
- `be/src/exec/schema_scanner/schema_compaction_history_scanner.h/cpp`（新增）
- `be/src/storage/compaction_history_tracker.h/cpp`（新增）
- `be/src/storage/compaction_task.cpp`（在任务完成时记录历史）
- `fe/fe-core/.../information_schema/`（注册新表）
- `gensrc/thrift/FrontendService.thrift`（新增 schema 定义）

#### 1.2 新增 `information_schema.rowsets` 系统表

**目标**：等价于 ClickHouse `system.parts`，提供 rowset 级别的生命周期视图

```sql
CREATE VIEW information_schema.rowsets AS
SELECT
    be_id               BIGINT,
    table_id            BIGINT,
    table_name          VARCHAR,
    db_name             VARCHAR,
    partition_id        BIGINT,
    tablet_id           BIGINT,
    rowset_id           VARCHAR,        -- rowset 唯一标识
    version             BIGINT,         -- 数据版本
    start_version       BIGINT,         -- 起始版本
    end_version         BIGINT,         -- 结束版本
    num_segments        INT,            -- segment 数
    num_rows            BIGINT,         -- 行数
    data_size_bytes     BIGINT,         -- 数据大小
    index_size_bytes    BIGINT,         -- 索引大小
    creation_time       DATETIME,       -- 创建时间
    -- 来源信息
    source_type         VARCHAR,        -- LOAD/COMPACTION/SCHEMA_CHANGE
    compaction_level    INT,            -- merge level（0=原始导入）
    -- 状态
    state               VARCHAR,        -- VISIBLE/PENDING/DELETE
    is_overlapping      BOOLEAN         -- 是否与其他 rowset 有 key 范围重叠
);
```

**实现要点**：
- BE 端通过 SchemaScanner 直接从 TabletMeta 读取当前 rowset 信息
- 不需要额外存储，实时查询
- 支持按 tablet_id/table_id 过滤，避免全量扫描

**对应文件**：
- `be/src/exec/schema_scanner/schema_rowsets_scanner.h/cpp`（新增）
- `fe/fe-core/.../information_schema/`（注册新表）

#### 1.3 增强 `information_schema.be_compactions` — 添加单任务进度

**目标**：shared-nothing 模式也能像 `be_cloud_native_compactions` 一样查看单个 compaction 任务

```sql
-- 新增 information_schema.running_compactions
SELECT
    be_id              BIGINT,
    tablet_id          BIGINT,
    table_name         VARCHAR,
    compaction_type    VARCHAR,        -- BASE/CUMULATIVE/UPDATE
    start_time         DATETIME,
    elapsed_seconds    DOUBLE,
    progress           DOUBLE,         -- 0.0 ~ 1.0
    input_rowsets      INT,
    input_bytes        BIGINT,
    bytes_read         BIGINT,         -- 已读取
    bytes_written      BIGINT,         -- 已写入
    rows_read          BIGINT,
    rows_written       BIGINT,
    peak_memory_bytes  BIGINT,
    compaction_score   DOUBLE
```

**对应文件**：
- `be/src/storage/compaction_task.h/cpp`（增加进度追踪字段）
- `be/src/exec/schema_scanner/schema_running_compactions_scanner.h/cpp`（新增）

---

### Phase 2：数据质量与诊断（中优先级，预计 3~4 周）

#### 2.1 Tablet 数据质量视图

**灵感来源**：Snowflake `SYSTEM$CLUSTERING_INFORMATION`

```sql
-- 新增 information_schema.tablet_compaction_status
SELECT
    be_id                   BIGINT,
    tablet_id               BIGINT,
    table_name              VARCHAR,
    partition_id            BIGINT,
    -- 数据质量指标
    num_rowsets              INT,           -- 当前 rowset 数
    num_segments             INT,           -- 当前 segment 数
    total_rows               BIGINT,        -- 总行数
    total_data_size          BIGINT,        -- 总数据大小
    avg_segment_size         BIGINT,        -- 平均 segment 大小
    min_segment_size         BIGINT,        -- 最小 segment 大小
    max_segment_size         BIGINT,        -- 最大 segment 大小
    p50_segment_size         BIGINT,        -- P50 segment 大小
    -- Compaction 健康度
    compaction_score         DOUBLE,        -- 当前 compaction score
    overlapping_rowsets      INT,           -- key 范围重叠的 rowset 数
    max_compaction_level     INT,           -- 最大 merge level
    -- 写入追赶状态
    versions_behind          INT,           -- 未 compaction 的版本数
    last_compaction_time     DATETIME,      -- 最后一次 compaction 时间
    last_compaction_duration_ms BIGINT,     -- 最后一次 compaction 耗时
    estimated_compaction_time_ms BIGINT     -- 预估完成 compaction 所需时间
```

**实现要点**：
- 文件大小分布统计（p50/avg/min/max）参考 Databricks 的思路
- `versions_behind` 帮助判断 compaction 是否跟上写入
- `estimated_compaction_time_ms` 基于历史吞吐预估

#### 2.2 Compaction 健康诊断函数

```sql
-- 表级别 compaction 健康报告
SELECT * FROM TABLE(compaction_diagnosis('db.table'));

-- 返回：
--   tablet_id | issue_type | severity | description | recommendation
-- 例如：
--   10001 | TOO_MANY_ROWSETS | HIGH | "45 rowsets, score=120" | "增加 compaction 线程数或减少写入频率"
--   10002 | SEGMENT_TOO_SMALL | MEDIUM | "avg segment 2MB" | "增大 write batch size"
--   10003 | COMPACTION_LAG | HIGH | "120 versions behind" | "检查 BE 资源是否充足"
```

---

### Phase 3：可视化与告警（低优先级，预计 2~3 周）

#### 3.1 内置 Compaction Dashboard（BE HTTP）

**灵感来源**：ClickHouse `/merges` dashboard

增强现有 `/api/compaction/running` 端点，增加：
- 全局 compaction 吞吐率趋势（MB/s, rows/s）
- 任务队列深度趋势
- Top-N 耗时最长的 compaction 任务
- 按 table/partition 聚合的 compaction 状态
- compaction score 分布直方图

#### 3.2 新增关键 Metrics

| Metric 名称 | 类型 | 说明 |
|-------------|------|------|
| `compaction_lag_versions` | Gauge | 全局最大版本落后数 |
| `compaction_lag_seconds` | Gauge | 最旧未 compaction 版本的年龄 |
| `compaction_queue_wait_time_seconds` | Histogram | 任务在队列中等待时间分布 |
| `compaction_input_output_ratio` | Gauge | 写放大比率 |
| `compaction_memory_usage_bytes` | Gauge | 当前 compaction 总内存使用 |
| `compaction_disk_io_bytes_total` | Counter | compaction 磁盘 IO 总量 |
| `compaction_error_total` | Counter（按错误类型分） | compaction 失败分类计数 |
| `tablet_rowset_count_distribution` | Histogram | 所有 tablet 的 rowset 数分布 |
| `tablet_segment_size_distribution` | Histogram | segment 大小分布 |

#### 3.3 告警规则建议

提供开箱即用的 Prometheus AlertManager 规则模板：

```yaml
groups:
  - name: compaction_alerts
    rules:
      - alert: CompactionLagHigh
        expr: starrocks_be_compaction_lag_versions > 100
        for: 10m
        labels:
          severity: warning
        annotations:
          summary: "Compaction falling behind on {{ $labels.instance }}"

      - alert: CompactionScoreCritical
        expr: starrocks_be_tablet_max_compaction_score > 500
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Tablet compaction score too high on {{ $labels.instance }}"

      - alert: CompactionFailureRate
        expr: rate(starrocks_be_compaction_error_total[5m]) > 0.1
        for: 5m
        labels:
          severity: warning
```

---

## 四、各 Phase 优先级排序与依赖关系

```
Phase 1 (核心系统表)          Phase 2 (数据质量)          Phase 3 (可视化告警)
├─ 1.1 compaction_history ──→ 2.1 tablet_status ──────→ 3.1 Dashboard
├─ 1.2 rowsets ─────────────→ 2.2 诊断函数
└─ 1.3 running_compactions                              3.2 新 Metrics
                                                        3.3 告警模板
```

---

## 五、完整对比矩阵

| 能力 | ClickHouse | Snowflake | Databricks | Doris | StarRocks 现状 | 方案覆盖 |
|------|-----------|-----------|------------|-------|--------------|---------|
| 实时 merge/compaction 任务监控 | ✅ system.merges | ❌ 托管 | ❌ 托管 | ⚠️ 基础 | ⚠️ be_compactions（汇总） | ✅ Phase 1.3 |
| 单任务进度（progress %） | ✅ | ❌ | ❌ | ❌ | ⚠️ 仅 cloud-native | ✅ Phase 1.3 |
| Part/Rowset 生命周期查询 | ✅ system.parts | ❌ | ⚠️ Delta Log | ❌ | ❌ | ✅ Phase 1.2 |
| 历史记录/审计 | ✅ system.part_log | ✅ CLUSTERING_HISTORY | ✅ DESCRIBE HISTORY | ❌ | ⚠️ 仅 cloud-native PROC | ✅ Phase 1.1 |
| 数据质量指标 | ⚠️ 通过 parts 派生 | ✅ clustering depth | ⚠️ 文件大小分布 | ❌ | ❌ | ✅ Phase 2.1 |
| 文件大小分布 | ⚠️ 需查询 | ❌ | ✅ p25/p50/p75 | ❌ | ❌ | ✅ Phase 2.1 |
| 资源消耗追踪 | ✅ memory per merge | ✅ credits | ⚠️ Spark metrics | ❌ | ⚠️ 仅总量 metrics | ✅ Phase 1.1+3.2 |
| 健康诊断 | ⚠️ 手动分析 | ⚠️ clustering_errors | ❌ | ❌ | ❌ | ✅ Phase 2.2 |
| 内置 Dashboard | ✅ /merges | ❌ 托管 UI | ❌ Spark UI | ❌ | ❌ | ✅ Phase 3.1 |
| 告警模板 | ⚠️ 社区提供 | ❌ 托管 | ❌ | ❌ | ❌ | ✅ Phase 3.3 |

**符号说明**：✅ = 完善支持 | ⚠️ = 部分支持 | ❌ = 不支持

---

## 六、总结

StarRocks 当前的 compaction 可观测性在 **metrics 维度**已经较为丰富（30+ 指标），但在 **系统表维度** 存在明显差距：

1. **最大差距**：缺少 compaction 历史记录表和 rowset 生命周期视图，这是 ClickHouse 和 Databricks 的核心能力
2. **shared-nothing 模式落后于 shared-data**：cloud-native 已有任务级进度和 profile，但传统模式只有汇总信息
3. **缺少数据质量视角**：没有类似 Snowflake 的 clustering depth 或 Databricks 的文件大小分布

建议优先实施 **Phase 1**（核心系统表），这将覆盖最大的可观测性差距，使 StarRocks 达到与 ClickHouse 对等的 compaction 可观测性水平。
