# 统一 Routine Load — 实现级设计补充

> **配套** `unified-routine-load-design-v0.3.md`。本文把 v0.3 中三个基础"净新增"件细化到可开工粒度,全部 code-anchored(行号基于 `origin/meegoo/unified-routine-load-6c0a`)。
>
> 三件:**① compile-once / rebind 执行模型**(v0.3 §8)、**② 行级 `__op`**(v0.3 §12.3)、**③ 绝对 `max_error_number` 窗口 + 统计回传**(v0.3 §13.2)。
>
> 标注:**[新增]** 全新代码;**[改]** 改现有;**[复用]** 直接用;**[验证]** 仅核对无需改。

---

## 1. compile-once / rebind-per-batch 执行模型

### 1.1 可行性结论

对抗式验证结论:**feasible-with-changes**。compile-once 骨架成立,deploy 层重绑缝真实存在,但 v0.2/早期表述里"每批只重绑 txn_id"**过于乐观**,有三处必须纠正(§1.3)。确认成立的:计划复用骨架、txn 从 plan 期解绑、并行度经 DAG 重建。

**为什么能复用**:`ExecPlan`(`ExecPlan.java:49-102`)持有 `final` fragments/scanNodes/descTbl,**不含 txn_id / query_id / scan range**;`StmtExecutor.handleDMLStmt` 不建计划,只消费它,并**每次新建 Coordinator**(`StmtExecutor.java:3289`)→ `DefaultCoordinator` 构造时从 `jobSpec.getFragments()` 现建 `ExecutionDAG`(`:285-286`)。scan range 存在 per-coordinator 的 `FragmentInstance.node2ScanRanges`(`FragmentInstance.java:82`),不在共享 `ScanNode` 上。`TFragmentInstanceFactory.toThriftFromCommonParams:121` **每次 deploy 都 `fragment.toThrift()` 重新序列化**——这正是重绑点。

### 1.2 复用什么 / 每批做什么(修正版)

| 量 | 处理 | 机制 / 锚点 |
|----|------|------|
| optimizer 输出 / `PlanFragment` 树形状 | **缓存复用**(真正的 compile-once 收益:省 optimizer + fragment-build) | `ExecPlan.java:52-77` |
| query_id / fragment_instance_id | 每批新生成(trivial) | `JobSpec.setQueryId:374` → `TFragmentInstanceFactory:129/236` |
| Coordinator / CoordinatorPreprocessor / ExecutionDAG | 每批现建(便宜,O(fragments),无优化器) | `DefaultCoordinator:285-286` |
| 并行度(pipeline_dop / instance 扇出) | 每批从同一 fragments 重建 DAG,**不 re-optimize** | `TFragmentInstanceFactory:205`(dop 为 instance 级) |
| Kafka offsets | 每批经 **scan range** 下发(净新增,§1.3-C) | `TFragmentInstanceFactory:237` `setPer_node_scan_ranges` |
| txn_id + label | 每批开新 txn,**并重跑 sink complete()**(§1.3-A) | `OlapTableSink.init:202` / `complete:336-371` |

### 1.3 三处必须纠正(来自验证)

**(A) `OlapTableSink` 烘焙的不止 txn_id —— 每批必须 re-complete sink(最大纠正)。**
`OlapTableSink.complete()`(`InsertPlanner.java:490`;`OlapTableSink.java:336-371`)把 **partition 参数**(`createPartition`,含 auto-partition shadow 解析 `:368`)、**tablet/replica LOCATION map**(`createLocation:371`)、**nodes_info**(`:372`)、以及 txnState 派生值(`:366`)烘焙进缓存的 `tDataSink`;`toThrift()` 只返回缓存(`:438`)。长驻 pipe 里这些会**漂移**:auto-partition 每日/小时新建分区、tablet clone/迁移/下线改 location、节点增减。若只 `set txn_id`,会把 stale location 再次下发 → 写到已迁移/死 tablet。**这正是现有 Routine Load 每个 task 都重跑 `complete()` 的原因。**
- **方案**:`resetForReuse(loadId, txnId, label)` 必须**重跑 `complete()`**(重解析 partition + location + schema + nodes),而非只设 txn_id。真正的每批成本是"re-complete sink",但仍省下 optimizer + fragment-build(compile-once 的真实收益)。
- **快路径优化(已定:便宜 epoch 主动门 + deploy-failure 兜底)**:每批比对一个便宜 epoch,**仅在变化时**才 re-complete,稳态跳过。
  - **便宜 epoch = `( lastSchemaUpdateTime, hash(partition_id_set), hash({backendId, isAlive}) )`**,全是 leader 本地、O(分区)/O(节点)级、无 RPC。核实结论:schema(`OlapTable.lastSchemaUpdateTime`,`PrepareStmtContext` 同款)、partition 集、node-set 三维**都有便宜戳**。
  - **tablet-location 漂移无便宜表级戳**:只有 per-tablet/per-backend 粒度。**已定不走** backend-report-version 代理(`SystemInfoService.idToReportVersionRef` 每几秒就 bump → 几乎每批失效,毁掉优化)。改为 **deploy-failure 兜底**:stale location 导致该批 deploy/写失败(tablet not found / 副本不可达 / NodeChannel 打开失败)→ 可重试错误 → abort → 强制 re-complete + 重试该批(复用 §1.7 / v0.3 §15 的 abort-retry)。
  - **为何够用**:node-set epoch **含 `isAlive`** → **BE 掉线**(最常见的 location 漂移触发)已被主动门捕获;残余的 clone/迁移/balance(节点集稳定下)较少见,由兜底吸收,代价仅偶尔一批重试。**Phase-2 可选优化**:新增 per-table `replicaLocationEpoch`(`AtomicLong`,在副本状态变更/clone 完成/迁移路径 bump)做精确化——净新增 instrumentation,profiling 显示重试偏多再上。

