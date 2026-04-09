# Rejected Records System Table Design

> **Author**: StarRocks Team
> **Status**: Draft
> **Created**: 2026-03-27

## 1. Background and Motivation

### 1.1 Customer Scenario and Original Requirements

**Customer Scenario**: AppLovin bulk-loads data from GCS Parquet files into StarRocks aggregate tables via `INSERT INTO ... SELECT ... FROM FILES()`. They use `max_filter_ratio` to tolerate a small number of bad rows and avoid downstream delays caused by entire-batch failures. However, for filtered/discarded rows, they need the ability to **track, inspect, and replay** all rejected rows — this is critical for data integrity.

**Core Pain Points**:

1. **Hard-coded error log cap is too small**: `kMaxErrorNum = 50` (`runtime_state_helper.cpp`) / `MAX_ERROR_NUM = 50` (`runtime_state.cpp`). A single batch may produce thousands of rejected rows, but only the first 50 are visible. The customer needs visibility into at least 10,000 rows.
2. **Rejected records are not logged by default**: `log_rejected_record_num` defaults to 0, and records are written to BE local files, which cannot be queried via SQL.
3. **No replay capability**: Rejected rows cannot be re-imported. The customer references Vertica's approach (rejected rows exported to a queryable table/folder that can be directly replayed) and wants StarRocks to provide similar capabilities.
4. **A dilemma**: Either set `max_filter_ratio=0` and have the entire batch fail, or allow filtering but lose visibility into bad rows.

**Original requirements broken down into two levels**:

| Level | Requirement | Status |
|-------|-------------|--------|
| **Short-term** | Make `kMaxErrorNum` a configurable session/system variable, allowing users to increase it to 10,000+ | Confirmed feasible (simple change) |
| **Long-term** | Provide structured, replayable rejected row export — write to a queryable table, support `INSERT INTO ... SELECT ... FROM` for replay | Goal of this design document |

### 1.2 Problem Summary

StarRocks currently handles bad data during imports through the **reject record mechanism** and the **error log mechanism**, which have the following systemic issues:

1. **Hard-coded error log row cap**: `kMaxErrorNum = 50`. Only 50 rows are visible out of potentially thousands of errors in a single batch.
2. **Rejected records disabled by default**: `log_rejected_record_num` defaults to 0. Even when enabled, records are written to BE local files with no HTTP download interface — SSH access to the BE node is required.
3. **Non-standard format**: Rejected records are written as tab-delimited text with no structured metadata (error codes, source location, timestamps, etc.).
4. **No replay support**: Rejected rows cannot be re-imported into the target table.
5. **Bad data permanently lost in Routine Load scenarios**: Once Kafka offsets advance, there is no way to go back.
6. **Especially poor for Parquet/ORC scenarios**: In `arrow_to_starrocks_converter.cpp`, Parquet only records column-level error context (`file=, column=, raw_data=`); `orc_chunk_reader.cpp` passes in an empty string, providing no usable row-level data.

### 1.3 Design Goals

Introduce a unified mechanism for persisting bad import data, **directly benchmarked against Vertica's rejected data table capability**:

| Goal | Corresponding Requirement | Description |
|------|--------------------------|-------------|
| **Full capture** | "10,000+ rows visibility" | All filtered rows are written to a queryable table, no longer limited to 50 rows |
| **SQL queryable** | "inspect rejected rows" | Query error data via standard SQL, including error reason, error column, and raw data |
| **Replayable** | "replay failed records" | `INSERT INTO target SELECT ... FROM dlq_table` for re-import |
| **Parquet scenario support** | "GCS Parquet → INSERT INTO ... SELECT ... FROM FILES()" | Rejected rows from Parquet/ORC columnar formats can also be structurally recorded and replayed |
| **Works with max_filter_ratio** | "partial success + inspect + replay pattern" | When rejected records are enabled, rows filtered by `max_filter_ratio` are automatically written to the system table |
| **All import methods** | Generalized | Stream Load, Routine Load, Broker Load, and INSERT are all supported |
| **Zero external dependencies** | Simple deployment | Pure StarRocks built-in OLAP table, no external systems like Kafka/S3 required |

### 1.4 Terminology Discussion: Why "Dead Letter Queue"?

**Origin of "Dead Letter"**: The term originates from the postal system. In 1737, Benjamin Franklin, as postmaster of Philadelphia, first used the term "dead letters" to refer to undeliverable mail. In 1825, the U.S. Postal Service formally established the Dead Letter Office, dedicated to handling mail that could not be delivered or returned due to incomplete addresses, non-existent recipients, etc.

**Adoption in computing**: Message queue systems (IBM MQ, RabbitMQ, Kafka Connect, etc.) borrowed this analogy — messages that cannot be successfully processed by consumers are routed to a dedicated "Dead Letter Queue". The core semantics are "data that cannot reach its destination".

**Is it applicable to batch imports?** The word "Queue" implies streaming/message pipeline semantics and is strictly more suited to real-time import scenarios like Routine Load (Kafka). For batch imports such as Stream Load and Broker Load, the industry uses a variety of different terms:

| System | Term | Applicable Scenario |
|--------|------|---------------------|
| Kafka Connect / RabbitMQ / Flink | **Dead Letter Queue / Topic** | Streaming message processing |
| ClickHouse v25.8 | **`system.dead_letter_queue`** | Kafka/RabbitMQ engine (streaming) |
| Amazon Redshift | **`STL_LOAD_ERRORS`** (Load Error Table) | COPY command (batch) |
| Vertica | **Rejected Data Table** | COPY (batch) |
| Teradata | **Error Table** | Batch loading |
| Oracle | **Bad Table** (`COPY$_BAD`) | Batch loading |
| Greenplum / HAWQ | **Error Table** | Batch loading |
| Apache Doris / StarRocks (existing) | **Rejected Record** | All imports |

**StarRocks naming choice**:

StarRocks covers both streaming (Routine Load) and batch (Stream Load / Broker Load / INSERT) scenarios. Candidate naming options:

| Candidate Name | Table Name | Pros | Cons |
|---------------|------------|------|------|
| **Dead Letter Queue** | `_rejected_records` | Most widely known in the industry; already adopted by ClickHouse | "Queue" implies streaming, slightly unnatural for batch scenarios |
| **Load Error Table** | `_load_errors` | Semantically accurate; Redshift `STL_LOAD_ERRORS` precedent | Less recognizable than DLQ |
| **Rejected Data Table** | `_rejected_data` | Consistent with existing rejected record concept; Vertica precedent | "rejected" does not imply reprocessability |
| **Error Table** | `_error_table` | Teradata/Greenplum precedent; concise | Too generic, may be confused with other errors |
| **Quarantine Table** | `_quarantine` | Semantically precise ("isolated for processing"); implies reprocessability | Rarely used in the industry |

> **Final choice**: Adopt **`_rejected_records`**, consistent with StarRocks' existing rejected record concept (Vertica precedent), while implying that data can be inspected and reprocessed.

## 2. Industry Survey

### 2.1 Message Queues / Stream Processing Systems

| System | DLQ Implementation | Characteristics |
|--------|-------------------|-----------------|
| **Apache Kafka (Connect)** | Separate DLQ Topic (`errors.tolerance=all`), bad messages auto-routed to `dlq-<connector>` topic, original key/value preserved, error metadata written to Kafka Headers | Native support, zero data loss, DLQ topic can be re-consumed |
| **Apache Flink** | Side Output mechanism (DataStream API), Confluent Flink SQL supports `error-handling.mode` to configure DLQ table | Limited to deserialization errors, UDF errors not supported |
| **Kafka Connect** | `errors.tolerance=all` + `errors.deadletterqueue.topic.name`, supports retry with exponential backoff | Industry standard pattern, Headers carry rich error context |

**Kafka DLQ Best Practices**:
- Preserve the original message key and value, do not wrap
- Attach metadata via Kafka Headers: original topic, partition, offset, error message, application name, error timestamp
- Use one DLQ topic per service/connector (not one per source topic)
- Distinguish transient errors (retry) from permanent errors (enter DLQ)
- Set up monitoring and alerting for the DLQ

### 2.2 Data Warehouses / OLAP Systems

| System | Error Handling | DLQ Support | Characteristics |
|--------|---------------|-------------|-----------------|
| **ClickHouse** (v25.8+) | `kafka_handle_error_mode='dead_letter'` → `system.dead_letter_queue` system table | **Native support** | Parse-failed Kafka/RabbitMQ messages auto-written to system table, including full error info and source message metadata |
| **Snowflake** | Openflow Kafka Connector supports DLQ topic; `COPY INTO` uses `ON_ERROR` parameter | **Partial support** (Kafka Connector only) | Kafka connector supports routing parse-failed messages to a DLQ Kafka topic; general data import (COPY INTO/Snowpipe) still uses traditional ON_ERROR/VALIDATION_MODE |
| **Amazon Redshift** | `MAXERROR` parameter + `STL_LOAD_ERRORS` / `SYS_LOAD_ERROR_DETAIL` system tables | **Native support** (system tables) | Global system tables persistently record detailed failure information for all COPY command rows, with automatic row-level isolation by userid |
| **Google BigQuery** | `max_bad_records` parameter controls tolerance count, Job result includes error details | None | Bad rows are simply skipped, not stored separately |
| **Vertica** | `REJECTED DATA AS TABLE` clause, per-load rejected data table (10 columns, including raw text line) | **Native support** | The benchmark system referenced by the customer. Per-load table, raw text line, SQL queryable, exportable for replay |
| **Apache Doris** | `max_filter_ratio` + `strict_mode`, `ErrorURL` provides HTTP download of error log | None | Essentially the same mechanism as StarRocks (same origin project) |

#### 2.2.1 ClickHouse `system.dead_letter_queue` Deep Dive (v25.8+, 2025-08)

ClickHouse introduced native DLQ support in v25.8, storing parse-failed messages from Kafka/RabbitMQ engines in the `system.dead_letter_queue` system table.

**How to enable**: Set `kafka_handle_error_mode='dead_letter'` on a Kafka/RabbitMQ engine table.

**`system.dead_letter_queue` Table Schema**:

| Column Name | Type | Description |
|-------------|------|-------------|
| `table_engine` | Enum8 | Streaming engine type: Kafka / RabbitMQ |
| `event_date` | Date | Message consumption date |
| `event_time` | DateTime | Message consumption time |
| `event_time_microseconds` | DateTime64 | Microsecond-precision time |
| `database` | LowCardinality(String) | Database |
| `table` | LowCardinality(String) | Table name |
| `error` | String | Error text |
| `raw_message` | String | Raw message body |
| `kafka_topic_name` | String | Kafka topic name |
| `kafka_partition` | UInt64 | Kafka partition |
| `kafka_offset` | UInt64 | Kafka offset |
| `kafka_key` | String | Kafka message key |

