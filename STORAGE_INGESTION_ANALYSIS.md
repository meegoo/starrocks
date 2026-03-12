# StarRocks 存储与导入子系统改进分析

> 分析日期：2026-03-12
> 基于 StarRocks 代码库的深入分析，涵盖 BE 存储引擎、导入管道、事务管理等核心模块

---

## 一、总览：存储与导入的关键改进方向

```
优先级    改进方向                           AI时代关联度    当前成熟度
─────────────────────────────────────────────────────────────────────
P0       主键索引内存治理                      ★★★★★         中等
P0       Partial Update 内存溢写              ★★★★★         缺失
P0       Compaction 智能化                    ★★★★          中等
P1       导入 Exactly-Once 语义               ★★★★          不完整
P1       向量索引生产化                        ★★★★★         Beta
P1       大值/大文档存储能力                    ★★★★★         受限
P2       导入自适应批量与内存管理               ★★★★          粗糙
P2       存储格式与压缩精细化                   ★★★           中等
P2       CDC/Binlog 能力增强                  ★★★★          基础
P3       Cloud-Native 存储层完善              ★★★★          进行中
P3       索引体系增强                          ★★★★          中等
```

---

## 二、P0 级改进：必须尽快解决

### 2.1 主键索引内存治理

**现状问题：**

主键表（Primary Key Table）是 StarRocks 支持实时更新的核心，但其主键索引面临严重的内存压力问题。

| 问题 | 代码位置 | 影响 |
|------|---------|------|
| Shared-nothing 模式下主键索引全部驻留内存 | `be/src/storage/primary_index.h` | 大表 OOM 风险 |
| Persistent Index phmap 逐条 dump，性能差 | `be/src/storage/persistent_index.cpp:1434` | 刷盘慢 |
| 批量 PK 查询内存限制仅 100MB | `config::primary_key_batch_get_index_memory_limit` | Compaction 时受限 |
| Cloud-native PK 索引的 memtable 没有有效的内存上限 | `be/src/storage/lake/lake_persistent_index.h` | 内存不可控 |
| 远程存储每次 `get_size()` 需 HEAD 请求(10-50ms) | `be/src/storage/rows_mapper.h:51-54` | 并行 PK 执行时极慢 |

**为什么 AI 时代更紧迫：**
- AI 应用产生的数据量级更大（embedding 表可达百亿行）
- 实时更新需求更频繁（推理结果回写、特征更新）
- 主键索引内存问题直接限制了表的规模上限

**改进建议：**
1. **分层索引架构**：热数据在内存，温数据在 SSD，冷数据在磁盘/对象存储
2. **phmap 批量序列化**：替代逐条 dump，代码中已有 TODO（`persistent_index.cpp:1436`）
3. **主键索引 Bloom Filter 前置过滤**：减少不必要的精确查找
4. **Cloud-native 模式下 memtable 大小自适应**：基于可用内存动态调整

---

### 2.2 Partial Update 内存溢写机制缺失

**现状问题：**

代码中明确标注了这个 TODO：

```cpp
// be/src/storage/rowset_update_state.h:189
// TODO: dump to disk if memory usage is too large
std::vector<PartialUpdateState> _partial_update_states;
```

同样的问题也存在于 Cloud-native 模式：
```cpp
// be/src/storage/lake/rowset_update_state.h:233
// 同样缺失内存溢写
```

**影响：**
- 大批量 Partial Update 可能导致 BE OOM
- Column-mode Partial Update 的 `partial_update_memory_limit_per_worker` 默认 2GB，但没有溢写到磁盘的能力
- AI 场景下，embedding 列的局部更新（如重新生成 embedding）涉及大量数据

**改进建议：**
1. 实现 Partial Update State 的磁盘溢写，类似 Load Spill 机制（`be/src/storage/load_spill_*` 已有参考实现）
2. 对 Column-mode Partial Update 实现流式处理，避免全量加载到内存

---

### 2.3 Compaction 智能化

**现状问题：**

