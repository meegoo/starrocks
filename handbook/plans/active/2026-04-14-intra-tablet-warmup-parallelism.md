# Data Cache Warmup: Tablet 内并行预热

- Status: active (方案设计阶段)
- Owner: TBD
- Last Updated: 2026-04-14

## 1. 背景与问题

### 1.1 现状

StarRocks 通过 `CACHE SELECT` 语句触发显式 data cache 预热。执行流程：

1. **FE**：`DataCacheSelectExecutor.cacheSelect()` 将 `CACHE SELECT` 转成 `INSERT INTO BLACKHOLE() SELECT ...`，为每个 compute resource 创建一个子 `ConnectContext`，强制覆写 session 变量（`enable_populate_datacache=true`、`DataCachePopulateMode=ALWAYS`、`enable_cache_select=true` 等），然后走普通查询路径。
2. **BE（Connector 路径，Hive/Iceberg 等）**：`CacheSelectScanner` 解析文件 footer 收集 disk ranges，通过 `CacheSelectInputStream::write_at_fully()` 逐段拉取远程数据并写入 block cache，**不做列解码**。
3. **BE（Lake 路径，cloud-native 表）**：`SegmentIterator` 在 `cache_file_only=true` 时，通过 `ColumnIterator::get_io_range_vec()` 获取列的 IO ranges，逐段调 `touch_cache(offset, size)` 拉取并 populate。

### 1.2 瓶颈

Lake tablet 可达 100GB。在当前架构下：

- 一个 tablet 对应一个 `TScanRangeLocations`，分配给一个 fragment instance → 一个 scanner 线程串行读完整个 tablet 的所有 rowset/segment。
- CACHE SELECT 是**同步等待**的——用户看到的墙钟时间 = `max(per-tablet warmup time)`。
- 单线程从对象存储读 100GB，按 S3 单连接 150-300 MB/s 计，需要 5.5-11 分钟。这是整个 warmup 作业的长尾。
- 即便 inter-tablet 并行度足够（多 CN × 多线程），一个巨型 tablet 仍然拖累全局完成时间。

### 1.3 已有内部先例

| 机制 | 位置 | 切分方式 | 适用范围 |
|------|------|---------|---------|
| `enable_tablet_internal_parallel` | `OlapScanNode` / `LakeDataSourceProvider` | row-key range（Morsel 切分） | 查询路径，不影响 warmup |
| Large rowset split compaction | `Rowset(tablet_mgr, metadata, rowset_index, segment_start, segment_end)` | segment range | Compaction，**可复用** |
| `CacheSelectScanner::_write_disk_ranges()` | Connector 路径 | disk range merge + 逐段 write_at_fully | 外表 cache select |

## 2. 什么时候需要并行 populate？

| 触发场景 | 是否需要 intra-tablet 并行 | 原因 |
|---------|:------------------------:|------|
| 写入时 populate（RowsetWriter） | 否 | 每个 writer 写一个 segment 时顺手填 cache，天然并行 |
| Compaction populate | 否 | 后台任务，非用户关键路径 |
| 普通查询 async populate | 否 | 不阻塞查询返回；scan DOP 已提供足够并行度 |
| 查询 miss → 远程读 | 否 | 查询的 scan range 切分已决定并行度 |
| **CACHE SELECT 显式预热** | **是** | 用户同步等待，墙钟时间 = max(per-tablet)，单 tablet 成长尾 |
| **冷启动 / cache 全量回填** | **是** | 首查延迟有 SLA，长尾 tablet 拖累所有依赖查询 |
| **CN 故障迁移 / tablet rebalance** | **是** | tablet primary 漂移到冷 CN，持续慢直到 warm 完成 |
| **弹性扩容（新 CN 加入）** | **是** | 新 CN 上线前的关键路径 |

**结论**：intra-tablet 并行仅在**用户/系统同步等待 + 单 tablet 是长尾**的场景有意义。设计目标聚焦 CACHE SELECT 路径。

**判定逻辑**：

```
需要 intra-tablet 并行 ⟺
  tablet_data_size / single_stream_bandwidth > acceptable_latency_threshold
  AND inter-tablet 并行已打满（tablet 数 < CN 数 × per-CN warmup concurrency）
```

小 tablet 不切——管理开销大于收益。典型阈值在 GB 量级。

## 3. 方案设计

### 3.1 切分粒度：Segment Range（推荐）

**选型对比：**

| 粒度 | 并行度上限 | 均衡性 | 复用性 | 额外开销 |
|------|----------|--------|--------|---------|
| **A1: Rowset** | rowset 数（通常 1-10） | 差，rowset 大小悬殊 | 无需新代码 | 极低 |
| **A2: Segment Range** | segment 数（通常 100-1000） | 好，可按 segment_size 均衡 | 复用 Rowset segment-range ctor | 低 |
| **A3: Byte Range** | 任意 | 最好 | 需写新 range fetcher | 中，需处理 block 对齐 |

**选择 A2**：

