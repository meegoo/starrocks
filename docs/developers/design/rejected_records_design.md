# Rejected Records 系统表概要设计

> **作者**: StarRocks Team
> **状态**: Draft
> **创建日期**: 2026-03-27

## 1. 背景与动机

### 1.1 客户场景与原始需求

**客户场景**：AppLovin 通过 `INSERT INTO ... SELECT ... FROM FILES()` 从 GCS Parquet 文件批量导入数据到 StarRocks 聚合表。他们使用 `max_filter_ratio` 容忍少量坏行以避免整批失败导致的下游延迟。但对于被过滤/丢弃的行，他们需要**追踪、检查和重放(replay)**所有被拒绝的行——这对数据完整性至关重要。

**核心痛点**：

1. **error log 硬编码上限太小**：`kMaxErrorNum = 50`（`runtime_state_helper.cpp`）/ `MAX_ERROR_NUM = 50`（`runtime_state.cpp`），单批可能产生数千条被拒绝行，但只能看到前 50 条。客户需要至少 10,000 行的可见性。
2. **rejected record 默认不记录**：`log_rejected_record_num` 默认为 0，且记录到 BE 本地文件，无法通过 SQL 查询。
3. **没有 replay 能力**：被拒绝的行无法重新导入。客户参考 Vertica 的方案（rejected rows 导出到可查询的表/文件夹，可直接 replay），希望 StarRocks 也能提供类似能力。
4. **两难选择**：要么 `max_filter_ratio=0` 整批失败，要么允许过滤但丢失坏行的可见性。

**原始需求拆分为两个层次**：

| 层次 | 需求 | 状态 |
|------|------|------|
| **短期** | 将 `kMaxErrorNum` 改为可配置的 session/system 变量，允许用户增大到 10,000+ | 已确认可行（简单改动） |
| **长期** | 提供结构化、可重放的 rejected row 导出——写入可查询的表，支持 `INSERT INTO ... SELECT ... FROM` 进行 replay | 本设计文档的目标 |

### 1.2 问题归纳

StarRocks 当前通过 **reject record 机制** 和 **error log 机制** 处理导入坏数据，存在以下系统性问题：

1. **error log 行数硬编码上限**：`kMaxErrorNum = 50`，单批数千条错误只能看到 50 条。
2. **rejected record 默认关闭**：`log_rejected_record_num` 默认为 0。即使开启，也写入 BE 本地文件，无 HTTP 下载接口，需 SSH 到 BE 节点。
3. **格式不标准**：rejected record 以 tab 分隔写入，无结构化元数据（错误码、来源位置、时间戳等）。
4. **不支持 replay**：无法将被拒绝的行重新导入目标表。
5. **Routine Load 场景坏数据永久丢失**：Kafka offset 前移后无法回溯。
6. **Parquet/ORC 场景尤其差**：`arrow_to_starrocks_converter.cpp` 中 Parquet 仅记录列级错误上下文（`file=, column=, raw_data=`），`orc_chunk_reader.cpp` 传入空字符串，没有可用的行级数据。

### 1.3 设计目标

引入统一的导入错误数据持久化机制，**直接对标 Vertica rejected data table 的能力**：

| 目标 | 对应需求 | 说明 |
|------|---------|------|
| **全量捕获** | "10,000+ rows visibility" | 所有被过滤的行写入可查询的表，不再受 50 行上限限制 |
| **SQL 可查询** | "inspect rejected rows" | 通过标准 SQL 查询错误数据，含错误原因、出错列、原始数据 |
| **可重放 (Replay)** | "replay failed records" | `INSERT INTO target SELECT ... FROM dlq_table` 重新导入 |
| **支持 Parquet 场景** | "GCS Parquet → INSERT INTO ... SELECT ... FROM FILES()" | Parquet/ORC 列式格式的被拒绝行也能结构化记录和重放 |
| **与 max_filter_ratio 协同** | "partial success + inspect + replay pattern" | rejected records 启用后，`max_filter_ratio` 允许的过滤行自动写入系统表 |
| **所有导入方式** | 通用化 | Stream Load、Routine Load、Broker Load、INSERT 均支持 |
| **零外部依赖** | 简单部署 | 纯 StarRocks 内置 OLAP 表，无需 Kafka/S3 等外部系统 |

### 1.4 术语讨论：为什么叫 "Dead Letter Queue"？

**"Dead Letter" 的词源**：源自邮政系统。1737 年 Benjamin Franklin 任费城邮政局长时首次使用 "dead letters" 一词，指无法投递的邮件。1825 年美国邮政正式建立 Dead Letter Office（死信办公室），专门处理因地址不全、收件人不存在等原因无法投递也无法退回的邮件。

**引入计算机领域**：消息队列系统（IBM MQ、RabbitMQ、Kafka Connect 等）借用了这个类比——无法被消费者成功处理的消息被路由到一个专门的 "Dead Letter Queue"。核心语义是"无法到达目的地的数据"。

**是否适用于批量导入？** "Queue"（队列）一词暗示流式/消息管道的语义，严格来说更适合 Routine Load（Kafka）这类实时导入场景。对于 Stream Load、Broker Load 等批量导入，业界使用了多种不同的术语：

| 系统 | 术语 | 适用场景 |
|------|------|---------|
| Kafka Connect / RabbitMQ / Flink | **Dead Letter Queue / Topic** | 流式消息处理 |
| ClickHouse v25.8 | **`system.dead_letter_queue`** | Kafka/RabbitMQ 引擎（流式） |
| Amazon Redshift | **`STL_LOAD_ERRORS`** (Load Error Table) | COPY 命令（批量） |
| Vertica | **Rejected Data Table** | COPY（批量） |
| Teradata | **Error Table** | 批量加载 |
| Oracle | **Bad Table** (`COPY$_BAD`) | 批量加载 |
| Greenplum / HAWQ | **Error Table** | 批量加载 |
| Apache Doris / StarRocks 现有 | **Rejected Record** | 所有导入 |

**StarRocks 的命名选择**：

StarRocks 同时覆盖流式（Routine Load）和批量（Stream Load / Broker Load / INSERT）场景。可选的命名方向：

| 候选名称 | 表名 | 优点 | 缺点 |
|---------|------|------|------|
| **Dead Letter Queue** | `_rejected_records` | 业界最广为人知；ClickHouse 已采用 | "Queue" 暗示流式，批量场景略显不自然 |
| **Load Error Table** | `_load_errors` | 语义准确；Redshift `STL_LOAD_ERRORS` 先例 | 不如 DLQ 有辨识度 |
| **Rejected Data Table** | `_rejected_data` | 与现有 rejected record 概念一脉相承；Vertica 先例 | "rejected" 不暗示可重处理 |
| **Error Table** | `_error_table` | Teradata/Greenplum 先例；简洁 | 过于泛化，可能与其他 error 混淆 |
| **Quarantine Table** | `_quarantine` | 语义精准（"隔离待处理"）；暗示可重处理 | 业界使用较少 |

> **最终选择**：采用 **`_rejected_records`**，与 StarRocks 现有的 rejected record 概念一脉相承（Vertica 先例），同时暗示数据可被检查和重处理。

## 2. 业界方案调研

### 2.1 消息队列 / 流处理系统

| 系统 | DLQ 实现方式 | 特点 |
|------|-------------|------|
| **Apache Kafka (Connect)** | 独立的 DLQ Topic (`errors.tolerance=all`)，坏消息自动路由到 `dlq-<connector>` topic，原始 key/value 保留，错误元数据写入 Kafka Headers | 原生支持，零数据丢失，可重新消费 DLQ topic |
| **Apache Flink** | Side Output 机制（DataStream API），Confluent Flink SQL 支持 `error-handling.mode` 配置 DLQ table | 仅限反序列化错误，UDF 错误不支持 |
| **Kafka Connect** | `errors.tolerance=all` + `errors.deadletterqueue.topic.name`，支持 retry with exponential backoff | 行业标准模式，Header 携带丰富的错误上下文 |

**Kafka DLQ 最佳实践**：
- 保留原始消息的 key 和 value，不做包装
- 通过 Kafka Headers 附加元数据：原始 topic、partition、offset、错误消息、应用名称、错误时间戳
- 每个服务/connector 使用一个 DLQ topic（而非每个源 topic 一个）
- 区分瞬时错误（重试）和永久错误（进入 DLQ）
- 设置 DLQ 的监控和告警

### 2.2 数据仓库 / OLAP 系统

| 系统 | 错误处理方式 | DLQ 支持 | 特点 |
|------|-------------|----------|------|
| **ClickHouse** (v25.8+) | `kafka_handle_error_mode='dead_letter'` → `system.dead_letter_queue` 系统表 | **原生支持** | 解析失败的 Kafka/RabbitMQ 消息自动写入系统表，包含完整错误信息和源消息元数据 |
| **Snowflake** | Openflow Kafka Connector 支持 DLQ topic；`COPY INTO` 使用 `ON_ERROR` 参数 | **部分支持**（仅 Kafka Connector） | Kafka 连接器支持将解析失败的消息路由到 DLQ Kafka topic；通用数据导入（COPY INTO/Snowpipe）仍使用传统 ON_ERROR/VALIDATION_MODE |
| **Amazon Redshift** | `MAXERROR` 参数 + `STL_LOAD_ERRORS` / `SYS_LOAD_ERROR_DETAIL` 系统表 | **原生支持**（系统表） | 全局系统表持久化记录所有 COPY 命令的失败行详细信息，按 userid 自动行级隔离 |
| **Google BigQuery** | `max_bad_records` 参数控制容忍数量，Job 结果包含错误详情 | 无 | 坏行直接跳过，不进入独立存储 |
| **Apache Doris** | `max_filter_ratio` + `strict_mode`，`ErrorURL` 提供 HTTP 下载错误日志 | 无 | 与 StarRocks 现有机制基本相同（同源项目） |

#### 2.2.1 ClickHouse `system.dead_letter_queue` 详解 (v25.8+, 2025-08)

ClickHouse 在 v25.8 版本引入了原生 DLQ 支持，通过 `system.dead_letter_queue` 系统表存储从 Kafka/RabbitMQ 引擎中解析失败的消息。

**启用方式**：在 Kafka/RabbitMQ 引擎表上设置 `kafka_handle_error_mode='dead_letter'`。

**`system.dead_letter_queue` 表 Schema**：

| 列名 | 类型 | 说明 |
|------|------|------|
| `table_engine` | Enum8 | 流引擎类型：Kafka / RabbitMQ |
| `event_date` | Date | 消息消费日期 |
| `event_time` | DateTime | 消息消费时间 |
| `event_time_microseconds` | DateTime64 | 微秒精度时间 |
| `database` | LowCardinality(String) | 所属数据库 |
| `table` | LowCardinality(String) | 所属表名 |
| `error` | String | 错误文本 |
| `raw_message` | String | 原始消息体 |
| `kafka_topic_name` | String | Kafka topic 名 |
| `kafka_partition` | UInt64 | Kafka partition |
| `kafka_offset` | UInt64 | Kafka offset |
| `kafka_key` | String | Kafka 消息 key |

