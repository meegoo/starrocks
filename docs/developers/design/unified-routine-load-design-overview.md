# 统一 Routine Load 概要设计（HLD）

> **版本**: v1.0（基于 v0.2 评审 → v0.3 → 实现级细化 → 拆分定稿）
> **状态**: 概要设计
> **配套详细设计**: `unified-routine-load-design-detail.md`（DLD）。本文只讲 **what / why**（架构、目标、模型、决策、兼容、路线图）;所有 **how**（thrift IDL、touch points、伪代码、算法复刻、边界）见 DLD。

## HLD ↔ DLD 章节映射

| 主题 | 本文（HLD） | 详细设计（DLD） |
|------|------------|----------------|
| 执行模型 compile-once/rebind | §8 | DLD §1 |
| PK 写语义（`__op`/partial/merge） | §12 | DLD §2 |
| 错误容忍与统计回传 | §13 | DLD §3 |
| SHOW ROUTINE LOAD 列契约 | §20.3 | DLD §4.1 |
| avro + Schema Registry | §5.2 | DLD §4.2 |
| 全错批次跳过 | §11.3 | DLD §4.3 |
| ERROR 自动恢复 / group_id | §18.1 / 附录B | DLD §4.4 |
| consumer 池 / 反压 / 资源隔离 | §14/§15/§16 | DLD §5 |
| 代码事实锚点（file:line） | —（不在 HLD 正文） | DLD §6 |

> 交叉引用统一写「详见 DLD §N」/「见 HLD §N」,不带文件名与版本号。

---

## 目录
1. 背景与动机 · 2. 业界调研 · 3. 设计目标与性能指标 · 4. 整体架构 · 5. SQL 语法设计 · 6. 模块职责 · 7. FE-BE 接口概述 · 8. 执行模型 · 9. 并发模型与动态并行度 · 10. 自适应降级与延迟/版本预算 · 11. Offset 与 Exactly-Once · 12. PK 写语义 · 13. 错误容忍 · 14. Consumer 管理 · 15. 反压 · 16. 资源隔离 · 17. 凭证管理 · 18. 状态机与生命周期 · 19. 存算分离适配 · 20. 兼容性 · 21. 可观测性 · 22. 配置参数 · 23. 路线图 · 附录 A/B/C

---

## 1. 背景与动机

现有 Routine Load 使用独立执行框架,问题:① 手动 Task 拆分(`desired_concurrent_number` + 静态 round-robin);② 独立执行路径(`StreamLoadPlanner` + `RoutineLoadTaskExecutor`),维护成本高;③ 无法利用 MPP 并行(每 Task 单 BE);④ 静态并行度;⑤ 与 Pipe 框架割裂(Pipe 已实现基于 INSERT 的持续加载,`Pipe.Type` 已预留 `KAFKA`,但 Kafka 源未打通)。

**目标**:把 Routine Load 统一为一种**流式 Pipe**——标准 `INSERT INTO ... SELECT FROM kafka(...)`、复用 INSERT/MPP 执行体、动态并行度、完全兼容现有能力。

> 就绪度提示:`Pipe.Type` 确含 `KAFKA`;但 `PipeSource`/`PipePiece` 多态、`kafka()` TVF、行级 `__op` 均为**净新增**(详见 DLD)。

## 2. 业界调研

| 系统 | 模型 | 并行度 | 动态 | 语义 |
|------|------|--------|------|------|
| ClickHouse | Kafka Engine + MV | num_consumers | 否 | at-least-once（实验 exactly-once） |
| Databricks | read_kafka TVF + Streaming Table | partition→task | 弱 | Delta exactly-once |
| Snowflake | Snowpipe Streaming | serverless | 是 | 默认 exactly-once |
| Flink SQL | CREATE TABLE + 连续 INSERT | scan.parallelism | Reactive | Kafka txn + checkpoint |

借鉴:标准 INSERT 驱动流式(Flink)、服务端自动扩缩(Snowflake)、TVF 封装源(Databricks/Flink)、声明式 Pipe(Snowflake+现有 Pipe)。完整对比见附录 A。

## 3. 设计目标与性能指标

### 3.1 核心目标
统一为 `Pipe.Type.KAFKA`;`INSERT INTO ... SELECT FROM kafka(...)` 语法;单 INSERT 多 BE MPP;动态并行度自适应;完全兼容 `CREATE ROUTINE LOAD`(含 PK 表 `__op`/partial-update、错误容忍语义)。

### 3.2 性能指标（预期目标,尽力而为）
`target_e2e_latency` 是**预期/目标延迟,不是硬 SLA**。系统尽力达成;当 commit/publish/compaction 跟不上时**自动降级**(加大 batch、降频,§10),并通过 `SHOW PIPES.EFFECTIVE_E2E` 暴露**实际有效延迟**与降级原因。

