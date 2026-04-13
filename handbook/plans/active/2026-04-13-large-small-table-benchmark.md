# Large/Small Table Mixed Benchmark (50 GB Tablet)

- Status: active
- Owner: luoyixin
- Last Updated: 2026-04-13
- Predecessor: [1000-Table 1 TXN/s Benchmark](./2026-04-02-1000-table-1txn-benchmark.md)

## Summary

前一轮 benchmark（Phase 1–3）聚焦 **1000 张小 tablet（≤10 GB）+ 1 TXN/s** 的高并发
Stream Load 场景。本轮切换到另一条正交的压力路径：**单 tablet 体量放大到
50 GB**，同时存在**大表（10 TB）**和**小表（100 GB）**混合负载，验证 shared-data
架构在「单 tablet 大数据量 + 表宽度差异巨大」的真实数仓场景下的瓶颈。

关注点：

- PK persistent index 在单 tablet 数十 GB 级别的 rebuild / multi_get 成本
- Compaction 在 50 GB tablet 上的调度、IO 放大和对写路径的 P99 影响
- 少量 hot tablet（大表）与海量 cold tablet（小表）共存时的线程池 / RPC 公平性
- 大表 bulk load (10 TB) + 小表持续 Stream Load 同时进行时的资源争抢
- Datacache 命中率和 OSS 远端读在大 tablet 场景下的表现

## Acceptance Criteria

Benchmark 完成时满足以下条件即视为本轮任务结束（找到并修复/记录瓶颈）：

1. 在目标 workload 下 Upsert **P50 < 1 s、P99 < 5 s**，error rate = 0%。
2. 每个被识别的瓶颈都有：trace / metric 证据 + 定位到的代码路径 + 修复或 config
   变更或 follow-up issue。
3. 结果表（iteration 表格）、最终 config diff、以及必要的 PR 链接在本文档更新。
4. 本轮 benchmark 工具（`realtime-benchmark` 的 `mixed-tables` template）合入上游
   仓库，可被后续人员一键复现。

## Workload 设计

### 拓扑（候选 A：对称 1000 tablet，便于对比 Phase 2）

| 类别   | 表数 | 每表大小 | 每表 bucket | tablet 数 | 单 tablet 数据 | 小计    |
| ------ | ---- | -------- | ----------- | --------- | -------------- | ------- |
| 大表   | 2    | 10 TB    | 200         | 400       | 50 GB          | 20 TB   |
| 小表   | 300  | 100 GB   | 2           | 600       | 50 GB          | 30 TB   |
| **总** |      |          |             | **1000**  |                | **50 TB** |

tablet 总数与 Phase 2 相同（1000），但单 tablet 数据量从 ≤10 GB 扩大到 50 GB，
可以直接对比同一线程池配置下的差异。

### 拓扑（候选 B：更激进，突出大表）

| 类别   | 表数 | 每表大小 | 每表 bucket | tablet 数 | 小计   |
| ------ | ---- | -------- | ----------- | --------- | ------ |
| 大表   | 10   | 10 TB    | 200         | 2000      | 100 TB |
| 小表   | 100  | 100 GB   | 2           | 200       | 10 TB  |
| **总** |      |          |             | **2200**  | 110 TB |

大表主导，用于观察 compaction / PK index 在大量大 tablet 上的可扩展性。
第一轮先跑候选 A，验证后再决定是否升级到 B。

### PK schema（沿用 Phase 2）

```sql
CREATE TABLE `rt_large_<i>` (
  tenant_id bigint NOT NULL,
  event_time datetime NOT NULL,
  event_id bigint NOT NULL,
  region varchar(32),
  device_type varchar(32),
  channel varchar(64),
  status tinyint,
  metric_a double,
  metric_b double,
  metric_c bigint,
  payload varchar(256)
) ENGINE=OLAP
PRIMARY KEY(tenant_id, event_time, event_id)
PARTITION BY date_trunc('month', event_time)
DISTRIBUTED BY HASH(tenant_id) BUCKETS 200          -- 大表 200, 小表 2
PROPERTIES (
  "enable_persistent_index" = "true",
  "persistent_index_type" = "CLOUD_NATIVE",
  "file_bundling" = "true",
  "compression" = "LZ4",
  "datacache.enable" = "true",
  "replication_num" = "1",
  "storage_volume" = "builtin_storage_volume"
);
```

`tenant_id` 分布需要在**大表内均匀**，避免 hot bucket；小表因为只有 2 bucket，
天然只需对 `tenant_id % 2` 均匀即可。

### Workload Phases

