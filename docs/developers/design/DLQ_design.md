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

| 系统 | 错误处理方式 | 特点 |
|------|-------------|------|
| **Snowflake** | `COPY INTO` 的 `ON_ERROR` 参数（CONTINUE/SKIP_FILE/ABORT_STATEMENT），`VALIDATION_MODE` 预检查，`VALIDATE()` 函数后检查 | 坏数据跳过但不持久化到独立表，需要用户自行编排 |
| **Google BigQuery** | `max_bad_records` 参数控制容忍数量，Job 结果包含错误详情（文件位置、行号、字段） | 坏行直接跳过，不进入独立存储 |
| **Amazon Redshift** | `MAXERROR` 参数 + `STL_LOAD_ERRORS` 系统表记录所有失败行的详细信息（行号、列名、原始数据、错误码） | **最接近 DLQ 的方案**：系统表持久化记录所有失败，可 SQL 查询 |
| **ClickHouse** | `input_format_allow_errors_num` + `input_format_allow_errors_ratio` 控制容忍度 | 仅跳过，无独立的错误记录存储 |
| **Apache Doris** | `max_filter_ratio` + `strict_mode`，`ErrorURL` 提供 HTTP 下载错误日志 | 与 StarRocks 现有机制基本相同（同源项目） |

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

```
┌─────────────────────────────────────────────────────────────────────┐
│                       DLQ 架构                                       │
│                                                                       │
│  Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ Table     │
│                                              │                        │
│                                              ▼                        │
│                                    ┌───────────────────┐             │
│                                    │   DLQ Router       │             │
│                                    │  (BE Pipeline)     │             │
│                                    └─────────┬─────────┘             │
│                                              │                        │
│                              ┌───────────────┼───────────────┐       │
│                              ▼               ▼               ▼       │
│                     ┌──────────────┐ ┌──────────────┐ ┌───────────┐ │
│                     │ DLQ Table    │ │  Kafka DLQ   │ │ File DLQ  │ │
│                     │ (StarRocks)  │ │   Topic      │ │ (S3/HDFS) │ │
│                     └──────────────┘ └──────────────┘ └───────────┘ │
│                           │                                          │
│                           ▼                                          │
│              ┌─────────────────────────┐                            │
│              │  SELECT * FROM dlq_table │                            │
│              │  WHERE ...               │ ← SQL 查询                 │
│              └─────────────────────────┘                            │
│                           │                                          │
│                           ▼                                          │
│              ┌─────────────────────────┐                            │
│              │  INSERT INTO target      │                            │
│              │  SELECT ... FROM dlq     │ ← 重新导入                 │
│              └─────────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.3 DLQ 存储方案

推荐 **方案一（StarRocks 内置表）** 作为默认实现，同时预留扩展点支持外部存储。

#### 方案一：StarRocks 内置 DLQ 表（推荐）

每个启用了 DLQ 的目标表，自动创建一个关联的 DLQ 表。

**DLQ 表 Schema**:

```sql
CREATE TABLE __starrocks_dlq_<db>_<table> (
    -- 唯一标识
    id                  BIGINT          NOT NULL AUTO_INCREMENT,

    -- 导入上下文
    load_job_id         BIGINT          COMMENT '导入任务 ID',
    load_label          VARCHAR(256)    COMMENT '导入 Label',
    load_type           VARCHAR(32)     COMMENT '导入类型: STREAM_LOAD/ROUTINE_LOAD/BROKER_LOAD/INSERT',
    txn_id              BIGINT          COMMENT '事务 ID',

    -- 错误信息
    error_code          VARCHAR(64)     COMMENT '错误码: TYPE_MISMATCH/NULL_VIOLATION/PARSE_ERROR/...',
    error_message       VARCHAR(4096)   COMMENT '错误详细描述',
    error_column        VARCHAR(256)    COMMENT '出错的列名 (如果可确定)',

    -- 原始数据
    raw_record          VARCHAR(65535)  COMMENT '原始记录 (原文)',
    record_format       VARCHAR(32)     COMMENT '记录格式: CSV/JSON/AVRO/ORC/PARQUET',

    -- 来源信息
    source_info         VARCHAR(4096)   COMMENT '来源描述 (JSON), 如 Kafka topic/partition/offset, 文件路径/行号',

    -- 目标信息
    target_database     VARCHAR(256)    COMMENT '目标数据库',
    target_table        VARCHAR(256)    COMMENT '目标表',

    -- 时间和状态
    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    retry_count         INT             DEFAULT 0 COMMENT '重试次数',
    status              VARCHAR(16)     DEFAULT 'PENDING' COMMENT '状态: PENDING/RETRIED/RESOLVED/EXPIRED',
    resolved_at         DATETIME        COMMENT '处理时间',

    -- BE 节点信息
    backend_id          BIGINT          COMMENT '产生该记录的 BE 节点 ID'
) ENGINE = OLAP
DUPLICATE KEY(id)
PARTITION BY date_trunc('day', created_at)
DISTRIBUTED BY HASH(id)
PROPERTIES (
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.history_partition_num" = "7",
    "replication_num" = "1"
);
```

**优点**：
- 用户可直接用 SQL 查询和分析 DLQ 数据
- 利用 StarRocks 现有的存储引擎，无需引入外部依赖
- 支持分区 TTL 自动过期
- 支持 `INSERT INTO target SELECT ... FROM dlq` 方式重新导入

**缺点**：
- 增加 StarRocks 自身的存储负担
- DLQ 写入和正常导入共享 BE 资源
- 如果坏数据量极大，可能影响集群性能

#### 方案二：Kafka DLQ Topic（适用于 Routine Load）

针对 Routine Load 场景，将坏记录写回 Kafka 的一个 DLQ topic。

```
Kafka Source Topic  ──→  Routine Load  ──→  Target Table
                                │
                                ▼ (filtered records)
                         Kafka DLQ Topic
                    (dlq-<db>-<table>-<routine_load_name>)