| 指标 | 目标 |
|------|------|
| 端到端延迟(预期) | 用户声明,默认 `1s`,可请求低至 `100ms` |
| 有效延迟(实测) | shared-nothing 窄表低负载可接近 100ms;shared-data 宽表/高负载通常 1–5s |
| 吞吐 | 高于现有 Routine Load |
| 降级响应 | ≤ 数个批次周期 |

### 3.3 非目标（当前版本）
- **Pulsar 源**:现有 `PulsarRoutineLoadJob` 保留在 legacy 引擎,本版只覆盖 KAFKA(§20.4)。
- **Protobuf**:StarRocks 任何路径都不支持(RL 仅 csv/json/avro),非对齐项,移除。
- **多 topic / 多表扇出**:Phase 1 为**单 topic、单目标表**每 pipe(对齐 RL);多 topic 订阅是 Phase 4;一 topic 多表扇出**不在范围**。
- 跨集群复制;CDC changelog 通用框架(`envelope=debezium` 走兼容映射,附录 B)。

## 4. 整体架构

```
用户接口:  CREATE PIPE p AS INSERT INTO t [(cols)] SELECT ... FROM kafka(...) [WHERE]
           兼容层:CREATE ROUTINE LOAD → 内部转换为 Kafka Pipe
FE 调度:   PipeManager → KafkaPipeScheduler → KafkaPipeSource
             (partition 发现 / committedOffsets / 动态并行度 + 降级 / 编译一次的 ExecPlan 缓存)
           提交:专用低延迟提交器(事件驱动,绕开 TaskManager 1s tick 与全局并发闸)
BE 执行:   KafkaScanNode(FE 下发 [begin,end)) → [exchange?] → OlapTableSink
           单批次单事务;offset 与统计随事务 commit-attachment 原子回传
```

### 4.1 核心变化

| 组件 | 现有 Routine Load | 统一后 |
|------|-------------------|--------|
| 语法 | `CREATE ROUTINE LOAD ... FROM KAFKA` | `CREATE PIPE ... AS INSERT ... FROM kafka(...)` |
| 调度 | RoutineLoadScheduler + TaskScheduler | PipeScheduler + KafkaPipeScheduler + 专用提交器 |
| 拆分 | FE 按 partition round-robin 拆 N Task | 单 INSERT,MPP 把 KafkaScanNode 分布到多 BE |
| 规划 | StreamLoadPlanner（绕过 CBO,固定 fragment） | compile-once:每 pipe 编译一次 ExecPlan,每批重绑(§8) |
| 并行度 | 静态 | 动态自适应 + 降级(§9/§10) |
| 事务 | N Task × N txn | 1 批次 × 1 txn |

**事务模型**:单次重试代价更小(批次窗口远小于旧 10s 默认);但**稳态版本创建率更高**——每微批每 tablet ≥1 rowset,批频(非批量)驱动版本增长。从 10s 降到亚秒使版本/rowset 率放大 10–100×,触及两道硬墙:shared-nothing 的版本数硬上限(超限补救是"调大降频",与降延迟相反)、shared-data 默认开的提交限速(compaction score 高时延迟/拒绝 commit)。故批频必须受**版本/compaction 预算**约束,由 §10 自适应降级统一处理。

## 5. SQL 语法设计

### 5.1 新语法
```sql
CREATE PIPE [IF NOT EXISTS] [db.]pipe_name
[PROPERTIES (
  "target_e2e_latency"="1s",        -- 预期延迟（默认 1s,可低至 100ms,尽力而为）
  "auto_parallelism"="true", "max_parallelism"="16", "warehouse"="...",
  "max_error_number"="0",            -- 绝对错误行数;0=零容忍（与旧语义一致）
  "max_filter_ratio"="1.0",          -- 比例门;默认 1.0（对齐 RL,见 §13）
  "strict_mode"="false",
  "enable_op_column"="false", "merge_condition"="", "partial_update"="false", "partial_update_mode"="row"
)]
AS INSERT INTO target_table [(col1,...)]
SELECT col1, ... FROM kafka(
  "broker_list"="h1:9092,h2:9092", "topic"="t", "group_id"="g",
  "format"="json",                   -- json/csv/avro/raw（无 protobuf）
  "partitions"="0,1,2", "offsets"="OFFSET_BEGINNING",   -- 仅初始 offset
  "confluent.schema.registry.url"="...",                 -- avro 必填
  "property.security.protocol"="SASL_PLAINTEXT", "property.sasl.mechanism"="PLAIN"
) [WHERE condition];

ALTER PIPE p SUSPEND|RESUME|SET(...);  DROP PIPE [IF EXISTS] p;  SHOW PIPES [...];
```

