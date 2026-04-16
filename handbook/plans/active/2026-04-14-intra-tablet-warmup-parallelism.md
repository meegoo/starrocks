# Data Cache Warmup: Tablet 内并行预热

- Status: active (方案设计阶段)
- Owner: TBD
- Last Updated: 2026-04-16

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

| 机制 | 位置 | 切分方式 | 对本方案的启示 |
|------|------|---------|---------------|
| `enable_tablet_internal_parallel` | `OlapScanNode` / `LakeDataSourceProvider` | row-key range / rowid range（Morsel） | 架构模式可借鉴：FE 给意图，BE 决定实际 DOP |
| Large rowset split compaction | `Rowset(tablet_mgr, metadata, rowset_index, segment_start, segment_end)` | segment range | 证明 "一个 tablet 内按 segment 并行" 可行；但本方案不直接复用这个 ctor（它是给 compaction 的） |
| `CacheSelectScanner::_write_disk_ranges()` | Connector 路径 | disk range merge + 逐段 write_at_fully | 外表路径已有细粒度并行，不在本期范围 |
| `sst_warmup_done` 原子 guard | `LakeDataSource::init_tablet_reader` / `SegmentIterator::_do_get_next` | 共享 `shared_ptr<atomic<bool>>` | 本方案保持 "一 tablet 一 LakeDataSource"，原子 guard 天然生效 |

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

### 3.1 切分粒度：Segment（推荐）

**选型对比：**

| 粒度 | 并行度上限 | 均衡性 | 实现方式 | 额外开销 |
|------|----------|--------|----------|---------|
| **A1: Rowset** | rowset 数（通常 1-10） | 差，rowset 大小悬殊 | 按 rowset 并发 | 极低 |
| **A2: Segment** | segment 数（通常 100-1000） | 好，可按 `segment_size` 均衡 | 以已有 `SegmentIterator` 为并行单元 | 低 |
| **A3: Byte Range** | 任意 | 最好 | 需写新 range fetcher | 中，需处理 block 对齐 |

**选择 A2**：

- 100GB tablet 天然有 100-1000 个 segment（默认 segment 大小 ~100MB），提供 100x+ 并行度，足够。
- `TabletReader::get_segment_iterators()` 本就会为每个 segment 创建 `SegmentIterator`，直接以这些 iterator 作为并行单元即可，无须引入新的切分构造器。
- `RowsetMetadataPB.segment_size[]`（`gensrc/proto/lake_types.proto:153`）提供每个 segment 的字节大小，可按字节做均衡分组。
- Block cache key = `hash64(filename) + mtime`，按 segment 文件天然不同，并行任务间无冲突。
- A3 虽更灵活，但 segment 级已经给出足够并行度，且不需处理 block 边界对齐等复杂性。

### 3.2 执行路径：BE 自切分 + FE 下发并行度 hint（B3+）

**选型对比：**

| 路径 | 改动量 | 可观测性 | 资源隔离 | 用户面变化 | metadata 依赖 |
|------|--------|---------|---------|----------|---------------|
| **B1: 新增独立 RPC** | 大（新 proto, 新 thread pool, 新 executor） | 独立 metric/cancel | 独立线程池 | 无 | FE 需拉 metadata |
| **B2: FE 切分 scan range** | 中（FE planner + BE 小改） | 复用现有 query profile | 共享 scan thread pool | 无 | **FE 需额外 RPC 拉 metadata** |
| **B3+: BE 自切分 + FE 下发并行度 hint** | 小（FE 加 hint 字段 + BE 加并行调度） | 汇聚在单 fragment profile 下 | 独立 warmup 线程池 | 无 | 无（BE 执行时本就加载 metadata） |

**选择 B3+**：

B2 方案的核心前提——"FE 已持有 tablet metadata（rowset 列表、segment 数量、segment_size）"——在 Lake 架构下**不成立**。代码验证：

- `LakeTablet.java` 只存储聚合信息：`dataSize`、`rowCount`、`dataSizeUpdateTime`，**没有 rowset/segment 级元数据**。
- `OlapScanNode.addScanRangeLocations()` 构建 `TInternalScanRange` 时只填 `tablet_id`、`version`、聚合 `row_count`、replica 地址，无 segment 信息下发。
- Per-segment 元数据（`RowsetMetadataPB.segments[]`、`segment_size[]`）存储在对象存储上的 tablet metadata 文件中，**仅在 BE 执行时加载**。
- FE 有 `LakeService.getTabletMetadatas()` RPC 可拉取详细元数据，但目前仅在 `ADMIN REPAIR TABLE` 路径使用，从未进入查询规划热路径。