**(B) 并发同 pipe 批次会 race 共享 `RuntimeFilterDescription`(SELECT 带 join 时)。**
`ExecutionFragment.setLayoutInfosForRuntimeFilters()`(`ExecutionFragment.java:154-163`,由 `TFragmentInstanceFactory.toThriftFromCommonParams:113` 调用)把 per-run 的 instance 数 / bucket-seq 写进**共享的** `planFragment.getBuildRuntimeFilters()` 对象。无 join 的 `INSERT...SELECT FROM kafka()` 是 no-op;但 SELECT 含 join(维表查找——流式 ETL 常见)时,这些共享子对象每批被改写。
- **方案**:`kafka_pipe_max_inflight_batches_per_pipe` 默认 **1**(单批在飞,last-writer-wins 正确);仅当 SELECT 为 **join-free** 才允许该 pipe 多批在飞;或对需要多批的场景每批 deep-copy fragment 树(部分抵消缓存收益)。

**(C) offsets-in-scan-range 是净新增,不是"复用"。**
今天**没有** `TKafkaScanNode`/`TKafkaScanRange`(`PlanNodes.thrift` 的 `TScanRange` union 止于 `TBenchmarkScanRange:478-491`),BE 也无 pull 式 Kafka scan operator(现 RoutineLoad 是 push 式 `KafkaConsumerPipe→StreamLoadScanNode`,offset 走 `TKafkaLoadInfo` sidecar `BackendService.thrift:62`)。增量 scan-range 重绑缝(`DefaultCoordinator.assignIncrementalScanRangesToDeployStates:697-748` 由 `scanNode.hasMoreScanRanges():718` 门控;`CoordinatorPreprocessor.assignIncrementalScanRangesToFragmentInstances:282`;`TFragmentInstanceFactory.createIncrementalScanRanges:96`)**真实可复用**,但只有在新 FE planner 节点 + thrift + BE operator 落地后才能用。
- **已定:offsets-in-scan-range 拉模型**(新 `TKafkaScanNode`/`TKafkaScanRange` + BE pull operator)。理由:对"INSERT...SELECT FROM kafka() 作真正 MPP INSERT 跑"这一前提,**BE pull scan operator 两种方案都躲不掉**(push 的 `KafkaConsumerPipe→StreamLoadScanNode` 不在 pipeline 引擎里),所以 sidecar 的"省 BE"是错觉——它只省一个 thrift struct,却把 offset 推到旁路 RPC、保留 legacy 双码路径、且不契合 compile-once(offset 不是 deploy 期 instance 参数)。scan-range 是唯一同时拿到 MPP 执行体 + compile-once + 单码路径的方案。pull operator 是 BE 最大单项,放 **Phase 1**(地基);in-batch 增量续填作 Phase 2 低延迟优化(见 §1.8)。

### 1.4 必解 blocker:txn 在 plan 期被焊死

`StatementPlanner.plan` 对 DmlStmt 在 plan 期调 `beginTransaction`(`:119` → `:517` → `stmt.setTxnId:637`),把 txnId 焊到 AST。复用要求计划**无 txn**。
- **方案**:建缓存计划时用现有早退守卫(`:520` `session.getTxnId()!=0`,`:543` txnId 已设)使 plan 跳过开 txn;每批由专用提交器自己 `beginTransaction` 并经 `resetForReuse` 绑回。建议加 `InsertStmt`/`ConnectContext` 上的 typed flag(而非复用 session sentinel)以免与显式多语句事务混淆(`StatementPlanner.java:520`)。

### 1.5 touch points

- `gensrc/thrift/PlanNodes.thrift` **[新增]**:`TKafkaScanNode`(挂 `TPlanNode`,`table_function_node=54` 后下一空序号;放**结构性** brokers/topic/properties/format/registry_url)+ `TKafkaScanRange`(挂 `TScanRange` union,`benchmark_scan_range=40` 后下一空序号;放**每批** partition_id/offset_begin/offset_end(-1=至超时)/consume_timeout_ms/max_batch_rows/max_batch_size)。
- `fe .../planner/KafkaScanNode.java` **[新增]**:`extends ScanNode`;`getScanRangeLocations(maxLen)` 每 partition 出一个 `TScanRangeLocations(TKafkaScanRange)`(分到该 partition 所属 CN);`hasMoreScanRanges()=true`;`setBatchOffsets(...)` 每批改。仿 `StreamLoadScanNode`(`StreamLoadPlanner.java:195`)+ `ScanNode.java:152/189`。
- `fe .../planner/OlapTableSink.java:198/336` **[改]**:抽 `resetForReuse(loadId, txnId, label)` —— **重跑 `complete()`**(partition+location+schema+nodes)+ 重设 txn_id + 重解析 txnState;配 epoch 门跳过稳态(§1.3-A)。
- `fe .../sql/StatementPlanner.java:119/517` **[改]**:pipe 建计划路径走 txn-free(§1.4)。
- `fe .../load/pipe/KafkaPipeExecPlanCache.java` **[新增]**:per-pipeId `{execPlan, sinkEpoch=(lastSchemaUpdateTime, partitionSet hash, nodeSet+isAlive hash), parallelism}`;`sinkEpoch` 变化 → re-complete sink(§1.3-A);schema/partition 变化 → 整计划失效重建;失效判定仿 `PrepareStmtContext.java:73`。**注**:tablet-location 不进 `sinkEpoch`,靠 deploy-failure 兜底(§1.3-A)。
- `fe .../load/pipe/KafkaPipeSubmitter.java` **[新增]**:专用执行器,见 §1.6。
- `fe .../qe/StmtExecutor.java:3289-3463` **[改/抽取]**:把 insert 内循环抽成 `runOneBatch(cachedPlan, txnId, label, offsets)`(`createInsertScheduler → exec → join → 读 counter → commit/abort`),供提交器调用而不重建计划。
- `fe .../qe/scheduler/dag/JobSpec.java:374` **[复用/加]**:`setQueryId` 复用;加 `Factory.fromKafkaPipeBatch(cachedFragments, scanNodes, descTbl, queryId, queryOptions)` 仿 `fromQuerySpec:207`。
- `fe .../qe/CoordinatorPreprocessor.java:282` + `DefaultCoordinator.java:697` **[复用/接线]**:接 `KafkaScanNode.hasMoreScanRanges()`,设 `jobSpec.isIncrementalScanRanges(true)`。
- `be/src/exec` **[新增]**:pull 式 `KafkaScanNode`/`ChunkSource`,复用 `be/src/runtime/routine_load` `DataConsumer/DataConsumerPool` 做 `seek(part, begin)` 读到 end/超时。
- `fe .../common/Config.java` **[新增]**:`kafka_pipe_submitter_threads`(默认 ~16)、`kafka_pipe_max_inflight_batches_per_pipe`(默认 **1**,§1.3-B);按仓库规范同步 `docs/en` + `docs/zh`。

