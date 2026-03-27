# Streaming Data Ingestion from Kafka: Cross-System Comparison

> Research summary comparing how ClickHouse, Databricks, Snowflake, and Apache Flink SQL
> handle streaming ingestion from Kafka and similar message queues.

---

## 1. ClickHouse — Kafka Table Engine

### SQL Syntax

ClickHouse uses a dedicated `Kafka` table engine. The canonical pattern is a **three-table architecture**:
a Kafka engine table (virtual stream), a MergeTree destination table, and a materialized view
that pipes data between them.

```sql
-- 1. Kafka engine table (virtual consumer)
CREATE TABLE kafka_source (
    user_id    UInt64,
    event_type String,
    ts         DateTime
) ENGINE = Kafka
SETTINGS
    kafka_broker_list       = 'broker1:9092,broker2:9092',
    kafka_topic_list        = 'user_events',
    kafka_group_name        = 'ch_consumer_group',
    kafka_format            = 'JSONEachRow',
    kafka_num_consumers     = 4,
    kafka_thread_per_consumer = 1;

-- 2. Destination table
CREATE TABLE events (
    user_id    UInt64,
    event_type String,
    ts         DateTime
) ENGINE = MergeTree()
ORDER BY (ts, user_id);

-- 3. Materialized view that bridges them
CREATE MATERIALIZED VIEW kafka_to_events TO events AS
SELECT * FROM kafka_source;
```

Required settings: `kafka_broker_list`, `kafka_topic_list`, `kafka_group_name`, `kafka_format`.

### Parallelism Model

| Setting | Purpose |
|---------|---------|
| `kafka_num_consumers` | Number of consumer threads per table. Capped at `min(physical_cores, topic_partitions)`. |
| `kafka_thread_per_consumer` | When `1`, each consumer flushes independently in parallel. When `0` (default), rows are squashed into a single block. |
| `background_message_broker_schedule_pool_size` | Server-level thread pool for all Kafka engine tables (default 16; increase if total consumers exceed 16). |

**Dynamic scaling is NOT supported.** Changing `kafka_num_consumers` requires `DROP` + re-`CREATE` of the
Kafka table. In Kubernetes, container CPU detection may artificially limit consumers; the
`kafka_disable_num_consumers_limit` setting overrides this.

### Error Handling

| Mode | Behavior |
|------|----------|
| `kafka_skip_broken_messages = N` | Silently skip up to N unparseable messages per block. |
| `kafka_handle_error_mode = 'stream'` | Route broken messages to virtual columns `_error` and `_raw_message`; capture via a second materialized view into an error table. |
| `kafka_handle_error_mode = 'dead_letter_queue'` | Route errors to `system.dead_letter_queue` (v25.8+). |

### Exactly-Once Semantics

ClickHouse provides **at-least-once** delivery by default. Offsets are committed after the
materialized view insert completes. An **experimental** feature stores committed offsets in
ClickHouse Keeper (`kafka_keeper_path` / `kafka_replica_name`), enabling deduplication and
closer-to-exactly-once behavior, but it is not production-ready.

### Monitoring & Management

```sql
-- View consumer state, offsets, exceptions
SELECT * FROM system.kafka_consumers;

-- Pause/resume consumption
DETACH TABLE kafka_to_events;   -- pause
ATTACH TABLE kafka_to_events;   -- resume
```

The `system.kafka_consumers` table exposes: `num_messages_read`, `num_commits`,
`last_commit_time`, `last_poll_time`, rebalance counts, last 10 exceptions, current offsets,
and raw `rdkafka_stat` JSON.

---

## 2. Databricks (Apache Spark Structured Streaming)

### SQL Syntax

Databricks offers the `read_kafka` table-valued function (Runtime 13.3 LTS+) and
`CREATE STREAMING TABLE` for declarative pipelines.

```sql
-- Batch read (ad-hoc)
SELECT value::string AS value
FROM read_kafka(
    bootstrapServers => 'kafka:9092',
    subscribe        => 'events'
) LIMIT 100;

-- Streaming table (Lakeflow Declarative Pipelines / Databricks SQL)
CREATE OR REFRESH STREAMING TABLE catalog.schema.raw_events AS
SELECT
    value::string:event_type AS event_type,
    to_timestamp(value::string:ts) AS ts
FROM STREAM read_kafka(
    bootstrapServers => 'kafka:9092',
    subscribe        => 'events'
);
```

Python DataFrame API (most common):

```python
df = (spark.readStream
      .format("kafka")
      .option("kafka.bootstrap.servers", "broker:9092")
      .option("subscribe", "events")
      .option("startingOffsets", "latest")
      .option("maxOffsetsPerTrigger", 100000)
      .load())

(df.selectExpr("CAST(value AS STRING)")
   .writeStream
   .format("delta")
   .option("checkpointLocation", "/checkpoints/events")
   .trigger(processingTime="10 seconds")
   .start("/tables/events"))
```

