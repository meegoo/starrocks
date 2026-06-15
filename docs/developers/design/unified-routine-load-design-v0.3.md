# 统一 Routine Load 概要设计文档

> **版本**: v0.3 (Draft)
>
> **状态**: 概要设计（基于 v0.2 的代码实证评审修订）
>
> **本版相对 v0.2 的关键变化**
> 1. **执行模型**：从"每批次重新解析 + 全量 CBO + 走 TaskManager"改为 **compile-once / rebind-per-batch + 专用低延迟提交路径**（§4、§8）。
> 2. **延迟目标**：`target_e2e_latency` 改为**尽力而为的预期值**而非硬 SLA；删除固定 `0.6/0.1/0.3` 预算切分，改为**自适应降级**（§3、§10）。
> 3. **版本/compaction 压力**：与延迟收敛到**同一个降级杠杆**——压力上升时自动加大 batch / 降频（§10）。
> 4. **PK 表写语义**：`__op`（行级 UPSERT/DELETE）、`merge_condition`、`partial_update` **对齐现有 Routine Load**，给出可落地机制（§12）。
> 5. **错误容忍与 Schema**：对齐 Routine Load 的两层错误门（RATIO 原生复用 + ABSOLUTE 净新增）；**无法对齐项见附录 C**（§13）。
> 6. **凭证安全**：结构化抽取 + `originSql` 占位符 + 渲染处脱敏，**提前到 Phase 1 GA blocker**（§17）。
> 7. **诚实化"复用"**：`Pipe.Type` 确已预留 `KAFKA`，但 `PipeSource/PipePiece` 多态、consumer 池配置、`SET_VAR(parallelism)`、状态机 `FINISHED` 等表述按代码事实更正（§6、附录 C）。

---

## 目录