### 1.6 专用低延迟提交器

`KafkaPipeSubmitter` **[新增]**:**非 tick、事件驱动**;cached 线程池(仿 `RoutineLoadTaskScheduler.java:85-87` 的 `LinkedBlockingQueue` + `newCachedThreadPool`),**不走** `pipe_scheduler` 1s tick,**不受** `task_runs_concurrency=4`(`TaskRunScheduler.java:127`)。一批 `coord.join()` 提交完立即算下批 offset 再提交(背靠背,无固定 sleep)。offset 进度复用 RoutineLoad `KafkaProgress`(持久化),重启可续。

### 1.7 边界与失效

- **空批次** → 不开 txn 直接短路(仿 `StmtExecutor:3445` `loadedRows==0`),否则 txn churn 刷爆 `GlobalTransactionMgr`。
- **query_id 唯一** → 每批唯一(`registerQuery` 按 query_id,`:3345`),否则同 pipe 并发批次注册冲突。
- **schema 变更** → 按 `table.lastSchemaUpdateTime` 失效重建,从保存 offset 续。
- **partition 集变更** → 失效;缩容清掉消失 partition 的 scan range(否则 BE 重扫旧 offset,即 `CoordinatorPreprocessor:288-298` 注释的 stale-bucket 坑)。
- **节点集变更** → 每批用新 `WorkerProvider` 重建 DAG;若 scan-locality 假设变,full 失效。
- **OlapTableSink dop 约束**(PK/lake 表,`StreamLoadPlanner.java:275-`):sink dop 受限,scan dop 高于 sink dop 须经 exchange,该 exchange 必须在缓存计划里;保留 `StreamLoadPlanner` 的 `load_dop` 逻辑。
- **重启 commit/abort 竞态** → 在飞批次 txn 可恢复;`KafkaProgress` 仅在 commit visible 后持久化,从已提交 offset 续(复用 RoutineLoad 语义)。

### 1.8 已定(决策记录)

1. **offset 传输:scan-range 拉模型**(非 sidecar)——见 §1.3-C 理由(MPP 前提下 pull operator 躲不掉,sidecar 省 BE 是错觉)。
2. **专用提交器:新建 `KafkaPipeSubmitter`,照搬 `RoutineLoadTaskScheduler` 模型**(cached 池 + 阻塞队列),不直接复用 legacy 类——它的非 tick 模型正确但直接复用会耦合要退役的代码,镜像到新类干净解耦。
3. **每批事务粒度:pipe×batch 全分区单 txn**——exactly-once 最简(齐进齐退)、txn 数最少(配合 v0.3 §10 降版本压力)、`resetForReuse` 映射简单;partition-group 仅在单批扇入过大时作后置优化。
4. **in-batch 增量 refill:Phase 2**——Phase 1 用"一批=一 deploy=一 txn"(简单),refill(BE 长驻 fragment + offset 推进 RPC)作 Phase 2 低延迟优化;compile-once + 背靠背提交已拿到大部分收益。
5. **re-complete epoch 门:便宜 epoch 主动门 + deploy-failure 兜底**——详见 §1.3-A(明确弃用 backend-report 代理)。

---

## 2. 行级 `__op`(UPSERT / DELETE)

### 2.1 BE 契约(位置式,零 thrift 改动)

op 完全由"**PK 表 + 输出 tuple 最后一个 slot 名为 `__op`(TINYINT)**"激活,BE 侧已完整实现:`tablet_sink.cpp:814`(`slots().back()->col_name()=="__op"`)、`memtable.cpp:104-122`(`_has_op_slot`)、`:310-317`/`:482-499`(split,ndel==0 短路)、`delta_writer.cpp:248-254/405-414`。**无 `TOpType`/`TOlapTableSchemaParam`/BE 改动**。陷阱:`OlapTableSink` 对每个 PK 表都把 `__op` 加进**索引列名列表**(`OlapTableSink.java:478-480`),但那是 inert 的——BE 只认尾 slot;仅靠列名列表条目**不**启用删除。

### 2.2 五件套

**(1) kafka() TVF 产出 `__op` 列。**
- JSON(CDC 默认):TVF 声明一个虚拟 `__op` TINYINT 输出列;用户未显式映射时,BE kafka json reader **自动提取** `__op` key 或 CDC envelope op(仿 `json_scanner.cpp:300/586-588` + `Load.java:417-418` null-expr + isLoadJson);key 缺失填默认 UPSERT(0)。
- CSV/raw:无自动 key,用户显式投影 `... 'delete' AS __op` 或 `CAST(c4 AS TINYINT) AS __op`。
无论哪种,relation 分析完时已有一个名为 `__op`、(可强转)TINYINT 的输出列。