**查询示例**：
```sql
SELECT event_time, database, table, error, raw_message, kafka_topic_name, kafka_partition, kafka_offset
FROM system.dead_letter_queue
WHERE event_date = today()
ORDER BY event_time DESC
LIMIT 10;
```

**设计要点**：
- 系统级全局表，所有 Kafka/RabbitMQ 引擎表共享同一个 DLQ 表
- 数据不会自动删除，需要用户手动清理
- 通过 `flush_interval_milliseconds` 控制刷新频率
- 仅支持流引擎（Kafka/RabbitMQ）的反序列化错误，不支持 INSERT/文件导入等场景
- 演进历史：`default`（静默忽略）→ `stream`（虚拟列）→ `dead_letter`（系统表）

**对 StarRocks 的启发**：
- ClickHouse 的方案验证了"系统表存储 DLQ 数据 + SQL 可查询"模式的可行性
- 但其仅限于 Kafka/RabbitMQ 引擎，StarRocks 的 DLQ 应覆盖所有导入方式（Stream Load/Routine Load/Broker Load/INSERT）
- StarRocks 同样采用全局单表设计，通过自动 Row Access Policy 按 `target_database.target_table` 权限实现行级隔离，比 ClickHouse 的无行级过滤更精细

#### 2.2.2 Snowflake Openflow Kafka Connector DLQ

Snowflake 的 DLQ 支持通过 **Openflow Kafka Connector**（Apache Kafka with DLQ and metadata connector）实现：

**启用方式**：在 connector 配置中设置 `Kafka DLQ Topic` 参数（必填）。

**DLQ 行为**：
- **解析失败**：无效的 JSON/AVRO 格式消息路由到 DLQ topic
- **Schema 不匹配**：当 schema evolution 禁用时，不匹配 schema 的消息路由到 DLQ topic
- **处理错误**：其他 ingestion 过程中的处理失败

**局限**：
- 仅适用于 Openflow Kafka Connector，不适用于 `COPY INTO`、Snowpipe、Snowpipe Streaming 等通用导入
- DLQ 目标是 Kafka topic（而非 Snowflake 表），需要额外的 Kafka 基础设施
- Snowpipe Streaming 的 `ON_ERROR` 仅支持 `CONTINUE`，无 DLQ 选项

**对 StarRocks 的启发**：
- Snowflake 将 DLQ 限定在 Kafka Connector 范围且依赖外部 Kafka 基础设施，StarRocks 采用内置 OLAP 表方案覆盖所有导入方式，零外部依赖

#### 2.2.3 Amazon Redshift `STL_LOAD_ERRORS` / `SYS_LOAD_ERROR_DETAIL` 详解

Redshift 是数据仓库中最早通过系统表实现"类 DLQ"能力的系统，提供两代系统表用于记录 COPY 命令的加载错误。

**两代系统表对比**：

| 维度 | `STL_LOAD_ERRORS`（经典） | `SYS_LOAD_ERROR_DETAIL`（新一代，推荐） |
|------|--------------------------|---------------------------------------|
| 适用范围 | 仅 Provisioned Cluster 主集群 | Provisioned + Serverless + 并发扩展集群 |
| 数据保留 | 7 天日志轮转，不自动备份 | 同上 |
| 可用性 | Serverless 不可用 | 全平台可用 |

**`STL_LOAD_ERRORS` 表 Schema（20 列）**：

| 列名 | 类型 | 说明 |
|------|------|------|
| `userid` | integer | 执行加载的用户 ID |
| `slice` | integer | 发生错误的 slice |
| `tbl` | integer | 目标表 ID |
| `starttime` | timestamp | 加载开始时间（UTC） |
| `session` | integer | Session ID |
| `query` | integer | Query ID（可 JOIN 其他系统表） |
| `filename` | char(256) | 输入文件完整路径 |
| `line_number` | bigint | 出错行号（JSON 时为最后一行的行号） |
| `colname` | char(127) | 出错的列名 |
| `type` | char(10) | 出错列的数据类型 |
| `col_length` | char(10) | 列长度限制（如 `character(3)` 则为 `3`） |
| `position` | integer | 错误在字段中的位置 |
| `raw_line` | char(1024) | 包含错误的原始数据行 |
| `raw_field_value` | char(1024) | 导致解析错误的原始字段值 |
| `err_code` | integer | 错误码 |
| `err_reason` | char(100) | 错误原因说明 |
| `is_partial` | integer | 输入文件是否被分割（1=是） |
| `start_offset` | bigint | 分割偏移量（字节） |
| `copy_job_id` | bigint | COPY 作业标识符 |

**`SYS_LOAD_ERROR_DETAIL` 表 Schema（新一代，更简洁）**：

| 列名 | 类型 | 说明 |
|------|------|------|
| `user_id` | integer | 执行用户 ID |
| `query_id` | bigint | Query ID |
| `transaction_id` | bigint | 事务 ID |
| `session_id` | integer | Session ID |
| `database_name` | char(64) | 数据库名 |
| `table_id` | integer | 目标表 ID |
| `start_time` | timestamp | 加载开始时间 |
| `file_name` | char(256) | 输入文件路径 |
| `line_number` | bigint | 出错行号 |
| `column_name` | char(127) | 出错列名 |
| `column_type` | char(10) | 出错列类型 |
| `column_length` | char(10) | 列长度 |
| `position` | integer | 错误位置 |
| `error_code` | integer | 错误码 |
| `error_message` | char(512) | 错误消息（比 STL 版更长：512 vs 100） |

**关键特点**：

1. **全局共享系统表**：整个集群所有数据库、所有表的 COPY 错误统一存储在同一张系统表中
2. **自动行级权限隔离**（详见下文 2.5 节）：superuser 可见所有行，普通用户仅能看到自己产生的数据
3. **与 `STL_LOADERROR_DETAIL` 联合查询**可获取更详细的上下文信息
4. **7 天自动轮转**：系统表数据仅保留 7 天，如需长期保存需手动 UNLOAD 到 S3
5. **不含原始完整记录**：`raw_line` 限制为 char(1024)，超长记录会被截断

**典型查询示例**：

```sql
-- 查看最近一次 COPY 的错误
SELECT d.query, substring(d.filename, 14, 20),
       d.line_number AS line,
       substring(d.value, 1, 16) AS value,
       substring(le.err_reason, 1, 48) AS err_reason
FROM stl_loaderror_detail d, stl_load_errors le
WHERE d.query = le.query
  AND d.query = pg_last_copy_id();

-- 新一代 SYS 视图查询
SELECT query_id, table_id, start_time,
       trim(file_name) AS file_name,
       trim(column_name) AS column_name,
       trim(error_message) AS error_message
FROM sys_load_error_detail
WHERE query_id = 762949
ORDER BY start_time LIMIT 10;
```

**对 StarRocks 的启发**：
- Redshift 的 `userid` 自动行过滤是全局单表方案下权限隔离的成熟实践
- `raw_line` char(1024) 的截断设计说明原始数据存储需要在完整性和存储开销之间权衡
- 7 天自动轮转与 StarRocks 的 `partition_live_number` 方案思路一致
- `SYS_LOAD_ERROR_DETAIL` 比 `STL_LOAD_ERRORS` 更简洁的演进方向值得参考

### 2.2.4 全局/共享系统表的权限控制方案对比

以下是业界不同系统在 "一张表存储所有错误数据" 场景下的权限控制方案：

| 系统 | 权限模型 | 实现机制 | 粒度 |
|------|---------|---------|------|
| **Amazon Redshift** | **按 userid 自动行过滤** | 系统表内置行为：superuser 看所有行，普通用户只能看自己（`userid = current_user_id`）产生的行。可通过 `ALTER USER ... SYSLOG ACCESS UNRESTRICTED` 提权 | 用户级 |
| **ClickHouse** | **system 库全局可访问** | `system` 数据库默认对所有用户可访问（用于查询处理），`system.dead_letter_queue` 不做额外行级过滤。通过 `GRANT SELECT ON system.dead_letter_queue TO ...` 控制表级访问 | 表级 |
| **PostgreSQL (as DLQ)** | **标准 GRANT + RLS** | DLQ 表使用标准的 `GRANT` 控制表级访问；可通过 Row-Level Security (RLS) Policy 按用户/角色过滤行 | 表级 / 行级（需手动配置 RLS） |

**Redshift 权限模型深入分析**：

```
┌───────────────────────────────────────────────────────────┐
│                  Redshift 系统表权限模型                    │
│                                                            │
│  STL_LOAD_ERRORS / SYS_LOAD_ERROR_DETAIL                  │
│  ┌──────────────────────────────────────────────────┐     │
│  │ userid=1 (superuser)  → 可见所有行                │     │
│  │ userid=100 (user_a)   → 仅可见 userid=100 的行    │     │
│  │ userid=101 (user_b)   → 仅可见 userid=101 的行    │     │
│  │ userid=102 (unrestricted_user)                    │     │
│  │   ALTER USER unrestricted_user                    │     │
│  │     SYSLOG ACCESS UNRESTRICTED;                   │     │
│  │   → 提权后可见所有行                               │     │
│  └──────────────────────────────────────────────────┘     │
│                                                            │
│  特点：                                                     │
│  - 权限过滤在引擎层自动完成，对用户透明                       │
│  - 不需要 RLS policy，是系统表的内置行为                      │
│  - 粒度是 "谁执行的 COPY 谁看到"                             │
│  - 无法按目标表做精细权限控制                                 │
└───────────────────────────────────────────────────────────┘
```

### 2.3 数据处理框架

| 系统 | 错误处理方式 | 特点 |
|------|-------------|------|
| **Apache Spark** | `badRecordsPath` 将坏记录写入指定路径（JSON 格式），PERMISSIVE 模式将坏数据放入 `_corrupt_record` 列 | badRecordsPath 主要在 Databricks，开源版支持有限 |
| **PostgreSQL as DLQ** | 使用数据库表作为 DLQ，记录 source_table、record_id、payload（JSON）、error_message、attempt_count 等 | 可 SQL 查询，ACID 保证，`FOR UPDATE SKIP LOCKED` 实现并行重处理 |

### 2.4 核心设计模式总结

从业界方案中提炼出以下核心设计模式：

1. **分层错误处理**：区分瞬时错误（可重试）和永久错误（进入 rejected records）
2. **保留原始数据**：rejected records 中必须包含原始数据，以便重处理
3. **丰富的错误上下文**：错误原因、来源位置、时间戳、load 标签等
4. **可查询性**：rejected records 应当可通过标准 SQL 查询
5. **可重处理性**：提供机制将 rejected records 重新导入
6. **生命周期管理**：rejected records 应有 TTL 和清理策略
7. **可观测性**：metrics 和告警支持

