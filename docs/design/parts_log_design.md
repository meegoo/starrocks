# StarRocks 存算分离模式写入放大追踪设计文档

## 1. 背景和目标

### 1.1 背景
在StarRocks存算分离（Cloud Native）模式下，我们需要精确追踪表的数据写入情况和compaction行为，以便：
- 监控数据导入的实际写入量
- 追踪compaction过程中的数据读写
- **计算compaction的写入放大系数（Write Amplification Factor, WAF）**
- 优化compaction策略和性能调优

**写入放大系数**定义为：
```
WAF = (用户导入写入量 + Compaction写入量) / 用户导入写入量
    = 1 + (Compaction写入量 / 用户导入写入量)
```

高写入放大会导致：
- 更多的磁盘I/O和网络带宽消耗
- 更高的存储成本（云存储按写入次数计费）
- 潜在的性能瓶颈

### 1.2 目标
实现一个轻量级的写入追踪系统，专门用于写入放大分析：
1. **记录数据导入写入量**：每次事务提交时记录写入的数据量
2. **记录Compaction写入量**：每次compaction完成时记录输出数据量
3. **支持按表/分区聚合统计**：方便计算不同粒度的写入放大系数
4. **仅支持存算分离模式**：专注于Lake/Cloud Native场景

### 1.3 现有功能参考
StarRocks已有的相关功能：
- `SHOW PROC '/compactions'`：查看compaction任务的总体状态
- `INFORMATION_SCHEMA.be_cloud_native_compactions`：查看正在运行的compaction任务详情
- `INFORMATION_SCHEMA.partitions_meta`：查看分区的compaction score

**限制**：这些功能只显示当前/实时的状态，无法追踪历史写入量，不支持写入放大计算

## 2. 总体设计

### 2.1 系统架构

采用**双表架构**（参考 loads_history 设计）：内存表 + 持久化表 + Union视图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Frontend (FE)                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  INFORMATION_SCHEMA.tablet_write_log (Union View)                 │  │
│  │  - UNION ALL of memory table and persistent table                 │  │
│  │  - 用户查询接口，自动合并最新数据和历史数据                          │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                          │                      ▲                         │
│                          ▼                      │                         │
│  ┌────────────────────────────────┐  ┌──────────────────────────────┐   │
│  │ INFORMATION_SCHEMA             │  │ _statistics_                  │   │
│  │ .be_tablet_write_log           │  │ .tablet_write_log_history     │   │
│  │ (Memory table - scans BE)      │  │ (Persistent table)            │   │
│  └────────────────────────────────┘  └──────────────────────────────┘   │
│                 │                                    ▲                    │
│                 │ RPC Scan                           │ INSERT SELECT      │
│                 ▼                                    │                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │           TabletWriteLogHistorySyncer (FE Daemon)                  │ │
│  │  - 每60秒运行一次                                                   │ │
│  │  - INSERT INTO history SELECT FROM memory WHERE log_time < NOW-1m │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │           TableKeeper (FE Daemon)                                   │ │
│  │  - 每30秒检查持久化表是否存在                                        │ │
│  │  - 自动创建表和管理分区                                              │ │
│  └────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
                              │ RPC
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Compute Node (CN/BE)                      │
│  ┌────────────────────────────────────────────────────────┐ │
│  │           TabletWriteLogManager                         │ │
│  │  - 管理内存日志缓冲区（环形缓冲区）                      │ │
│  │  - 提供查询接口给FE SchemaScanner                       │ │
│  │  - 不负责持久化（由FE同步到持久化表）                    │ │
│  └────────────────────────────────────────────────────────┘ │
│              ▲                          ▲                    │
│              │                          │                    │
│  ┌───────────┴──────────┐   ┌──────────┴──────────────┐    │
│  │   Lake DeltaWriter    │   │  Lake CompactionTask    │    │
│  │  (数据导入)           │   │  (Compaction)           │    │
│  │  - finish时记录日志   │   │  - 完成时记录日志        │    │
│  └───────────────────────┘   └─────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 BE组件（C++）

**TabletWriteLogEntry（日志条目）**
- 记录单个tablet的数据写入或compaction事件
- 包含关键指标用于WAF计算

**TabletWriteLogManager（日志管理器）**
- 维护内存中的日志缓冲区（环形缓冲区，固定大小）
- 提供日志查询接口给SchemaScanner
- 自动清理过期日志（基于时间窗口）
- 线程安全
- **不负责持久化**（由FE syncer负责）

**SchemaBeTabletWriteLogScanner（系统表扫描器）**
- 扫描CN本地内存中的写入日志
- 返回日志条目给FE
- 用于 `information_schema.be_tablet_write_log` 内存表

#### 2.2.2 FE组件（Java）

**BeTabletWriteLogSystemTable（内存表）**
- 定义`INFORMATION_SCHEMA.be_tablet_write_log`内存表
- 通过RPC聚合所有CN的内存日志数据
- 保存最近数据（默认30分钟）

**TabletWriteLogHistorySystemTable（持久化表）**
- 定义`_statistics_.tablet_write_log_history`持久化表
- 物理表，支持分区和TTL
- 保存历史数据

**TabletWriteLogUnionView（Union视图）**
- 定义`INFORMATION_SCHEMA.tablet_write_log`视图
- `UNION ALL`合并内存表和持久化表
- **推荐用户使用此视图进行查询**

**TabletWriteLogHistorySyncer（历史同步器）**
- 继承`FrontendDaemon`，后台定期运行
- 每60秒执行一次
- 从内存表同步数据到持久化表（INSERT SELECT）
- 只同步1分钟前的已完成日志（避免重复）