### 5.2 kafka() TVF
净新增 TVF(现仅有 `files()`),复用 BE 现有 json/csv/avro scanner。**Schema 策略 = 命名 SELECT/COLUMNS 投影下推**(不是笼统"目标表 schema 下推"):BE 的 JSON 按名匹配依赖目标 slot 列名,故生成的 INSERT 必须用命名投影(非 `SELECT *`)。

| format | schema | 对齐 |
|--------|--------|------|
| json(无 jsonpaths) | 投影列名 ↔ JSON key 自动匹配,缺失填 NULL | 同 RL |
| json(有 jsonpaths) | 第 i 个 jsonpath → 第 i 列,支持嵌套 | 同 RL |
| csv | 位置映射 + `column_separator`/`trim_space`/`enclose`/`escape` | 同 RL/files() |
| avro | `confluent.schema.registry.url` 注册表推导,按名匹配 | **net-new BE from-bytes 路径**(复用 AvroScanner+libserdes,详见 DLD §4.2) |
| raw | 固定 6 列 `_key/_value/_topic/_partition/_offset/_timestamp` | net-new,类 read_kafka |

`format=raw` 用固定 schema(不下推),其余用命名投影下推。所有格式可经 SELECT 表达式派生/转换。`__op` 作为可选尾列投影(§12)。

### 5.3 兼容语法
`CREATE ROUTINE LOAD` 在 FE 解析后内部转换为等价 Kafka Pipe;完整属性映射见附录 B;转换层对**无法映射**属性 fail-fast。兼容 `SHOW/PAUSE/RESUME/STOP/ALTER ROUTINE LOAD`(列映射见 §20.3)。

## 6. 模块职责

**FE**:`PipeSource`/`PipePiece`(抽象基类,**净新增**)、`Pipe`(扩展 `Type.KAFKA`)、`PipeManager`/`PipeScheduler`(扩展)、`KafkaPipeSource`(新增:partition 发现、committedOffsets、动态并行度+降级、绝对错误窗口)、`KafkaPipePiece`(新增)、`KafkaPipeExecPlanCache`(新增)、`KafkaPipeSubmitter`(新增专用提交器)、`KafkaTableFunction`/`KafkaTableFunctionRelation`(新增 TVF,渲染处脱敏)、`KafkaScanNode`(新增 FE plan 节点)、`InsertPlanner`(扩展:PK+opt-in 追加尾 `__op` slot)。
**BE**:`kafka_scan_node`(新增 pull operator,复用 `DataConsumer`/`DataConsumerPool`)、consumer 池(扩展:可配 cap、分桶、idle 回收、group.id 移出 match key)。
> 包路径、接口/IDL、touch points 见 DLD。

## 7. FE-BE 接口概述

新增一个 `KafkaScanNode` 计划节点(结构性 broker/topic/format/registry_url 进计划),**每批的 `[begin,end)` offset 经 scan-range 下发**(走既有 deploy 期 per-instance scan-range 缝)。**统计与终止 offset 经 FE 内部事务 commit-attachment 回传**——具体是**扩展现有 `InsertTxnCommitAttachment`**(加 optional Gson 字段:`partitionEndOffsets`/`filteredRows`/`unselectedRows`/`receivedBytes`/`trackingUrl`),**不引入** `TKafkaConsumeReport`(本库不存在该 struct)。IDL 字段与 BE 解码见 DLD §1.5/§3.3/§4.2。

## 8. 执行模型（概念）

### 8.1 为什么不每批走 TaskManager
TaskManager 派发是 1s tick(硬编码,非 Config)、全局并发上限(与 MV refresh 共享)、无 INSERT plan cache。现 Routine Load 恰恰绕开 CBO。故本版**不沿用** TaskManager 逐批派发。

### 8.2 compile-once vs per-batch

| 编译一次（每 pipe） | 每批次（高频） |
|---------------------|----------------|
| 走标准 Transformer+CBO+PlanFragmentBuilder **一次**,缓存 ExecPlan 模板 | 现建 Coordinator(便宜,无优化器) |
| 失效条件:schema / partition 集 / 节点集 / 并行度变更 | 经 scan-range 下发本批 `[begin,end)` |
| 真实收益 = 省 optimizer + fragment-build | 开新 txn,**重跑 sink complete()**(便宜 epoch 门稳态跳过) + 新 query_id |

> **关键(详见 DLD §1.3)**:每批不止重绑 txn_id——`OlapTableSink.complete()` 还烘焙了 partition/tablet-location/nodes,长驻 pipe 下会漂移,故需 re-complete(epoch 门:schema+partitionSet+node/isAlive;tablet-location 漂移靠 deploy-failure 兜底)。SELECT 含 join 时多批在飞会竞争共享 runtime-filter 状态,故每 pipe 默认单批在飞。

