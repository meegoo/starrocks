# StarRocks Parts Log 设计文档

## 1. 背景和目标

### 1.1 背景
在生产环境中，我们需要精确追踪表的数据写入情况和compaction行为，以便：
- 监控数据导入的实际写入量
- 追踪compaction过程中的数据读写
- 计算compaction的写入放大系数（Write Amplification Factor）
- 优化存储引擎性能和compaction策略

### 1.2 目标
实现类似ClickHouse `system.part_log` 的功能，记录：
1. **数据导入事件**：记录每次数据导入产生的rowset信息（行数、字节数、压缩后大小等）
2. **Compaction事件**：记录compaction的输入输出rowset信息
3. **写入放大统计**：基于日志数据计算写入放大系数

### 1.3 ClickHouse parts_log 参考
ClickHouse的`system.part_log`记录MergeTree表的data part事件，包括：
- 事件类型：NewPart（新数据写入）、MergeParts（合并完成）、DownloadPart等
- 数据part元数据：part名称、分区ID、路径
- 大小信息：行数、压缩字节数、未压缩字节数
- 合并信息：合并原因、合并算法、源part列表
- 性能指标：执行时间、内存使用

## 2. 总体设计

### 2.1 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                        Frontend (FE)                         │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  INFORMATION_SCHEMA.be_parts_log (System Table)        │ │
│  │  - 聚合各BE的parts_log数据                              │ │
│  │  - 提供SQL查询接口                                      │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │ RPC
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                        Backend (BE)                          │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              PartsLogManager                            │ │
│  │  - 管理parts_log内存缓冲区                              │ │
│  │  - 定期flush到磁盘                                      │ │
│  │  - 提供查询接口                                         │ │
│  └────────────────────────────────────────────────────────┘ │
│              ▲                          ▲                    │
│              │                          │                    │
│  ┌───────────┴──────────┐   ┌──────────┴──────────────┐    │
│  │   DeltaWriter         │   │  CompactionTask         │    │
│  │  (数据导入)           │   │  (Compaction)           │    │
│  │  - commit时记录日志   │   │  - 完成时记录日志        │    │
│  └───────────────────────┘   └─────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 PartsLogEntry（日志条目）
记录单个rowset的创建或compaction事件。

#### 2.2.2 PartsLogManager（日志管理器）
- 维护内存中的日志缓冲区
- 定期将日志flush到磁盘
- 提供日志查询接口
- 自动清理过期日志

#### 2.2.3 SchemaBePartsLogScanner（系统表扫描器）
- 扫描BE本地的parts_log
- 返回日志条目给FE

#### 2.2.4 BePartsLogSystemTable（FE系统表）
- 定义`INFORMATION_SCHEMA.be_parts_log`系统表
- 聚合所有BE的日志数据

## 3. Schema 设计

### 3.1 系统表Schema：`INFORMATION_SCHEMA.be_parts_log`

| 列名 | 类型 | 说明 |
|------|------|------|
| **基础信息** | | |
| `event_time` | DATETIME | 事件发生时间（微秒精度） |
| `event_type` | VARCHAR | 事件类型：NEW_ROWSET, COMPACTION_START, COMPACTION_FINISH |
| `duration_ms` | BIGINT | 事件持续时间（毫秒），对于NEW_ROWSET是写入耗时 |
| **标识信息** | | |
| `be_id` | BIGINT | Backend ID |
| `database` | VARCHAR | 数据库名 |
| `table` | VARCHAR | 表名 |
| `table_id` | BIGINT | 表ID |
| `partition_id` | BIGINT | 分区ID |
| `tablet_id` | BIGINT | Tablet ID |
| `txn_id` | BIGINT | 事务ID（仅NEW_ROWSET） |
| **Rowset信息** | | |
| `rowset_id` | VARCHAR | Rowset ID（输出rowset） |
| `version` | VARCHAR | Version范围，格式：[start-end] |
| `num_rows` | BIGINT | 行数 |
| `data_size` | BIGINT | 压缩后的数据大小（字节） |
| `bytes_uncompressed` | BIGINT | 未压缩数据大小（字节） |
| `num_segments` | INT | Segment文件数量 |
| `rowset_type` | VARCHAR | Rowset类型：ALPHA, BETA |
| **Compaction信息** | | |
| `compaction_type` | VARCHAR | Compaction类型：BASE, CUMULATIVE, 空表示非compaction |
| `compaction_algorithm` | VARCHAR | Compaction算法：HORIZONTAL, VERTICAL |
| `input_rowsets` | ARRAY<VARCHAR> | 输入rowset ID列表（仅compaction） |
| `input_rows` | BIGINT | 输入总行数（仅compaction） |
| `input_data_size` | BIGINT | 输入总数据大小（仅compaction） |
| `merged_rows` | BIGINT | 合并的行数（仅compaction） |
| `filtered_rows` | BIGINT | 过滤的行数（仅compaction） |
| **性能指标** | | |
| `peak_memory_usage` | BIGINT | 峰值内存使用（字节） |
| `compaction_score` | DOUBLE | Compaction评分（仅compaction） |