The Kafka source always returns columns: `key` (binary), `value` (binary), `topic`, `partition`,
`offset`, `timestamp`, `timestampType`, and optionally `headers`.

### Parallelism & Auto-Scaling

| Trigger Mode | Latency | Description |
|-------------|---------|-------------|
| `processingTime='10 seconds'` | Seconds | Micro-batch at fixed intervals. |
| `availableNow=True` | Minutes | Scheduled incremental batch. |
| `realTime='5 minutes'` | Sub-second (5 ms possible) | Long-running batch with streaming shuffle; requires Runtime 16.4 LTS+. |

Parallelism is determined by the number of Kafka partitions (each partition maps to a Spark task).
Rate limiting is controlled by `maxOffsetsPerTrigger`.

**Auto-scaling is explicitly NOT recommended** for standard Structured Streaming jobs.
Databricks recommends using **Lakeflow Spark Declarative Pipelines** (formerly Delta Live Tables)
with enhanced autoscaling for streaming workloads.

### Exactly-Once Semantics

Structured Streaming guarantees **exactly-once** end-to-end when writing to Delta Lake, using
the write-ahead log (WAL) / checkpoint mechanism. The `foreachBatch` sink provides only
**at-least-once** and requires idempotent writes for exactly-once behavior.

### Error Handling

- Schema enforcement via Delta Lake `mergeSchema` / `overwriteSchema` options.
- `failOnDataLoss` option (default `true`) fails the query if offsets are out of range.
- Dead-letter patterns are implemented manually via `foreachBatch` + try/catch logic.
- Bad records handling via `PERMISSIVE` / `DROPMALFORMED` / `FAILFAST` parse modes.

### Monitoring & Management

```python
# Programmatic monitoring
query.status        # current status
query.recentProgress  # recent micro-batch metrics
query.lastProgress    # last completed batch

# StreamingQueryListener for external systems
spark.streams.addListener(MyListener())
```

- Spark UI **Structured Streaming** tab shows input rate, processing rate, batch duration.
- Lakeflow Declarative Pipelines provides a dedicated monitoring dashboard.
- `DESCRIBE TABLE EXTENDED` and `SHOW TABLE EXTENDED` for streaming table metadata.

---

## 3. Snowflake — Snowpipe Streaming + Kafka Connector

### Architecture Variants

Snowflake has two generations of Kafka integration:

| Generation | Connector Version | Architecture | Max Throughput |
|-----------|------------------|--------------|----------------|
| **Classic** (Snowpipe Streaming) | 2.x / 3.x | Java Ingest SDK | Moderate |
| **High-Performance** (new) | 4.x (Public Preview) | Rust-based client core, PIPE objects | Up to 10 GB/s per table |

### SQL Syntax

Snowflake's streaming ingestion is **connector-driven**, not SQL-driven. The connector is
configured via a properties file, not SQL DDL. However, server-side objects are required:

```sql
-- Target table
CREATE TABLE events (
    record_metadata VARIANT,
    record_content  VARIANT
);

-- For high-performance architecture: CREATE PIPE with streaming source
CREATE OR REPLACE PIPE my_kafka_pipe AS
COPY INTO events
FROM (
    SELECT $1:event_type::STRING, $1:ts::TIMESTAMP
    FROM TABLE(DATA_SOURCE(TYPE => 'STREAMING'))
);

-- Grant permissions
GRANT USAGE ON PIPE my_kafka_pipe TO ROLE kafka_role;
```

Connector configuration (properties file, not SQL):

```properties
snowflake.ingestion.method=SNOWPIPE_STREAMING
snowflake.url.name=myaccount.snowflakecomputing.com
snowflake.user.name=kafka_user
snowflake.role.name=kafka_role
snowflake.private.key=<key>
buffer.flush.time=10
buffer.count.records=10000
buffer.size.bytes=20000000
enable.streaming.client.optimization=true
```

### Auto-Scaling

Snowpipe Streaming is a **serverless offering** — compute resources scale automatically based on
ingestion load. Users do not configure parallelism directly within Snowflake. Parallelism on the
Kafka Connect side is controlled by the number of Kafka Connect tasks (`tasks.max`) and partitions.

The `enable.streaming.client.optimization` property (default `true`) creates one client for
multiple topic partitions, reducing overhead. For high-throughput scenarios (>50 MB/s per connector),
disabling this may reduce latency.

### Exactly-Once Semantics

Exactly-once is the **default** guarantee. The connector uses:

- **Consumer offset** (managed by Kafka) — tracks consumption position.
- **Offset token** (managed by Snowflake) — tracks committed position in Snowflake.
- On channel open/reopen, the Snowflake offset token is the source of truth; Kafka consumer
  offset is reset accordingly.