1. [背景与动机](#1-背景与动机)
2. [业界调研（摘要）](#2-业界调研摘要)
3. [设计目标与性能指标](#3-设计目标与性能指标)
4. [整体架构](#4-整体架构)
5. [SQL 语法设计](#5-sql-语法设计)
6. [核心模块设计（含净新增标注）](#6-核心模块设计含净新增标注)
7. [FE-BE 接口设计](#7-fe-be-接口设计)
8. [执行模型：compile-once / rebind-per-batch](#8-执行模型compile-once--rebind-per-batch)
9. [并发模型与动态并行度](#9-并发模型与动态并行度)
10. [自适应降级与延迟/版本预算](#10-自适应降级与延迟版本预算)
11. [Offset 管理与 Exactly-Once 语义](#11-offset-管理与-exactly-once-语义)
12. [PK 表写语义：__op / partial_update / merge_condition](#12-pk-表写语义__op--partial_update--merge_condition)
13. [错误容忍与 Schema/格式对齐](#13-错误容忍与-schema格式对齐)
14. [Kafka Consumer 连接管理](#14-kafka-consumer-连接管理)
15. [反压与流控](#15-反压与流控)
16. [资源隔离与过载保护](#16-资源隔离与过载保护)
17. [安全性：凭证管理](#17-安全性凭证管理)
18. [状态机与生命周期管理](#18-状态机与生命周期管理)
19. [存算分离（Shared-Data）适配](#19-存算分离shared-data适配)
20. [兼容性设计](#20-兼容性设计)
21. [可观测性](#21-可观测性)
22. [配置参数](#22-配置参数)
23. [实现路线图](#23-实现路线图)
24. [附录 A：业界方案对比矩阵](#附录-a业界方案对比矩阵)
25. [附录 B：CREATE ROUTINE LOAD 属性兼容映射表](#附录-bcreate-routine-load-属性兼容映射表)
26. [附录 C：无法对齐 / 净新增清单（请 review）](#附录-c无法对齐--净新增清单请-review)

---

## 1. 背景与动机

### 1.1 现有 Routine Load 的问题

| 问题 | 描述 |
|------|------|
| **手动 Task 拆分** | 用户需指定 `desired_concurrent_number`，FE 按 Kafka partition 静态 round-robin 拆 Task，无法按实际吞吐动态调整。 |
| **独立执行路径** | 走 `StreamLoadPlanner` + `RoutineLoadTaskExecutor` 专用路径，与标准 INSERT 不同，维护成本高。 |
| **无法利用 MPP 并行** | 每个 Task 只在单 BE 执行，不能像标准 INSERT 那样在多 BE 上 MPP 并行。 |
| **静态并行度** | 并行度在 Job 创建后固定，无法按实时吞吐自适应。 |
| **与 Pipe 框架割裂** | Pipe 框架已实现基于 INSERT 的持续加载（仅 FILE 源）；`Pipe.Type` 枚举**已预留 `KAFKA`**（`Pipe.java:829`），但 Kafka 源尚未打通。 |

### 1.2 期望的改进

将 Routine Load 统一为一种**流式 Pipe**：标准 `INSERT INTO ... SELECT FROM kafka(...)` 语法、复用 INSERT 执行体（MPP）、动态并行度、完全兼容现有 Routine Load 能力、统一管理监控。

> **就绪度修正（来自代码核实）**：v0.2 把多处能力描述为"复用现有扩展点"，但代码事实是：
> - ✅ `Pipe.Type` 确实含 `KAFKA`（`Pipe.java:829`）。
> - ❌ **没有** `PipeSource` 基类（`FilePipeSource implements GsonPostProcessable`），`FilePipePiece` **未继承**抽象 `PipePiece`，`buildNewTasks` 有 FILE-only assert。→ source/piece 多态是**净新增**。
> - ❌ **没有** `kafka()` TVF（仅 `files()`/`TableFunctionTable`）。整个 kafka() 源是净新增。
> 详见附录 C「净新增清单」。

---

## 2. 业界调研（摘要）

| 系统 | 模型 | 并行度 | 动态调整 | 语义 |
|------|------|--------|----------|------|
| ClickHouse | Kafka Engine + MV | `num_consumers`（≤核数） | 否（改表重建） | 默认 at-least-once，实验性 exactly-once |
| Databricks | `read_kafka` TVF + Streaming Table | partition→task | 不推荐（需 Lakeflow） | Delta exactly-once |
| Snowflake | Snowpipe Streaming | serverless 自动扩缩 | 是 | 默认 exactly-once |
| Flink SQL | `CREATE TABLE` + 连续 INSERT | `scan.parallelism` | Reactive/Autopilot | Kafka txn + checkpoint exactly-once |

**设计启发**：标准 INSERT 语法驱动流式摄入（Flink）、服务端自动扩缩（Snowflake）、TVF 封装数据源（Databricks/Flink）、声明式 Pipe 管理（Snowflake + 现有 Pipe）、txn 保证 exactly-once。详见附录 A 完整对比。

---

## 3. 设计目标与性能指标

### 3.1 核心目标

1. **统一框架**：Routine Load 统一为 `Pipe.Type.KAFKA`。
2. **标准语法**：`INSERT INTO ... SELECT FROM kafka(...)`。
3. **MPP 并行**：单 INSERT 在多 BE MPP 并行，不再手动拆 Task。
4. **动态并行度**：按实时吞吐 + 资源 + 版本压力自适应。
5. **完全兼容**：兼容现有 `CREATE ROUTINE LOAD` 语法与能力（含 PK 表 `__op`/partial-update、错误容忍语义）。

### 3.2 性能指标（预期目标，尽力而为）

> **重要语义变更**：`target_e2e_latency` 是用户声明的**预期/目标延迟**，**不是硬 SLA**。系统尽力达成；当 commit/publish/compaction 跟不上时，pipe **自动降级**（加大 batch、降低频率，§10），并通过 `SHOW PIPES` 暴露**实际有效延迟** `EFFECTIVE_E2E` 与降级原因，而不是制造一个存储层无法兑现的承诺。

| 指标 | 目标 | 说明 |
|------|------|------|
| **端到端延迟（预期）** | 用户声明 `target_e2e_latency`，默认 `1s`，可请求低至 `100ms` | 达成与否取决于表宽（tablet 数）、存储后端、集群负载、compaction 余量。系统按实测自适应；达不到时降级而非违约。 |
| **有效延迟（实测）** | `SHOW PIPES.EFFECTIVE_E2E` | 真实可达延迟。shared-nothing 窄表低负载下可接近 100ms；shared-data 宽表/高负载下通常落在 1–5s（publish 慢日志阈值本身即 `lake_publish_version_slow_log_ms=1000`）。 |
| **吞吐量** | 高于现有 Routine Load | MPP 并行 + 动态并行度。 |
| **降级响应** | ≤ 数个批次周期 | 检测到 commit/publish 变慢或版本/compaction 压力后，下几个批次内放大 batch、抬高有效延迟。 |

### 3.3 非目标（当前版本）

- **Pulsar 源**：现有 `PulsarRoutineLoadJob` 已上线，但本版 `kafka()` TVF 只覆盖 KAFKA 源；Pulsar 作业**继续留在 legacy 引擎**（§20.4），不被 `enable_unified_routine_load` 接管。
- **Protobuf 格式**：StarRocks 现今**任何路径都不支持 protobuf**（Routine Load 仅 csv/json/avro）。v0.2 反复出现的"avro/protobuf"为**事实错误**，本版移除 protobuf（如需为后续独立 feature）。
- 跨集群 Kafka 复制；CDC changelog 通用框架（`envelope=debezium` 走兼容映射，见附录 B）。

---

## 4. 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│ 用户接口层                                                              │
│   CREATE PIPE my_pipe [PROPERTIES(...)] AS                             │
│   INSERT INTO target_table [(cols)] SELECT ... FROM kafka(...) [WHERE] │
│   兼容层：CREATE ROUTINE LOAD → 内部转换为 Kafka Pipe                   │
└───────────────────────────────┬──────────────────────────────────────┘
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│ FE 调度层（leader-only）                                               │
│   PipeManager → KafkaPipeScheduler → KafkaPipeSource                   │
│     - partition 发现 / committedOffsets（commit-attachment 权威）       │
│     - 动态并行度 + 自适应降级控制器                                     │
│     - 编译一次的 ExecPlan 模板缓存（compile-once）                      │
│   提交：专用低延迟提交器（事件驱动，绕开 TaskManager 1s tick 与 cap=4）  │
└───────────────────────────────┬──────────────────────────────────────┘
                                ▼  rebind(offsets, parallelism) → 执行 ExecPlan
┌──────────────────────────────────────────────────────────────────────┐
│ BE 执行层（MPP，标准 INSERT 执行体）                                    │
│   KafkaScanNode(分区/起始offset) → [exchange?] → OlapTableSink          │
│   单批次单事务；BE 自主消费时间窗口，commit-attachment 回报实际终止 offset│
└──────────────────────────────────────────────────────────────────────┘
```

### 4.1 核心变化（修正版）

| 组件 | 现有 Routine Load | 统一后（v0.3） |
|------|-------------------|----------------|
| 用户语法 | `CREATE ROUTINE LOAD ... FROM KAFKA(...)` | `CREATE PIPE ... AS INSERT INTO ... SELECT FROM kafka(...)` |
| FE 调度 | `RoutineLoadScheduler` + `RoutineLoadTaskScheduler` | `PipeScheduler` + `KafkaPipeScheduler` + **专用提交器** |
| 任务拆分 | FE 按 partition round-robin 拆 N Task | FE 生成单 INSERT，MPP 把 `KafkaScanNode` 分布到多 BE |
| 执行规划 | `StreamLoadPlanner.do_plan`（绕过 CBO，固定 fragment） | **compile-once**：每个 pipe 编译一次 ExecPlan 模板，每批只 rebind offset/并行度（§8） |
| 并行度 | 静态 | 动态（自适应，含降级，§9/§10） |
| 事务 | N Task × N txn | 1 批次 × 1 txn |

**事务模型分析（修正）**：
- **单次重试代价**：新模型单批次窗口（默认贴近 `target_e2e_latency`，远小于旧模型的 `DEFAULT_TASK_SCHED_INTERVAL_SECOND=10s` 默认）确实更小——这点 v0.2 正确，保留。
- **稳态版本创建率（v0.2 遗漏）**：每个提交的微批至少给每个 tablet 产生一个 rowset/版本。**版本创建率由批次频率而非批次大小决定**。把 10s 批次降到亚秒会让版本/rowset 创建率放大 10–100×。这引出两道硬墙：
  - **Shared-nothing**：`tablet_max_versions=1000`（`config.h:991`）硬上限；超限报错的官方补救是"调大、降频"——与盲目降延迟相反。
  - **Shared-data**：`CommitRateLimiter` **默认开**（`lake_enable_ingest_slowdown=true`），compaction score >100 按 `writeDuration*(score-100)*0.1` 延迟 commit，>2000 直接拒绝。
- **结论**：批次频率必须受**版本/compaction 预算**约束。本版用 §10 的自适应降级统一处理：稳态下保证 `rowset 到达率 ≤ compaction 排空率`，压力升高时自动加大 batch、抬高有效延迟。

---

## 5. SQL 语法设计

### 5.1 新语法（推荐）

```sql
CREATE PIPE [IF NOT EXISTS] [db.]pipe_name
[PROPERTIES (
    "target_e2e_latency" = "1s",      -- 预期延迟（默认 1s，可请求低至 100ms；尽力而为）
    "auto_parallelism"   = "true",
    "max_parallelism"    = "16",
    "warehouse"          = "default_warehouse",
    -- 错误容忍（对齐 Routine Load 语义，见 §13）
    "max_error_number"   = "0",        -- 绝对错误行数；0 = 零容忍（与旧语义一致，见 §13/附录C）
    "max_filter_ratio"   = "1.0",      -- 比例门；默认 1.0（对齐 RL，见 §13 默认翻转告警）
    "strict_mode"        = "false",
    -- PK 表写语义（见 §12）
    "enable_op_column"   = "false",    -- 是否启用行级 __op（UPSERT/DELETE）
    "merge_condition"    = "",
    "partial_update"     = "false",
    "partial_update_mode"= "row"
)]
AS INSERT INTO target_table [ (col1, col2, ...) ]
SELECT col1, col2, ...
FROM kafka(
    "broker_list" = "host1:9092,host2:9092",
    "topic"       = "my_topic",
    "group_id"    = "starrocks_group",      -- 可选，见 §14 group.id 策略
    "format"      = "json",                   -- json / csv / avro / raw（无 protobuf）
    "partitions"  = "0,1,2,3",               -- 可选，默认全部
    "offsets"     = "OFFSET_BEGINNING",       -- 仅作初始 offset，运行后以 commit-attachment 为权威
    "confluent.schema.registry.url" = "...",  -- avro 必填
    "property.security.protocol" = "SASL_PLAINTEXT",
    "property.sasl.mechanism"    = "PLAIN"
    -- 凭证（property.sasl.* / property.ssl.*）由 FE 抽取后脱敏存储，见 §17
)
[ WHERE condition ];

ALTER PIPE pipe_name SUSPEND | RESUME | SET ("max_parallelism"="32");
DROP PIPE [IF EXISTS] pipe_name;
SHOW PIPES [FROM db] [LIKE 'pattern'];
```

### 5.2 kafka() TVF 设计

`kafka()` 是**净新增** TVF（不存在于现有代码，只有 `files()`）。它作为 Kafka 数据源的 SQL 抽象，**复用 BE 现有 json/csv/avro scanner**。

**Schema 策略（v0.2 措辞修正）**：必须是 **"命名 SELECT/COLUMNS 投影下推"**，不是笼统的"目标表 schema 下推"。原因：BE 的 JSON auto-key-match 是按**目标 slot 的列名**匹配（`json_scanner.cpp:611`），而这些 slot 来自 SELECT/COLUMNS 投影。若生成的 INSERT 用 `SELECT *` 或位置引用，JSON 自动按名匹配会静默失效。

| format | Schema 确定 | 与现有行为对齐 |
|--------|------------|----------------|
| **json（无 jsonpaths）** | 投影列名 → JSON 顶层 key 自动匹配；缺失列填 NULL，多余 key 跳过 | 与现有 RL 一致（`json_scanner` 按 slot col_name 匹配） |
| **json（有 jsonpaths）** | 第 i 个 jsonpath → 第 i 个投影列；支持嵌套 `$.a.b` | 与现有 RL 一致 |
| **csv** | 位置映射，`column_separator`（来自旧 `COLUMNS TERMINATED BY`）+ `trim_space/enclose/escape` | 与现有 RL + `files()` csv 一致 |
| **avro** | `confluent.schema.registry.url` 注册表推导，按名匹配 | **net-new BE 路径**：RL 在 BE 从 Kafka 原始字节 + registry 解析（`KafkaTaskInfo.java`），与 `files()` 的 avro 容器文件 scanner **不同**（见附录 C） |
| **raw** | 固定 6 列 `_key/_value/_topic/_partition/_offset/_timestamp`，用户在 SELECT 自行解析 | net-new，类 Databricks `read_kafka` |

> **优先级规则**：`format='raw'` 用固定 schema（不下推）；其余格式用命名投影下推。

所有格式均可通过 SELECT 表达式做列派生/类型转换，对齐 RL `COLUMNS(col1, col2, col3=expr)`。`__op` 作为可选的尾列投影（§12）。

### 5.3 兼容语法（CREATE ROUTINE LOAD → Kafka Pipe）

现有 `CREATE ROUTINE LOAD` 在 FE 解析后内部转换为等价 Kafka Pipe。完整属性映射见**附录 B**；转换层对**无法映射的属性 fail-fast**（绝不静默降级），对 `__op`/`merge_condition`/`partial_update` 见 §12，对错误容忍默认翻转见 §13。

兼容层保证 `SHOW / PAUSE / RESUME / STOP / ALTER ROUTINE LOAD` 继续工作（映射到 Pipe 操作 + 展示格式转换，列映射见 §20.3）。

---

## 6. 核心模块设计（含净新增标注）

> 标注 **[新增]** = 代码中不存在，需从零实现；**[扩展]** = 在现有类上加能力；**[复用]** = 直接用。

### 6.1 FE 侧

```
com.starrocks.load.pipe/
├── PipeSource.java            # [新增] 抽象基类/接口（当前无；FilePipeSource 仅 implements GsonPostProcessable）
├── PipePiece.java             # [复用] 抽象类已存在；但 FilePipePiece 需改为继承它 [扩展]
├── Pipe.java                  # [扩展] Type.KAFKA 已预留；去掉 buildNewTasks 的 FILE-only assert
├── PipeManager / PipeScheduler# [扩展] Kafka 生命周期 + 调度
├── KafkaPipeSource.java       # [新增] partition 发现、committedOffsets、动态并行度、降级控制器、绝对错误窗口
├── KafkaPipePiece.java        # [新增] 单批次：partition→起始 offset + 并行度 + 消费窗口
├── KafkaPipePlanCache         # [新增] 每 pipe 编译一次的 ExecPlan 模板缓存（§8）
└── KafkaPipeSubmitter         # [新增] 专用低延迟提交器（绕开 TaskManager 1s tick / cap=4，§8）

com.starrocks.sql.ast/
├── KafkaTableFunctionRelation # [新增] kafka() TVF AST 节点（toSql() 构造即脱敏，§17）
com.starrocks.catalog/
├── KafkaTableFunction         # [新增] kafka() TVF 元信息（类比 TableFunctionTable）
com.starrocks.planner/
├── KafkaScanNode              # [新增] FE 计划节点：partition→instance 分配、起始 offset、并行度
└── InsertPlanner              # [扩展] PK 表 + 启用 op 列时追加尾部 __op slot（§12）
```

### 6.2 BE 侧

```
be/src/exec/
├── kafka_scan_node.cpp/.h     # [新增] 从 DataConsumerPool 取/还 consumer；seek；时间窗口消费；解析；回报实际 offset + 行数/字节/过滤数
be/src/runtime/routine_load/
├── data_consumer*.cpp         # [扩展] 池：可配 cap、按 (broker,topic) 分桶、group.id 移出 match key、修复 idle 回收（§14）
be/src/exec/file_scanner/      # [复用] json/csv/avro scanner；[新增] avro+schema-registry from-bytes 路径
```

> **无需 BE/thrift 改动的关键点**：`__op` 通过"输出 tuple 尾列名为 `__op`"的**位置约定**激活（`tablet_sink.cpp:814`、`memtable.cpp:104`），BE 侧已完整实现。FE 产出该尾列即可点亮（§12）。

---

## 7. FE-BE 接口设计

### 7.1 TKafkaScanNode（新增）

```thrift
struct TKafkaScanNode {
    1: required string broker_list
    2: required string topic
    3: required map<i32, i64> partition_start_offsets
    4: required string format                          // json/csv/avro/raw
    5: optional map<string, string> kafka_properties   // 透传（凭证已在 FE 解密注入）
    6: optional i64 max_batch_rows
    7: optional i64 consume_timeout_ms                 // 消费窗口（FE 从有效延迟推导，§10）
    8: optional string jsonpaths
    9: optional string json_root
    10: optional bool strip_outer_array
    11: optional string confluent_schema_registry_url
    12: optional string column_separator
    13: optional string enclose
    14: optional string escape
    15: optional bool trim_space
}
```

### 7.2 上报实际消费结果 + 错误统计

> v0.2 的 `TKafkaConsumeReport` 不够：INSERT 路径今天**只把 loadedRows 落进 `InsertTxnCommitAttachment`**，丢弃 filtered/unselected。为支持绝对 `max_error_number`（§13），扩展 commit-attachment（**全部 optional，不复用序号**）：

```thrift
struct TKafkaConsumeReport {
    1: required map<i32, i64> partition_end_offsets    // 实际消费终止 offset (exclusive)
    2: required i64 total_rows_loaded
    3: optional i64 total_rows_filtered                // 新增：错误行
    4: optional i64 total_rows_unselected              // 新增：被 WHERE 过滤
    5: optional i64 total_bytes_received               // 新增
    6: optional i64 consume_duration_ms
    7: optional string tracking_url                    // 错误样本（load_tracking_logs）
}
```

`StmtExecutor` 需额外读取 BE 已上报但当前未消费的 `unselected.rows`（`exec_state_reporter.cpp:117`），并把上述字段写入 commit-attachment，供 FE 在 `afterCommitted` + **edit-log replay** 路径读取（§11）。

### 7.3 TPlanNode 扩展

`TPlanNode` union 增加 `optional TKafkaScanNode kafka_scan_node`；`TPlanNodeType` 增加 `KAFKA_SCAN_NODE`。

---

## 8. 执行模型：compile-once / rebind-per-batch

### 8.1 为什么不每批次走 TaskManager（回答 P0-1）

代码事实：
- TaskManager 派发循环是 `scheduleAtFixedRate(..., 0, 1, TimeUnit.SECONDS)`（`TaskManager.java:142`），**1s tick 硬编码、非 Config**。
- 并发受 `task_runs_concurrency=4`（`TaskRunScheduler.java:127`）**FE 全局上限**约束，与 MV refresh / `SUBMIT TASK` 共享。
- `PipeScheduler` 的 interval 在构造时读一次（`PipeScheduler.java:42`），运行期不变。
- **没有 INSERT plan cache**：`SqlTaskRunProcessor` 每次重解析（`SqlTaskRunProcessor.java:50`）并跑完整 CBO；`PrepareStmtPlanner` 仅服务 point-query。

**关于"能否把 TaskManager tick 调低"**：可以把 1s tick 改成 Config（小改动），但**不够**——`task_runs_concurrency=4` 的全局并发上限和与 MV refresh 的 FCFS 争用才是硬瓶颈，全局调低 tick 会放大整集群的 FE 唤醒与 TaskRun 历史写入。因此本版**不沿用 TaskManager 的逐批派发**。

### 8.2 v0.3 执行模型

```
pipe 激活 / 并行度变更 / schema 变更时（不频繁）:
  1. KafkaPipeSource 生成一次 INSERT...SELECT FROM kafka() 的 ExecPlan 模板
     （走标准 Transformer + CBO + PlanFragmentBuilder 一次），缓存到 KafkaPipePlanCache。
  2. 失效条件（必须 bust 缓存）：目标表 schema 变更、partition 集变更、存活节点集变更、并行度变更。

每批次（高频）:
  3. KafkaPipeSource.pullPiece() 计算起始 offset + 并行度 + 消费窗口（含降级，§10）。
  4. rebind：把缓存 ExecPlan 的 KafkaScanNode 起始 offset / 消费窗口 / label 重绑，
     不重新解析、不重跑 CBO。
  5. KafkaPipeSubmitter 通过专用执行器调用 StmtExecutor 的 DML 执行入口
     （prebuilt ExecPlan，类 handleDMLStmtWithProfile），绕开 TaskManager 1s tick 与 cap=4。
  6. 标准 INSERT 执行体：KafkaScanNode → [exchange?] → OlapTableSink → 单事务 commit。
  7. commit-attachment 回报实际终止 offset + 行/字节/过滤统计；FE onBatchComplete 更新。
  8. 回到步骤 3。流水线：批次 N commit 时即可启动批次 N+1 的消费（Phase 1 必需，见 §23）。
```

> **专用提交器的资源约束**：见 §16——给 Kafka pipe 一条独立的并发车道（独立计数/优先级），既不被 MV refresh 饿死，也不淹没共享队列。

### 8.3 与现有 Pipe (FILE) 的复用程度（修正）

| 组件 | FILE Pipe | KAFKA Pipe | 复用 |
|------|-----------|-----------|------|
| `PipeManager` | ✓ | ✓ | 完全复用 |
| `PipeScheduler` | ✓ | 扩展 Kafka 逻辑 | 扩展 |
| `PipeSource`(抽象) | — **当前无基类** | `KafkaPipeSource` | **需先新增基类** |
| `PipePiece`(抽象) | `FilePipePiece` **未继承** | `KafkaPipePiece` | 需补继承 |
| 执行体（INSERT/MPP） | TaskManager 逐批 | **专用提交器 + plan cache** | 执行体复用，调度路径**新增** |
| `Pipe.State` | SUSPEND/RUNNING/**FINISHED**/ERROR | 同左（Kafka 永不 FINISHED） | 复用（v0.2 称"无 FINISHED"有误） |

---

## 9. 并发模型与动态并行度

### 9.1 新并发模型

单 INSERT 由 MPP 把 `KafkaScanNode` 的 partition 分布到多 BE fragment instance（`Instance-k(BE-x): Pi,Pj`），与 `OlapTableSink` 组成执行计划，单事务全局 commit。并行度 = 跨 BE 的 scan-range 分配（即 fragment instance 数），**不是** `pipeline_dop`。

> **修正**：v0.2 的 `/*+ SET_VAR(parallelism=6) */` 中 `parallelism` **不是合法 session 变量**（plan 期抛 `ERR_UNKNOWN_SYSTEM_VARIABLE`）。跨 BE 分布由 `KafkaScanNode` 的 partition→instance 分配 + BackendSelector 决定；如需 hint 用 `pipeline_dop` / `parallel_fragment_exec_instance_num`，但二者都不决定 BE 台数。

### 9.2 动态并行度算法（修正版）

输入信号：partition 数、存活节点数、`max_parallelism`、上批吞吐（行/字节）、当前 lag、lag 速率、`target_lag = EWMA(throughput) * target_e2e_latency`、**per-node** BE CPU/Mem（带新鲜度门）、**compaction score / commit 延迟**（§10）、上次扩缩有效性、冷却期。

修正点（相对 v0.2）：
1. **资源信号新鲜度门 + per-node max**：只在 `isResourceUsageFresh()==true` 的节点上判断；用 **per-node max** 而非集群均值（单台打满即阻止扩容）；`memLimitBytes==0`（首次心跳前）视为未知而非 0%。leader 换届/BE 重启后至少保持一个新鲜周期（默认 5s）再信任。
2. **并行度变更非免费**：变更触发 plan cache 失效（重编译一次）+ partition 落新 BE 的冷 consumer。引入 **stickiness**（§9.3）+ **非对称滞后**（扩容 1–2 个高 lag 批次即可；缩容需多个低 lag 批次）。
3. **compaction/版本压力纳入**：新增 scale-DOWN/降频分支——score 接近 `lake_ingest_slowdown_threshold(100)` 或 commit 延迟上升时，**抬高消费窗口（降频）** 而非缩并行度（§10）。
4. **冷启动**：CREATE/RESUME/新分区时 seed `current_parallelism = min_parallelism`（默认 1，v0.2 定义了但未接入）；首 K 批 warm-up 只允许 +1（禁用 ×2 快扩），`target_lag` 在无历史时取保守绝对下限，避免 `target_lag=0` 触发狂扩。
5. **跨 pipe 公平**：每个调度 tick 对所有 RUNNING Kafka pipe **统一**采集一次 BE 资源快照并按权重/lag 份额分配 headroom，避免 N 个 controller 同时读"OK"齐刷刷扩容（thundering herd）。

### 9.3 Partition 分配（sticky）

持久化 partition→instance 映射；并行度变更时只迁移**最小** partition 集（一致性哈希/增量再均衡），而非每批按 lag 全量重排——既保暖 consumer 命中，又避免每批 churn。

---

## 10. 自适应降级与延迟/版本预算

> 本节取代 v0.2 §19.1 的固定 `0.6/0.1/0.3` 切分。延迟与版本压力收敛到**同一个降级杠杆**：**有效批次间隔/大小**。

### 10.1 预算：从固定比例到实测下限

```
E2E ≈ schedule_interval + consume_time + commit_publish_time
```
- `commit_publish_time` **不是** E2E 的固定比例。它是 `f(tablet 数, 存储后端, 对象存储 RTT, compaction score)` 的**下限**：
  - shared-data make-visible 需 publish：每 tablet 读基线 `tablet_metadata` + 写新 `tablet_metadata` + 写 `txn_log`（`transactions.cpp:186/192`、`delta_writer.cpp:897`），异步 `PublishVersionDaemon` 执行；代码自带慢阈值 `lake_publish_version_slow_log_ms=1000`。
  - 叠加默认开的 `CommitRateLimiter` 延迟。
- `effective_e2e_floor = schedule_floor + 单批 plan/commit + per-tablet publish 成本之和`，向上 clamp `target_e2e_latency`，并在 `SHOW PIPES.EFFECTIVE_E2E` 暴露（含原因）。

### 10.2 降级控制器（核心）

```
每批次完成后，除并行度外再决定"有效批次间隔/大小"：

输入：
  - 实测 commit→VISIBLE 延迟（含 CommitRateLimiter 等待）
  - 目标表 max tablet version_count（shared-nothing）/ max partition compaction score（shared-data）
  - 批次是否 commit 超时 / 被 CommitRateExceeded
  - 稳态约束：rowset 到达率 (= pipes × batches/s × tablets/batch) ≤ compaction 排空率

降级（压力上升）：
  - 若 commit 延迟 > 预期、或 score 接近 slowdown_threshold、或 version_count 接近 tablet_max_versions：
      → 提高 effective_batch_interval（降低提交频率）并增大 max_batch_rows/size（每版本攒更多行）
      → 即"牺牲延迟换版本/提交稳定"，EFFECTIVE_E2E 抬升，SHOW PIPES 给出 THROTTLED_BY_COMMIT_RATE / VERSION_PRESSURE 原因
  - 若单批 commit 超时（batch_timeout）：abort + 下批以更大 batch 重试（§15 watchdog）

恢复（压力回落）：
  - 持续多个批次 score 低、commit 快 → 逐步回收有效间隔，趋近 target_e2e_latency

硬地板：
  - 每表最小有效批次间隔 = g(tablet_max_versions / compaction 排空率, tablet 数)，
    使热表不会被推过版本墙；该地板也在 SHOW PIPES 暴露。
```

> 这样既满足"**100ms 作为预期目标不变**"——在窄表/低负载/shared-nothing 下能逼近；又满足"**超时/版本压力可降级**"——宽表/高负载/shared-data 下自动退避到可持续的有效延迟，而不是违约或把集群推过版本墙。

---

## 11. Offset 管理与 Exactly-Once 语义

### 11.1 Offset 生命周期（BE 自主消费）

FE 仅下发**起始 offset**；BE 在消费窗口内自主消费，commit-attachment 回报**实际终止 offset**。批次成功 → committedOffsets 推进；失败/abort → committedOffsets 不变，下轮从原位重消费。

### 11.2 Exactly-Once（权威与恢复，修正）

> v0.2 在 `afterCommitted/afterVisible` **回调**更新 committedOffsets，并有 10s 周期持久化 → 双权威歧义、崩溃丢进度。修正：

1. **唯一权威**：committedOffsets 的唯一持久权威是 **COMMITTED 事务的 commit-attachment（`TKafkaConsumeReport`）**，与数据原子提交（对齐现有 `RLTaskTxnCommitAttachment` + `KafkaProgress`）。
2. **恢复**：在 `afterCommitted` 实时路径 **与 edit-log replay 路径**都应用 offset（Pipe 版的 `replayOnCommitted`），leader 换届后从 replay 的 txn attachment 重建。
3. **周期持久化降级**：`pipe_kafka_offset_persist_interval_millis` 仅作派生缓存/检查点以**界定 replay 扫描长度**，**绝不**作为可覆盖 attachment 真值的权威。
4. **不依赖 Kafka consumer group offset**：StarRocks 自管 offset；group offset 仅供外部监控（可选）。

### 11.3 全错批次跳过（对齐 RL，net-new）

现有 RL 有 `NO_ROWS_IMPORTED` 特例：整批全错（txn ABORTED）也推进 committed offset 越过毒批（`KafkaRoutineLoadJob.java:367-380`）。INSERT 路径无此能力（abort 不推进 offset）。为对齐，需在 KafkaPipeSource 增加"全错批次按上报的终止 offset 推进 committedOffsets"的 net-new 逻辑（见附录 C）。

---

## 12. PK 表写语义：__op / partial_update / merge_condition

> **用户要求：必须对齐现有 Routine Load 能力。** 核实结论：BE 的 op 列契约是**位置式**（输出 tuple 尾列名为 `__op`，`tablet_sink.cpp:814`、`memtable.cpp:104-122`），已完整实现；`merge_condition`/`partial_update` 在 INSERT 路径**已通**。只有行级 `__op` 与 kafka() TVF 是净新增。**无需 BE / thrift 改动。** 对抗式验证结论：implementable-with-changes，无硬阻断。

### 12.1 merge_condition —— 已通，仅需透传

INSERT 路径已支持：`InsertStmt.getMergingCondition()` 读 `PROPERTIES("merge_condition")`（`InsertStmt.java:296`），经 `InsertAnalyzer.analyzeProperties → LoadStmt.checkProperties` 校验（`MERGE_CONDITION ∈ LoadStmt.PROPERTIES_SET`），`InsertPlanner.java:487-490` 推到 `OlapTableSink.complete()` → `tSink.merge_condition`。
→ **兼容层把 RL `merge_condition` 映射为 Pipe PROPERTIES，并在生成的 INSERT 带 `PROPERTIES("merge_condition"="col")`。零 FE/BE/thrift 改动。**

### 12.2 partial_update —— 已通，建议补显式属性

INSERT 路径已支持，但当前是**隐式**触发：目标列是子集 + session var `enable_insert_partial_update` → `InsertAnalyzer.java:300-302` 置 `usePartialUpdate`；模式由 session var `partial_update_mode`（`column`/`auto`）在 `InsertPlanner.checkIfUseColumnUpsertMode` 决定。
→ **兼容层**：(1) 生成 `INSERT INTO tbl (子集列)` 触发 partial update；(2) 通过 Pipe 的 `task.` 级 session 变量设 `task.partial_update_mode='column'` 等。建议小幅 net-new：在 `InsertAnalyzer` 增加显式 `PROPERTIES("partial_update"/"partial_update_mode")` 处理（RL 属性名已存在于 `LoadStmt.java:97/104`），使其声明式而非依赖 session var 突变。

### 12.3 行级 __op（UPSERT/DELETE）—— net-new FE 代码（无 BE 改动）

机制（复用 Load + BE 既有原语）：
1. **kafka() TVF**：目标是 PK 表且 `enable_op_column=true` 时，输出一个尾列 `__op`（TINYINT）。
   - JSON：从消息的 `__op` key 提取（镜像 `Load.java:417-418` 的 `isLoadJson ⇒ null`，BE 运行时取）。
   - csv/raw：用户在 SELECT 写 `__op = upsert/delete` 或表达式。
2. **生成 SQL**：`INSERT INTO tbl SELECT <cols...>, <__op-expr> FROM kafka(...)`，尾投影即 op 值。
3. **InsertPlanner [扩展]**：当目标是 PK 且源带 op 列时，在 sink 输出 tuple **追加尾部 TINYINT `__op` slot**（镜像 `LoadPlanner.generateTupleDescriptor:422-428`），并把 op 投影绑定到该 slot。这是点亮 BE 既有契约的**唯一**改动。
4. **Analyzer [扩展]**：把 `'upsert'/'delete'` 字面量转 `TOpType`(UPSERT=0/DELETE=1)，复用 `Load.java:393-404` 逻辑，强制 TINYINT。
5. **opt-in 门**：仅当 `enable_op_column=true`（或兼容层为 PK 表自动开）才追加 op slot，避免普通用户 INSERT 意外长出 op 列而静默开始删除。

> **陷阱（勿踩）**：`OlapTableSink` 对每个 PK 表都会把 `__op` 加进**每索引的列名列表**（`OlapTableSink.java:478-480），但这是 inert 的——BE 只认输出 tuple 尾 slot。仅靠列名列表条目**不会**启用删除。

### 12.4 行为一致性约束（与 Load 相同）

`__op` DELETE 与 column-mode partial update 在 sort-key 表上的组合，受 `delta_writer.cpp:405-414` 同等约束（column-mode partial update 不能更新 sort-key 列；非 column-mode partial update 须提供全部 sort key）。v0.3 保持与 Load 一致，不放宽（放宽需 BE 改动）。

---

## 13. 错误容忍与 Schema/格式对齐

> **用户要求：对齐现有 Routine Load；无法对齐项列出来 check（见附录 C）。** RL 与 INSERT 共享同一套 BE scanner 与计数器，但 **FE 侧错误门结构不同**：RL 是**双门**（FE 绝对计数 + BE 比例），INSERT 只有**比例门**。

### 13.1 比例门 + strict_mode + timezone —— 原生复用

通过 Pipe 的 `task.` 级 session 变量（`Pipe.java` 的 `task.` 前缀 → `TaskRun.modifySystemVariable`）逐批注入，**复用 session 变量名**使行为一致、自说明：
- RL `strict_mode` → `task.enable_insert_strict`
- RL `max_filter_ratio` → `task.insert_max_filter_ratio`
- RL `timezone` → `task.time_zone`

> ⚠️ **默认翻转告警（必须处理）**：INSERT 的 `insert_max_filter_ratio` 默认 **0（零容忍）**，而 RL 的 BE 比例默认 **1.0（全容忍，错误由 FE 绝对计数把关）**。兼容层**必须**把 `task.insert_max_filter_ratio` 设为 RL 的 `maxFilterRatio`（默认 1.0），否则会把 RL 本可继续的 pipe 误 PAUSE。

### 13.2 绝对 max_error_number —— net-new（FE 重建）

INSERT 无绝对计数、无跨批累计。需在 `KafkaPipeSource` **精确复刻** `RoutineLoadJob.updateNumOfData`（`RoutineLoadJob.java:829-895`）：
- 维护 `currentErrorRows/currentTotalRows`，滑动窗口 = `maxBatchRows*10`；
- 每批从 commit-attachment 的 `total_rows_filtered` 累加；
- `currentErrorRows > maxErrorNum` → PAUSE（`ALTER PIPE SUSPEND`，原因 `TOO_MANY_FAILURE_ROWS`）；
- 在窗口边界重置。默认 `maxErrorNum=0`、`maxBatchRows=200000`（窗口 = 2,000,000 行）。

### 13.3 每批统计上报 —— net-new（小改）

§7.2 已述：`StmtExecutor` 增读 `unselected.rows`，并把 `filtered/unselected/received_bytes` 写入扩展后的 commit-attachment，供 §13.2 累加器使用。

### 13.4 错误样本 / tracking —— 原生

INSERT 路径的 `InsertLoadJob` 已生成真实 `tracking_url` 并暴露 `information_schema.load_tracking_logs WHERE job_id=<jobId>`（`StmtExecutor.java:3320-3342`）。把每批 jobId/tracking_url 收进 pipe 级 `EvictingQueue`（深度 3，镜像 `RoutineLoadJob.java:321`），作为 `SHOW PIPES.ErrorLogUrls` 暴露，与 `SHOW ROUTINE LOAD` 1:1。

### 13.5 Schema/格式 —— 原生映射

json（`jsonpaths/json_root/strip_outer_array`）、csv（`column_separator/trim_space/enclose/escape/skip_header`）直接映射为 kafka() TVF 参数，复用 `files()` 的同名参数与同一 BE scanner；`COLUMNS/派生列/WHERE` → 生成 SELECT/WHERE；`PARTITIONS(...)` → `INSERT INTO tbl PARTITION(...)`。JSON 自动按名匹配依赖**命名投影**（§5.2）。

> **无法对齐 / 需 net-new 的项**（绝对 max_error_number 的窗口、跨批累计、unselectedRows 当前被丢弃、avro+schema-registry from-bytes、protobuf 不存在、全错批次跳过、BE 比例默认翻转）—— **完整清单见附录 C，请逐条 check。**

---

## 14. Kafka Consumer 连接管理

> v0.2 §11 与代码不符，按事实修正：`kafka_consumer_pool_size_per_broker`/`kafka_consumer_idle_timeout_ms` **不存在**；真实 cap 是硬编码 `10`（`routine_load_task_executor.h:64`）的**全进程扁平池**，满了丢弃返还的 consumer（`data_consumer_pool.cpp:142`）；idle 回收是**死代码**；`match()` 含 per-job 唯一 `group.id` → 跨 pipe 不能共享；BE 用 `assign()` 无 consumer group join（§11.1 的"group join 开销"措辞删除）。

设计（全部标记为净新增/扩展）：
1. cap 做成真配置 `routine_load_kafka_consumer_pool_size`（默认按集群并发工作集，如 64–128；0=不限）。
2. 池改为按 `(broker, topic, 连接相关属性 hash)` **分桶**，避免小 pipe 互相饿死。
3. **group.id 移出 match key**（消费基于 `assign()`、offset 由 StarRocks 管，正确性无关）；定义 kafka() 的 group.id 默认策略（缺省不设由 BE 给监控用 id，或派生稳定 id），并文档化对外部监控可见性的影响。
4. 满桶时 LRU 驱逐**异key**的 idle consumer，而非丢弃刚归还的热 consumer。
5. 接上 idle 回收（现 `start_bg_worker` 只 sleep）。
6. 池迁移到 exec_env 作用域，便于从 `KafkaScanNode`（exec 层）访问。

---

## 15. 反压与流控

复用 INSERT 框架的 pipeline 反压：`OlapTableSink` 写阻塞 → pipeline 引擎暂停上游 `KafkaScanNode.get_next()`。

> 修正（来自代码核实）：
> - 反压是**有界缓冲滞后**，不是"零积压"：数据先填 per-NodeChannel `send_channel_buffer_limit=64MB × N` + scan buffer，恢复由 ~10ms poller 粒度（`pipeline_driver_poller.cpp`）。最坏在途内存 ≈ `N_nodes×64MB + scan buffer`，受共享 `load_process_max_memory_limit`（默认 30% BE 内存）约束（§16）。
> - 默认 `replicated_storage` 开时 scan+sink 同 fragment，直接背压成立；仅当 PK 表关闭 replicated_storage 才出现 `SHUFFLE_AGG` exchange 把 sink 放到另一 fragment、加一层 SinkBuffer 滞后。建议低延迟 pipe 保持 replicated_storage 开。
> - **stall watchdog 是 net-new**：v0.2 的"暂停 > consume_timeout_ms*4 → abort"机制**当前不存在**（driver 会长期 `OUTPUT_FULL` 挂到通用超时）。需在 `KafkaScanNode` 实现：跟踪无 offset 进展时长，超 `batch_timeout` 以可重试错误 fail fragment → txn abort → 下批以更大 batch 重消费（与 §10 降级联动）。统一阈值符号 `batch_timeout_ms`，并定义与 `load_process_max_memory_limit`、通用 query/insert 超时的触发先后；abort 须立即释放资源车道、per-load 内存、归还 consumer。

---

## 16. 资源隔离与过载保护

> 修正 v0.2 §13 的自相矛盾（§13.1"不做调度层准入" vs §13.2/§8.2"调度前按 BE 利用率限流"）。

**统一模型（分层，只表述一次）**：
1. **准入**：不按集群利用率阻塞批次提交；INSERT 进入 Query Queue / Resource Group。删除 v0.2 §13.2 的"调度前检查 / 总并行度上限排队 / `WAITING_FOR_SLOT` / `THROTTLED_BY_RESOURCE`"。
2. **并行度阻尼**：§9.2 的 `resource_headroom` 仅**抑制 scale-up**（热集群保持当前 DOP，从不阻塞批次，从不低于 1）——这是被认可的唯一"利用率影响调度"的点。
3. **专用车道**：Kafka pipe 的 INSERT 走 §8 的专用提交器，有**独立并发预算/优先级**，既不被 MV refresh / 交互查询饿死，也不淹没共享队列。
4. **诚实前提**：v0.2 称"INSERT 天然受 Query Queue 保护"**默认是空头支票**——`enable_query_queue_load=false`、cpu/mem permille/pct 阈值默认 0 不生效。文档要求显式开启，或由专用车道自管。
5. **共享内存背压**：真正的 receiver 背压是**共享的** `load_process_max_memory_limit`（默认 30% BE 内存，所有 load 共用），Resource Group **不细分**它。如需 per-pipe 隔离，给每条 INSERT 设 `task.load_mem_limit`（按 `max_batch_size × DOP` 推导）。
6. **commit-rate 背压**：纳入 §10 降级（compaction score / CommitRateLimiter）。

`SHOW PIPES.SCHEDULE_STATUS` 仅保留 `RUNNING / QUEUED_BY_RESOURCE / NO_NEW_DATA`，并新增 §10 的 `THROTTLED_BY_COMMIT_RATE / VERSION_PRESSURE`。

---

## 17. 安全性：凭证管理

> **P0 / GA blocker——提前到 Phase 1。** 代码核实：凭证写在 kafka() TVF 文本里 → 整条语句作为 `Pipe.originSql`（`@SerializedName`，`Pipe.java:94`）持久化 → `DESC PIPE` 在 `ShowExecutor.java:3037` **原样 dump**；审计 redactor `SqlCredentialRedactor` 不含 `sasl.jaas.config`/`ssl.*.password`；v0.2 把加密排到 Phase 3 → Phase 1-2 期间 image/BDBJE WAL 明文。

设计：
1. **结构化抽取**：CREATE 时解析 kafka() TVF，把 `property.sasl.*`/`property.ssl.*`/凭证类参数抽到 `KafkaPipeSource` 的结构化字段；`originSql` 中这些值**以占位符 token 存储**。
2. **唯一渲染脱敏点**：`KafkaTableFunctionRelation.toSql()`（及 TVF 属性 formatter）**构造即脱敏**，除非带授权的 reveal 标志——`DESC PIPE`、`SHOW CREATE`、`information_schema.pipes`、profile `SQL_STATEMENT`、异常消息全部经此点，杜绝散点漏出。
3. **审计 redactor 扩展**：`SqlCredentialRedactor` 增加 `property.sasl.jaas.config`、`property.sasl.password`、`property.ssl.key.password`、`property.ssl.keystore.password`、`property.ssl.truststore.password`、`property.sasl.oauthbearer.*` 及任意匹配 password/secret 的 `property.*`。
4. **落盘**：脱敏展示 + 避免明文落盘（占位符 + 结构化字段）作为 **Phase 1**；AES-at-rest + 密钥管理可留 Phase 3，但 Phase 1 不得明文落 image/WAL。

---

## 18. 状态机与生命周期管理

```
状态：SUSPEND / RUNNING / ERROR（FINISHED 存在于枚举，但 Kafka 源永不进入）

RUNNING ──suspend──▶ SUSPEND ──resume──▶ RUNNING
RUNNING ──可重试错误(退避自动恢复)──▶ RUNNING
RUNNING ──致命错误/超错误阈值──▶ ERROR ──手动 RESUME──▶ RUNNING
```

> 修正：v0.2 称"无 FINISHED"有误（枚举含 `FINISHED`）。

### 18.1 ERROR 自动恢复（对齐 RL，修复运维回归）

现有 RL 对瞬时故障（全 BE 短暂掉线 `REPLICA_FEW_ERR` 等）**带退避自动恢复**（同 `period_of_auto_resume_min` 窗口内最多 3 次，超过才锁定为需手动）。统一到 Pipe 后必须保留：区分**可重试错误**（broker 不可达 / BE 掉线 / txn `REPLICA_FEW`）与**致命错误**（schema 不兼容 / 认证失败）。`PipeScheduler` 对可重试错误指数退避自动 `ERROR→RUNNING`，N 次窗口内失败才锁定为需手动 RESUME。`SHOW PIPES` 暴露 `AUTO_RESUME_COUNT` / 锁定原因。

### 18.2 RUNNING→ERROR 判定输入（明确化）

明确触发输入：(a) 连续批次执行失败次数（对应现 Pipe 粗粒度计数）、(b) 累计 error rows 超 `max_error_number`（§13.2）、(c) 过滤比例超 `max_filter_ratio`（§13.1）。三者机制不同，须分别定义。

---

## 19. 存算分离（Shared-Data）适配

| 维度 | 差异（修正后明确列出） |
|------|------------------------|
| Sink 写入 | `OlapTableSink → 对象存储`，**且**每 tablet 每版本写新 `tablet_metadata`、写 `txn_log`，异步 `PublishVersionDaemon` make-visible |
| 提交限速 | 默认开 `CommitRateLimiter`（score>100 延迟、>2000 拒绝）——纳入 §10 降级与 `THROTTLED_BY_COMMIT_RATE` |
| 延迟 | publish 慢阈值本身 `lake_publish_version_slow_log_ms=1000`；shared-data 有效延迟下限通常 1–5s，随 tablet/bucket 数增长 |
| Warehouse | Pipe `PROPERTIES("warehouse"=...)`，CN 调度 |

> `target_e2e_latency` 在 shared-data 不能低于实测单批 publish 延迟；§10 控制器据实测自动抬升有效延迟。

---

## 20. 兼容性设计

### 20.1 语法兼容
现有 `CREATE ROUTINE LOAD` → Kafka Pipe 内部转换；完整属性映射见**附录 B**。

### 20.2 转换层策略
- 对**无法映射**的属性 **fail-fast**（清晰报错），绝不静默丢弃（保护 CSV `trim_space/enclose/escape` 等会改变解析结果的属性）。
- 错误容忍默认翻转处理见 §13.1。
- PK 写语义（`__op`/`merge_condition`/`partial_update`）映射见 §12。

### 20.3 SHOW ROUTINE LOAD 列契约
现 `SHOW ROUTINE LOAD` 24 列需逐列映射；分三档：
- **可直接映射**：Id/Name/CreateTime/DbName/TableName/State/DataSourceType/JobProperties/DataSourceProperties/CustomProperties、LatestSourcePosition/OffsetLag（对齐 §11 offset 模型）。
- **可从 commit-attachment 重建**：Statistic 的 loadedRows/errorRows/unselectedRows/receivedBytes（§7.2 扩展后）。
- **无干净对应，须显式决定**：`CurrentTaskNum`（重定义为当前并行度）、Statistic 的 `committedTaskNum/abortedTaskNum`（重定义为批次计数或标弃用/0）、`ErrorLogUrls/TrackingSQL`（依赖 §13.4 的 tracking 收集）。
> 把列集作为**向后兼容契约**加测试。`SHOW ROUTINE LOAD TASK` 同样给出"最近 N 批次"视图（§21）。

### 20.4 升级 / 回滚 / Pulsar
- `enable_unified_routine_load`（默认 `false`）**只接管 KAFKA 源**；**Pulsar 作业留 legacy 引擎**并加断言防误转（`Pipe.Type` 无 `PULSAR`，兼容层全是 Kafka）。
- **ADMIN MIGRATE**（具体化）：(a) offset 变换 `committedOffsets[p] = partitionIdToOffset[p] - 1`（旧存 next-to-consume，`KafkaProgress.java:201-203`）；(b) 特判 `OFFSET_BEGINNING(-2)/OFFSET_END(-1)` sentinel，不可朴素减一；(c) 迁移前要求源作业 PAUSED/STOPPED 且无 in-flight txn；(d) 幂等（按源 job id 键，已存在目标 Pipe 则拒绝）；(e) 回滚 export 反转 off-by-one 并从 `target_e2e_latency/max_parallelism` 反推旧参数。
- **回滚契约**：迁移单向守卫，或新 Pipe/offset 记录用前后兼容的 journal（optional 字段，不引入新 required opcode），使旧 FE 能安全跳过。

---

## 21. 可观测性

### 21.1 SHOW PIPES 增强（补诊断信号）
新增：per-partition committed/latest offset 与 lag、`EFFECTIVE_E2E`（与请求值对比 + 原因）、`ErrorLogUrls`/`TrackingSQL`（对接 load_tracking_logs）、`REASON_OF_STATE_CHANGED` 历史、`SCHEDULE_STATUS`（含 `THROTTLED_BY_COMMIT_RATE`/`VERSION_PRESSURE`）、`PARALLELISM_CHANGE_REASON`（枚举：SCALE_UP_LAG_HIGH/HELD_COOLDOWN/HELD_NO_HEADROOM/HELD_LAST_SCALE_INEFFECTIVE/SCALE_DOWN_LOW_LAG/THROTTLE_VERSION_PRESSURE）、`IN_FLIGHT_BATCHES`（txn_id/label）。

### 21.2 Metrics
per-pipe lag/throughput、`BATCH_ABORT_COUNT`、`COMMIT_WAIT_MS`、`BACKPRESSURE_PAUSED`、`pipe_kafka_parallelism_change_total{reason}`、版本/compaction score。

### 21.3 审计
**不逐批写审计/TaskRun 历史**（亚秒摄入会淹没观测面）：新增 `QuerySource.PIPE_KAFKA`，在 `auditAfterExec`/历史归档处跳过逐批记录，改为 pipe 维度按批次**聚合**一条审计 + 滚动批次历史（供 `SHOW ROUTINE LOAD TASK`）。

---

## 22. 配置参数

### 22.1 设计理念
用户声明 `target_e2e_latency`（预期值，§3.2）；系统据**实测**自适应推导内部窗口并按 §10 降级，而非固定比例。

### 22.2 Pipe 级别参数
| 参数 | 默认 | 说明 |
|------|------|------|
| `target_e2e_latency` | `1s` | 预期延迟（可请求低至 100ms，尽力而为） |
| `auto_parallelism` | `true` | |
| `max_parallelism` / `min_parallelism` | `0`(自动) / `1` | min 已接入冷启动（§9.2） |
| `max_batch_rows` / `max_batch_size` | `500000` / `100MB` | 也是 §13.2 绝对错误窗口基数 |
| `max_error_number` | `0` | **绝对错误行数；0=零容忍（与旧语义一致，勿误作"不限"，见附录 C）** |
| `max_filter_ratio` | `1.0` | 比例门（对齐 RL；注意默认翻转，§13.1） |
| `strict_mode` | `false` | |
| `enable_op_column` / `merge_condition` / `partial_update` / `partial_update_mode` | `false` / `` / `false` / `row` | PK 写语义（§12） |
| `warehouse` / `resource_group` | (default) | |
| `task.*`（如 `task.time_zone`/`task.load_mem_limit`） | — | 透传到每批 INSERT 的 session 变量（§13、§16） |

### 22.3 FE 全局参数
| 参数 | 默认 | 说明 |
|------|------|------|
| `enable_unified_routine_load` | `false` | 仅接管 KAFKA 源 |
| `pipe_kafka_offset_persist_interval_millis` | `10000` | 仅派生缓存/检查点（§11.2） |
| `pipe_kafka_partition_discovery_interval_s` | `600` | partition 发现 |
| `routine_load_kafka_consumer_pool_size` | `64` | §14 真配置（替代不存在的旧名） |

> 不引入 v0.2 中**不存在**的 `kafka_consumer_pool_size_per_broker`/`kafka_consumer_idle_timeout_ms`。

---

## 23. 实现路线图

### Phase 1（基础框架 + GA blocker）
- `kafka()` TVF（FE 解析/语义 + BE json/csv scanner 复用）+ `KafkaScanNode`（FE plan + BE 执行）。
- **compile-once / rebind + 专用提交器**（§8）；**流水线批次**（批次 N commit 时启动 N+1，避免串行卡吞吐）。
- BE 自主消费 + 扩展 `TKafkaConsumeReport`（含 filtered/unselected/bytes/tracking）。
- Offset 管理 + exactly-once（commit-attachment 唯一权威 + replay，§11）。
- **凭证脱敏 + 避免明文落盘**（§17，GA blocker）。
- **错误容忍对齐**（比例门原生 + 绝对 max_error_number 窗口 + 默认翻转处理，§13）。
- **错误可观测前置**：error-row 采样 / tracking URL / per-batch filtered-unselected（§13.4），不推到 Phase 4。
- consumer 池修复（§14）。

### Phase 2（动态并行度 + PK 写语义）
- 动态并行度（fresh-gate+max、stickiness、compaction-score 降级、冷启动、跨 pipe 公平，§9/§10）。
- **PK 写语义**：行级 `__op`（§12.3 net-new FE）、`merge_condition`/`partial_update` 透传（§12.1/12.2）。
- avro + Confluent Schema Registry（net-new BE from-bytes 路径，§5.2）。

### Phase 3（兼容 + 存算分离）
- `CREATE/SHOW/PAUSE/RESUME/STOP/ALTER ROUTINE LOAD` 兼容 + 列契约（§20.3）。
- `ADMIN MIGRATE` + 回滚契约（§20.4）；Pulsar 留 legacy 断言。
- Warehouse 集成；凭证 AES-at-rest。

### Phase 4（增强）
- DLQ（路由毒消息到另一 topic）——**其前置已在 Phase 1 落地**。
- 多 topic 订阅、Kafka header、Resource Group 级隔离、仪表盘集成。

---

## 附录 A：业界方案对比矩阵

| 特性 | ClickHouse | Databricks | Snowflake | Flink SQL | StarRocks 现有 | StarRocks 统一后 |
|------|-----------|------------|-----------|-----------|----------------|-------------------|
| SQL 原生 | 是(DDL) | 部分 | 否(connector) | 是(DDL) | 是(专用语法) | 是(INSERT+TVF) |
| 标准 INSERT 语法 | 否(MV) | 否 | 否(COPY) | 是 | 否 | **是** |
| 并行度单元 | num_consumers | partition | serverless | scan.parallelism | Task 数 | **MPP fragment instance** |
| 动态并行度 | 否 | 否 | 是 | 是 | 否 | **是（多维自适应+降级）** |
| MPP 跨节点 | 否 | 是 | N/A | 是 | 否 | **是** |
| Exactly-Once | 实验性 | 是 | 是 | 是 | 是(txn) | 是(txn+commit-attachment) |
| 端到端延迟 | 秒级 | 亚秒~秒 | 5-10s | 毫秒~秒 | 秒级 | **预期亚秒~秒（实测自适应，超时降级）** |
| 管理命令 | DETACH/ATTACH | stop/start | ALTER PIPE | SHOW JOBS | SHOW ROUTINE LOAD | **SHOW PIPES + 兼容旧命令** |

---

## 附录 B：CREATE ROUTINE LOAD 属性兼容映射表

> 来源：`CreateRoutineLoadStmt.PROPERTIES_SET`（24 项，`CreateRoutineLoadStmt.java:150-174`）+ FROM 子句数据源属性 + `COLUMNS TERMINATED BY`。分类：**TVF**=映射为 kafka() TVF 参数；**PIPE**=Pipe PROPERTIES；**SESSION**=`task.` 会话变量；**DERIVE**=由 `target_e2e_latency` 推导/弃用；**REJECT**=无法映射须 fail-fast；**IGNORE**=接受但无效（向后兼容）。

| 属性 | 分类 | 映射 / 说明 |
|------|------|-------------|
| `desired_concurrent_number` | PIPE | → `max_parallelism`(+`auto_parallelism`)。注意：迁移建议同时设 `min_parallelism` 以保静态行为，否则吞吐会变（见附录 C）。 |
| `max_batch_interval` | DERIVE | → `target_e2e_latency`（旧默认 10s）。语义有损：旧只是单任务消费窗口，新驱动整体节奏。 |
| `max_batch_rows` | PIPE | 保留为 escape hatch（无延迟语义对应）。默认 200000。 |
| `max_batch_size` | IGNORE | 代码中从未被 stmt 解析（仅 `Config.max_routine_load_batch_size` 生效）。接受并忽略。 |
| `max_error_number` | PIPE | 绝对计数（§13.2）。**默认 0 = 零容忍**（勿误作"不限"）。 |
| `max_filter_ratio` | PIPE/SESSION | → `task.insert_max_filter_ratio`。**默认 1.0；勿用 INSERT 默认 0**（§13.1）。 |
| `strict_mode` | SESSION | → `task.enable_insert_strict`（默认 false）。 |
| `timezone` | SESSION | → `task.time_zone`。 |
| `format` | TVF | csv/json/avro（**无 protobuf**）。avro 需 `confluent.schema.registry.url`。 |
| `jsonpaths` / `json_root` / `strip_outer_array` | TVF | 仅 json；`json_root`/`strip_outer_array` 与 `envelope` 互斥。 |
| `trim_space` / `enclose` / `escape` | TVF | 仅 csv。 |
| `COLUMNS TERMINATED BY`（非 property） | TVF | → csv `column_separator`（来自 `RoutineLoadDesc.columnSeparator`）。 |
| `log_rejected_record_num` | PIPE | 默认 0；-1=不限。 |
| `task_consume_second` / `task_timeout_second` | DERIVE | 由 `target_e2e_latency` 推导。注意旧固定 4:1 比例与新推导不同（见附录 C）。 |
| `pause_on_fatal_parse_error` | PIPE | 默认 false。 |
| `envelope` (debezium) | TVF | 仅 `debezium`；要求 `format=json`，禁 json_root/strip_outer_array。 |
| `kafka_broker_list` | TVF | → `broker_list`（必填，正则校验）。兼容层接受 `kafka_` 前缀。 |
| `kafka_topic` | TVF | → `topic`（必填）。 |
| `kafka_partitions` | TVF | → `partitions`。 |
| `kafka_offsets` | TVF | → `offsets`（**仅初始**；运行后以 commit-attachment 为权威，见附录 C）。`-2=BEGINNING/-1=END`。 |
| `kafka_default_offsets` | TVF | 折叠进 `offsets` 默认语义（实为 property.* key）。 |
| `property.*` | TVF | 透传 `property.*`（凭证经 §17 脱敏）。 |
| `confluent.schema.registry.url` | TVF | 命名 FROM 属性 → TVF param（avro）。 |

---

## 附录 C：无法对齐 / 净新增清单（请 review）

> 以下为对齐现有 Routine Load 时**无法仅靠配置/SQL 映射**、必须 net-new 实现或存在结构性差异的项。每项已标代码依据，请逐条确认是否接受。

### C.1 错误容忍 / 统计（§13）
1. **绝对 `max_error_number`**：INSERT 只有比例门，无绝对计数、无跨批累计。须在 `KafkaPipeSource` 重建 RL 的滑动窗口逻辑（`RoutineLoadJob.java:846-895`）。映射成 ratio **不等价**。
2. **跨批错误累计**：每个 INSERT 是独立 txn，filtered/unselected 算完即弃（`InsertTxnCommitAttachment` 只存 loadedRows）。须扩展 commit-attachment（§7.2）+ FE 累加器，否则 `max_error_number` 无输入。
3. **`unselectedRows` 当前被丢弃**：BE 上报 `unselected.rows`（`exec_state_reporter.cpp:117`）但 `StmtExecutor` 从不读取。达到 RL 的 totalRows/unselectedRows 列需改代码。
4. **BE 比例默认翻转**：RL BE 默认 ratio=1.0（全容忍），INSERT 默认 0（零容忍）。转换层**必须**显式设 `task.insert_max_filter_ratio=RL.maxFilterRatio`，否则误 PAUSE。
5. **全错批次跳过（`NO_ROWS_IMPORTED`）**：RL 在整批全错且 txn ABORTED 时仍推进 committed offset 越过毒批（`KafkaRoutineLoadJob.java:367-380`）。INSERT abort 不推进 offset；须 net-new offset 逻辑（§11.3）。

### C.2 格式（§5.2 / §13.5）
6. **PROTOBUF 不存在**：StarRocks 任何路径都无 protobuf scanner，RL 也仅 csv/json/avro。v0.2 的"avro/protobuf"为事实错误。本版移除；如需为后续独立 feature。
7. **AVRO + Confluent Schema Registry**：RL 在 BE 用 `confluent_schema_registry_url` 解析 Kafka 原始字节；`files()` 的 avro scanner 期望 avro 容器文件。kafka() TVF 需**净新增** BE registry-fetch / from-bytes 路径，与 files() 模型不同。
8. **JSON 自动按名匹配依赖命名投影**：BE 按目标 slot 列名匹配（`json_scanner.cpp:611`）。生成的 INSERT 必须用**命名** SELECT/COLUMNS（不能 `SELECT *`/位置引用），否则静默失配。v0.2 的"目标表 schema 下推"措辞已更正为"命名投影下推"。

### C.3 PK 写语义（§12）
9. **行级 `__op`（DELETE）净新增**：普通 `INSERT INTO pk_tbl SELECT` 永远是 UPSERT；`InsertPlanner` 无任何 `__op` 引用。须 net-new：PK 表 + opt-in 时追加尾部 `__op` slot（`InsertPlanner`）+ analyzer 识别/转换 op 列 + kafka() TVF 产出 `__op`（含 JSON `__op` key 自动提取）。**无 BE/thrift 改动**（位置约定）。
10. **partial_update 触发面不同**：INSERT 隐式（目标子集 + session var）vs RL 显式属性。须设 per-pipe session var 或加显式属性处理（小改）。
11. **sort-key + column-mode partial update + DELETE 组合受限**：与 Load 同等约束（`delta_writer.cpp:402-424`），不能更宽松（更宽松需 BE 改）。
12. **OlapTableSink 列名列表陷阱**：它对每个 PK 表都把 `__op` 加进索引列名列表（`OlapTableSink.java:478-480`），但 BE 忽略它、只认尾 slot。实现勿误以为列名列表条目就启用了删除。

### C.4 调度 / 执行（§8）
13. **TaskManager 1s tick 硬编码**：`scheduleAtFixedRate(...,0,1,SECONDS)` 非 Config；`task_runs_concurrency=4` 全局且与 MV refresh 共享；`PipeScheduler` interval 构造期冻结。→ 本版不沿用逐批 TaskManager，改专用提交器（§8）。
14. **无 INSERT plan cache**：须 net-new compile-once 缓存 + prebuilt ExecPlan 执行入口（`PrepareStmtPlanner` 仅 point-query）。

### C.5 兼容映射（附录 B）
15. **`task_consume_second/task_timeout_second` 比例不同**：旧固定 4:1 且 timeout>consume 配对校验；`target_e2e_latency` 推导用不同切分，无法逐字节复现旧节奏——须接受差异。
16. **`kafka_offsets` 不能进 rebind 的 TVF 文本**：compile-once 下当前 offset 移到 commit-attachment（与 RL 把 offset 存 `KafkaProgress` 是结构性差异）。
17. **`group_id` vs `property.group.id` 重复**：现有两处都能设 group.id（`KafkaRoutineLoadJob.java:103`），无优先级规则；须新增决策（§14）。
18. **Pulsar 无映射**：`kafka()` TVF 只覆盖 KAFKA；现有 Pulsar Routine Load 留 legacy 或拒绝（§20.4）。

### C.6 状态机 / 可观测
19. **`SHOW ROUTINE LOAD` 部分列无干净对应**：`CurrentTaskNum`、`committedTaskNum/abortedTaskNum`、`ErrorLogUrls/TrackingSQL` 须显式重定义或依赖 §13.4 的 net-new 采集（§20.3）。
20. **ERROR 自动恢复**：现有 Pipe 的 ERROR 是手动恢复终态，无 RL 的退避自动恢复——须 net-new（§18.1），否则 7×24 摄入回归。