**Query example**:
```sql
SELECT event_time, database, table, error, raw_message, kafka_topic_name, kafka_partition, kafka_offset
FROM system.dead_letter_queue
WHERE event_date = today()
ORDER BY event_time DESC
LIMIT 10;
```

**Design highlights**:
- System-level global table, all Kafka/RabbitMQ engine tables share the same DLQ table
- Data is not automatically deleted; users must clean up manually
- Flush frequency controlled by `flush_interval_milliseconds`
- Only supports deserialization errors from streaming engines (Kafka/RabbitMQ), not INSERT/file import scenarios
- Evolution history: `default` (silent ignore) → `stream` (virtual columns) → `dead_letter` (system table)

**Implications for StarRocks**:
- ClickHouse's approach validates the feasibility of the "system table stores DLQ data + SQL queryable" pattern
- However, it is limited to Kafka/RabbitMQ engines; StarRocks' DLQ should cover all import methods (Stream Load/Routine Load/Broker Load/INSERT)
- StarRocks also adopts a global single-table design, but uses automatic Row Access Policy to achieve row-level isolation by `target_database.target_table` permissions, which is more granular than ClickHouse's lack of row-level filtering

#### 2.2.2 Snowflake Openflow Kafka Connector DLQ

Snowflake's DLQ support is implemented through the **Openflow Kafka Connector** (Apache Kafka with DLQ and metadata connector):

**How to enable**: Set the `Kafka DLQ Topic` parameter in the connector configuration (required).

**DLQ behavior**:
- **Parse failures**: Invalid JSON/AVRO format messages routed to DLQ topic
- **Schema mismatch**: When schema evolution is disabled, messages not matching the schema are routed to DLQ topic
- **Processing errors**: Other processing failures during ingestion

**Limitations**:
- Only applies to the Openflow Kafka Connector, not to `COPY INTO`, Snowpipe, Snowpipe Streaming, or other general imports
- DLQ target is a Kafka topic (not a Snowflake table), requiring additional Kafka infrastructure
- Snowpipe Streaming's `ON_ERROR` only supports `CONTINUE`, no DLQ option

**Implications for StarRocks**:
- Snowflake restricts DLQ to the Kafka Connector scope and depends on external Kafka infrastructure; StarRocks adopts a built-in OLAP table approach covering all import methods with zero external dependencies

#### 2.2.3 Amazon Redshift `STL_LOAD_ERRORS` / `SYS_LOAD_ERROR_DETAIL` Deep Dive

Redshift is one of the earliest data warehouse systems to implement "DLQ-like" capability through system tables, providing two generations of system tables for recording COPY command load errors.

**Two-generation system table comparison**:

| Dimension | `STL_LOAD_ERRORS` (Classic) | `SYS_LOAD_ERROR_DETAIL` (New generation, recommended) |
|-----------|-----------------------------|-------------------------------------------------------|
| Scope | Provisioned Cluster main cluster only | Provisioned + Serverless + concurrency scaling clusters |
| Data retention | 7-day log rotation, no auto-backup | Same |
| Availability | Not available on Serverless | Available on all platforms |

**`STL_LOAD_ERRORS` Table Schema (20 columns)**:

| Column Name | Type | Description |
|-------------|------|-------------|
| `userid` | integer | User ID that executed the load |
| `slice` | integer | Slice where the error occurred |
| `tbl` | integer | Target table ID |
| `starttime` | timestamp | Load start time (UTC) |
| `session` | integer | Session ID |
| `query` | integer | Query ID (can JOIN with other system tables) |
| `filename` | char(256) | Full path of input file |
| `line_number` | bigint | Error line number (for JSON, the line number of the last line) |
| `colname` | char(127) | Name of the error column |
| `type` | char(10) | Data type of the error column |
| `col_length` | char(10) | Column length limit (e.g., `3` for `character(3)`) |
| `position` | integer | Position of the error within the field |
| `raw_line` | char(1024) | Raw data line containing the error |
| `raw_field_value` | char(1024) | Raw field value that caused the parse error |
| `err_code` | integer | Error code |
| `err_reason` | char(100) | Error reason description |
| `is_partial` | integer | Whether the input file was split (1=yes) |
| `start_offset` | bigint | Split offset (bytes) |
| `copy_job_id` | bigint | COPY job identifier |

**`SYS_LOAD_ERROR_DETAIL` Table Schema (New generation, more concise)**:

| Column Name | Type | Description |
|-------------|------|-------------|
| `user_id` | integer | Executing user ID |
| `query_id` | bigint | Query ID |
| `transaction_id` | bigint | Transaction ID |
| `session_id` | integer | Session ID |
| `database_name` | char(64) | Database name |
| `table_id` | integer | Target table ID |
| `start_time` | timestamp | Load start time |
| `file_name` | char(256) | Input file path |
| `line_number` | bigint | Error line number |
| `column_name` | char(127) | Error column name |
| `column_type` | char(10) | Error column type |
| `column_length` | char(10) | Column length |
| `position` | integer | Error position |
| `error_code` | integer | Error code |
| `error_message` | char(512) | Error message (longer than STL version: 512 vs 100) |

**Key characteristics**:

1. **Global shared system table**: COPY errors from all databases and all tables across the entire cluster are stored in a single system table
2. **Automatic row-level permission isolation** (see Section 2.5 below): Superusers can see all rows; regular users can only see data they produced
3. **Can be queried jointly with `STL_LOADERROR_DETAIL`** for more detailed context information
4. **7-day automatic rotation**: System table data is retained for only 7 days; for long-term retention, manual UNLOAD to S3 is required
5. **Does not contain complete raw records**: `raw_line` is limited to char(1024); overly long records are truncated

**Typical query examples**:

```sql
-- View errors from the most recent COPY
SELECT d.query, substring(d.filename, 14, 20),
       d.line_number AS line,
       substring(d.value, 1, 16) AS value,
       substring(le.err_reason, 1, 48) AS err_reason
FROM stl_loaderror_detail d, stl_load_errors le
WHERE d.query = le.query
  AND d.query = pg_last_copy_id();

-- New generation SYS view query
SELECT query_id, table_id, start_time,
       trim(file_name) AS file_name,
       trim(column_name) AS column_name,
       trim(error_message) AS error_message
FROM sys_load_error_detail
WHERE query_id = 762949
ORDER BY start_time LIMIT 10;
```

**Implications for StarRocks**:
- Redshift's automatic `userid` row filtering is a mature practice for permission isolation in a global single-table design
- The `raw_line` char(1024) truncation design shows that raw data storage requires a trade-off between completeness and storage overhead
- The 7-day automatic rotation is consistent with StarRocks' `partition_live_number` approach
- The more concise evolution direction from `STL_LOAD_ERRORS` to `SYS_LOAD_ERROR_DETAIL` is worth referencing

### 2.2.4 Permission Control Approaches for Global/Shared System Tables

The following compares permission control approaches across different systems for the "one table stores all error data" scenario:

| System | Permission Model | Implementation Mechanism | Granularity |
|--------|-----------------|--------------------------|-------------|
| **Amazon Redshift** | **Automatic row filtering by userid** | Built-in system table behavior: superusers see all rows, regular users only see their own (`userid = current_user_id`) produced rows. Can be elevated via `ALTER USER ... SYSLOG ACCESS UNRESTRICTED` | User-level |
| **ClickHouse** | **system database globally accessible** | `system` database is accessible to all users by default (used for query processing); `system.dead_letter_queue` does not perform additional row-level filtering. Table-level access controlled via `GRANT SELECT ON system.dead_letter_queue TO ...` | Table-level |
| **PostgreSQL (as DLQ)** | **Standard GRANT + RLS** | DLQ table uses standard `GRANT` for table-level access; Row-Level Security (RLS) Policy can filter rows by user/role | Table-level / Row-level (requires manual RLS configuration) |

**Redshift permission model in-depth analysis**:

```
┌───────────────────────────────────────────────────────────┐
│              Redshift System Table Permission Model        │
│                                                            │
│  STL_LOAD_ERRORS / SYS_LOAD_ERROR_DETAIL                  │
│  ┌──────────────────────────────────────────────────┐     │
│  │ userid=1 (superuser)  → all rows visible          │     │
│  │ userid=100 (user_a)   → only userid=100 rows      │     │
│  │ userid=101 (user_b)   → only userid=101 rows      │     │
│  │ userid=102 (unrestricted_user)                    │     │
│  │   ALTER USER unrestricted_user                    │     │
│  │     SYSLOG ACCESS UNRESTRICTED;                   │     │
│  │   → all rows visible after elevation              │     │
│  └──────────────────────────────────────────────────┘     │
│                                                            │
│  Characteristics:                                          │
│  - Permission filtering done automatically at engine       │
│    level, transparent to users                             │
│  - No RLS policy needed, built-in system table behavior    │
│  - Granularity is "whoever executed the COPY sees it"      │
│  - Cannot do fine-grained permission control by target     │
│    table                                                   │
└───────────────────────────────────────────────────────────┘
```

#### 2.2.5 Vertica Rejected Data Table Deep Dive

Vertica is the system explicitly referenced by the customer (AppLovin), and its rejected data table is the direct benchmark for this design.

**How to enable**: Use the `REJECTED DATA AS TABLE` clause in a COPY statement:

```sql
COPY target_table FROM '/data/input.csv' REJECTED DATA AS TABLE my_rejects;
```

**Table creation**:
- Vertica automatically creates (if not exists) or appends (if exists). Cannot be manually created outside of COPY.
- It is a special type of table: only supports SELECT and DROP TABLE, **does not support DML (INSERT/UPDATE/DELETE) or DDL (ALTER)**.
- Not K-safe by default (no replicas); requires `CREATE TABLE AS SELECT` to copy to a regular table for high availability.

**Table Schema (10 columns)**:

| Column Name | Type | Description |
|-------------|------|-------------|
| `node_name` | VARCHAR | Vertica node name that executed the load |
| `file_name` | VARCHAR | Input file name ("STDIN" for STDIN) |
| `session_id` | VARCHAR | Session ID of the COPY statement |
| `transaction_id` | INTEGER | Transaction ID (can JOIN `QUERY_REQUESTS` and other system tables) |
| `statement_id` | INTEGER | Statement ID within the transaction |
| `batch_number` | INTEGER | Internal use, indicates which batch (chunk) the data came from |
| `row_number` | INTEGER | Row number in the input file (-1 if undetermined) |
| `rejected_data` | LONG VARCHAR | **Raw rejected data line** |
| `rejected_data_orig_length` | INTEGER | Original data length |
| `rejected_reason` | VARCHAR | Rejection reason (same content as exceptions file) |

**Key characteristics**:

