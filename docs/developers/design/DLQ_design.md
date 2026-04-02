# Dead Letter Queue (DLQ) 概要设计

> **作者**: StarRocks Team
> **状态**: Draft
> **创建日期**: 2026-03-27

## 1. 背景与动机

### 1.1 问题描述

StarRocks 在数据导入过程中（Stream Load、Routine Load、Broker Load、INSERT INTO）会遇到各种数据质量问题：类型转换失败、NOT NULL 约束违反、字段缺失、格式解析错误等。当前系统通过 **reject record 机制** 和 **error log 机制** 来处理这些坏数据，但存在以下痛点：

1. **坏数据丢失，无法恢复**：被过滤的行仅记录在 BE 本地文件中，不具备持久化和恢复能力，无法重新导入。
2. **访问不便**：rejected record 文件存储在 BE 本地磁盘，用户需要 SSH 到对应的 BE 节点才能查看，没有便捷的 HTTP 下载接口（error log 有 `ErrorURL`，但 rejected record 没有）。
3. **信息不完整**：error log 硬编码最多记录 50 行（`kMaxErrorNum = 50`），rejected record 数量受 `log_rejected_record_num` 控制（默认不记录）。
4. **格式不标准**：rejected record 以 tab 分隔写入文件，没有结构化的元数据（如错误原因分类、源文件位置、时间戳等）。
5. **不支持重试/重处理**：没有自动或手动重新导入坏数据的机制。
6. **Routine Load 场景下问题尤为突出**：Kafka 消息一旦消费后 offset 前移，被过滤的坏数据将永久丢失，无法回溯。

### 1.2 期望目标

引入 Dead Letter Queue (DLQ) 机制，实现：

- **坏数据不丢失**：所有因数据质量问题被过滤的记录，自动路由到可持久化的 DLQ 存储中。
- **便捷查询**：用户可通过标准 SQL 查询 DLQ 中的数据，了解错误详情。
- **支持重处理**：用户可以在修正数据或 schema 后，将 DLQ 中的数据重新导入目标表。
- **可观测性**：提供 DLQ 相关的 metrics，监控坏数据的产生速率和处理情况。

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
- ClickHouse 使用全局共享系统表，StarRocks 采用 per-database 粒度，兼顾简洁性和数据库级权限隔离

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
- Snowflake 将 DLQ 限定在 Kafka Connector 范围是一个局限，StarRocks 应提供更通用的 DLQ 支持
- Snowflake 将坏消息写回 Kafka topic 的方案可以作为 StarRocks Routine Load 场景的一个可选 DLQ sink

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

### 2.5 全局/共享 DLQ 表的权限控制方案对比

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

1. **分层错误处理**：区分瞬时错误（可重试）和永久错误（进入 DLQ）
2. **保留原始数据**：DLQ 中必须包含原始数据，以便重处理
3. **丰富的错误上下文**：错误原因、来源位置、时间戳、load 标签等
4. **可查询性**：DLQ 数据应当可通过标准 API/SQL 查询
5. **可重处理性**：提供机制将 DLQ 数据重新导入
6. **生命周期管理**：DLQ 数据应有 TTL 和清理策略
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

## 4. DLQ 概要设计

### 4.1 设计目标

| 目标 | 优先级 | 描述 |
|------|--------|------|
| 坏数据持久化存储 | P0 | 所有被过滤的行写入 DLQ 表，不丢失 |
| SQL 可查询 | P0 | 通过标准 SQL 查询 DLQ 数据 |
| 支持重新导入 | P1 | 从 DLQ 中选取数据重新导入目标表 |
| 所有导入方式支持 | P1 | Stream Load、Routine Load、Broker Load、INSERT |
| 生命周期管理 | P1 | TTL 自动过期 + 手动清理 |
| 可观测性 | P2 | DLQ metrics + 告警 |
| 对导入性能影响最小 | P0 | DLQ 写入不应显著影响正常导入的吞吐和延迟 |

### 4.2 总体架构

采用 **全局一张 DLQ 表** 的设计（与 ClickHouse `system.dead_letter_queue`、Redshift `STL_LOAD_ERRORS` 一致），通过 `target_database` + `target_table` 列区分不同目标，自动 Row Access Policy 实现权限隔离，零外部依赖。