### 3.2 内存数据结构：PartsLogEntry

```cpp
struct PartsLogEntry {
    // 基础信息
    int64_t event_time_us;           // 事件时间（微秒）
    EventType event_type;            // 事件类型
    int64_t duration_ms;             // 持续时间

    // 标识信息
    int64_t be_id;
    std::string database;
    std::string table;
    int64_t table_id;
    int64_t partition_id;
    int64_t tablet_id;
    int64_t txn_id;                  // 仅NEW_ROWSET有效

    // Rowset信息
    std::string rowset_id;           // 输出rowset ID
    std::string version;             // version范围
    int64_t num_rows;
    int64_t data_size;               // 压缩后大小
    int64_t bytes_uncompressed;      // 未压缩大小
    int32_t num_segments;
    std::string rowset_type;

    // Compaction信息
    std::string compaction_type;     // BASE/CUMULATIVE/空
    std::string compaction_algorithm; // HORIZONTAL/VERTICAL
    std::vector<std::string> input_rowsets; // 输入rowset列表
    int64_t input_rows;
    int64_t input_data_size;
    int64_t merged_rows;
    int64_t filtered_rows;

    // 性能指标
    int64_t peak_memory_usage;
    double compaction_score;
};

enum EventType {
    NEW_ROWSET,           // 新rowset创建（数据导入）
    COMPACTION_START,     // Compaction开始
    COMPACTION_FINISH     // Compaction完成
};
```

## 4. 详细设计

### 4.1 PartsLogManager 设计

```cpp
class PartsLogManager {
public:
    static PartsLogManager* instance();

    // 记录新rowset创建事件
    void log_new_rowset(const Tablet* tablet,
                        const Rowset* rowset,
                        int64_t txn_id,
                        int64_t duration_ms);

    // 记录compaction开始事件
    void log_compaction_start(const CompactionTask* task);

    // 记录compaction完成事件
    void log_compaction_finish(const CompactionTask* task,
                               const Rowset* output_rowset,
                               const Status& status);

    // 查询日志（用于系统表）
    std::vector<PartsLogEntry> query_logs(int64_t start_time_us,
                                          int64_t end_time_us,
                                          int64_t tablet_id = -1);

private:
    // 内存缓冲区（环形缓冲区）
    std::deque<PartsLogEntry> _log_buffer;
    std::mutex _buffer_mutex;

    // 配置参数
    size_t _max_buffer_size = 100000;  // 最大缓存条目数
    int64_t _retention_seconds = 604800; // 保留7天

    // 后台flush线程
    void _flush_thread_func();
    std::thread _flush_thread;

    // 持久化到磁盘（可选，简化版本可以只保留内存）
    void _flush_to_disk();
    void _load_from_disk();
};
```

### 4.2 日志记录点

#### 4.2.1 数据导入记录点
在 `DeltaWriter::commit()` 或 `TxnManager::commit_txn()` 成功后记录：

```cpp
// 在 delta_writer.cpp 中
Status DeltaWriter::commit() {
    // ... 原有代码 ...

    // 创建rowset后记录日志
    if (_rowset != nullptr) {
        PartsLogManager::instance()->log_new_rowset(
            _tablet.get(),
            _rowset.get(),
            _opt.txn_id,
            _total_write_time_ms
        );
    }

    return Status::OK();
}
```

