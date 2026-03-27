# 统一 Routine Load 概要设计文档

> **版本**: v0.2 (Draft)
>
> **状态**: 概要设计

---

## 目录

1. [背景与动机](#1-背景与动机)
2. [业界调研](#2-业界调研)
3. [设计目标与性能指标](#3-设计目标与性能指标)
4. [整体架构](#4-整体架构)
5. [SQL 语法设计](#5-sql-语法设计)
6. [核心模块设计](#6-核心模块设计)
7. [FE-BE 接口设计（Thrift/Protobuf）](#7-fe-be-接口设计thriftprotobuf)
8. [并发模型与动态并行度](#8-并发模型与动态并行度)
9. [执行框架复用](#9-执行框架复用)
10. [Offset 管理与 Exactly-Once 语义](#10-offset-管理与-exactly-once-语义)
11. [Kafka Consumer 连接管理](#11-kafka-consumer-连接管理)
12. [反压与流控](#12-反压与流控)
13. [资源隔离与过载保护](#13-资源隔离与过载保护)
14. [安全性：凭证管理](#14-安全性凭证管理)
15. [状态机与生命周期管理](#15-状态机与生命周期管理)
16. [存算分离（Shared-Data）适配](#16-存算分离shared-data适配)
17. [兼容性设计](#17-兼容性设计)
18. [可观测性](#18-可观测性)
19. [配置参数](#19-配置参数)
20. [实现路线图](#20-实现路线图)
21. [附录：业界方案对比矩阵](#附录业界方案对比矩阵)

---

## 1. 背景与动机

### 1.1 现有 Routine Load 的问题

当前 StarRocks 的 Routine Load 使用独立的执行框架，存在以下核心问题：

| 问题 | 描述 |
|------|------|
| **手动 Task 拆分** | 用户需要指定 `desired_concurrent_number`，FE 按 Kafka partition 数量将 Job 静态拆分为多个 Task，每个 Task 绑定一组 partition 的子集，调度到不同 BE 执行。拆分逻辑固定为 round-robin，无法根据实际吞吐动态调整。 |
| **独立执行路径** | Routine Load 使用 `StreamLoadPlanner` + `RoutineLoadTaskExecutor` 的专用执行路径，与标准 INSERT 执行路径不同，维护成本高。BE 侧使用独立的 `DataConsumer` / `DataConsumerGroup` + `KafkaConsumerPipe` 读取 Kafka 数据。 |
| **无法利用 MPP 并行** | 每个 Task 只在单个 BE 上执行，不能像标准 INSERT 那样利用 MPP 引擎在多个 BE 上并行执行同一个 fragment。 |
| **静态并行度** | 并行度在 Job 创建后固定（`min(partitionNum, desiredConcurrentNum, aliveNodeNum, max_concurrent_num)`），无法根据实时吞吐量自适应调整。 |
| **与 Pipe 框架割裂** | Pipe 框架（`CREATE PIPE ... AS INSERT INTO ... SELECT FROM files(...)`）已实现了基于 INSERT 的持续数据加载范式，但仅支持 FILE 源，Kafka 源尚未打通。Pipe 的 `Type` 枚举中已预留 `KAFKA` 类型。 |

### 1.2 期望的改进

将 Routine Load 统一到 Pipe 框架下，成为一种**流式 Pipe**：

- 使用标准 `INSERT INTO ... SELECT FROM kafka(...)` 语法
- 复用 INSERT 执行框架，利用 MPP 引擎实现多 BE 并行
- 支持动态并行度自适应调整
- 完全兼容现有 Routine Load 的语法和能力
- 统一管理、监控和运维接口

---

## 2. 业界调研

### 2.1 ClickHouse — Kafka Table Engine

**模型**: 三表架构（Kafka 引擎表 → 物化视图 → MergeTree 目标表）

```sql
CREATE TABLE kafka_source (...) ENGINE = Kafka
SETTINGS kafka_broker_list = 'broker:9092',
         kafka_topic_list = 'events',
         kafka_group_name = 'ch_group',
         kafka_format = 'JSONEachRow',
         kafka_num_consumers = 4;

CREATE MATERIALIZED VIEW kafka_to_events TO events AS
SELECT * FROM kafka_source;
```

- **并行度**: `kafka_num_consumers`（上限为 CPU 核数），**不支持动态调整**，修改需重建表
- **语义**: 默认 at-least-once，实验性 exactly-once（基于 ClickHouse Keeper）
- **管理**: `DETACH/ATTACH` 物化视图实现暂停/恢复
- **优点**: SQL 原生，声明式
- **缺点**: 并行度静态，exactly-once 不成熟

### 2.2 Databricks — Structured Streaming

**模型**: `read_kafka` TVF + `CREATE STREAMING TABLE`

```sql
CREATE OR REFRESH STREAMING TABLE raw_events AS
SELECT value::string:event_type AS event_type
FROM STREAM read_kafka(
    bootstrapServers => 'kafka:9092',
    subscribe => 'events'
);
```

- **并行度**: Kafka partition → Spark task 映射，`maxOffsetsPerTrigger` 限流
- **动态调整**: 标准 Structured Streaming **不推荐** auto-scaling，推荐 Lakeflow Declarative Pipelines
- **语义**: Delta Lake sink 支持 exactly-once
- **优点**: SQL + Python 灵活，exactly-once 可靠
- **缺点**: SQL-only 支持有限，动态调整需额外框架

### 2.3 Snowflake — Snowpipe Streaming

**模型**: Kafka Connect connector + 服务端 Pipe 对象

```sql
CREATE PIPE my_kafka_pipe AS
COPY INTO events FROM (
    SELECT $1:event_type::STRING FROM TABLE(DATA_SOURCE(TYPE => 'STREAMING'))
);
```

- **并行度**: 服务端 **serverless 自动扩缩**，用户不直接控制
- **语义**: 默认 exactly-once（双重 offset 追踪）
- **管理**: `ALTER PIPE ... SET PIPE_EXECUTION_PAUSED = TRUE/FALSE`
- **优点**: 零运维，自动扩缩，exactly-once 默认
- **缺点**: 非 SQL 原生（配置在 connector 侧）

### 2.4 Flink SQL — Kafka Connector

**模型**: `CREATE TABLE` + 连续 `INSERT INTO ... SELECT`

```sql
CREATE TABLE kafka_events (
    user_id BIGINT,
    event_type STRING
) WITH (
    'connector' = 'kafka',
    'topic' = 'events',
    'properties.bootstrap.servers' = 'broker:9092',
    'scan.parallelism' = '8',
    'format' = 'json'
);

INSERT INTO target_table SELECT * FROM kafka_events WHERE event_type <> 'heartbeat';
```

- **并行度**: `scan.parallelism`，Reactive Mode / Autopilot 支持动态调整
- **语义**: 基于 Kafka 事务 + Checkpoint 的 exactly-once
- **管理**: `SHOW JOBS`, `STOP JOB WITH SAVEPOINT`
- **优点**: 流处理原生，细粒度并行控制，丰富 CDC 支持
- **缺点**: 需要 Flink 集群基础设施

### 2.5 设计启发

| 借鉴点 | 来源 | 在 StarRocks 中的应用 |
|--------|------|---------------------|
| 标准 INSERT 语法驱动流式摄入 | Flink SQL | `INSERT INTO ... SELECT FROM kafka(...)` |
| 服务端自动扩缩并行度 | Snowflake | 动态并行度调整算法 |
| Table Function 封装数据源 | Databricks, Flink | `kafka()` TVF |
| 声明式 Pipe 管理 | Snowflake, 现有 Pipe | `CREATE PIPE ... AS INSERT INTO ...` |
| checkpoint + txn 保证 exactly-once | Flink, Snowflake | 基于 StarRocks 事务的 offset 管理 |

---

## 3. 设计目标与性能指标

### 3.1 核心目标

1. **统一框架**: Routine Load 统一到 Pipe 框架下，成为 `Pipe.Type.KAFKA` 类型
2. **标准语法**: 使用 `INSERT INTO ... SELECT FROM kafka(...)` 语法，通过 `kafka()` TVF 读取 Kafka 数据
3. **MPP 并行**: 每个 INSERT 任务可在多个 BE 上 MPP 并行执行，不再手动拆分 Task
4. **动态并行度**: 根据实时吞吐量自适应调整下次调度的并行度
5. **完全兼容**: 兼容现有 `CREATE ROUTINE LOAD` 语法，内部转换为新框架执行

### 3.2 性能指标

| 指标 | 目标 | 说明 |
|------|------|------|
| **端到端延迟** | < 1 秒 | 从消息写入 Kafka 到在 StarRocks 中可查询的时间（单批次场景） |
| **吞吐量** | 高于现有 Routine Load | 通过 MPP 并行 + 动态并行度，单 Pipe 吞吐量应显著超过现有单 Job 多 Task 模式 |
| **批次调度开销** | < 100ms | 从上一批次完成到下一批次开始执行的 FE 调度延迟 |
| **并行度调整响应** | 1 个批次周期 | 检测到 lag 变化后，在下一个批次即应用新的并行度 |

### 3.3 非目标（当前版本）

- Pulsar 源支持（后续扩展，但 `PipeSource` 抽象需为 `PulsarPipeSource` 留出扩展点）
- 跨集群 Kafka 复制
- CDC / changelog 格式支持（独立 feature）

---

## 4. 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                          用户接口层                                    │
│                                                                      │
│   CREATE PIPE my_pipe AS                                             │
│   INSERT INTO target_table                                           │
│   SELECT * FROM kafka(                                               │
│       'broker_list' = 'host:9092',                                   │
│       'topic' = 'events',                                            │
│       'format' = 'json'                                              │
│   );                                                                 │
│                                                                      │
│   ┌────────────────────────────────────────────┐                     │
│   │  兼容层：CREATE ROUTINE LOAD → Pipe 转换    │                     │
│   └────────────────────────────────────────────┘                     │
└───────────────────────────────┬──────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         FE 调度层                                     │
│                                                                      │
│   ┌─────────────┐   ┌──────────────────┐   ┌─────────────────────┐  │
│   │ PipeManager  │   │ KafkaPipeScheduler│   │ KafkaPipeSource    │  │
│   │             │──▶│  (扩展 Pipe       │──▶│  - offset 管理     │  │
│   │             │   │   Scheduler)      │   │  - partition 发现  │  │
│   └─────────────┘   └──────────────────┘   │  - 动态并行度计算  │  │
│                              │              └─────────────────────┘  │
│                              ▼                                       │
│                     ┌─────────────────┐                              │
│                     │  TaskManager    │                              │
│                     │  (INSERT 执行)  │                              │
│                     └────────┬────────┘                              │
└──────────────────────────────┼───────────────────────────────────────┘
                               │
                               ▼  INSERT INTO ... SELECT FROM kafka(...)
┌──────────────────────────────────────────────────────────────────────┐
│                        BE 执行层 (MPP)                                │
│                                                                      │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│   │  BE-1    │  │  BE-2    │  │  BE-3    │  │  BE-N    │           │
│   │          │  │          │  │          │  │          │           │
│   │ KafkaScan│  │ KafkaScan│  │ KafkaScan│  │ KafkaScan│           │
│   │ Node     │  │ Node     │  │ Node     │  │ Node     │           │
│   │ (P0,P3)  │  │ (P1,P4)  │  │ (P2,P5)  │  │ (P6,P7)  │           │
│   │    │     │  │    │     │  │    │     │  │    │     │           │
│   │    ▼     │  │    ▼     │  │    ▼     │  │    ▼     │           │
│   │ OLAP     │  │ OLAP     │  │ OLAP     │  │ OLAP     │           │
│   │ TableSink│  │ TableSink│  │ TableSink│  │ TableSink│           │
│   └──────────┘  └──────────┘  └──────────┘  └──────────┘           │
│                                                                      │
│                  (partition 分配由 FE Planner 决定)                    │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.1 核心变化

| 组件 | 现有 Routine Load | 统一后 |
|------|-------------------|--------|
| 用户语法 | `CREATE ROUTINE LOAD ... FROM KAFKA (...)` | `CREATE PIPE ... AS INSERT INTO ... SELECT FROM kafka(...)` |
| FE 调度 | `RoutineLoadScheduler` + `RoutineLoadTaskScheduler` | `PipeScheduler` + `KafkaPipeScheduler` |
| 任务拆分 | FE 按 partition round-robin 拆分为 N 个独立 Task | FE 生成单个 INSERT，由 MPP 引擎将 KafkaScanNode 分布到多 BE |
| BE 执行 | `RoutineLoadTaskExecutor` + `DataConsumer` | 标准 INSERT 执行 + `KafkaScanNode`(新增) |
| 并行度 | 静态（Job 创建时确定） | 动态（每轮调度自适应调整） |
| 事务 | N 个 Task × N 个独立 txn（每个消费 ~30s） | 1 个 INSERT 批次 × 1 个 txn（消费 ~1s），重试代价等价 |

**事务模型分析**: 旧模型中 3 个 Task 各消费 30s 后各自 commit，如果 1 个 Task 失败则重试该 Task 的 30s 数据量。新模型中 1 个 INSERT 批次消费 ~1s 后全局 commit，如果失败则重试 1s 的全量数据。由于新模型的单批次时间窗口远小于旧模型，单次失败的重试代价是等价甚至更低的。

---

## 5. SQL 语法设计

### 5.1 新语法（推荐）

```sql
-- 创建 Kafka 流式 Pipe
CREATE PIPE [IF NOT EXISTS] [db.]pipe_name
[PROPERTIES (
    "poll_interval" = "10",
    "max_batch_rows" = "500000",
    "max_batch_interval" = "10",
    "auto_parallelism" = "true",
    "max_parallelism" = "16",
    "warehouse" = "default_warehouse"
)]
AS INSERT INTO target_table
[ (col1, col2, ...) ]
SELECT
    col1, col2, ...
FROM kafka(
    "broker_list" = "host1:9092,host2:9092",
    "topic" = "my_topic",
    "group_id" = "starrocks_group",
    "format" = "json",                       -- json / csv / avro / protobuf
    "partitions" = "0,1,2,3",               -- 可选，默认全部
    "offsets" = "OFFSET_BEGINNING",          -- OFFSET_BEGINNING / OFFSET_END / 具体值
    "confluent.schema.registry.url" = "...", -- 可选
    "property.security.protocol" = "SASL_PLAINTEXT",  -- 透传 Kafka consumer 配置
    "property.sasl.mechanism" = "PLAIN"
)
[ WHERE condition ];

-- 管理操作（复用现有 Pipe 语法）
ALTER PIPE pipe_name SUSPEND;
ALTER PIPE pipe_name RESUME;
ALTER PIPE pipe_name SET ("max_parallelism" = "32");
DROP PIPE [IF EXISTS] pipe_name;
SHOW PIPES [FROM db] [LIKE 'pattern'];
```

### 5.2 kafka() TVF 设计

`kafka()` 是一个表值函数（Table-Valued Function），作为 Kafka 数据源的 SQL 抽象：

```sql
kafka(
    -- 必选参数
    "broker_list" = "host:9092",
    "topic" = "topic_name",

    -- 可选参数
    "format" = "json",                    -- 数据格式 (json/csv/avro/protobuf)
    "group_id" = "consumer_group",        -- Kafka consumer group
    "partitions" = "0,1,2",              -- 指定消费的 partition 列表
    "offsets" = "OFFSET_BEGINNING",       -- 起始 offset
    "max_poll_records" = "500",           -- 单次 poll 最大记录数
    "task_consume_second" = "15",         -- 单批次消费时间窗口

    -- Schema 相关
    "jsonpaths" = "[\"$.id\", \"$.name\"]",  -- JSON 列映射路径
    "json_root" = "$.data",              -- JSON 根路径
    "strip_outer_array" = "true",        -- 剥离外层数组
    "confluent.schema.registry.url" = "...",

    -- Kafka consumer 透传配置（property. 前缀）
    "property.security.protocol" = "SASL_PLAINTEXT",
    "property.sasl.mechanism" = "PLAIN",
    "property.sasl.jaas.config" = "..."
)
```

**Schema 策略：对齐现有 Routine Load 和 FILES() TVF 的行为，根据格式采用不同的 schema 确定方式。**

与文件场景不同，Kafka 是流式数据源，FE 在 plan 阶段无法采样消息来推导 schema。因此 `kafka()` TVF 采用**目标表 schema 下推**（与 `FILES()` 的 `INSERT INTO ... SELECT FROM files(...)` 一致）+ **BE 运行时自动匹配**的策略：

| format | Schema 确定方式 | 与现有行为的对齐 |
|--------|---------------|-----------------|
| **json（无 `jsonpaths`）** | **自动匹配**：TVF 输出列 = 目标表列（schema 下推）。BE 运行时将 JSON 顶层 key 按名称匹配到目标列；无匹配的 key 跳过，缺失列填 NULL。 | 与现有 Routine Load JSON 无 `jsonpaths` 行为**完全一致** |
| **json（有 `jsonpaths`）** | **位置映射**：第 i 个 jsonpath 表达式 → 第 i 个目标列。支持嵌套路径（如 `$.user.name`）。 | 与现有 Routine Load `jsonpaths` 行为**完全一致** |
| **csv** | **位置映射**：TVF 输出列 = 目标表列（schema 下推）。CSV 字段按分隔符拆分后按位置对应到列。用户可通过 SELECT 表达式进行类型转换和列派生。 | 与现有 Routine Load CSV + `COLUMNS` 行为一致；同时对齐 `FILES()` CSV 的位置列模式 |
| **avro / protobuf** | **Schema Registry 推导**：从 `confluent.schema.registry.url` 获取 schema，按名称匹配到目标列。 | 与现有 Routine Load Avro 行为一致 |
| **raw** | **固定 schema**：`_key VARBINARY, _value VARBINARY, _topic STRING, _partition INT, _offset BIGINT, _timestamp DATETIME`。用户在 SELECT 中自行解析 `_value`。 | 新增模式，类似 Databricks `read_kafka` 的固定列输出 |

**与其他系统的对齐：**
- **Flink SQL**: `CREATE TABLE` 显式定义 schema，JSON format 按 column name 自动匹配 → 我们的 json 无 `jsonpaths` 模式等价（目标表定义 schema，JSON key 自动匹配）
- **ClickHouse**: Kafka Engine 在 `CREATE TABLE` 中显式定义 schema，`JSONEachRow` 格式按名称自动映射 → 同上
- **Databricks**: `read_kafka` 返回固定列（key/value/topic/partition/offset/timestamp），用户必须在 SELECT 中解析 value → 我们的 `raw` 模式等价

**目标表 schema 下推机制（对齐 FILES()）：**

```
CREATE PIPE ... AS
INSERT INTO target_table (user_id, event_type, created_at)  -- 目标列
SELECT user_id, event_type, created_at                       -- SELECT 列
FROM kafka("broker_list" = "...", "topic" = "...", "format" = "json")

Planner 阶段:
  1. 从 INSERT INTO 目标表获取列信息 (user_id BIGINT, event_type VARCHAR, created_at DATETIME)
  2. 下推到 kafka() TVF，作为其输出 schema
  3. BE 运行时 JSON 解析器按 key 名称匹配:
     {"user_id": 123, "event_type": "click", "created_at": "2026-03-27", "extra": "ignored"}
     → user_id=123, event_type="click", created_at="2026-03-27"  (extra 被跳过)
```

**所有 format 均可通过 SELECT 中的表达式进行列映射、类型转换和列派生**，对齐 Routine Load `COLUMNS(col1, col2, col3 = expr)` 的能力：

```sql
-- JSON: 无需 jsonpaths 的简单场景（自动按名称匹配）
INSERT INTO events SELECT user_id, event_type, ts
FROM kafka("broker_list" = "...", "topic" = "events", "format" = "json");

-- JSON: 使用 jsonpaths 提取嵌套字段
INSERT INTO events SELECT user_id, event_type, created_at
FROM kafka("broker_list" = "...", "topic" = "events", "format" = "json",
           "jsonpaths" = "[\"$.user.id\", \"$.event.type\", \"$.meta.created_at\"]");

-- CSV: 位置映射 + 表达式派生
INSERT INTO events (user_id, event_type, event_time)
SELECT $1, $2, from_unixtime(CAST($3 AS BIGINT))
FROM kafka("broker_list" = "...", "topic" = "events", "format" = "csv",
           "columns_terminated_by" = ",");

-- raw: 用户自行解析 Kafka value（类似 Databricks read_kafka）
INSERT INTO events (user_id, event_type, kafka_ts)
SELECT
    CAST(json_query(_value, '$.user_id') AS BIGINT),
    json_query(_value, '$.event_type'),
    _timestamp
FROM kafka("broker_list" = "...", "topic" = "events", "format" = "raw");
```

### 5.3 兼容语法（向后兼容现有 Routine Load）

现有 `CREATE ROUTINE LOAD` 语法在 FE 解析后，**内部转换**为等价的 Kafka Pipe：

```sql
-- 用户原有写法（继续支持）
CREATE ROUTINE LOAD db.job_name ON target_table
COLUMNS(col1, col2, tmp_col, col3 = tmp_col + 1),
WHERE col1 > 0
PROPERTIES (
    "desired_concurrent_number" = "3",
    "max_batch_interval" = "20",
    "max_batch_rows" = "300000",
    "max_error_number" = "1000",
    "strict_mode" = "true",
    "format" = "json",
    "jsonpaths" = "[\"$.id\", \"$.name\"]"
)
FROM KAFKA (
    "kafka_broker_list" = "host:9092",
    "kafka_topic" = "my_topic",
    "kafka_partitions" = "0,1,2",
    "kafka_offsets" = "100,200,300"
);
```

**内部等价转换为**：

```sql
CREATE PIPE db.__routine_load_job_name
PROPERTIES (
    "max_batch_interval" = "20",
    "max_batch_rows" = "300000",
    "max_error_number" = "1000",
    "strict_mode" = "true",
    "auto_parallelism" = "true",
    "max_parallelism" = "3"         -- 从 desired_concurrent_number 映射
)
AS INSERT INTO target_table (col1, col2, col3)
SELECT col1, col2, tmp_col + 1 AS col3
FROM kafka(
    "broker_list" = "host:9092",
    "topic" = "my_topic",
    "format" = "json",
    "jsonpaths" = "[\"$.id\", \"$.name\"]",
    "partitions" = "0,1,2",
    "offsets" = "100,200,300"
)
WHERE col1 > 0;
```

兼容层保证：
- `SHOW ROUTINE LOAD` 继续工作（查询内部 Pipe 状态并转换展示格式）
- `PAUSE/RESUME/STOP ROUTINE LOAD` 映射到 `ALTER/DROP PIPE`
- `ALTER ROUTINE LOAD` 映射到 `ALTER PIPE SET PROPERTIES`

---

## 6. 核心模块设计

### 6.1 FE 侧新增/修改模块

```
com.starrocks.load.pipe/
├── Pipe.java                  # 扩展支持 Type.KAFKA
├── PipeManager.java           # 扩展 Kafka Pipe 生命周期管理
├── PipeScheduler.java         # 扩展 Kafka Pipe 调度逻辑
├── PipeSource.java            # [修改] 抽象类，增加 Kafka/Pulsar 扩展点
├── KafkaPipeSource.java       # [新增] Kafka 数据源管理
│   ├── partition 发现与变更检测
│   ├── offset 管理（读取/持久化）
│   ├── 构建带起始 offset 的 INSERT SQL
│   └── 消费进度追踪
├── KafkaPipePiece.java        # [新增] 单批次消费描述
│   ├── partition → 起始 offset 映射
│   ├── 并行度
│   └── 超时配置
└── KafkaProgressTracker.java  # [新增] 全局 offset 进度追踪

com.starrocks.planner/
├── KafkaScanNode.java         # [新增] Kafka 扫描计划节点
│   ├── partition 分配策略
│   ├── 起始 offset 约束
│   └── 并行度 hint

com.starrocks.sql.ast/
├── KafkaTableFunctionRelation.java  # [新增] kafka() TVF 的 AST 节点

com.starrocks.catalog/
├── KafkaTableFunction.java    # [新增] kafka() TVF 元信息
```

### 6.2 BE 侧新增/修改模块

```
be/src/exec/
├── kafka_scan_node.cpp/.h     # [新增] Kafka 扫描节点执行
│   ├── 从 DataConsumerPool 获取/归还 consumer
│   ├── partition 分配（来自 FE plan）
│   ├── offset seek 与时间窗口消费
│   ├── 数据解析（JSON/CSV/Avro/Protobuf）
│   └── 吞吐量统计 + 实际消费 offset 上报

be/src/runtime/routine_load/
├── data_consumer.cpp          # 复用现有 librdkafka 消费者
├── data_consumer_pool.cpp     # 复用消费者连接池（增加 KafkaScanNode 适配）
```

### 6.3 关键类设计

#### KafkaPipeSource

```java
public class KafkaPipeSource extends PipeSource {
    private String brokerList;
    private String topic;
    private String groupId;
    private String format;
    private Map<String, String> kafkaProperties;

    // partition 管理
    private List<Integer> assignedPartitions;
    private Map<Integer, Long> committedOffsets;    // 已提交的 offset
    private Map<Integer, Long> latestOffsets;       // Kafka 端最新 offset（仅用于 lag 计算）

    // 动态并行度
    private int currentParallelism;
    private ThroughputTracker throughputTracker;

    // 构建每批次 INSERT SQL（BE 自主消费模式：只传起始 offset，不传终止 offset）
    public String buildInsertSql(Pipe pipe, KafkaPipePiece piece) {
        // 将 kafka() TVF 中的 offset 参数替换为本批次的起始 offset
        // BE 在 task_consume_second 时间窗口内自主消费，消费完毕后上报实际终止 offset
        // 返回带有 label 的 INSERT SQL
    }

    // 拉取一个新的消费批次
    public KafkaPipePiece pullPiece() {
        // 1. 检查 latestOffsets vs committedOffsets 是否有新数据（lag > 0）
        // 2. 确定本批次各 partition 的起始 offset = committedOffset + 1
        // 3. 基于吞吐历史 + BE 资源利用率计算并行度
        // 4. 构建 KafkaPipePiece
    }

    // 批次完成后更新 offset（从 BE 上报的实际消费终止 offset）
    public void onBatchComplete(Map<Integer, Long> actualConsumedOffsets) {
        // 更新 committedOffsets = actualConsumedOffsets
        // 记录吞吐量用于下轮并行度计算
    }

    // partition 变更检测
    public boolean detectPartitionChange() {
        // 定期从 Kafka 获取 partition 列表，与当前列表比较
    }
}
```

#### KafkaScanNode (BE)

```cpp
class KafkaScanNode : public ScanNode {
public:
    Status init(const TPlanNode& tnode, RuntimeState* state) override;
    Status open(RuntimeState* state) override;
    Status get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) override;
    Status close(RuntimeState* state) override;

private:
    // 从 FE plan 获取的分配信息
    std::string _broker_list;
    std::string _topic;
    std::vector<PartitionOffset> _partition_start_offsets;  // 本节点负责的 partition + 起始 offset
    std::string _format;
    std::map<std::string, std::string> _kafka_properties;

    // 消费控制（BE 自主消费：在时间窗口内消费尽可能多的数据）
    int64_t _max_batch_rows;
    int64_t _max_batch_interval_ms;        // 消费时间窗口
    int64_t _consumed_rows = 0;
    MonotonicStopWatch _consume_timer;

    // librdkafka consumer (从 DataConsumerPool 获取，用完归还)
    std::shared_ptr<KafkaDataConsumer> _consumer;

    // 数据解析器
    std::unique_ptr<StreamLoadParser> _parser;

    // 实际消费统计（执行完毕后上报给 FE）
    std::map<int32_t, int64_t> _actual_end_offsets;  // partition → 实际消费到的 offset
    int64_t _total_bytes_consumed = 0;
    int64_t _total_rows_consumed = 0;
};
```

---

## 7. FE-BE 接口设计（Thrift/Protobuf）

### 7.1 新增 Thrift 结构

```thrift
// KafkaScanNode 的计划参数
struct TKafkaScanNode {
    1: required string broker_list
    2: required string topic
    3: required map<i32, i64> partition_start_offsets   // partition_id → start_offset
    4: required string format                           // json / csv / avro / protobuf / raw
    5: optional map<string, string> kafka_properties    // 透传的 Kafka consumer 配置
    6: optional i64 max_batch_rows
    7: optional i64 max_batch_interval_ms               // 消费时间窗口
    8: optional string jsonpaths
    9: optional string json_root
    10: optional bool strip_outer_array
    11: optional string confluent_schema_registry_url
    12: optional string columns_terminated_by
}
```

### 7.2 BE → FE 上报实际消费结果

INSERT 执行完成后，BE 通过事务 commit attachment 上报每个 fragment instance 的实际消费信息：

```thrift
// 附加在 txn commit attachment 中
struct TKafkaConsumeReport {
    1: required map<i32, i64> partition_end_offsets     // partition_id → 实际消费终止 offset (exclusive)
    2: required i64 total_rows_consumed
    3: required i64 total_bytes_consumed
    4: required i64 consume_duration_ms
}
```

FE 在 `afterCommitted` / `afterVisible` 回调中从 attachment 提取 `TKafkaConsumeReport`，更新 `committedOffsets` 和吞吐量统计。

### 7.3 TPlanNode 扩展

在现有 `TPlanNode` 的 union 字段中增加 `TKafkaScanNode`：

```thrift
struct TPlanNode {
    ...
    // 新增
    30: optional TKafkaScanNode kafka_scan_node
}

enum TPlanNodeType {
    ...
    KAFKA_SCAN_NODE
}
```

---

## 8. 并发模型与动态并行度

### 8.1 新并发模型 vs 旧模型

```
旧模型 (Routine Load):
┌─────────────────────────────────────────────────┐
│ Job (desired_concurrent_number = 3)             │
│                                                  │
│ Task-0 (P0,P3) ──→ BE-1  [独立事务, 消费30s]    │
│ Task-1 (P1,P4) ──→ BE-2  [独立事务, 消费30s]    │
│ Task-2 (P2,P5) ──→ BE-3  [独立事务, 消费30s]    │
│                                                  │
│ 每个 Task 独立消费，独立 commit                    │
│ 无法跨 BE 并行，静态分配                          │
└─────────────────────────────────────────────────┘

新模型 (Unified Pipe):
┌─────────────────────────────────────────────────┐
│ Pipe (一个 INSERT, 消费 ~1s)                     │
│                                                  │
│ INSERT INTO target SELECT ... FROM kafka(...)    │
│      │                                           │
│      ▼ MPP Plan (parallelism = 6)                │
│                                                  │
│ Fragment-0: KafkaScanNode                        │
│   Instance-0 (BE-1): P0,P1                       │
│   Instance-1 (BE-2): P2,P3                       │
│   Instance-2 (BE-3): P4,P5                       │
│                                                  │
│ Fragment-1: OlapTableSink                        │
│   Instance-0 (BE-1): Tablet X,Y                  │
│   Instance-1 (BE-2): Tablet Z,W                  │
│   Instance-2 (BE-3): Tablet M,N                  │
│                                                  │
│ 单一事务，全局 commit                              │
└─────────────────────────────────────────────────┘
```

### 8.2 动态并行度算法

每轮调度时（每批次 INSERT 完成后），`KafkaPipeSource` 根据多维信号计算下一轮并行度：

```
输入信号:
  - kafka_partition_count:      Kafka topic 的 partition 数量
  - alive_be_count:             存活的 BE/CN 节点数
  - max_parallelism:            用户配置的上限
  - last_throughput_rows_per_s: 上一批次的吞吐量（行/秒）
  - last_throughput_bytes_per_s:上一批次的吞吐量（字节/秒）
  - current_lag:                当前消费延迟（所有 partition 的 lag 总和）
  - lag_velocity:               lag 变化速率（正 = lag 在增长，负 = lag 在收敛）
  - target_lag_threshold:       目标延迟阈值
  - avg_be_cpu_permille:        BE 平均 CPU 利用率（千分比，来自 TResourceUsage 上报）
  - avg_be_mem_used_pct:        BE 平均内存使用率（来自 ComputeNode.getMemUsedPct()）
  - last_scale_effect:          上一次扩缩容是否有效（扩容后吞吐是否提升 > 10%）
  - last_scale_time:            上一次并行度变更的时间

资源利用率数据来源:
  BE 通过 TReportRequest.resource_usage 定期上报 TResourceUsage，
  FE 在 ComputeNode 中维护 cpuUsedPermille 和 memUsedBytes。
  算法读取 SystemInfoService 中各 BE 的实时资源数据。

算法 (多维自适应并行度):
  base_parallelism = min(kafka_partition_count, alive_be_count, max_parallelism)

  // 资源约束：如果 BE 负载已高，不再增加并行度
  resource_headroom = (avg_be_cpu_permille < 800 AND avg_be_mem_used_pct < 0.8)

  // 冷却期：距离上次变更不足 3 个批次周期，保持不变
  if (now - last_scale_time) < cooldown_period:
      desired = current_parallelism

  // 扩缩容有效性检查：如果上次扩容后吞吐未提升，说明瓶颈不在并行度
  elif last_scale_direction == UP and last_scale_effect == false:
      desired = current_parallelism  // 不再盲目扩容

  elif current_lag > target_lag_threshold * 2 and lag_velocity > 0 and resource_headroom:
      // lag 严重且在增长，且 BE 有资源余量
      desired = min(current_parallelism * 2, base_parallelism)

  elif current_lag > target_lag_threshold and resource_headroom:
      // lag 偏高
      desired = min(current_parallelism + 1, base_parallelism)

  elif current_lag < target_lag_threshold * 0.1 and current_parallelism > 1:
      // lag 很低，可以减少并行度节约资源
      desired = max(current_parallelism - 1, 1)

  else:
      desired = current_parallelism

  next_parallelism = clamp(desired, 1, base_parallelism)
```

### 8.3 Partition 分配策略

FE 的 `KafkaScanNode` 在 plan 阶段将 Kafka partition 分配到各 BE fragment instance：

```
分配算法: 加权 round-robin
  1. 获取 alive BE 列表，按负载从低到高排序
  2. 计算每个 partition 的预估 lag（latest_offset - committed_offset）
  3. 按 lag 从大到小排序 partitions
  4. 依次将 partition 分配给当前负载最低的 fragment instance
  5. 每次分配后更新 instance 的预估负载

目标: lag 大的 partition 优先分配，负载均衡
```

---

## 9. 执行框架复用

### 9.1 执行流程（BE 自主消费模型）

```
┌──────────────────────────────────────────────────────────────┐
│  PipeScheduler 调度一轮:                                      │
│                                                              │
│  1. KafkaPipeSource.pullPiece()                              │
│     - 获取各 partition 的 committedOffset（起始 offset）       │
│     - 计算动态并行度                                          │
│     - 构建 KafkaPipePiece (partition→起始 offset)             │
│                                                              │
│  2. KafkaPipeSource.buildInsertSql(piece)                    │
│     - 生成带起始 offset 的 INSERT SQL:                         │
│       INSERT INTO target /*+ SET_VAR(parallelism=6) */       │
│       SELECT ... FROM kafka(                                 │
│         'broker_list'='...', 'topic'='...',                  │
│         'partitions'='0,1,2,3,4,5',                          │
│         'offsets'='1000,2000,3000,4000,5000,6000'             │
│       )                                                      │
│       注意：不传 max_offsets，BE 自主在时间窗口内消费           │
│     - 为本批次生成唯一 label                                   │
│                                                              │
│  3. TaskManager.executeTaskAsync(sql)                        │
│     - SqlTaskRunProcessor → StmtExecutor                     │
│     - 标准 INSERT 执行路径                                    │
│                                                              │
│  4. Planner 阶段:                                             │
│     - 识别 kafka() TVF → KafkaScanNode                       │
│     - 根据 parallelism hint 确定 fragment instance 数         │
│     - Kafka partition 分配到各 instance                       │
│     - 与 OlapTableSink 组成执行计划                           │
│                                                              │
│  5. BE 执行阶段 (每个 fragment instance):                     │
│     - KafkaScanNode::open()                                  │
│       → 从 DataConsumerPool 获取 consumer（连接复用）          │
│       → seek 到起始 offset                                    │
│     - KafkaScanNode::get_next()                              │
│       → 在 max_batch_interval_ms 时间窗口内持续 poll + 解析   │
│       → 达到时间/行数/字节数上限后设置 eos = true              │
│       → 记录各 partition 实际消费到的终止 offset               │
│     - KafkaScanNode::close()                                 │
│       → 归还 consumer 到 DataConsumerPool                     │
│     - OlapTableSink → 写入 tablet                            │
│                                                              │
│  6. 事务 commit + offset 上报:                                │
│     - INSERT 完成 → txn commit                               │
│     - commit attachment 携带 TKafkaConsumeReport              │
│       (各 partition 实际终止 offset + 吞吐量统计)              │
│     - FE onBatchComplete() 更新 committedOffsets              │
│     - 释放资源                                                │
│                                                              │
│  7. 返回步骤 1，开始下一轮调度                                  │
└──────────────────────────────────────────────────────────────┘
```

### 9.2 与现有 Pipe (FILE) 的复用

| 组件 | FILE Pipe | KAFKA Pipe | 复用程度 |
|------|-----------|------------|----------|
| `PipeManager` | 管理 FILE 类型 Pipe | 管理 KAFKA 类型 Pipe | 完全复用 |
| `PipeScheduler` | 调度 FILE Pipe | 调度 KAFKA Pipe（增加 Kafka 特有逻辑） | 扩展复用 |
| `PipeListener` | 文件列表发现 | Kafka partition/offset 监听 | 分别实现 |
| `TaskManager` | 执行 INSERT SQL Task | 执行 INSERT SQL Task | 完全复用 |
| `SqlTaskRunProcessor` | SQL → StmtExecutor | SQL → StmtExecutor | 完全复用 |
| `PipeSource` (abstract) | `FilePipeSource` | `KafkaPipeSource` | 抽象复用 |
| `PipePiece` (abstract) | `FilePipePiece` | `KafkaPipePiece` | 抽象复用 |
| `Pipe.State` 状态机 | SUSPEND/RUNNING/FINISHED/ERROR | SUSPEND/RUNNING/ERROR (无 FINISHED) | 复用 |

---

## 10. Offset 管理与 Exactly-Once 语义

### 10.1 Offset 管理策略（BE 自主消费模型）

```
┌──────────────────────────────────────────────────────┐
│                  Offset 生命周期                       │
│                                                      │
│  Kafka Broker                                        │
│    topic: events                                     │
│    P0: [0, 1, 2, ..., 999, 1000, ..., 1500]         │
│                      ↑                ↑               │
│               committed=999    latest=1500            │
│                                                      │
│  FE (KafkaPipeSource):                               │
│    committedOffsets: {P0: 999, P1: 500, ...}         │
│    (持久化到 StarRocks 元数据)                         │
│                                                      │
│  批次 N:                                              │
│    pullPiece() → 分配起始 offset: P0[1000, ...]      │
│    buildInsertSql() → INSERT ... offsets=1000         │
│      (不传 max_offsets，BE 自主消费)                   │
│                                                      │
│    BE 在时间窗口内消费:                                │
│      P0: 实际消费到 1350（时间窗口到了）               │
│                                                      │
│    INSERT 成功 → txn COMMITTED                        │
│    BE 上报 TKafkaConsumeReport:                       │
│      {P0: 1350, rows: 350, bytes: 15MB}              │
│    FE 更新 committedOffsets: {P0: 1350}              │
│                                                      │
│  批次 N+1:                                            │
│    起始 offset: P0[1351, ...]                         │
│                                                      │
│  如果批次 N 失败 → txn ABORTED                        │
│    committedOffsets 不变: {P0: 999}                   │
│    下轮重新从 P0[1000, ...] 消费                      │
└──────────────────────────────────────────────────────┘
```

### 10.2 Exactly-Once 保证

通过 StarRocks 事务机制实现 exactly-once：

1. **每批次对应一个事务**: INSERT 带唯一 label → `GlobalTransactionMgr.beginTransaction`
2. **offset 与数据原子提交**: 事务 commit attachment 中携带各 partition 的实际消费终止 offset（由 BE 上报）
3. **故障恢复**: FE 重启后，通过 label → txn 状态查询恢复 committed offsets
   - txn VISIBLE → 从 attachment 提取 offset，已提交
   - txn COMMITTED 但未 VISIBLE → 等待 VISIBLE
   - txn ABORTED / 不存在 → 从 committedOffsets 重新消费
4. **不依赖 Kafka consumer group offset**: StarRocks 自主管理 offset，Kafka 侧的 consumer group offset 仅用于外部监控工具（可选同步）

### 10.3 与旧 Routine Load 的对比

| 维度 | 旧 Routine Load | 统一后 |
|------|-----------------|--------|
| 事务粒度 | 每个 Task 一个 txn | 每个 INSERT 批次一个 txn |
| offset 确定方式 | FE 预定义范围 + BE 执行 | BE 自主消费 + 事后上报实际 offset |
| offset 存储 | `KafkaProgress` 对象（FE 内存 + 元数据） | `KafkaPipeSource.committedOffsets`（同样持久化） |
| 故障恢复 | `RoutineLoadJob.recovery()` via label/txn | `Pipe.recovery()` 扩展 Kafka offset |
| Kafka offset commit | BE 侧 txn commit 后 `consumer.commit()` | 可选，仅用于外部监控工具可见性 |

---

## 11. Kafka Consumer 连接管理

### 11.1 问题分析

Kafka consumer 的创建开销较大（TCP 连接建立、consumer group join、metadata fetch），如果每个 INSERT 批次在每个 BE 上都创建新的 consumer，会严重影响端到端延迟。

### 11.2 连接池复用策略

复用现有 BE 侧的 `DataConsumerPool` 机制，并进行扩展：

```
┌─────────────────────────────────────────────────────┐
│  BE DataConsumerPool (每个 BE 进程一个)              │
│                                                      │
│  Key: (broker_list, topic, kafka_properties_hash)    │
│                                                      │
│  ┌──────────────────────────────────────────┐        │
│  │ Pool Entry: broker1:9092 / events        │        │
│  │   Consumer-0: [idle, last_used=10:30:01] │        │
│  │   Consumer-1: [in_use, by=scan_node_xyz] │        │
│  │   Consumer-2: [idle, last_used=10:29:55] │        │
│  └──────────────────────────────────────────┘        │
│                                                      │
│  获取流程:                                            │
│    1. KafkaScanNode::open() 调用 pool.acquire(key)   │
│    2. Pool 返回空闲 consumer（优先最近使用的）         │
│    3. 如无空闲 consumer，创建新的（不超过上限）        │
│    4. 调用 consumer.seek(partition, offset) 定位      │
│                                                      │
│  归还流程:                                            │
│    1. KafkaScanNode::close() 调用 pool.release(c)    │
│    2. Consumer 保持连接，标记为 idle                   │
│    3. 超过 idle_timeout 后自动关闭                    │
│                                                      │
│  动态并行度变化时:                                     │
│    - 并行度增加 → 更多 scan node 需要 consumer        │
│      → pool 按需创建新 consumer                       │
│    - 并行度减少 → 多余 consumer 归还 pool             │
│      → 在 idle_timeout 后自然回收                     │
└─────────────────────────────────────────────────────┘
```

### 11.3 配置参数

| 参数 | 默认值 | 描述 |
|------|--------|------|
| `kafka_consumer_pool_size_per_broker` | `10` | 每个 (broker, topic) 的 consumer 池大小上限 |
| `kafka_consumer_idle_timeout_ms` | `600000` (10min) | 空闲 consumer 自动关闭超时 |

---

## 12. 反压与流控

### 12.1 问题场景

当目标表写入较慢时（compaction 积压、tablet 版本数过多等），`KafkaScanNode` 持续 poll 可能导致内存积压。

### 12.2 流控机制

**复用现有 INSERT 框架的流控**：标准 INSERT 执行路径已有 `OlapTableSink` 的反压机制——当 Sink 端写入阻塞时，pipeline 引擎会自动暂停上游算子（`KafkaScanNode`）的 `get_next()` 调用。

**Kafka 消费侧的额外保护**：

```
KafkaScanNode 内置流控:
  1. 每次 get_next() 返回一个 Chunk 后检查:
     - consumed_rows >= max_batch_rows → eos
     - consume_timer >= max_batch_interval_ms → eos
     - consumed_bytes >= max_batch_size → eos
  2. 如果 pipeline 引擎因 Sink 反压暂停了调度:
     - KafkaScanNode 不再调用 rdkafka poll
     - 消费自然暂停，不会积压内存
  3. 如果暂停时间过长（超过 task_timeout_second）:
     - 整个 INSERT 批次超时，txn abort
     - 下轮从 committedOffsets 重新消费
```

### 12.3 Sink 端限速

在目标表负载较高时，可以通过以下方式间接限速：

- `max_batch_rows` / `max_batch_size`：限制单批次数据量
- `poll_interval`：增大批次间隔，给目标表更多消化时间
- 动态并行度算法会检测 BE 资源利用率，在 CPU/内存高时不增加并行度

---

## 13. 资源隔离与过载保护

### 13.1 多 Pipe 并发的资源管控

多个 Kafka Pipe 同时运行时，需要防止集群过载：

| 层级 | 机制 | 参数 |
|------|------|------|
| **全局** | Kafka Pipe 总数上限 | `pipe_kafka_max_pipes`（默认 100） |
| **全局** | 所有 Kafka Pipe 的总并行度上限 | `pipe_kafka_total_max_parallelism`（默认 = alive_be_count * 3） |
| **单 BE** | 单 BE 上 Kafka consumer 并发数上限 | `max_kafka_consumers_per_be`（默认 16，类比旧 `max_routine_load_task_num_per_be`） |
| **单 Pipe** | 单 Pipe 的最大并行度 | Pipe PROPERTIES `max_parallelism` |
| **Resource Group** | 基于 Resource Group 的资源隔离 | Pipe PROPERTIES `resource_group` |

### 13.2 过载保护与用户提示

当系统检测到资源紧张时：

```
调度前检查:
  1. 查询当前所有 Kafka Pipe 的总并行度
  2. 查询目标 BE 的 cpuUsedPermille 和 memUsedPct
  3. 如果总并行度达到上限 → 新 Pipe 进入排队等待
  4. 如果 BE 资源超阈值（CPU > 900‰ 或 Mem > 90%）→ 降低新批次的并行度

用户可见提示:
  - SHOW PIPES 增加 SCHEDULE_STATUS 列:
    "RUNNING" / "WAITING_FOR_SLOT" / "THROTTLED_BY_RESOURCE"
  - 创建 Pipe 时如果接近上限，返回 WARNING:
    "Kafka Pipe created, but system is near capacity.
     Current: 95/100 pipes, 45/48 total parallelism.
     Consider reducing parallelism or removing unused pipes."
```

### 13.3 与 Query Queue 的集成

现有 StarRocks 的 Query Queue（`query_queue_cpu_used_permille_limit`）可以阻止新查询在资源紧张时执行。Kafka Pipe 的 INSERT 执行也受此机制保护——当 BE 报告 `isResourceOverloaded()` 时，INSERT 会被排队。

---

## 14. 安全性：凭证管理

### 14.1 问题

`kafka()` TVF 中的 Kafka 认证凭证（SASL 密码、SSL 证书路径等）如果明文存储在 Pipe 元数据中，会在以下场景暴露：

- `SHOW CREATE PIPE` 输出
- FE 审计日志
- `information_schema` 查询
- FE 元数据 image/WAL

### 14.2 凭证脱敏方案

```
存储层:
  - Pipe 元数据中的 property.* 凭证字段使用 AES 加密存储
  - 加密 key 存储在 FE 的 key management 中

展示层:
  - SHOW CREATE PIPE / SHOW PIPES:
    凭证字段显示为 "property.sasl.jaas.config" = "******"
  - 审计日志: 同样脱敏
  - information_schema: 凭证字段不暴露

运行时:
  - 构建 INSERT SQL 时，凭证解密后传入执行计划
  - BE 侧 KafkaScanNode 接收到的是明文凭证（Thrift 通信已加密）
  - BE 执行完毕后不持久化凭证
```

---

## 15. 状态机与生命周期管理

### 15.1 Kafka Pipe 状态机

```
                 ┌──────────────┐
  CREATE PIPE    │   RUNNING    │   ALTER PIPE RESUME
     ┌──────────▶│              │◀──────────────┐
     │           │  (调度中)    │               │
     │           └───┬──────┬──┘               │
     │               │      │                  │
     │    ALTER PIPE │      │ 超出错误阈值      │
     │    SUSPEND    │      │                  │
     │               ▼      ▼                  │
     │        ┌─────────┐ ┌───────┐            │
     │        │ SUSPEND │ │ ERROR │            │
     │        │         │ │       │────────────┘
     │        │(已暂停) │ │(异常) │  ALTER PIPE RESUME
     │        └────┬────┘ └───┬───┘
     │             │          │
     │        DROP PIPE   DROP PIPE
     │             │          │
     │             ▼          ▼
     │           (Pipe 被删除)
     │
     │  注: Kafka Pipe 无 FINISHED 状态
     │  (流式持续消费，除非用户主动 DROP)
     │  ERROR 状态可通过 ALTER PIPE RESUME 恢复
```

与现有 Pipe 框架保持一致：`State.canResume()` 对 `SUSPEND` 和 `ERROR` 均返回 `true`，用户可以从 ERROR 状态恢复 Pipe。

### 15.2 生命周期事件

| 事件 | 触发时机 | 动作 |
|------|---------|------|
| **CREATE** | 用户执行 `CREATE PIPE` 或 `CREATE ROUTINE LOAD` | 创建 Pipe 对象，注册到 PipeManager，初始化 KafkaPipeSource |
| **SCHEDULE** | PipeScheduler 每 `pipe_scheduler_interval_millis` 检查 | 检查 lag → pullPiece → buildInsertSql → executeTaskAsync |
| **BATCH_COMPLETE** | INSERT 事务 VISIBLE | 从 BE 上报的 TKafkaConsumeReport 更新 offset，记录吞吐量，计算下轮并行度 |
| **BATCH_FAIL** | INSERT 事务 ABORT 或超时 | 不更新 offset，记录错误次数，判断是否暂停 |
| **PARTITION_CHANGE** | 定期检测到 Kafka partition 增减 | 更新 partition 列表，下轮自动纳入新 partition |
| **SUSPEND** | 用户执行 `ALTER PIPE SUSPEND` | 停止调度，等待正在执行的批次完成 |
| **RESUME** | 用户执行 `ALTER PIPE RESUME`（从 SUSPEND 或 ERROR） | 重新开始调度，从 committedOffsets 继续消费 |
| **ALTER** | 用户修改 Pipe 属性 | 热更新配置（如 max_parallelism, max_batch_interval 等） |
| **DROP** | 用户执行 `DROP PIPE` 或 `STOP ROUTINE LOAD` | 停止调度，清理元数据和 offset 信息 |
| **FE_RECOVERY** | FE leader 切换/重启 | 通过 label/txn 状态 + commit attachment 恢复 committed offsets |

---

## 16. 存算分离（Shared-Data）适配

### 16.1 Shared-Data 架构下的差异

在存算分离模式下，数据存储在对象存储（S3/HDFS），计算由 CN（Compute Node）而非 BE 执行。KafkaScanNode 需要在 CN 上执行。

### 16.2 适配方案

| 维度 | Shared-Nothing | Shared-Data |
|------|---------------|-------------|
| 执行节点 | BE | CN (Compute Node) |
| KafkaScanNode 调度 | 分配到 BE fragment instance | 分配到 CN fragment instance |
| Consumer Pool | 每个 BE 一个 DataConsumerPool | 每个 CN 一个 DataConsumerPool |
| Sink 写入 | OlapTableSink → 本地 tablet | OlapTableSink → 对象存储 |
| Warehouse 隔离 | N/A | 通过 Pipe PROPERTIES `warehouse` 指定 |

### 16.3 Warehouse 集成

```sql
-- 指定 Warehouse 执行 Kafka Pipe
CREATE PIPE kafka_events_pipe
PROPERTIES (
    "warehouse" = "etl_warehouse"
)
AS INSERT INTO events
SELECT * FROM kafka("broker_list" = "...", "topic" = "events");
```

- `KafkaPipeSource` 在 `pullPiece()` 时通过 `WarehouseManager.acquireComputeResource()` 获取计算资源
- 并行度计算使用指定 warehouse 中的 alive CN 数量（而非全局 BE 数量）
- 资源隔离由 Warehouse 层面保证，不同 Pipe 可指定不同 Warehouse

---

## 17. 兼容性设计

### 17.1 语法兼容

| 旧语法 | 新行为 |
|--------|--------|
| `CREATE ROUTINE LOAD` | FE 解析后创建等价的 Kafka Pipe |
| `SHOW ROUTINE LOAD` | 查询 Pipe 状态，转换为 Routine Load 展示格式 |
| `SHOW ROUTINE LOAD TASK` | 查询 Pipe Task 执行历史，转换展示 |
| `PAUSE ROUTINE LOAD` | `ALTER PIPE SUSPEND` |
| `RESUME ROUTINE LOAD` | `ALTER PIPE RESUME` |
| `STOP ROUTINE LOAD` | `DROP PIPE` |
| `ALTER ROUTINE LOAD` | `ALTER PIPE SET PROPERTIES` |

### 17.2 升级兼容

- **滚动升级**: 升级期间旧版 FE 的 Routine Load Job 继续以旧方式运行
- **元数据迁移**: 新版 FE 启动时，提供 `ADMIN MIGRATE ROUTINE LOAD TO PIPE` 命令，将旧 Job 转换为 Pipe
- **回滚安全**: 迁移后的 Pipe 元数据可通过工具导出为旧 RoutineLoadJob 格式
- **双写期**: 提供配置开关 `enable_unified_routine_load`，允许一段时间内新旧共存

### 17.3 行为兼容

| 能力 | 旧 Routine Load | 统一后 |
|------|-----------------|--------|
| Column mapping | `COLUMNS(col1, col2, expr)` | `SELECT col1, col2, expr FROM kafka(...)` |
| WHERE 过滤 | `WHERE col1 > 0` | `SELECT ... FROM kafka(...) WHERE col1 > 0` |
| Partition 指定 | `PARTITIONS(p1, p2)` | `INSERT INTO table PARTITION(p1, p2) SELECT ...` |
| strict_mode | PROPERTIES | Pipe PROPERTIES |
| max_error_number | PROPERTIES | Pipe PROPERTIES (`max_error_number`) |
| max_filter_ratio | PROPERTIES | Pipe PROPERTIES (`max_filter_ratio`) |
| jsonpaths | PROPERTIES | kafka() TVF 参数 |
| json_root | PROPERTIES | kafka() TVF 参数 |
| strip_outer_array | PROPERTIES | kafka() TVF 参数 |
| timezone | PROPERTIES | Session variable (via `TASK.timezone`) |
| Avro/Protobuf + Schema Registry | PROPERTIES | kafka() TVF 参数 |

---

## 18. 可观测性

### 18.1 SHOW PIPES 增强

```sql
mysql> SHOW PIPES\G
*************************** 1. row ***************************
           PIPE_ID: 10001
         PIPE_NAME: kafka_events_pipe
         DATABASE: test_db
             STATE: RUNNING
   SCHEDULE_STATUS: RUNNING
         PIPE_TYPE: KAFKA
        TABLE_NAME: events
      KAFKA_TOPIC: user_events
    KAFKA_BROKERS: broker1:9092,broker2:9092
     PARTITION_NUM: 6
       PARALLELISM: 4
    COMMITTED_LAG: 1500
   ROWS_LOADED_TOTAL: 5000000
  BYTES_LOADED_TOTAL: 2.5 GB
  LAST_BATCH_ROWS: 50000
 LAST_BATCH_DURATION: 3.2s
LAST_BATCH_PARALLELISM: 4
  THROUGHPUT_ROWS_S: 15625
 THROUGHPUT_BYTES_S: 8.0 MB/s
     ERROR_COUNT: 0
   LAST_ERROR_MSG: NULL
       CREATE_TIME: 2026-03-27 10:00:00
  LAST_SCHEDULE_TIME: 2026-03-27 14:30:00
```

`SCHEDULE_STATUS` 取值：`RUNNING` / `WAITING_FOR_SLOT` / `THROTTLED_BY_RESOURCE` / `NO_NEW_DATA`

### 18.2 Metrics

| Metric | 类型 | 描述 |
|--------|------|------|
| `pipe_kafka_rows_loaded_total` | Counter | 累计加载行数 |
| `pipe_kafka_bytes_loaded_total` | Counter | 累计加载字节数 |
| `pipe_kafka_batch_duration_seconds` | Histogram | 批次执行耗时 |
| `pipe_kafka_consumer_lag` | Gauge | 当前消费延迟 (所有 partition 总和) |
| `pipe_kafka_current_parallelism` | Gauge | 当前并行度 |
| `pipe_kafka_batch_error_total` | Counter | 批次错误次数 |
| `pipe_kafka_partition_count` | Gauge | 订阅的 partition 数量 |

### 18.3 审计日志

每批次完成时记录审计信息：

```
[KAFKA_PIPE] pipe=kafka_events_pipe batch_id=1001 label=pipe_10001_1001
  partitions=[0:1000-1350, 1:500-780, 2:200-590]
  parallelism=4 rows=50000 bytes=25MB duration=3.2s
  status=SUCCESS txn_id=12345
```

---

## 19. 配置参数

### 19.1 Pipe 级别参数（用户可配置）

| 参数 | 默认值 | 描述 |
|------|--------|------|
| `poll_interval` | `10` (秒) | 检查新数据的间隔 |
| `max_batch_interval` | `10` (秒) | 单批次最长消费时间窗口 |
| `max_batch_rows` | `500000` | 单批次最大行数 |
| `max_batch_size` | `104857600` (100MB) | 单批次最大字节数 |
| `auto_parallelism` | `true` | 是否启用动态并行度 |
| `max_parallelism` | `0` (自动) | 最大并行度（0 = min(partition_count, be_count)） |
| `min_parallelism` | `1` | 最小并行度 |
| `max_error_number` | `0` (不限) | 最大允许错误行数 |
| `max_filter_ratio` | `0` | 最大允许过滤比例 |
| `strict_mode` | `false` | 严格模式 |
| `target_lag_threshold` | `100000` | 触发并行度提升的 lag 阈值 |
| `task_consume_second` | `15` | 单批次消费时间上限 |
| `task_timeout_second` | `60` | 单批次执行超时 |
| `warehouse` | (default) | 指定执行的 Warehouse（存算分离模式） |
| `resource_group` | (default) | 指定 Resource Group |

### 19.2 FE 全局参数

| 参数 | 默认值 | 描述 |
|------|--------|------|
| `enable_unified_routine_load` | `false` | 是否启用统一 Routine Load |
| `pipe_kafka_scheduler_interval_millis` | `5000` | Kafka Pipe 调度间隔 |
| `pipe_kafka_max_pipes` | `100` | 最大 Kafka Pipe 数量 |
| `pipe_kafka_total_max_parallelism` | `0` (自动) | 所有 Kafka Pipe 总并行度上限（0 = alive_be_count * 3） |
| `pipe_kafka_offset_persist_interval_millis` | `10000` | Offset 持久化间隔 |
| `pipe_kafka_partition_discovery_interval_s` | `600` | Partition 发现间隔 |
| `max_kafka_consumers_per_be` | `16` | 单 BE/CN 上 Kafka consumer 并发数上限 |
| `pipe_kafka_parallelism_cooldown_batches` | `3` | 并行度变更后的冷却期（批次数） |

---

## 20. 实现路线图

### Phase 1: 基础框架

- `kafka()` TVF 实现（FE 语法解析 + 语义分析）
- `KafkaScanNode` 实现（FE plan 节点 + BE 执行节点）
- BE 自主消费模型 + TKafkaConsumeReport 上报
- `KafkaPipeSource` + `KafkaPipePiece` 实现
- 基本 `CREATE PIPE ... AS INSERT INTO ... SELECT FROM kafka(...)` 端到端
- offset 管理 + exactly-once 语义
- Kafka Consumer 连接池复用
- Thrift 接口定义（TKafkaScanNode, TKafkaConsumeReport）

### Phase 2: 动态并行度与流水线

- 吞吐量采集与上报
- 多维自适应并行度算法（lag + BE 资源利用率 + 冷却期 + 扩缩容有效性）
- 自适应 partition 分配策略
- partition 变更自动感知
- **流水线批次执行**: 支持当前批次 commit 时即启动下一批次的消费，提升吞吐

### Phase 3: 兼容性与存算分离

- `CREATE ROUTINE LOAD` 到 Pipe 的内部转换层
- `SHOW/PAUSE/RESUME/STOP ROUTINE LOAD` 兼容
- `ALTER ROUTINE LOAD` 兼容
- 元数据迁移工具
- Warehouse 集成（存算分离模式下的 CN 调度）
- 凭证加密存储与脱敏展示

### Phase 4: 增强能力

- Avro / Protobuf 格式 + Schema Registry
- Dead Letter Queue (错误消息路由)
- 多 topic 订阅（正则匹配）
- Kafka Header 访问
- Resource Group 级别的资源隔离
- 消费指标仪表盘集成

---

## 附录：业界方案对比矩阵

| 特性 | ClickHouse | Databricks | Snowflake | Flink SQL | StarRocks (现有) | StarRocks (统一后) |
|------|-----------|------------|-----------|-----------|-----------------|-------------------|
| **SQL 原生** | 是 (DDL) | 部分 | 否 (connector) | 是 (DDL) | 是 (专用语法) | 是 (INSERT + TVF) |
| **标准 INSERT 语法** | 否 (MV) | 否 (stream API) | 否 (COPY INTO) | 是 | 否 (专用框架) | **是** |
| **并行度单元** | num_consumers | Kafka partition | serverless | scan.parallelism | Task 数 | **MPP fragment instance** |
| **动态并行度** | 否 | 否 | 是 (serverless) | 是 (Reactive) | 否 | **是 (多维自适应算法)** |
| **MPP 跨节点并行** | 否 (单节点) | 是 (Spark) | N/A (serverless) | 是 (分布式) | 否 (单 BE/Task) | **是** |
| **Exactly-Once** | 实验性 | 是 (Delta) | 是 (默认) | 是 (Kafka txn) | 是 (txn) | 是 (txn) |
| **动态 Partition 发现** | 否 | 是 | 是 | 是 | 是 | 是 |
| **DLQ 支持** | 是 (v25.8+) | 手动 | 是 | 手动 | 否 | 规划中 |
| **E2E 延迟目标** | 秒级 | 亚秒~秒 | 5-10s | 毫秒~秒 | 秒级 | **< 1s** |
| **管理命令** | DETACH/ATTACH | stop/start | ALTER PIPE | SHOW JOBS | SHOW ROUTINE LOAD | **SHOW PIPES + 兼容旧命令** |