**(2) Analyzer:识别/转换 op 列,强制 TINYINT。**
在 `InsertAnalyzer`(~`:318` 目标列算完后)新增分支:当 (a) `enable_op_column=true`、(b) `Load.tableSupportOpColumn(table)`、(c) query relation 有非真实表列的输出列名 `__op`(大小写不敏感)时:置 `insertStmt.hasOpColumn=true`;把 `__op` **从数据列计数中剔除**(不进 `targetColumns`/`mentionedColumns`,使 `:346-348` 的计数校验仍按数据列对齐);强转 op:`'upsert'/'delete'` StringLiteral → `IntLiteral(TOpType 0/1)`(复用 `Load.java:392-404`),否则 `CAST(... AS TINYINT)`。校验:`__op` 不得与真实列重名;只允许一个;仅 PK OlapTable;非 PK 或门关却出现 `__op` → `SemanticException`。op 列记在 `InsertStmt` 专属字段,**不**折进 `targetColumns`(保持数据列匹配长度正确)。

**(3) `InsertPlanner` 改动:追加尾 `__op` slot + 绑定 op ColumnRef(两处协同)。**
- **3a.** 在 `outputFullSchema` slot 循环(`InsertPlanner.java:394-407`)之后、`computeMemLayout(:408)` 之前,若 `hasOpColumn()`:`addSlotDescriptor` 追加**一个** TINYINT、非空、materialized、名为 `Load.LOAD_OP_COLUMN` 的 slot(克隆 `LoadPlanner.java:424-428`)→ 使最后 slot 为 `__op`,满足 `tablet_sink.cpp:814`。
- **3b.** 新增 `fillOpColumn` 步,插在 `castOutputColumnsTypeToTargetColumns` 之后(`:360` 后),把 op 表达式(literal 的 `ConstantOperator(TINYINT)`,或源 query 名为 `__op` 的输出 ColumnRef 经 `CastOperator(TINYINT)` 仿 `:965-966`)作为 `outputColumns` 的**最后一个**元素追加(经 `LogicalProjectOperator`/`withNewRoot`)。使 `outputColumns` 长度 = tuple slot 数(满足 `tablet_sink.cpp:268-279` 计数+类型校验)。**务必最后追加**,且不要让 op 列走 `fillDefaultValue`/`fillGeneratedColumns`/cast 循环(它们只遍历 `outputFullSchema`,天然不含 `__op`)。
- **3c.** `OlapTableSink` 无需改:`createSchema` 已对 PK 无条件加 `LOAD_OP_COLUMN`(`:478-480`);tuple 现已匹配。

**(4) opt-in 门 `enable_op_column`。**
新 session var `ENABLE_OP_COLUMN`(`SessionVariable.java`,默认 **FALSE**)。仅当 true 才把 `__op` 输出列当作 op 指令;false 时 `__op` 投影按普通列处理 → 无该表列 → 显式报 `Unknown column '__op'`(`InsertAnalyzer.java:282`)。即**普通 INSERT 永不长出 op slot**,失败是显式报错而非静默删除。默认 false 保证现有 INSERT 字节不变;kafka()→Pipe 重写对 PK 目标**自动开启**(CDC 开箱即用),普通 INSERT 保持关。

**(5) 与 column-mode partial update + sort key 的交互。**
- `hasOpColumn && usePartialUpdate`:op slot 仍须是 tuple 尾 slot;`__op` **不进** partial-update 输出 schema(`inferOutputSchemaForPartialUpdate:227-288` 只遍历真实列,3a 在缩减后的循环之后追加,天然排除)。保证缩减 schema 后 op slot 仍最后。
- DELETE 行只需有效 PK(`memtable.cpp:514-529` 仅 PK 编码 delete),非 key 列可空。
- 已知限制:column-mode partial update + DELETE 在 sort-key 表上受 `delta_writer.cpp:402-424` 约束(混 upsert/delete 可能 NotSupported);向用户暴露。

### 2.3 touch points

- `InsertPlanner.java:plan` **[改]**:3a 追加尾 `__op` slot(克隆 `LoadPlanner.java:424-428`)。
- `InsertPlanner.java:plan` + `fillOpColumn` **[新增方法]**:3b,op ColumnRef 作 `outputColumns` 末元素。
- `InsertAnalyzer.java`(~`:228-349`)**[改]**:识别 `__op` 输出列、排除计数、强转、校验、记 `InsertStmt`。
- `InsertStmt.java` **[新增字段]**:`hasOpColumn` + op 表达式/源输出名(仿 `usePartialUpdate` `:62/308-313`)。
- `Load.java:normalizeOpColumnExpr` **[新增/重构]**:抽 `:392-404` 的 `'upsert'/'delete'`→`IntLiteral(TOpType)` 供复用。
- `SessionVariable.java` **[新增]**:`ENABLE_OP_COLUMN`,默认 false。
- `OlapTableSink.java:478-480` **[验证]**:无需改,核对 slot 数。
- kafka() TVF relation **[新增]**:声明可选尾 `__op` TINYINT;json 自动绑 `__op`/CDC op;csv/raw 由投影提供。
- `be/src/exec` kafka json reader **[新增]**:复制 `json_scanner.cpp:300-301/586-588` 的 `__op` 自动提取 + 默认 UPSERT 填充。
- **无改动**:`gensrc/thrift`(`TOpType`/`TOlapTableSchemaParam`)、`tablet_sink.cpp`、`memtable.cpp`、`delta_writer.cpp`。

### 2.4 边界

- **count+type 不变式**:尾 `__op` slot 必须有恰好一个 TINYINT 输出表达式对应(`tablet_sink.cpp:268-279`)。
- **顺序**:`__op` 必须**严格最后**(`memtable._split_upserts_deletes` 弹最后列;`tablet_sink.cpp:814`/`delta_writer.cpp:405` 看 `slots().back()`)。`fillOpColumn` 必须在所有 fill*/cast 之后,且后续步骤(iceberg shuffle 投影 `:652-660`、generated/shadow 列)不得在 op 之后再追加。
- **门关却有 `__op`** → 显式报错(`Unknown column __op`),绝不静默当 op 或静默丢(防 CDC 数据损坏)。
- **非 PK 表 `__op`** → analyzer 拒(`tableSupportOpColumn` false)。
- **列数校验**:`InsertAnalyzer:346-348` 须把 `__op` 从 query relationFields 计数中减掉,否则 `ERR_INSERT_COLUMN_COUNT_MISMATCH`。
- **DELETE 行默认值**:full INSERT(非 partial)仍要求 NOT NULL 列被投影(per-statement,非 per-row),即使 delete-only 流;文档说明。
- **auto-increment + DELETE**:`tablet_sink.cpp:814-841` 对 delete 行 zero-init auto-inc,依赖尾 slot 为 `__op`;确保 op slot 存活到运行期。
- **match-by-name INSERT**(`InsertAnalyzer:230-244`):须把 `__op` 从 `targetColumnNames` 过滤掉,否则当真实列查找而失败。
- **literal 规范化**:仅 `'upsert'/'delete'` 或 0/1,其余报错(对齐 `Load.java:402-407`)。