1. **Per-load table, not a global table**: Each COPY specifies a reject table name, managed by the user. Multiple COPYs can append to the same reject table, but there is no global sharing mechanism.
2. **`rejected_data` stores the raw text line**: LONG VARCHAR type, no conversion or structuring — it is the raw text of the rejected line from the input file. This means replay requires re-parsing via COPY, not direct INSERT.
3. **Replay workflow**: export → fix → re-COPY:
   ```sql
   -- Export rejected raw data
   \o rejected.txt
   SELECT rejected_data FROM my_rejects;
   \o
   -- Fix rejected.txt then reload
   COPY target_table FROM 'rejected.txt' ...;
   ```
   Or replay directly via pipe (using `::!` for forced type casting):
   ```sql
   \! vsql -Atc "SELECT rejected_data FROM my_rejects;" | vsql -c "COPY target_table (col_filler FILLER VARCHAR, col AS col_filler::!INT) FROM STDIN;"
   ```
4. **`REJECTMAX`**: Controls the maximum number of rejected rows; COPY fails when this value is reached. `0` = no limit. Similar to StarRocks' `max_filter_ratio`.
5. **Transformation rejection**: By default, only parse-phase errors are captured. To capture expression evaluation phase errors, set `CopyFaultTolerantExpressions=1`.

**Comparison with StarRocks design**:

| Dimension | Vertica | StarRocks (this design) |
|-----------|---------|------------------------|
| Table granularity | Per-load (user specifies table name for each COPY) | Global single table (`_statistics_._rejected_records`) |
| Data format | Raw text line (LONG VARCHAR) | Unified JSON (`{col: val, ...}`) |
| Replay method | Export file → re-COPY | `INSERT INTO target SELECT raw_record->>'col' FROM _rejected_records` |
| Permission isolation | Users manage their own reject tables | Automatic Row Access Policy |
| Write timing | Synchronous write during COPY statement | BE local file + background async sync |
| DML support | INSERT/UPDATE/DELETE not supported | Supported (PK table) |
| Automatic cleanup | None (manual DROP recommended) | `partition_live_number` TTL auto-expiry |

**Implications for StarRocks**:
- Vertica's `rejected_data` is **raw text line**, simple but unstructured. StarRocks chooses JSON to support Parquet/ORC and other formats without raw text lines, as well as more convenient per-column extraction for replay.
- Vertica's per-load table model is natural for ELT workflows (load → inspect → replay → drop), but management is fragmented. StarRocks' global table + TTL is better suited for continuous operations.
- Vertica's replay requires exporting to a file then re-COPY; StarRocks' JSON format supports `INSERT INTO ... SELECT` for direct replay without intermediate files.

### 2.3 Data Processing Frameworks

| System | Error Handling | Characteristics |
|--------|---------------|-----------------|
| **Apache Spark** | `badRecordsPath` writes bad records to a specified path (JSON format), PERMISSIVE mode puts bad data into `_corrupt_record` column | badRecordsPath is mainly for Databricks, limited support in open-source version |
| **PostgreSQL as DLQ** | Uses database tables as DLQ, records source_table, record_id, payload (JSON), error_message, attempt_count, etc. | SQL queryable, ACID guarantees, `FOR UPDATE SKIP LOCKED` for parallel reprocessing |

### 2.4 Core Design Patterns Summary

The following core design patterns are distilled from industry approaches:

1. **Layered error handling**: Distinguish transient errors (retryable) from permanent errors (enter rejected records)
2. **Preserve raw data**: Rejected records must contain raw data for reprocessing
3. **Rich error context**: Error reason, source location, timestamp, load label, etc.
4. **Queryability**: Rejected records should be queryable via standard SQL
5. **Reprocessability**: Provide a mechanism to re-import rejected records
6. **Lifecycle management**: Rejected records should have TTL and cleanup policies
7. **Observability**: Metrics and alerting support

## 3. Existing Mechanism Analysis

### 3.1 Error Log Mechanism

```
Data flow: Scanner/Sink → append_error_msg_to_file() → BE local file
Access: ErrorURL → HTTP GET /api/_load_error_log?file=...
Limitation: Max 50 rows (kMaxErrorNum), only records "Error: <reason>. Row: <line>"
```

**Key code paths**:
- BE: `be/src/runtime/runtime_state_helper.cpp` → `append_error_msg_to_file()`
- BE: `be/src/runtime/load_path_mgr.cpp` → File path management
- FE: `QueryRuntimeProfile.java` → Collects `tracking_url`

### 3.2 Rejected Record Mechanism

```
Data flow: Scanner/Sink → append_rejected_record_to_file() → BE local file
Access: rejected_record_path (BE local path, no HTTP interface)
Limitation: Controlled by log_rejected_record_num (default 0 = no logging)
Format: record\terror_msg\tsource (tab-delimited)
```

**Key code paths**:
- BE: `be/src/runtime/runtime_state_helper.cpp` → `append_rejected_record_to_file()`
- BE: `be/src/runtime/runtime_state.h` → `enable_log_rejected_record()`
- FE: `SessionVariable.java` → `log_rejected_record_num`
- FE: `QueryRuntimeProfile.java` → Collects `rejected_record_path`

### 3.3 Filter Control Mechanism

| Parameter | Scope | Description |
|-----------|-------|-------------|
| `max_filter_ratio` | Stream Load / Broker Load | Maximum allowed filter ratio (0~1) |
| `max_error_number` | Routine Load | Cumulative error row limit; job pauses when exceeded |
| `insert_max_filter_ratio` | INSERT statements | Maximum allowed filter ratio for INSERT |
| `enable_insert_strict` / `strict_mode` | INSERT / Broker Load | Strict mode control |

### 3.4 Core Issues with Existing Mechanisms

```
┌─────────────────────────────────────────────────────────────────┐
│                     Existing Architecture                       │
│                                                                 │
│  Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ Table│
│                                              │                  │
│                                              ▼                  │
│                                    ┌──────────────────┐        │
│                                    │  BE Local Files   │        │
│                                    │  (error_log +     │        │
│                                    │   rejected_record)│        │
│                                    └──────────────────┘        │
│                                         │                      │
│                                    ❌ Not persisted             │
│                                    ❌ Hard to access            │
│                                    ❌ Cannot reprocess          │
│                                    ❌ Quantity limited           │
│                                    ❌ Non-standard format        │
└─────────────────────────────────────────────────────────────────┘
```

## 4. High-Level Design

### 4.1 Design Goals

| Goal | Priority | Description |
|------|----------|-------------|
| Bad data persistent storage | P0 | All filtered rows written to system table, no loss |
| SQL queryable | P0 | Query rejected data via standard SQL |
| Support re-import | P1 | Select data from system table for re-import to target table |
| All import methods supported | P1 | Stream Load, Routine Load, Broker Load, INSERT |
| Lifecycle management | P1 | TTL auto-expiry + manual cleanup |
| Observability | P2 | Metrics + alerting |
| Minimal impact on import performance | P0 | Rejected records writing should not significantly affect normal import throughput and latency |

### 4.2 Overall Architecture

Adopts a **global single rejected records system table** design (consistent with ClickHouse `system.dead_letter_queue` and Redshift `STL_LOAD_ERRORS`), using `target_database` + `target_table` columns to distinguish different targets, automatic Row Access Policy for permission isolation, and zero external dependencies.

```
Source Data ──→ [Parse/Transform] ──→ [Quality Check] ──→ db.table
                                            │
                                            ▼ (rejected rows)
                                   Rejected Record Writer (BE)
                                   → JSON Lines local file
                                            │
                                            ▼ (RejectedRecordSyncDaemon async)
                              _statistics_._rejected_records
                              (global PK table, merge commit write)
                                            │
                        ┌───────────────────┼───────────────────┐
                        ▼                                       ▼
             SELECT * FROM                          INSERT INTO target
             _statistics_._rejected_records            SELECT raw_record->>'col'
             WHERE target_table = '...'             FROM _statistics_._rejected_records
             (automatic Row Access Policy)          (replay)
```

**Core design decisions**:

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Table granularity | Global single table | Consistent with ClickHouse/Redshift; simplest management model; permission isolation solved via automatic Row Access Policy |
| Permission model | Automatic Row Access Policy | FE automatically injects row filtering at query time, filtering by SELECT permission on `target_database.target_table`; transparent to users |
| Storage approach | StarRocks built-in OLAP table | Zero external dependencies; standard SQL queries and operations; reuses existing storage engine |
| Partitioning | Expression partition (`PARTITION BY date_trunc('day', created_at)`) | Auto-partition by day + `partition_live_number` TTL |
| Data model | Primary Key | PK table supports UUID deduplication, combined with at-least-once retry achieves exactly-once write |

### 4.3 System Table Schema Detailed Design

#### 4.3.1 Cross-Industry Schema Comparison

First, summarize which fields each system records, then derive the optimal schema for StarRocks.

| Field Category | Redshift `STL_LOAD_ERRORS` | Redshift `SYS_LOAD_ERROR_DETAIL` | ClickHouse `system.dead_letter_queue` | StarRocks existing rejected record | Design Trade-off |
|---------------|---------------------------|----------------------------------|--------------------------------------|-------------------------------|-----------------|
| **Time** | `starttime` | `start_time` | `event_time`, `event_time_microseconds`, `event_date` | None | Needed, also serves as partition key |
| **User** | `userid` (row filtering basis) | `user_id` | None | None | Needed for auditing and optional user-level filtering |
| **Target DB** | None (single-DB model) | `database_name` | `database` | None | Needed, essential for global table |
| **Target Table** | `tbl` (table ID) | `table_id` | `table` (table name) | None | Needed, use table name instead of ID (more readable, permission checks are name-based) |
| **Load Identifier** | `query`, `session`, `copy_job_id` | `query_id`, `transaction_id`, `session_id` | None | None | Need `load_label` (core identifier in StarRocks import system) + `txn_id` |
| **Load Type** | Implicit (COPY only) | Implicit (COPY only) | `table_engine` (Kafka/RabbitMQ) | None | Needed, StarRocks has 4 import methods |
| **Error Code** | `err_code` (integer) | `error_code` (integer) | None (error text) | None | Needed, string enum is more readable than integer |
| **Error Message** | `err_reason` char(100) | `error_message` char(512) | `error` (String) | error_msg (no length limit) | Needed, VARCHAR(1024) balances information and storage |
| **Error Column** | `colname`, `type`, `col_length`, `position` | `column_name`, `column_type`, `column_length`, `position` | None | None | Need column name; type/position info can be included in error message |
| **Raw Data** | `raw_line` char(1024), `raw_field_value` char(1024) | None | `raw_message` (String) | record (no length limit) | **Core field**, unified JSON format (column values serialized, format-independent) |
| **Source File/Location** | `filename` char(256), `line_number`, `is_partial`, `start_offset` | `file_name`, `line_number` | `kafka_topic_name`, `kafka_partition`, `kafka_offset`, `kafka_key` | source (unstructured) | JSON type, flexibly adapts to file/Kafka/memory sources |
| **Data Format** | Implicit | Implicit | Implicit | None | Recorded in `source_info` JSON (`format` field) |
| **Node Info** | `slice` | None | None | None | `backend_id`, for troubleshooting |