**TableKeeper集成**
- 复用现有的TableKeeper daemon
- 自动创建持久化表（如果不存在）
- 管理动态分区和TTL

## 3. Schema 设计

### 3.1 概述

采用**三表架构**（参考 loads_history）：

1. **`INFORMATION_SCHEMA.be_tablet_write_log`** - 内存表（Memory Table）
   - 通过SchemaScanner扫描所有CN的内存缓冲区
   - 包含最近的日志数据（默认30分钟）
   - 查询实时性高，但CN重启后数据丢失

2. **`_statistics_.tablet_write_log_history`** - 持久化表（Persistent Table）
   - 物理表，持久化存储
   - FE Syncer定期从内存表同步数据
   - 支持动态分区和自动TTL

3. **`INFORMATION_SCHEMA.tablet_write_log`** - Union视图（推荐使用）
   - `UNION ALL`合并内存表和持久化表
   - 用户查询的推荐入口
   - 自动去重（持久化表只包含1分钟前的数据）

### 3.2 字段定义（所有表统一Schema）

**设计原则**：简洁实用，只保留WAF计算必需的字段。

| 列名 | 类型 | 说明 |
|------|------|------|
| **基础信息** | | |
| `log_time` | DATETIME | 日志记录时间 |
| `event_type` | VARCHAR | 事件类型：LOAD（导入）、COMPACTION（合并） |
| **标识信息** | | |
| `cn_id` | BIGINT | Compute Node ID（也叫BE_ID） |
| `db_name` | VARCHAR | 数据库名 |
| `table_name` | VARCHAR | 表名 |
| `table_id` | BIGINT | 表ID |
| `partition_id` | BIGINT | 分区ID |
| `tablet_id` | BIGINT | Tablet ID |
| **写入量统计** | | |
| `txn_id` | BIGINT | 事务ID |
| `version` | BIGINT | Version（LOAD为新version，COMPACTION为输出version） |
| `num_rows` | BIGINT | 行数 |
| `data_size` | BIGINT | **压缩后的数据大小（字节）- WAF计算核心指标** |
| `num_segments` | INT | Segment文件数量 |
| **Compaction专用字段** | | |
| `input_versions` | VARCHAR | 输入version列表，格式："2,3,4,5"（仅COMPACTION） |
| `input_rows` | BIGINT | 输入总行数（仅COMPACTION） |
| `input_data_size` | BIGINT | **输入总数据大小（字节）- 用于验证**（仅COMPACTION） |

**字段说明**：
- **核心WAF指标**：`data_size`字段记录每次写入的压缩后数据量
  - LOAD事件：用户导入的数据量
  - COMPACTION事件：compaction输出的数据量
- **可选验证字段**：`input_data_size`用于验证compaction是否有效压缩数据
- **最小化字段数量**：去除了profile、状态、算法等非必需字段

### 3.3 Union视图定义

**`INFORMATION_SCHEMA.tablet_write_log`** - 推荐用户使用的统一视图

```sql
CREATE VIEW information_schema.tablet_write_log AS
SELECT * FROM information_schema.be_tablet_write_log  -- 内存表（最新数据）
UNION ALL
SELECT * FROM _statistics_.tablet_write_log_history;  -- 持久化表（历史数据）
```

**去重机制**：
- FE Syncer只同步`log_time < NOW() - INTERVAL 1 MINUTE`的数据
- 内存表包含最近30分钟的数据
- 持久化表只包含1分钟前的数据
- 因此UNION ALL不会产生重复数据

### 3.4 内存数据结构：TabletWriteLogEntry

```cpp
namespace starrocks::lake {

enum class WriteEventType : uint8_t {
    LOAD = 0,        // 用户数据导入
    COMPACTION = 1   // Compaction合并
};

struct TabletWriteLogEntry {
    // 基础信息
    int64_t log_time;               // Unix时间戳（秒）
    WriteEventType event_type;

    // 标识信息
    int64_t cn_id;
    std::string db_name;
    std::string table_name;
    int64_t table_id;
    int64_t partition_id;
    int64_t tablet_id;

    // 写入量统计
    int64_t txn_id;
    int64_t version;
    int64_t num_rows;
    int64_t data_size;              // 压缩后大小 - WAF核心指标
    int32_t num_segments;

    // Compaction专用
    std::string input_versions;     // 格式："2,3,4,5"
    int64_t input_rows;
    int64_t input_data_size;        // 输入总大小，用于验证
};

} // namespace starrocks::lake
```

**内存占用估算**：
- 每条日志约 200-300 字节
- 假设高负载场景：110条/秒 × 1800秒 = 约20万条/30分钟
- 推荐缓冲区大小：10万条（约30分钟数据）
- 内存占用：10万条 × 300字节 ≈ 30MB per CN

## 4. 详细设计

### 4.1 TabletWriteLogManager 设计（BE组件）

