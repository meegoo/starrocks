# Data Cache Populate：为什么大 Tablet 不需要额外的并行化

- Status: **archived**（决策记录，不实施）
- Last Updated: 2026-04-16
- 最初动机：想解决 "CACHE SELECT 遇到 100GB tablet 时单 scanner 串行、墙钟长尾" 的问题
- 结论：**现有机制已经提供足够并行化，真正的瓶颈是资源限制，不是并行度不足**

## 0. TL;DR

如果一个开发者看到 "大 tablet 的 data cache warmup 好像很慢"，想写一个新的并行化子系统——请先读完本文。结论是：

1. Lake 模式所有 data cache populate 路径已经天然并行（scan DOP、tablet internal parallel、async 写回）。
2. CACHE SELECT 自 3.4 起默认继承 tablet internal parallel，不需要额外切分机制。
3. 真正的瓶颈是**资源限制**（对象存储带宽、本地 SSD IOPS、cache 容量），加并行只会更快撞上限制。
4. 如果用户觉得 CACHE SELECT 慢，调两个 session 变量即可（`tablet_internal_parallel_mode='force_split'` + `pipeline_dop`）。
5. 唯一可以考虑的代码改动是 **`DataCacheSelectExecutor` 自动开 `force_split`（约 5-10 行 Java）**，不是新建 thread pool / scan range 字段 / TabletReader 分组逻辑。

## 1. 最初看起来像个问题

Lake tablet 可达 100GB。按 S3 单连接 150-300 MB/s 计，单线程读完要 5-11 分钟。CACHE SELECT 同步等待——如果 "一 tablet 一 scanner 串行"，单个大 tablet 就是整个 warmup 的长尾。

这个推理看似合理，但两个前提需要验证：

- (P1) 所有 populate 场景的并行度是否真的够？
- (P2) CACHE SELECT 是否真的 "一 tablet 一 scanner"？

代码审查显示 **(P1) 够，(P2) 不是**。

## 2. 所有 populate 路径的并行化现状

StarRocks Lake 架构下所有往 local data cache 写数据的路径：

| 触发源 | 代码入口 | Populate 时机 | 并行度来源 |
|--------|---------|--------------|----------|
| **写入**（TabletWriter） | `fs_starlet.cpp:383`，`WritableFileOptions.skip_fill_local_cache=false`（默认） | 写入时，starlet 顺手填本地 cache | 多 tablet × 多 segment writer |
| **Compaction 产出新 segment** | 同上（compaction 也走 TabletWriter） | 同上 | Compaction worker 池 + 多 tablet 并发 |
| **Compaction 读输入** | `horizontal_compaction_task.cpp:62`，`lake_io_opts.fill_data_cache=true` | 读输入时 piggyback populate | Compaction 内部多 segment iterator + range-split 子任务 |
| **普通查询** | `cache_input_stream.cpp:_populate_to_cache` + `options.async=true`（3.3+ 默认） | Miss 时从远程读 + async 写 cache | scan DOP（fragment 数 × tablet 数 × tablet-internal-parallel morsel 数 × per-column IO） |
| **CACHE SELECT** | `segment_iterator.cpp:2048-2091` `cache_file_only` 路径 | 显式 touch_cache，不解码 | **继承普通查询的 tablet internal parallel**（见 §3）|
| **Peer cache → local** | `cache_input_stream.cpp:147-163` | Miss 本地时先问 peer，命中后填本地 | 同普通查询 |

**关键事实**：

1. **写入和 compaction 产出的新数据，在生产它们的 CN 上默认已经在 local cache 里**（starlet 写穿透，`fs_starlet.cpp:383` `fslib_opts.skip_fill_local_cache = opts.skip_fill_local_cache`，默认 false）。
2. **普通查询的 populate 是 async + best-effort**，piggyback 在 read 上（`cache_input_stream.cpp:442-501`）。
3. **`datacache_max_concurrent_inserts` 默认 150 万**，`ResourceBusy` 被 `_can_ignore_populate_error` 吞掉，查询不受影响（`cache_input_stream.cpp:503-509`）。

## 3. CACHE SELECT 已经有 intra-tablet 并行

这是最容易误判的一点。证据链：

### 3.1 `DataCacheSelectExecutor` 不关 tablet internal parallel

`fe/fe-core/.../datacache/DataCacheSelectExecutor.java:148-161` 只覆盖这 7 个 session 变量：

```java
sessionVariable.setCatalog(...);
sessionVariable.setEnableScanDataCache(true);
sessionVariable.setEnablePopulateDataCache(true);
sessionVariable.setDataCachePopulateMode(DataCachePopulateMode.ALWAYS.modeName());
sessionVariable.setEnableDataCacheAsyncPopulateMode(false);
sessionVariable.setEnableDataCacheIOAdaptor(false);
sessionVariable.setDataCacheEvictProbability(100);
sessionVariable.setDataCachePriority(...);
sessionVariable.setDatacacheTTLSeconds(...);
sessionVariable.setEnableCacheSelect(true);
```

**不包含** `enable_tablet_internal_parallel` 或 `enable_lake_tablet_internal_parallel`——继承用户 session。