#### 4.3.2 Final Schema

```sql
CREATE TABLE _statistics_._rejected_records (
    -- ① Unique identifier (PK, idempotent write guarantee)
    id                  VARCHAR(64)     NOT NULL COMMENT 'UUID, generated by BE, used for at-least-once retry deduplication',

    -- ② Time (partition key)
    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                        COMMENT 'Record write time',

    -- ③ Target info (permission filtering basis)
    target_database     VARCHAR(256)    NOT NULL COMMENT 'Target database name',
    target_table        VARCHAR(256)    NOT NULL COMMENT 'Target table name',

    -- ④ Import context
    load_label          VARCHAR(256)    NOT NULL COMMENT 'Import Label (unique identifier in StarRocks import system)',
    load_type           VARCHAR(32)     NOT NULL COMMENT 'STREAM_LOAD / ROUTINE_LOAD / BROKER_LOAD / INSERT',
    txn_id              BIGINT          COMMENT 'Transaction ID',
    user_name           VARCHAR(128)    COMMENT 'User name that executed the import',

    -- ⑤ Error information
    error_code          VARCHAR(64)     COMMENT 'Error code enum, e.g. TYPE_MISMATCH / NULL_VIOLATION / PARSE_ERROR',
    error_message       VARCHAR(1024)   COMMENT 'Detailed error description',
    error_column        VARCHAR(256)    COMMENT 'Error column name (if determinable)',

    -- ⑥ Rejected row data
    raw_record          JSON            COMMENT 'Rejected row data, unified JSON format {col: value, ...}',

    -- ⑦ Source information
    source_info         JSON            COMMENT 'Source metadata, structure varies by import type',

    -- ⑧ Runtime information
    backend_id          BIGINT          COMMENT 'BE node ID that produced this record'
) ENGINE = OLAP
PRIMARY KEY(id, created_at)
PARTITION BY date_trunc('day', created_at)
DISTRIBUTED BY HASH(id)
PROPERTIES (
    "partition_live_number" = "7",
    "replication_num" = "3"
);
```

#### 4.3.3 Per-Column Design Rationale

**① `id` — Unique Identifier**

| Dimension | Design |
|-----------|--------|
| Type | `VARCHAR(64)` NOT NULL |
| Purpose | First column of Primary Key; deduplication basis for background sync at-least-once retries |
| Generation | UUID generated by BE's Rejected Record Writer during `append()` (`generate_uuid_string()`) |
| Why needed | `RejectedRecordSyncDaemon` writes to the system table via Stream Load, and may retry on network timeout or similar scenarios. The PK table guarantees that records with the same `id` are stored only once, achieving at-least-once + idempotent = exactly-once semantics |

**② `created_at` — Time**

| Dimension | Design |
|-----------|--------|
| Type | `DATETIME` |
| Purpose | Second column of Primary Key; partition key (by day); query filtering; TTL basis |
| Reference | ClickHouse uses `event_time`, Redshift uses `start_time`, both use time as primary key/partition |

**③ `target_database` + `target_table` — Target Information**

| Dimension | Design |
|-----------|--------|
| Type | `VARCHAR(256)` NOT NULL |
| Purpose | Identifies which table the bad data belongs to; **filtering basis for Row Access Policy** |
| Reference | ClickHouse uses `database` + `table` (table name); Redshift uses `tbl` (table ID) |
| Choosing table name over ID | Table ID requires JOINing metadata tables to be readable; table name is directly readable and permission checks are based on `db.table` name |
| Query efficiency | Not a PK prefix column, but high-frequency query conditions can be accelerated via secondary index or bitmap index |

**④ Import Context**

| Column | Type | Description |
|--------|------|-------------|
| `load_label` | `VARCHAR(256)` NOT NULL | Core identifier in StarRocks import system. Stream Load, Broker Load, and Routine Load each have a unique label per task, which is the user's primary anchor for troubleshooting. Redshift equivalent: `query_id` + `copy_job_id`. |
| `load_type` | `VARCHAR(32)` NOT NULL | Distinguishes four import methods. ClickHouse uses `table_engine` to distinguish Kafka/RabbitMQ; StarRocks needs to distinguish more types. |
| `txn_id` | `BIGINT` | Transaction ID, can be correlated with system tables like `information_schema.loads`. Redshift equivalent: `transaction_id`. |
| `user_name` | `VARCHAR(128)` | User name that executed the import. Redshift's `userid` is the core of row filtering; StarRocks' Row Access Policy is based on target_table permissions rather than userid, but user_name is retained for auditing. |

**⑤ Error Information**

| Column | Type | Description |
|--------|------|-------------|
| `error_code` | `VARCHAR(64)` | String enum (e.g., `TYPE_MISMATCH`), more readable than Redshift's integer `err_code`, no need to look up error code documentation. |
| `error_message` | `VARCHAR(1024)` | Redshift evolved from 100 characters in STL to 512 characters in SYS; ClickHouse has no limit. 1024 characters balances information and storage. |
| `error_column` | `VARCHAR(256)` | Error column name. Redshift additionally records `type`/`col_length`/`position`, but this information can be fully included in `error_message` without needing separate columns. |

**⑥ `raw_record` — Rejected Row Data**

| Column | Type | Description |
|--------|------|-------------|
| `raw_record` | `JSON` | Rejected row data, **unified JSON format** (`{col_name: value, ...}`). |

**Core design decision: `raw_record` uses JSON type, storing serialized target table column values**.

#### Why JSON

Original input formats vary (CSV/JSON/Parquet/ORC/Avro), and at different rejection stages in BE, original format text may no longer be available (see Section 5.1 for details). A unified `raw_record` storage format is needed. Candidate comparison:

| Candidate | Storage Content | Replay Method | Pros | Cons |
|-----------|----------------|---------------|------|------|
| **JSON** (`{col: val}`) | Target table column name → column value KV pairs | `raw_record->>'col'` to extract values | Self-describing, includes column names; StarRocks native JSON type supports arrow operators for extraction; replay is column-order independent | Serialization overhead; nested types (ARRAY/MAP/STRUCT) require recursive serialization |
| **CSV** (comma-separated values) | Column values concatenated in target table column order | Split by position | Simplest serialization (existing `rebuild_csv_row()` already implemented) | Not self-describing — no column names, replay must strictly align by position; commas/newlines in values need escaping; `debug_item()` wraps strings in single quotes, incompatible with standard CSV |
| **VARCHAR raw text** | Original input text | Parse by original format | Highest fidelity | Parquet/ORC have no raw text; at Sink stage, raw text for all formats is unavailable; different formats require different replay methods |

**Reasons for choosing JSON**:

1. **Self-describing**: `{"order_id": 10001, "amount": "bad"}` includes column names, immediately readable by users; replay uses `raw_record->>'col'` to extract by name, independent of column order. CSV's `10001,bad` loses column name information; replay must know column order.
2. **Unified replay**: Replay SQL is identical for all formats and all stages (`raw_record->>'col_name'`), no need to distinguish original format.
3. **StarRocks native support**: JSON is a first-class type in StarRocks, supporting `->>` / `->` / `json_query` / `get_json_string` and other rich extraction functions. Stored in the system table using StarRocks' binary JSON format (flatbuffers), more compact and faster to query than text JSON.
4. **Complex type compatibility**: ARRAY, MAP, STRUCT and other nested types can naturally serialize to JSON arrays/objects; CSV cannot express these.

**Reasons for not choosing CSV**: The existing `rebuild_csv_row()` seems directly reusable, but has a fundamental problem — it calls `debug_item()` to build values, which wraps string types in single quotes (`'Alice'` instead of `Alice`) and outputs numbers directly, with no unified escaping mechanism. This is not standard CSV and cannot be directly used as a re-importable format.

**Reasons for not choosing raw text**: Parquet/ORC have no raw text; at the Sink stage, raw text for all formats is no longer available. Forcing raw text preservation means only Scanner-stage CSV/JSON can be covered, making coverage incomplete.

#### JSON Serialization Construction

Rejected Record Writer adds a `build_json_record()` method (does not reuse `debug_item()`), building standard JSON directly from Column native values:

```cpp
// Pseudocode
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
            builder.add_string(col_names[i], col->get_slice(row_idx));  // no quote wrapping
        } else {
            builder.add_string(col_names[i], col->debug_item(row_idx)); // fallback
        }
    }
    return builder.build();
}
```

#### Scanner Parse Failure Fallback Handling

When a record is rejected at the Scanner stage due to complete parse failure (e.g., CSV column count mismatch preventing column splitting, illegal JSON format), no complete column values are available in the Chunk. In this case, `raw_record` falls back to a JSON containing the raw text: `{"_raw": "raw text content"}`.

#### Removal of `record_format` Column

After unifying `raw_record` to JSON, there is no longer a need to tag the original input format. If the original format information is needed, it can be recorded in `source_info`.

**⑦ `source_info` — Source Information**

| Dimension | Design |
|-----------|--------|
| Type | `JSON` |
| Design rationale | Metadata structures differ completely across import sources. Using fixed columns would result in many NULLs or require separate columns for each source type. Redshift uses `filename` + `line_number` + `is_partial` + `start_offset` (4 columns, file sources only); ClickHouse uses `kafka_topic_name` + `kafka_partition` + `kafka_offset` + `kafka_key` (4 columns, Kafka only). StarRocks needs to simultaneously cover file, Kafka, INSERT, and other sources; JSON is the most flexible choice. |

`source_info` structure for each import type:

```json
// Stream Load / Broker Load (file source)
{
    "format": "csv",
    "file": "hdfs://namenode/data/orders.csv",
    "line": 42,
    "offset": 8192
}

// Broker Load (Parquet source, no line number)
{
    "format": "parquet",
    "file": "gs://bucket/data/orders.parquet"
}

// Routine Load (Kafka source)
{
    "format": "json",
    "topic": "orders_topic",
    "partition": 3,
    "offset": 156789,
    "key": "order_001"
}

// INSERT INTO ... SELECT (query source)
{
    "source_fragment": "INSERT INTO orders SELECT * FROM staging.orders_raw"
}
```

**⑧ `backend_id` — Runtime Information**

Redshift has `slice` (compute slice) as an identifier. `backend_id` records the BE node that produced the rejected record, used for troubleshooting write-side issues.

#### 4.3.4 Fields Not Included in Schema and Rationale