| 问题 | 代码位置 | 影响 |
|------|---------|------|
| Compaction 策略仍在优化中 | `default_compaction_policy.cpp:308` "TODO: still in optimize" | 策略不够智能 |
| CompactionManager "非常低效的实现" | `compaction_manager.cpp` "TODO: Very inefficient" | 调度性能差 |
| 垂直 Compaction 引入了小 segment 文件回归 | `tablet_updates.h:201-202` | 需要额外修复 Compaction |
| tablet_max_versions 默认 1000，高频导入易触达 | `config::tablet_max_versions` | 写入被拒绝 |
| Compaction 内存限制 `compaction_max_memory_limit` 默认 -1（无限制） | `config.h` | 内存失控 |

**为什么 AI 时代更紧迫：**
- AI 应用的数据写入模式更碎片化（大量小批次推理结果回写）
- Embedding 重新生成导致大量 Partial Update，加重 Compaction 负担
- 实时特征更新导致版本数快速增长

**改进建议：**
1. **Compaction 调度智能化**：基于写入负载、版本数、读取模式自适应调度
2. **Compaction 内存预算管理**：设置合理默认值，避免和查询争抢内存
3. **修复垂直 Compaction 回归**：消除小 segment 文件问题
4. **自适应版本限制**：根据表的写入频率动态调整 `tablet_max_versions`
5. **Compaction 优先级队列**：区分紧急 Compaction（版本溢出）和后台 Compaction

---

## 三、P1 级改进：显著影响竞争力

### 3.1 导入 Exactly-Once 语义

**现状问题：**

Routine Load（Kafka 消费）的幂等性保证不完整：

```java
// fe/fe-core/.../transaction/RoutineLoadMgr.java
// TODO(ml): Idempotency
```

具体表现：
- Kafka offset 追踪可用，但任务重试时可能重复消费
- FE 层没有去重机制
- Pipe（文件导入）的 `LoadStatus` 和 `FileList` 一致性无法保证（代码中有 TODO）

**为什么 AI 时代更紧迫：**
- AI 应用的数据管道要求端到端 Exactly-Once（推理结果不能重复写入）
- 实时特征管道（Kafka → StarRocks）是 AI 基础设施的关键路径
- RAG 场景中文档入库不能有重复

**改进建议：**
1. 实现基于 `(routine_load_id, partition, offset)` 的去重表
2. 对 Pipe 的元数据变更引入 WAL 机制
3. 提供用户可见的 Exactly-Once 语义保证

---

### 3.2 向量索引生产化

**现状问题（基于代码分析）：**

| 限制 | 代码位置 | 影响 |
|------|---------|------|
| 每表仅支持一个向量索引 | `VectorIndexParams` | 无法同时索引多个 embedding 列 |
| 仅支持 shared-nothing 模式 | 配置限制 | 云原生部署无法使用 |
| IVFPQ 索引需要 `index_build_threshold` | `vector_index_writer.cpp:40-51` | 小表无法使用 |
| 实验性功能需手动开启 | `enable_experimental_vector = true` | 生产环境顾虑 |
| ARRAY 列不支持 Zone Map | `segment_writer.cpp:168-183` | 向量列无法做范围过滤 |
| 向量索引在独立文件中 | `IndexDescriptor::vector_index_file_path` | 增加存储开销和管理复杂度 |

**AI 时代的核心需求：**
- 每行数据可能有多个 embedding（标题 embedding、内容 embedding、图片 embedding）
- 混合查询（向量相似度 + 标量过滤）是 RAG 的标准模式
- 生产环境需要在 Cloud-native 模式下使用

**改进建议：**
1. **支持多向量索引**：每表多个 ARRAY<FLOAT> 列各自建索引
2. **Cloud-native 向量索引**：在 shared-data 架构下支持向量索引
3. **混合查询优化**：实现 Pre-filter（先标量过滤再向量搜索）策略
4. **增加 DiskANN 索引**：更适合大规模、磁盘友好的向量索引算法
5. **向量列的元数据索引**：维度范围、模值范围等辅助过滤

---

