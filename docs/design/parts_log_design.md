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

```
┌─────────────────────────────────────────────────────────────┐
│                        Frontend (FE)                         │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  INFORMATION_SCHEMA.be_tablet_write_log                │ │
│  │  - 聚合各CN的tablet写入日志                             │ │
│  │  - 提供SQL查询接口用于WAF计算                           │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │ RPC
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Compute Node (CN/BE)                      │
│  ┌────────────────────────────────────────────────────────┐ │
│  │           TabletWriteLogManager                         │ │
│  │  - 管理内存日志缓冲区（环形缓冲区）                      │ │
│  │  - 提供查询接口给FE                                     │ │
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

#### 2.2.1 TabletWriteLogEntry（日志条目）
记录单个tablet的数据写入或compaction事件，包含关键指标用于WAF计算。

#### 2.2.2 TabletWriteLogManager（日志管理器）
- 维护内存中的日志缓冲区（环形缓冲区，固定大小）
- 提供日志查询接口
- 自动清理过期日志（基于时间窗口）
- 线程安全

#### 2.2.3 SchemaBeTabletWriteLogScanner（系统表扫描器）
- 扫描CN本地的写入日志
- 返回日志条目给FE

#### 2.2.4 BeTabletWriteLogSystemTable（FE系统表）
- 定义`INFORMATION_SCHEMA.be_tablet_write_log`系统表
- 聚合所有CN的日志数据

## 3. Schema 设计

### 3.1 系统表Schema：`INFORMATION_SCHEMA.be_tablet_write_log`

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

### 3.2 内存数据结构：TabletWriteLogEntry

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
- 10万条日志约 20-30 MB
- 推荐缓冲区大小：10万条（保留约1-2天的数据）

## 4. 详细设计

### 4.1 TabletWriteLogManager 设计

```cpp
namespace starrocks::lake {

class TabletWriteLogManager {
public:
    static TabletWriteLogManager* instance();

    // 初始化（从磁盘加载历史日志）
    Status init();

    // 停止（flush所有未写入的日志）
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

    // 查询日志（用于系统表）
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

    // 内存缓冲区
    std::deque<TabletWriteLogEntry> _log_buffer;        // 所有加载到内存的日志
    std::deque<TabletWriteLogEntry> _unflushed_logs;    // 未flush到磁盘的日志
    mutable std::mutex _buffer_mutex;

    // 配置参数
    size_t _max_buffer_size = 100000;       // 最大缓存条目数
    int64_t _retention_seconds = 172800;     // 保留2天（48小时）
    int64_t _flush_interval_seconds = 300;   // Flush间隔（5分钟）
    size_t _flush_threshold = 10000;         // 未flush日志达到此数量时触发flush

    // 持久化相关
    std::string _log_dir;                    // 日志目录路径
    int64_t _last_flush_time = 0;            // 上次flush时间
    std::atomic<bool> _stopped{false};       // 停止标志
    std::thread _flush_thread;               // 后台flush线程
    std::thread _cleanup_thread;             // 后台清理线程

    // 内部方法
    void _evict_old_logs();                  // 清理过期内存日志
    int64_t _get_cn_id() const;              // 获取CN ID

    // 持久化方法
    void _flush_to_disk();                   // Flush到磁盘
    void _load_from_disk();                  // 从磁盘加载
    void _cleanup_old_files();               // 清理过期文件
    void _flush_thread_func();               // Flush线程函数
    void _cleanup_thread_func();             // 清理线程函数

    // 文件操作
    std::string _get_log_file_path(const std::string& date) const;
    void _append_to_file(const std::string& file_path,
                         const std::vector<TabletWriteLogEntry>& entries);
    std::vector<TabletWriteLogEntry> _read_from_file(const std::string& file_path);
    std::vector<std::string> _list_log_files();
    std::string _extract_date_from_filename(const std::string& file_path);
    int64_t _parse_date_to_timestamp(const std::string& date);
};

} // namespace starrocks::lake
```

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

```
用户查询：SELECT * FROM INFORMATION_SCHEMA.be_tablet_write_log
                WHERE table_id = 12345;
    │
    ▼