- Sequential offset validation ensures no gaps.

### Error Handling

| Property | Behavior |
|----------|----------|
| `errors.tolerance=NONE` | Stop on first error (default). |
| `errors.tolerance=ALL` | Skip all errors and continue. |
| `errors.deadletterqueue.topic.name` | Route failed records to a Kafka DLQ topic. |
| `errors.log.enable=TRUE` | Write errors to Kafka Connect log. |

Limitation: with the high-perf connector, only non-convertible records go to DLQ; records that
fail Snowflake ingestion are not automatically routed there.

### Monitoring & Management

```sql
-- Pipe status (traditional Snowpipe)
SELECT SYSTEM$PIPE_STATUS('my_kafka_pipe');

-- Load history (last 14 days)
SELECT * FROM TABLE(INFORMATION_SCHEMA.COPY_HISTORY(
    TABLE_NAME => 'events',
    START_TIME => DATEADD(hours, -24, CURRENT_TIMESTAMP())
));

-- Snowpipe Streaming client history (account-level, up to 365 days)
SELECT * FROM SNOWFLAKE.ACCOUNT_USAGE.SNOWPIPE_STREAMING_CLIENT_HISTORY
WHERE EVENT_TIMESTAMP > DATEADD(hours, -24, CURRENT_TIMESTAMP());

-- Pause/resume pipe
ALTER PIPE my_kafka_pipe SET PIPE_EXECUTION_PAUSED = TRUE;
ALTER PIPE my_kafka_pipe SET PIPE_EXECUTION_PAUSED = FALSE;
```

---

## 4. Apache Flink SQL — Kafka Connector

### SQL Syntax

Flink uses `CREATE TABLE` with connector options. No separate "engine" or "pipe" concept — the
table itself is the streaming source or sink.

```sql
-- Source table
CREATE TABLE kafka_events (
    user_id    BIGINT,
    event_type STRING,
    ts         TIMESTAMP_LTZ(3) METADATA FROM 'timestamp',
    `partition` BIGINT METADATA VIRTUAL,
    `offset`    BIGINT METADATA VIRTUAL
) WITH (
    'connector'                     = 'kafka',
    'topic'                         = 'user_events',
    'properties.bootstrap.servers'  = 'broker1:9092,broker2:9092',
    'properties.group.id'           = 'flink_consumer',
    'scan.startup.mode'             = 'earliest-offset',
    'format'                        = 'json',
    'json.ignore-parse-errors'      = 'true',
    'scan.parallelism'              = '8'
);

-- Destination table (e.g., JDBC, filesystem, another Kafka topic)
CREATE TABLE events_sink (
    user_id    BIGINT,
    event_type STRING,
    ts         TIMESTAMP(3)
) WITH (
    'connector' = 'jdbc',
    'url'       = 'jdbc:mysql://host:3306/db',
    'table-name'= 'events'
);

-- Continuous streaming INSERT
INSERT INTO events_sink
SELECT user_id, event_type, ts
FROM kafka_events
WHERE event_type <> 'heartbeat';
```

Upsert-Kafka variant for changelog streams:

```sql
CREATE TABLE upsert_kafka_table (
    user_id    BIGINT,
    user_name  STRING,
    PRIMARY KEY (user_id) NOT ENFORCED
) WITH (
    'connector'                    = 'upsert-kafka',
    'topic'                        = 'user_updates',
    'properties.bootstrap.servers' = 'broker:9092',
    'key.format'                   = 'json',
    'value.format'                 = 'json',
    'scan.parallelism'             = '4',
    'sink.parallelism'             = '4'
);
```

### Parallelism Configuration

| Option | Scope | Description |
|--------|-------|-------------|
| `scan.parallelism` | Source | Parallelism of the Kafka source operator (added in kafka-connector 4.0.0 / Flink 2.0). Falls back to global default if unset. |
| `sink.parallelism` | Sink | Parallelism of the Kafka sink operator. |
| `SET 'parallelism.default' = '8';` | Session | Global default parallelism for all operators. |
| `SET 'pipeline.max-parallelism' = '128';` | Session | Upper bound for auto-scaling. |

Flink's parallelism at the source is typically `min(configured_parallelism, num_kafka_partitions)`.

### Dynamic Scaling

| Mode | Description |
|------|-------------|
| **Reactive Mode** (open-source) | Automatically adjusts parallelism to use all available TaskManager slots. Adding/removing TaskManagers rescales jobs. |
| **Flink Autopilot** (Confluent Cloud) | Automatically tunes parallelism up/down based on throughput and lag metrics. |
| **Manual rescaling** | Stop job with savepoint, change parallelism, resume. Or use REST API rescale on Confluent Platform. |

### Exactly-Once Semantics

Flink provides **exactly-once** end-to-end via Kafka transactions:

```sql
CREATE TABLE kafka_sink (...) WITH (
    'connector'                       = 'kafka',
    'sink.delivery-guarantee'         = 'exactly-once',
    'sink.transactional-id-prefix'    = 'my-app',
    'properties.transaction.timeout.ms' = '900000',
    ...
);
```

Options: `none`, `at-least-once` (default), `exactly-once`.

For exactly-once reads, Flink's checkpointing mechanism ensures that on failure, consumption
resumes from the last successfully checkpointed offset. Enable checkpointing:

```sql
SET 'execution.checkpointing.interval' = '60s';
SET 'execution.checkpointing.mode' = 'EXACTLY_ONCE';
```

### Error Handling

| Mechanism | Details |
|-----------|---------|
| `json.ignore-parse-errors = 'true'` | Skip malformed JSON records (returns nulls). |
| `csv.ignore-parse-errors = 'true'` | Similar for CSV format. |
| CDC formats (debezium, canal, maxwell) | Built-in changelog interpretation with schema validation. |
| `scan.topic-partition-discovery.interval` | Auto-discover new partitions (default 5 min). |

Flink does not have a built-in dead-letter queue; DLQ patterns are typically implemented
via side outputs in the DataStream API or by filtering error rows.

### Monitoring & Management

```sql
-- View running jobs (Flink SQL client)
SHOW JOBS;

-- Cancel a job
STOP JOB '<job_id>' WITH SAVEPOINT;

-- Session configuration
SET 'execution.checkpointing.interval' = '30s';
SHOW FULL MODULES;
SHOW TABLES;
```

Flink Web UI provides: job topology, throughput (records/sec per operator), checkpoint history,
backpressure detection, watermark progression. Metrics integrate with Prometheus, Datadog,
Graphite, and other reporters.

---

## Cross-System Comparison Matrix

| Feature | ClickHouse | Databricks | Snowflake | Flink SQL |
|---------|-----------|------------|-----------|-----------|
| **Ingestion model** | Kafka table engine + MV | readStream API / `read_kafka` TVF | Kafka Connect + Snowpipe Streaming | CREATE TABLE with Kafka connector |
| **SQL-native** | Yes (DDL) | Partial (SQL + Python) | No (connector config) | Yes (DDL) |
| **Parallelism unit** | `kafka_num_consumers` | Kafka partitions = Spark tasks | Kafka Connect tasks + serverless scaling | `scan.parallelism` / operator parallelism |
| **Max parallelism** | Physical CPU cores | Cluster size | Serverless (auto) | TaskManager slots |
| **Dynamic scaling** | No (requires table re-creation) | No (except Lakeflow DLT) | Yes (serverless auto-scale) | Yes (Reactive Mode / Autopilot) |
| **Exactly-once** | Experimental (Keeper-based) | Yes (Delta Lake sink) | Yes (default) | Yes (Kafka transactions + checkpointing) |
| **Error handling** | Skip / stream / DLQ modes | Parse modes + manual DLQ | errors.tolerance + DLQ topic | Format-level ignore + side outputs |
| **Latency** | Seconds (flush interval) | Sub-second to seconds | 5-10 seconds (high-perf) | Milliseconds to seconds |
| **Throughput ceiling** | Depends on CPU cores / consumers | Cluster-dependent | Up to 10 GB/s per table | Cluster-dependent |
| **Monitoring** | `system.kafka_consumers` | Spark UI / StreamingQueryListener | `COPY_HISTORY` / `PIPE_STATUS` | Flink Web UI / Prometheus |
| **Pause/resume** | `DETACH` / `ATTACH` MV | Stop/start streaming query | `ALTER PIPE ... PAUSED` | `STOP JOB WITH SAVEPOINT` |

---

## Key Takeaways

1. **ClickHouse** provides the most SQL-native Kafka integration via the Kafka table engine,
   but lacks dynamic scaling and production-grade exactly-once semantics. The three-table
   (source → MV → destination) pattern is unique and powerful but requires manual DDL management.

2. **Databricks** offers the most flexible programming model (Python + SQL), but its SQL-only
   Kafka support is limited to Lakeflow Declarative Pipelines. Auto-scaling for streaming is
   actively discouraged in favor of fixed-size clusters or the DLT framework.

3. **Snowflake** takes a connector-first approach — SQL is used for table/pipe creation, but
   the streaming configuration is entirely in Kafka Connect properties. It stands out with
   serverless auto-scaling and exactly-once as the default, plus the new high-performance
   architecture supporting 10 GB/s throughput.

4. **Flink SQL** provides the richest streaming-native SQL experience with fine-grained
   parallelism control, exactly-once via Kafka transactions, watermark management, and
   CDC format support. It is the only system that treats streaming as a first-class SQL
   concept with unbounded tables and continuous queries.