#### Phase A — 大表预填（bulk load，10 TB × 2）

- 工具：**Spark Load** 或 **Broker Load from OSS**，不走 Stream Load（Stream
  Load 在 10 TB 级别吞吐不够，且会污染写路径 benchmark）。
- 生成脚本产出 parquet（每大表 200 份，每份 ~50 GB，直接对应 200 bucket）。
- 目标：在 benchmark 正式开始前，每个大表 bucket 已经有 50 GB 数据。
- 校验：`SHOW PARTITIONS` 行数 & OSS 对象大小；`SHOW TABLET` 平均 rowset 数
  ≤ 20（有必要先手动触发一次 major compaction）。

#### Phase B — 小表预填（100 GB × 300）

- 100 GB × 300 = 30 TB，数据量可控，用 **并行 Broker Load**（300 个并发 job）。
- 同样要求预填后 per-tablet rowset ≤ 20。

#### Phase C — 稳态混合写（benchmark 主负载）

两类负载同时运行：

| Stream  | 目标                 | 并发                 | QPS / 表 | 每次行数   |
| ------- | -------------------- | -------------------- | -------- | ---------- |
| 大表写  | 贴近 ETL append      | 10（每大表 5 个）    | 0.2      | 100,000    |
| 小表写  | 贴近实时 upsert      | 300（每小表 1 个）   | 1        | 200        |

- 大表单次 batch 大（100k 行），模拟 10–30 MB HTTP body，走 Stream Load；
  也可以改用 `INSERT INTO SELECT` 对比。
- 小表沿用 Phase 2 的 1 TXN/s × 200 rows 模式。
- 运行时长：≥ 30 分钟稳态 + 额外 10 分钟给 compaction settle。

#### Phase D — 读叠加（可选，第二轮）

- 每个大表并发 5 个点查 + 1 个聚合（`tenant_id = ?` 和 `GROUP BY region`）。
- 每个小表 1 QPS 点查。
- 用于观察写压下的查询 P99 / datacache 行为。

## Metrics & Observability

每次 iteration 必须收集下列字段，写回 iteration 表格：

- **客户端侧（Benchmark tool）**
  - 按表类别分桶：`upsert_p50 / p95 / p99 / max`（大表、小表分别统计）
  - `error_rate`、`retry_rate`
  - 客户端侧 CPU、aiohttp event-loop lag（复用 Phase 1 已经有的计量）
- **服务端侧**
  - `load_profile`：`WriteData / Commit / PublishVersion` 占比，大表/小表分别聚合
  - PK index：`pindex_init_sst_open`、`rebuild_index_del_cost`、`multiget_t3`、
    `cache_hit / cache_miss`、`read_block_max_latency_us`
  - 线程池：`transaction_publish_version` active/queue、`async_delta_writer`
    active/queue、`pk_index_parallel_execution` active/queue
  - Compaction：`cumulative_compaction_num_rowsets`、`base_compaction_bytes`、
    每 CN compaction 任务队列长度
  - OSS：请求 QPS / P99 latency（从 CN 侧 metric + OSS 侧日志）
  - Datacache：命中率、淘汰速率、本地磁盘占用
- **集群侧**
  - CN CPU / mem / EBS iops / network in-out
  - FE `max_running_txn_num_per_db` 水位

推荐的 trace PR：继续沿用 Phase 2 #10 里的 `multiget_t3` breakdown
（#71439），大 tablet 场景会让这段更有价值。

## Iteration Template

每次 iteration 填一行到下方表格（初始为空）。

| #   | 假设的瓶颈 | 变更            | 大表 P50 | 大表 P99 | 小表 P50 | 小表 P99 | Error | 观测/下一步 |
| --- | ---------- | --------------- | -------- | -------- | -------- | -------- | ----- | ----------- |
|     |            |                 |          |          |          |          |       |             |

## 预期瓶颈清单（作为 hypothesis，不是结论）

按优先级排列，第一轮 iteration 从 H1/H2 开始验证：

- **H1 PK persistent index 体积**：单 tablet 50 GB 行 → PK index 也是 GB 级，
  SST 层数增加，`multiget_t3` 会比 Phase 2 进一步恶化。关注 `cache_miss`
  在有 2–4 GB datacache budget 时的表现。
- **H2 Compaction 放大**：50 GB tablet 做一次 base compaction 近似要读写
  50 GB，会长时间占住 CN 带宽，触发小表 P99 尖刺。需要看
  `cumulative_compaction` 是否能跟上写入速度。