### 8.3 与现有 Pipe (FILE) 复用
PipeManager 完全复用;PipeScheduler 扩展;执行体(INSERT/MPP)复用但**调度路径新增**(专用提交器 + plan cache);`Pipe.State` 含 `FINISHED`(Kafka 源永不进入)。

### 8.4 自动建分区与 schema 变更
- **自动建分区**:kafka INSERT 与 RL 一样继承表的自动分区——新分区由 **BE sink 运行时经 `createPartition` RPC** 创建(不由 FE 计划),缓存 sink 须保留 `enableAutomaticPartition`;运行时新建分区自然让下批 partitionSet epoch 变化而触发 re-complete。
- **schema 变更**:schema 变更触发**整计划重建**(非仅 sink re-complete,因 slot/tuple 布局变);已部署在飞批次按旧 schema **abort 重消费**(committedOffsets 仅 COMMITTED 才推进,安全);重建后 `__op` 尾 slot 需重新校验。

## 9. 并发模型与动态并行度

单 INSERT 由 MPP 把 KafkaScanNode 的 partition 分布到多 BE fragment instance,与 OlapTableSink 组成计划,单事务 commit。并行度 = 跨 BE 的 scan-range 分配(instance 数),非 `pipeline_dop`。

动态并行度算法(详见 DLD)按多信号自适应:lag、lag 速率、`target_lag=EWMA(throughput)*target_e2e_latency`、**per-node**(非均值)BE CPU/Mem(带新鲜度门)、**compaction score / commit 延迟**(§10)、扩缩有效性、冷却期。要点:资源信号 fresh-gate + per-node max(单台打满即不扩);并行度变更非免费(重 plan + consumer 冷启动),故 partition→instance **sticky** + 非对称滞后;压力升高走**降频**(§10)而非缩并行度;冷启动 seed `min_parallelism` + warm-up;跨 pipe 每 tick 统一采集资源避免 thundering herd。

## 10. 自适应降级与延迟/版本预算

延迟与版本压力收敛到**同一降级杠杆 = 有效批次间隔/大小**。`commit+publish` 不是 E2E 的固定比例,而是 `f(tablet 数, 存储后端, 对象存储 RTT, compaction score)` 的**下限**。`effective_e2e_floor = schedule_floor + 单批 plan/commit + per-tablet publish 成本`,向上 clamp `target_e2e_latency`,在 `SHOW PIPES.EFFECTIVE_E2E` 暴露。

**降级控制器**:压力上升(commit 延迟高 / compaction score 接近阈值 / 版本数接近上限)→ 提高有效批次间隔 + 增大**有效批大小**(运行时下发 TVF 的批上限;**不改**声明的 `max_batch_rows`——后者是 §13 错误窗口基数,降级不动它,见 DLD §3.2),每版本攒更多行,牺牲延迟换稳定,`SHOW PIPES` 给 `THROTTLED_BY_COMMIT_RATE`/`VERSION_PRESSURE`;单批 commit 超时 → abort + 下批以更大 batch 重试。稳态保证 `rowset 到达率 ≤ compaction 排空率`,每表设最小有效批次间隔硬地板。

> 这样既让 100ms 在窄表/低负载/shared-nothing 下可逼近,又在宽表/高负载/shared-data 下自动退避到可持续延迟,而非违约或撞版本墙。

## 11. Offset 与 Exactly-Once

### 11.1 Offset 模型（FE 下发区间）
FE 每批经 scan-range 下发明确的 `[begin, end)`(`end=-1` 表示消费至超时);BE pull operator 在区间/超时内消费。`committedOffsets` 是每 partition 的"下一个待消费" offset。

### 11.2 Exactly-Once（权威 = commit-attachment）
- **唯一持久权威 = COMMITTED 事务的 commit-attachment**:扩展后的 `InsertTxnCommitAttachment` 携带 `partitionEndOffsets`,与数据**原子提交**(对齐现有 `RLTaskTxnCommitAttachment` + `KafkaProgress`)。固定区间(`end!=-1`)路径 `committedOffsets` 推进到 FE 已知 `piece.end`;**时间窗口模式(`end=-1`,主用低延迟模式)与 short-read**(BE 实消费少于请求区间)下,实际 end 由 BE 决定,经 attachment 的 `partitionEndOffsets` 回报为准(该 BE→FE 通道是 net-new,见 DLD §3.4)。
- **恢复**:`afterCommitted` 实时路径 **与 edit-log replay 路径**都从 attachment 应用 offset(Pipe 版 `replayOnCommitted`),leader 换届后重建。
- 周期持久化仅作派生缓存/界定 replay 长度,**绝不**覆盖 attachment 真值。不依赖 Kafka consumer group offset(仅供外部监控)。
- **latest offset / lag**:由 FE **周期性高水位探测**(`KafkaUtil.getLatestOffsets`,同 RL 的 `latestPartitionOffsets`)获得,**不**来自 commit-attachment;有界限陈旧窗口。