```

**DLQ Topic 消息格式（Headers）**:

| Header Key | Value |
|------------|-------|
| `dlq.source.topic` | 原始 topic 名 |
| `dlq.source.partition` | 原始 partition |
| `dlq.source.offset` | 原始 offset |
| `dlq.error.code` | 错误码 |
| `dlq.error.message` | 错误消息 |
| `dlq.target.database` | 目标数据库 |
| `dlq.target.table` | 目标表 |
| `dlq.timestamp` | 错误发生时间 |
| `dlq.load.label` | 导入 Label |

消息的 key 和 value 保持与原始消息完全一致。

**优点**：
- 完全复用 Kafka 生态，用户可用现有 Kafka 工具监控和消费 DLQ
- 不增加 StarRocks 存储负担
- Kafka 天然支持消息重放

**缺点**：
- 仅适用于 Routine Load（Kafka 数据源）
- 需要 Kafka 生产者权限
- 需要管理额外的 Kafka topic

#### 方案三：外部存储 DLQ（S3/HDFS/本地文件）

将坏数据写入外部文件系统，格式化为 JSON 或 CSV 文件。

```
Source Data ──→ Load Pipeline ──→ Target Table
                     │
                     ▼ (filtered records)
              s3://bucket/dlq/<db>/<table>/<date>/<load_label>.json
```

**优点**：
- 不影响 StarRocks 集群性能
- 大容量存储，适合坏数据量大的场景
- 可集成外部数据处理系统

**缺点**：
- 无法直接 SQL 查询（需 FILES() 函数或外部表）
- 增加外部存储依赖
- 生命周期管理需要外部机制

### 4.4 推荐分阶段实施方案

| 阶段 | 内容 | 存储方案 |
|------|------|----------|
| **Phase 1** | Stream Load + Broker Load + INSERT 支持 DLQ | StarRocks 内置 DLQ 表 |
| **Phase 2** | Routine Load 支持 DLQ | StarRocks 内置 DLQ 表 + Kafka DLQ Topic（可选） |
| **Phase 3** | 外部存储 DLQ + 高级重处理 | S3/HDFS DLQ + 自动重试框架 |

### 4.5 DLQ 配置设计

#### 4.5.1 表级别配置

通过表属性 (Table Properties) 启用和配置 DLQ：

```sql
-- 创建表时启用 DLQ
CREATE TABLE my_table (
    ...
) PROPERTIES (
    "dead_letter_queue.enable" = "true",
    "dead_letter_queue.ttl_days" = "7",
    "dead_letter_queue.max_records" = "1000000",
    "dead_letter_queue.replication_num" = "1"
);