### 3.2 `enable_lake_tablet_internal_parallel` 默认 true（3.4+）

`release-3.4.md:119`：

> 参数 enable_lake_tablet_internal_parallel 默认设置为 true，存算分离集群中的云原生表默认开启并行扫描。

### 3.3 切分机制完整走通

```
FE 下发 enable_tablet_internal_parallel=true
  ↓
LakeDataSourceProvider::_could_tablet_internal_parallel (lake_connector.cpp:1250-1296)
  ↓ 满足条件时切
PhysicalSplitMorselQueue (rowid-range 切分，每 morsel 一个 pipeline driver)
  ↓
每个 morsel 创建独立的 LakeDataSource → TabletReader → SegmentIterator
  ↓
SegmentIterator::_init() → _get_row_ranges_by_rowid_range()
  ↓  _scan_range &= rowid_range_option  (segment_iterator.cpp:3517)
cache_file_only 路径用被过滤后的 _scan_range 调 get_io_range_vec
  ↓  (segment_iterator.cpp:2065)
get_io_range_vec(_scan_range, ...) 只返回本 morsel 对应 rowid 的 page 字节范围
  ↓  (scalar_column_iterator.cpp:738-779)
touch_cache(offset, size) 只 populate 本 morsel 的那部分数据
```

**每个 morsel 对应的 rowid range 对应不同的 page 字节范围 → 不同的 cache block → 多 morsel 并行 populate 无冲突、无重复 IO**。

### 3.4 结论

3.4+ Lake 模式下，CACHE SELECT 遇到大 tablet **已经自动按 rowid-range 切 morsel 并行 warmup**。"一 tablet 一 scanner" 的前提错误。

## 4. 真正的瓶颈：资源限制

大 tablet 场景确实可能"慢"，但根源不是并行度：

| 表象 | 真实根因 | 现有对策 |
|------|---------|---------|
| 首查延迟高 | 冷数据、远程 IO 带宽 | 提前 CACHE SELECT 预热 |
| Cache 污染（大查询挤掉热数据） | Cache 容量有限 | SLRU eviction + `DataCachePopulateMode`（AUTO/NEVER/ALWAYS）+ `datacache_evict_probability` + 优先级 |
| Populate 部分丢失 | `datacache_max_concurrent_inserts`、容量、SSD 写带宽限流 | `_can_ignore_populate_error` 吞 `ResourceBusy`/`CapacityLimitExceeded`/`AlreadyExist`，查询不失败 |
| 多次查仍冷 | 上一条累积的结果（populate 被丢） | CACHE SELECT 同步、full 覆盖 |
| 对象存储带宽打满 | 物理上限 | 水平扩 CN |

**增加并行度一个都解决不了**——更多并行只会更快撞上以上资源上限。

## 5. CACHE SELECT 仍有的小 gap（但不需要新机制）

`_could_tablet_internal_parallel` 的触发条件（`lake_connector.cpp:1254-1256`）：

```cpp
bool force_split = tablet_internal_parallel_mode == TTabletInternalParallelMode::type::FORCE_SPLIT;
if (!force_split && num_total_scan_ranges >= pipeline_dop) {
    return false;
}
```

对普通查询是合理的（tablet 数 >= DOP 时已经能填满 CPU 并行度），但对 CACHE SELECT 偏保守：

| | 普通查询 | CACHE SELECT |
|---|--------|-------------|
| 关心什么 | CPU 利用率 | IO 墙钟（单 tablet 是长尾） |
| DOP 够了但单 tablet 大 | 无所谓 | **仍然要切** |

### 5.1 用户现在就能调的旋钮

如果 CACHE SELECT 确实觉得慢，不需要改代码：

- `SET tablet_internal_parallel_mode = 'force_split'` —— 绕过 `num_total_scan_ranges >= pipeline_dop` 判断
- `SET pipeline_dop = <更大>` —— 提高总并行上限
- `SET enable_lake_tablet_internal_parallel = true` —— 仅在 3.3 或被显式关闭时

### 5.2 可选的小改动（如果嫌手动调不友好）

唯一值得做的代码改动：让 `DataCacheSelectExecutor` 自动设 `force_split`（约 5-10 行 Java）。

```java
// DataCacheSelectExecutor.buildCacheSelectConnectContext()
sessionVariable.setTabletInternalParallelMode("force_split");
```

副作用：CACHE SELECT 会更激进地切 morsel。收益：去掉 CACHE SELECT 下的 tablet-数-大于-DOP 保守拦截。

**这不是本 plan 的范围，如果要做应该另开一个 ~10 行改动的小 PR。**

## 6. 不要做的事

以下是走过的弯路，记录在案避免重复踩坑：

