# RCA: BE Memory Fluctuation + FE Young GC Spikes + Slow Query Timeouts

**Date**: 2026-04-10  
**Cluster**: hujie  
**Incident Window**: ~11:45 – 12:25 (GMT-7: 20:45 – 21:25 on 2026-04-09)  
**Status**: Root cause identified; profile data needed for final confirmation

---

## 1. Collected Evidence

### 1.1 Monitoring Charts

#### FE Query Total (3 FE nodes)
- **Normal baseline**: ~500 queries/s per FE node
- **During incident**: Periodic spikes to 1000–1500 queries/s
- **Spike pattern**: Multiple bursts between 11:45–12:25, synchronized across all 3 FE nodes
- **Post-incident**: Returns to stable ~500 queries/s after 12:25

#### FE JVM Young GC Time (3 FE nodes)
- **Normal baseline**: Near 0 ms
- **During incident**: GC pause spikes reaching **up to 2 seconds**, occurring repeatedly between 11:45–12:25
- **Spike pattern**: Strongly correlated with FE query total spikes — every query burst triggers a corresponding GC spike
- **Post-incident**: Returns to near 0 ms after 12:25

#### BE Process Memory Bytes
- **Normal baseline**: ~128 GiB (stable)
- **During incident**: Fluctuations up to ~160 GiB, with periodic dips
- **Node variation**: One node (purple line) shows the most significant fluctuation

#### BE Query Memory Bytes
- **Normal baseline**: Near 0
- **During incident**: Large **negative spikes down to -96 GiB** (purple line), indicating rapid bulk memory release events
- **Interpretation**: Negative values in `query_pool_mem_tracker` are caused by bulk memory deallocation when large queries complete — jemalloc hook-based tracking shows the instantaneous accounting delta

### 1.2 Slow Query Log (fe.audit.log, sampled at ~04:00 UTC on 2026-04-10)

A single 6-second window (04:00:00 – 04:00:07 UTC) produced **30+ slow queries**, all from the same workload.

#### Common Query Characteristics

| Attribute | Value |
|-----------|-------|
| User | `cdp_query_user` |
| Source Application | `Intuit.mailchimp.mcmktg.cdpsegmentation` / `Mailchimp.mailchimp.mcmktg.segmentationtoken` |
| Resource Group | `default_wg` (no concurrency limit) |
| Query Queue | **Disabled** (`enable_query_queue_select = false`) |
| Query Type | CDP contact audience segmentation |
| Query Pattern | `SELECT contact.id FROM cdp.contact LEFT OUTER JOIN <MV_tables> WHERE account_id = <X> GROUP BY contact.id ORDER BY contact.id LIMIT 500001` |
| Joined MV Tables | `segment_membership_query_mv`, `contact_merge_fields_v7_query_mv`, `email_send_query_mv`, `email_open_query_mv`, `email_click_v1_query_mv`, `order_v2_query_mv` |
| JOINs per Query | 1–3 LEFT OUTER JOINs |
| Filters per Query | 3–12 conditional filters |

#### Query Status Breakdown

**TIMEOUT Queries (State=ERR, ErrorCode=TIMEOUT):**

| AccountId | Time (ms) | ScanBytes | ScanRows | FE Alloc Mem | PlanMemCost |
|-----------|-----------|-----------|----------|--------------|-------------|
| 15506419 | 60031 | **0** | **0** | 68 MB | 134 GB |
| 1529605 | 60026 | **0** | **0** | 59 MB | 23 GB |
| 222319122 | 60037 | **0** | **0** | 105 MB | 56 GB |
| 185791166 | 60039 | **0** | **0** | 97 MB | 149 GB |
| 44490269 | 60058 | **0** | **0** | 113 MB | 191 GB |
| 35154529 | 60040 | **0** | **0** | 93 MB | 171 GB |
| 31997751 | 60041 | **0** | **0** | 111 MB | 174 GB |
| 245484894 | 60159 | **0** | **0** | 66 MB | 118 GB |
| 242035718 | 60033 | **0** | **0** | 97 MB | 152 GB |
| 156119882 | 60044 | **0** | **0** | 104 MB | 63 GB |
| 8021957 | 60046 | **0** | **0** | 107 MB | 57 GB |
| 191466382 | 60030 | **0** | **0** | 75 MB | 109 GB |
| 26394267 | 60042 | **0** | **0** | 75 MB | 141 GB |
| 154980206 | 60039 | **0** | **0** | 63 MB | 105 GB |
| 230087158 | 60043 | **0** | **0** | 98 MB | 152 GB |
| 245484894 | 60047 | **0** | **0** | 76 MB | 118 GB |
| 45167705 | 60039 | **0** | **0** | 113 MB | 250 GB |
| 227941910 | 60052 | **0** | **0** | 115 MB | 117 GB |
| 19328475 | 60058 | **0** | **0** | **175 MB** | **231 GB** |
| 243352698 | 60037 | **0** | **0** | 106 MB | 121 GB |