-- 修改已有表的 DLQ 配置
ALTER TABLE my_table SET ("dead_letter_queue.enable" = "true");
```

| 属性 | 默认值 | 说明 |
|------|--------|------|
| `dead_letter_queue.enable` | `false` | 是否启用 DLQ |
| `dead_letter_queue.ttl_days` | `7` | DLQ 数据保留天数 |
| `dead_letter_queue.max_records` | `-1` (无限制) | 单次导入最大 DLQ 记录数（-1 = 不限制） |
| `dead_letter_queue.replication_num` | `1` | DLQ 表副本数 |

#### 4.5.2 导入级别配置

通过导入属性覆盖表级别配置：

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
    "dead_letter_queue" = "true",
    "dead_letter_queue.kafka_topic" = "my-dlq-topic"  -- 可选：写回 Kafka
);
```

#### 4.5.3 全局配置

FE 配置 (`fe.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `enable_dead_letter_queue` | `false` | 全局 DLQ 开关 |
| `dead_letter_queue_default_ttl_days` | `7` | 默认 TTL |
| `dead_letter_queue_batch_size` | `4096` | BE 批量写入 DLQ 的行数 |
| `dead_letter_queue_flush_interval_ms` | `1000` | BE 刷新 DLQ 缓冲区的间隔 |

BE 配置 (`be.conf`):

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `dead_letter_queue_buffer_size` | `65536` | 每个导入任务的 DLQ 缓冲区大小 |
| `dead_letter_queue_max_batch_bytes` | `67108864` (64MB) | 单次批量写入 DLQ 的最大字节数 |

### 4.6 SQL 接口设计

#### 4.6.1 查询 DLQ

```sql
-- 查看某个表的 DLQ 数据
SELECT * FROM __starrocks_dlq_db1_table1
WHERE created_at > '2026-03-01'
  AND error_code = 'TYPE_MISMATCH'
ORDER BY created_at DESC
LIMIT 100;

-- 统计各类错误分布
SELECT error_code, COUNT(*) as cnt, MAX(created_at) as latest
FROM __starrocks_dlq_db1_table1
GROUP BY error_code
ORDER BY cnt DESC;

-- 查看某次导入的所有 DLQ 记录
SELECT * FROM __starrocks_dlq_db1_table1
WHERE load_label = 'my_load_20260327';

-- 快捷语法糖
SHOW DEAD LETTER QUEUE FOR db1.table1;
SHOW DEAD LETTER QUEUE FOR db1.table1 WHERE load_label = 'xxx';
```

#### 4.6.2 重新导入

```sql
-- 从 DLQ 重新导入到目标表（修正 schema 后）
INSERT INTO db1.table1
SELECT parse_json(raw_record)  -- 需要转换函数
FROM __starrocks_dlq_db1_table1
WHERE status = 'PENDING'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-26';

-- 标记已处理
UPDATE __starrocks_dlq_db1_table1
SET status = 'RESOLVED', resolved_at = NOW()
WHERE id IN (...);

-- 快捷重处理语法（Phase 3）
REPROCESS DEAD LETTER QUEUE FOR db1.table1
WHERE load_label = 'my_load_20260327';
```

#### 4.6.3 管理操作

```sql
-- 手动清理 DLQ
TRUNCATE TABLE __starrocks_dlq_db1_table1;

-- 清理特定条件的数据
DELETE FROM __starrocks_dlq_db1_table1
WHERE created_at < '2026-03-01';

-- 禁用 DLQ
ALTER TABLE db1.table1 SET ("dead_letter_queue.enable" = "false");

-- 查看 DLQ 状态
SHOW DEAD LETTER QUEUE STATUS;
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
│  ┌─────────────────┐    ┌───────────────────┐  │
│  │ DLQManager       │    │ DLQTableManager   │  │
│  │                  │    │                   │  │
│  │ - 管理 DLQ 配置  │    │ - 创建/删除 DLQ 表 │  │
│  │ - 传递 DLQ 参数  │    │ - 分区 TTL 管理    │  │
│  │   给 BE          │    │ - Schema 管理      │  │
│  │ - 统计汇总       │    │                   │  │
│  └─────────────────┘    └───────────────────┘  │
│                                                 │
│  ┌─────────────────────────────────────────┐   │
│  │ 导入流程集成                             │   │
│  │                                          │   │
│  │ StreamLoadPlanner:                       │   │
│  │   - 检查目标表是否启用 DLQ               │   │
│  │   - 向 TQueryOptions 注入 DLQ 参数       │   │
│  │                                          │   │
│  │ RoutineLoadJob:                          │   │
│  │   - 管理 DLQ Kafka producer (可选)       │   │
│  │   - 报告 DLQ 统计信息                    │   │
│  │                                          │   │
│  │ BulkLoadJob / BrokerLoadJob:             │   │
│  │   - 传递 DLQ 配置                        │   │
│  └─────────────────────────────────────────┘   │
└────────────────────────────────────────────────┘
```

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
目标表创建 + DLQ 启用
         │
         ▼
   FE 自动创建 DLQ 表
   (dynamic partition, TTL)
         │
         ├──→ 正常运行：动态分区按天创建和过期
         │
         ├──→ ALTER TABLE ... SET ("dead_letter_queue.enable" = "false")
         │         → DLQ 表保留，但停止写入
         │
         ├──→ DROP TABLE target_table
         │         → DLQ 表同步删除（或保留一段时间）
         │
         └──→ TRUNCATE TABLE __starrocks_dlq_...
                  → 手动清理
```

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
| `starrocks_fe_dlq_tables_total` | Gauge | 当前 DLQ 表总数 |
| `starrocks_fe_dlq_records_total` | Counter | 所有 DLQ 表的记录总数 |
| `starrocks_fe_dlq_reprocess_total` | Counter | 重处理操作次数 |
| `starrocks_fe_dlq_reprocess_success` | Counter | 重处理成功次数 |

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
| 存储位置 | BE 本地文件系统 | StarRocks 内置表 / Kafka Topic / 外部存储 |
| 持久性 | 临时文件，依赖 BE 节点 | 持久化，跨节点可用 |
| 可查询性 | 需 SSH 到 BE 节点 | 标准 SQL 查询 |
| 数据完整性 | 受 `log_rejected_record_num` 限制 | 可配置，默认记录所有 |
| 错误分类 | 无结构化分类 | 标准错误码体系 |
| 重处理能力 | 无 | 支持 SQL 驱动的重新导入 |
| 生命周期 | 依赖 BE 清理策略 | 分区 TTL 自动过期 |
| 可观测性 | 无专门 metrics | 完整的 metrics 体系 |
| 对性能影响 | 本地文件写入，低 | 涉及表写入，需异步和批量优化 |
| 适用场景 | 调试 | 生产环境数据质量保障 |

## 11. 开放问题

1. **DLQ 表的命名规范**：`__starrocks_dlq_<db>_<table>` vs `<db>.__dlq_<table>` vs 用户自定义表名？
2. **DLQ 表的 schema 演进**：如果目标表 schema 变更，DLQ 表是否需要跟随变更？由于 DLQ 存储原始数据（VARCHAR），schema 变更影响较小。
3. **跨集群 DLQ**：shared-data 模式下，DLQ 表应该如何处理？
4. **DLQ 写入失败的处理**：best-effort 是否足够？是否需要 fallback 到本地文件？
5. **Routine Load Kafka DLQ 的事务性**：如何保证 DLQ topic 写入和 offset 提交的一致性？
6. **大文件导入场景**：Broker Load 导入 TB 级文件时，如果有大量坏行，DLQ 写入可能成为瓶颈。
7. **DLQ 表的存储格式**：原始记录存储为 VARCHAR 可能损失二进制数据的保真度，是否需要 VARBINARY 支持？

## 12. 里程碑计划

### Phase 1: 基础 DLQ 框架

- [ ] DLQ 表自动创建和管理（FE）
- [ ] DLQ Writer 实现（BE）
- [ ] Stream Load DLQ 支持
- [ ] INSERT INTO DLQ 支持
- [ ] SQL 查询 DLQ 数据
- [ ] 基本 metrics
- [ ] 表属性配置

### Phase 2: 完整导入支持

- [ ] Broker Load DLQ 支持
- [ ] Routine Load DLQ 支持
- [ ] Kafka DLQ Topic（可选）
- [ ] `SHOW DEAD LETTER QUEUE` 语法
- [ ] DLQ 分区 TTL 管理
- [ ] 完整 metrics 和监控集成

### Phase 3: 高级功能

- [ ] `REPROCESS DEAD LETTER QUEUE` 语法
- [ ] 外部存储 DLQ（S3/HDFS）
- [ ] 自动重试框架
- [ ] DLQ 数据可视化（Web UI）
- [ ] 采样模式
- [ ] 跨集群 DLQ 支持