- **H3 大表 publish_version 成本**：大表单个 txn 涉及 200 bucket → 200 个
  publish RPC 并发发起，可能再次把 `transaction_publish_version` 打满，即使已
  配到 512。
- **H4 Stream Load body 大**：大表 100k 行 / 次 → ~30 MB HTTP body，走
  `streaming_load_thread_pool`，读网络 + 反序列化 + PK index 查询串行；需要确认
  与小表共享线程池是否公平。
- **H5 OSS 并发**：bulk load + 稳态写 + compaction 三者都打 OSS，单 CN OSS
  连接池 / bandwidth 可能先于 CPU 饱和。
- **H6 FE metadata**：大表 200 bucket × 12 partition × N rowset 的 meta 更新
  是否会让 publish 阶段 FE 侧变慢。

## 工具改造（`realtime-benchmark`）

上一轮用的是 `--template 1000-tables`，本轮需要新模板 `--template mixed-tables`。
改造要点（这些改动要提到 `StarRocks/realtime-benchmark` 上游 PR）：

### 1. 多类别表声明

在 `templates/mixed-tables.yaml` 里支持类别配置：

```yaml
categories:
  large:
    count: 2
    size_gb: 10240
    buckets: 200
    stream:
      workers_per_table: 5
      rows_per_batch: 100000
      target_qps_per_table: 0.2
  small:
    count: 300
    size_gb: 100
    buckets: 2
    stream:
      workers_per_table: 1
      rows_per_batch: 200
      target_qps_per_table: 1.0
```

### 2. Bulk preloader

新增 `preload` 子命令，功能：

- 按 category 生成 parquet 到 OSS（每 bucket 一个 part，便于后续 Broker Load
  按 bucket 并行）。
- 调用 FE 提交 Broker Load / Spark Load job，并轮询到 FINISHED。
- 生成并保存 `preload_manifest.json` 记录实际落地行数、rowset 数、tablet 大小。
- 支持 `--resume`，中断后不重复写。

### 3. 按 category 分组的 AsyncIngestionPool

Phase 1 做的 `AsyncIngestionPool` 单进程单 loop，对当前 QPS 够用。本轮要扩展：

- 每个 category 一个独立 `AsyncIngestionPool` 实例（独立 event loop 线程），
  避免大表的大 body send 阻塞小表的高 QPS。
- 生产者线程池按 category 隔离（大表 CPU 重，小表 CPU 轻）。
- 每个 batch 打 `category` 标签，metrics 分桶输出。

### 4. Metrics & report

- Prometheus `/metrics` 端点按 `category`、`table_id` 维度暴露：
  `upsert_latency_ms{category="large",quantile="0.99"}` 等。
- 运行结束产出 `report.md`，表格形式给出 iteration 需要的所有字段，可直接贴回
  本文档。
- 自动抓取 FE 的 `LOAD` profile（通过 `SHOW LOAD PROFILE` / `/api/profile`）
  并按 category 聚合关键算子耗时。

### 5. 读 workload generator（Phase D 用）

- 新增 `--read-mix` 参数，支持点查和聚合两类 SQL 模板。
- 读写共用同一个 metric pipeline，report 里读延迟和写延迟并列。

### 6. Safety / Idempotency

- 所有 DDL（`CREATE TABLE`、`DROP TABLE`）支持 `--dry-run`，避免误伤。
- `rt_bench_*` 命名前缀固定，避免误操作用户表。
- preload 失败自动降级：允许某些 bucket 预填数据 < 目标，但会在 report 里标红。

## 时间线 / 里程碑（粗粒度）

1. **工具改造**：`mixed-tables` template + preloader + category AsyncPool。
2. **数据预填**：Phase A + B 跑通，validated 单 tablet ≈ 50 GB。
3. **稳态 Iteration 1**：直接沿用 Phase 2 收尾的 config，测 baseline。
4. **Iteration 2–N**：按 hypothesis 表格逐个打掉瓶颈。
5. **Phase D**：加入读负载，补充混合场景数据。
6. **收尾**：config diff 固化、PR 合入、结果回写。

## Decision Log

- 2026-04-13: 选择「每 tablet 50 GB」作为正交于 Phase 2 的压力维度，tablet 数
  仍控制在 1000 以便与 Phase 2 结果可比（拓扑 A）。
- 2026-04-13: 大表预填走 Broker Load / Spark Load 而不是 Stream Load，原因是
  Stream Load 在 10 TB 级别吞吐不够，且会污染写路径 benchmark。
- 2026-04-13: 按 category 拆 `AsyncIngestionPool`，避免大表的大 body I/O
  阻塞小表的 1 TXN/s 稳态流量。