FE解析查询，向各CN发送RPC请求
    │
    ▼
CN执行 SchemaBeTabletWriteLogScanner::get_next()
    │
    ├─► 调用 TabletWriteLogManager::query_logs()
    │
    ├─► 从内存缓冲区中筛选符合条件的日志
    │
    └─► 返回日志条目
    │
    ▼
FE聚合各CN返回的数据
    │
    ▼
返回给用户
```

### 4.4 日志生命周期管理

**持久化存储**：
- **本地磁盘持久化**：每个CN将日志持久化到本地磁盘
- **CN重启后恢复**：启动时从磁盘加载历史日志到内存
- **内存 + 磁盘双层存储**：
  - 内存缓冲区：最近的日志，提供快速查询
  - 磁盘文件：完整的历史日志，保证数据不丢失

#### 4.4.1 存储位置和格式

**存储路径**：
```
${STORAGE_ROOT_PATH}/tablet_write_log/
├── write_log_20250119.bin    # 每天一个文件
├── write_log_20250120.bin
└── write_log_20250121.bin
```

**文件格式**：
- 使用 **Protocol Buffers** 二进制格式，紧凑高效
- 文件命名：`write_log_YYYYMMDD.bin`
- 每个文件包含一天的日志记录

**Protobuf定义**：
```protobuf
message TabletWriteLogEntryPB {
    int64 log_time = 1;
    string event_type = 2;  // "LOAD" or "COMPACTION"

    int64 cn_id = 3;
    string db_name = 4;
    string table_name = 5;
    int64 table_id = 6;
    int64 partition_id = 7;
    int64 tablet_id = 8;

    int64 txn_id = 9;
    int64 version = 10;
    int64 num_rows = 11;
    int64 data_size = 12;
    int32 num_segments = 13;

    string input_versions = 14;  // 仅COMPACTION
    int64 input_rows = 15;       // 仅COMPACTION
    int64 input_data_size = 16;  // 仅COMPACTION
}