```
┌───────────────────────────────────────────────────────────────────────┐
│                           DLQ 架构                                     │
│                                                                         │
│  Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ db.table   │
│                                              │                          │
│                                              ▼                          │
│                                    ┌───────────────────┐               │
│                                    │   DLQ Writer       │               │
│                                    │  (BE Pipeline)     │               │
│                                    └─────────┬─────────┘               │
│                                              │                          │
│                                              ▼                          │
│                          ┌─────────────────────────────────┐           │
│                          │  _starrocks_sys._dead_letter_q  │           │
│                          │  (全局单表，系统数据库下)         │           │
│                          │                                  │           │
│                          │ target_database = 'db1'          │           │
│                          │ target_table    = 'orders'       │           │
│                          │ load_label      = 'load_001'     │           │
│                          │ error_code      = 'TYPE_MISMATCH'│           │
│                          │ raw_record      = '...'          │           │
│                          └─────────────────────────────────┘           │
│                                       │                                 │
│                   ┌───────────────────┼────────────────────┐           │
│                   ▼                                        ▼           │
│   ┌───────────────────────────┐   ┌─────────────────────────────┐    │
│   │  SELECT * FROM            │   │  INSERT INTO db1.orders      │    │
│   │  _starrocks_sys           │   │  SELECT parse_json(raw_record│    │
│   │    ._dead_letter_q        │   │  FROM _starrocks_sys         │    │
│   │  WHERE target_table=...   │   │    ._dead_letter_q           │    │
│   │                           │   │  WHERE ...                   │    │
│   │  (自动 Row Access Policy  │   │                              │    │
│   │   按 target_table 权限    │   │  ← 重新导入                   │    │
│   │   过滤可见行)              │   │                              │    │
│   └───────────────────────────┘   └─────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘
```

**核心设计决策**：

| 决策 | 选择 | 理由 |
|------|------|------|
| DLQ 表粒度 | 全局单表 | 与 ClickHouse/Redshift 一致；最简单的管理模型；通过自动 Row Access Policy 解决权限隔离 |
| 权限模型 | 自动 Row Access Policy（方案 B） | FE 查询时自动注入行过滤，按 `target_database.target_table` 的 SELECT 权限隔离；对用户透明 |
| 存储方案 | StarRocks 内置 OLAP 表 | 零外部依赖；标准 SQL 查询和操作；复用现有存储引擎 |
| 分区方式 | 表达式分区 (`PARTITION BY date_trunc('day', created_at)`) | 按天自动分区 + `partition_live_number` TTL |
| 数据模型 | Duplicate Key | DLQ 是纯追加（append-only）写入，无更新需求 |

### 4.3 DLQ 表 Schema 详细设计

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
| **原始数据** | `raw_line` char(1024), `raw_field_value` char(1024) | 无 | `raw_message` (String) | record (无长度限制) | **核心字段**，VARCHAR(1048576) 1MB 上限 |
| **源文件/位置** | `filename` char(256), `line_number`, `is_partial`, `start_offset` | `file_name`, `line_number` | `kafka_topic_name`, `kafka_partition`, `kafka_offset`, `kafka_key` | source (无结构) | JSON 类型，灵活适配文件/Kafka/内存多种来源 |
| **数据格式** | 隐含 | 隐含 | 隐含 | 无 | 需要，便于重处理时选择解析方式 |
| **节点信息** | `slice` | 无 | 无 | 无 | `backend_id`，用于排障定位 |

#### 4.3.2 最终 Schema