- ❌ **新增 `cache_warmup_thread_pool`**：pipeline driver executor + scan task queue 已经是做这件事的通用基础设施，再做一个只服务 CACHE SELECT 的独立池是重复造轮子。
- ❌ **新增 `TInternalScanRange.cache_warmup_parallelism` thrift 字段**：`enable_tablet_internal_parallel` + `tablet_internal_parallel_mode` 已经表达了同一意图。
- ❌ **在 `TabletReader::init_collector` 里再加一层并行分组**：rowid-range morsel 切分 + pipeline driver 并行已经在做这件事，再加一层会和 morsel 机制打架。
- ❌ **FE 侧切分 scan range**：FE 没有 segment metadata（`LakeTablet.java` 只有聚合 `dataSize`，拉 metadata 需要 RPC），且一 tablet 多 scan range 会引发 Coordinator 分发锚定问题 + SST warmup 多次执行问题。
- ❌ **给 compaction 输出加并行 re-warmup**：starlet 的 `skip_fill_local_cache=false` 默认行为已经在 compaction worker CN 上填了 cache，不存在 "compaction 后 cache gap" 问题。

## 7. 给遇到类似问题的人

如果你看到 "CACHE SELECT 好像慢"、"大 tablet warmup 有长尾"，按以下顺序排查：

1. **确认版本**：是 3.4+ 且 `enable_lake_tablet_internal_parallel` 为 true 吗？不是则先打开。
2. **看 profile**：查询 profile 里 morsel 数 / driver 数是否 > 1 per tablet？如果是 1，说明切分没触发。
3. **确认被"tablet 数 >= DOP"拦截了**：就用 `SET tablet_internal_parallel_mode = 'force_split'`，重跑看效果。
4. **物理瓶颈**：如果切分已经触发但仍慢，看对象存储出口带宽、CN 本地 SSD 写带宽、`datacache_max_concurrent_inserts` 指标。这些是扩容问题，不是代码问题。
5. **Populate 是否丢了**：查 `skip_write_cache_count`、`write_cache_fail_count` 统计。如果大量丢，说明撞了容量/并发/带宽限流，扩容或降压。

**不要第一反应就"写个新机制"**。至少 90% 的"warmup 慢"是配置 + 资源问题。

## 8. 关键代码引用

| 组件 | 文件 | 关键行 |
|------|------|--------|
| FE CACHE SELECT 入口 | `fe/fe-core/.../datacache/DataCacheSelectExecutor.java` | 45-104（执行），148-161（session 覆写） |
| FE CACHE SELECT 分析 | `fe/fe-core/.../sql/analyzer/DataCacheStmtAnalyzer.java` | 128-171 |
| FE LakeTablet（只有聚合 metadata） | `fe/fe-core/.../lake/LakeTablet.java` | 42-211 |
| FE OlapScanNode.addScanRangeLocations | `fe/fe-core/.../planner/OlapScanNode.java` | 553-709 |
| `enable_lake_tablet_internal_parallel` 默认 true | 3.4 release notes | - |
| BE starlet 写穿透 cache | `be/src/fs/fs_starlet.cpp` | 383 |
| BE `WritableFileOptions.skip_fill_local_cache` 默认 false | `be/src/fs/fs.h` | 281 |
| BE Compaction 读输入填 cache | `be/src/storage/lake/horizontal_compaction_task.cpp` | 62-63 |
| BE `CacheInputStream` async populate | `be/src/io/cache_input_stream.cpp` | 442-509 |
| BE `_can_ignore_populate_error`（吞 ResourceBusy） | `be/src/io/cache_input_stream.cpp` | 503-509 |
| BE `cache_file_only` 路径 | `be/src/storage/rowset/segment_iterator.cpp` | 2048-2091 |
| BE SST warmup 原子 guard | 同上 | 2080-2086 |
| BE `_get_row_ranges_by_rowid_range` | `be/src/storage/rowset/segment_iterator.cpp` | 3511-3517 |
| BE `get_io_range_vec`（按 rowid range 映射到字节） | `be/src/storage/rowset/scalar_column_iterator.cpp` | 738-779 |
| BE LakeDataSource 设置 `cache_file_only` | `be/src/connector/lake_connector.cpp` | 329-331 |
| BE Lake tablet internal parallel 触发判定 | `be/src/connector/lake_connector.cpp` | 1250-1296 |
| BE `datacache_max_concurrent_inserts` 默认 150 万 | `be/src/common/config.h` | 1465 |
| BE `datacache_evict_probability` / SLRU 配置 | `be/src/common/config.h` | 1456-1520 |

## 9. 分析时走过的弯路（供参考）

1. **最初方案 B2（FE 切 scan range）**：前提假设 "FE 持有 tablet metadata" 错误——FE 只有聚合 `dataSize`，没有 rowset/segment 列表。如果要做需要新增 metadata RPC，架构侵入大。
2. **修正方案 B3+（BE 自切 + FE 下发 hint + 新 thread pool）**：解决了 metadata 问题，但不知道 rowid-range morsel 已经存在，所以重复造了一套并行 executor。
3. **方案 C（调整 tablet internal parallel 的触发条件）**：接近正确答案，但没完全意识到现有 session 变量已经能做到。
4. **最终结论（本文档）**：根本不需要新代码，现有机制足够，手动调 session 变量即可。如果觉得手动调不友好，5-10 行 Java 让 `DataCacheSelectExecutor` 自动开 `force_split` 是唯一值得做的事。

这个走弯路的过程本身说明一件事：**提方案之前必须先把现有代码搞透，否则会给已经解决的问题重新设计一套机制**。
