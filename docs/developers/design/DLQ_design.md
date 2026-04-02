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
| **Amazon Redshift** | `MAXERROR` 参数 + `STL_LOAD_ERRORS` 系统表 | 类 DLQ（系统表记录） | 系统表持久化记录所有失败行的详细信息（行号、列名、原始数据、错误码），可 SQL 查询 |
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

采用 **每个数据库一张 DLQ 表** 的设计（类似 ClickHouse `system.dead_letter_queue` 和 Redshift `STL_LOAD_ERRORS` 的全局表模式），通过 `target_table` 列区分不同目标表的坏数据，零外部依赖。

```
┌──────────────────────────────────────────────────────────────────┐
│                       DLQ 架构                                    │
│                                                                    │
│  Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ Table  │
│                                              │                     │
│                                              ▼                     │
│                                    ┌───────────────────┐          │
│                                    │   DLQ Writer       │          │
│                                    │  (BE Pipeline)     │          │
│                                    └─────────┬─────────┘          │
│                                              │                     │
│                                              ▼                     │
│                                   ┌────────────────────┐          │
│                                   │  _starrocks_dlq    │          │
│                                   │  (per-database)    │          │
│                                   │                    │          │
│                                   │ target_table = ... │          │
│                                   │ load_label = ...   │          │
│                                   │ error_code = ...   │          │
│                                   │ raw_record = ...   │          │
│                                   └────────────────────┘          │
│                                              │                     │
│                              ┌───────────────┼───────────────┐    │
│                              ▼                               ▼    │
│              ┌─────────────────────────┐  ┌────────────────────┐ │
│              │  SELECT * FROM          │  │  INSERT INTO target │ │
│              │    _starrocks_dlq       │  │  SELECT ... FROM    │ │
│              │  WHERE target_table=... │  │    _starrocks_dlq   │ │
│              │    ← SQL 查询           │  │    ← 重新导入        │ │
│              └─────────────────────────┘  └────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

**核心设计决策**：

| 决策 | 选择 | 理由 |
|------|------|------|
| DLQ 表粒度 | 每个数据库一张 DLQ 表 | 避免表爆炸（per-table 方案会导致大量 DLQ 表）；通过 `target_table` 列过滤；权限自然跟随数据库隔离 |
| 存储方案 | StarRocks 内置 OLAP 表 | 零外部依赖；标准 SQL 查询和操作；复用现有存储引擎 |
| 分区方式 | 表达式分区 (`PARTITION BY`) | 现代 StarRocks 标准方式，配合分区 TTL 自动过期 |

### 4.3 DLQ 表设计

#### 4.3.1 DLQ 表 Schema

每个数据库在首次启用 DLQ 时，自动创建一张 `_starrocks_dlq` 表：

```sql
CREATE TABLE _starrocks_dlq (
    -- 导入上下文
    load_job_id         BIGINT          COMMENT '导入任务 ID',
    load_label          VARCHAR(256)    COMMENT '导入 Label',
    load_type           VARCHAR(32)     COMMENT '导入类型: STREAM_LOAD/ROUTINE_LOAD/BROKER_LOAD/INSERT',
    txn_id              BIGINT          COMMENT '事务 ID',

    -- 目标表信息
    target_table        VARCHAR(256)    NOT NULL COMMENT '目标表名',

    -- 错误信息
    error_code          VARCHAR(64)     COMMENT '错误码: TYPE_MISMATCH/NULL_VIOLATION/PARSE_ERROR/...',
    error_message       VARCHAR(4096)   COMMENT '错误详细描述',
    error_column        VARCHAR(256)    COMMENT '出错的列名 (如果可确定)',

    -- 原始数据
    raw_record          VARCHAR(65535)  COMMENT '原始记录 (原文)',
    record_format       VARCHAR(32)     COMMENT '记录格式: CSV/JSON/AVRO/ORC/PARQUET',

    -- 来源信息
    source_info         JSON            COMMENT '来源描述, 如 {"kafka_topic":"t","partition":0,"offset":123} 或 {"file":"hdfs://...","line":42}',

    -- 时间
    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    -- BE 节点信息
    backend_id          BIGINT          COMMENT '产生该记录的 BE 节点 ID'
) ENGINE = OLAP
DUPLICATE KEY(load_job_id)
PARTITION BY date_trunc('day', created_at)
DISTRIBUTED BY HASH(load_job_id)
PROPERTIES (
    "partition_live_number" = "7",
    "replication_num" = "1"
);
```

#### 4.3.2 设计说明

**为什么是 per-database 而不是 per-table 或全局**：

| 方案 | 优点 | 缺点 |
|------|------|------|
| **全局单表** (ClickHouse 模式) | 最简单 | 权限难以隔离（不同数据库的数据混在一起）；高并发写入集中在一张表上 |
| **per-table** | 数据天然隔离 | 表数量爆炸（1000 张业务表 → 1000 张 DLQ 表）；元数据膨胀严重 |
| **per-database** (推荐) | 权限自然跟随 database 隔离；DLQ 表数量 = database 数量（通常很少）；通过 `target_table` 列灵活过滤 | 同 database 下高并发写入时有竞争（可通过 hash 分桶缓解） |

**分区与 TTL**：

- 使用表达式分区 `PARTITION BY date_trunc('day', created_at)`，按天自动创建分区
- 通过 `partition_live_number` 控制保留的分区数（默认 7 天），过期分区自动删除
- 用户可通过 `ALTER TABLE _starrocks_dlq SET ("partition_live_number" = "14")` 调整保留策略

**Schema 简化**：

- 移除了 `status`/`retry_count`/`resolved_at` 等状态管理列——DLQ 表定位为数据记录而非工作流引擎，重处理由用户通过 SQL 自行编排
- `source_info` 使用 JSON 类型，灵活适配不同导入来源（Kafka、文件、INSERT）的元数据
- `target_table` 记录目标表名，用于在同一 DLQ 表中区分不同表的坏数据

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

DLQ 表是该数据库下的一张普通 OLAP 表（`_starrocks_dlq`），用户通过标准 SQL 进行查询和操作。

#### 4.6.1 查询 DLQ

```sql
-- 查看某个目标表的 DLQ 数据
SELECT * FROM _starrocks_dlq
WHERE target_table = 'table1'
  AND created_at > '2026-03-01'
  AND error_code = 'TYPE_MISMATCH'