| Field | Source | Reason for Exclusion |
|-------|--------|---------------------|
| `status` / `retry_count` / `resolved_at` | Initial design | The system table is positioned as a data record, not a workflow engine. Reprocessing is orchestrated by users via SQL (query → fix → INSERT INTO target table → DELETE). Although PK tables support UPDATE, introducing state management would increase usage complexity. |
| `id` (AUTO_INCREMENT) | Initial design | Changed to UUID (VARCHAR(64)), generated by BE. AUTO_INCREMENT is not used because multiple BEs write to local files in parallel, and auto-increment IDs cannot guarantee uniqueness across BEs. |
| `column_type` / `column_length` / `position` | Redshift | This detailed column metadata information can be included in `error_message`; separate columns provide low benefit but increase schema complexity. |
| `raw_field_value` | Redshift | Redshift additionally records the raw value of the error field. `raw_record` JSON already contains all column values, and `error_column` already identifies the error column. |
| `record_format` | Initial design | No longer needed after unifying `raw_record` to JSON format. Original input format can be recorded in `source_info` if needed. |
| `kafka_topic` / `kafka_partition` / `kafka_offset` / `kafka_key` | ClickHouse | As separate columns, these only apply to Kafka sources; using `source_info` JSON for unified containment is more flexible. |
| `session_id` | Redshift | StarRocks' import system uses `load_label` + `txn_id` as core identifiers; session information can be correlated from FE audit logs. |
| `load_job_id` | Initial design | Not all import methods have a job_id (Stream Load does not). `load_label` is a more universal identifier. |

#### 4.3.5 Table Property Design Rationale

| Property | Value | Description |
|----------|-------|-------------|
| `ENGINE` | `OLAP` | Standard StarRocks storage engine |
| `PRIMARY KEY` | `(id, created_at)` | PK model guarantees deduplication on the same `id`, achieving at-least-once idempotent writes |
| `PARTITION BY` | `date_trunc('day', created_at)` | Expression partition, auto-created by day. `created_at` as the second PK column also enables partition pruning |
| `DISTRIBUTED BY` | `HASH(id)` | PK table requires hash distribution; `id` is UUID ensuring even distribution |
| `partition_live_number` | `7` | Default retention of 7 days, consistent with Redshift's 7-day system table log rotation |
| `replication_num` | `3` | Default 3 replicas for data reliability (not a concern in shared-data mode) |

### 4.4 Configuration Design

#### 4.4.1 Import-Level Configuration

Control rejected records behavior through import properties:

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

#### 4.4.2 Session Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `log_rejected_record_num` | `0` | `0` = disabled; `-1` = unlimited, record all rejected rows; positive number N = record at most N rows. **Reuses existing parameter** with extended semantics: previously only controlled BE local file record count, now also controls writing to the system table. |

```sql
SET log_rejected_record_num = -1;  -- Record all rejected rows to system table
INSERT INTO my_table SELECT * FROM source_table;
```

#### 4.4.3 Global Configuration

FE configuration (`fe.conf`):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `rejected_record_ttl_days` | `7` | Default partition retention days for system table (`partition_live_number`) |

BE configuration (`be.conf`):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `rejected_record_sync_interval_sec` | `30` | `RejectedRecordSyncDaemon` background sync interval |
| `rejected_record_local_retention_hours` | `24` | Maximum local file retention time (fallback for sync failures) |

### 4.5 SQL Interface Design

`_statistics_._rejected_records` resides in an internal database. Users query and operate via standard SQL, with Row Access Policy automatically filtering visible rows.

#### 4.5.1 Querying

```sql
-- View rejected records for a target table (regular users auto-filtered by permissions)
SELECT * FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND error_code = 'TYPE_MISMATCH'
ORDER BY created_at DESC
LIMIT 100;

-- Aggregate error distribution for a specific database table
SELECT error_code, COUNT(*) as cnt, MAX(created_at) as latest
FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
GROUP BY error_code
ORDER BY cnt DESC;

-- View all rejected records for a specific import
SELECT * FROM _statistics_._rejected_records
WHERE load_label = 'load_orders_20260327';

-- Admin global overview (admin has no row filtering)
SELECT target_database, target_table, error_code, COUNT(*) as cnt
FROM _statistics_._rejected_records
WHERE created_at >= current_date() - INTERVAL 1 DAY
GROUP BY target_database, target_table, error_code
ORDER BY cnt DESC;
```

#### 4.5.2 Re-import

```sql
-- Re-import from rejected records to target table (after fixing schema)
-- raw_record is already in JSON format {col: val, ...}, column values can be directly extracted
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

#### 4.5.3 Management Operations

```sql
-- Adjust data retention days (default 7 days)
ALTER TABLE _statistics_._rejected_records SET ("partition_live_number" = "14");

-- Manual cleanup (requires admin privileges)
TRUNCATE TABLE _statistics_._rejected_records;

-- Clean up historical data for a specific target table
DELETE FROM _statistics_._rejected_records
WHERE target_database = 'db1'
  AND target_table = 'orders'
  AND created_at < '2026-03-01';
```

### 4.6 Error Classification

Define a standardized error code system:

| Error Code | Category | Description | Retryable |
|------------|----------|-------------|-----------|
| `PARSE_ERROR` | Format error | CSV/JSON/Avro parse failure | No (data fix required) |
| `TYPE_MISMATCH` | Type error | Field value does not match target type | No (data or schema fix required) |
| `NULL_VIOLATION` | Constraint violation | NOT NULL column encountered a NULL value | No (data or schema fix required) |
| `VALUE_OUT_OF_RANGE` | Value range error | Numeric overflow or out of range | No (data fix required) |
| `COLUMN_MISMATCH` | Structure error | Column count mismatch | No (data format fix required) |
| `PARTITION_NOT_FOUND` | Partition error | No matching partition found | Possibly (retryable after partition creation) |
| `ENCODING_ERROR` | Encoding error | Character encoding issue | No (data fix required) |
| `TRANSFORM_ERROR` | Transform error | Column mapping/expression evaluation failure | No (transform logic fix required) |
| `UNIQUE_VIOLATION` | Unique constraint | Primary key conflict (specific scenarios) | Possibly |
| `INTERNAL_ERROR` | System error | BE internal error | Yes (retryable) |

### 4.7 Relationship with Existing Mechanisms

**The `_rejected_records` system table completely replaces the existing rejected record file mechanism**. The error log is retained as a lightweight debugging tool (50-row cap, for ErrorURL).

```
                     log_rejected_record_num
                                │
                    ┌───────────┼───────────┐
                    │ OFF       │           │ ON
                    ▼           │           ▼
           ┌───────────────┐   │   ┌───────────────┐
           │ Existing       │   │   │ New behavior   │
           │ behavior       │   │   │                │
           │ error_log      │   │   │ error_log      │ ← Retained (50-row debug)
           │ rejected_record│   │   │ System table   │ ← Replaces rejected_record file
           │ max_filter_ratio│  │   │ max_filter_ratio│ ← Unchanged
           └───────────────┘   │   └───────────────┘
```

**Replacement relationship**:

| Dimension | rejected_record file (deprecated) | `_rejected_records` system table (replacement) |
|-----------|----------------------------------|-----------------------------------------------|
| Storage | BE local file | OLAP table |
| Control | `log_rejected_record_num` (default 0) | `log_rejected_record_num` (extended semantics: 0=disabled, -1=unlimited, N=max N rows) |
| Data content | `record \t error_msg \t source` | Structured 14 columns (including UUID), `raw_record` unified JSON |
| Implementation entry point | `append_rejected_record_to_file()` | `RejectedRecordWriter::append_from_chunk()` / `append_from_slices()` / `append_raw()` |

The `log_rejected_record_num` parameter semantic extension is backward compatible: `0` = disabled (consistent with existing default behavior), positive number/`-1` = enable system table writing (new behavior). Long-term plan to deprecate `append_rejected_record_to_file()`.

## 5. Key Technical Design

### 5.1 BE-Side Rejected Record Writer Detailed Design

#### 5.1.1 Existing Rejected Record Rejection Point Analysis

Through code analysis, the current call sites of `append_rejected_record_to_file()` and `append_error_msg_to_file()` are distributed across two stages, **and the raw data available at each stage differs**:

**Stage One: Scanner (Parse/Type Conversion)**

| Format | Call Location | Existing `record` Parameter | Available Row Data |
|--------|--------------|----------------------------|--------------------|
| CSV | `csv_scanner.cpp` → `_report_rejected_record()` | `record.to_string()` — raw CSV text line | **Complete**: per-column raw text values → JSON |
| JSON | `json_scanner.cpp` → `_construct_row` failure | `row.raw_json()` — raw JSON object substring | **Complete**: raw JSON used directly |
| Avro | `avro_scanner.cpp` / `avro_reader.cpp` | `""` or `datum_to_json()` | **Complete**: datum JSON serialization |
| Parquet | `arrow_to_starrocks_converter.cpp` → `report_error_message()` | Column-level context only (`file=, column=, raw_data=`) | **Complete**: all column values for that row available in Chunk before filter → JSON |
| ORC | `orc_chunk_reader.cpp` → `report_error_message()` | `""` empty string | **Complete**: `_broker_load_filter` marks row, all column values for that row available in Chunk before filter → JSON |

**Stage Two: Sink (Constraint Validation/Partition/Length/Precision)**

| Call Location | `record` Parameter Content | Raw Data Availability |
|--------------|---------------------------|-----------------------|
| `tablet_sink.cpp` → `_validate_data()` | `chunk->rebuild_csv_row(j, ",")` — **CSV line rebuilt from output chunk column values** | **Rebuilt row** (not raw text; CSV concatenation of StarRocks internal column values) |
| `tablet_sink.cpp` → partition out of range | Same as above | Same as above |
| `file_scanner.cpp` → strict mode filter | `src->rebuild_csv_row(i, ",")` — rebuilt from source chunk | Same as above (includes `TODO(meegoo): support other file format` comment) |

**Key conclusions**:
- At the Sink stage, raw text for all formats (including CSV/JSON) is **no longer available**; only `rebuild_csv_row()` rebuilt rows can be obtained
- Parquet/ORC have **no concept of row-level raw data** at any stage
- Therefore, Rejected Record Writer's `raw_record` field cannot guarantee storing "original input line", but rather the **best row-level representation available at the current stage**

#### 5.1.2 Rejected Record Writer Interface Design

`RejectedRecordWriter` replaces `append_rejected_record_to_file()`, providing a unified record collection interface:

```cpp
// be/src/runtime/dlq_writer.h
class RejectedRecordWriter {
public:
    // Method 1: Build JSON from Chunk column values (Sink stage / already-parsed rows)
    void append_from_chunk(
        const Chunk& chunk,                // Chunk containing rejected row
        size_t row_index,                  // Rejected row number
        const std::vector<std::string>& col_names,  // Column name list
        const std::string& error_code,     // Error code enum
        const std::string& error_message,  // Error details
        const std::string& error_column,   // Error column name (optional)
        const std::string& source_info     // Source JSON (optional)
    );