### 3.3 大值/大文档存储能力提升

**现状问题：**

```cpp
// be/src/types/type_descriptor.h:37
static constexpr int MAX_VARCHAR_LENGTH = 1048576;  // 1 MB

// be/src/common/config.h:1752
CONF_mInt32(olap_string_max_length, "1048576");  // 可调但全局生效
```

| 限制 | 影响 |
|------|------|
| VARCHAR/VARBINARY 最大 1MB | 长文本、PDF 内容存储受限 |
| JSON 无硬性大小限制但内存解析 | 大 JSON 文档全部加载到内存 |
| Row Store 编码对大 VARCHAR 有栈溢出风险 | `row_store_encoder_simple.cpp` TODO |
| 字符串最大长度全局配置 | 无法按列设置 |

**为什么 AI 时代更紧迫：**
- RAG 场景需要存储完整文档（论文、合同、代码文件可超过 1MB）
- LLM 的上下文窗口持续增长（128K tokens ≈ 500KB+）
- 多模态数据（图像 base64、音频特征）可能超过当前限制

**改进建议：**
1. **提升 VARCHAR 默认上限**：至少 16MB，对标 PostgreSQL TOAST
2. **实现大值分页存储**：超过阈值的值自动分页存储，避免内存尖峰
3. **支持按列设置最大长度**：而非全局统一
4. **流式 JSON 解析器**：当前 JSON 缓冲区上限 4GB（`kJSONMaxBufferSize`），需要流式处理

---

## 四、P2 级改进：提升效率和体验

### 4.1 导入自适应批量与内存管理

**现状问题：**

```cpp
// be/src/runtime/stream_load/stream_load_context.h:234-236
int64_t max_batch_rows = 100000;           // 固定 10 万行
int64_t max_batch_size = 100 * 1024 * 1024; // 固定 100MB
```

| 问题 | 影响 |
|------|------|
| Stream Load 固定 100MB/10 万行 | 宽表（如特征表 1000+ 列）可能 OOM |
| 不根据表 schema 动态调整 | 窄表批量太小，宽表批量太大 |
| OlapTableSink 每节点通道 100MB 缓冲 | 多副本 × 多 tablet = 内存爆炸 |
| StreamLoadPipe 默认仅 1MB | 高吞吐场景频繁上下文切换 |
| load_process_max_memory_limit_percent 默认 30% | 留给导入的内存可能不足 |

**改进建议：**
1. **自适应批量大小**：`max_rows = min(user_limit, available_memory / avg_row_size)`
2. **通道级内存预算**：根据可用内存动态分配通道缓冲区
3. **导入内存保留机制**：在开始导入前预留内存，避免和查询争抢
4. **StreamLoadPipe 自适应缓冲**：根据检测到的吞吐率动态调整

---

### 4.2 存储格式与压缩精细化

**现状问题：**

| 问题 | 代码位置 | 影响 |
|------|---------|------|
| 压缩按表级别设置，不支持按列设置 | `segment_writer.cpp:94` | 无法对不同列用不同压缩算法 |
| ARRAY<FLOAT> 不支持 Zone Map | `segment_writer.cpp:168-183` | 向量列无法做范围裁剪 |
| TINYINT 不支持字典编码 | `encoding_info.h:56-59` | 合理限制，但可优化 |
| Decimal 类型采样不支持 | `data_sample.cpp:98` TODO | 统计信息不完整 |
| Merge Iterator 限制 65535 行 chunk | `merge_iterator.h:26` uint16_t | 大合并受限 |
| Zone Map 的 num_rows 设置不正确 | `zone_map_detail.h:48` FIXME | 统计信息不准确 |

**改进建议：**
1. **列级压缩策略**：embedding 列用 ZSTD，元数据列用 LZ4，文本列用 ZSTD 高压缩级别
2. **ARRAY 列的辅助索引**：至少支持数组长度的 Zone Map
3. **扩大 Merge Iterator chunk 上限**：使用 uint32_t 替代 uint16_t
4. **完善统计信息采样**：支持所有数值类型