#### 4.2.2 Compaction记录点
在 `CompactionTask::run()` 中记录开始和完成事件：

```cpp
// 在 compaction_task.cpp 中
void CompactionTask::run() {
    // 记录compaction开始
    PartsLogManager::instance()->log_compaction_start(this);

    Status st = run_impl();

    // 记录compaction完成（成功或失败）
    if (_output_rowset != nullptr) {
        PartsLogManager::instance()->log_compaction_finish(
            this, _output_rowset.get(), st);
    }
}
```

### 4.3 数据查询流程

```
用户查询：SELECT * FROM INFORMATION_SCHEMA.be_parts_log
                WHERE tablet_id = 12345;
    │
    ▼
FE解析查询，向各BE发送RPC请求
    │
    ▼
BE执行 SchemaBePartsLogScanner::get_next()
    │
    ├─► 调用 PartsLogManager::query_logs()
    │
    ├─► 从内存缓冲区中筛选符合条件的日志
    │
    └─► 返回日志条目
    │
    ▼
FE聚合各BE返回的数据
    │
    ▼
返回给用户
```

### 4.4 日志持久化（可选）

为了防止BE重启后日志丢失，可以实现磁盘持久化：

1. **存储位置**：`${STORAGE_ROOT_PATH}/parts_log/`
2. **文件格式**：
   - 文件名：`parts_log_{date}_{sequence}.json` 或 `.parquet`
   - 使用JSON或Parquet格式存储
3. **Flush策略**：
   - 每隔5分钟flush一次
   - 缓冲区超过10000条记录时flush
   - BE正常关闭时flush
4. **清理策略**：
   - 保留最近7天的日志文件
   - 超过7天的文件自动删除

**简化方案**：初期可以只保留内存缓冲区，不做持久化，重启后日志丢失是可接受的。

## 5. 写入放大系数计算

### 5.1 计算公式

**写入放大系数（Write Amplification Factor, WAF）**定义为：

```
WAF = 总物理写入量 / 用户数据写入量
```

- **总物理写入量**：包括初始导入写入 + 所有compaction写入
- **用户数据写入量**：用户导入的原始数据量

### 5.2 SQL查询示例

#### 5.2.1 计算单表的写入放大系数

```sql
-- 计算表test_table在过去24小时的写入放大系数
WITH
-- 用户导入的数据量
user_writes AS (
    SELECT
        table_id,
        SUM(data_size) as user_data_size,
        SUM(bytes_uncompressed) as user_data_uncompressed
    FROM information_schema.be_parts_log
    WHERE event_type = 'NEW_ROWSET'
      AND table = 'test_table'
      AND event_time >= NOW() - INTERVAL 24 HOUR
    GROUP BY table_id
),
-- 所有物理写入（包括导入和compaction）
total_writes AS (
    SELECT
        table_id,
        SUM(data_size) as total_data_size
    FROM information_schema.be_parts_log
    WHERE event_type IN ('NEW_ROWSET', 'COMPACTION_FINISH')
      AND table = 'test_table'
      AND event_time >= NOW() - INTERVAL 24 HOUR
    GROUP BY table_id
)
SELECT
    u.table_id,
    u.user_data_size / 1024 / 1024 / 1024 as user_data_gb,
    t.total_data_size / 1024 / 1024 / 1024 as total_written_gb,
    t.total_data_size / u.user_data_size as write_amplification
FROM user_writes u
JOIN total_writes t ON u.table_id = t.table_id;
```

#### 5.2.2 按tablet统计compaction写入量

```sql
SELECT
    tablet_id,
    COUNT(*) as compaction_count,
    SUM(input_data_size) / 1024 / 1024 as total_input_mb,
    SUM(data_size) / 1024 / 1024 as total_output_mb,
    SUM(data_size) / SUM(input_data_size) as compaction_ratio,
    AVG(duration_ms) as avg_duration_ms
FROM information_schema.be_parts_log
WHERE event_type = 'COMPACTION_FINISH'
  AND table = 'test_table'
  AND event_time >= NOW() - INTERVAL 7 DAY
GROUP BY tablet_id
ORDER BY total_output_mb DESC
LIMIT 10;
```