若走 B2，需要在 CACHE SELECT 规划路径引入一次批量 metadata RPC，带来：
- **额外延迟**：tablet 数大时不可忽视（metadata 在对象存储上，BE 需先加载再返回）。
- **一致性窗口**：FE 拿到 metadata 后到 BE 执行之间，compaction 可能已改变 segment 布局。
- **架构侵入**：违背 "FE 轻量规划、BE 重执行" 原则。

**B3+ 核心思路**：

1. **FE**：基于 `tablet.dataSize`（已有）和阈值判断是否需要切分；若需要，在 `TInternalScanRange` 中下发一个 `cache_warmup_parallelism` hint（并行度目标值），**不做 segment 级切分**。
2. **BE**：`LakeDataSource` 在 `cache_file_only=true` 且收到 hint > 1 时，利用其**本就要加载**的 tablet metadata 获取 segment 列表，在内部把 segment 分组派发到独立的 warmup 线程池并行执行 touch_cache。

**优势**：

- **零额外 RPC**：FE 只用已有的 `tablet.dataSize`；BE 用其本就要加载的 metadata。
- **一个 tablet → 一个 LakeDataSource**：SST warmup 的 `sst_warmup_done` 原子 flag 在单 LakeDataSource 内共享，自然只执行一次，无 B2 方案下"多子 scan range 导致 SST 重复 warmup"问题。
- **调度无锚定问题**：一个 tablet 仍然只产生一个 `TScanRangeLocations`，现有 Coordinator 分发逻辑无需改动。
- **资源隔离**：warmup 跑在独立 `cache_warmup_pool`，不挤占查询 scan pool。
- **用户面不变**：仍是 `CACHE SELECT PROPERTIES(...) SELECT ...`。

**劣势与缓解**：

- FE 不感知实际并行度 → FE 下发的是 hint（建议值），BE 可根据实际 segment 数/大小自行调整，不做全局精确调度。
- 相比 B2 失去 "每个子 scan range 独立的 IO profile"；改为在 `cache_file_only` 路径下汇总统计（per-tablet warmup 耗时、并行度、分组字节数），仍可满足可观测性。

### 3.3 详细设计

#### 3.3.1 FE 侧改动

**决策逻辑**（在 `OlapScanNode.addScanRangeLocations()` 中，不是 `DataCacheSelectExecutor`——切分决策要在常规 scan range 构建路径做，避免在 `DataCacheSelectExecutor` 里绕回 planner）：

```
对每个 tablet:
  if !session.enableCacheSelect:               # 非 CACHE SELECT 查询
    不下发 hint
  elif !(tablet instanceof LakeTablet):        # 非 cloud-native 表
    不下发 hint
  elif tablet.dataSize <= threshold:           # 小 tablet
    不下发 hint
  else:
    internalRange.cache_warmup_parallelism = session.cacheSelectIntraTabletParallelism
```

FE 只做这一件事——设置 hint。不做 segment 级切分，不发 metadata RPC。

**新增 session variable**：
- `cache_select_intra_tablet_parallelism`：单 tablet 最大 warmup 并行度，默认 4（保守，避免 IO 压垮对象存储）。
- `cache_select_intra_tablet_threshold_bytes`：触发 intra-tablet 并行的 tablet 大小阈值，默认 1GB。

**TScanRange 扩展**：
- 在 `TInternalScanRange` 中增加可选字段 `cache_warmup_parallelism: i32`（取新字段序号 17）。缺省表示不启用。

#### 3.3.2 BE 侧改动

**新增 warmup 线程池**：
- 在 `ExecEnv` 中新增 `_cache_warmup_pool`（`ThreadPool`，默认 max_threads = min(cores, 16)，max_queue_size 有界）。
- 对应 BE 配置：`cache_warmup_thread_pool_size`，`<=0` 时取默认。
- 独立线程池的作用：避免 warmup IO 占用查询 scan pool，同时提供有界队列做背压。

**并行执行点**：`TabletReader::init_collector()`（Lake 路径）

选在此处而非 `SegmentIterator` 内部，原因：
- `SegmentIterator` 只看到单个 segment，不感知 tablet 全貌，无法做跨 segment 的负载均衡。
- `TabletReader` 是首个能同时看到所有 segment 的层次，也已经加载完 tablet metadata 和所有 `Rowset`/`SegmentIterator`。
- 不引入新的切分构造器（如 `Rowset` 的 segment-range ctor）——那是为 compaction 设计的，复用它会把 morsel/rowid-range 切分和 segment-range 切分搅在一起。