- 100GB tablet 天然有 100-1000 个 segment（默认 segment 大小 ~100MB），提供 100x+ 并行度，足够。
- `Rowset` 已有 `(rowset_index, segment_start, segment_end)` 构造函数（`be/src/storage/lake/rowset.h:63-64`），原为 split compaction 设计，直接可复用。
- `RowsetMetadataPB.segment_size`（`gensrc/proto/lake_types.proto:153`）提供每个 segment 的字节大小，可按字节做均衡切分。
- Block cache key = `hash64(filename) + mtime`，按 segment 文件天然不同，无跨 subtask 冲突。
- A3 虽更灵活，但 segment 级已经给出足够并行度，且不需处理 block 边界对齐等复杂性。

### 3.2 执行路径：复用查询栈 + 切分 Scan Range（推荐）

**选型对比：**

| 路径 | 改动量 | 可观测性 | 资源隔离 | 用户面变化 |
|------|--------|---------|---------|----------|
| **B1: 新增独立 RPC** | 大（新 proto, 新 thread pool, 新 executor） | 独立 metric/cancel | 独立线程池 | 无 |
| **B2: 复用查询栈，FE 切分 scan range** | 中（FE planner 改动 + BE 小改） | 复用现有 query profile | 共享 scan thread pool | 无 |
| **B3: BE 自行切分** | 小（BE only） | 需新增 metric | 共享线程池 | 无 |

**选择 B2**：

- **保持 CACHE SELECT 用户面不变**——用户仍写 `CACHE SELECT PROPERTIES(...) SELECT ...`，无需学新语法。
- FE 已持有 tablet metadata（包括 rowset 列表、segment 数量、segment_size），天然是切分决策点。
- 切分后的子 scan range 走现有的 fragment 分发 → scan node → segment iterator 路径，**复用已有的 `cache_file_only` 优化**（跳过列解码，直接 touch_cache）。
- 可观测性免费获得——每个 sub-range 的 IO 统计汇聚在 query profile 里。
- 相比 B1，少维护一套独立的 RPC + executor + metric 体系。
- 相比 B3，切分策略在 FE 统一管控，BE 只做执行，职责清晰。

### 3.3 详细设计

#### 3.3.1 FE 侧改动

**切分决策**（在 `DataCacheSelectExecutor` 或 planner 中）：

```
对每个 tablet:
  if tablet.dataSize < intra_tablet_warmup_threshold:  # 默认 1GB
    不切分，走原路径
  else:
    从 tablet metadata 获取 rowset 列表
    对每个 rowset:
      按 segment_size 累加，切分为 N 个 segment-range subtask
      每个 subtask = (tablet_id, version, rowset_index, segment_start, segment_end)
    将 subtask 生成为多个 TScanRangeLocations，分配到同一 CN
```

**新增 session variable**：
- `cache_select_intra_tablet_parallelism`：单 tablet 最大并行度，默认 16
- `cache_select_intra_tablet_threshold_bytes`：触发 intra-tablet 切分的 tablet 大小阈值，默认 1GB

**TScanRange 扩展**：
- 在 `TInternalScanRange` 中增加可选字段 `segment_range_start` / `segment_range_end`，标识当前 scan range 只负责 tablet 中的部分 segment。

#### 3.3.2 BE 侧改动

**Lake scan path**：

`LakeDataSource`（或 `SegmentIterator`）识别 scan range 中的 `segment_range_start/end`：
- 如果存在，只加载 `[segment_range_start, segment_range_end)` 范围的 segment。
- 复用 `Rowset(tablet_mgr, metadata, rowset_index, segment_start, segment_end)` 构造函数。
- 后续 `cache_file_only` 路径不变——遍历这些 segment 的列，逐个 `touch_cache`。

**关键不变量**：
- populate 逻辑完全不变——仍走 `CacheInputStream::_populate_to_cache()` → `BlockCache::write()`。
- cache key 由 `hash64(filename) + mtime` 决定，不同 segment 文件 key 不同，子任务之间无冲突。
- `_already_populated_blocks` 在每个 `CacheInputStream` 实例内去重；跨实例的重复由 `BlockCache` 的 `already_exist` 处理。

#### 3.3.3 Connector 路径（Hive/Iceberg）

Connector 路径的 CACHE SELECT 已经由 `CacheSelectScanner` 按 disk range 拉取，粒度已较细。如果外表文件也很大（如单个 Parquet 文件几十 GB），可在 FE 侧对 `THdfsScanRange` 做 byte-range 切分（`offset + length`），但这是后续优化，不在本期范围内。

#### 3.3.4 背压与资源控制

- **Per-CN 并发上限**：`cache_select_intra_tablet_parallelism` 控制单 tablet 的最大子任务数，避免一个 tablet 占满所有 scan 线程。
- **对象存储限流**：子任务共享 `SharedBufferedInputStream` 的 IO 调度，不会突破 starlet fs 的连接池上限。
- **内存**：每个子任务持有若干 segment reader + 一个 4MB buffer（`CacheInputStream._buffer`），16 个并发 ≈ 64MB，可接受。

#### 3.3.5 幂等性与容错