### 11.3 全错批次跳过
整批消费了行但全被过滤/出错(毒批)时,推进 `committedOffsets` 跳过它(受 `max_error_number`/`max_filter_ratio` 约束),避免"abort→永久重消费"的 livelock;真正的 fragment/txn 失败则不前移、重消费。三态分类与边界(空 poll 不前移、毒批前移)详见 DLD §4.3。

## 12. PK 写语义

**用户要求:必须对齐现有 Routine Load 能力。** 核实结论:BE 的 op 列契约是**位置式**(输出 tuple 尾列名 `__op`),已完整实现;**无需 BE / thrift 改动**。
- **merge_condition**:INSERT 路径**已通**,兼容层经 PROPERTIES 透传即可。
- **partial_update**(column 模式):INSERT 路径**已通**(目标列子集 + 模式),兼容层声明式透传。
- **行级 `__op`(UPSERT/DELETE)**:**net-new FE**——PK 表 + `enable_op_column=true` 时,kafka() TVF 产出尾 `__op` 列、analyzer 强转 TINYINT、`InsertPlanner` 追加尾 slot 并绑定,点亮 BE 既有契约。`enable_op_column` 默认 false(kafka→Pipe 重写对 PK 自动开),普通 INSERT 永不长出 op slot。
- **约束**:column 模式 partial update + DELETE 在 sort-key 表上与 Load 同等受限,不放宽。

装配步骤(3a/3b/3c)、analyzer 计数剔除、CDC `c/r/u/d`→`TOpType` 投影、touch points 详见 DLD §2。

## 13. 错误容忍

| 门 | RL | INSERT 路径 |
|----|----|------------|
| 比例门 `max_filter_ratio` | BE,默认 1.0 | `insert_max_filter_ratio`,**默认 0** |
| 绝对计数 `max_error_number` | FE 滑动窗口累计 → 超限停 | **无**(净新增) |

**双闸模型**:**per-batch 比例门**(BE/StmtExecutor)决定本批 commit-vs-abort-as-poison;**cross-batch `max_error_number`**(FE 滑动窗口)决定 quietly-skip-vs-**停 pipe**。超 `max_error_number` → pipe 进 **`State.ERROR`(终态,仅手动 RESUME)**,原因 `TOO_MANY_FAILURE_ROWS_ERR`;per-batch 比例超限**不**直接进 ERROR,而是该批 abort-as-poison + offset 跳过(§11.3),pipe 继续 RUNNING。

> ⚠️ **默认翻转告警**:`insert_max_filter_ratio` 默认 0(零容忍)vs RL 默认 1.0(全容忍,靠绝对计数把关)。兼容层**必须**把每批 INSERT 的比例设为 pipe 的 `max_filter_ratio`(默认 1.0,**经 INSERT 的 `MAX_FILTER_RATIO_PROPERTY`、非全局会话变量**,见 DLD §3.7),否则一条坏行就 abort,滑动窗口成死代码。
> **关键(详见 DLD §3)**:统计回传(filtered/unselected/tracking_url)经扩展的 `InsertTxnCommitAttachment`,在 commit **与 abort**(毒批)两条路径都回传,使绝对计数窗口能见到全过滤批——否则全过滤批 abort 后窗口永远收不到、毒批流永不停。

## 14. Consumer 管理

复用 BE 现有 consumer 池,但需修四点:① cap 配置化(替代硬编码 10);② 按 `(broker,topic,连接属性)` 分桶;③ `group.id` 移出 match key(消费基于 `assign()`、offset 由 SR 管);④ 接上 idle 回收(现为死代码)。实现见 DLD §5。

## 15. 反压

复用 INSERT pipeline 的**有界缓冲背压**(OlapTableSink 阻塞 → 暂停上游 KafkaScanNode;非"零积压",数据先填 NodeChannel 发送缓冲);新增 **stall watchdog**(无进展超时 → abort + 下批重试,与 §10 降级联动)。机制与缓冲量见 DLD §5。

## 16. 资源隔离

分层:① **准入** = 不按集群利用率阻塞批次,进 Query Queue / Resource Group;② **并行度阻尼** = §9 的资源信号仅**抑制 scale-up**,从不阻塞批次;③ **专用车道** = Kafka pipe INSERT 走专用提交器,独立并发预算/优先级,不被 MV refresh/查询饿死也不淹没共享队列;④ 真正的 receiver 背压是**共享的** load 内存上限(非 per-pipe,Resource Group 不细分),如需隔离给每批设 `load_mem_limit`;⑤ commit-rate 背压纳入 §10。
> "INSERT 天然受 Query Queue 保护"**默认是空头支票**(Query Queue 默认关),文档要求显式开启或由专用车道自管。详见 DLD §5。