### 2.5 已定(决策记录)

1. **`enable_op_column` 默认 FALSE 全局 + kafka()→Pipe 对 PK 自动开**——现有 INSERT 字节不变(安全),CDC 开箱即用;不做 per-statement hint(防误用)。
2. **op 列名硬定 `__op`**——BE 只认字面 `__op` 尾 slot;可配名也得投影 `AS __op`,零收益徒增表面。Debezium 的 `op` 字段经 SELECT 投影成 `__op`。
3. **CDC `c/r/u/d`→`TOpType` 在 FE 投影**——`envelope=debezium` 时由 TVF/重写注入 `CASE ... AS __op`;让 BE 保持格式无关、复用既有 literal→TOpType analyzer。仅裸 `__op`(整型/upsert-delete key)走 BE 自动提取(对齐现有 Load)。
4. **`__op` + `merge_condition` 冲突:FE 分析期拒绝**——`memtable.cpp:500-503` 运行期禁 delete+merge_condition,FE fail-fast 优于 BE 运行期报错,对齐 Load 语义。
5. **v0.3 限 streaming/Pipe 路径**——`enable_op_column` 仅由 kafka()→Pipe 重写有意义地置上;改动天然通用,将来翻 flag 即可放开"SQL 通用行级 upsert/delete"。

---

## 3. 绝对 `max_error_number` 窗口 + 统计回传

### 3.1 决策:超阈值 → ERROR 态(手动 RESUME)

**[已定]** 累计错误超 `max_error_number` 时,pipe 进入 **`State.ERROR`**,需 `ALTER PIPE RESUME` 手动恢复——**不是** RL 式可恢复的 SUSPEND/PAUSED。

> 与 v0.3 §18.1 一致并互补:**瞬时错误**(broker 抖动 / BE 掉线)走带退避的**自动恢复**;**数据质量错误**(超 `max_error_number`)是终态 `ERROR`,**不自动恢复**,只手动 RESUME。`Pipe.State.canResume()` 对 `ERROR` 返回 true,故 `ALTER PIPE RESUME` 可用。

### 3.2 `KafkaPipeSource` 滑动窗口累加器

新类 `com.starrocks.load.pipe.KafkaPipeSource`(仿 `FilePipeSource`)。字段(`@SerializedName` 持久化,仿 `RoutineLoadJob.java:281-291`):
- 累计:`totalRows / errorRows / unselectedRows / receivedBytes`
- 窗口:`currentErrorRows / currentTotalRows`(**持久化**,使 leader 换届不重置半填窗口)
- 限额:`maxErrorNum`(默认 0,RL `DEFAULT_MAX_ERROR_NUM`)、`maxBatchRows`(默认 `DEFAULT_MAX_BATCH_ROWS`)、`maxFilterRatio`(默认 **1.0**,对齐 RL,见 §3.7)
- 错误样本:`transient Queue<String> errorLogUrls = EvictingQueue.create(3)`(gsonPostProcess 重建,仿 RL/FilePipeSource)

方法 `updateNumOfData(numTotal, numError, numUnselected, receivedBytes, isReplay)` —— **逐字移植** `RoutineLoadJob.updateNumOfData`(`:829-896`):累加 累计+窗口 计数;`currentTotalRows > maxBatchRows*10` 时:若 `currentErrorRows>maxErrorNum && !isReplay` → **pause(进 ERROR)**,然后重置窗口;否则重置窗口。窗口未满但 `currentErrorRows>maxErrorNum && !isReplay` 也 pause(`:877-895`)。**pause 动作 = `pipe.changeState(State.ERROR)`**(§3.1),带 RL 式消息 `"Current error rows: N is more than max error num: M"`(keyed `max_error_number`)。`replay` 路径 **不重复 pause**(`isReplay=true`),靠状态变更单独 replay(与 RL 同)。

### 3.3 扩展 `InsertTxnCommitAttachment`

`InsertTxnCommitAttachment.java` 加 `@SerializedName` 字段(**仅 Gson,非 thrift struct,无序号管理**):`filteredRows / unselectedRows / receivedBytes / trackingUrl`(保留 `loadedRows`)。加 fluent setter(`setFilteredRows` 等)供 `StmtExecutor` 调,**不加**冲突的位置 ctor(保持 `InsertOverwriteJobRunner`/`OlapDeleteJob`/统计采集等现有调用点兼容)。旧 edit-log 记录把新字段反序列化为 0/null —— 向后兼容,无序号复用。

> 注:任务里提到的 `TKafkaConsumeReport` 在本库**不存在**;流式 Pipe 的统计通道是 FE 内部的 `InsertTxnCommitAttachment`,不是 BE thrift report。若后续要 BE 推送,正确做法是给现有 struct 加 optional 字段、绝不复用序号;但 v0.3 不需要。

### 3.4 `StmtExecutor` 改动

`handleDMLStmt` 的 counter 读块(`:3405-3414`):新增读 `coord.getLoadCounters().get(LoadJob.UNSELECTED_ROWS)`(key 已存在,BE 已发 `exec_state_reporter.cpp:117`,只是 StmtExecutor 从不读);capture `trackingUrl = coord.getTrackingUrl()`(`Coordinator.java:219`)。在 attachment 构造点(`:3540-3567`)调 `attachment.setFilteredRows(...).setUnselectedRows(...).setReceivedBytes(...).setTrackingUrl(...)`。门控到 streaming-insert 路径(普通 INSERT 无害)。`receivedBytes` 复用 sink 侧 `loaded.bytes`(已定,§3.8)。