**算法**：

```
Status init_collector(params):
    seg_iters = get_segment_iterators(params)   # 已有逻辑

    if params.lake_io_opts.cache_file_only
       and params.lake_io_opts.cache_warmup_parallelism > 1
       and seg_iters.size() > 1:
        parallelism = min(hint, seg_iters.size())

        # 按 segment 大小（来自 RowsetMetadataPB.segment_size）贪心分组
        # 使各组累加字节数尽量均衡
        groups = balance_by_bytes(seg_iters, segment_sizes, parallelism)

        futures = []
        for group in groups:
            futures.push(pool.submit([group]{
                for iter in group:
                    while get_next() != EOF: pass   # 触发 cache_file_only 路径的 touch_cache
                    iter.close()
            }))
        wait_all(futures)

        _collect_iter = empty_iterator()          # 所有工作已在并行任务里完成
        return OK
    # 否则走原有串行路径
    ...
```

均衡策略使用 `segment_size[i]`（从 `RowsetMetadataPB.segment_size[]` 读取）做贪心装箱，而非简单的 round-robin，以降低大 segment 倾斜的尾延迟。

**关键不变量**：
- populate 逻辑不变——仍走 `CacheInputStream::_populate_to_cache()` → `BlockCache::write()`。
- cache key = `hash64(filename) + mtime`，不同 segment 文件 key 不同，并行任务间无冲突。
- **SST warmup 语义保持**：`sst_warmup_done` 是 `shared_ptr<atomic<bool>>`，在同一个 `LakeDataSource`/`TabletReader` 内的所有 `SegmentIterator` 共享。并行执行时，首个 CAS 成功的任务执行 SST warmup，其余跳过。
- `cache_file_only` 路径的 `_do_get_next` 语义不变——读完所有列 IO range、触发 touch_cache、返回 EOF。

#### 3.3.3 Connector 路径（Hive/Iceberg）

Connector 路径的 CACHE SELECT 已经由 `CacheSelectScanner` 按 disk range 拉取，粒度已较细，不在本期 B3+ 范围。若外表单文件成为长尾（单 Parquet >几十 GB），可在 FE 侧对 `THdfsScanRange` 做 byte-range 切分，作为后续独立方案。

#### 3.3.4 背压与资源控制

- **独立线程池**：`cache_warmup_pool` 与 scan pool 隔离，warmup 不影响在线查询。
- **有界队列**：`cache_warmup_pool` 的 `max_queue_size` 有界（如 1024），超额时 `submit_func` 返回错误，fallback 到当前线程串行执行该分组，天然限流。
- **对象存储限流**：共享 `SharedBufferedInputStream` 的 IO 调度，并发数上限 = `cache_warmup_parallelism`（默认 4）× 同时运行的 tablet warmup 数，不突破 starlet fs 连接池。
- **内存**：每个并行任务持有 若干 segment reader + 一个 4MB buffer（`CacheInputStream._buffer`）。单 tablet 4 路并发 ≈ 16MB，可接受。

#### 3.3.5 幂等性与容错

- **幂等**：`BlockCache::write()` 对 already_exist 返回 OK，重复写入无副作用。并行任务可任意重试。
- **版本漂移**：`TInternalScanRange` 携带 `tablet_id + version`，BE 加载对应版本的 metadata；compaction 换 segment 文件名 → cache key 不同 → 新旧 cache 共存。
- **取消**：CACHE SELECT 的取消由 `ConnectContext.kill()` → `StmtExecutor.cancel()` → fragment cancel 传播。`init_collector` 内的并行任务在等待 `futures.get()` 时，若上层 cancel，会收到 `is_cancelled` 信号传递到 `touch_cache` 的读循环中中断。需在 warmup lambda 中检查 `runtime_state->is_cancelled()`，避免 cancel 后仍跑完所有 segment。
- **单任务失败**：任一并行任务返回非 OK，`init_collector` 等待所有未决 future 后返回第一个错误，上层 fragment 正常传播失败。
- **线程池满**：submit 失败时降级为该分组在当前线程内串行执行，保证前进性。

## 4. 设计折衷总结

### 4.1 A2（Segment）vs A1（Rowset）vs A3（Byte Range）

| | A1 Rowset | A2 Segment | A3 Byte Range |
|--|----------|-----------|--------------|
| 优势 | 零改动 | 100x 并行度，按 `segment_size` 均衡 | 完美均衡 |
| 劣势 | 并行度不够（1-10），大小极不均衡 | segment 间仍有轻微大小差异 | 需新写 range fetcher，处理 block 对齐 |
| 结论 | ❌ 不够 | ✅ 选择（以已有 segment iterator 为并行单元） | ❌ 过度设计 |