```cpp
namespace starrocks::lake {

class TabletWriteLogManager {
public:
    static TabletWriteLogManager* instance();

    // 初始化
    Status init();

    // 停止
    void stop();

    // 记录数据导入事件
    void log_load(int64_t table_id,
                  int64_t partition_id,
                  int64_t tablet_id,
                  int64_t txn_id,
                  int64_t version,
                  int64_t num_rows,
                  int64_t data_size,
                  int32_t num_segments,
                  const std::string& db_name,
                  const std::string& table_name);

    // 记录compaction事件
    void log_compaction(int64_t table_id,
                        int64_t partition_id,
                        int64_t tablet_id,
                        int64_t txn_id,
                        int64_t output_version,
                        const std::vector<int64_t>& input_versions,
                        int64_t input_rows,
                        int64_t input_data_size,
                        int64_t output_rows,
                        int64_t output_data_size,
                        int32_t output_segments,
                        const std::string& db_name,
                        const std::string& table_name);

    // 查询日志（用于SchemaScanner）
    std::vector<TabletWriteLogEntry> query_logs(
        int64_t start_time = 0,
        int64_t end_time = INT64_MAX,
        int64_t table_id = -1,
        int64_t tablet_id = -1);

    // 获取日志条目数量
    size_t log_count() const;

private:
    TabletWriteLogManager();
    ~TabletWriteLogManager();

    DISALLOW_COPY_AND_MOVE(TabletWriteLogManager);

    // 内存缓冲区（环形缓冲区）
    std::deque<TabletWriteLogEntry> _log_buffer;
    mutable std::mutex _buffer_mutex;

    // 配置参数
    size_t _max_buffer_size = 100000;       // 最大缓存条目数（约30分钟数据）
    int64_t _retention_seconds = 1800;      // 保留30分钟

    // 内部方法
    void _evict_old_logs();                  // 清理过期内存日志
    int64_t _get_cn_id() const;              // 获取CN ID
};

} // namespace starrocks::lake
```

**设计要点**：
- **仅内存存储**：不负责持久化，数据只保存在内存中
- **环形缓冲区**：达到max_buffer_size后，自动淘汰最老的日志
- **线程安全**：使用mutex保护缓冲区
- **轻量级**：去除了flush相关的线程和SQL执行逻辑

### 4.2 日志记录点

#### 4.2.1 数据导入记录点
在 Lake DeltaWriter 完成时记录。参考位置：`be/src/storage/lake/delta_writer.cpp`

```cpp
// 在 lake::DeltaWriterImpl::finish_with_txnlog() 成功后记录
Status DeltaWriterImpl::finish_with_txnlog(...) {
    // ... 原有代码：生成 txn_log ...

    // 记录写入日志
    if (config::enable_tablet_write_log) {
        auto* op_write = txn_log->mutable_op_write();
        for (const auto& rowset : op_write->rowsets()) {
            TabletWriteLogManager::instance()->log_load(
                _table_id,
                _partition_id,
                _tablet_id,
                _txn_id,
                rowset.version(),           // 新version
                rowset.num_rows(),
                rowset.data_size(),         // 核心：压缩后大小
                rowset.segments_size(),
                _db_name,
                _table_name
            );
        }
    }

    return Status::OK();
}
```

#### 4.2.2 Compaction记录点
在 Lake CompactionTask 完成时记录。参考位置：`be/src/storage/lake/compaction_task.cpp`

```cpp
// 在 CompactionTask::execute() 成功后记录
Status CompactionTask::execute(...) {
    // ... 原有代码：执行compaction，生成 txn_log ...

    // 记录compaction日志
    if (config::enable_tablet_write_log && txn_log != nullptr) {
        auto* op_compaction = txn_log->mutable_op_compaction();

        // 收集输入version列表
        std::vector<int64_t> input_versions;
        int64_t input_rows = 0;
        int64_t input_data_size = 0;
        for (const auto& rowset : _input_rowsets) {
            input_versions.push_back(rowset->version());
            input_rows += rowset->num_rows();
            input_data_size += rowset->data_size();
        }

        // 记录日志
        TabletWriteLogManager::instance()->log_compaction(
            _tablet.table_id(),
            _tablet.partition_id(),
            _tablet.id(),
            _txn_id,
            op_compaction->output_rowset().version(),
            input_versions,
            input_rows,
            input_data_size,
            op_compaction->output_rowset().num_rows(),
            op_compaction->output_rowset().data_size(),  // 核心：输出大小
            op_compaction->output_rowset().segments_size(),
            _db_name,
            _table_name
        );
    }

    return Status::OK();
}
```

### 4.3 数据查询流程

**推荐查询方式**：使用Union视图

```
用户查询：SELECT * FROM INFORMATION_SCHEMA.tablet_write_log  -- Union视图
                WHERE table_id = 12345;
    │
    ▼
FE执行UNION ALL查询
    ├─► 子查询1: SELECT * FROM information_schema.be_tablet_write_log  -- 内存表
    │   │
    │   ├─► FE向各CN发送RPC请求
    │   │
    │   ├─► CN执行 SchemaBeTabletWriteLogScanner::get_next()
    │   │       └─► 调用 TabletWriteLogManager::query_logs()
    │   │       └─► 从内存缓冲区中筛选符合条件的日志
    │   │
    │   └─► FE聚合各CN返回的数据（最新数据）
    │
    └─► 子查询2: SELECT * FROM _statistics_.tablet_write_log_history  -- 持久化表
        │
        ├─► FE执行普通表扫描（带分区裁剪）
        │
        └─► 返回历史数据（1分钟前的数据）
    │
    ▼
FE合并两个子查询结果（UNION ALL，无重复）
    │
    ▼
返回给用户
```

**直接查询内存表**（实时数据，但CN重启后丢失）：

```sql
SELECT * FROM information_schema.be_tablet_write_log WHERE table_id = 12345;
```

**直接查询持久化表**（历史数据，持久化，但有1分钟延迟）：

```sql
SELECT * FROM _statistics_.tablet_write_log_history WHERE table_id = 12345;
```

### 4.4 数据持久化和同步（参考 loads_history）

采用**FE侧定期同步**方案，而非BE侧flush。

#### 4.4.1 持久化表Schema定义

**表名**：`_statistics_.tablet_write_log_history`（持久化表，用户可查询）