## 3. 现有机制分析

### 3.1 Error Log 机制

```
数据流: Scanner/Sink → append_error_msg_to_file() → BE 本地文件
访问: ErrorURL → HTTP GET /api/_load_error_log?file=...
限制: 最多 50 行 (kMaxErrorNum)，仅记录 "Error: <reason>. Row: <line>"
```

**关键代码路径**:
- BE: `be/src/runtime/runtime_state_helper.cpp` → `append_error_msg_to_file()`
- BE: `be/src/runtime/load_path_mgr.cpp` → 文件路径管理
- FE: `QueryRuntimeProfile.java` → 收集 `tracking_url`

### 3.2 Rejected Record 机制

```
数据流: Scanner/Sink → append_rejected_record_to_file() → BE 本地文件
访问: rejected_record_path (BE 本地路径，无 HTTP 接口)
限制: 受 log_rejected_record_num 控制 (默认 0 = 不记录)
格式: record\terror_msg\tsource (tab 分隔)
```

**关键代码路径**:
- BE: `be/src/runtime/runtime_state_helper.cpp` → `append_rejected_record_to_file()`
- BE: `be/src/runtime/runtime_state.h` → `enable_log_rejected_record()`
- FE: `SessionVariable.java` → `log_rejected_record_num`
- FE: `QueryRuntimeProfile.java` → 收集 `rejected_record_path`

### 3.3 过滤控制机制

| 参数 | 作用域 | 说明 |
|------|--------|------|
| `max_filter_ratio` | Stream Load / Broker Load | 允许的最大过滤比例 (0~1) |
| `max_error_number` | Routine Load | 累计错误行数上限，超出后暂停 Job |
| `insert_max_filter_ratio` | INSERT 语句 | INSERT 允许的最大过滤比例 |
| `enable_insert_strict` / `strict_mode` | INSERT / Broker Load | 严格模式控制 |

### 3.4 现有机制的核心问题

```
┌─────────────────────────────────────────────────────────────────┐
│                     现有架构                                      │
│                                                                   │
│  Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ Table │
│                                              │                    │
│                                              ▼                    │
│                                    ┌──────────────────┐          │
│                                    │  BE Local Files   │          │
│                                    │  (error_log +     │          │
│                                    │   rejected_record)│          │
│                                    └──────────────────┘          │
│                                         │                        │
│                                    ❌ 不持久化                    │
│                                    ❌ 难以访问                    │
│                                    ❌ 无法重处理                  │
│                                    ❌ 数量受限                    │
│                                    ❌ 格式不标准                  │
└─────────────────────────────────────────────────────────────────┘
```

## 4. 概要设计

### 4.1 设计目标

| 目标 | 优先级 | 描述 |
|------|--------|------|
| 坏数据持久化存储 | P0 | 所有被过滤的行写入系统表，不丢失 |
| SQL 可查询 | P0 | 通过标准 SQL 查询被拒绝的数据 |
| 支持重新导入 | P1 | 从系统表中选取数据重新导入目标表 |
| 所有导入方式支持 | P1 | Stream Load、Routine Load、Broker Load、INSERT |
| 生命周期管理 | P1 | TTL 自动过期 + 手动清理 |
| 可观测性 | P2 | metrics + 告警 |
| 对导入性能影响最小 | P0 | rejected records 写入不应显著影响正常导入的吞吐和延迟 |

### 4.2 总体架构

采用**全局一张 rejected records 系统表**的设计（与 ClickHouse `system.dead_letter_queue`、Redshift `STL_LOAD_ERRORS` 一致），通过 `target_database` + `target_table` 列区分不同目标，自动 Row Access Policy 实现权限隔离，零外部依赖。

```
Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ db.table
                                            │
                                            ▼ (rejected rows)
                                   Rejected Record Writer (BE)
                                   → JSON Lines 本地文件
                                            │
                                            ▼ (RejectedRecordSyncDaemon 异步)
                              _statistics_._rejected_records
                              (全局 PK 表, merge commit 写入)
                                            │
                        ┌───────────────────┼───────────────────┐
                        ▼                                       ▼
             SELECT * FROM                          INSERT INTO target
             _statistics_._rejected_records            SELECT raw_record->>'col'
             WHERE target_table = '...'             FROM _statistics_._rejected_records
             (自动 Row Access Policy)               (replay)
```

**核心设计决策**：

| 决策 | 选择 | 理由 |
|------|------|------|
| 表粒度 | 全局单表 | 与 ClickHouse/Redshift 一致；最简单的管理模型；通过自动 Row Access Policy 解决权限隔离 |
| 权限模型 | 自动 Row Access Policy | FE 查询时自动注入行过滤，按 `target_database.target_table` 的 SELECT 权限隔离；对用户透明 |
| 存储方案 | StarRocks 内置 OLAP 表 | 零外部依赖；标准 SQL 查询和操作；复用现有存储引擎 |
| 分区方式 | 表达式分区 (`PARTITION BY date_trunc('day', created_at)`) | 按天自动分区 + `partition_live_number` TTL |
| 数据模型 | Primary Key | PK 表支持 UUID 去重，配合 at-least-once 重试实现 exactly-once 写入 |

### 4.3 系统表 Schema 详细设计

#### 4.3.1 业界 Schema 横向对比

先汇总各系统记录了哪些字段，再推导 StarRocks 的最优 Schema。

| 字段类别 | Redshift `STL_LOAD_ERRORS` | Redshift `SYS_LOAD_ERROR_DETAIL` | ClickHouse `system.dead_letter_queue` | StarRocks 现有 rejected record | 设计取舍 |
|---------|---------------------------|----------------------------------|--------------------------------------|-------------------------------|---------|
| **时间** | `starttime` | `start_time` | `event_time`, `event_time_microseconds`, `event_date` | 无 | 需要，且作为分区键 |
| **用户** | `userid` (行过滤依据) | `user_id` | 无 | 无 | 需要，用于审计和可选的用户级过滤 |
| **目标库** | 无（单库模型） | `database_name` | `database` | 无 | 需要，全局表必备 |
| **目标表** | `tbl` (表 ID) | `table_id` | `table` (表名) | 无 | 需要，用表名而非 ID（更易读，且权限检查基于名字） |
| **导入标识** | `query`, `session`, `copy_job_id` | `query_id`, `transaction_id`, `session_id` | 无 | 无 | 需要 `load_label`（StarRocks 导入体系的核心标识） + `txn_id` |
| **导入类型** | 隐含（仅 COPY） | 隐含（仅 COPY） | `table_engine`（Kafka/RabbitMQ） | 无 | 需要，StarRocks 有 4 种导入方式 |
| **错误码** | `err_code` (integer) | `error_code` (integer) | 无（错误文本） | 无 | 需要，用字符串枚举比 integer 更易读 |
| **错误消息** | `err_reason` char(100) | `error_message` char(512) | `error` (String) | error_msg (无长度限制) | 需要，VARCHAR(1024) 平衡信息量和存储 |
| **出错列** | `colname`, `type`, `col_length`, `position` | `column_name`, `column_type`, `column_length`, `position` | 无 | 无 | 需要列名；类型/位置信息包含在错误消息中即可 |
| **原始数据** | `raw_line` char(1024), `raw_field_value` char(1024) | 无 | `raw_message` (String) | record (无长度限制) | **核心字段**，统一 JSON 格式（列值序列化，格式无关） |
| **源文件/位置** | `filename` char(256), `line_number`, `is_partial`, `start_offset` | `file_name`, `line_number` | `kafka_topic_name`, `kafka_partition`, `kafka_offset`, `kafka_key` | source (无结构) | JSON 类型，灵活适配文件/Kafka/内存多种来源 |
| **数据格式** | 隐含 | 隐含 | 隐含 | 无 | 记录在 `source_info` JSON 中（`format` 字段） |
| **节点信息** | `slice` | 无 | 无 | 无 | `backend_id`，用于排障定位 |

#### 4.3.2 最终 Schema

```sql
CREATE TABLE _statistics_._rejected_records (
    -- ① 唯一标识（PK，幂等写入保证）
    id                  VARCHAR(64)     NOT NULL COMMENT 'UUID, BE 生成, 用于 at-least-once 重试去重',

    -- ② 时间（分区键）
    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                        COMMENT '记录写入时间',

    -- ③ 目标信息（权限过滤依据）
    target_database     VARCHAR(256)    NOT NULL COMMENT '目标数据库名',
    target_table        VARCHAR(256)    NOT NULL COMMENT '目标表名',

    -- ④ 导入上下文
    load_label          VARCHAR(256)    NOT NULL COMMENT '导入 Label (StarRocks 导入体系的唯一标识)',
    load_type           VARCHAR(32)     NOT NULL COMMENT 'STREAM_LOAD / ROUTINE_LOAD / BROKER_LOAD / INSERT',
    txn_id              BIGINT          COMMENT '事务 ID',
    user_name           VARCHAR(128)    COMMENT '执行导入的用户名',

    -- ⑤ 错误信息
    error_code          VARCHAR(64)     COMMENT '错误码枚举, 如 TYPE_MISMATCH / NULL_VIOLATION / PARSE_ERROR',
    error_message       VARCHAR(1024)   COMMENT '错误详细描述',
    error_column        VARCHAR(256)    COMMENT '出错的列名 (如果可确定)',

    -- ⑥ 被拒绝的行数据
    raw_record          JSON            COMMENT '被拒绝的行数据, 统一 JSON 格式 {col: value, ...}',

    -- ⑦ 来源信息
    source_info         JSON            COMMENT '来源元数据, 结构因导入类型而异',

    -- ⑧ 运行时信息
    backend_id          BIGINT          COMMENT '产生该记录的 BE 节点 ID'
) ENGINE = OLAP
PRIMARY KEY(id, created_at)
PARTITION BY date_trunc('day', created_at)
DISTRIBUTED BY HASH(id)
PROPERTIES (
    "partition_live_number" = "7",
    "replication_num" = "3"
);
```

#### 4.3.3 逐列设计说明

**① `id` — 唯一标识**

| 维度 | 设计 |
|------|------|
| 类型 | `VARCHAR(64)` NOT NULL |
| 作用 | Primary Key 第一列；后台同步 at-least-once 重试时的去重依据 |
| 生成 | BE 的 Rejected Record Writer 在 `append()` 时生成 UUID（`generate_uuid_string()`） |
| 为什么需要 | `RejectedRecordSyncDaemon` 通过 Stream Load 写入系统表，网络超时等场景下可能重试。PK 表保证同一 `id` 的记录只存一份，实现 at-least-once + 幂等 = exactly-once 语义 |

**② `created_at` — 时间**

| 维度 | 设计 |
|------|------|
| 类型 | `DATETIME` |
| 作用 | Primary Key 第二列；分区键（按天）；查询过滤；TTL 基准 |
| 来源 | ClickHouse 用 `event_time`，Redshift 用 `start_time`，均以时间为主键/分区 |