    // Method 2: Build from raw per-column text values (CSV Scanner stage, Chunk incomplete but raw Slices available)
    void append_from_slices(
        const std::vector<Slice>& col_values,  // Raw text Slices for each column
        const std::vector<std::string>& col_names,
        const std::string& error_code,
        const std::string& error_message,
        const std::string& error_column,
        const std::string& source_info
    );

    // Method 3: Build from raw text (Scanner parse completely failed, cannot split columns)
    void append_raw(
        const std::string& raw_text,       // Raw text → stored as {"_raw": "..."}
        const std::string& error_code,
        const std::string& error_message,
        const std::string& error_column,
        const std::string& source_info
    );

    Status flush();

private:
    DLQRecordBuffer _buffer;
    // target_database, target_table, load_label etc. context obtained from RuntimeState
    std::string build_json_from_chunk(const Chunk& chunk, size_t row_index,
                                      const std::vector<std::string>& col_names);
};
```

**Call site refactoring** (using `tablet_sink.cpp` as an example):

```cpp
// Existing code (rejected_record):
if (state->enable_log_rejected_record()) {
    RuntimeStateHelper::append_rejected_record_to_file(
        state, chunk->rebuild_csv_row(j, ","), error_msg, "");
}

// After refactoring (rejected records):
if (state->enable_dlq()) {
    state->dlq_writer()->append_from_chunk(
        *chunk, j, _output_col_names,        // Chunk + row number + column names → JSON
        "NULL_VIOLATION",                     // error_code
        error_msg,                            // error_message
        desc->col_name(),                     // error_column
        ""                                    // source_info
    );
}
```

**Using `csv_scanner.cpp` type conversion failure as an example** (per-column Slices available but Chunk incomplete):

```cpp
// Existing code:
_report_rejected_record(record, error_msg);  // record.to_string()

// After refactoring: Build JSON from per-column raw Slices
state->dlq_writer()->append_from_slices(
    _get_column_slices(row),                 // Raw text Slices for each column
    _src_col_names,                          // Column name list
    "TYPE_MISMATCH",
    error_msg,
    slot->col_name(),
    "{\"file\":\"" + _curr_reader->filename() + "\"}"
);
```

**Using CSV column count mismatch as an example** (cannot split columns, only raw line text available):

```cpp
// After refactoring: Raw text fallback
state->dlq_writer()->append_raw(
    record.to_string(),                      // → {"_raw": "..."}
    "COLUMN_MISMATCH",
    error_msg,
    "",
    "{\"file\":\"" + _curr_reader->filename() + "\"}"
);
```

#### 5.1.3 Specific Content and Re-import Method for `raw_record`

`raw_record` is uniformly output as JSON, but the construction source varies depending on the **rejection stage** and **input format**. The following explains each stage of the import pipeline.

##### Stage A: Scanner Parse/Type Conversion (Raw Data May Be Available)

This stage occurs during the Scanner's process of parsing raw input into StarRocks internal Chunks. **Key distinction**: text formats (CSV/JSON) have raw line text available; columnar formats (Parquet/ORC) do not.

**A1. CSV — Column Count Mismatch / Illegal UTF-8 / Type Conversion Failure**

In existing code, the CSV Scanner parses line by line in the `_parse_csv()` loop. Each line is a `CSVReader::Record` whose `to_string()` returns the raw CSV text line. When `read_string_for_adaptive_null_column()` conversion fails for a column, the entire row is rejected, **and the raw CSV line is available at this point**.

```
Raw CSV line:     10001,Alice,not_a_number,2026-03-27
                      ↓
                 Parse failure at column 3 amount (INT type received "not_a_number")
                      ↓
raw_record build: Directly from record.to_string() + column name mapping → JSON
```

```json
// raw_record content
{"order_id": "10001", "customer_name": "Alice", "amount": "not_a_number", "created_at": "2026-03-27"}
```

Construction method: During parsing, the CSV Scanner knows the raw text value of each column (`row.columns[j]` is the raw Slice). All column raw text values are assembled into JSON by column name. Note: values here are **raw text strings** (no type conversion), since the conversion itself is the failure cause.

Re-import:
```sql
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    raw_record->>'customer_name',
    CAST(raw_record->>'amount' AS DECIMAL(10,2)),  -- Fix data or use wider type
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'TYPE_MISMATCH';
```

**A2. JSON — Field Construction Failure**

The JSON Scanner uses simdjson to parse each JSON object; `raw_json()` returns the raw JSON substring. When `_construct_row()` fails, **the raw JSON object is available**.

```
Raw JSON:       {"order_id": 10001, "amount": "abc", "ts": "2026-03-27"}
                      ↓
                 _construct_row failure (amount type conversion)
                      ↓
raw_record build: Directly use raw_json()'s raw JSON object
```

```json
// raw_record content — the raw JSON object itself
{"order_id": 10001, "amount": "abc", "ts": "2026-03-27"}
```

Construction method: `raw_json()` returns JSON directly, no conversion needed.

Re-import: Same SQL pattern as CSV.

**A3. Parquet / ORC — Filter Rejection After Column Read**

Parquet/ORC are columnar formats; there is no concept of "raw line text". However, after StarRocks reads them, they are assembled into Chunks (row batches), and all column values for each row are **fully present** in the Chunk.

Key flow (using ORC as an example; Parquet is similar):

```
ORC file ──→ OrcChunkReader::_fill_chunk()
              │
              ├── Read column by column, write to Chunk
              ├── Some row/column violates constraint (e.g., NOT NULL column has null)
              │     → _broker_load_filter[row] = 0  (mark that row)
              │     → report_error_message(error_msg) (only writes error log)
              │
              ├── After all columns are read, all column values for that row in Chunk are complete
              │
              └── chunk->filter(*_broker_load_filter)  ← bad rows removed here
                                    ↑
                                    │
                        Rejected Record Writer needs to intervene before filter
```

**Key insight**: Although ORC/Parquet's `report_error_message()` only writes to the error log (passing empty string/column-level info) without calling `append_rejected_record_to_file()`, before `chunk->filter()` executes, **all column values for rows marked as 0 in `_broker_load_filter` are fully available** in the Chunk.

```
Intervention point:  Before chunk->filter(*_broker_load_filter)
              Iterate through rows with value 0 in the filter
              Extract all column values for these rows from Chunk → JSON
```

```json
// raw_record content — built from complete Chunk column values (before filter)
{"order_id": 10001, "customer_name": "Alice", "amount": null, "created_at": "2026-03-27"}
```

Construction method: After `_fill_chunk()` returns but before `chunk->filter()` executes, iterate through `_broker_load_filter` to find rejected row numbers, and call `build_json_record()` to build JSON from Chunk column values.

Re-import:
```sql
-- Rejected Parquet rows, raw_record has complete column values, can directly replay
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    raw_record->>'customer_name',
    CAST(raw_record->'amount' AS DECIMAL(10,2)),  -- Fix data or use wider type
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'NULL_VIOLATION';
```

**Parquet's Arrow conversion path**: Similar to ORC, `ArrowConvertContext::report_error_message()` currently only records column-level context to the error log. Likewise, complete rows need to be extracted from the Chunk before filtering.

**A4. Avro — Datum Conversion Failure**

The Avro Scanner converts Avro datums to JSON (`AvroUtils::datum_to_json()`), then constructs the Chunk.

```json
// raw_record content — JSON serialization of Avro datum
{"order_id": 10001, "amount": "\u0000\u0001"}
```

Construction method: `datum_to_json()` already returns JSON, used directly.

##### Stage B: Expr/Strict Mode (Chunk Column Values Available, Raw Text Unavailable)

This stage occurs in `file_scanner.cpp`'s `materialize()`, where strict mode checks column value conversions. Data is already in the source Chunk.

```
source Chunk:    [10001, "Alice", null (conversion failed), "2026-03-27"]
                      ↓
                 strict mode finds amount original value non-null but converted to null → reject
                      ↓
raw_record build: Build JSON from source Chunk column values
```

```json
// raw_record content
{"order_id": 10001, "customer_name": "Alice", "amount": null, "created_at": "2026-03-27"}
```

Construction method: Call `build_json_record()` to iterate source Chunk column values and build JSON. Existing code uses `rebuild_csv_row(i, ",")`, changed to `build_json_record()`.

Re-import: Same as above.

##### Stage C: Sink Constraint Validation (Output Chunk Column Values Available, Raw Text Unavailable)

This stage occurs in `tablet_sink.cpp`'s `_validate_data()`, performing final validation on the output Chunk. Data has already been through all expression calculations and type conversions. **Regardless of whether the original input format was CSV/JSON/Parquet/ORC, at the Sink stage the Chunk column values are completely identical.**

```
output Chunk:    [10001, "Alice_very_long_name_exceeds_64_chars...", 99.99, "2026-03-27"]
                      ↓
                 _validate_data: customer_name VARCHAR(64) exceeds length → reject
                      ↓
raw_record build: Build JSON from output Chunk column values
```

```json
// raw_record content — unified across all formats at Sink stage
{"order_id": 10001, "customer_name": "Alice_very_long_name_exceeds_64_chars...", "amount": 99.99, "created_at": "2026-03-27"}
```

Various Sink rejection scenarios:

| Rejection Reason | `error_code` | `raw_record` Example |
|-----------------|-------------|---------------------|
| NOT NULL column is null | `NULL_VIOLATION` | `{"id": 1, "name": null, "age": 25}` |
| VARCHAR exceeds length | `VALUE_OUT_OF_RANGE` | `{"id": 1, "name": "very long string...(200 chars)", "age": 25}` |
| Decimal overflow | `VALUE_OUT_OF_RANGE` | `{"id": 1, "price": 99999999999.99, "qty": 1}` |
| Partition not found | `PARTITION_NOT_FOUND` | `{"id": 1, "dt": "2099-01-01", "val": 42}` |

Construction method: Call `build_json_record()` to iterate output Chunk column values and build JSON.

Re-import:
```sql
-- Re-import after fixing VARCHAR length issue
INSERT INTO orders (order_id, customer_name, amount, created_at)
SELECT
    CAST(raw_record->'order_id' AS INT),
    LEFT(raw_record->>'customer_name', 64),     -- Truncate to legal length
    CAST(raw_record->'amount' AS DECIMAL(10,2)),
    raw_record->>'created_at'