**建表DDL**（由TableKeeper自动创建）：
```sql
CREATE TABLE IF NOT EXISTS _statistics_.tablet_write_log_history (
    log_time DATETIME NOT NULL,
    event_type VARCHAR(20) NOT NULL,
    cn_id BIGINT NOT NULL,
    db_name VARCHAR(256),
    table_name VARCHAR(256),
    table_id BIGINT NOT NULL,
    partition_id BIGINT NOT NULL,
    tablet_id BIGINT NOT NULL,
    txn_id BIGINT NOT NULL,
    version BIGINT NOT NULL,
    num_rows BIGINT NOT NULL,
    data_size BIGINT NOT NULL,
    num_segments INT NOT NULL,
    input_versions VARCHAR(1024),
    input_rows BIGINT,
    input_data_size BIGINT
)
DUPLICATE KEY (log_time, event_type, cn_id, table_id, tablet_id)
PARTITION BY RANGE(log_time) ()
DISTRIBUTED BY HASH(tablet_id) BUCKETS 8
PROPERTIES (
    "replication_num" = "1",
    "storage_medium" = "SSD",
    "compression" = "LZ4",
    "enable_persistent_index" = "false",
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    "dynamic_partition.start" = "-7",      -- 保留7天历史数据
    "dynamic_partition.end" = "1",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "8"
);
```

**设计要点**：
- **分区策略**：按日期（log_time）动态分区，自动创建和删除分区
- **保留策略**：dynamic_partition保留最近7天数据（可配置）
- **副本数**：replication_num=1（统计表，无需高可用）
- **分桶**：按tablet_id分桶，WAF查询时常用过滤条件
- **压缩**：使用LZ4快速压缩
- **索引**：DUPLICATE KEY模型，无需持久化索引

#### 4.4.2 TabletWriteLogHistorySyncer（FE组件）

**类定义**：

```java
package com.starrocks.statistic;

public class TabletWriteLogHistorySyncer extends FrontendDaemon {
    private static final long SYNC_INTERVAL_MS = 60 * 1000; // 60秒
    private static final String MEMORY_TABLE = "information_schema.be_tablet_write_log";
    private static final String HISTORY_TABLE = "_statistics_.tablet_write_log_history";

    private long syncedLogTime = 0;  // 上次同步的最大log_time（Unix timestamp秒）

    public TabletWriteLogHistorySyncer() {
        super("tablet_write_log_history_syncer", SYNC_INTERVAL_MS);
    }

    @Override
    protected void runAfterCatalogReady() {
        // 1. 检查持久化表是否存在（由TableKeeper负责创建）
        if (!checkTableExists()) {
            LOG.warn("Persistent table {} does not exist, skip sync", HISTORY_TABLE);
            return;
        }

        // 2. 初始化syncedLogTime（首次运行）
        if (syncedLogTime == 0) {
            syncedLogTime = getMaxLogTimeFromHistory();
        }

        // 3. 构造同步SQL
        String syncSql = buildSyncSql();

        // 4. 执行同步
        try {
            executeSql(syncSql);

            // 5. 更新syncedLogTime
            syncedLogTime = getCurrentTimestamp() - 60; // NOW() - 1分钟

            LOG.info("Synced tablet write log history, new syncedLogTime: {}", syncedLogTime);
        } catch (Exception e) {
            LOG.warn("Failed to sync tablet write log history", e);
        }
    }

    private String buildSyncSql() {
        // 类似 loads_history 的同步逻辑
        return String.format(
            "INSERT INTO %s " +
            "SELECT * FROM %s " +
            "WHERE log_time < FROM_UNIXTIME(%d) " +  // 只同步1分钟前的数据
            "  AND log_time > FROM_UNIXTIME(%d)",    // 避免重复同步
            HISTORY_TABLE,
            MEMORY_TABLE,
            getCurrentTimestamp() - 60,  // NOW() - 1分钟
            syncedLogTime                 // 上次同步的时间点
        );
    }

    private long getMaxLogTimeFromHistory() {
        // SELECT MAX(log_time) FROM _statistics_.tablet_write_log_history
        // 如果为空返回0
        String sql = String.format(
            "SELECT COALESCE(MAX(UNIX_TIMESTAMP(log_time)), 0) FROM %s",
            HISTORY_TABLE
        );
        return executeSqlForLong(sql);
    }

    private boolean checkTableExists() {
        // 检查 _statistics_.tablet_write_log_history 是否存在
        return GlobalStateMgr.getCurrentState().getDb("_statistics_")
            .getTable("tablet_write_log_history") != null;
    }
}
```

**同步逻辑**：
1. 每60秒运行一次
2. 只同步`log_time < NOW() - INTERVAL 1 MINUTE`的数据（避免同步进行中的日志）
3. 只同步`log_time > syncedLogTime`的数据（避免重复同步）
4. 使用`INSERT INTO ... SELECT`批量同步，高效

**去重保证**：
- 内存表包含最近30分钟的数据
- 持久化表只包含1分钟前的数据
- UNION ALL不会产生重复数据

#### 4.4.3 TableKeeper集成

复用现有的`TableKeeper` daemon，添加tablet_write_log_history表的管理：

```java
// 在 TableKeeper.runAfterCatalogReady() 中添加
private void ensureTabletWriteLogHistoryTable() {
    Database db = GlobalStateMgr.getCurrentState().getDb("_statistics_");
    if (db == null) {
        return; // _statistics_ database不存在，跳过
    }

    Table table = db.getTable("tablet_write_log_history");
    if (table != null) {
        return; // 表已存在
    }

    // 创建持久化表
    String createTableSql = buildTabletWriteLogHistoryTableDDL();
    try {
        executeSql(createTableSql);
        LOG.info("Created tablet_write_log_history table");
    } catch (Exception e) {
        LOG.warn("Failed to create tablet_write_log_history table", e);
    }
}
```

#### 4.4.4 BE内存日志管理