## 17. 凭证管理

四原则(GA blocker,Phase 1):① **结构化抽取**——CREATE 时把 `property.sasl.*`/`property.ssl.*` 抽到结构化字段,原始 SQL 存**占位符**;② **唯一渲染脱敏点**——TVF 的 `toSql()` 构造即脱敏,`DESC PIPE`/`SHOW CREATE`/`information_schema`/profile/异常全经此点;③ **审计 redactor 扩展**——覆盖 `sasl.jaas.config`/`ssl.*.password` 等;④ **不明文落盘**——Phase 1 即避免明文进 image/WAL;AES-at-rest 可留 Phase 3。渲染点/落盘点 file:line 见 DLD §6。

## 18. 状态机与生命周期

```
状态:SUSPEND / RUNNING / ERROR（FINISHED 存在但 Kafka 源永不进入）
RUNNING ─用户 suspend→ SUSPEND ─resume→ RUNNING
RUNNING ─可重试错误(退避自动恢复)→ RUNNING
RUNNING ─致命/数据质量错误(超 max_error_number / schema / auth)→ ERROR ─手动 RESUME→ RUNNING
```

### 18.1 自动恢复 vs 终态
- **可重试(自动退避恢复)**:broker 不可达、全 BE 短暂掉线、txn 瞬时 abort。复刻带退避的有限次自动恢复(窗口内 N 次后锁定为需手动),详见 DLD §4.4。
- **致命(ERROR 终态,仅手动 RESUME)**:超 `max_error_number`(原因 `TOO_MANY_FAILURE_ROWS_ERR`)、schema 不兼容、认证失败。
> RUNNING→ERROR 的判定输入只有两类:连续批次执行失败(可重试计数)、累计 `max_error_number` 超限(数据质量)。per-batch 比例超限不在此列(§13)。

### 18.2 在飞批次的取消（SUSPEND / DROP / 换届）
专用提交器**绕开 TaskManager**,故 `Pipe.suspend()` 原有的"interrupt TaskManager 任务"对 Kafka 批次无效。**KafkaPipeSubmitter 必须跟踪每 pipe 的在飞批次**(txnId/label/Coordinator);SUSPEND/DROP 须 abort 这些 txn、cancel/join coordinator、释放资源车道、归还 consumer;DROP 须等待或强制 abort 在飞 txn 再删 offset 状态;leader 换届时孤儿 txn 按 §11.2 恢复语义处理。详见 DLD §1/§5。

## 19. 存算分离（Shared-Data）适配

| 维度 | 差异 |
|------|------|
| Sink 写入 | OlapTableSink → 对象存储;每 tablet 每版本写 `tablet_metadata` + `txn_log`,异步 publish |
| 提交限速 | 默认开 CommitRateLimiter（score 高→延迟,过高→拒绝）→ 纳入 §10 降级 + `THROTTLED_BY_COMMIT_RATE` |
| 延迟 | publish 慢阈值本身 ~1s;有效延迟下限通常 1–5s,随 tablet/bucket 数增长 |
| **Scan 侧（新 KafkaScanNode）** | CN 分配:partition→scan-range 落到 warehouse 的可用 CN;须与 §9.3 的 sticky partition→instance 协调(避免每批 reshuffle);`computeResource`/warehouse 流入 scan-range location;无本地存储 CN 上的 consumer 池/datacache 行为需明确(warehouse 路由完整集成属 Phase 3)。详见 DLD §5.4。 |
| Warehouse | Pipe `PROPERTIES("warehouse"=...)`,CN 调度 |

## 20. 兼容性

### 20.1 语法兼容
`CREATE ROUTINE LOAD` → Kafka Pipe 内部转换;属性映射见附录 B;无法映射 fail-fast。

### 20.2 PK 写语义 / 错误容忍
见 §12 / §13。

### 20.3 SHOW ROUTINE LOAD 列契约
`SHOW ROUTINE LOAD [TASK]` 的 `TITLE_NAMES` **冻结不动**,feeder 改读 KafkaPipeSource 累加器 + Pipe 状态。列分三档:**直接映射**(Pipe 元数据)/ **由累加器重建**(Statistic/Progress/OffsetLag/ErrorLogUrls)/ **无干净对应须显式重定义**(部分计数列,如 CurrentTaskNum 重定义为当前并行度)。逐列映射明细见 DLD §4.1。把列集作为向后兼容契约**加测试**。`SHOW PIPES` 8 列不变(与 SHOW ROUTINE LOAD 有意分流:前者文件/字节,后者行/offset)。
> 列数口径统一:`22 列 + SHARED_DATA 条件追加 Warehouse`。