#### 5.2.3 分析compaction类型的写入分布

```sql
SELECT
    compaction_type,
    COUNT(*) as count,
    SUM(input_rows) as total_input_rows,
    SUM(num_rows) as total_output_rows,
    SUM(merged_rows) as total_merged_rows,
    SUM(filtered_rows) as total_filtered_rows,
    SUM(data_size) / 1024 / 1024 / 1024 as total_output_gb,
    AVG(duration_ms) as avg_duration_ms
FROM information_schema.be_parts_log
WHERE event_type = 'COMPACTION_FINISH'
  AND table = 'test_table'
  AND event_time >= NOW() - INTERVAL 7 DAY
GROUP BY compaction_type;
```

#### 5.2.4 查看最近的高写入放大的compaction

```sql
SELECT
    event_time,
    tablet_id,
    compaction_type,
    compaction_algorithm,
    input_rows,
    num_rows,
    input_data_size / 1024 / 1024 as input_mb,
    data_size / 1024 / 1024 as output_mb,
    duration_ms,
    compaction_score
FROM information_schema.be_parts_log
WHERE event_type = 'COMPACTION_FINISH'
  AND table = 'test_table'
  AND input_data_size > 0
  AND event_time >= NOW() - INTERVAL 1 DAY
ORDER BY event_time DESC
LIMIT 20;
```

### 5.3 监控指标

基于parts_log可以建立以下监控指标：

1. **实时写入放大系数**（每小时/天）
2. **Compaction频率**（按类型统计）
3. **Compaction平均耗时**
4. **单表/单tablet的数据写入量趋势**
5. **异常compaction检测**（耗时过长、输入输出比例异常）

## 6. 配置参数

在 `be.conf` 中添加配置项：

```properties
# 是否启用parts_log
enable_parts_log = true

# 内存缓冲区最大条目数
parts_log_max_buffer_size = 100000

# 日志保留时间（秒），默认7天
parts_log_retention_seconds = 604800

# 是否持久化到磁盘
parts_log_enable_persistence = false

# flush间隔（秒）
parts_log_flush_interval_seconds = 300
```

## 7. 实现计划

### Phase 1: 核心功能（MVP）
**目标**：实现基本的日志记录和查询功能

1. **BE端实现**
   - [ ] 实现 `PartsLogEntry` 数据结构
   - [ ] 实现 `PartsLogManager` 基础功能（内存缓冲区）
   - [ ] 在 `DeltaWriter::commit()` 中添加NEW_ROWSET日志记录
   - [ ] 在 `CompactionTask::run()` 中添加COMPACTION日志记录
   - [ ] 实现 `SchemaBePartsLogScanner` 扫描器

2. **FE端实现**
   - [ ] 实现 `BePartsLogSystemTable` 系统表定义
   - [ ] 注册系统表到 INFORMATION_SCHEMA

3. **测试**
   - [ ] 单元测试：PartsLogManager基本功能
   - [ ] 集成测试：数据导入后验证日志
   - [ ] 集成测试：Compaction后验证日志
   - [ ] 系统表查询测试

### Phase 2: 增强功能
**目标**：提升稳定性和可用性

1. **持久化支持**
   - [ ] 实现日志flush到磁盘（JSON格式）
   - [ ] 实现BE启动时加载历史日志
   - [ ] 实现过期日志清理

2. **性能优化**
   - [ ] 优化内存缓冲区（环形缓冲区）
   - [ ] 异步写入日志，避免阻塞主流程
   - [ ] 批量查询优化

3. **功能完善**
   - [ ] 支持按database、table、tablet_id过滤
   - [ ] 添加更多性能指标（CPU、IO等）
   - [ ] 支持COMPACTION_START事件

### Phase 3: 高级特性
**目标**：提供更丰富的分析能力

1. **聚合视图**
   - [ ] 创建 `be_parts_log_summary` 视图（预聚合统计）
   - [ ] 按小时/天聚合的写入放大系数视图