### 3.5 恢复

- **实时**:Pipe 注册 `TxnStateChangeCallback`,在 `afterCommitted(txnState)`(`TransactionState.java:816-844`)取 `InsertTxnCommitAttachment`(`:720`),调 `kafkaPipeSource.updateNumOfData(loaded+filtered+unselected, filtered, unselected, receivedBytes, false)`;若 `trackingUrl!=null && filtered>0` 则 `errorLogUrls.add(trackingUrl)`。仿 RL `afterCommitted`(`RoutineLoadJob.java:1047-1080`)含 PAUSE-on-throwable 兜底。
- **replay**:实现 `replayOnCommitted`(仿 `RoutineLoadJob.java:1083-1093`),`updateNumOfData(..., isReplay=true)`,使重启后计数收敛而不二次 pause;`replaySetTransactionStatus`(`TransactionState.java:846-862`)已把 COMMITTED 路由到 `replayOnCommitted`。
- **持久化**:累加器 + 窗口计数随 `AlterPipeLog` 的 `LoadStatus`(`Pipe.finalizeTasks:436-446`)piggyback;扩 `LoadStatus`(`Pipe.java:762-797`)加 `errorRows/unselectedRows/receivedBytes` 并在 `cloneForUpdate` 拷贝;窗口计数在 `KafkaPipeSource`(本身 `@SerializedName` 于 `Pipe` 内,随 Pipe 落日志时持久化)。

### 3.6 错误样本 → SHOW PIPES

`KafkaPipeSource.errorLogUrls`(`EvictingQueue.create(3)`,仿 `RoutineLoadJob.java:321`)。`ShowResultMetaFactory.visitShowPipeStatement`(`LAST_ERROR` 后 `:678`)加列 `ERROR_LOG_URLS`,`ShowPipeStmt.handleShow`(`:75` 后)`row.add(Joiner.on(", ").join(errorLogUrls))`,按 type==KAFKA 门控(FILE pipe 留空)。保持 meta 与 handleShow 列序一致(`ShowPipeStmt.java:61-63` 自带 NOTE)。

### 3.7 默认翻转告警(必须处理)

`insert_max_filter_ratio` 默认 **0**(`SessionVariable.java:1656`)——任一过滤行即在 `StmtExecutor.java:3422` 失败整批;而 RL `maxFilterRatio` 默认 **1.0**(`RoutineLoadJob.java:215`,不因比例 abort,把滑动窗口 `max_error_number` 留作唯一 pause 触发)。**kafka pipe 生成的每批 INSERT 必须把 `MAX_FILTER_RATIO_PROPERTY` / 会话变量设为 pipe 的 `maxFilterRatio`(默认 1.0)**(类比 `FilePipeSource.buildInsertSql`),否则每批一条坏行就在 `:3422` abort,滑动窗口成死代码。

### 3.8 touch points + 已定决策

**touch points**:`KafkaPipeSource.java`[新增累加器]、`InsertTxnCommitAttachment.java`[加 Gson 字段]、`StmtExecutor.java:3405-3414/3540-3567`[读 unselected + 填 attachment]、`Pipe.java:762-797`[扩 LoadStatus]、`KafkaPipeSource.replayOnCommitted`[新增]、`ShowResultMetaFactory.java:678` + `ShowPipeStmt.java:75`[加 ERROR_LOG_URLS 列]。

**已定(决策记录)**:
1. **`receivedBytes` 复用 sink 侧 `loaded.bytes`**(文档注明语义为 loaded 而非 source-consumed)——零新增 BE 工作;source/sink 字节差异运维上极少要紧,需要再加 source 计数后置。
2. **三个 quota 做 CREATE PIPE 属性**(`PipeAnalyzer`,持久化在 `KafkaPipeSource`),非会话变量——它们是 per-pipe 持久预算(须随重启/edit-log 存活);每批 INSERT 的 `SET_VAR`(`enable_insert_strict`/`insert_max_filter_ratio`)在提交时由属性派生。对齐 RL job 属性。
3. **`ERROR_LOG_URLS` 做 SHOW PIPES 新列**——SHOW PIPES 本就在 v0.3 §21 扩列;与 `SHOW ROUTINE LOAD.ErrorLogUrls` 1:1,dashboard 解析离散列比解 blob 干净(注意 meta 与 handleShow 列序对齐)。

---

## 4. 附录 C 其余 net-new 项的实现设计(全部接受为 in-scope)

> §1-3 已覆盖 compile-once / `__op` / `max_error_number`(对应附录 C 的 C.3/C.4/C.1 主项)。本节细化剩余被接受的净新增项。

### 4.1 SHOW ROUTINE LOAD / SHOW ROUTINE LOAD TASK 逐列契约(C.6.19)

**策略**:`SHOW ROUTINE LOAD [TASK]` **不重指向** SHOW PIPES 列;`TITLE_NAMES` 与 `RoutineLoadJob.getShowInfo()`(`:1590-1668`)字节不变,只把 feeder 方法改成读 **KafkaPipeSource 累加器 + Pipe 状态**(替代 legacy task scheduler)。`SHOW PIPES` 的 8 列(`ShowResultMetaFactory.visitShowPipeStatement:670`)**保持不变**——两视图有意分流:SHOW PIPES 面向文件/字节,SHOW ROUTINE LOAD 面向行/offset。

**累加器字段**(`KafkaPipeSource`,由每批 commit-attachment 喂):`totalRows / errorRows / unselectedRows / receivedBytes / totalTaskExecMs / committedBatchNum / abortedBatchNum / committedOffsets{p:off} / latestOffsets{p:off} / lastCommitTime / currentParallelism / errorLogUrls(EvictingQueue(3))`。