FROM _statistics_._rejected_records
WHERE target_table = 'orders' AND error_code = 'VALUE_OUT_OF_RANGE';
```

##### Stage Summary

| Stage | Format | raw_record Source | Data Completeness | Replayable |
|-------|--------|-------------------|-------------------|------------|
| A (Scanner) | CSV | Raw line `record.to_string()` + per-column `row.columns[j]` Slices → JSON | **Complete**: raw text values for all columns available (including failed column's raw text) | **Yes** |
| A (Scanner) | JSON | `raw_json()` raw JSON substring | **Complete**: the raw JSON object itself | **Yes** |
| A (Scanner) | Parquet/ORC | Chunk column values before filter → JSON | **Complete**: `_fill_chunk()` reads all columns before filter; failed column values may be null | **Yes** (null columns need user fix) |
| A (Scanner) | Avro | `datum_to_json()` | **Complete**: JSON serialization of Avro datum | **Yes** |
| B (Strict) | All | source Chunk column values → JSON | **Complete**: all columns successfully parsed into Chunk | **Yes** |
| C (Sink) | All | output Chunk column values → JSON | **Complete**: all columns through expression calculation, format-independent | **Yes** |

**Key code-level details**:

- **CSV Scanner Stage A**: When type conversion fails at column j (`csv_scanner.cpp:442`), columns 0 through j-1 have been appended to the Chunk, but columns j through N have not yet been appended — Chunk column values for this row are **incomplete**. However, `record` (raw CSV line) and `row.columns` (per-column raw Slices) are still fully available; JSON is built directly from raw Slices, **not depending on the Chunk**.
- **Parquet/ORC Scanner Stage A**: `_fill_chunk()` first reads **all columns completely** into the Chunk, then marks and filters with `_broker_load_filter`. Intervention occurs before `chunk->filter()`. Failed columns (e.g., NOT NULL violation) have null values in the Chunk; other columns are complete.
- **Stages B/C**: Data is fully present in the Chunk, unified across all formats, no differences.

**Conclusion: All formats and all stages can construct equivalent complete JSON records for replay.** CSV/JSON at the Scanner stage are built from raw text (highest fidelity), Parquet/ORC at the Scanner stage are built from Chunk column values (failed columns are null), and at the Sink stage all formats are uniformly built from the Chunk.

#### 5.1.4 Write Path Design

##### Write Approach: BE Local File Cache + Background Batch Sync

Rejected records are first written to BE local files (same path as existing rejected records), and a background daemon thread periodically batch-syncs them to the `_statistics_._rejected_records` table.

```
┌──────────────────────────────────────────────────────────────────────┐
│                Import Path (synchronous, zero transaction overhead)  │
│                                                                      │
│  Scanner ──→ [Expr/Strict] ──→ OlapTableSink (main table)           │
│     │              │                 │                                │
│     │(rejected)    │(rejected)       │(rejected)                     │
│     ▼              ▼                 ▼                                │
│  ┌──────────────────────────────────────────────────┐               │
│  │ RejectedRecordWriter::append_from_chunk/slices/raw()        │               │
│  │   → Build JSON record                            │               │
│  │   → Append to BE local rejected record file      │               │
│  │     (same path as existing                       │               │
│  │      append_rejected_record_to_file)             │               │
│  └──────────────────────────────────────────────────┘               │
│                                                                      │
│  After main import completes, file path reported to FE               │
│  (via TReportExecStatusParams)                                       │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                Background Sync (async, batch transaction)            │
│                                                                      │
│  BE RejectedRecordSyncDaemon (background thread, similar to         │
│  load_error_log cleanup thread):                                     │
│                                                                      │
│    Every N seconds / accumulated M records:                          │
│      1. Scan local rejected record file directory                    │
│      2. Read multiple files → merge to construct Chunk               │
│      3. Batch write via internal Stream Load                         │
│         _statistics_._rejected_records                               │
│      4. Write success → delete local files                           │
│      5. Write failure → retain files, retry next time                │
│                                                                      │
│  Batching effect:                                                    │
│    100 small Stream Loads each producing 1 bad row                   │
│    → 100 local files                                                 │
│    → 1 batch Stream Load transaction writing 100 rejected records    │
└──────────────────────────────────────────────────────────────────────┘
```

##### Local File Format

Rejected record local files use JSON Lines format (one JSON object per line), file paths reuse existing `LoadPathMgr` management:

```
<storage_path>/dlq/<db_name>/<label>_<fragment_id>.jsonl
```

Each line content:
```json
{"id":"550e8400-e29b-41d4-a716-446655440000","created_at":"2026-03-27 10:30:00","target_database":"db1","target_table":"orders","load_label":"load_001","load_type":"INSERT","txn_id":12345,"user_name":"root","error_code":"TYPE_MISMATCH","error_message":"Cannot cast 'abc' to INT","error_column":"amount","raw_record":{"order_id":"10001","customer_name":"Alice","amount":"abc"},"source_info":{"format":"parquet","file":"gs://bucket/data.parquet"},"backend_id":10001}
```

The `id` field is generated by BE's Rejected Record Writer during append (`generate_uuid_string()`), written to the local file. Background sync writes it as-is to the PK table, ensuring retry idempotency.

##### Background Sync Design

**BE-side — `RejectedRecordSyncDaemon`**:

```cpp
class RejectedRecordSyncDaemon : public Thread {
    void run() {
        while (!_stopped) {
            sleep(sync_interval_sec);  // default 30s
            scan_and_sync();
        }
    }

    void scan_and_sync() {
        auto files = scan_dlq_directory();
        if (files.empty()) return;

        auto chunk = read_and_merge(files);

        // Write via internal Stream Load + merge commit
        // merge commit merges Stream Loads from multiple BEs into the same transaction
        auto status = internal_stream_load(chunk, {
            .enable_merge_commit = true,
            .target_table = "_statistics_._rejected_records",
            .enable_dlq = false,  // prevent recursion
        });

        if (status.ok()) {
            delete_synced_files(files);
        }
        // Failure → retain files, retry next time
        // PK table + UUID guarantees retry idempotency: same id records won't duplicate
    }
};
```

**Key mechanism: merge commit + PK deduplication**

1. **merge commit**: `RejectedRecordSyncDaemon`'s Stream Load enables `enable_merge_commit`, causing FE to merge Stream Loads from multiple BEs within the same time window into a single transaction. This way N BEs syncing simultaneously ultimately produce only 1 transaction (not N).

2. **PK deduplication guarantees idempotency**: The system table is a Primary Key table; each record has a BE-generated UUID (`id` column). If a Stream Load needs to retry due to network timeout or similar reasons, records with the same `id` are automatically deduplicated by the PK table. This achieves **at-least-once delivery + PK deduplication = exactly-once** semantics.

```
BE-1 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┐
BE-2 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┤──→ FE merge commit ──→ 1 transaction
BE-3 RejectedRecordSyncDaemon ─── Stream Load (merge commit) ──┘
```

**Sync parameters**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `rejected_record_sync_interval_sec` | `30` | Background sync interval (seconds) |
| `rejected_record_sync_max_batch_rows` | `10000` | Maximum rows per sync batch |
| `rejected_record_local_retention_hours` | `24` | Maximum local file retention time (fallback for sync failures) |

**Transaction optimization effect**:

| Scenario | 1:1 txn approach | Local cache + merge commit |
|----------|-----------------|---------------------------|
| 100 Stream Loads, 1 bad row each, 3 BEs | 100 txn | **1 txn** (3 BEs merge commit) |
| Routine Load 100 tasks, 3 BEs | 100 txn | **1~5 txn** (merged by sync cycle) |
| 1 large Broker Load, 3 BEs each producing bad rows | 3 txn | **1 txn** |

##### Data Visibility Timeline

```
t=0    Import starts
t=5    Import completes, rejected record files written to BE local disk
       User queries _statistics_._rejected_records → no data yet
t=35   RejectedRecordSyncDaemon triggers sync, reads local files, batch writes via merge commit
       User queries _statistics_._rejected_records → data visible
```

With a default 30-second sync interval, users can query rejected records within at most 30 seconds after import completion. This is perfectly acceptable for the "inspect + replay" usage pattern.

#### 5.1.5 Core Design Points

1. **Three unified entry methods**: `append_from_chunk()` builds JSON from Chunk column values (Sink/Strict/Parquet/ORC stages), `append_from_slices()` builds JSON from raw text Slices (CSV Scanner stage), `append_raw()` falls back to wrapping raw text (`{"_raw": "..."}`, when parsing completely fails).
2. **Context injection**: `target_database`, `target_table`, `load_label`, `load_type`, `txn_id`, `user_name`, `backend_id` are obtained from `RuntimeState` in one go.
3. **Local file writing**: Rejected records are serialized as JSON Lines and written to BE local files. The write path is the same as existing `append_rejected_record_to_file()` — local append, zero transaction overhead, no impact on import latency.
4. **Background async sync**: The `RejectedRecordSyncDaemon` daemon thread on BE periodically scans local rejected record files, aggregates bad rows from multiple files/multiple import tasks, and batch-writes to `_statistics_._rejected_records` via a single Stream Load.
5. **Best-effort**: Local file write failures are silently skipped. Background sync failures retain files for next retry; files are cleaned up after `rejected_record_local_retention_hours` timeout.
6. **Lifecycle**: Rejected Record Writer is created/destroyed with the fragment; files are flushed when the fragment ends. `RejectedRecordSyncDaemon` is a BE-level resident daemon thread.

#### 5.1.6 Key Design Constraints

**Constraint 1: Zero transaction overhead on the import path**

Only local file append is performed on the import path — no transactions opened, no BRPC, no FE interaction. All transaction overhead is transferred to the background `RejectedRecordSyncDaemon`, fully decoupled from the main import.

**Constraint 2: Data visibility has second-level delay**

Data is not immediately visible after import completion; it requires waiting for the next sync cycle of `RejectedRecordSyncDaemon` (default 30 seconds). This is perfectly acceptable for the "inspect + replay after import completion" usage pattern.

**Constraint 3: Recursion prevention**

When `RejectedRecordSyncDaemon` writes to `_statistics_._rejected_records` via internal Stream Load, the rejected records feature is disabled for that write (RuntimeState checks the target table name).

**Constraint 4: Distinguishing Parquet/ORC column read failure vs. row-level constraint violation**

Rows marked by ORC/Parquet's `_broker_load_filter` are **row-level constraint violations** (e.g., NOT NULL column has null); all column values for such rows are complete in the Chunk. However, if it is a **column read failure itself** (e.g., file corruption causing a column's `get_next()` to return Error), the entire batch fails, and no per-row rejected records are produced — this is an import-level failure, not a row-level rejection.

### 5.2 FE-Side Management

```
┌─────────────────────────────────────────────────────┐
│                   FE                                 │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ RejectedRecordManager                         │  │
│  │                                                │  │
│  │ - Ensures _statistics_._rejected_records       │  │
│  │   exists at FE startup (similar to             │  │
│  │   information_schema initialization)           │  │
│  │ - Injects config params into TQueryOptions     │  │
│  │ - Injects Row Access Policy row filtering      │  │
│  │   at query time                                │  │
│  │ - Aggregates statistics                        │  │
│  └───────────────────────────────────────────────┘  │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │ Import Flow Integration                        │  │
│  │                                                │  │
│  │ StreamLoadPlanner / RoutineLoadJob / BulkLoad: │  │
│  │   - Check log_rejected_record_num > 0          │  │
│  │   - Inject params into TQueryOptions           │  │
│  │   - Report statistics                          │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**Table creation timing**: `_statistics_._rejected_records` is asynchronously initialized by `RejectedRecordManager` (FrontendDaemon) after the leader node starts up, following the same pattern as `StatisticsMetaManager` creating statistics tables. `RejectedRecordManager` checks whether the `_rejected_records` table already exists in the `_statistics_` database, and creates it via `CreateTableStmt` if it does not.