### 20.4 升级 / 回滚 / Pulsar
`enable_unified_routine_load`(默认 false)**只接管 KAFKA 源**;**Pulsar 作业留 legacy** 并加断言防误转。**ADMIN MIGRATE**:offset 变换(旧存 next-to-consume,-1 偏移)、特判 `-2/-1` sentinel、迁移前 PAUSED/STOPPED 无在飞 txn、幂等(按源 job id)、回滚 export 反转。回滚契约用前后兼容 journal(optional 字段,不引入新 required opcode)。

## 21. 可观测性

- **SHOW PIPES 增强**:per-partition committed/latest offset 与 lag(latest 来自 §11.2 的周期探测)、`EFFECTIVE_E2E`(+原因)、`ErrorLogUrls`/`TrackingSQL`、`REASON_OF_STATE_CHANGED` 历史、`SCHEDULE_STATUS`(含 `THROTTLED_BY_COMMIT_RATE`/`VERSION_PRESSURE`)、`PARALLELISM_CHANGE_REASON`、`IN_FLIGHT_BATCHES`(txn/label)、`AUTO_RESUME_COUNT`。
- **Metrics**:per-pipe lag/throughput、`BATCH_ABORT_COUNT`、`COMMIT_WAIT_MS`、`BACKPRESSURE_PAUSED`、`pipe_kafka_parallelism_change_total{reason}`、版本/compaction score。
- **审计**:**不逐批写审计/TaskRun 历史**(亚秒摄入会淹没观测面),按 pipe 聚合一条 + 滚动批次历史(供 SHOW ROUTINE LOAD TASK)。

## 22. 配置参数

### 22.1 Pipe 级
| 参数 | 默认 | 说明 |
|------|------|------|
| `target_e2e_latency` | `1s` | 预期延迟（可低至 100ms,尽力而为） |
| `auto_parallelism` / `max_parallelism` / `min_parallelism` | true / 0(自动) / 1 | |
| `max_batch_rows` / `max_batch_size` | `200000` / `100MB` | 声明值;`max_batch_rows` 是 §13 错误窗口基数(窗口=×10=2,000,000),**降级只调运行时有效批大小、不改此声明值**(DLD §3.2) |
| `max_error_number` | `0` | **绝对错误行数;0=零容忍（勿误作"不限"）** |
| `max_filter_ratio` | `1.0` | 比例门(对齐 RL;注意默认翻转,§13) |
| `strict_mode` | `false` | |
| `enable_op_column`/`merge_condition`/`partial_update`/`partial_update_mode` | false/``/false/row | PK 写语义(§12) |
| `warehouse`/`resource_group` | (default) | |
| `task.*`（如 `task.time_zone`/`task.load_mem_limit`） | — | 透传到每批 INSERT 的会话变量 |

### 22.2 FE 全局
`enable_unified_routine_load`(默认 false,仅 KAFKA)、`pipe_kafka_offset_persist_interval_millis`(仅派生缓存)、`pipe_kafka_partition_discovery_interval_s`、`routine_load_kafka_consumer_pool_size`(替代不存在的旧名)、`kafka_pipe_submitter_threads`、`kafka_pipe_max_inflight_batches_per_pipe`(默认 1)。
> 不引入不存在的 `kafka_consumer_pool_size_per_broker`/`kafka_consumer_idle_timeout_ms`。

## 23. 路线图

- **Phase 1（基础 + GA blocker）**:kafka() TVF + KafkaScanNode(FE plan + BE pull);compile-once/rebind + 专用提交器 + 流水线批次;**扩展 InsertTxnCommitAttachment**(offset + filtered/unselected/bytes/tracking,**commit 与 abort 两路**);Offset/exactly-once(attachment 权威 + replay);**凭证脱敏 + 不明文落盘**;**错误容忍对齐**(比例门 + 绝对窗口 + 默认翻转);**错误可观测前置**(error-row 采样 / tracking / per-batch filtered-unselected);consumer 池修复;在飞批次取消。
- **Phase 2**:动态并行度(fresh-gate+max、sticky、compaction-score 降级、冷启动、跨 pipe 公平);**PK 写语义**(行级 `__op` net-new;merge/partial 透传);avro + Schema Registry;in-batch 增量 refill。
- **Phase 3**:`CREATE/SHOW/PAUSE/RESUME/STOP/ALTER ROUTINE LOAD` 兼容 + 列契约;ADMIN MIGRATE + 回滚;Pulsar 留 legacy 断言;Warehouse 集成;凭证 AES-at-rest。
- **Phase 4**:DLQ(前置已在 Phase 1);多 topic;Kafka header;Resource Group 级隔离;仪表盘集成。

---

## 附录 A：业界方案对比矩阵