```sql
CREATE TABLE _starrocks_sys._dead_letter_q (
    -- ① 时间（分区键）
    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                        COMMENT '记录写入时间',

    -- ② 目标信息（权限过滤依据）
    target_database     VARCHAR(256)    NOT NULL COMMENT '目标数据库名',
    target_table        VARCHAR(256)    NOT NULL COMMENT '目标表名',

    -- ③ 导入上下文
    load_label          VARCHAR(256)    NOT NULL COMMENT '导入 Label (StarRocks 导入体系的唯一标识)',
    load_type           VARCHAR(32)     NOT NULL COMMENT 'STREAM_LOAD / ROUTINE_LOAD / BROKER_LOAD / INSERT',
    txn_id              BIGINT          COMMENT '事务 ID',
    user_name           VARCHAR(128)    COMMENT '执行导入的用户名',

    -- ④ 错误信息
    error_code          VARCHAR(64)     COMMENT '错误码枚举, 如 TYPE_MISMATCH / NULL_VIOLATION / PARSE_ERROR',
    error_message       VARCHAR(1024)   COMMENT '错误详细描述',
    error_column        VARCHAR(256)    COMMENT '出错的列名 (如果可确定)',

    -- ⑤ 原始数据
    raw_record          VARCHAR(1048576) COMMENT '原始记录原文, 上限 1MB',
    record_format       VARCHAR(16)     COMMENT '记录格式: csv / json / avro / orc / parquet',

    -- ⑥ 来源信息
    source_info         JSON            COMMENT '来源元数据, 结构因导入类型而异',

    -- ⑦ 运行时信息
    backend_id          BIGINT          COMMENT '产生该记录的 BE 节点 ID'
) ENGINE = OLAP
DUPLICATE KEY(created_at, target_database, target_table)
PARTITION BY date_trunc('day', created_at)
DISTRIBUTED BY RANDOM
PROPERTIES (
    "partition_live_number" = "7",
    "replication_num" = "1"
);
```

#### 4.3.3 逐列设计说明

**① `created_at` — 时间**

| 维度 | 设计 |
|------|------|
| 类型 | `DATETIME` |
| 作用 | 分区键（按天）；查询过滤；TTL 基准 |
| 来源 | ClickHouse 用 `event_time`，Redshift 用 `start_time`，均以时间为主键/分区 |
| 排序位置 | Duplicate Key 第一列，保证分区裁剪和时间范围查询效率 |

**② `target_database` + `target_table` — 目标信息**

| 维度 | 设计 |
|------|------|
| 类型 | `VARCHAR(256)` NOT NULL |
| 作用 | 标识坏数据来自哪张表；**Row Access Policy 的过滤依据** |
| 来源 | ClickHouse 用 `database` + `table`（表名）；Redshift 用 `tbl`（表 ID） |
| 选择表名而非 ID | 表 ID 需要 JOIN 元数据表才能可读，表名直接可读且权限检查基于 `db.table` 名称 |
| 排序位置 | Duplicate Key 第二、三列，保证 `WHERE target_database = 'x' AND target_table = 'y'` 的查询效率 |

**③ 导入上下文**

| 列 | 类型 | 说明 |
|----|------|------|
| `load_label` | `VARCHAR(256)` NOT NULL | StarRocks 导入体系的核心标识。Stream Load、Broker Load、Routine Load 每次任务都有唯一 label，是用户排查问题的第一锚点。Redshift 对应 `query_id` + `copy_job_id`。 |
| `load_type` | `VARCHAR(32)` NOT NULL | 区分四种导入方式。ClickHouse 用 `table_engine` 区分 Kafka/RabbitMQ；StarRocks 需要区分更多类型。 |
| `txn_id` | `BIGINT` | 事务 ID，可与 `information_schema.loads` 等系统表关联。Redshift 对应 `transaction_id`。 |
| `user_name` | `VARCHAR(128)` | 执行导入的用户名。Redshift 的 `userid` 是行过滤的核心；StarRocks 的 Row Access Policy 基于 target_table 权限而非 userid，但保留 user_name 用于审计。 |

**④ 错误信息**

| 列 | 类型 | 说明 |
|----|------|------|
| `error_code` | `VARCHAR(64)` | 字符串枚举（如 `TYPE_MISMATCH`），比 Redshift 的 integer `err_code` 更易读，无需查错误码文档。 |
| `error_message` | `VARCHAR(1024)` | Redshift 从 STL 的 100 字符演进到 SYS 的 512 字符；ClickHouse 无限制。取 1024 字符平衡信息量和存储。 |
| `error_column` | `VARCHAR(256)` | 出错的列名。Redshift 额外记录了 `type`/`col_length`/`position`，但这些信息完全可以包含在 `error_message` 中，无需独立列。 |

**⑤ 原始数据**