**逐列三档映射**(`ShowRoutineLoadStmt.TITLE_NAMES:59-89`,22 列 + 条件 `Warehouse`):
- **直接映射(Pipe 元数据)**:`Id/Name/CreateTime/DbName/TableName` ← Pipe;`DataSourceType`=常量 `KAFKA`;`State` ← `Pipe.State` 映射 JobState(RUNNING→RUNNING;SUSPEND→PAUSED;ERROR→PAUSED+reason;FINISHED→STOPPED);`JobProperties/DataSourceProperties/CustomProperties`(后者复用 `getMaskedCustomProperties` 掩码 `*password*/*secret*`);`Warehouse`。
- **由累加器重建**:`CurrentTaskNum`=`currentParallelism`(**语义变**:旧=task 列表大小含等待;新=当前并行度,需文档化);`Statistic`=按累加器重建 11-key JSON(`totalRows/loadedRows=total-error-unselected/errorRows/unselectedRows/receivedBytes/taskExecuteTimeMs/receivedBytesRate/loadRowsRate/committedTaskNum/abortedTaskNum/partitionLagTime`);`Progress`={p:committedOffset};`LatestSourcePosition`={p:latestOffset};`OffsetLag`={p:max(0,latest-committed)}(保留特殊 offset 过滤 `checkProgressVal`);`ErrorLogUrls`←EvictingQueue(3);`TrackingSQL`=同款硬编码串但 keyed by pipeId(需 `load_tracking_logs` 以 pipeId 为 job_id);`TimestampProgress`=`"{}"`(若未接 Kafka 时间戳→offset 查询,**必须空 JSON 非 null**)。
- **无对应→常量/弃用**:`PauseTime/EndTime`(Pipe 无,可加 `lastSuspendTime`/`endTime` 或空);`ReasonOfStateChanged` ← `Pipe.lastErrorInfo`;`OtherMsg`=空。

**SHOW ROUTINE LOAD TASK**:把 "task" 映射为 pipe 的**最近/在飞批次**;`TaskId`=批次 UUID、`TxnId/TxnStatus` ← 该批 load txn、`BeId` ← 执行该批 fragment 的 CN、`DataSourceProperties`=`"Progress:{p:beginOff},LatestOffset:{p:latestOff}"`(同 `KafkaTaskInfo.getTaskDataSourceProperties:221`)。若 v0.3 不留已提交批次历史,则只列在飞批次(语义等同今天只列 live task)。

**touch points**:`RoutineLoadJob.getShowInfo:1590` / `getStatistic` / `getSourceProgressString:675` / `getSourceLagString:677` [改:feeder 换累加器];`KafkaRoutineLoadJob.getStatistic:490` [改:累加器→JSON];`RoutineLoadTaskInfo.getTaskShowInfo:276` [改:每在飞批次一行];`KafkaPipeSource` [新增累加器+序列化];`ShowRoutineLoadStmt/ShowRoutineLoadTaskStmt.TITLE_NAMES` [**不改**,契约冻结]。
**边界**:`totalTaskExecMs` 保 `>=1` floor(否则 rate 计算除零,`RoutineLoadJob.java:293`);`SHARED_DATA` 的 `Warehouse` 在两个 title 列表里**条件追加**,Pipe 路径须在同位置 emit 否则列数错位;`getShowInfo` 持 readLock 单快照,累加器读须快照一致(避免 Statistic/Progress/OffsetLag 撕裂);State 坍缩(Pipe 4 态 vs JobState 5 态)使 grep `State='CANCELLED'/'NEED_SCHEDULE'` 的工具静默失配——文档化。

### 4.2 avro + Confluent Schema Registry(C.2.7)

**决策:复用 `AvroScanner` + libserdes(Option A,零改动 scanner)**。kafka() TVF 的 BE pull operator 只需保证:(a) registry-framed 消息字节一条一 buffer 到达解码器,(b) confluent URL 到达解码器。**不用** `AvroCppScanner`(容器读),**不重写** magic-byte/schema-id 解析(libserdes 负责)。

- **thrift**:`TKafkaScanNode` 加 `optional string confluent_schema_registry_url` + `optional TFileFormatType format` + jsonpaths/列映射(镜像 `TBrokerScanRangeParams` field 28 `PlanNodes.thrift:297`)。
- **FE**:kafka() analyzer 解析 `confluent.schema.registry.url`(复用 `CreateRoutineLoadStmt.CONFLUENT_SCHEMA_REGISTRY_URL`),`format=avro` 时**必填校验**(同 `CreateRoutineLoadStmt.java:759-770`);`KafkaScanNode.toThrift()` 设 URL + `format=FORMAT_AVRO`(结构同 `StreamLoadScanNode.java:257-258`)。
- **BE pull operator**:持 `KafkaDataConsumer`(复用 `data_consumer.cpp`)+ 内嵌 `StreamLoadPipe`;每条消息 `pipe->append_json(payload,len,'\n',partition,offset)`(`kafka_consumer_pipe.h:78`,保一消息一 buffer + partition/offset 元数据),再 `AvroScanner::get_next()` 抽干 chunk。用合成的 `TBrokerScanRange`(`params.confluent_schema_registry_url` 设上)构造一个 `AvroScanner`,`AvroScanner::open` 从该 URL 建 serdes handle(`avro_scanner.cpp:135-152`)**零改动**。净新增仅 operator 壳 + consumer→pipe pump。
- **registry 缓存/错误**:每 operator 实例一个 `serdes_t`(in-handle 按 schema id 缓存,首条新 id 同步 HTTP GET `/schemas/ids/{id}`,后续命中);错误同 `avro_scanner.cpp:282-288`(坏 framing/未知 id/registry 不可达→计入 rejected,按 `max_filter_ratio` 跳行或失败);凭证(URL 内 user:pass)由 libserdes/libcurl 解析,显示走 `getPrintableConfluentSchemaRegistryUrl:708-722` + `PrintableMap.SENSITIVE_KEY` 掩码。
- **依赖**:libserdes 7.3.1 已是 thirdparty(`thirdparty/vars.sh:377`)且已链入 BE(`be/CMakeLists.txt:673 serdes`),新 operator target 加同款 `serdes` 链接即可,无新 thirdparty。
- **边界**:URL 今天在两处 thrift 字段(`TKafkaLoadInfo.confluent_schema_registry_url` 给 consumer/meta-proxy、`TBrokerScanRangeParams` 给 scanner);解码只读 scan 侧,若 operator 也做 partition discovery 则两处都要设。缺 URL + `format=avro` → FE 分析期拒(不要让 BE `avro_scanner.cpp:138` 才报)。