说明：B3+ 方案下，A2 的落地不需要引入 `Rowset` 的 segment-range 构造函数——`TabletReader` 本就会为每个 segment 创建一个 `SegmentIterator`，我们直接把这些现成的 iterator 按 `segment_size` 分组交给线程池即可。

### 4.2 B3+ vs B1（独立 RPC）vs B2（FE 切分）

| | B1 独立 RPC | B2 FE 切 scan range | **B3+ BE 自切 + FE hint** |
|--|-----------|-------------------|--------------------------|
| 优势 | 完全独立的 metric/cancel/优先级 | 切分策略 FE 集中控制 | FE 无额外 metadata RPC；SST warmup 单次；无调度锚定问题；独立线程池 |
| 劣势 | 大量新代码（proto + pool + executor） | **FE 缺 segment metadata，需加 RPC**；一 tablet 多 scan range 引发调度锚定 & SST 重复 warmup | FE 不感知实际并行度（仅是 hint） |
| 结论 | ❌ 过重 | ❌ 前提（FE 持有 metadata）不成立 | ✅ 选择 |

### 4.3 为什么 hint 而不是精确切分

- FE 只知道 `tablet.dataSize`（聚合），不知道 segment 数/大小 —— 精确切分在 FE 侧不可能，除非先做 metadata RPC。
- BE 执行时本就会加载 tablet metadata，获取 segment 信息零额外成本。
- hint 作为上限建议：BE 看到实际 segment 数 < hint 时可自动收窄；看到 segment 极少（如 1 个）时直接走串行路径。
- 这种 "FE 定意图、BE 定策略" 的分层与 StarRocks 其他类似机制（如 `enable_tablet_internal_parallel` 只告诉 BE "允许切"，实际 DOP 由 BE `_could_tablet_internal_parallel` 决定）一致。

### 4.4 保留 CACHE SELECT 用户面

- 不引入新 DDL/DML 语法。
- 新机制纯属内部分流，老用户零迁移成本。
- 小 tablet（≤ 阈值）不发 hint，完全走原串行路径，行为不变。
- 未设置 session 变量的用户行为不变（默认并行度 4，阈值 1GB，已覆盖大多数场景）。

## 5. 已识别风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| 对象存储限流 | 并发过高触发 S3 429 | 独立 `cache_warmup_pool` 大小有限（默认 ≤ 16）+ 单 tablet hint 默认 4，共同限流 |
| 内存 | 每并行任务持有 segment reader + buffer | 单 tablet 4 路 ≈ 16MB；多 tablet 同时 warmup 受线程池总数限制，上限 ≈ 16 × 4MB = 64MB |
| Segment 大小倾斜 | 部分分组偏慢 | 用 `RowsetMetadataPB.segment_size[]` 按字节贪心装箱分组 |
| SST warmup 被重复触发 | PK 索引 SST 文件被下载 N 次 | B3+ 保持一 tablet 一 `LakeDataSource`，`sst_warmup_done` 共享原子 flag 自然只执行一次 |
| Warmup 与 compaction 版本漂移 | FE 下发的 version 已被 compaction 替代 | `TInternalScanRange` pin `tablet_id + version`，segment 文件名变化 → cache key 不同，新旧共存无冲突 |
| 线程池队列溢出 | 并发 CACHE SELECT 过多时 `submit_func` 失败 | 降级为在当前线程内串行执行该分组，保证前进性 |
| 取消不及时 | 大 tablet warmup 中途 query 被 cancel，未能立即中断 | warmup lambda 在每个 segment 结束后检查 `runtime_state->is_cancelled()` |
| hint 与实际不匹配 | FE 按聚合 dataSize 估，实际 segment 数极少（如 1 个大 segment） | BE 在 `seg_iters.size() ≤ 1` 或 `parallelism ≤ 1` 时直接走串行路径 |

## 6. Open Questions