| 列 | 类型 | 说明 |
|----|------|------|
| `raw_record` | `VARCHAR(1048576)` (1MB) | **DLQ 的核心价值**——保留原始数据用于重处理。Redshift 的 `raw_line` 仅 char(1024) 严重不足；ClickHouse 的 `raw_message` 无限制。StarRocks 取 1MB 上限，覆盖绝大多数行级记录，超长记录截断并在 `error_message` 中注明。 |
| `record_format` | `VARCHAR(16)` | 记录原始数据格式，重处理时需要知道按什么格式解析。Redshift/ClickHouse 均为隐含（仅支持特定格式），StarRocks 支持多种格式需显式记录。 |

**⑥ `source_info` — 来源信息**

| 维度 | 设计 |
|------|------|
| 类型 | `JSON` |
| 设计理由 | 不同导入来源的元数据结构完全不同，用固定列会导致大量 NULL 或需要为每种来源单独建列。Redshift 用 `filename` + `line_number` + `is_partial` + `start_offset` 共 4 列（仅适用于文件来源）；ClickHouse 用 `kafka_topic_name` + `kafka_partition` + `kafka_offset` + `kafka_key` 共 4 列（仅适用于 Kafka）。StarRocks 需要同时覆盖文件、Kafka、INSERT 等多种来源，JSON 是最灵活的选择。 |

各导入类型的 `source_info` 结构：

```json
// Stream Load / Broker Load（文件来源）
{
    "file": "hdfs://namenode/data/orders.csv",
    "line": 42,
    "offset": 8192
}

// Routine Load（Kafka 来源）
{
    "topic": "orders_topic",
    "partition": 3,
    "offset": 156789,
    "key": "order_001"
}

// INSERT INTO ... SELECT（查询来源）
{
    "source_query": "INSERT INTO orders SELECT * FROM staging.orders_raw"
}
```

**⑦ `backend_id` — 运行时信息**

Redshift 有 `slice`（计算分片）标识。`backend_id` 记录产生该 DLQ 记录的 BE 节点，用于排障定位写入侧问题。

#### 4.3.4 未纳入 Schema 的字段及理由

| 字段 | 来源 | 不纳入原因 |
|------|------|-----------|
| `status` / `retry_count` / `resolved_at` | 初版设计 | DLQ 表定位为 append-only 的数据记录，不是工作流引擎。重处理由用户通过 SQL 自行编排。引入状态列会带来 UPDATE 需求，与 Duplicate Key 模型冲突。 |
| `id` (AUTO_INCREMENT) | 初版设计 | Duplicate Key 模型下用 `created_at` + `target_database` + `target_table` 已足够标识和排序，无需全局自增 ID。 |
| `column_type` / `column_length` / `position` | Redshift | 这些详细的列元数据信息可以包含在 `error_message` 中，独立建列收益低但会增加 schema 复杂度。 |
| `raw_field_value` | Redshift | Redshift 额外记录出错字段的原始值。`raw_record` 已包含完整原始行，`error_column` 已标识出错列，可从原始行中提取。 |
| `kafka_topic` / `kafka_partition` / `kafka_offset` / `kafka_key` | ClickHouse | 作为独立列仅适用于 Kafka 来源，用 `source_info` JSON 统一承载更灵活。 |
| `session_id` | Redshift | StarRocks 的导入体系以 `load_label` + `txn_id` 为核心标识，session 信息可从 FE audit log 关联。 |
| `load_job_id` | 初版设计 | 非所有导入方式都有 job_id（Stream Load 没有）。`load_label` 是更通用的标识。 |

#### 4.3.5 表属性设计说明

| 属性 | 值 | 说明 |
|------|---|------|
| `ENGINE` | `OLAP` | 标准 StarRocks 存储引擎 |
| `DUPLICATE KEY` | `(created_at, target_database, target_table)` | Duplicate 模型适合 append-only 场景；前缀索引加速时间范围 + 目标表的查询 |
| `PARTITION BY` | `date_trunc('day', created_at)` | 表达式分区，按天自动创建 |
| `DISTRIBUTED BY` | `RANDOM` | DLQ 是纯追加写入，无 JOIN 需求，RANDOM 分布最均匀且写入开销最低 |
| `partition_live_number` | `7` | 默认保留 7 天，与 Redshift 的 7 天系统表日志轮转一致 |
| `replication_num` | `1` | DLQ 数据非关键路径，单副本降低存储开销。用户可按需调整 |