### 5.3 Thrift Interface Extension

```thrift
// InternalService.thrift
struct TQueryOptions {
    // ... existing fields ...

    // Rejected records configuration
    optional i64 log_rejected_record_num;              // 0=disabled, -1=unlimited, N=max N records
}

// FrontendService.thrift
struct TReportExecStatusParams {
    // ... existing fields ...

    optional i64 rejected_records_written;             // BE reports number of records written to file
    optional string rejected_record_file_path;       // BE local file path
}
```

The Thrift interface is very lightweight: FE only needs to set `log_rejected_record_num` in `TQueryOptions`; it does not need to push the system table's tablet information (background sync is initiated by BE itself via Stream Load).

### 5.4 Table Lifecycle

```
FE startup
  │
  ▼
RejectedRecordManager initializes _statistics_._rejected_records
(expression partition, partition_live_number = 7)
  │
  ├──→ Normal operation: partitions auto-created by day, expired partitions auto-deleted
  │
  ├──→ ALTER TABLE ... SET ("partition_live_number" = "14")
  │         → Adjust retention policy
  │
  └──→ TRUNCATE TABLE / DELETE FROM ...
           → Manual cleanup (requires admin privileges)
```

`_rejected_records` is global infrastructure; its lifecycle is consistent with the cluster. Deletion of any database or table does not affect the existence of the system table or its existing data (historical bad data remains queryable).

## 6. Metrics Design

### 6.1 BE Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `starrocks_be_rejected_record_total` | Counter | Cumulative total rejected records written |
| `starrocks_be_rejected_record_failed` | Counter | Number of records that failed to write |
| `starrocks_be_rejected_record_sync_total` | Counter | Cumulative records written to system table by background sync |
| `starrocks_be_rejected_record_sync_failed` | Counter | Number of background sync failures |

### 6.2 FE Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `starrocks_fe_rejected_record_total` | Counter | Total rejected records written |

## 7. Performance Considerations

### 7.1 Zero Overhead on Happy Path

When `log_rejected_record_num = 0`, there should be no additional overhead. Implementation approach:
- Compile time: via conditional compilation or constant folding
- Runtime: add `_enable_dlq` flag in `RuntimeState`, with a fast check at the append path entry point

### 7.2 Bad Data Path Performance

Only local file append is performed on the import path, adding no extra latency. The background `RejectedRecordSyncDaemon` batch-writes to the system table via merge commit, with extremely low transaction overhead.

### 7.3 Storage Cost Estimation

Assuming an average of 2KB per rejected record (including raw data and metadata):
- 10,000 records/day → ~20MB/day → ~140MB/week
- 1,000,000 records/day → ~2GB/day → ~14GB/week
- 100,000,000 records/day → ~200GB/day → sampling or limiting should be considered

It is recommended to limit per-import record count via `log_rejected_record_num` to avoid storage explosion in extreme scenarios.

## 8. Permission Control Design

### 8.1 Problem and Approach Selection

A global single table means bad data from all databases and all tables is mixed together. It must be guaranteed that users can only see rows corresponding to `target_database.target_table` for which they have SELECT permission.

Industry approaches: Redshift uses automatic `userid` row filtering (insufficient granularity — filtering is by importer, not target table); ClickHouse only does table-level GRANT (no row-level isolation).

StarRocks adopts **automatic Row Access Policy**: FE automatically injects row filtering predicates when querying `_rejected_records`, reusing existing `SecurityPolicyRewriteRule` + `Authorizer` infrastructure.

### 8.2 Implementation

```sql
-- User's original query:
SELECT * FROM _statistics_._rejected_records
WHERE error_code = 'TYPE_MISMATCH';

-- After FE Analyzer rewrite (conceptual illustration):
SELECT * FROM _statistics_._rejected_records dlq
WHERE error_code = 'TYPE_MISMATCH'
  AND EXISTS (
    SELECT 1 FROM <internal privilege view> p
    WHERE p.database_name = dlq.target_database
      AND p.table_name = dlq.target_table
      AND p.user_name = current_user()
      AND p.privilege = 'SELECT'
  );
```

| User Role | Behavior |
|-----------|----------|
| Admin / root | No filtering injected, all rows visible |
| Regular user | EXISTS subquery filter automatically injected |
| User with no SELECT permission on any table | Query returns empty result set |

Specific implementation can inline permission checks as predicate functions in `QueryAnalyzer` (e.g., `has_table_privilege(current_user(), target_database, target_table, 'SELECT')`), avoiding materializing a permission table.

### 8.3 Write and Management Permissions

| Operation | Permission Required |
|-----------|-------------------|
| Data writing | Internal system (BE RejectedRecordWriter), users have no direct INSERT permission |
| SELECT query | Automatic Row Access Policy filtering |
| DELETE cleanup | Requires ADMIN privileges |
| TRUNCATE | Requires ADMIN privileges |
| ALTER TABLE (adjust TTL) | Requires ADMIN privileges |

### 8.4 Other Security Considerations

- **Data sensitivity**: The `raw_record` column contains raw data, which may include sensitive information. If masking is needed, a Column Masking Policy can be applied to this column (StarRocks already supports `Authorizer.getColumnMaskingPolicy()`).
- **Auditing**: Queries, DELETE, and TRUNCATE operations on the system table should be recorded in audit logs.

## 9. Compatibility

- **Backward compatible**: `log_rejected_record_num` defaults to 0 (disabled), not affecting existing behavior.
- **Upgrade path**: After upgrade, users can optionally enable the feature.
- **Downgrade**: Upon downgrade, the `_rejected_records` table will remain as a regular OLAP table, but automatic writing will cease.

## 10. Comparison with Existing Reject Record Mechanism

| Dimension | Existing Reject Record | `_rejected_records` System Table (New Approach) |
|-----------|----------------------|------------------------------------------------|
| Storage location | BE local file system | StarRocks global OLAP table `_statistics_._rejected_records` |
| Persistence | Temporary files, depends on BE node | Persistent, available across nodes |
| Queryability | Requires SSH to BE node | Standard SQL query |
| Permission control | None | Automatic Row Access Policy (filtered by target_table permission) |
| Data completeness | Limited by `log_rejected_record_num` | Configurable, records all by default. PK + UUID guarantees exactly-once |
| Error classification | No structured classification | Standardized error code system |
| Reprocessing capability | None | `INSERT INTO target SELECT ... FROM _statistics_._rejected_records` |
| Lifecycle | Depends on BE cleanup policy | Expression partition + `partition_live_number` auto-expiry (default 7 days) |
| Observability | No dedicated metrics | Complete metrics system |
| Performance impact | Local file write, low | Involves table writes, requires async and batch optimization |
| External dependencies | None | None (pure StarRocks built-in) |
| Applicable scenarios | Debugging | Production data quality assurance |

## 11. Requirements Coverage Analysis

Item-by-item verification of original requirements coverage by the current design:

| Requirement | Current Design Coverage | Implementation |
|-------------|------------------------|----------------|
| Rejected rows written to a queryable table | ✅ | `_statistics_._rejected_records` global OLAP table, SQL queryable |
| Support 10,000+ rows | ✅ | `log_rejected_record_num` configurable, default -1 (unlimited) |
| Structured error information | ✅ | `error_code`, `error_message`, `error_column` and other structured columns |
| Replayable re-import | ✅ | `raw_record` is JSON format, `INSERT INTO target SELECT raw_record->>'col' FROM _statistics_._rejected_records` |
| Parquet scenario support | ✅ | Section 5.1.3 details Parquet's `raw_record` construction at each stage (Chunk column values → JSON) |
| INSERT INTO ... SELECT ... FROM FILES() | ✅ | INSERT statements enable via session variable `log_rejected_record_num` |
| max_filter_ratio cooperation | ✅ | When enabled, rows filtered by `max_filter_ratio` are automatically written to the system table; filtering behavior unchanged |
| Vertica-like user experience | ✅ | Vertica's `REJECTED DATA AS TABLE` corresponds to this design's global system table + SQL query + replay |

### 11.3 AppLovin End-to-End Usage Flow

```sql
-- 1. Enable rejected records
SET log_rejected_record_num = -1;    -- Record all rejected rows
SET insert_max_filter_ratio = 0.01;  -- Allow 1% filtering

-- 2. Bulk import (GCS Parquet → aggregate table)
INSERT INTO agg_table
SELECT * FROM FILES(
    "path" = "gs://bucket/data/*.parquet",
    "format" = "parquet",
    "gs.credential.json" = '...'
);
-- Even with bad rows, as long as they don't exceed 1%, the import succeeds

-- 3. Inspect rejected rows
SELECT error_code, error_column, error_message, raw_record
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND load_label = '<label of this import>'
ORDER BY created_at;

-- 4. Analyze error patterns
SELECT error_code, error_column, COUNT(*) as cnt
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND created_at > '2026-03-27'
GROUP BY error_code, error_column
ORDER BY cnt DESC;

-- 5. Replay after fix (e.g., after fixing upstream Parquet generation logic)
INSERT INTO agg_table (col1, col2, col3)
SELECT
    CAST(raw_record->'col1' AS INT),
    raw_record->>'col2',
    CAST(raw_record->'col3' AS DECIMAL(10,2))
FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND error_code = 'TYPE_MISMATCH'
  AND created_at > '2026-03-27';

-- 6. Clean up processed data
DELETE FROM _statistics_._rejected_records
WHERE target_table = 'agg_table'
  AND created_at > '2026-03-27';
```


## 12. Milestone Plan

### Phase 1: Foundation Framework

- [ ] `_statistics_._rejected_records` table initialization (FE RejectedRecordManager)
- [ ] Automatic Row Access Policy implementation
- [ ] Rejected Record Writer implementation (BE: async batch write)
- [ ] Stream Load support
- [ ] INSERT INTO support
- [ ] Basic metrics
- [ ] Import-level configuration + Session variables

### Phase 2: Full Import Support + Operations

- [ ] Broker Load support
- [ ] Routine Load support
- [ ] Full metrics and monitoring integration
- [ ] Documentation and user guide

### Phase 3: Advanced Features

- [ ] Data visualization (Web UI integration)