### 4.3 全错批次跳过 / `NO_ROWS_IMPORTED`(C.1.5)

**目标**:某批消费了行但**全被过滤/出错**(毒批)时,把 committedOffsets 推进到已消费 end offset 跳过它(受 `max_error_number`/`max_filter_ratio` 约束);而真正的 fragment/txn 失败则**不前移**、重消费。否则全过滤批在"abort→重消费"下会**永久 livelock**。

**3-way BatchOutcome 分类**(`KafkaRoutineLoadJob.checkCommitInfo:358-389` 的移植):
- `COMMITTED`:txn 提交、有 loaded → 推进到 endOffsets(正常)。
- `SKIPPABLE_EMPTY_OR_POISON`:txn ABORTED 但 `totalConsumed=loaded+filtered+unselected>0 && loaded==0`,且 abort reason 为 `FILTER_DATA_ERR`(`StmtExecutor.java:3436/3440`)或 `ERR_NO_ROWS_IMPORTED`(`:3456`)→ **推进到 endOffsets**(跳过毒批)。
- `HARD_FAILURE`:其余 abort(coordinator 错/超时/`OFFSET_OUT_OF_RANGE`/不可重试)→ **留 beginOffsets**,重消费,失败计数 +1。

**关键**:FE **已知**本批 dispatched 的 `[begin,end)`(存在 `KafkaPipePiece`),所以 SKIPPABLE 时直接 advance 到 `piece.endOffsets()`,**无需** BE 回显 end offset(常见路径)。`KafkaPipeSource.finishPiece(piece, outcome)` 据 outcome 切换 advance/stay,committedOffsets 与内存推进**同一 edit 持久化**(仿 `replayOnAborted→replayUpdateProgress`),重启从跳过位续。
**双闸**:per-batch `max_filter_ratio` 在 `StmtExecutor.java:3422` 内决定 commit-vs-abort-as-poison;cross-batch `max_error_number`(§3.2 窗口)决定 quietly-skip-vs-pause-pipe(超阈值 → §3.1 的 ERROR 态,offset 已前移但 pipe 停下让人介入)。
**边界**:**空 poll**(`totalConsumed==0`)**不前移**(高水位无新数据,下轮自然续);**short-read**(BE 实际消费少于请求范围)是唯一需 BE 回显实际 end offset 的情形,只 advance 到真实消费位。
**touch points**:`KafkaPipeSource.finishPiece` [新增,核心门]、`KafkaPipePiece{beginOffsets,endOffsets}` [新增]、`BatchOutcome.classify(reason,loaded,filtered,unselected)` [新增]。

### 4.4 ERROR 自动恢复(带退避)+ group_id 规则(C.6.20 / C.5.17)

**(a) 自动恢复**:复刻 `ScheduleRule`(`ScheduleRule.java:63-99`)——`autoResumeLock` 为真则不恢复;`firstResumeTimestamp==0` → 置 now、`autoResumeCount=1`;在 `Config.period_of_auto_resume_min*60000` 窗口内,`autoResumeCount>=3` → 置 `autoResumeLock=true`(锁定,需手动),否则 `count++`;窗口过期 → 重置 count=1。手动 `RESUME` 清零三者(`RoutineLoadMgr:420-422`)。
**可重试 vs 致命分类**:
- **可重试(自动退避恢复)**:broker 不可达、全 BE 短暂掉线 / `REPLICA_FEW`、txn `TASKS_ABORT_ERR` 瞬时(`RoutineLoadJob.java:1224/1239/1249`)。
- **致命(ERROR 终态,仅手动 RESUME)**:`TOO_MANY_FAILURE_ROWS_ERR`(超 `max_error_number`,§3.1)、schema 不兼容、认证失败。
`PipeScheduler` 对可重试错误把 pipe 自动 `ERROR→RUNNING`(带上述退避),`SHOW PIPES` 暴露 `AUTO_RESUME_COUNT` 与锁定原因。
**(b) group_id**:现 RL 默认 `name+"_"+UUID`(`KafkaRoutineLoadJob.java:634`,key `group.id` `:103`)。v0.3:**`group.id` 已从 consumer 池 match key 移除**(§14,消费基于 `assign()`、offset 由 SR 管)。默认策略:**缺省不设** → BE 给监控用 `localhost_<uid>`;`group_id` TVF 参数为**首选**,`property.group.id` 为兼容别名;**二者都给且不同则 FE 报错**。

### 4.5 其余小项(落档,无需展开)
- **C.5.15** `task_consume_second/task_timeout_second` 比例差异:**接受**,文档说明 `target_e2e_latency` 推导用不同切分,旧固定 4:1 节奏不逐字节复现。
- **C.5.16** `kafka_offsets` 不进 rebind 的 TVF 文本:已由 §11 offset 模型承载——**初始** offset 在 TVF 文本,**运行** offset 在 `KafkaProgress`/commit-attachment + scan-range,不进缓存计划。
- **C.5.18** Pulsar 留 legacy:v0.3 §20.4 已定(`enable_unified_routine_load` 只接管 KAFKA,加断言防误转)。
- **C.2.8** JSON 命名投影保证:kafka→Pipe 重写生成**命名** SELECT(非 `SELECT *`/位置引用),保 `json_scanner` 按 slot 名匹配(v0.3 §5.2)。
- **C.2.6** protobuf 移除:v0.3 §3.3 已定(StarRocks 任何路径都不支持,非对齐项)。