2. **告警和监控**
   - [ ] 集成Prometheus metrics
   - [ ] 异常compaction告警（写入放大过高）

3. **诊断工具**
   - [ ] 提供写入放大分析工具脚本
   - [ ] Compaction效率分析报告

## 8. 性能考量

### 8.1 内存开销

每条日志条目大小估算：
- 固定字段：约200字节
- 可变字段（字符串）：约100-300字节
- input_rowsets数组：平均10个rowset × 40字节 = 400字节
- **总计**：约700字节/条

缓冲区内存开销：
- 100,000条 × 700字节 ≈ 70MB

**结论**：内存开销可控，对BE影响较小。

### 8.2 性能影响

1. **数据导入**：在commit阶段额外记录日志，耗时<1ms，影响可忽略
2. **Compaction**：同样在完成阶段记录，不影响compaction性能
3. **查询**：从内存扫描，性能高效

### 8.3 线程安全

- 使用 `std::mutex` 保护 `_log_buffer`
- 日志记录操作使用lock_guard，锁粒度小
- 不会引入死锁风险

## 9. 兼容性和向后兼容

1. **配置默认关闭**：`enable_parts_log = false`，避免影响现有部署
2. **系统表始终存在**：即使功能关闭，系统表也存在，只是返回空结果
3. **版本兼容**：FE和BE需要同时升级才能使用该功能

## 10. 测试计划

### 10.1 单元测试
- [ ] PartsLogManager 日志记录测试
- [ ] PartsLogManager 查询过滤测试
- [ ] PartsLogEntry 序列化/反序列化测试
- [ ] 缓冲区溢出和清理测试

### 10.2 集成测试
- [ ] 数据导入后查询be_parts_log验证
- [ ] 执行compaction后查询验证
- [ ] 多表并发导入测试
- [ ] 长时间运行测试（内存泄漏检测）

### 10.3 性能测试
- [ ] 大量日志条目查询性能
- [ ] 日志记录对导入性能的影响（<1%）
- [ ] 日志记录对compaction性能的影响（<1%）

### 10.4 端到端测试
- [ ] 计算写入放大系数的完整流程
- [ ] 不同compaction策略下的日志记录
- [ ] Cloud native模式下的日志记录

## 11. 文档

需要编写的文档：

1. **用户文档**
   - 功能介绍和使用场景
   - 系统表schema说明
   - SQL查询示例
   - 写入放大系数计算方法

2. **运维文档**
   - 配置参数说明
   - 性能影响和调优建议
   - 故障排查

3. **开发文档**
   - 架构设计
   - 代码结构
   - 扩展指南

## 12. 风险和限制

### 12.1 风险
1. **内存使用**：如果日志生成速度过快，可能导致缓冲区频繁溢出
   - **缓解**：设置合理的缓冲区大小和清理策略
2. **性能影响**：日志记录可能影响导入和compaction性能
   - **缓解**：异步记录，最小化锁粒度

### 12.2 限制
1. **历史数据**：默认只保留7天，更长期的分析需要外部存储
2. **精度**：时间精度为微秒，对于极短操作可能不够精确
3. **不支持删除操作**：目前只记录新增和compaction，不记录delete操作

## 13. 未来扩展

1. **导出到外部系统**：支持导出到Kafka、S3等外部存储
2. **更多事件类型**：记录delete、schema change等事件
3. **自动分析**：定期生成写入放大分析报告
4. **预测和优化**：基于历史数据预测compaction行为，优化策略

## 14. 参考资料

1. ClickHouse system.part_log文档：https://clickhouse.com/docs/operations/system-tables/part_log
2. RocksDB Write Amplification：https://github.com/facebook/rocksdb/wiki/Write-Amplification
3. StarRocks Compaction设计文档：（内部链接）

## 15. 总结

本设计文档提出了一个完整的parts_log实现方案，可以有效追踪StarRocks表的数据写入和compaction行为，为计算写入放大系数提供数据基础。实现采用分阶段策略，MVP版本专注于核心功能，后续逐步增强。该功能对现有系统影响较小，可以安全地在生产环境中部署使用。