BE端TabletWriteLogManager仅负责内存管理：

```cpp
void TabletWriteLogManager::_evict_old_logs() {
    std::lock_guard<std::mutex> lock(_buffer_mutex);

    int64_t now = time(nullptr);
    int64_t cutoff_time = now - _retention_seconds;  // 默认30分钟

    // 删除过期日志
    while (!_log_buffer.empty() && _log_buffer.front().log_time < cutoff_time) {
        _log_buffer.pop_front();
    }

    // 删除超过max_buffer_size的最老日志
    while (_log_buffer.size() > _max_buffer_size) {
        _log_buffer.pop_front();
    }
}
```

**清理触发时机**：
- 每次添加新日志时检查
- 定期后台清理（可选）
## 5. 写入放大系数计算

### 5.1 计算公式

**写入放大系数（Write Amplification Factor, WAF）**定义为：

```
WAF = (用户导入写入量 + Compaction写入量) / 用户导入写入量
    = 1 + (Compaction写入量 / 用户导入写入量)
```

- **用户导入写入量**：LOAD事件的 data_size 总和
- **Compaction写入量**：COMPACTION事件的 data_size 总和
- **WAF含义**：
  - WAF = 1.0：没有compaction（或compaction输出等于输入）
  - WAF = 2.0：compaction写入量等于用户导入量（放大1倍）
  - WAF = 3.0：compaction写入量是用户导入量的2倍（放大2倍）

### 5.2 SQL查询示例

#### 5.2.1 计算单表的写入放大系数（推荐）

```sql
-- 计算表test_table在过去24小时的写入放大系数
-- 使用Union视图，自动合并内存表和持久化表的数据
WITH
-- 用户导入的数据量
load_writes AS (
    SELECT
        table_id,
        table_name,
        SUM(data_size) as load_data_size,
        SUM(num_rows) as load_rows,
        COUNT(*) as load_count
    FROM information_schema.tablet_write_log  -- Union视图
    WHERE event_type = 'LOAD'
      AND table_name = 'test_table'
      AND log_time >= NOW() - INTERVAL 24 HOUR
    GROUP BY table_id, table_name
),
-- Compaction写入的数据量
compaction_writes AS (
    SELECT
        table_id,
        SUM(data_size) as compaction_data_size,
        SUM(num_rows) as compaction_output_rows,
        COUNT(*) as compaction_count
    FROM information_schema.tablet_write_log  -- Union视图
    WHERE event_type = 'COMPACTION'
      AND table_name = 'test_table'
      AND log_time >= NOW() - INTERVAL 24 HOUR
    GROUP BY table_id
)
SELECT
    l.table_id,
    l.table_name,
    l.load_data_size / 1024 / 1024 / 1024 as load_data_gb,
    COALESCE(c.compaction_data_size, 0) / 1024 / 1024 / 1024 as compaction_data_gb,
    (l.load_data_size + COALESCE(c.compaction_data_size, 0)) / 1024 / 1024 / 1024 as total_written_gb,
    1 + COALESCE(c.compaction_data_size, 0) / l.load_data_size as write_amplification_factor,
    l.load_count,
    COALESCE(c.compaction_count, 0) as compaction_count
FROM load_writes l
LEFT JOIN compaction_writes c ON l.table_id = c.table_id;
```

#### 5.2.2 按分区统计WAF

```sql
SELECT
    partition_id,
    SUM(CASE WHEN event_type = 'LOAD' THEN data_size ELSE 0 END) / 1024 / 1024 / 1024 as load_gb,
    SUM(CASE WHEN event_type = 'COMPACTION' THEN data_size ELSE 0 END) / 1024 / 1024 / 1024 as compaction_gb,
    1 + SUM(CASE WHEN event_type = 'COMPACTION' THEN data_size ELSE 0 END) /
        NULLIF(SUM(CASE WHEN event_type = 'LOAD' THEN data_size ELSE 0 END), 0) as waf
FROM information_schema.tablet_write_log  -- Union视图
WHERE table_name = 'test_table'
  AND log_time >= NOW() - INTERVAL 24 HOUR
GROUP BY partition_id
ORDER BY waf DESC;
```

#### 5.2.3 按tablet统计compaction写入量（Top 10）

```sql
SELECT
    tablet_id,
    COUNT(*) as compaction_count,
    SUM(input_data_size) / 1024 / 1024 as total_input_mb,
    SUM(data_size) / 1024 / 1024 as total_output_mb,
    SUM(data_size) / NULLIF(SUM(input_data_size), 0) as compression_ratio
FROM information_schema.tablet_write_log  -- Union视图
WHERE event_type = 'COMPACTION'
  AND table_name = 'test_table'
  AND log_time >= NOW() - INTERVAL 7 DAY
GROUP BY tablet_id
ORDER BY total_output_mb DESC
LIMIT 10;
```

#### 5.2.4 查看最近的compaction详情

```sql
SELECT
    log_time,
    cn_id,
    tablet_id,
    txn_id,
    version,
    input_versions,
    input_rows,
    num_rows,
    input_data_size / 1024 / 1024 as input_mb,
    data_size / 1024 / 1024 as output_mb,
    ROUND(data_size * 100.0 / NULLIF(input_data_size, 0), 2) as compression_pct
FROM information_schema.tablet_write_log  -- Union视图
WHERE event_type = 'COMPACTION'
  AND table_name = 'test_table'
  AND log_time >= NOW() - INTERVAL 1 DAY
ORDER BY log_time DESC
LIMIT 20;
```

#### 5.2.5 按小时统计WAF趋势