---

### 4.3 CDC/Binlog 能力增强

**现状问题：**

Binlog 模块已有基础实现（`binlog_manager.h`、`binlog_file_writer.cpp`等 37 个相关文件），但有多个待优化项：

```cpp
// binlog_file_reader.cpp
// TODO: Use global buffer to optimize sequential reads

// binlog_file_writer.cpp
// TODO: Reduce estimation cost and reuse serialized content

// binlog_builder.cpp
// TODO: sync parent dir if new files created, path gc for cleanup
```

**为什么 AI 时代需要更强的 CDC：**
- AI 应用需要监听数据变更触发推理流水线
- 特征实时同步到在线服务
- Embedding 重新生成的增量触发
- 与 Flink/Spark Streaming 的深度集成

**改进建议：**
1. **Binlog 读取性能优化**：实现全局缓冲区、减少序列化开销
2. **Binlog GC 自动化**：当前缺失路径清理
3. **结构化 CDC 输出**：支持 Debezium 格式输出

---

## 五、P3 级改进：中长期架构升级

### 5.1 Cloud-Native 存储层完善

**代码中的关键 TODO：**

| TODO | 文件 | 影响 |
|------|------|------|
| Lake PK 索引不支持真正的 upsert/delete 交错顺序 | `lake_persistent_index.cpp:860` | 事务语义不完整 |
| Lake schema change 不支持 writer final merge | `lake/schema_change.cpp:352` | Schema 变更效率低 |
| Lake compaction 不支持 row source mask buffer | `lake/compaction_policy.cpp:579` | Compaction 功能受限 |
| Lake PK 索引重建和本地模式走不同路径 | `lake_local_persistent_index.cpp:27` | 代码重复，维护困难 |
| DeleteVec 文件未清理 | `lake_primary_key_recover.cpp:33` | 存储泄漏 |
| 对 StarOS 硬依赖 | `lake/tablet_manager.cpp` | 存储后端不可替换 |

**改进建议：**
1. 统一 shared-nothing 和 shared-data 的 PK 索引实现路径
2. 实现 Lake 模式下的 upsert/delete 正确交错
3. 解耦 StarOS 依赖，支持更多对象存储后端

---

### 5.2 索引体系增强

**当前索引体系：**

| 索引类型 | 适用场景 | 成熟度 | 缺失能力 |
|---------|---------|--------|---------|
| Zone Map | 范围裁剪 | 成熟 | 不支持 ARRAY 类型 |
| Bloom Filter | 等值判断 | 成熟 | 不支持 FLOAT/DOUBLE |
| Bitmap Index | 低基数过滤 | 成熟 | 不支持 FLOAT/DOUBLE/TINYINT |
| 倒排索引 (CLucene) | 全文检索 | 中等 | 分词器选择有限 |
| N-gram Index | 子串匹配 | 中等 | — |
| 向量索引 | 相似搜索 | Beta | 多索引/Cloud-native 不支持 |
| **多列联合索引** | 联合过滤 | **缺失** | `tablet_schema.cpp` 有 TODO |

**改进建议：**
1. **多列联合索引**：AI 查询经常涉及多维过滤
2. **倒排索引增强**：支持更多分析器（中文分词、语义分词）
3. **Bloom Filter 对 FLOAT 支持**：允许 embedding 维度上的快速过滤
4. **自适应索引**：根据查询模式自动建议索引

---

## 六、导入子系统专题分析

### 6.1 各导入方式改进优先级

| 导入方式 | 当前状态 | 最紧迫改进 |
|---------|---------|----------|
| **Stream Load** | 生产就绪 | 自适应批量大小、大 JSON 流式解析 |
| **Routine Load** | 生产可用但有隐患 | Exactly-Once 语义、CN/DN 架构适配 |
| **Broker Load** | 生产就绪 | — |
| **INSERT INTO** | 生产就绪 | — |
| **Pipe (文件)** | 有一致性风险 | LoadStatus/FileList 原子性 |
| **Batch Write** | 中等 | TxnState 批量分发、自适应窗口 |
| **Partial Update** | 中等偏低 | 内存溢写、Column-mode 稳定性 |