**Key observation**: All TIMEOUT queries have `ScanBytes=0, ScanRows=0` but `QueryFEAllocatedMemory > 0` (59–175 MB). This means FE planning completed successfully (memory was consumed during plan generation), fragments were deployed to BE, but **scan never started on BE before the 60s timeout**.

**Successful but Slow Queries (State=EOF):**

| AccountId | Time (ms) | ScanBytes | ScanRows | ReturnRows | MemCostBytes | FE Alloc Mem |
|-----------|-----------|-----------|----------|------------|-------------|-------------|
| 245484894 | 52843 | **2.78 GB** | 41.6M | 1 | 113 MB | 66 MB |
| 31935035 | 58285 | 374 MB | 16.0M | 1868 | 73 MB | 96 MB |
| 157015350 | 55124 | 1.0 GB | 15.4M | 0 | 53 MB | 68 MB |
| 102494734 | 54548 | 163 MB | 2.5M | 1 | 39 MB | 62 MB |
| 89042209 | 49427 | 174 MB | 2.6M | 5636 | 42 MB | 98 MB |
| 188693226 | 47868 | 271 MB | 4.1M | 2 | 41 MB | 74 MB |
| 7013037 | 48021 | 156 MB | 2.1M | 1 | 38 MB | 75 MB |
| 175240925 | 43619 | 38 MB | 494K | 1 | 33 MB | 66 MB |
| 1549469 | 42814 | 3.1 MB | 29K | 0 | 25 MB | 65 MB |

**Key observation**: Successful queries scan up to 2.78 GB / 41.6M rows, consuming up to 113 MB of BE memory. These large queries run 42–58 seconds, leaving very little headroom under the 60s timeout.

---

## 2. Slow Query Summary

### 2.1 Workload Profile

All slow queries originate from a **CDP (Customer Data Platform) audience segmentation service** that:

1. **Submits queries in bulk**: 30+ concurrent queries in a 6-second window, each for a different `account_id`
2. **Uses a uniform query pattern**: Every query follows the same template — `SELECT FROM cdp.contact LEFT JOIN <MV_tables> WHERE account_id = X GROUP BY id ORDER BY id LIMIT 500001`
3. **Varies in complexity by account**: Small accounts scan 3 MB / 29K rows; large accounts scan 2.78 GB / 41.6M rows
4. **JOINs multiple materialized views**: 1–3 LEFT OUTER JOINs with MV tables (`segment_membership_query_mv`, `contact_merge_fields_v7_query_mv`, `email_open_query_mv`, `email_send_query_mv`, etc.)
5. **All runs in `default_wg`**: No resource group isolation, no concurrency limit, no query queue

### 2.2 Two Categories of Slow Queries

| Category | Count | Avg Time | ScanBytes | Root Issue |
|----------|-------|----------|-----------|------------|
| **TIMEOUT (ScanBytes=0)** | ~20 | 60s (hard limit) | 0 | Scan never started before timeout |
| **Slow but Successful** | ~15 | 42–58s | 3 MB – 2.78 GB | Large scan + JOIN + aggregation |

---

## 3. Root Cause Analysis

### 3.1 Causal Chain