```sql
SELECT
    DATE_FORMAT(log_time, '%Y-%m-%d %H:00:00') as hour,
    SUM(CASE WHEN event_type = 'LOAD' THEN data_size ELSE 0 END) / 1024 / 1024 / 1024 as load_gb,
    SUM(CASE WHEN event_type = 'COMPACTION' THEN data_size ELSE 0 END) / 1024 / 1024 / 1024 as compaction_gb,
    1 + SUM(CASE WHEN event_type = 'COMPACTION' THEN data_size ELSE 0 END) /
        NULLIF(SUM(CASE WHEN event_type = 'LOAD' THEN data_size ELSE 0 END), 0) as waf
FROM information_schema.tablet_write_log  -- Union视图
WHERE table_name = 'test_table'
  AND log_time >= NOW() - INTERVAL 7 DAY
GROUP BY DATE_FORMAT(log_time, '%Y-%m-%d %H:00:00')
ORDER BY hour;
```

### 5.3 监控指标

基于tablet_write_log可以建立以下监控指标：

1. **实时WAF**（每小时/天）：用于识别写入放大异常
2. **Compaction频率**：单位时间内的compaction次数
3. **LOAD vs COMPACTION写入比例**：评估compaction效率
4. **单表/分区的数据写入量趋势**：容量规划
5. **Top N高WAF表/分区**：定位优化目标

## 6. 配置参数

### 6.1 BE/CN配置（cn.conf 或 be.conf）

```properties
# 是否启用tablet写入日志（Cloud Native模式专用）
enable_tablet_write_log = true

# 内存缓冲区最大条目数（建议值：100000）
tablet_write_log_max_buffer_size = 100000

# 内存日志保留时间（秒），默认30分钟（1800秒）
# 超过此时间的日志会被自动从内存中清理
tablet_write_log_retention_seconds = 1800
```

**配置说明**：
- **enable_tablet_write_log**：默认关闭，避免影响未使用该功能的用户
- **max_buffer_size**：10万条约占用30MB内存，保留30分钟数据（FE Syncer每60秒同步，30分钟提供充足的缓冲）
- **retention_seconds**：30分钟足够覆盖同步延迟和实时查询需求，历史数据已在持久化表中

### 6.2 FE配置（fe.conf）

```properties
# TabletWriteLogHistorySyncer同步间隔（毫秒），默认60秒
tablet_write_log_sync_interval_ms = 60000

# 持久化表动态分区保留天数，默认7天
tablet_write_log_history_retention_days = 7
```

**配置说明**：
- **sync_interval_ms**：FE Syncer同步间隔，默认60秒（参考loads_history）
- **history_retention_days**：持久化表保留天数，可根据存储空间和分析需求调整

## 7. 实现计划

### 7.1 Phase 1: 核心功能（MVP）
**目标**：实现基本的日志记录、持久化和查询功能，支持WAF计算

**BE/CN端实现**（C++）:
- [ ] 实现 `TabletWriteLogEntry` 数据结构（be/src/storage/lake/tablet_write_log_entry.h）
- [ ] 实现 `TabletWriteLogManager` 类（be/src/storage/lake/tablet_write_log_manager.h/cpp）
  - [ ] 内存缓冲区管理（环形缓冲区）
  - [ ] 日志记录接口（log_load, log_compaction）
  - [ ] 查询接口（query_logs）
  - [ ] 内存清理逻辑（_evict_old_logs）
- [ ] 在 `lake::DeltaWriter::finish_with_txnlog()` 中添加LOAD日志记录
- [ ] 在 `lake::CompactionTask::execute()` 中添加COMPACTION日志记录
- [ ] 实现 `SchemaBeTabletWriteLogScanner` 扫描器（be/src/exec/schema_scanner/）
- [ ] 添加BE配置参数支持（enable_tablet_write_log等）
- [ ] CN启动时初始化 TabletWriteLogManager

**FE端实现**（Java）:
- [ ] 实现 `BeTabletWriteLogSystemTable` 内存表（fe/fe-core/src/main/java/com/starrocks/catalog/system/information/）
  - [ ] 定义schema
  - [ ] 注册到 INFORMATION_SCHEMA
- [ ] 实现 `TabletWriteLogHistorySystemTable` 持久化表（fe/fe-core/src/main/java/com/starrocks/statistic/）
  - [ ] 定义schema（物理表，在_statistics_数据库）
  - [ ] TableKeeper集成（自动创建表）
- [ ] 实现 `TabletWriteLogUnionView` Union视图
  - [ ] CREATE VIEW定义
  - [ ] 注册到 INFORMATION_SCHEMA
- [ ] 实现 `TabletWriteLogHistorySyncer` 同步器（fe/fe-core/src/main/java/com/starrocks/statistic/）
  - [ ] 继承FrontendDaemon
  - [ ] 每60秒执行INSERT SELECT同步
  - [ ] 跟踪syncedLogTime避免重复
- [ ] 在GlobalStateMgr中注册Syncer daemon
- [ ] 添加FE配置参数支持（sync_interval_ms等）
- [ ] 添加必要的Thrift接口（BE扫描器通信）

**测试**:
- [ ] 单元测试：TabletWriteLogManager内存管理
- [ ] 单元测试：SchemaScanner数据扫描
- [ ] 集成测试：数据导入后验证内存表日志
- [ ] 集成测试：Compaction后验证日志
- [ ] 集成测试：FE Syncer同步到持久化表
- [ ] 集成测试：Union视图查询验证
- [ ] 集成测试：动态分区自动清理
- [ ] 端到端测试：SQL查询并计算WAF

**预计工作量**：3-4周

### 7.2 Phase 2: 优化和完善
**目标**：提升性能、稳定性和易用性