ORDER BY created_at DESC
LIMIT 100;

-- 统计某个表各类错误分布
SELECT error_code, COUNT(*) as cnt, MAX(created_at) as latest
FROM _starrocks_dlq
WHERE target_table = 'table1'
GROUP BY error_code
ORDER BY cnt DESC;

-- 查看某次导入的所有 DLQ 记录
SELECT * FROM _starrocks_dlq
WHERE load_label = 'my_load_20260327';

-- 查看整个数据库的 DLQ 概览
SELECT target_table, error_code, COUNT(*) as cnt
FROM _starrocks_dlq
WHERE created_at >= current_date() - INTERVAL 1 DAY
GROUP BY target_table, error_code
ORDER BY cnt DESC;
```

#### 4.6.2 重新导入

```sql
-- 从 DLQ 重新导入到目标表（修正 schema 后）
INSERT INTO table1
SELECT parse_json(raw_record)
FROM _starrocks_dlq
WHERE target_table = 'table1'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-26';
```

#### 4.6.3 管理操作

```sql
-- 手动清理整个 DLQ
TRUNCATE TABLE _starrocks_dlq;

-- 清理某个目标表的历史 DLQ 数据
DELETE FROM _starrocks_dlq
WHERE target_table = 'table1'
  AND created_at < '2026-03-01';

-- 调整 DLQ 数据保留天数（默认 7 天）
ALTER TABLE _starrocks_dlq SET ("partition_live_number" = "14");
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
┌────────────────────────────────────────────────┐
│                   FE                            │
│                                                 │
│  ┌──────────────────────────────────────────┐  │
│  │ DLQManager                                │  │
│  │                                           │  │
│  │ - 懒创建 DLQ 表 (首次启用 DLQ 时自动创建) │  │
│  │ - 向 TQueryOptions 注入 DLQ 表名和参数    │  │
│  │ - 汇总 DLQ 统计信息                       │  │
│  └──────────────────────────────────────────┘  │
│                                                 │
│  ┌─────────────────────────────────────────┐   │
│  │ 导入流程集成                             │   │
│  │                                          │   │
│  │ StreamLoadPlanner:                       │   │
│  │   - 检查 DLQ 是否启用                    │   │
│  │   - 确保 _starrocks_dlq 表存在           │   │
│  │   - 向 TQueryOptions 注入 DLQ 参数       │   │
│  │                                          │   │
│  │ RoutineLoadJob:                          │   │
│  │   - 传递 DLQ 参数到 task                 │   │
│  │   - 报告 DLQ 统计信息                    │   │
│  │                                          │   │
│  │ BulkLoadJob / BrokerLoadJob:             │   │
│  │   - 传递 DLQ 配置                        │   │
│  └─────────────────────────────────────────┘   │
└────────────────────────────────────────────────┘
```

**DLQ 表懒创建**：当某个导入任务首次启用 DLQ 时，FE 的 `DLQManager` 检查目标 database 下是否已存在 `_starrocks_dlq` 表。若不存在，自动创建。后续同 database 下的所有 DLQ 写入共享该表。

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
首次在 database 中启用 DLQ 的导入任务
         │
         ▼
   FE DLQManager 自动创建 _starrocks_dlq 表
   (表达式分区, partition_live_number TTL)
         │
         ├──→ 正常运行：按天自动创建分区，过期分区自动删除
         │
         ├──→ DROP DATABASE
         │         → DLQ 表随 database 一起删除
         │
         └──→ TRUNCATE TABLE _starrocks_dlq / DELETE FROM ...
                  → 手动清理
```