```
Root Cause: CDP segmentation service submits 30-40+ concurrent JOIN queries simultaneously
    │
    ├─[1] FE Impact: Young GC pressure
    │   ├─ Each query consumes 40–175 MB of FE heap during planning
    │   │   (ExecPlan, OptExpression trees, partition/tablet metadata, descriptor tables)
    │   ├─ 40 concurrent queries × ~75 MB avg = ~3 GB of short-lived young gen objects
    │   ├─ Young generation fills rapidly → GC pauses up to 2 seconds (STW)
    │   ├─ GC STW stalls all FE threads:
    │   │   - Query planning threads blocked
    │   │   - Fragment deploy RPC threads blocked
    │   │   - Result receiver threads blocked
    │   └─ FE GC does NOT directly cause TIMEOUT, but degrades overall FE throughput
    │
    ├─[2] BE Impact: Resource contention from concurrent large queries
    │   ├─ Successful queries scan up to 2.78 GB / 41.6M rows each
    │   ├─ Each query builds hash tables for JOIN + aggregation (tens of MB to GB)
    │   ├─ 15-20 concurrent large queries compete for:
    │   │   - CPU (pipeline driver scheduling)
    │   │   - Memory (hash join build side, aggregation hash map)
    │   │   - I/O bandwidth (scan throughput)
    │   ├─ BE process memory spikes from 128 GiB → 160 GiB
    │   └─ When queries complete in bursts → bulk memory release → query_mem negative spikes
    │
    ├─[3] TIMEOUT queries (ScanBytes=0): Starvation on BE
    │   ├─ FE planning completes (QueryFEAllocatedMemory > 0 confirms this)
    │   ├─ Fragments deployed to BE successfully
    │   ├─ BUT on BE, the query is stuck before scan starts:
    │   │   ├─ Possible: Waiting for runtime filter from build side of hash join
    │   │   │   (build side query on large account hasn't finished → probe side blocks)
    │   │   ├─ Possible: Pipeline driver scheduling delay under heavy concurrency
    │   │   │   (40+ queries × multiple pipelines = hundreds of drivers competing for CPU)
    │   │   └─ Possible: Upstream fragment in shuffle stage hasn't produced data yet
    │   ├─ After 60 seconds, FE ResultReceiver hits deadline → throws TimeoutException
    │   └─ Query cancelled with TIMEOUT, ScanBytes=0 because scan operator never got to run
    │
    └─[4] Feedback loop
        ├─ Slow queries occupy BE resources longer → more queries queue behind them
        ├─ More concurrent queries → more FE GC pressure → FE becomes less responsive
        ├─ FE GC stalls delay result fetching → queries take even longer
        └─ Cycle continues until the batch of segmentation queries completes (~12:25)
```

### 3.2 Why ScanBytes=0 for TIMEOUT Queries?

This is the key question. The evidence shows:

1. **FE planning completed** — `QueryFEAllocatedMemory` ranges from 59 MB to 175 MB for TIMEOUT queries, meaning the optimizer ran, plan was generated, and fragments were deployed
2. **Query queue is disabled** (`enable_query_queue_select = false`) — queries are NOT blocked waiting for slots in FE
3. **BE does not queue queries for memory** — BE's `WorkGroup::acquire_running_query_token()` either accepts or immediately rejects (returns `TooManyTasks`), it does not block
4. **Therefore, queries reached BE but scan never started** — the scan operator was ready but never got to execute within 60 seconds

The most likely BE-side bottleneck causing scan to never start:

| Hypothesis | Mechanism | Likelihood |
|------------|-----------|------------|
| **Runtime filter wait** | Probe-side scan blocked waiting for build-side hash join to finish constructing runtime filter. Under heavy concurrency, build side takes too long. | **High** |
| **Pipeline driver scheduling starvation** | Hundreds of pipeline drivers from 40+ concurrent queries compete for CPU cores. Some drivers never get scheduled within 60s. | **Medium** |
| **Data shuffle dependency** | Scan happens in a downstream fragment that depends on upstream fragment output. Upstream fragment stuck due to resource contention. | **Medium** |

### 3.3 Configuration Gaps

| Issue | Current State | Impact |
|-------|---------------|--------|
| No query queue | `enable_query_queue_select = false` | 40+ queries hit BE simultaneously with no admission control |
| No resource group isolation | All queries in `default_wg` | CDP workload competes with all other queries |
| No concurrency limit | `default_wg` has no `concurrency_limit` | No cap on concurrent queries |
| Query timeout too tight | `query_timeout = 60s` | Large account queries take 42–58s, leaving no margin |

---

## 4. Action Items Required from User

### 4.1 Profile Data Needed (Critical)

To confirm the exact BE-side bottleneck, we need query profiles from TIMEOUT queries. Specifically:

```sql
-- 1. Enable profile collection (if not already enabled)
SET GLOBAL enable_profile = true;
SET GLOBAL pipeline_profile_level = 1;

-- 2. During the next incident window, capture profiles for TIMEOUT queries
-- Use any of the TIMEOUT query IDs from the audit log, e.g.:
SHOW PROFILELIST LIMIT 100;

-- 3. For a specific query profile:
ANALYZE PROFILE FROM '<query_id>';
-- Or export full profile:
SHOW PROFILE '/query/<query_id>';
```