**③ `target_database` + `target_table` — 目标信息**

| 维度 | 设计 |
|------|------|
| 类型 | `VARCHAR(256)` NOT NULL |
| 作用 | 标识坏数据来自哪张表；**Row Access Policy 的过滤依据** |
| 来源 | ClickHouse 用 `database` + `table`（表名）；Redshift 用 `tbl`（表 ID） |
| 选择表名而非 ID | 表 ID 需要 JOIN 元数据表才能可读，表名直接可读且权限检查基于 `db.table` 名称 |
| 查询效率 | 非 PK 前缀列，但高频查询条件可通过 secondary index 或 bitmap index 加速 |

**④ 导入上下文**

| 列 | 类型 | 说明 |
|----|------|------|
| `load_label` | `VARCHAR(256)` NOT NULL | StarRocks 导入体系的核心标识。Stream Load、Broker Load、Routine Load 每次任务都有唯一 label，是用户排查问题的第一锚点。Redshift 对应 `query_id` + `copy_job_id`。 |
| `load_type` | `VARCHAR(32)` NOT NULL | 区分四种导入方式。ClickHouse 用 `table_engine` 区分 Kafka/RabbitMQ；StarRocks 需要区分更多类型。 |
| `txn_id` | `BIGINT` | 事务 ID，可与 `information_schema.loads` 等系统表关联。Redshift 对应 `transaction_id`。 |
| `user_name` | `VARCHAR(128)` | 执行导入的用户名。Redshift 的 `userid` 是行过滤的核心；StarRocks 的 Row Access Policy 基于 target_table 权限而非 userid，但保留 user_name 用于审计。 |

**⑤ 错误信息**

| 列 | 类型 | 说明 |
|----|------|------|
| `error_code` | `VARCHAR(64)` | 字符串枚举（如 `TYPE_MISMATCH`），比 Redshift 的 integer `err_code` 更易读，无需查错误码文档。 |
| `error_message` | `VARCHAR(1024)` | Redshift 从 STL 的 100 字符演进到 SYS 的 512 字符；ClickHouse 无限制。取 1024 字符平衡信息量和存储。 |
| `error_column` | `VARCHAR(256)` | 出错的列名。Redshift 额外记录了 `type`/`col_length`/`position`，但这些信息完全可以包含在 `error_message` 中，无需独立列。 |

**⑥ `raw_record` — 被拒绝的行数据**

| 列 | 类型 | 说明 |
|----|------|------|
| `raw_record` | `JSON` | 被拒绝的行数据，**统一使用 JSON 格式**（`{col_name: value, ...}`）。 |

**核心设计决策：`raw_record` 使用 JSON 类型，存储目标表列值的序列化**。

#### 为什么选择 JSON

原始输入格式各异（CSV/JSON/Parquet/ORC/Avro），且在 BE 的不同拒绝阶段，原始格式的文本可能已不可获取（详见 5.1 节）。需要选择一种统一的 `raw_record` 存储格式。候选方案对比：

| 候选 | 存储内容 | replay 方式 | 优点 | 缺点 |
|------|---------|------------|------|------|
| **JSON** (`{col: val}`) | 目标表列名→列值的 KV 对 | `raw_record->>'col'` 提取值 | 自描述、带列名；StarRocks 原生 JSON 类型支持箭头运算符提取；replay 时不依赖列序 | 序列化开销；嵌套类型（ARRAY/MAP/STRUCT）需递归序列化 |
| **CSV** (逗号分隔值) | 列值按目标表列序拼接 | 按位置 split | 序列化最简单（现有 `rebuild_csv_row()` 已实现） | 不自描述——无列名，replay 时必须严格按位置对齐；值中包含逗号/换行需转义；`debug_item()` 对字符串加单引号，与标准 CSV 不兼容 |
| **VARCHAR 原始文本** | 原始输入文本 | 按原始格式解析 | 保真度最高 | Parquet/ORC 无原始文本；Sink 阶段所有格式的原始文本都不可获取；不同格式 replay 方式不同 |

**选择 JSON 的理由**：

1. **自描述**：`{"order_id": 10001, "amount": "bad"}` 带列名，用户一眼可读，replay 时 `raw_record->>'col'` 按名提取，不依赖列序。CSV 的 `10001,bad` 丢失了列名信息，replay 必须知道列序。
2. **replay 统一**：所有格式、所有阶段的 replay SQL 完全相同（`raw_record->>'col_name'`），无需区分原始格式。
3. **StarRocks 原生支持**：JSON 是 StarRocks 的一等类型，支持 `->>` / `->` / `json_query` / `get_json_string` 等丰富的提取函数。存储在系统表中使用 StarRocks 的二进制 JSON 格式（flatbuffers），比文本 JSON 更紧凑且查询更快。
4. **复杂类型兼容**：ARRAY、MAP、STRUCT 等嵌套类型可自然序列化为 JSON 的数组/对象，CSV 无法表达。

**不选 CSV 的理由**：现有 `rebuild_csv_row()` 看似可直接复用，但存在根本问题——它调用 `debug_item()` 构建值，字符串类型会加单引号包裹（`'Alice'` 而非 `Alice`），数值直接输出，没有统一的转义机制。这不是标准 CSV，无法直接作为可重导入的格式使用。

**不选原始文本的理由**：Parquet/ORC 无原始文本，Sink 阶段所有格式的原始文本都已不可获取。强制保留原始文本意味着只能覆盖 Scanner 阶段的 CSV/JSON，覆盖面不完整。

#### JSON 序列化的构建方式

Rejected Record Writer 新增 `build_json_record()` 方法（不复用 `debug_item()`），直接从 Column 的原生值构建标准 JSON：

```cpp
// 伪代码
JsonValue build_json_record(const Chunk& chunk, size_t row_idx,
                            const vector<string>& col_names) {
    JsonObjectBuilder builder;
    for (size_t i = 0; i < chunk.num_columns(); i++) {
        const Column* col = chunk.get_column_by_index(i);
        if (col->is_null(row_idx)) {
            builder.add_null(col_names[i]);
        } else if (col->is_numeric()) {
            builder.add_number(col_names[i], col->get_numeric(row_idx));
        } else if (col->is_binary()) {
            builder.add_string(col_names[i], col->get_slice(row_idx));  // 无引号包裹
        } else {
            builder.add_string(col_names[i], col->debug_item(row_idx)); // fallback
        }
    }
    return builder.build();
}
```

#### Scanner 解析失败的降级处理

当记录在 Scanner 阶段因解析彻底失败被拒绝时（如 CSV 列数不匹配无法拆列、JSON 格式非法），Chunk 中无完整列值可用。此时 `raw_record` 降级为包含原始文本的 JSON：`{"_raw": "原始文本内容"}`。

#### 移除 `record_format` 列

`raw_record` 统一为 JSON 后，不再需要标记原始输入格式。原始格式信息如有需要，可记录在 `source_info` 中。

**⑦ `source_info` — 来源信息**

| 维度 | 设计 |
|------|------|
| 类型 | `JSON` |
| 设计理由 | 不同导入来源的元数据结构完全不同，用固定列会导致大量 NULL 或需要为每种来源单独建列。Redshift 用 `filename` + `line_number` + `is_partial` + `start_offset` 共 4 列（仅适用于文件来源）；ClickHouse 用 `kafka_topic_name` + `kafka_partition` + `kafka_offset` + `kafka_key` 共 4 列（仅适用于 Kafka）。StarRocks 需要同时覆盖文件、Kafka、INSERT 等多种来源，JSON 是最灵活的选择。 |

各导入类型的 `source_info` 结构：

```json
// Stream Load / Broker Load（文件来源）
{
    "format": "csv",
    "file": "hdfs://namenode/data/orders.csv",
    "line": 42,
    "offset": 8192
}

// Broker Load（Parquet 来源，无行号）
{
    "format": "parquet",
    "file": "gs://bucket/data/orders.parquet"
}

// Routine Load（Kafka 来源）
{
    "format": "json",
    "topic": "orders_topic",
    "partition": 3,
    "offset": 156789,
    "key": "order_001"
}

// INSERT INTO ... SELECT（查询来源）
{
    "source_fragment": "INSERT INTO orders SELECT * FROM staging.orders_raw"
}
```

**⑧ `backend_id` — 运行时信息**

Redshift 有 `slice`（计算分片）标识。`backend_id` 记录产生该 rejected record 的 BE 节点，用于排障定位写入侧问题。

#### 4.3.4 未纳入 Schema 的字段及理由

| 字段 | 来源 | 不纳入原因 |
|------|------|-----------|
| `status` / `retry_count` / `resolved_at` | 初版设计 | 系统表定位为数据记录，不是工作流引擎。重处理由用户通过 SQL 自行编排（查询 → 修正 → INSERT INTO 目标表 → DELETE）。虽然 PK 表支持 UPDATE，但引入状态管理会增加使用复杂度。 |
| `id` (AUTO_INCREMENT) | 初版设计 | 改为 UUID（VARCHAR(64)），由 BE 生成。不用 AUTO_INCREMENT 是因为多 BE 并行写入本地文件，自增 ID 无法跨 BE 保证唯一。 |
| `column_type` / `column_length` / `position` | Redshift | 这些详细的列元数据信息可以包含在 `error_message` 中，独立建列收益低但会增加 schema 复杂度。 |
| `raw_field_value` | Redshift | Redshift 额外记录出错字段的原始值。`raw_record` JSON 已包含所有列值，`error_column` 已标识出错列。 |
| `record_format` | 初版设计 | `raw_record` 统一为 JSON 格式后不再需要。原始输入格式如有需要，可记录在 `source_info` 中。 |
| `kafka_topic` / `kafka_partition` / `kafka_offset` / `kafka_key` | ClickHouse | 作为独立列仅适用于 Kafka 来源，用 `source_info` JSON 统一承载更灵活。 |
| `session_id` | Redshift | StarRocks 的导入体系以 `load_label` + `txn_id` 为核心标识，session 信息可从 FE audit log 关联。 |
| `load_job_id` | 初版设计 | 非所有导入方式都有 job_id（Stream Load 没有）。`load_label` 是更通用的标识。 |

#### 4.3.5 表属性设计说明

| 属性 | 值 | 说明 |
|------|---|------|
| `ENGINE` | `OLAP` | 标准 StarRocks 存储引擎 |
| `PRIMARY KEY` | `(id, created_at)` | PK 模型保证同一 `id` 的记录去重，实现 at-least-once 幂等写入 |
| `PARTITION BY` | `date_trunc('day', created_at)` | 表达式分区，按天自动创建。`created_at` 作为 PK 第二列同时用于分区裁剪 |
| `DISTRIBUTED BY` | `HASH(id)` | PK 表需要 hash 分布，`id` 为 UUID 保证均匀分布 |
| `partition_live_number` | `7` | 默认保留 7 天，与 Redshift 的 7 天系统表日志轮转一致 |
| `replication_num` | `3` | 默认 3 副本保证数据可靠性（shared-data 模式下无需关注） |