| 特性 | ClickHouse | Databricks | Snowflake | Flink SQL | StarRocks 现有 | 统一后 |
|------|-----------|------------|-----------|-----------|----------------|--------|
| SQL 原生 | 是(DDL) | 部分 | 否 | 是(DDL) | 是(专用语法) | 是(INSERT+TVF) |
| 标准 INSERT 语法 | 否 | 否 | 否 | 是 | 否 | **是** |
| 并行度单元 | num_consumers | partition | serverless | scan.parallelism | Task 数 | **MPP fragment instance** |
| 动态并行度 | 否 | 否 | 是 | 是 | 否 | **是(自适应+降级)** |
| MPP 跨节点 | 否 | 是 | N/A | 是 | 否 | **是** |
| Exactly-Once | 实验 | 是 | 是 | 是 | 是(txn) | 是(txn+commit-attachment) |
| 端到端延迟 | 秒 | 亚秒~秒 | 5-10s | 毫秒~秒 | 秒 | **预期亚秒~秒(实测自适应,超时降级)** |
| 管理命令 | DETACH/ATTACH | stop/start | ALTER PIPE | SHOW JOBS | SHOW ROUTINE LOAD | **SHOW PIPES + 兼容旧命令** |

## 附录 B：CREATE ROUTINE LOAD 属性兼容映射（摘要）

分类:**TVF**=kafka() 参数;**PIPE**=Pipe 属性;**SESSION**=`task.` 会话变量;**DERIVE**=由 `target_e2e_latency` 推导;**REJECT**=fail-fast;**IGNORE**=接受但无效。

| 属性 | 分类 | 说明 |
|------|------|------|
| `desired_concurrent_number` | PIPE | → `max_parallelism`(迁移建议同设 `min_parallelism` 保静态行为) |
| `max_batch_interval` | DERIVE | → `target_e2e_latency`（有损,旧只是单任务窗口） |
| `max_batch_rows` | PIPE | 保留(无延迟语义对应),默认 200000 |
| `max_batch_size` | IGNORE | 代码中从不被 stmt 解析,接受并忽略 |
| `max_error_number` | PIPE | 绝对计数;**默认 0=零容忍** |
| `max_filter_ratio` | PIPE/SESSION | → `task.insert_max_filter_ratio`;**默认 1.0,勿用 INSERT 默认 0** |
| `strict_mode` / `timezone` | SESSION | → `task.enable_insert_strict` / `task.time_zone` |
| `format` | TVF | csv/json/avro(**无 protobuf**);avro 需 registry url |
| `jsonpaths`/`json_root`/`strip_outer_array` | TVF | 仅 json;后两者与 envelope 互斥 |
| `trim_space`/`enclose`/`escape` / `COLUMNS TERMINATED BY` | TVF | 仅 csv;`COLUMNS TERMINATED BY`→`column_separator` |
| `log_rejected_record_num` / `pause_on_fatal_parse_error` | PIPE | |
| `task_consume_second`/`task_timeout_second` | DERIVE | 由 `target_e2e_latency` 推导(旧固定 4:1 比例不逐字节复现,接受差异) |
| `envelope`(debezium) | TVF | 仅 debezium;要求 json,禁 json_root/strip_outer_array |
| `kafka_broker_list/topic/partitions/offsets` | TVF | → `broker_list/topic/partitions/offsets`(offsets 仅初始;运行位走 KafkaProgress/attachment) |
| `property.*` / `confluent.schema.registry.url` | TVF | 透传(凭证经 §17 脱敏) |
| `group.id` / `group_id` | TVF | `group_id` 首选;`property.group.id` 为兼容别名;**二者都给且不同则报错**;缺省不设(BE 给监控用 id) |

## 附录 C：净新增 / 无法对齐清单（全部接受为 in-scope）

对齐 RL 时无法仅靠配置/SQL 映射、需 net-new 或存在结构性差异的项,全部接受为 in-scope,实现见 DLD:
- **C.1 错误容忍**:绝对 `max_error_number` 窗口(DLD §3)、跨批累计(DLD §3)、`unselectedRows` 当前被丢弃需补读(DLD §3.4)、BE 比例默认翻转(§13)、**全错批次跳过**(DLD §4.3)。
- **C.2 格式**:protobuf 移除(§3.3)、avro+registry net-new(DLD §4.2)、JSON 命名投影保证(§5.2)。
- **C.3 PK 写语义**:行级 `__op` net-new(DLD §2)、partial-update 触发面不同、sort-key+column-mode+DELETE 受限、列名列表 inert 陷阱。
- **C.4 调度执行**:TaskManager 1s tick 硬编码、无 INSERT plan cache(DLD §1)。
- **C.5 兼容映射**:`task_consume/timeout` 比例差异(接受)、`kafka_offsets` 运行位不进 TVF 文本、`group_id` 优先级、Pulsar 留 legacy(§20.4)。
- **C.6 状态机/可观测**:SHOW ROUTINE LOAD 部分列无对应(DLD §4.1)、ERROR 自动恢复(DLD §4.4)。