**What to look for in the profile:**

| Profile Metric | What It Tells Us |
|----------------|-----------------|
| `RuntimeFilterWaitTime` on scan operators | If > 0, confirms scan is blocked waiting for runtime filter |
| `ScheduleTime` on pipeline drivers | If very high, confirms driver scheduling starvation |
| `WaitForDependency` time | If > 0, confirms waiting for upstream fragment data |
| `BlockByInputEmpty` time on probe-side operators | Confirms probe side is starved of input |
| `PeakMemoryUsage` per query | Shows actual memory consumption on BE |
| `PullChunkNum = 0` on scan operators | Confirms scan never pulled any data |

### 4.2 Additional Configuration Data Needed

```sql
-- Runtime filter settings
SHOW VARIABLES LIKE '%runtime_filter%';
-- Key: runtime_filter_wait_timeout_ms, runtime_filter_on_exchange_node

-- Query timeout
SHOW VARIABLES LIKE 'query_timeout';

-- BE memory configuration
-- curl http://<be_host>:8040/api/show_config | grep -E 'mem_limit|spill|query_pool'

-- Pipeline DOP
SHOW VARIABLES LIKE 'pipeline_dop';

-- FE JVM settings
-- grep -E 'Xmx|Xms|Xmn|NewRatio' fe/conf/fe.conf
```

---

## 5. Recommended Mitigations

### 5.1 Short-term (No Code Change)

1. **Enable query queue with concurrency limit**:
   ```sql
   SET GLOBAL enable_query_queue_select = true;
   SET GLOBAL query_queue_concurrency_limit = 15;
   SET GLOBAL query_queue_pending_timeout_second = 300;
   ```
   This prevents 40+ queries from hitting BE simultaneously.

2. **Create dedicated resource group for CDP workload**:
   ```sql
   CREATE RESOURCE GROUP cdp_segmentation_wg
   WITH (cpu_weight=4, mem_limit='50%', concurrency_limit=15);
   
   -- Bind user to resource group
   CREATE RESOURCE GROUP CLASSIFIER
   FOR cdp_segmentation_wg (user='cdp_query_user');
   ```

3. **Reduce runtime filter wait timeout** (if confirmed as root cause):
   ```sql
   SET GLOBAL runtime_filter_wait_timeout_ms = 5000;  -- down from default
   ```

4. **Increase query timeout** for this workload:
   ```sql
   -- Set for the CDP user session
   SET query_timeout = 120;
   ```

5. **Increase FE Young Generation size** to reduce GC frequency:
   ```
   # fe.conf — adjust JAVA_OPTS
   -Xmn6g   (increase from current value)
   ```

### 5.2 Medium-term (Application-side)

6. **Limit client-side concurrency**: CDP segmentation service should throttle to 10–15 concurrent queries max
7. **Check retry logic**: If the application retries TIMEOUT queries immediately, it creates a thundering herd. Implement exponential backoff
8. **Enable spill** for large queries:
   ```sql
   SET GLOBAL enable_spill = true;
   SET GLOBAL spill_mem_limit_threshold = 0.8;
   ```

### 5.3 Long-term (Architecture)

9. **Pre-compute common segments** into materialized views to reduce query-time JOIN cost
10. **Batch large-account queries separately** from small-account queries to avoid head-of-line blocking

---

## 6. Summary

| Aspect | Finding |
|--------|---------|
| **What happened** | CDP segmentation service submitted 30-40+ concurrent JOIN queries, causing BE memory spikes, FE Young GC pauses up to 2s, and ~40% of queries timing out with ScanBytes=0 |
| **Root cause** | No admission control (query queue disabled, no resource group) allows unbounded concurrency; queries starve each other on BE |
| **Why ScanBytes=0** | Queries reached BE but scan never started — most likely blocked waiting for runtime filters or starved of pipeline scheduling under heavy concurrency |
| **Needs confirmation** | Query profile data from TIMEOUT queries to pinpoint exact BE-side bottleneck (runtime filter wait vs. scheduling starvation vs. shuffle dependency) |
| **Key fix** | Enable query queue or resource group with concurrency limit to prevent resource starvation |