- [ ] 性能优化：
  - [ ] BE内存缓冲区优化（无锁数据结构）
  - [ ] FE Syncer批量写入优化
  - [ ] Union视图查询性能优化（查询改写）
- [ ] 查询优化：
  - [ ] 支持更多过滤条件下推（db_name、partition_id等）
  - [ ] 持久化表索引优化
- [ ] 监控完善：
  - [ ] 暴露Prometheus metrics（waf_gauge、log_count、sync_delay等）
  - [ ] 添加FE Syncer同步延迟监控
  - [ ] 添加内存表大小监控
  - [ ] 添加持久化表磁盘空间监控
- [ ] 错误处理：
  - [ ] FE Syncer失败时的重试机制
  - [ ] 持久化表写入失败时的告警
  - [ ] 内存缓冲区满时的降级策略
- [ ] 文档完善：用户手册、最佳实践、故障排查、WAF分析指南

**预计工作量**：2-3周

### 7.3 未来扩展（Phase 3+）

1. **聚合视图**：创建物化视图或定期聚合的摘要表
2. **历史数据导出**：支持导出到外部存储（S3/HDFS）进行长期分析
3. **自动化告警**：WAF超过阈值时自动告警
4. **优化建议**：基于WAF数据自动给出compaction策略建议

## 8. 性能考量

### 8.1 内存开销（BE/CN端）

**每条日志条目大小估算**：
- 内存中C++对象（TabletWriteLogEntry）：约250-300字节/条

**缓冲区内存开销**：
- 高负载场景：110条/秒 × 1800秒 = 约20万条/30分钟
- 推荐配置：10万条缓冲区
- 内存占用：10万条 × 300字节 ≈ 30MB per CN
- **结论**：内存开销很小，对CN影响可忽略

### 8.2 磁盘开销（FE持久化表）

**每天日志数据大小估算**（以高负载场景为例）：
- 假设每秒100次LOAD + 10次COMPACTION = 110条日志/秒
- 每天 = 110 × 86400 ≈ 950万条
- 持久化表磁盘占用（LZ4压缩后）：
  - 原始数据：950万 × 300字节 ≈ 2.85GB/天
  - LZ4压缩后：约1.4-1.7GB/天（压缩比约50%）
- 保留7天（默认）= 约10-12GB

**优势**：
- StarRocks自动压缩（LZ4），无需额外配置
- 列式存储，压缩效率更高
- 动态分区自动清理过期数据

### 8.3 性能影响

#### 8.3.1 日志记录（写入路径 - BE端）

**数据导入**：
- 在finish阶段记录日志，仅增加内存写入
- 不触发任何I/O或RPC
- 耗时 <0.1ms，影响可忽略

**Compaction**：
- 在完成阶段记录，不在关键路径上
- 同样是内存操作
- 影响可忽略

#### 8.3.2 FE Syncer同步（FE端）

**同步操作**（TabletWriteLogHistorySyncer）：
- 每60秒运行一次
- 执行INSERT SELECT批量同步
- 在FE后台线程执行，不影响BE/CN

**预期同步耗时**：
- 假设每分钟新增6,600条日志（110条/秒 × 60秒）
- INSERT SELECT执行：< 1秒（StarRocks批量写入优化）
- **总计**：< 1秒，完全在FE后台完成，不影响BE

**优化**：
- 批量INSERT减少事务开销
- 利用StarRocks查询优化器
- 可选：使用Stream Load API进一步提升性能

#### 8.3.3 查询性能

**内存表查询**（information_schema.be_tablet_write_log）：
- 通过RPC扫描所有CN的内存缓冲区
- 查询速度快（纯内存）
- 适合查询最近数据（< 30分钟）

**持久化表查询**（_statistics_.tablet_write_log_history）：
- 利用分区裁剪，只扫描相关分区
- 列式存储，扫描效率高
- 适合查询历史数据（> 1分钟前）

**Union视图查询**（推荐）：
- 自动合并内存表和持久化表
- 优化器可能并行执行两个子查询
- 整体查询性能良好

### 8.4 整体性能影响总结

**预期性能影响**：
- **导入吞吐量**：< 0.1% 影响（仅内存写入）
- **Compaction吞吐量**：< 0.1% 影响（仅内存写入）
- **BE/CN内存增加**：约30MB per CN（保留30分钟数据）
- **FE持久化表磁盘空间**：约10-12GB（保留7天，LZ4压缩）
- **FE Syncer开销**：每60秒约1秒（完全在FE后台，不影响BE）
- **查询性能**：毫秒级（内存表）到秒级（持久化表大范围查询）

**双表+FE Syncer方案的优势**：
- ✅ **BE轻量化**：BE只负责内存管理，无I/O和SQL执行开销
- ✅ **无需序列化**：FE端直接SQL INSERT/SELECT，简化BE实现
- ✅ **自动压缩**：持久化表使用LZ4压缩，磁盘占用小
- ✅ **自动管理**：动态分区自动删除过期数据，无需手动清理
- ✅ **高可用**：FE Syncer失败不影响BE，可自动重试
- ✅ **灵活查询**：用户可选择查询内存表、持久化表或Union视图
- ✅ **参考成熟实现**：loads_history已验证该架构的稳定性

### 8.5 线程安全

**BE端**：
- 使用 `std::mutex` 保护 `_log_buffer`
- 日志记录操作使用 `std::lock_guard`，锁粒度小（仅内存操作）
- SchemaScanner查询时使用共享锁或快照读取
- 不会引入死锁风险

**FE端**：
- TabletWriteLogHistorySyncer运行在独立daemon线程
- 使用StarRocks内部事务机制保证INSERT SELECT的原子性
- 多个FE节点中只有Leader FE执行Syncer