### 4.4 配置设计

#### 4.4.1 导入级别配置

通过导入属性控制 rejected records 行为：

```bash
# Stream Load
curl -H "log_rejected_record_num: 10000" \
     -T data.csv \
     http://fe:8030/api/db/table/_stream_load

# Broker Load
LOAD LABEL my_label (
    DATA INFILE("hdfs://...")
    INTO TABLE my_table
)
WITH BROKER
PROPERTIES (
    "log_rejected_record_num" = "10000"
);

# Routine Load
CREATE ROUTINE LOAD my_job ON my_table
PROPERTIES (
    "log_rejected_record_num" = "10000"
);
```

#### 4.4.2 Session 变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `log_rejected_record_num` | `0` | `0` = 关闭；`-1` = 不限制，记录所有被拒绝行；正数 N = 最多记录 N 条。**复用现有参数**，语义扩展：之前仅控制 BE 本地文件记录数，现在同时控制写入系统表。 |

```sql
SET log_rejected_record_num = -1;  -- 记录所有被拒绝的行到系统表
INSERT INTO my_table SELECT * FROM source_table;
```

#### 4.4.3 全局配置

FE 配置 (`fe.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `rejected_record_ttl_days` | `7` | 系统表默认分区保留天数（`partition_live_number`） |

BE 配置 (`be.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `rejected_record_sync_interval_sec` | `30` | `RejectedRecordSyncDaemon` 后台同步间隔 |
| `rejected_record_local_retention_hours` | `24` | 本地文件最长保留时间（同步失败兜底） |

### 4.5 SQL 接口设计

`_statistics_._rejected_records` 位于内部数据库下。用户通过标准 SQL 查询和操作，Row Access Policy 自动过滤可见行。

#### 4.5.1 查询

```sql
-- 查看某个目标表的 rejected records（普通用户自动按权限过滤）
SELECT * FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND error_code = 'TYPE_MISMATCH'
ORDER BY created_at DESC
LIMIT 100;

-- 统计某个库某个表的各类错误分布
SELECT error_code, COUNT(*) as cnt, MAX(created_at) as latest
FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
GROUP BY error_code
ORDER BY cnt DESC;

-- 查看某次导入的所有 rejected records
SELECT * FROM _statistics_._rejected_records
WHERE load_label = 'load_orders_20260327';

-- 管理员查看全局概览（admin 无行过滤）
SELECT target_database, target_table, error_code, COUNT(*) as cnt
FROM _statistics_._rejected_records
WHERE created_at >= current_date() - INTERVAL 1 DAY
GROUP BY target_database, target_table, error_code
ORDER BY cnt DESC;
```

#### 4.5.2 重新导入

```sql
-- 从 rejected records 重新导入到目标表（修正 schema 后）
-- raw_record 已是 JSON 格式 {col: val, ...}，可直接提取列值
INSERT INTO db1.orders (order_id, customer_name, amount, created_at)
SELECT
    raw_record->'order_id',
    raw_record->'customer_name',
    CAST(raw_record->'amount' AS DECIMAL(10,2)),
    raw_record->'created_at'
FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-26';
```

#### 4.5.3 管理操作

```sql
-- 调整数据保留天数（默认 7 天）
ALTER TABLE _statistics_._rejected_records SET ("partition_live_number" = "14");

-- 手动清理（需 admin 权限）
TRUNCATE TABLE _statistics_._rejected_records;

-- 清理某个目标表的历史数据
DELETE FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND created_at < '2026-03-01';
```

### 4.6 错误分类

定义标准化的错误码体系：

| 错误码 | 分类 | 说明 | 可重试性 |
|--------|------|------|----------|
| `PARSE_ERROR` | 格式错误 | CSV/JSON/Avro 解析失败 | 否（需修正数据） |
| `TYPE_MISMATCH` | 类型错误 | 字段值不匹配目标类型 | 否（需修正数据或 schema） |
| `NULL_VIOLATION` | 约束违反 | NOT NULL 列遇到 NULL 值 | 否（需修正数据或 schema） |
| `VALUE_OUT_OF_RANGE` | 值域错误 | 数值溢出或超出范围 | 否（需修正数据） |
| `COLUMN_MISMATCH` | 结构错误 | 列数不匹配 | 否（需修正数据格式） |
| `PARTITION_NOT_FOUND` | 分区错误 | 无法找到匹配的分区 | 可能（分区创建后可重试） |
| `ENCODING_ERROR` | 编码错误 | 字符编码问题 | 否（需修正数据） |
| `TRANSFORM_ERROR` | 转换错误 | 列映射/表达式计算失败 | 否（需修正转换逻辑） |
| `UNIQUE_VIOLATION` | 唯一约束 | 主键冲突（特定场景） | 可能 |
| `INTERNAL_ERROR` | 系统错误 | BE 内部错误 | 是（可重试） |

### 4.7 与现有机制的关系

**`_rejected_records` 系统表完全替代现有 rejected record 文件机制**。error log 作为轻量级调试手段保留（50 行上限，面向 ErrorURL）。

```
                     log_rejected_record_num
                                │
                    ┌───────────┼───────────┐
                    │ OFF       │           │ ON
                    ▼           │           ▼
           ┌───────────────┐   │   ┌───────────────┐
           │ 现有行为        │   │   │ 新行为          │
           │                │   │   │                │
           │ error_log      │   │   │ error_log      │ ← 保留(50行调试)
           │ rejected_record│   │   │ 系统表写入       │ ← 替代 rejected_record 文件
           │ max_filter_ratio│  │   │ max_filter_ratio│ ← 不变
           └───────────────┘   │   └───────────────┘
```

**替代关系**：

| 维度 | rejected_record 文件 (废弃) | `_rejected_records` 系统表 (替代) |
|------|----------------------|-----------|
| 存储 | BE 本地文件 | OLAP 表 |
| 控制 | `log_rejected_record_num` (默认 0) | `log_rejected_record_num` (扩展语义: 0=关闭, -1=不限, N=最多N条) |
| 数据内容 | `record \t error_msg \t source` | 结构化 14 列（含 UUID），`raw_record` 统一 JSON |
| 实现入口 | `append_rejected_record_to_file()` | `RejectedRecordWriter::append_from_chunk()` / `append_from_slices()` / `append_raw()` |

`log_rejected_record_num` 参数语义扩展后向后兼容：`0` = 关闭（与现有默认行为一致），正数/`-1` = 启用系统表写入（新行为）。长期计划废弃 `append_rejected_record_to_file()`。

## 5. 关键技术设计

### 5.1 BE 侧 Rejected Record Writer 详细设计

#### 5.1.1 现有 rejected record 的拒绝点分析

通过代码分析，当前 `append_rejected_record_to_file()` 和 `append_error_msg_to_file()` 的调用点分布在两个阶段，**每个阶段可获取的原始数据不同**：

**阶段一：Scanner（解析/类型转换）**

| 格式 | 调用位置 | 现有 `record` 参数 | 可获取的行数据 |
|------|---------|-------------------|-------------------|
| CSV | `csv_scanner.cpp` → `_report_rejected_record()` | `record.to_string()` — 原始 CSV 文本行 | **完整**：各列原始文本值 → JSON |
| JSON | `json_scanner.cpp` → `_construct_row` 失败 | `row.raw_json()` — 原始 JSON 对象子串 | **完整**：原始 JSON 直接使用 |
| Avro | `avro_scanner.cpp` / `avro_reader.cpp` | `""` 或 `datum_to_json()` | **完整**：datum JSON 序列化 |
| Parquet | `arrow_to_starrocks_converter.cpp` → `report_error_message()` | 仅列级上下文（`file=, column=, raw_data=`） | **完整**：filter 前 Chunk 中该行所有列值可用 → JSON |
| ORC | `orc_chunk_reader.cpp` → `report_error_message()` | `""` 空字符串 | **完整**：`_broker_load_filter` 标记行，filter 前 Chunk 中该行所有列值可用 → JSON |

**阶段二：Sink（约束校验/分区/长度/精度）**

| 调用位置 | `record` 参数内容 | 原始数据可用性 |
|---------|------------------|-------------|
| `tablet_sink.cpp` → `_validate_data()` | `chunk->rebuild_csv_row(j, ",")` — 从 output chunk 的**列值重建的 CSV 行** | **重建行**（非原始文本，是 StarRocks 内部列值的 CSV 拼接） |
| `tablet_sink.cpp` → partition out of range | 同上 | 同上 |
| `file_scanner.cpp` → strict mode 过滤 | `src->rebuild_csv_row(i, ",")` — 从 source chunk 重建 | 同上（含 `TODO(meegoo): support other file format` 注释） |

**关键结论**：
- 在 Sink 阶段，所有格式（包括 CSV/JSON）的原始文本都**已不可用**，只能获得 `rebuild_csv_row()` 重建行
- Parquet/ORC 在任何阶段都**没有行级原始数据**的概念
- 因此，Rejected Record Writer 的 `raw_record` 字段不能保证存储"原始输入行"，而是**当前阶段可获取的最佳行级表示**

#### 5.1.2 Rejected Record Writer 接口设计

`RejectedRecordWriter` 替代 `append_rejected_record_to_file()`，提供统一的记录收集接口：

```cpp
// be/src/runtime/dlq_writer.h
class RejectedRecordWriter {
public:
    // 方式一: 从 Chunk 列值构建 JSON (Sink 阶段 / 已解析的行)
    void append_from_chunk(
        const Chunk& chunk,                // 包含被拒绝行的 Chunk
        size_t row_index,                  // 被拒绝的行号
        const std::vector<std::string>& col_names,  // 列名列表
        const std::string& error_code,     // 错误码枚举
        const std::string& error_message,  // 错误详情
        const std::string& error_column,   // 出错列名 (可选)
        const std::string& source_info     // 来源 JSON (可选)
    );

    // 方式二: 从原始各列文本值构建 (CSV Scanner 阶段, Chunk 不完整但原始 Slice 可用)
    void append_from_slices(
        const std::vector<Slice>& col_values,  // 各列的原始文本 Slice
        const std::vector<std::string>& col_names,
        const std::string& error_code,
        const std::string& error_message,
        const std::string& error_column,
        const std::string& source_info
    );

    // 方式三: 从原始文本构建 (Scanner 解析彻底失败, 无法拆列)
    void append_raw(
        const std::string& raw_text,       // 原始文本 → 存为 {"_raw": "..."}
        const std::string& error_code,
        const std::string& error_message,
        const std::string& error_column,
        const std::string& source_info
    );

    Status flush();

private:
    DLQRecordBuffer _buffer;
    // target_database, target_table, load_label 等上下文从 RuntimeState 获取
    std::string build_json_from_chunk(const Chunk& chunk, size_t row_index,
                                      const std::vector<std::string>& col_names);
};
```

**调用点改造**（以 `tablet_sink.cpp` 为例）：

```cpp
// 现有代码 (rejected_record):
if (state->enable_log_rejected_record()) {
    RuntimeStateHelper::append_rejected_record_to_file(
        state, chunk->rebuild_csv_row(j, ","), error_msg, "");
}

// 改造后 (rejected records):
if (state->enable_dlq()) {
    state->dlq_writer()->append_from_chunk(
        *chunk, j, _output_col_names,        // Chunk + 行号 + 列名 → JSON
        "NULL_VIOLATION",                     // error_code
        error_msg,                            // error_message
        desc->col_name(),                     // error_column
        ""                                    // source_info
    );
}
```

**以 `csv_scanner.cpp` 类型转换失败为例**（各列 Slice 可用但 Chunk 不完整）：

```cpp
// 现有代码:
_report_rejected_record(record, error_msg);  // record.to_string()

// 改造后: 从各列原始 Slice 构建 JSON
state->dlq_writer()->append_from_slices(
    _get_column_slices(row),                 // 各列的原始文本 Slice
    _src_col_names,                          // 列名列表
    "TYPE_MISMATCH",
    error_msg,
    slot->col_name(),
    "{\"file\":\"" + _curr_reader->filename() + "\"}"
);
```

**以 CSV 列数不匹配为例**（无法拆列，只有原始行文本）：

```cpp
// 改造后: 原始文本降级
state->dlq_writer()->append_raw(
    record.to_string(),                      // → {"_raw": "..."}
    "COLUMN_MISMATCH",
    error_msg,
    "",
    "{\"file\":\"" + _curr_reader->filename() + "\"}"
);
```

#### 5.1.3 `raw_record` 的具体内容与重导入方式

`raw_record` 统一输出为 JSON，但构建来源因**拒绝阶段**和**输入格式**不同而异。下面按导入 pipeline 的阶段逐一说明。

##### 阶段 A：Scanner 解析/类型转换（原始数据可能可用）

此阶段发生在 Scanner 将原始输入解析为 StarRocks 内部 Chunk 的过程中。**关键区分**：文本格式（CSV/JSON）有原始行文本可用，列式格式（Parquet/ORC）没有。

**A1. CSV — 列数不匹配 / UTF-8 非法 / 类型转换失败**

现有代码中，CSV Scanner 在 `_parse_csv()` 循环中逐行解析。每行是一个 `CSVReader::Record`，其 `to_string()` 返回原始 CSV 文本行。当某列的 `read_string_for_adaptive_null_column()` 转换失败时，整行被拒绝，**此时原始 CSV 行可用**。

```
原始 CSV 行:     10001,Alice,not_a_number,2026-03-27
                      ↓
                 解析到第3列 amount 失败 (INT 类型收到 "not_a_number")
                      ↓
raw_record 构建: 直接从 record.to_string() + 列名映射 → JSON
```

```json
// raw_record 内容
{"order_id": "10001", "customer_name": "Alice", "amount": "not_a_number", "created_at": "2026-03-27"}
```

构建方式：CSV Scanner 在解析时已知每列的原始文本值（`row.columns[j]` 是原始 Slice），将所有列的原始文本值按列名组装成 JSON。注意：此处的值是**原始文本字符串**（不做类型转换），因为转换本身就是失败原因。

重导入：
```sql
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    raw_record->>'customer_name',
    CAST(raw_record->>'amount' AS DECIMAL(10,2)),  -- 修正数据或改用宽类型
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'TYPE_MISMATCH';
```

**A2. JSON — 字段构造失败**

JSON Scanner 使用 simdjson 解析每个 JSON 对象，`raw_json()` 返回原始 JSON 子串。当 `_construct_row()` 失败时，**原始 JSON 对象可用**。

```
原始 JSON:       {"order_id": 10001, "amount": "abc", "ts": "2026-03-27"}
                      ↓
                 _construct_row 失败 (amount 类型转换)
                      ↓
raw_record 构建: 直接使用 raw_json() 的原始 JSON 对象
```

```json
// raw_record 内容 — 就是原始 JSON 对象本身
{"order_id": 10001, "amount": "abc", "ts": "2026-03-27"}
```

构建方式：`raw_json()` 返回的就是 JSON，无需转换。

重导入：与 CSV 相同的 SQL 模式。

**A3. Parquet / ORC — 列读取后 filter 拒绝**

Parquet/ORC 是列式格式，不存在"原始行文本"。但 StarRocks 读取后会组装为 Chunk（行的 batch），每行的所有列值在 Chunk 中是**完整存在**的。

关键流程（以 ORC 为例，Parquet 类似）：

```
ORC 文件 ──→ OrcChunkReader::_fill_chunk()
              │
              ├── 逐列读取，写入 Chunk
              ├── 某行某列违反约束 (如 NOT NULL 列有 null)
              │     → _broker_load_filter[row] = 0  (标记该行)
              │     → report_error_message(error_msg) (仅写 error log)
              │
              ├── 所有列读完后，Chunk 中该行的所有列值是完整的
              │
              └── chunk->filter(*_broker_load_filter)  ← 坏行在此被移除
                                    ↑
                                    │
                        Rejected Record Writer 需要在 filter 之前介入
```

**关键洞察**：虽然 ORC/Parquet 的 `report_error_message()` 只写了 error log（传入空字符串/列级信息），没有调 `append_rejected_record_to_file()`。但在 `chunk->filter()` 执行之前，Chunk 中被 `_broker_load_filter` 标记为 0 的行的**所有列值都是完整可用的**。

```
介入时机:  在 chunk->filter(*_broker_load_filter) 之前
              遍历 filter 中值为 0 的行号
              从 Chunk 中提取这些行的所有列值 → JSON
```

```json
// raw_record 内容 — 从 Chunk 完整列值构建 (filter 之前)
{"order_id": 10001, "customer_name": "Alice", "amount": null, "created_at": "2026-03-27"}
```

构建方式：在 `_fill_chunk()` 返回后、`chunk->filter()` 执行前，遍历 `_broker_load_filter` 找到被拒绝的行号，调用 `build_json_record()` 从 Chunk 列值构建 JSON。

重导入：
```sql
-- Parquet 被拒绝的行，raw_record 中有完整列值，可直接 replay
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    raw_record->>'customer_name',
    CAST(raw_record->'amount' AS DECIMAL(10,2)),  -- 修正数据或改宽类型
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'NULL_VIOLATION';
```

**Parquet 的 Arrow 转换路径**：与 ORC 类似，`ArrowConvertContext::report_error_message()` 当前仅记录列级上下文到 error log。同样需要在 filter 之前从 Chunk 提取完整行。

**A4. Avro — datum 转换失败**

Avro Scanner 将 Avro datum 转为 JSON（`AvroUtils::datum_to_json()`），然后构造 Chunk。

```json
// raw_record 内容 — Avro datum 的 JSON 序列化
{"order_id": 10001, "amount": "\u0000\u0001"}
```

构建方式：`datum_to_json()` 已返回 JSON，直接使用。

##### 阶段 B：Expr/Strict Mode（Chunk 列值可用，原始文本不可用）

此阶段在 `file_scanner.cpp` 的 `materialize()` 中，strict mode 检查列值转换。数据已经在 source Chunk 中。

```
source Chunk:    [10001, "Alice", null (转换失败), "2026-03-27"]
                      ↓
                 strict mode 发现 amount 原值非 null 但转换后为 null → 拒绝
                      ↓
raw_record 构建: 从 source Chunk 列值构建 JSON
```

```json
// raw_record 内容
{"order_id": 10001, "customer_name": "Alice", "amount": null, "created_at": "2026-03-27"}
```

构建方式：调用 `build_json_record()` 遍历 source Chunk 列值构建 JSON。现有代码使用 `rebuild_csv_row(i, ",")`，改为 `build_json_record()`。

重导入：同上。

##### 阶段 C：Sink 约束校验（output Chunk 列值可用，原始文本不可用）

此阶段在 `tablet_sink.cpp` 的 `_validate_data()` 中，对 output Chunk 做最终校验。数据已经过所有表达式计算和类型转换。**不论原始输入格式是 CSV/JSON/Parquet/ORC，到 Sink 阶段都是完全相同的 Chunk 列值。**

```
output Chunk:    [10001, "Alice_very_long_name_exceeds_64_chars...", 99.99, "2026-03-27"]
                      ↓
                 _validate_data: customer_name VARCHAR(64) 超长 → 拒绝
                      ↓
raw_record 构建: 从 output Chunk 列值构建 JSON
```

```json
// raw_record 内容 — Sink 阶段所有格式统一
{"order_id": 10001, "customer_name": "Alice_very_long_name_exceeds_64_chars...", "amount": 99.99, "created_at": "2026-03-27"}
```

各种 Sink 拒绝场景：

| 拒绝原因 | `error_code` | `raw_record` 示例 |
|---------|-------------|-------------------|
| NOT NULL 列为 null | `NULL_VIOLATION` | `{"id": 1, "name": null, "age": 25}` |
| VARCHAR 超长 | `VALUE_OUT_OF_RANGE` | `{"id": 1, "name": "超长字符串...(200字符)", "age": 25}` |
| Decimal 溢出 | `VALUE_OUT_OF_RANGE` | `{"id": 1, "price": 99999999999.99, "qty": 1}` |
| 分区找不到 | `PARTITION_NOT_FOUND` | `{"id": 1, "dt": "2099-01-01", "val": 42}` |

构建方式：调用 `build_json_record()` 遍历 output Chunk 列值构建 JSON。

重导入：
```sql
-- 修正 VARCHAR 超长问题后重导入
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    LEFT(raw_record->>'customer_name', 64),     -- 截断到合法长度
    CAST(raw_record->'amount' AS DECIMAL(10,2)),
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'VALUE_OUT_OF_RANGE';
```

##### 阶段总结

| 阶段 | 格式 | raw_record 来源 | 数据完整性 | 可否 replay |
|------|------|----------------|----------|-----------|
| A (Scanner) | CSV | 原始行 `record.to_string()` + 各列 `row.columns[j]` Slice → JSON | **完整**：各列的原始文本值均可获取（含失败列的原始文本） | **可以** |
| A (Scanner) | JSON | `raw_json()` 原始 JSON 子串 | **完整**：原始 JSON 对象本身 | **可以** |
| A (Scanner) | Parquet/ORC | filter 前 Chunk 列值 → JSON | **完整**：`_fill_chunk()` 先读完所有列再 filter，失败列的值可能为 null | **可以**（null 列需用户修正） |
| A (Scanner) | Avro | `datum_to_json()` | **完整**：Avro datum 的 JSON 序列化 | **可以** |
| B (Strict) | 所有 | source Chunk 列值 → JSON | **完整**：所有列已成功解析到 Chunk | **可以** |
| C (Sink) | 所有 | output Chunk 列值 → JSON | **完整**：所有列经过表达式计算，格式无关 | **可以** |

**关键代码级细节**：

- **CSV Scanner 阶段 A**：当第 j 列类型转换失败时（`csv_scanner.cpp:442`），第 0~j-1 列已 append 到 Chunk，第 j~N 列尚未 append，Chunk 中该行列值**不完整**。但此时 `record`（原始 CSV 行）和 `row.columns`（各列原始 Slice）仍然完整可用，直接从原始 Slice 构建 JSON，**不依赖 Chunk**。
- **Parquet/ORC Scanner 阶段 A**：`_fill_chunk()` 先**逐列全部读完**写入 Chunk，再用 `_broker_load_filter` 标记并 filter。在 `chunk->filter()` 之前介入。失败列（如 NOT NULL 违反）的值在 Chunk 中为 null，其他列完整。
- **阶段 B/C**：数据已完整存在于 Chunk 中，所有格式统一，无差异。

**结论：所有格式、所有阶段都能构建出等价的完整 JSON 记录用于 replay。** CSV/JSON 在 Scanner 阶段从原始文本构建（保真度最高），Parquet/ORC 在 Scanner 阶段从 Chunk 列值构建（失败列为 null），Sink 阶段所有格式统一从 Chunk 构建。

#### 5.1.4 写入路径设计

##### 写入方案：BE 本地文件缓存 + 后台批量同步

Rejected records 先写入 BE 本地文件（与现有 rejected record 相同的路径），由后台守护线程定期批量同步到 `_statistics_._rejected_records` 表。

```
┌──────────────────────────────────────────────────────────────────────┐
│                      导入路径（同步，零事务开销）                       │
│                                                                       │
│  Scanner ──→ [Expr/Strict] ──→ OlapTableSink (主表)                  │
│     │              │                 │                                 │
│     │(rejected)    │(rejected)       │(rejected)                      │
│     ▼              ▼                 ▼                                 │
│  ┌──────────────────────────────────────────────────┐                │
│  │ RejectedRecordWriter::append_from_chunk/slices/raw()        │                │
│  │   → 构建 JSON 记录                                │                │
│  │   → 追加写入 BE 本地 rejected record 文件                      │                │
│  │     (与现有 append_rejected_record_to_file 同路径) │                │
│  └──────────────────────────────────────────────────┘                │
│                                                                       │
│  主导入完成后，文件路径上报给 FE (通过 TReportExecStatusParams)    │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                  后台同步（异步，批量事务）                             │
│                                                                       │
│  BE RejectedRecordSyncDaemon (后台线程，类似 load_error_log 清理线程):           │
│                                                                       │
│    每 N 秒 / 积累到 M 条记录:                                         │
│      1. 扫描本地 rejected record 文件目录                              │
│      2. 读取多个文件 → 合并构造 Chunk                            │
│      3. 通过内部 Stream Load 批量写入                                  │
│         _statistics_._rejected_records                                   │
│      4. 写入成功 → 删除本地文件                                        │
│      5. 写入失败 → 保留文件，下次重试                                  │
│                                                                       │
│  批量效果:                                                             │
│    100 个小 Stream Load 各产生 1 条坏行                                │
│    → 100 个本地文件                                                     │
│    → 1 个批量 Stream Load 事务写入 100 条 rejected records                     │
└──────────────────────────────────────────────────────────────────────┘
```

##### 本地文件格式

Rejected record 本地文件使用 JSON Lines 格式（每行一个 JSON 对象），文件路径复用现有的 `LoadPathMgr` 管理：

```
<storage_path>/dlq/<db_name>/<label>_<fragment_id>.jsonl
```

每行内容：
```json
{"id":"550e8400-e29b-41d4-a716-446655440000","created_at":"2026-03-27 10:30:00","target_database":"db1","target_table":"orders","load_label":"load_001","load_type":"INSERT","txn_id":12345,"user_name":"root","error_code":"TYPE_MISMATCH","error_message":"Cannot cast 'abc' to INT","error_column":"amount","raw_record":{"order_id":"10001","customer_name":"Alice","amount":"abc"},"source_info":{"format":"parquet","file":"gs://bucket/data.parquet"},"backend_id":10001}
```

`id` 字段由 BE 的 Rejected Record Writer 在 append 时生成（`generate_uuid_string()`），写入本地文件。后台同步时原样写入 PK 表，保证重试幂等。

##### 后台同步设计

**BE 侧 — `RejectedRecordSyncDaemon`**：

```cpp
class RejectedRecordSyncDaemon : public Thread {
    void run() {
        while (!_stopped) {
            sleep(sync_interval_sec);  // 默认 30s
            scan_and_sync();
        }
    }

    void scan_and_sync() {
        auto files = scan_dlq_directory();
        if (files.empty()) return;

        auto chunk = read_and_merge(files);

        // 通过内部 Stream Load + merge commit 写入
        // merge commit 使多个 BE 的 Stream Load 合并到同一个事务
        auto status = internal_stream_load(chunk, {
            .enable_merge_commit = true,
            .target_table = "_statistics_._rejected_records",
            .enable_dlq = false,  // 防递归
        });

        if (status.ok()) {
            delete_synced_files(files);
        }
        // 失败 → 保留文件，下次重试
        // PK 表 + UUID 保证重试幂等：同一 id 的记录不会重复
    }
};
```

**关键机制：merge commit + PK 去重**

1. **merge commit**：`RejectedRecordSyncDaemon` 的 Stream Load 开启 `enable_merge_commit`，FE 将多个 BE 同一时间窗口内的 Stream Load 合并到同一个事务。这样 N 个 BE 同时同步，最终只产生 1 个事务（而非 N 个）。

2. **PK 去重保证幂等**：系统表是 Primary Key 表，每条记录有 BE 生成的 UUID（`id` 列）。如果 Stream Load 因网络超时等原因需要重试，相同 `id` 的记录会被 PK 表自动去重。这实现了 **at-least-once 投递 + PK 去重 = exactly-once** 语义。

```
BE-1 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┐
BE-2 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┤──→ FE merge commit ──→ 1 个事务
BE-3 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┘
```

**同步参数**：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `rejected_record_sync_interval_sec` | `30` | 后台同步间隔（秒） |
| `rejected_record_sync_max_batch_rows` | `10000` | 单次同步最大行数 |
| `rejected_record_local_retention_hours` | `24` | 本地文件最长保留时间（同步失败兜底） |

**事务优化效果**：

| 场景 | 1:1 txn 方案 | 本地缓存 + merge commit |
|------|-------------|----------------------|
| 100 个 Stream Load，各 1 条坏行，3 个 BE | 100 txn | **1 txn**（3 个 BE merge commit） |
| Routine Load 100 个 task，3 个 BE | 100 txn | **1~5 txn**（按同步周期 merge） |
| 1 个大 Broker Load，3 个 BE 各产生坏行 | 3 txn | **1 txn** |

##### 数据可见性时序

```
t=0    导入开始
t=5    导入完成，rejected record 文件写入 BE 本地磁盘
       用户查 _statistics_._rejected_records → 暂无数据
t=35   RejectedRecordSyncDaemon 触发同步，读取本地文件，通过 merge commit 批量写入
       用户查 _statistics_._rejected_records → 数据可见
```

默认 30 秒同步间隔，用户在导入完成后最多等待 30 秒即可查询 rejected records。对于 `inspect + replay` 的使用模式完全可接受。

#### 5.1.5 核心设计要点

1. **统一入口三种方法**：`append_from_chunk()` 从 Chunk 列值构建 JSON（Sink/Strict/Parquet/ORC 阶段），`append_from_slices()` 从原始文本 Slice 构建 JSON（CSV Scanner 阶段），`append_raw()` 降级包装原始文本（`{"_raw": "..."}`，解析彻底失败时）。
2. **上下文注入**：`target_database`、`target_table`、`load_label`、`load_type`、`txn_id`、`user_name`、`backend_id` 从 `RuntimeState` 一次性获取。
3. **本地文件写入**：rejected records 序列化为 JSON Lines 写入 BE 本地文件。写入路径与现有 `append_rejected_record_to_file()` 相同——本地 append，零事务开销，不影响导入延迟。
4. **后台异步同步**：BE 上的 `RejectedRecordSyncDaemon` 守护线程定期扫描本地 rejected record 文件，聚合多个文件/多个导入任务的坏行，通过 1 次 Stream Load 批量写入 `_statistics_._rejected_records`。
5. **Best-effort**：本地文件写入失败时静默跳过。后台同步失败时保留文件下次重试，`rejected_record_local_retention_hours` 超时后清理。
6. **生命周期**：Rejected Record Writer 随 fragment 创建/销毁，fragment 结束时确保文件 flush。`RejectedRecordSyncDaemon` 是 BE 级别的常驻守护线程。

#### 5.1.6 关键设计约束

**约束一：导入路径零事务开销**

导入路径上只做本地文件 append，不开事务、不做 BRPC、不与 FE 交互。所有事务开销转移到后台 `RejectedRecordSyncDaemon`，与主导入完全解耦。

**约束二：数据可见性有秒级延迟**

数据在导入完成后不立即可见，需等待 `RejectedRecordSyncDaemon` 的下一次同步周期（默认 30 秒）。对于"导入完成后手动检查 + replay"的使用模式完全可接受。

**约束三：递归防护**

`RejectedRecordSyncDaemon` 通过内部 Stream Load 写入 `_statistics_._rejected_records` 时，对该写入禁用 rejected records 功能（RuntimeState 检查目标表名）。

**约束四：Parquet/ORC 列读取失败 vs 行级约束违反的区分**

ORC/Parquet 的 `_broker_load_filter` 标记的行是**行级约束违反**（如 NOT NULL 列有 null），此时该行所有列值在 Chunk 中完整。但如果是**列读取本身失败**（如文件损坏导致某列 `get_next()` 返回 Error），则整个 batch 失败，不产生逐行的 rejected records——这种情况是导入级别的失败，不是行级别的拒绝。

### 5.2 FE 侧管理

```
┌─────────────────────────────────────────────────────┐
│                   FE                                 │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ RejectedRecordManager                                     │  │
│  │                                                │  │
│  │ - FE 启动时确保 _statistics_._rejected_records │  │
│  │   存在（类似 information_schema 初始化）        │  │
│  │ - 向 TQueryOptions 注入配置参数                 │  │
│  │ - 查询时注入 Row Access Policy 行过滤          │  │
│  │ - 汇总统计信息                                 │  │
│  └───────────────────────────────────────────────┘  │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ 导入流程集成                                   │  │
│  │                                                │  │
│  │ StreamLoadPlanner / RoutineLoadJob / BulkLoad: │  │
│  │   - 检查 log_rejected_record_num > 0            │  │
│  │   - 向 TQueryOptions 注入参数                  │  │
│  │   - 报告统计信息                               │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**表创建时机**：`_statistics_._rejected_records` 由 `RejectedRecordManager`（FrontendDaemon）在 leader 节点启动后异步初始化，与 `StatisticsMetaManager` 创建统计表的模式一致。`RejectedRecordManager` 检查 `_statistics_` 库中是否已存在 `_rejected_records` 表，不存在则通过 `CreateTableStmt` 创建。

### 5.3 Thrift 接口扩展

```thrift
// InternalService.thrift
struct TQueryOptions {
    // ... existing fields ...

    // Rejected records configuration
    optional i64 log_rejected_record_num;              // 0=关闭, -1=不限, N=最多N条
}

// FrontendService.thrift
struct TReportExecStatusParams {
    // ... existing fields ...

    optional i64 rejected_records_written;             // BE 上报写入文件的记录数
    optional string rejected_record_file_path;       // BE 本地文件路径
}
```

Thrift 接口非常轻量：FE 只需在 `TQueryOptions` 中设置 `log_rejected_record_num`，不需要下发系统表的 tablet 信息（后台同步由 BE 自行发起 Stream Load）。

### 5.4 表生命周期

```
FE 启动
  │
  ▼
RejectedRecordManager 初始化 _statistics_._rejected_records
(表达式分区, partition_live_number = 7)
  │
  ├──→ 正常运行：按天自动创建分区，过期分区自动删除
  │
  ├──→ ALTER TABLE ... SET ("partition_live_number" = "14")
  │         → 调整保留策略
  │
  └──→ TRUNCATE TABLE / DELETE FROM ...
           → 手动清理（需 admin 权限）
```

`_rejected_records` 是全局基础设施，生命周期与集群一致。任何 database 或 table 被删除，不影响系统表的存在和已有数据（历史坏数据仍可查询）。

## 6. Metrics 设计

### 6.1 BE Metrics

| Metric | 类型 | 说明 |
|--------|------|------|
| `starrocks_be_rejected_record_total` | Counter | 累计写入的 rejected record 总数 |
| `starrocks_be_rejected_record_failed` | Counter | 写入失败的记录数 |
| `starrocks_be_rejected_record_sync_total` | Counter | 后台同步累计写入系统表的记录数 |
| `starrocks_be_rejected_record_sync_failed` | Counter | 后台同步失败次数 |

### 6.2 FE Metrics

| Metric | 类型 | 说明 |
|--------|------|------|
| `starrocks_fe_rejected_record_total` | Counter | rejected record 写入总数 |

## 7. 性能考量

### 7.1 正常路径 (Happy Path) 零开销

当 `log_rejected_record_num = 0` 时，不应有任何额外开销。实现方式：
- 编译时：通过条件编译或常量折叠
- 运行时：在 `RuntimeState` 中增加 `_enable_dlq` 标志位，在 append 路径入口快速判断

### 7.2 坏数据路径性能

导入路径上只做本地文件 append，不增加额外延迟。后台 `RejectedRecordSyncDaemon` 通过 merge commit 批量写入系统表，事务开销极低。

### 7.3 存储成本预估

假设平均每条 rejected record 2KB（包含原始数据和元数据）：
- 1 万条/天 → ~20MB/天 → ~140MB/周
- 100 万条/天 → ~2GB/天 → ~14GB/周
- 1 亿条/天 → ~200GB/天 → 需要考虑采样或限制

建议通过 `log_rejected_record_num` 限制单次导入的记录数，避免极端场景下的存储爆炸。

## 8. 权限控制设计

### 8.1 问题与方案选择

全局单表意味着所有数据库、所有表的坏数据混在一起。必须保证：用户只能看到自己有 SELECT 权限的 `target_database.target_table` 对应的行。

业界方案：Redshift 按 `userid` 自动行过滤（粒度不够——按导入者而非目标表）；ClickHouse 仅做表级 GRANT（无行级隔离）。

StarRocks 采用 **自动 Row Access Policy**：FE 在查询 `_rejected_records` 时自动注入行过滤谓词，复用已有的 `SecurityPolicyRewriteRule` + `Authorizer` 基础设施。

### 8.2 实现方式

```sql
-- 用户原始查询:
SELECT * FROM _statistics_._rejected_records
WHERE error_code = 'TYPE_MISMATCH';

-- FE Analyzer 改写后（概念示意）:
SELECT * FROM _statistics_._rejected_records dlq
WHERE error_code = 'TYPE_MISMATCH'
  AND EXISTS (
    SELECT 1 FROM <内部权限视图> p
    WHERE p.database_name = dlq.target_database
      AND p.table_name = dlq.target_table
      AND p.user_name = current_user()
      AND p.privilege = 'SELECT'
  );
```

| 用户角色 | 行为 |
|---------|------|
| Admin / root | 不注入过滤，可见全部行 |
| 普通用户 | 自动注入 EXISTS 子查询过滤 |
| 无任何表 SELECT 权限的用户 | 查询返回空集 |

具体实现可在 `QueryAnalyzer` 中将权限检查内联为谓词函数（如 `has_table_privilege(current_user(), target_database, target_table, 'SELECT')`），避免物化权限表。

### 8.3 写入与管理权限

| 操作 | 权限要求 |
|------|---------|
| 数据写入 | 系统内部（BE RejectedRecordWriter），用户无直接 INSERT 权限 |
| SELECT 查询 | 自动 Row Access Policy 过滤 |
| DELETE 清理 | 需要 ADMIN 权限 |
| TRUNCATE | 需要 ADMIN 权限 |
| ALTER TABLE (调整 TTL) | 需要 ADMIN 权限 |

### 8.4 其他安全考虑

- **数据敏感性**：`raw_record` 列包含原始数据，可能含敏感信息。如需脱敏，可对该列应用 Column Masking Policy（StarRocks 已支持 `Authorizer.getColumnMaskingPolicy()`）。
- **审计**：系统表的查询、DELETE、TRUNCATE 操作需记录审计日志。

## 9. 兼容性

- **向后兼容**：`log_rejected_record_num` 默认 0（关闭），不影响现有行为。
- **upgrade path**：升级后用户可选择性启用。
- **降级**：降级时 `_rejected_records` 表将作为普通 OLAP 表保留，但不再自动写入。

## 10. 与现有 reject record 机制的对比

| 维度 | 现有 Reject Record | `_rejected_records` 系统表 (新方案) |
|------|-------------------|-------------|
| 存储位置 | BE 本地文件系统 | StarRocks 全局 OLAP 表 `_statistics_._rejected_records` |
| 持久性 | 临时文件，依赖 BE 节点 | 持久化，跨节点可用 |
| 可查询性 | 需 SSH 到 BE 节点 | 标准 SQL 查询 |
| 权限控制 | 无 | 自动 Row Access Policy（按 target_table 权限过滤） |
| 数据完整性 | 受 `log_rejected_record_num` 限制 | 可配置，默认记录所有。PK + UUID 保证 exactly-once |
| 错误分类 | 无结构化分类 | 标准错误码体系 |
| 重处理能力 | 无 | `INSERT INTO target SELECT ... FROM _statistics_._rejected_records` |
| 生命周期 | 依赖 BE 清理策略 | 表达式分区 + `partition_live_number` 自动过期（默认 7 天） |
| 可观测性 | 无专门 metrics | 完整的 metrics 体系 |
| 对性能影响 | 本地文件写入，低 | 涉及表写入，需异步和批量优化 |
| 外部依赖 | 无 | 无（纯 StarRocks 内置） |
| 适用场景 | 调试 | 生产环境数据质量保障 |

## 11. 需求覆盖分析

逐项核对原始需求与当前设计的覆盖情况：

| 需求 | 当前设计覆盖 | 实现方式 |
|------|------------|---------|
| rejected rows 写入可查询的表 | ✅ | `_statistics_._rejected_records` 全局 OLAP 表，SQL 可查询 |
| 支持 10,000+ 行 | ✅ | `log_rejected_record_num` 可配置，默认 -1（不限制） |
| 结构化错误信息 | ✅ | `error_code`, `error_message`, `error_column` 等结构化列 |
| 可 replay 重导入 | ✅ | `raw_record` 为 JSON 格式，`INSERT INTO target SELECT raw_record->>'col' FROM _statistics_._rejected_records` |
| 支持 Parquet 场景 | ✅ | 5.1.3 节详细设计了 Parquet 在各阶段的 `raw_record` 构建方式（Chunk 列值 → JSON） |
| INSERT INTO ... SELECT ... FROM FILES() | ✅ | INSERT 语句通过 session 变量 `log_rejected_record_num` 启用 |
| max_filter_ratio 协同 | ✅ | 启用后，被 `max_filter_ratio` 允许的过滤行自动写入系统表，过滤行为不变 |
| Vertica 式的使用体验 | ✅ | Vertica 的 `REJECTED DATA AS TABLE` 对应本设计的全局系统表 + SQL 查询 + replay |

### 11.3 AppLovin 端到端使用流程

```sql
-- 1. 启用 rejected records
SET log_rejected_record_num = -1;    -- 记录所有被拒绝行
SET insert_max_filter_ratio = 0.01;  -- 允许 1% 过滤

-- 2. 批量导入（GCS Parquet → 聚合表）
INSERT INTO agg_table
SELECT * FROM FILES(
    "path" = "gs://bucket/data/*.parquet",
    "format" = "parquet",
    "gs.credential.json" = '...'
);
-- 即使有坏行，只要不超过 1%，导入成功

-- 3. 检查被拒绝的行
SELECT error_code, error_column, error_message, raw_record
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND load_label = '<本次导入的 label>'
ORDER BY created_at;

-- 4. 分析错误模式
SELECT error_code, error_column, COUNT(*) as cnt
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND created_at > '2026-03-27'
GROUP BY error_code, error_column
ORDER BY cnt DESC;

-- 5. 修正后 replay（例如修正了上游 Parquet 生成逻辑后）
INSERT INTO agg_table (col1, col2, col3)
SELECT
    CAST(raw_record->'col1' AS INT),
    raw_record->>'col2',
    CAST(raw_record->'col3' AS DECIMAL(10,2))
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-27';

-- 6. 清理已处理的数据
DELETE FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND created_at > '2026-03-27';
```


## 12. 里程碑计划

### Phase 1: 基础框架

- [ ] `_statistics_._rejected_records` 表初始化（FE RejectedRecordManager）
- [ ] 自动 Row Access Policy 实现
- [ ] Rejected Record Writer 实现（BE：异步批量写入）
- [ ] Stream Load 支持
- [ ] INSERT INTO 支持
- [ ] 基本 metrics
- [ ] 导入级别配置 + Session 变量

### Phase 2: 完整导入支持 + 运维

- [ ] Broker Load 支持
- [ ] Routine Load 支持
- [ ] 完整 metrics 和监控集成
- [ ] 文档和用户指南

### Phase 3: 高级功能

- [ ] 数据可视化（Web UI 集成）