### 6.2 格式解析器改进

| 格式 | 文件数 | 当前能力 | 需改进 |
|------|--------|---------|--------|
| CSV | 26 个转换器 | 全面 | 数值溢出检测 |
| JSON | 5 个列处理器 | 基础 | **流式解析器（上限 4GB 缓冲不够）** |
| Parquet | 40+ 文件 | 完整嵌套支持 | 深层嵌套边界案例 |
| ORC | 25+ 文件 | 完整 | Schema 演进文档化 |
| Avro | Confluent 注册 | 基础 | Schema 版本策略 |

### 6.3 事务与并发改进

| 问题 | 严重程度 | 改进方向 |
|------|---------|---------|
| Routine Load 幂等性未实现 | 严重 | 基于 offset 的去重表 |
| Publish Version 单线程 | 中等 | 批量并行 RPC |
| Merge Condition 仅支持简单列引用 | 中等 | 支持表达式（`TODO:(caneGuy)`） |
| Merge Condition 类型限制过严 | 中等 | 放开 VARCHAR 等类型 |
| 导入 DOP 自动检测逻辑不清晰 | 低 | 明确自动调优策略 |

---

## 七、与 AI 时代的关联总结

```
                          存储与导入改进如何服务 AI 场景
┌─────────────────────────────────────────────────────────────────┐
│                                                                 │
│  RAG / 语义搜索                                                  │
│  ├── 向量索引生产化（多索引、Cloud-native）                         │
│  ├── 大文档存储（VARCHAR > 1MB）                                  │
│  └── 混合查询优化（向量 + 标量）                                   │
│                                                                 │
│  实时特征管道                                                     │
│  ├── Routine Load Exactly-Once（特征不重复）                      │
│  ├── Partial Update 内存溢写（大规模特征更新）                      │
│  ├── CDC/Binlog 增强（特征变更触发下游）                           │
│  └── 导入自适应内存管理（高频小批次不 OOM）                         │
│                                                                 │
│  Embedding 管理                                                  │
│  ├── 主键索引内存治理（百亿级 embedding 表）                       │
│  ├── Compaction 智能化（频繁 embedding 更新不堆积版本）              │
│  ├── 列级压缩（embedding 列 ZSTD，元数据列 LZ4）                  │
│  └── ARRAY<FLOAT> 辅助索引（维度/模值 Zone Map）                  │
│                                                                 │
│  AI 数据准备                                                     │
│  ├── JSON 流式解析（大文档批量导入）                               │
│  ├── 多列联合索引（多维特征过滤）                                  │
│  └── Cloud-native 存储完善（弹性扩缩容）                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 八、核心结论

**最需要尽快改进的三件事：**

1. **主键索引内存治理 + Partial Update 溢写**
   - 这是 StarRocks 支持大规模实时更新（AI 应用的核心需求）的基础
   - 当前代码中有明确的 TODO 标记，说明开发团队也意识到了问题
   - 不解决这个问题，表规模上限直接受限

2. **Compaction 智能化**
   - AI 应用的写入模式（高频小批次 + 大量 Partial Update）会导致版本快速堆积
   - 当前的 Compaction 调度被标记为"非常低效"
   - 版本数超限会直接拒绝写入，影响数据管道稳定性

3. **向量索引从 Beta 到生产**
   - 单表单向量索引的限制在 AI 场景中完全不够用
   - Cloud-native 模式不支持向量索引是部署架构的重大缺口
   - 混合查询优化（标量过滤 + 向量搜索）是 RAG 的标准查询模式

**一句话总结：** StarRocks 存储引擎的 OLAP 基础能力扎实（列存、压缩、索引体系齐全），但在 AI 时代最核心的三个方面——**大规模实时更新的内存管理**、**高频写入的 Compaction 效率**、**向量搜索的生产可用性**——需要尽快突破，这三者共同决定了 StarRocks 能否成为 AI 应用的实时数据基础设施。