### 4.4 DLQ 配置设计

#### 4.4.1 导入级别配置

通过导入属性控制 DLQ 行为：

```bash
# Stream Load
curl -H "dead_letter_queue: true" \
     -H "dead_letter_queue_max_records: 10000" \
     -T data.csv \
     http://fe:8030/api/db/table/_stream_load

# Broker Load
LOAD LABEL my_label (
    DATA INFILE("hdfs://...")
    INTO TABLE my_table
)
WITH BROKER
PROPERTIES (
    "dead_letter_queue" = "true"
);

# Routine Load
CREATE ROUTINE LOAD my_job ON my_table
PROPERTIES (
    "dead_letter_queue" = "true"
);
```

#### 4.4.2 Session 变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `enable_dead_letter_queue` | `false` | 当前 session 的 DLQ 开关，对 INSERT 语句生效 |
| `dead_letter_queue_max_records` | `-1` (无限制) | 单次导入最大 DLQ 记录数（-1 = 不限制） |

```sql
SET enable_dead_letter_queue = true;
INSERT INTO my_table SELECT * FROM source_table;
```

#### 4.4.3 全局配置

FE 配置 (`fe.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `dead_letter_queue_default_ttl_days` | `7` | DLQ 表默认分区保留天数（`partition_live_number`） |
| `dead_letter_queue_batch_size` | `4096` | BE 批量写入 DLQ 的行数 |
| `dead_letter_queue_flush_interval_ms` | `1000` | BE 刷新 DLQ 缓冲区的间隔 |

BE 配置 (`be.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `dead_letter_queue_buffer_size` | `65536` | 每个导入任务的 DLQ 内存缓冲区大小（字节） |
| `dead_letter_queue_max_batch_bytes` | `67108864` (64MB) | 单次批量写入 DLQ 的最大字节数 |

### 4.6 SQL 接口设计

DLQ 表位于系统数据库 `_starrocks_sys` 下，全限定名为 `_starrocks_sys._dead_letter_q`。用户通过标准 SQL 查询和操作，Row Access Policy 自动过滤可见行。

#### 4.6.1 查询 DLQ

```sql
-- 查看某个目标表的 DLQ 数据（普通用户自动按权限过滤）
SELECT * FROM _starrocks_sys._dead_letter_q
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND error_code = 'TYPE_MISMATCH'
ORDER BY created_at DESC
LIMIT 100;

-- 统计某个库某个表的各类错误分布
SELECT error_code, COUNT(*) as cnt, MAX(created_at) as latest
FROM _starrocks_sys._dead_letter_q
WHERE target_database = 'db1'
  AND target_table = 'orders'
GROUP BY error_code
ORDER BY cnt DESC;

-- 查看某次导入的所有 DLQ 记录
SELECT * FROM _starrocks_sys._dead_letter_q
WHERE load_label = 'load_orders_20260327';

-- 管理员查看全局 DLQ 概览（admin 无行过滤）
SELECT target_database, target_table, error_code, COUNT(*) as cnt
FROM _starrocks_sys._dead_letter_q
WHERE created_at >= current_date() - INTERVAL 1 DAY
GROUP BY target_database, target_table, error_code
ORDER BY cnt DESC;
```

#### 4.6.2 重新导入

```sql
-- 从 DLQ 重新导入到目标表（修正 schema 后）
INSERT INTO db1.orders
SELECT parse_json(raw_record)
FROM _starrocks_sys._dead_letter_q
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-26';
```

#### 4.6.3 管理操作

```sql
-- 调整 DLQ 数据保留天数（默认 7 天）
ALTER TABLE _starrocks_sys._dead_letter_q SET ("partition_live_number" = "14");

-- 手动清理整个 DLQ（需 admin 权限）
TRUNCATE TABLE _starrocks_sys._dead_letter_q;

-- 清理某个目标表的历史 DLQ 数据
DELETE FROM _starrocks_sys._dead_letter_q
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND created_at < '2026-03-01';
```

### 4.7 错误分类

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

### 4.8 与现有机制的关系

DLQ 机制不替代现有的 error log 和 rejected record 机制，而是作为增强：

```
                              DLQ 开关
                                │
                    ┌───────────┼───────────┐
                    │ OFF       │           │ ON
                    ▼           │           ▼
           ┌───────────────┐   │   ┌───────────────┐
           │ 现有行为        │   │   │ DLQ 行为       │
           │                │   │   │                │
           │ error_log      │   │   │ error_log      │ ← 保持兼容
           │ rejected_record│   │   │ rejected_record│ ← 保持兼容
           │ max_filter_ratio│  │   │ max_filter_ratio│ ← 保持兼容
           │                │   │   │ + DLQ 表写入    │ ← 新增
           └───────────────┘   │   └───────────────┘
                               │
```

- `max_filter_ratio` / `max_error_number` 控制逻辑不变
- 当 DLQ 启用时，被 filter 的行**额外**写入 DLQ 表
- error_log 和 rejected_record 作为轻量级的调试手段继续保留

## 5. 关键技术设计

### 5.1 BE 侧 DLQ Writer

```
┌───────────────────────────────────────────────────┐
│                   BE Pipeline                      │
│                                                    │
│  Scanner ──→ [Chunk Processing] ──→ OlapTableSink  │
│                     │                              │
│                     │ (filtered rows)              │
│                     ▼                              │
│              ┌─────────────────┐                   │
│              │  DLQRecordBuffer│                   │
│              │  (per-fragment)  │                   │
│              └────────┬────────┘                   │
│                       │                            │
│                       ▼ (batch flush)              │
│              ┌─────────────────┐                   │
│              │  DLQWriter       │                   │
│              │  (Stream Load    │                   │
│              │   to DLQ table)  │                   │
│              └─────────────────┘                   │
└───────────────────────────────────────────────────┘
```

**核心设计要点**：

1. **异步写入**：DLQ 记录先写入内存 buffer（`DLQRecordBuffer`），达到阈值后异步批量写入 DLQ 表，不阻塞主导入流水线。
2. **内部 Stream Load**：BE 使用内部的 Stream Load 机制将 DLQ 数据写入 DLQ 表，复用现有的数据写入路径。
3. **Best-effort 语义**：DLQ 写入失败不应导致主导入失败。写入失败时记录日志并更新 metric，但不影响原始导入任务。
4. **内存限制**：通过 `dead_letter_queue_buffer_size` 控制 buffer 大小，超过后丢弃最老的记录（FIFO）。

### 5.2 FE 侧 DLQ 管理

```
┌─────────────────────────────────────────────────────┐
│                   FE                                 │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ DLQManager                                     │  │
│  │                                                │  │
│  │ - FE 启动时确保 _starrocks_sys._dead_letter_q │  │
│  │   存在（类似 information_schema 初始化）        │  │
│  │ - 向 TQueryOptions 注入 DLQ 参数               │  │
│  │ - 查询时注入 Row Access Policy 行过滤          │  │
│  │ - 汇总 DLQ 统计信息                            │  │
│  └───────────────────────────────────────────────┘  │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ 导入流程集成                                   │  │
│  │                                                │  │
│  │ StreamLoadPlanner / RoutineLoadJob / BulkLoad: │  │
│  │   - 检查 DLQ 是否启用                          │  │
│  │   - 向 TQueryOptions 注入 DLQ 参数             │  │
│  │   - 报告 DLQ 统计信息                          │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**DLQ 表创建时机**：`_starrocks_sys._dead_letter_q` 在 FE 启动时由 `DLQManager` 初始化（类似 `information_schema` 的系统表初始化流程），而非懒创建。这保证了全局只有一张表，且任何导入任务启用 DLQ 时无需等待建表。

### 5.3 Thrift 接口扩展

```thrift
// InternalService.thrift
struct TQueryOptions {
    // ... existing fields ...

    // DLQ configuration
    optional bool enable_dead_letter_queue;
    optional string dlq_table_name;
    optional i64 dlq_max_records;
    optional i32 dlq_batch_size;
    optional i64 dlq_flush_interval_ms;
}

// FrontendService.thrift
struct TReportExecStatusParams {
    // ... existing fields ...

    // DLQ statistics
    optional i64 dlq_records_written;
    optional i64 dlq_records_failed;
    optional i64 dlq_bytes_written;
}
```

### 5.4 DLQ 表生命周期

```
FE 启动
  │
  ▼
DLQManager 初始化 _starrocks_sys._dead_letter_q
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

DLQ 表是全局基础设施，生命周期与集群一致。任何 database 或 table 被删除，不影响 DLQ 表的存在和已有数据（历史坏数据仍可查询）。

## 6. Metrics 设计

### 6.1 BE Metrics

| Metric | 类型 | 说明 |
|--------|------|------|
| `starrocks_be_dlq_records_total` | Counter | DLQ 累计写入的记录总数 |
| `starrocks_be_dlq_records_failed` | Counter | DLQ 写入失败的记录数 |
| `starrocks_be_dlq_bytes_total` | Counter | DLQ 累计写入字节数 |
| `starrocks_be_dlq_flush_duration_ms` | Histogram | DLQ 批量写入耗时 |
| `starrocks_be_dlq_buffer_usage_ratio` | Gauge | DLQ buffer 使用率 |

### 6.2 FE Metrics

| Metric | 类型 | 说明 |
|--------|------|------|
| `starrocks_fe_dlq_records_total` | Counter | DLQ 记录写入总数 |

## 7. 性能考量

### 7.1 正常路径 (Happy Path) 零开销

当 DLQ 未启用时，不应有任何额外开销。实现方式：
- 编译时：通过条件编译或常量折叠
- 运行时：在 `RuntimeState` 中增加 `_enable_dlq` 标志位，在 append 路径入口快速判断

### 7.2 坏数据路径性能

| 策略 | 描述 |
|------|------|
| 内存缓冲 | 坏记录先写入线程本地 buffer，减少锁竞争 |
| 批量写入 | 累积到一定量或时间间隔后批量写入 DLQ 表 |
| 异步写入 | DLQ 写入在独立线程池中执行 |
| 背压控制 | buffer 满时丢弃记录而非阻塞主流水线 |
| 采样模式 | 超大量坏数据时可开启采样（如每 N 条记录 1 条） |

### 7.3 存储成本预估

假设平均每条 DLQ 记录 2KB（包含原始数据和元数据）：
- 1 万条/天 → ~20MB/天 → ~140MB/周
- 100 万条/天 → ~2GB/天 → ~14GB/周
- 1 亿条/天 → ~200GB/天 → 需要考虑采样或限制

建议通过 `dead_letter_queue.max_records` 限制单次导入的 DLQ 记录数，避免极端场景下的存储爆炸。

## 8. 权限控制设计

### 8.1 问题与方案选择

全局单表意味着所有数据库、所有表的坏数据混在一起。必须保证：用户只能看到自己有 SELECT 权限的 `target_database.target_table` 对应的 DLQ 行。

业界方案：Redshift 按 `userid` 自动行过滤（粒度不够——按导入者而非目标表）；ClickHouse 仅做表级 GRANT（无行级隔离）。

StarRocks 采用 **自动 Row Access Policy**：FE 在查询 `_dead_letter_q` 时自动注入行过滤谓词，复用已有的 `SecurityPolicyRewriteRule` + `Authorizer` 基础设施。

### 8.2 实现方式

```
用户原始查询:
  SELECT * FROM _starrocks_sys._dead_letter_q
  WHERE error_code = 'TYPE_MISMATCH';

FE Analyzer 改写后:
  SELECT * FROM _starrocks_sys._dead_letter_q
  WHERE error_code = 'TYPE_MISMATCH'
    AND (target_database, target_table) IN (
        <当前用户拥有 SELECT 权限的 (database, table) 列表>
    );
```

**核心规则**：

| 用户角色 | 行为 |
|---------|------|
| Admin / root | 不注入过滤，可见全部行 |
| 普通用户 | 自动注入 `(target_database, target_table) IN (...)` 过滤 |
| 无任何表 SELECT 权限的用户 | 查询返回空集 |

**实现位置**：`QueryAnalyzer` 识别到 `_starrocks_sys._dead_letter_q` 时，调用 `Authorizer` 收集当前用户有 SELECT 权限的 `(database, table)` 集合，作为谓词注入。当集合过大时，优化为与权限元数据表的 semi-join。

### 8.3 写入与管理权限

| 操作 | 权限要求 |
|------|---------|
| DLQ 数据写入 | 系统内部（BE DLQ Writer），用户无直接 INSERT 权限 |
| SELECT 查询 | 自动 Row Access Policy 过滤 |
| DELETE 清理 | 需要 `_starrocks_sys` 数据库的 DROP 或 ADMIN 权限 |
| TRUNCATE | 需要 ADMIN 权限 |
| ALTER TABLE (调整 TTL) | 需要 ADMIN 权限 |

### 8.4 其他安全考虑

- **数据敏感性**：`raw_record` 列包含原始数据，可能含敏感信息。如需脱敏，可对该列应用 Column Masking Policy（StarRocks 已支持 `Authorizer.getColumnMaskingPolicy()`）。
- **审计**：DLQ 表的查询、DELETE、TRUNCATE 操作需记录审计日志。

## 9. 兼容性

- **向后兼容**：DLQ 默认关闭，不影响现有行为。
- **upgrade path**：升级后用户可选择性启用 DLQ。
- **降级**：降级时 DLQ 表将作为普通的 OLAP 表保留，但不再自动写入。

## 10. 与现有 reject record 机制的对比

| 维度 | 现有 Reject Record | DLQ (新方案) |
|------|-------------------|-------------|
| 存储位置 | BE 本地文件系统 | StarRocks 内置全局 OLAP 表 (`_starrocks_sys._dead_letter_q`) |
| 持久性 | 临时文件，依赖 BE 节点 | 持久化，跨节点可用 |
| 可查询性 | 需 SSH 到 BE 节点 | 标准 SQL 查询 |
| 权限控制 | 无 | 自动 Row Access Policy（按 target_table 权限过滤） |
| 数据完整性 | 受 `log_rejected_record_num` 限制 | 可配置，默认记录所有 |
| 错误分类 | 无结构化分类 | 标准错误码体系 |
| 重处理能力 | 无 | `INSERT INTO target SELECT ... FROM _starrocks_sys._dead_letter_q` |
| 生命周期 | 依赖 BE 清理策略 | 表达式分区 + `partition_live_number` 自动过期（默认 7 天） |
| 可观测性 | 无专门 metrics | 完整的 metrics 体系 |
| 对性能影响 | 本地文件写入，低 | 涉及表写入，需异步和批量优化 |
| 外部依赖 | 无 | 无（纯 StarRocks 内置） |
| 适用场景 | 调试 | 生产环境数据质量保障 |

## 11. 开放问题

1. **系统数据库命名**：`_starrocks_sys` 是否与现有系统数据库（`information_schema`、`_statistics_`）的命名风格一致？是否复用已有的系统数据库？
2. **DLQ 写入失败的处理**：best-effort 是否足够？是否需要 fallback 到本地文件？
3. **大量坏数据场景**：Broker Load 导入 TB 级文件，坏行比例高时，DLQ 写入可能成为瓶颈。是否需要采样机制或自动降级。
4. **`raw_record` 截断策略**：超过 1MB 的记录如何处理？截断并在 `error_message` 中标注，还是直接丢弃原始数据？
5. **二进制数据**：`raw_record` 为 VARCHAR 类型，无法保真存储二进制格式（如 Avro/Parquet 的原始 bytes）。是否需要 VARBINARY 支持？
6. **shared-data 模式**：DLQ 表在 shared-data 模式下的存储和读取是否有特殊考量？
7. **Row Access Policy 优化**：当用户有权限的 `(database, table)` 集合很大时，IN 谓词的性能影响如何？是否需要优化为与权限元数据的 semi-join？

## 12. 里程碑计划

### Phase 1: 基础 DLQ 框架

- [ ] `_starrocks_sys._dead_letter_q` 表初始化（FE DLQManager）
- [ ] 自动 Row Access Policy 实现
- [ ] DLQ Writer 实现（BE：异步批量写入）
- [ ] Stream Load DLQ 支持
- [ ] INSERT INTO DLQ 支持
- [ ] 基本 metrics
- [ ] 导入级别配置 + Session 变量

### Phase 2: 完整导入支持 + 运维

- [ ] Broker Load DLQ 支持
- [ ] Routine Load DLQ 支持
- [ ] 完整 metrics 和监控集成
- [ ] 文档和用户指南

### Phase 3: 高级功能

- [ ] 采样模式（超大量坏数据场景）
- [ ] DLQ 数据可视化（Web UI 集成）