- **幂等**：`BlockCache::write()` 对 already_exist 返回 OK，重复写入无副作用。子任务可任意重试。
- **版本漂移**：scan range 中携带 `tablet_id + version`，BE 加载对应版本的 metadata；如果 compaction 导致版本变更，segment 文件名变化 → cache key 变化，新旧 cache 共存，无冲突。
- **取消**：CACHE SELECT 的取消由 `ConnectContext.kill()` → `StmtExecutor.cancel()` → 各 fragment cancel 传播，子 scan range 与原 scan range 走同一个取消链。

## 4. 设计折衷总结

### 4.1 A2（Segment Range）vs A1（Rowset）vs A3（Byte Range）

| | A1 Rowset | A2 Segment Range | A3 Byte Range |
|--|----------|-----------------|--------------|
| 优势 | 零改动 | 100x 并行度，复用现有 ctor | 完美均衡 |
| 劣势 | 并行度不够（1-10），大小极不均衡 | segment 间仍有轻微大小差异 | 需新写 range fetcher，处理 block 对齐 |
| 结论 | ❌ 不够 | ✅ 选择 | ❌ 过度设计 |

### 4.2 B2（复用查询栈）vs B1（独立 RPC）vs B3（BE 自切分）

| | B1 独立 RPC | B2 复用查询栈 | B3 BE 自切分 |
|--|-----------|-------------|-------------|
| 优势 | 完全独立的 metric/cancel/优先级 | 改动量适中，复用 profile/cancel | 改动最小 |
| 劣势 | 大量新代码（proto + pool + executor） | 共享 scan 线程池，可能影响查询 | FE 不感知切分，不好做全局调度 |
| 结论 | ❌ 过重 | ✅ 选择 | ⚠️ 可作为短期快速方案 |

### 4.3 FE 切分 vs BE 切分

- **FE 切分**（选择）：FE 持有 tablet metadata（segment 数量、大小），是全局调度的自然入口。切分策略统一管控，BE 只执行被交付的子 scan range。
- **BE 切分**：BE 需要从 metadata 获取 segment 信息，然后自行起多线程——职责边界模糊，且 FE 无法感知实际并行度做全局背压。

### 4.4 保留 CACHE SELECT 用户面

- 不引入新 DDL/DML 语法。
- 新机制纯属 executor/planner 内部分流，老用户零迁移成本。
- 小 tablet 完全走原路径，行为不变。

## 5. 已识别风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| 对象存储限流 | 并发过高触发 S3 429 | 受 starlet fs 连接池 + per-tablet 并行度上限双重约束 |
| 内存 | 每子任务持有 segment reader + buffer | 16 并发 ≈ 64MB，可接受；可加 memory tracker |
| Segment 大小倾斜 | 部分子任务偏慢 | 用 `segment_size` 按字节均衡分组，而非简单等分 segment 数 |
| Warmup 与 compaction 版本漂移 | scan range 指定的 version 被 compaction 替代 | scan range pin `tablet_id + version`，metadata 不匹配时 BE 报错，FE 重排 |
| 共享 scan 线程池争抢 | warmup 子任务占用查询 scan 线程 | 可通过 resource group 或独立 scan pool 隔离（后续优化） |

## 6. Open Questions

1. **是否需要独立的 warmup scan thread pool？** 短期复用 scan pool，长期如果 warmup 频率高，可考虑隔离。
2. **Connector 路径是否也需要 intra-file 并行？** 本期不做，外表单文件通常不到 100GB。
3. **跨 FE failover 的 warmup 恢复**：CACHE SELECT 是一个普通查询，FE 切主后不自动重试。是否需要 warmup 任务持久化？
4. **warmup 优先级是否独立于 data cache 优先级？** 当前 `DataCachePriority` 由 CACHE SELECT PROPERTIES 指定，子任务继承。

## 7. 关键代码引用

| 组件 | 文件 | 关键行 |
|------|------|--------|
| FE warmup 入口 | `fe/fe-core/.../datacache/DataCacheSelectExecutor.java` | 45-104 |
| FE session 覆写 | 同上 | 148-161 |
| BE CacheSelectInputStream | `be/src/io/cache_select_input_stream.hpp` | 22-92 |
| BE CacheInputStream populate | `be/src/io/cache_input_stream.cpp` | 442-509 |
| BE cache_file_only 路径 | `be/src/storage/rowset/segment_iterator.cpp` | 2048-2091 |
| BE Rowset segment-range ctor | `be/src/storage/lake/rowset.h` | 55-64 |
| Connector CacheSelectScanner | `be/src/exec/hdfs_scanner/cache_select_scanner.cpp` | 全文 |
| Lake scan range 构建 | `be/src/connector/lake_connector.cpp` | 329-334 |
| Lake tablet internal parallel | `be/src/connector/lake_connector.cpp` | 1225-1295 |
| RowsetMetadataPB.segment_size | `gensrc/proto/lake_types.proto` | 153 |
| Block cache key | `be/src/io/cache_input_stream.cpp` | 46-62 |
| ExecEnv 线程池 | `be/src/runtime/exec_env.h` | datacache_rpc_pool |