1. **线程池大小是否要动态调整？** 当前 B3+ 用固定大小 `cache_warmup_thread_pool_size`（默认 min(cores, 16)）。若多租户场景下并发 CACHE SELECT 频繁，可考虑按 warehouse/resource group 拆分独立池。
2. **Connector 路径是否也需要 intra-file 并行？** 本期不做，外表单文件通常不到 100GB。若需要，可在 FE 对 `THdfsScanRange` 做 byte-range 切分（另立方案）。
3. **跨 FE failover 的 warmup 恢复**：CACHE SELECT 是一个普通查询，FE 切主后不自动重试。是否需要 warmup 任务持久化？
4. **warmup 优先级是否独立于 data cache 优先级？** 当前 `DataCachePriority` 由 CACHE SELECT PROPERTIES 指定，并行 warmup 任务全部继承同一 priority。
5. **需要新增 profile 指标吗？** 为了验证并行效果，建议在 `cache_file_only` 路径下加若干 counter：`CacheWarmupParallelism`（实际并行度）、`CacheWarmupGroupBytes`（各组字节数的 min/max/avg）、`CacheWarmupWallTime`。

## 7. 关键代码引用

### 7.1 已有代码（理解现状用）

| 组件 | 文件 | 关键行 |
|------|------|--------|
| FE warmup 入口 | `fe/fe-core/.../datacache/DataCacheSelectExecutor.java` | 45-104 |
| FE session 覆写 | 同上 | 148-161 |
| FE CACHE SELECT 分析（仅外表 + 共享数据 OLAP） | `fe/fe-core/.../sql/analyzer/DataCacheStmtAnalyzer.java` | 128-171 |
| FE `LakeTablet` 元数据（仅聚合 dataSize/rowCount） | `fe/fe-core/.../lake/LakeTablet.java` | 42-211 |
| FE `OlapScanNode.addScanRangeLocations` | `fe/fe-core/.../planner/OlapScanNode.java` | 553-709 |
| BE `CacheSelectInputStream` | `be/src/io/cache_select_input_stream.hpp` | 22-92 |
| BE `CacheInputStream::_populate_to_cache` | `be/src/io/cache_input_stream.cpp` | 442-509 |
| BE `cache_file_only` 路径（touch_cache 循环） | `be/src/storage/rowset/segment_iterator.cpp` | 2048-2091 |
| BE SST warmup 原子 guard | 同上 | 2080-2086 |
| BE `LakeDataSource::init_tablet_reader`（设置 `sst_warmup_done`） | `be/src/connector/lake_connector.cpp` | 429-438 |
| BE Lake scan range 构建 + `cache_file_only` 开关 | `be/src/connector/lake_connector.cpp` | 329-334 |
| BE Lake tablet internal parallel（rowid-range，不是 segment-range） | `be/src/connector/lake_connector.cpp` | 1225-1295 |
| BE `TabletReader::init_collector`（本期主要改动点） | `be/src/storage/lake/tablet_reader.cpp` | 509+ |
| `RowsetMetadataPB.segment_size[]`（用于字节均衡） | `gensrc/proto/lake_types.proto` | 153 |
| BE Block cache key (`hash64(filename) + mtime`) | `be/src/io/cache_input_stream.cpp` | 46-62 |
| BE `LakeIOOptions`（`cache_file_only`、`sst_warmup_done`、`sst_warmup_fn`） | `be/src/storage/options.h` | 72-89 |
| BE ExecEnv 线程池声明 | `be/src/runtime/exec_env.h` | 301, 403 |
| BE ExecEnv 线程池初始化 | `be/src/runtime/exec_env.cpp` | 440-447 |

### 7.2 本期改动触点

| 组件 | 文件 | 改动性质 |
|------|------|---------|
| 新增 `cache_warmup_parallelism` 字段 | `gensrc/thrift/PlanNodes.thrift` `TInternalScanRange` | 新增 optional 字段（ordinal 17） |
| 新增 session 变量 | `fe/fe-core/.../qe/SessionVariable.java` | `cache_select_intra_tablet_parallelism`、`cache_select_intra_tablet_threshold_bytes` |
| FE 设置 hint | `fe/fe-core/.../planner/OlapScanNode.java::addScanRangeLocations` | 在 `internalRange` 上 setCache_warmup_parallelism |
| BE 新增配置 | `be/src/common/config.h` | `cache_warmup_thread_pool_size` |
| BE 新增线程池 | `be/src/runtime/exec_env.{h,cpp}` | `_cache_warmup_pool`（`ThreadPool`） |
| BE `LakeIOOptions` 新增字段 | `be/src/storage/options.h` | `cache_warmup_parallelism` |
| BE 读 hint | `be/src/connector/lake_connector.cpp::init_reader_params` | 从 `_scan_range` 拷到 `_params.lake_io_opts` |
| BE 并行执行 | `be/src/storage/lake/tablet_reader.cpp::init_collector` | 按 `segment_size` 分组 → 提交到 `_cache_warmup_pool` → 等 future |