message TabletWriteLogFilePB {
    repeated TabletWriteLogEntryPB entries = 1;
}
```

#### 4.4.2 写入策略（Flush到磁盘）

**Flush触发条件**（满足任一即触发）：
1. **定期Flush**：每隔5分钟自动flush
2. **缓冲区阈值**：内存中未flush的记录数 ≥ 10,000条
3. **CN关闭时**：正常关闭时强制flush所有未写入的日志

**Flush实现**：
```cpp
void TabletWriteLogManager::_flush_to_disk() {
    std::lock_guard<std::mutex> lock(_buffer_mutex);

    // 按日期分组日志
    std::map<std::string, std::vector<TabletWriteLogEntry>> logs_by_date;
    for (auto& entry : _unflushed_logs) {
        std::string date = format_date(entry.log_time);  // YYYYMMDD
        logs_by_date[date].push_back(entry);
    }

    // 写入对应日期的文件（追加模式）
    for (auto& [date, entries] : logs_by_date) {
        std::string file_path = _get_log_file_path(date);
        _append_to_file(file_path, entries);
    }

    _unflushed_logs.clear();
    _last_flush_time = time(nullptr);
}
```

**后台Flush线程**：
```cpp
void TabletWriteLogManager::_flush_thread_func() {
    while (!_stopped) {
        std::this_thread::sleep_for(std::chrono::seconds(300));  // 5分钟
        _flush_to_disk();
    }
}
```

#### 4.4.3 加载策略（CN启动时）

**启动时加载**：
```cpp
void TabletWriteLogManager::_load_from_disk() {
    // 只加载最近 retention_seconds 内的日志文件
    int64_t now = time(nullptr);
    int64_t cutoff_time = now - _retention_seconds;

    std::vector<std::string> log_files = _list_log_files();
    for (const auto& file_path : log_files) {
        // 解析文件名获取日期
        std::string date = _extract_date_from_filename(file_path);
        int64_t file_time = _parse_date_to_timestamp(date);

        if (file_time >= cutoff_time) {
            // 加载文件到内存
            auto entries = _read_from_file(file_path);
            for (auto& entry : entries) {
                if (entry.log_time >= cutoff_time) {
                    _log_buffer.push_back(std::move(entry));
                }
            }
        }
    }

    // 按时间排序
    std::sort(_log_buffer.begin(), _log_buffer.end(),
              [](const auto& a, const auto& b) { return a.log_time < b.log_time; });
}
```

#### 4.4.4 清理策略（删除过期文件）

**定期清理**：
- 每天凌晨2点自动清理
- 删除超过 `retention_seconds` 的日志文件

```cpp
void TabletWriteLogManager::_cleanup_old_files() {
    int64_t now = time(nullptr);
    int64_t cutoff_time = now - _retention_seconds;

    std::vector<std::string> log_files = _list_log_files();
    for (const auto& file_path : log_files) {
        std::string date = _extract_date_from_filename(file_path);
        int64_t file_time = _parse_date_to_timestamp(date);

        if (file_time < cutoff_time) {
            std::filesystem::remove(file_path);
            LOG(INFO) << "Removed old tablet write log file: " << file_path;
        }
    }
}

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
WITH
-- 用户导入的数据量
load_writes AS (
    SELECT
        table_id,
        table_name,
        SUM(data_size) as load_data_size,
        SUM(num_rows) as load_rows,
        COUNT(*) as load_count
    FROM information_schema.be_tablet_write_log
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
    FROM information_schema.be_tablet_write_log
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
FROM information_schema.be_tablet_write_log
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
FROM information_schema.be_tablet_write_log
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
FROM information_schema.be_tablet_write_log
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
FROM information_schema.be_tablet_write_log
WHERE table_name = 'test_table'
  AND log_time >= NOW() - INTERVAL 7 DAY
GROUP BY DATE_FORMAT(log_time, '%Y-%m-%d %H:00:00')
ORDER BY hour;
```

### 5.3 监控指标

基于be_tablet_write_log可以建立以下监控指标：

1. **实时WAF**（每小时/天）：用于识别写入放大异常
2. **Compaction频率**：单位时间内的compaction次数
3. **LOAD vs COMPACTION写入比例**：评估compaction效率
4. **单表/分区的数据写入量趋势**：容量规划
5. **Top N高WAF表/分区**：定位优化目标

## 6. 配置参数

在 `cn.conf`（或 `be.conf`）中添加配置项：

```properties
# 是否启用tablet写入日志（Cloud Native模式专用）
enable_tablet_write_log = true

# 内存缓冲区最大条目数（建议值：100000）
tablet_write_log_max_buffer_size = 100000

# 日志保留时间（秒），默认2天（172800秒）
# 超过此时间的日志会被自动清理（内存和磁盘）
tablet_write_log_retention_seconds = 172800

# Flush到磁盘的时间间隔（秒），默认5分钟
tablet_write_log_flush_interval_seconds = 300

# Flush到磁盘的阈值（未flush的日志条目数），默认10000条
# 当未flush的日志达到此数量时，会立即触发flush
tablet_write_log_flush_threshold = 10000

# 日志存储目录（可选，默认为 ${STORAGE_ROOT_PATH}/tablet_write_log）
# tablet_write_log_dir = /path/to/log/dir
```

**配置说明**：
- **enable_tablet_write_log**：默认关闭，避免影响未使用该功能的用户
- **max_buffer_size**：10万条约占用20-30MB内存，可根据CN内存大小调整
- **retention_seconds**：2天足够用于大多数WAF分析场景，可根据需要调整
- **flush_interval_seconds**：5分钟可以平衡数据持久性和性能
- **flush_threshold**：避免内存中堆积过多未flush的日志
- **log_dir**：可自定义日志存储目录，默认使用STORAGE_ROOT_PATH

## 7. 实现计划

### 7.1 Phase 1: 核心功能（MVP）
**目标**：实现基本的日志记录、持久化和查询功能，支持WAF计算

**CN/BE端实现**（C++）:
- [ ] 定义 `TabletWriteLogEntryPB` Protobuf消息
- [ ] 实现 `TabletWriteLogEntry` 数据结构
- [ ] 实现 `TabletWriteLogManager` 类
  - [ ] 内存缓冲区管理
  - [ ] 日志记录接口（log_load, log_compaction）
  - [ ] 持久化：flush到磁盘（Protobuf格式）
  - [ ] 启动加载：从磁盘恢复历史日志
  - [ ] 后台线程：定期flush和清理
- [ ] 在 `lake::DeltaWriter::finish_with_txnlog()` 中添加LOAD日志记录
- [ ] 在 `lake::CompactionTask::execute()` 中添加COMPACTION日志记录
- [ ] 实现 `SchemaBeTabletWriteLogScanner` 扫描器
- [ ] 添加配置参数支持（所有tablet_write_log_*参数）
- [ ] CN启动时初始化 TabletWriteLogManager
- [ ] CN关闭时graceful shutdown（flush所有日志）

**FE端实现**（Java）:
- [ ] 实现 `BeTabletWriteLogSystemTable` 系统表定义
- [ ] 注册系统表到 INFORMATION_SCHEMA
- [ ] 添加必要的Thrift接口

**测试**:
- [ ] 单元测试：TabletWriteLogManager基本功能
- [ ] 单元测试：Protobuf序列化/反序列化
- [ ] 单元测试：磁盘读写和文件管理
- [ ] 集成测试：数据导入后验证日志（内存+磁盘）
- [ ] 集成测试：Compaction后验证日志
- [ ] 集成测试：CN重启后日志恢复
- [ ] 端到端测试：SQL查询并计算WAF

**预计工作量**：3-4周

### 7.2 Phase 2: 优化和完善
**目标**：提升性能、稳定性和易用性

- [ ] 性能优化：
  - [ ] 异步flush，避免阻塞主流程
  - [ ] 批量序列化优化
  - [ ] 内存池优化
- [ ] 查询优化：支持更多过滤条件（db_name、partition_id等）
- [ ] 监控完善：
  - [ ] 暴露Prometheus metrics（waf_gauge、log_count等）
  - [ ] 添加日志flush延迟监控
  - [ ] 添加磁盘空间使用监控
- [ ] 错误处理：
  - [ ] 磁盘写入失败时的重试机制
  - [ ] 磁盘空间不足时的告警和降级
- [ ] 文档完善：用户手册、最佳实践、故障排查

**预计工作量**：2-3周

### 7.3 未来扩展（Phase 3+）

1. **聚合视图**：创建物化视图或定期聚合的摘要表
2. **历史数据导出**：支持导出到外部存储（S3/HDFS）进行长期分析
3. **自动化告警**：WAF超过阈值时自动告警
4. **优化建议**：基于WAF数据自动给出compaction策略建议

## 8. 性能考量

### 8.1 内存开销

**每条日志条目大小估算**：
- Protobuf序列化后：约200-250字节/条（带压缩）
- 内存中C++对象：约250-300字节/条

**缓冲区内存开销**：
- 100,000条 × 300字节 ≈ 30MB
- **结论**：内存开销很小，对CN影响可忽略

### 8.2 磁盘开销

**每天日志文件大小估算**（以高负载场景为例）：
- 假设每秒100次LOAD + 10次COMPACTION = 110条日志/秒
- 每天 = 110 × 86400 ≈ 950万条
- 磁盘占用 = 950万 × 250字节 ≈ 2.4GB/天（Protobuf压缩后）
- 保留2天 = 约5GB

**优化**：
- Protobuf天然支持高效序列化
- 可选择性开启压缩（gzip）进一步减少50%空间

### 8.3 性能影响

#### 8.3.1 日志记录（写入路径）

**数据导入**：
- 在finish阶段记录日志，仅增加内存写入
- 不触发磁盘I/O（由后台线程异步flush）
- 耗时 <0.1ms，影响可忽略

**Compaction**：
- 在完成阶段记录，不在关键路径上
- 同样是内存操作，后台异步flush
- 影响可忽略

#### 8.3.2 后台Flush（持久化）

**Flush操作**：
- 每5分钟一次，或10,000条未flush日志时触发
- 使用后台线程，不阻塞主流程
- Protobuf序列化性能：约10万条/秒
- 磁盘顺序写入，性能很高

**预期Flush耗时**：
- 10,000条 × 250字节 = 2.5MB
- 序列化时间：< 100ms
- 磁盘写入时间：< 50ms（SSD）
- **总计**：< 150ms，完全在后台完成

#### 8.3.3 启动加载

**CN启动时加载**：
- 只加载最近2天的日志文件
- 假设2天 × 2.4GB = 4.8GB
- SSD顺序读取：约500MB/s → 加载时间 < 10秒
- Protobuf反序列化：约5万条/秒 → 950万条需约3分钟

**优化策略**：
- 可选：启动时只加载最近6小时的热数据
- 按需加载：查询时如果需要更早的数据再加载

### 8.4 整体性能影响总结

**预期性能影响**：
- **导入吞吐量**：< 0.1% 影响（仅内存写入）
- **Compaction吞吐量**：< 0.1% 影响（仅内存写入）
- **内存增加**：约30MB per CN
- **磁盘空间**：约5GB per CN（保留2天）
- **CN启动时间**：增加10秒 - 3分钟（取决于日志量和加载策略）

### 8.5 线程安全

- 使用 `std::mutex` 保护 `_log_buffer` 和 `_unflushed_logs`
- 日志记录操作使用 `std::lock_guard`，锁粒度小（仅内存操作）
- 后台flush线程和清理线程与主流程独立
- 不会引入死锁风险

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
1. **内存使用**：高频导入/compaction场景下，可能导致缓冲区频繁溢出
   - **缓解**：使用环形缓冲区，自动淘汰旧记录
2. **性能影响**：虽然预期影响 < 0.1%，但仍需实测验证
   - **缓解**：提供开关可随时关闭功能

### 12.2 限制
1. **历史数据有限**：默认只保留2天，长期分析需定期导出到外部存储
2. **仅Cloud Native模式**：本地存储模式不支持（设计选择）
3. **单CN数据**：每个CN独立存储自己的日志，查询时需聚合所有CN
4. **启动时间增加**：CN启动时需要加载历史日志，可能增加10秒-3分钟启动时间

## 13. 参考资料

1. ClickHouse system.part_log：https://clickhouse.com/docs/operations/system-tables/part_log
2. RocksDB Write Amplification：https://github.com/facebook/rocksdb/wiki/Write-Amplification
3. StarRocks存算分离Compaction：`docs/en/administration/management/compaction.md`

## 14. 总结

本设计文档提出了一个**轻量级的写入放大追踪系统**，专门用于StarRocks存算分离模式：

**核心特性**：
- ✅ 简洁的Schema设计（16个字段）
- ✅ **持久化存储**：本地磁盘持久化，CN重启不丢失
- ✅ **双层存储**：内存缓冲区 + 磁盘文件，兼顾性能和可靠性
- ✅ 性能影响极小（< 0.1%）
- ✅ 内存占用可控（30MB）
- ✅ 磁盘占用可控（约5GB/CN，保留2天）
- ✅ 专注WAF计算，易于使用

**持久化方案**：
- **存储格式**：Protobuf二进制，高效紧凑
- **写入策略**：后台异步flush，不阻塞主流程
- **加载策略**：CN启动时自动恢复历史数据
- **清理策略**：自动删除过期文件

**实现策略**：
- Phase 1（3-4周）：实现核心MVP功能（包括持久化）
- Phase 2（2-3周）：性能优化、监控集成、错误处理
- Phase 3+：扩展功能（聚合视图、自动告警等）

该方案专注于解决用户的实际需求——**追踪写入放大并支持持久化**，在保证数据可靠性的同时，保持了简单高效的设计。