## 9. 兼容性和向后兼容

1. **配置默认关闭**：`enable_tablet_write_log = false`，避免影响现有部署
2. **系统表始终存在**：即使功能关闭，系统表也存在，只是返回空结果
3. **仅Cloud Native模式**：功能仅在存算分离模式下可用，本地存储模式不受影响
4. **版本兼容**：FE和CN需要同时升级才能使用该功能

## 10. 测试计划

### 10.1 单元测试
- [ ] TabletWriteLogManager 日志记录测试
- [ ] TabletWriteLogManager 查询过滤测试
- [ ] 缓冲区溢出和清理测试
- [ ] 线程安全测试（并发写入）

### 10.2 集成测试
- [ ] Stream Load后查询日志验证
- [ ] Broker Load后查询日志验证
- [ ] Compaction完成后查询日志验证
- [ ] 长时间运行测试（内存稳定性）

### 10.3 性能测试
- [ ] 日志记录对导入吞吐量的影响（< 0.1%）
- [ ] 日志记录对compaction性能的影响（< 0.1%）
- [ ] 系统表查询性能测试

### 10.4 端到端测试
- [ ] 完整WAF计算流程验证
- [ ] 多表并发导入场景
- [ ] 高频compaction场景

## 11. 文档

需要编写的文档：

1. **用户文档**（中英文）
   - 功能介绍和使用场景
   - 配置参数说明
   - 系统表schema详解
   - WAF计算SQL示例
   - 最佳实践

2. **开发文档**
   - 架构设计
   - 代码结构
   - 添加新字段的指南

## 12. 风险和限制

### 12.1 风险
1. **BE内存使用**：高频导入/compaction场景下，可能导致缓冲区频繁溢出
   - **缓解**：使用环形缓冲区，自动淘汰旧记录
2. **FE Syncer延迟**：FE故障或过载时，可能导致持久化表数据延迟
   - **缓解**：内存表仍可查询最新数据；Syncer恢复后自动补齐
3. **性能影响**：虽然预期影响 < 0.1%，但仍需实测验证
   - **缓解**：提供开关可随时关闭功能

### 12.2 限制
1. **内存表数据有限**：BE内存仅保留30分钟，CN重启后丢失
   - **缓解**：持久化表保留7天历史数据（可配置），30分钟足够覆盖FE同步延迟和短期查询
2. **持久化延迟**：持久化表有1分钟延迟（FE Syncer间隔）
   - **权衡**：查询Union视图可获取最新数据（包含内存表）
3. **仅Cloud Native模式**：本地存储模式不支持（设计选择）
4. **单CN数据**：内存表查询需聚合所有CN（通过RPC）
5. **持久化表存储**：占用FE集群磁盘空间（约10-12GB，保留7天）
6. **FE Leader依赖**：只有Leader FE执行Syncer，Follower FE故障转移时需重新初始化

## 13. 参考资料

1. ClickHouse system.part_log：https://clickhouse.com/docs/operations/system-tables/part_log
2. RocksDB Write Amplification：https://github.com/facebook/rocksdb/wiki/Write-Amplification
3. StarRocks存算分离Compaction：`docs/en/administration/management/compaction.md`
4. **StarRocks loads_history实现**（本设计的参考模式）：
   - `fe/fe-core/src/main/java/com/starrocks/load/loadv2/LoadsHistorySyncer.java` - FE定期同步器
   - `fe/fe-core/src/main/java/com/starrocks/scheduler/history/TableKeeper.java` - 表管理和创建
   - `fe/fe-core/src/main/java/com/starrocks/catalog/system/information/LoadsSystemTable.java` - 内存表
   - 双表架构：`information_schema.loads` (内存) + `_statistics_.loads_history` (持久化)

## 14. 总结

本设计文档提出了一个**轻量级的写入放大追踪系统**，专门用于StarRocks存算分离模式：

**核心特性**：
- ✅ 简洁的Schema设计（16个字段，专注WAF计算）
- ✅ **双表架构**：内存表 + 持久化表 + Union视图
- ✅ **数据持久化**：FE定期同步到持久化表，数据不丢失
- ✅ **BE轻量化**：BE仅负责内存管理（保留30分钟），无I/O开销
- ✅ 性能影响极小（< 0.1%，仅内存写入）
- ✅ 内存占用很小（约30MB per CN，保留30分钟数据）
- ✅ 持久化表存储（约10-12GB，保留7天，LZ4压缩）
- ✅ 灵活查询（内存表/持久化表/Union视图）

**双表+FE Syncer架构的优势**（参考 loads_history）：
- **职责分离**：BE负责采集，FE负责持久化和同步
- **简化BE实现**：无需SQL执行、无需序列化、无需I/O
- **自动压缩**：持久化表使用LZ4压缩，磁盘占用小
- **自动管理**：动态分区自动删除过期数据
- **高可用**：FE Syncer失败不影响BE，可自动重试恢复
- **灵活查询**：
  - 内存表：实时性高，适合查询最近数据
  - 持久化表：可靠持久，适合历史数据分析
  - Union视图：自动合并，推荐用户使用
- **参考成熟实现**：loads_history已验证该架构的稳定性和可靠性

**实现策略**：
- Phase 1（3-4周）：实现核心MVP功能
  - BE：内存管理 + SchemaScanner
  - FE：内存表 + 持久化表 + Union视图 + Syncer
- Phase 2（2-3周）：性能优化、监控集成、错误处理
- Phase 3+：扩展功能（聚合视图、自动告警、优化建议等）

该方案通过**采用FE侧定期同步的双表架构**，简化了BE实现，同时提供了可靠的数据持久化和灵活的查询方式，是追踪写入放大的最佳实践方案。