DLQ 表是 database 级别的基础设施，**不与任何特定目标表绑定**。目标表被删除不影响 DLQ 表的存在和已有数据。

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
| `starrocks_fe_dlq_databases_total` | Gauge | 当前启用了 DLQ 的数据库数量 |
| `starrocks_fe_dlq_records_total` | Counter | 所有 DLQ 表的记录写入总数 |

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

## 8. 安全考虑

- **权限模型**：DLQ 表的访问权限继承目标表。拥有目标表 SELECT 权限的用户可以查询对应的 DLQ 表。
- **数据敏感性**：DLQ 表包含原始数据，可能包含敏感信息。遵循与原始数据相同的安全策略。
- **审计**：DLQ 表的创建、查询、删除操作需要记录审计日志。

## 9. 兼容性

- **向后兼容**：DLQ 默认关闭，不影响现有行为。
- **upgrade path**：升级后用户可选择性启用 DLQ。
- **降级**：降级时 DLQ 表将作为普通的 OLAP 表保留，但不再自动写入。

## 10. 与现有 reject record 机制的对比

| 维度 | 现有 Reject Record | DLQ (新方案) |
|------|-------------------|-------------|
| 存储位置 | BE 本地文件系统 | StarRocks 内置 OLAP 表（per-database） |
| 持久性 | 临时文件，依赖 BE 节点 | 持久化，跨节点可用 |
| 可查询性 | 需 SSH 到 BE 节点 | 标准 SQL 查询 |
| 数据完整性 | 受 `log_rejected_record_num` 限制 | 可配置，默认记录所有 |
| 错误分类 | 无结构化分类 | 标准错误码体系 |
| 重处理能力 | 无 | 支持 `INSERT INTO target SELECT ... FROM _starrocks_dlq` |
| 生命周期 | 依赖 BE 清理策略 | 表达式分区 + `partition_live_number` 自动过期 |
| 可观测性 | 无专门 metrics | 完整的 metrics 体系 |
| 对性能影响 | 本地文件写入，低 | 涉及表写入，需异步和批量优化 |
| 外部依赖 | 无 | 无（纯 StarRocks 内置） |
| 适用场景 | 调试 | 生产环境数据质量保障 |

## 11. 开放问题

1. **DLQ 表命名**：`_starrocks_dlq` 是否需要加更多前缀以避免与用户表冲突？是否保留 `_` 前缀命名空间？
2. **DLQ 表可见性**：`SHOW TABLES` 中是否显示 DLQ 表？是否需要 `SHOW TABLES ALL` 才能看到？
3. **DLQ 写入失败的处理**：best-effort 是否足够？是否需要 fallback 到本地文件？
4. **大文件导入场景**：Broker Load 导入 TB 级文件时，如果有大量坏行，DLQ 写入可能成为瓶颈，是否需要采样机制。
5. **原始记录存储格式**：VARCHAR(65535) 可能损失二进制数据的保真度，是否需要 VARBINARY 支持？
6. **DLQ 表权限**：DLQ 表的读写权限是否需要独立管控，还是继承 database 权限即可？
7. **shared-data 模式**：shared-data 模式下 DLQ 表的行为是否有特殊考量？

## 12. 里程碑计划

### Phase 1: 基础 DLQ 框架

- [ ] DLQ 表懒创建（FE DLQManager：首次启用时自动创建 `_starrocks_dlq`）
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
