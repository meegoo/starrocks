# 统一 Routine Load 详细设计（DLD）

> **配套** 概要设计（HLD,`unified-routine-load-design-overview.md`）。本文是 **how**:把净新增件细化到可开工粒度,全部 code-anchored。行号基于 `origin/meegoo/unified-routine-load-6c0a`,为**指示性**锚点,实现时以实际为准。
> 纯决策/事实小项(protobuf 移除、Pulsar 留 legacy、`task_consume/timeout` 比例差异等)见 **HLD 附录 C**,本文只细化需 net-new 代码的项。
> 标注:**[新增]** 全新代码;**[改]** 改现有;**[复用]** 直接用;**[验证]** 仅核对无需改。
>
> 章节对应:§1↔HLD §8、§2↔HLD §12、§3↔HLD §13、§4.1↔HLD §20.3、§4.2↔HLD §5.2、§4.3↔HLD §11.3、§4.4↔HLD §18.1、§5↔HLD §14/§15/§16、§6=代码事实锚点。

---

## 1. compile-once / rebind-per-batch 执行模型

### 1.1 可行性结论
对抗式验证结论:**feasible-with-changes**。compile-once 骨架成立,deploy 层重绑缝真实存在,但"每批只重绑 txn_id"**过于乐观**,有三处必须纠正(§1.3)。确认成立的:计划复用骨架、txn 从 plan 期解绑、并行度经 DAG 重建。

**为什么能复用**:`ExecPlan`(`ExecPlan.java:49-102`)持有 `final` fragments/scanNodes/descTbl,**不含 txn_id / query_id / scan range**;`StmtExecutor.handleDMLStmt` 不建计划,只消费它,并**每次新建 Coordinator**(`StmtExecutor.java:3289`)→ `DefaultCoordinator` 构造时从 `jobSpec.getFragments()` 现建 `ExecutionDAG`(`:285-286`)。scan range 存在 per-coordinator 的 `FragmentInstance.node2ScanRanges`(`FragmentInstance.java:82`),不在共享 `ScanNode` 上。`TFragmentInstanceFactory.toThriftFromCommonParams:121` **每次 deploy 都 `fragment.toThrift()` 重新序列化**——这正是重绑点。

### 1.2 复用什么 / 每批做什么

| 量 | 处理 | 机制 / 锚点 |
|----|------|------|
| optimizer 输出 / `PlanFragment` 树形状 | **缓存复用**(真正的 compile-once 收益:省 optimizer + fragment-build) | `ExecPlan.java:52-77` |
| query_id / fragment_instance_id | 每批新生成(trivial) | `JobSpec.setQueryId:374` → `TFragmentInstanceFactory:129/236` |
| Coordinator / CoordinatorPreprocessor / ExecutionDAG | 每批现建(便宜,O(fragments),无优化器) | `DefaultCoordinator:285-286` |
| 并行度(pipeline_dop / instance 扇出) | 每批从同一 fragments 重建 DAG,**不 re-optimize** | `TFragmentInstanceFactory:205`(dop 为 instance 级) |
| Kafka offsets | 每批经 **scan range** 下发(§1.3-C) | `TFragmentInstanceFactory:237` `setPer_node_scan_ranges` |
| txn_id + label | 每批开新 txn,**并重跑 sink complete()**(§1.3-A) | `OlapTableSink.init:202` / `complete:336-371` |

### 1.3 三处必须纠正

**(A) `OlapTableSink` 烘焙的不止 txn_id —— 每批必须 re-complete sink（最大纠正）。**
`OlapTableSink.complete()`(`InsertPlanner.java:490`;`OlapTableSink.java:336-371`)把 **partition 参数**(`createPartition`,含 auto-partition shadow 解析 `:368`)、**tablet/replica LOCATION map**(`createLocation:371`)、**nodes_info**(`:372`)、txnState 派生值(`:366`)烘焙进缓存的 `tDataSink`;`toThrift()` 只返回缓存(`:438`)。长驻 pipe 里这些会**漂移**:auto-partition 新建分区、tablet clone/迁移/下线改 location、节点增减。若只 `set txn_id`,会把 stale location 再次下发 → 写到已迁移/死 tablet。**这正是现有 RL 每个 task 都重跑 `complete()` 的原因。**
- **方案**:`resetForReuse(loadId, txnId, label)` 必须**重跑 `complete()`**,而非只设 txn_id。每批成本 = "re-complete sink",但仍省 optimizer + fragment-build。
- **快路径(便宜 epoch 主动门 + deploy-failure 兜底)**:每批比对便宜 epoch,**仅在变化时** re-complete,稳态跳过。
  - **便宜 epoch = `(lastSchemaUpdateTime, hash(partition_id_set), hash({backendId, isAlive}))`**,全 leader 本地、O(分区)/O(节点)、无 RPC。schema/partition/node 三维都有便宜戳。
  - **tablet-location 漂移无便宜表级戳**(只有 per-tablet/per-backend)。**弃用** backend-report-version 代理(每几秒 bump → 几乎每批失效)。改 **deploy-failure 兜底**:stale location → 写失败(tablet not found / 副本不可达 / NodeChannel 打开失败)→ 可重试错误 → abort → 强制 re-complete + 重试该批(走 §4.4 retryable 分类)。
  - **为何够用**:node-set epoch **含 `isAlive`** → BE 掉线(最常见触发)已被主动门捕获;残余 clone/迁移/balance 由兜底吸收。**Phase-2 可选**:per-table `replicaLocationEpoch`(`AtomicLong`,在副本状态变更/clone/迁移路径 bump)做精确化。

**(B) 并发同 pipe 批次 race 共享 `RuntimeFilterDescription`(SELECT 带 join 时）。**
`ExecutionFragment.setLayoutInfosForRuntimeFilters()`(`ExecutionFragment.java:154-163`,由 `:113` 调用)把 per-run instance/bucket 写进**共享的** `planFragment.getBuildRuntimeFilters()`。无 join 是 no-op;含 join(维表查找)时每批被改写。
- **方案**:`kafka_pipe_max_inflight_batches_per_pipe` 默认 **1**(单批在飞,last-writer-wins);仅 SELECT **join-free** 才放开多批;或多批场景每批 deep-copy fragment 树。

**(C) offsets-in-scan-range 是净新增,不是"复用"。**
今天**没有** `TKafkaScanNode`/`TKafkaScanRange`(`PlanNodes.thrift` 的 `TScanRange` union 止于 `TBenchmarkScanRange:478-491`),BE 也无 pull 式 Kafka scan operator(现 RL 是 push 式 `KafkaConsumerPipe→StreamLoadScanNode`,offset 走 `TKafkaLoadInfo` sidecar)。增量 scan-range 重绑缝(`DefaultCoordinator.assignIncrementalScanRangesToDeployStates:697-748` 由 `hasMoreScanRanges():718` 门控;`CoordinatorPreprocessor.assignIncrementalScanRangesToFragmentInstances:282`;`TFragmentInstanceFactory.createIncrementalScanRanges:96`)**真实可复用**,但需新 FE 节点 + thrift + BE operator 落地后才能用。
- **已定:offsets-in-scan-range 拉模型**。理由:MPP INSERT 前提下 **BE pull scan operator 两种方案都躲不掉**(push 链不在 pipeline 引擎里),sidecar 的"省 BE"是错觉——只省一个 thrift struct,却把 offset 推旁路 RPC、留 legacy 双码路径、不契合 compile-once。pull operator 是 BE 最大单项,放 **Phase 1**。

### 1.4 必解 blocker:txn 在 plan 期被焊死
`StatementPlanner.plan` 对 DmlStmt 在 plan 期调 `beginTransaction`(`:119`→`:517`→`stmt.setTxnId:637`),把 txnId 焊到 AST。复用要求计划**无 txn**。
- **方案**:建缓存计划时用现有早退守卫(`:520` `session.getTxnId()!=0`,`:543` txnId 已设)跳过开 txn;每批由专用提交器自己 `beginTransaction` 并经 `resetForReuse` 绑回。建议加 `InsertStmt`/`ConnectContext` 上的 typed flag(而非复用 session sentinel),以免与显式多语句事务混淆。
- **源类型必须是 `INSERT_STREAMING`(修复 VFF-3,关键)**:提交器开 txn 时**必须**用 `LoadJobSourceType.INSERT_STREAMING`——它是**唯一**绕过 `DatabaseTransactionMgr.commitTransaction:375` 的 `empty_load_as_error` 门(默认 `true`)的源类型,§4.3 的"全过滤批 COMMIT(空 commit-infos)而非 abort、无 livelock"完全依赖于此。**警告**:§1.8 决策 #2"照搬 `RoutineLoadTaskScheduler` 模型"**只照搬其非 tick 调度模型,绝不照搬其 `ROUTINE_LOAD_TASK` 源类型**——后者**不**绕过该门,会让每个全过滤/毒批 → `ERR_NO_ROWS_IMPORTED` → abort → 永久重消费,正是 §4.3/§11.3 要消除的 livelock。

### 1.5 touch points
- `gensrc/thrift/PlanNodes.thrift` **[新增]**:`TKafkaScanNode`(挂 `TPlanNode`,`table_function_node=54` 后下一空序号;结构性 brokers/topic/properties/format/registry_url)+ `TKafkaScanRange`(挂 `TScanRange` union,`benchmark_scan_range=40` 后下一空序号;每批 partition_id/offset_begin/offset_end(-1=至超时)/consume_timeout_ms/max_batch_rows/max_batch_size)。
- `gensrc/thrift/FrontendService.thrift` **[新增]**:`TReportExecStatusParams.30: optional map<i32,i64> kafka_partition_end_offsets`(BE pull operator 上报实际终止 offset 的通道,§3.4;现有 load 聚合只有 4 个标量 counter,无 offset 字段)。**序号必须用 30(= 现有最大序号 29 之后),不要套用本节别处的"下一空序号"启发式——该 struct 序号 1-25、跳 26、27-29,空位 26 是退役序号(原 `source_scan_bytes`,2023 年改号到 24),复用违反仓库"never reuse ordinals"不变式(VFF-1)。**
- `fe .../planner/KafkaScanNode.java` **[新增]**:`extends ScanNode`;`getScanRangeLocations(maxLen)` 每 partition 出一 range(分到所属 CN);`hasMoreScanRanges()=true`;`setBatchOffsets(...)`。仿 `StreamLoadScanNode`(`StreamLoadPlanner.java:195`)+ `ScanNode.java:152/189`。
- `fe .../planner/OlapTableSink.java:198/336` **[改]**:抽 `resetForReuse(loadId,txnId,label)` —— 重跑 `complete()` + 重设 txn_id + 重解析 txnState;配 epoch 门(§1.3-A)。
- `fe .../sql/StatementPlanner.java:119/517` **[改]**:pipe 建计划路径 txn-free(§1.4)。
- `fe .../load/pipe/KafkaPipeExecPlanCache.java` **[新增]**:per-pipeId `{execPlan, sinkEpoch=(lastSchemaUpdateTime, partitionSet hash, nodeSet+isAlive hash), parallelism}`;`sinkEpoch` 变 → re-complete;schema/partition 变 → 整计划重建;仿 `PrepareStmtContext.java:73`。tablet-location 不进 epoch(§1.3-A)。
- `fe .../load/pipe/KafkaPipeSubmitter.java` **[新增]**:专用执行器,见 §1.6;**须跟踪每 pipe 在飞批次**(§1.7)。
- `fe .../qe/StmtExecutor.java:3289-3463` **[改/抽取]**:抽 `runOneBatch(cachedPlan, txnId, label, offsets)`,供提交器调用而不重建计划。
- `fe .../qe/scheduler/dag/JobSpec.java:374` **[复用/加]**:`setQueryId` 复用;加 `Factory.fromKafkaPipeBatch(...)` 仿 `fromQuerySpec:207`。
- `fe .../qe/CoordinatorPreprocessor.java:282` + `DefaultCoordinator.java:697` **[复用/接线]**:接 `KafkaScanNode.hasMoreScanRanges()`,设 `jobSpec.isIncrementalScanRanges(true)`。
- `be/src/exec` **[新增]**:pull 式 `KafkaScanNode`/`ChunkSource`,复用 `DataConsumer/DataConsumerPool` 做 `seek(part,begin)` 读到 end/超时。
- `fe .../common/Config.java` **[新增]**:`kafka_pipe_submitter_threads`(默认 ~16)、`kafka_pipe_max_inflight_batches_per_pipe`(默认 1);同步 `docs/en`+`docs/zh`。

### 1.6 专用低延迟提交器
`KafkaPipeSubmitter` **[新增]**:**非 tick、事件驱动**;cached 线程池(仿 `RoutineLoadTaskScheduler.java:85-87` 的 `LinkedBlockingQueue` + `newCachedThreadPool`),**不走** 1s pipe_scheduler tick,**不受** `task_runs_concurrency=4`(`TaskRunScheduler.java:127`)。一批 `coord.join()` 完立即算下批 offset 再提交(背靠背)。offset 进度复用 `KafkaProgress`(持久化),重启可续。
> **label 策略(修复 EOC-4)**:label **确定性地**由 `(pipeId, 本批各 partition begin-offset, planEpoch)` 生成,使 **HARD_FAILURE 重试同一 `[begin,end)` 时复用同一 label** → `GlobalTransactionMgr` 的 label dedup 拒绝重复提交,关上"FE 侧 abort 与 BE 侧 commit 竞态 + 重试用新 label 造成双提交"的窗口。重试若撞上仍未 resolve 的同 label 旧 txn,按 dedup 等待其终态而非另开新 txn。

### 1.7 边界与生命周期
- **空批次** → 不开 txn 直接短路(仿 `StmtExecutor:3445`),否则 txn churn 刷爆 `GlobalTransactionMgr`。
- **query_id 唯一** → 每批唯一(`registerQuery` 按 query_id,`:3345`)。
- **schema 变更（mid-stream）** → `lastSchemaUpdateTime` 触发**整计划重建**(非仅 sink re-complete,因 slot/tuple 布局变);**已部署在飞批次按旧 schema abort 重消费**(committedOffsets 仅 COMMITTED 推进,安全);重建后 `__op` 尾 slot 重校验(§2)。
- **partition 集变更** → 失效;缩容清掉消失 partition 的 scan range(否则 BE 重扫旧 offset,`CoordinatorPreprocessor:288-298` 注释的 stale-bucket 坑)。
- **节点集变更** → 每批用新 `WorkerProvider` 重建 DAG。
- **在飞批次取消(SUSPEND/DROP/换届)** **[新增,完整性]**:专用提交器绕开 TaskManager,故 `Pipe.suspend()`(`Pipe.java:555-570`)的 "interrupt TaskManager 任务" 对 Kafka 批次**无效**。`KafkaPipeSubmitter` 须维护 per-pipe 在飞批次表(txnId/label/Coordinator 句柄),供 Pipe 生命周期可见:SUSPEND/DROP → abort 这些 txn、cancel/join coordinator、释放 §5 资源车道、归还 consumer;DROP 须等待或强制 abort 在飞 txn 再删 offset 状态;leader 换届时孤儿 txn 按 §3.5 恢复语义处理。
- **plan-cache 重建 vs 在飞批次(修复 NI-4)**:schema/partition/node 变更触发 `KafkaPipeExecPlanCache` 重建或 sinkEpoch re-complete 时,**必须先 abort 并 join 该 pipe 当前在飞批次**(复用上面的在飞批次表),再**原子替换** cache entry,下批用新 plan 重消费——避免重建冲掉正被在飞批次引用的 `ExecPlan`/fragments(§1.3-B 的共享 `RuntimeFilterDescription` 同理靠 `max_inflight=1`)。失效检查点定在提交器**算下批之前**(同步,非异步),消除 check-then-deploy 的 TOCTOU;该 abort-join 走与 SUSPEND 相同的在飞批次取消路径。
- **OlapTableSink dop 约束**(PK/lake,`StreamLoadPlanner.java:275-`):sink dop 受限,scan dop 高于 sink dop 须经 exchange,该 exchange 须在缓存计划里;保留 `load_dop` 逻辑。

### 1.8 已定决策
1. offset 传输 = **scan-range 拉模型**(§1.3-C)。
2. 专用提交器 = **新建 `KafkaPipeSubmitter`,照搬 `RoutineLoadTaskScheduler` 模型**(不直接复用 legacy 类)。
3. 每批事务粒度 = **pipe×batch 全分区单 txn**(exactly-once 最简、txn 数最少;partition-group 仅大扇入时后置)。
4. in-batch 增量 refill = **Phase 2**(Phase 1 一批=一 deploy=一 txn)。
5. re-complete epoch 门 = **便宜 epoch 主动门 + deploy-failure 兜底**(§1.3-A)。

---

## 2. 行级 `__op`（UPSERT / DELETE）

### 2.1 BE 契约（位置式,零 thrift 改动）
op 完全由"**PK 表 + 输出 tuple 最后一个 slot 名为 `__op`(TINYINT)**"激活,BE 已完整实现:`tablet_sink.cpp:814`(`slots().back()->col_name()=="__op"`)、`memtable.cpp:104-122`(`_has_op_slot`)、`:310-317`/`:482-499`(split,ndel==0 短路)、`delta_writer.cpp:248-254/405-414`。**无 `TOpType`/`TOlapTableSchemaParam`/BE 改动**。陷阱:`OlapTableSink` 对每个 PK 表都把 `__op` 加进**索引列名列表**(`OlapTableSink.java:478-480`),但那是 inert 的——BE 只认尾 slot;仅靠列名列表条目**不**启用删除。

### 2.2 五件套
**(1) kafka() TVF 产出 `__op` 列。** JSON:声明虚拟 `__op` TINYINT 输出列,未显式映射时 BE kafka json reader 自动提取 `__op` key 或 CDC envelope op(仿 `json_scanner.cpp:300/586-588` + `Load.java:417-418`),缺失填 UPSERT(0)。CSV/raw:用户显式投影 `... 'delete' AS __op` 或 `CAST(c4 AS TINYINT) AS __op`。
**(2) Analyzer 识别/转换,强制 TINYINT。** `InsertAnalyzer`(~`:318`)新增分支:当 `enable_op_column=true` + `Load.tableSupportOpColumn(table)` + query relation 有非真实表列的输出列名 `__op` 时:置 `insertStmt.hasOpColumn=true`;把 `__op` **从数据列计数剔除**(不进 `targetColumns`,使 `:346-348` 计数校验按数据列对齐);`'upsert'/'delete'`→`IntLiteral(TOpType 0/1)`(复用 `Load.java:392-404`),否则 `CAST(... AS TINYINT)`。校验:不与真实列重名、只一个、仅 PK OlapTable、非 PK 或门关却出现 → `SemanticException`。
**(3) `InsertPlanner` 追加尾 slot + 绑定 op ColumnRef。**
- **3a.** 在 `outputFullSchema` slot 循环之后、`computeMemLayout` 之前,若 `hasOpColumn()`:追加**一个** TINYINT 非空 materialized、名 `Load.LOAD_OP_COLUMN` 的 slot(克隆 `LoadPlanner.java:424-428`)→ 末 slot 为 `__op`。
- **3b.** 新增 `fillOpColumn` 步,插在 cast(`castOutputColumnsTypeToTargetColumns`)之后,把 op 表达式(literal 的 `ConstantOperator(TINYINT)`,或源 query `__op` 输出 ColumnRef 经 `CastOperator(TINYINT)`)作为 `outputColumns` **最后一个**元素追加(经 `LogicalProjectOperator`/`withNewRoot`),使 `outputColumns` 长度 = tuple slot 数(满足 `tablet_sink.cpp:268-279` 计数+类型校验)。**务必最后追加**,不要让 op 列走 `fillDefaultValue`/`fillGeneratedColumns`/cast 循环。
- **3c.** `OlapTableSink` 无需改(`createSchema` 已对 PK 加 `LOAD_OP_COLUMN`,tuple 现已匹配)。
> 行号校核:`InsertPlanner.plan` 的 fill*/cast 步骤、`outputFullSchema` slot 循环、`computeMemLayout` 的相对顺序需在目标分支重新定位;**不变式**(序数语义):op ColumnRef 严格在所有 fill*/cast **之后**、LogicalPlan 冻结之前追加;op slot 严格在 `outputFullSchema` 循环**之后**、`computeMemLayout` 之前追加;之后任何步骤(iceberg shuffle 投影、generated/shadow 列)不得再追加。
**(4) opt-in 门 `enable_op_column`。** 新 session var `ENABLE_OP_COLUMN`(默认 **FALSE**)。仅 true 才把 `__op` 当 op 指令;false 时 `__op` 投影按普通列 → 无该表列 → 显式报 `Unknown column '__op'`(`InsertAnalyzer.java:282`)。普通 INSERT 永不长出 op slot。kafka()→Pipe 重写对 PK 目标自动开。
**(5) 与 column-mode partial update + sort key 交互。** `hasOpColumn && usePartialUpdate`:op slot 仍须尾 slot;`__op` 不进 partial-update 输出 schema(`inferOutputSchemaForPartialUpdate:227-288` 只遍历真实列,3a 在缩减循环后追加,天然排除)。DELETE 行只需有效 PK(`memtable.cpp:514-529`)。已知限制:column-mode partial update + DELETE 在 sort-key 表受 `delta_writer.cpp:402-424` 约束(混 upsert/delete 可能 NotSupported),向用户暴露。

### 2.3 touch points
`InsertPlanner.java:plan` [改:3a 尾 slot] + `fillOpColumn` [新增:3b];`InsertAnalyzer.java`(~`:228-349`)[改:识别/剔除计数/强转/校验];`InsertStmt.java` [新增字段 `hasOpColumn` + op 表达式/源输出名,仿 `usePartialUpdate`];`Load.java:normalizeOpColumnExpr` [新增/重构:抽 `:392-404`];`SessionVariable.java` [新增 `ENABLE_OP_COLUMN`];`OlapTableSink.java:478-480` [验证];kafka() TVF relation [新增:声明尾 `__op`];`be/src/exec` kafka json reader [新增:复制 `json_scanner.cpp:300-301/586-588` 自动提取];**无改动** `gensrc/thrift`/`tablet_sink.cpp`/`memtable.cpp`/`delta_writer.cpp`。

### 2.4 边界
count+type 不变式(尾 slot 恰一 TINYINT 输出表达式,`tablet_sink.cpp:268-279`);**顺序严格最后**(`memtable._split_upserts_deletes` 弹最后列;`tablet_sink.cpp:814`/`delta_writer.cpp:405` 看 `slots().back()`);门关却有 `__op` → 显式报错;非 PK 表 `__op` → analyzer 拒;列数校验须把 `__op` 从 query relationFields 减掉(否则 `ERR_INSERT_COLUMN_COUNT_MISMATCH`);full INSERT 仍要求 NOT NULL 列被投影(per-statement);auto-increment + DELETE(`tablet_sink.cpp:814-841` zero-init,依赖尾 slot);match-by-name INSERT 须把 `__op` 从 `targetColumnNames` 过滤;literal 仅 `'upsert'/'delete'`/0/1。

### 2.5 已定决策
1. `enable_op_column` 默认 FALSE 全局 + kafka()→Pipe 对 PK 自动开。
2. op 列名硬定 `__op`(BE 只认字面尾 slot;Debezium 的 `op` 经 SELECT 投影成 `__op`)。
3. CDC `c/r/u/d`→`TOpType` 在 **FE 投影**(`envelope=debezium` 注入 `CASE ... AS __op`);裸 `__op` 走 BE 自动提取。
4. `__op` + `merge_condition` 冲突 → **FE 分析期拒绝**(`memtable.cpp:500-503` 运行期禁,FE fail-fast 优先)。
5. v0.3 限 streaming/Pipe 路径(改动通用,翻 flag 即可放开)。

---

## 3. 绝对 `max_error_number` 窗口 + 统计/offset 回传

### 3.1 决策:超阈值 → ERROR 态（手动 RESUME）
**[已定]** 累计错误超 `max_error_number` 时,pipe 进 **`State.ERROR`**,需 `ALTER PIPE RESUME` 手动恢复——**不是** RL 式可恢复的 SUSPEND/PAUSED。原因符号 `TOO_MANY_FAILURE_ROWS_ERR`。
> 与 §4.4 互补:**瞬时错误**(broker 抖动/BE 掉线)走带退避的**自动恢复**;**数据质量错误**(超 `max_error_number`)是终态 `ERROR` 不自动恢复。`Pipe.State.canResume()` 对 `ERROR` 返回 true(`Pipe.java:825`),故手动 RESUME 可用。HLD §13/§18 与本节口径一致(均 ERROR,非 SUSPEND)。

### 3.2 `KafkaPipeSource` 滑动窗口累加器
新类 `com.starrocks.load.pipe.KafkaPipeSource`(仿 `FilePipeSource`)。字段(`@SerializedName` 持久化,仿 `RoutineLoadJob.java:281-291`):累计 `totalRows/errorRows/unselectedRows/receivedBytes`;窗口 `currentErrorRows/currentTotalRows`(**持久化**,leader 换届不重置半填窗口);限额 `maxErrorNum`(默认 0)、**`errorWindowRows`**(= 用户声明的 `max_batch_rows`,默认 `DEFAULT_MAX_BATCH_ROWS=200000`,**只随 CREATE/ALTER PIPE 变**,作错误窗口基数)、`maxFilterRatio`(默认 **1.0**,见 §3.7);错误样本 `transient Queue<String> errorLogUrls = EvictingQueue.create(3)`(gsonPostProcess 重建)。

`updateNumOfData(numTotal, numError, numUnselected, receivedBytes, isReplay)` —— **逐字移植** `RoutineLoadJob.updateNumOfData`(`:829-896`):累加;`currentTotalRows > errorWindowRows*10` 时若 `currentErrorRows>maxErrorNum && !isReplay` → pause,再重置窗口;窗口未满但超限也 pause(`:877-895`)。**pause = `pipe.changeState(State.ERROR)`**(§3.1)。`replay` 不重复 pause(`isReplay=true`)。
> ⚠️ **窗口基数 vs 降级批大小(修复 NI-1)**:窗口公式用 `errorWindowRows`(声明值),**不是**降级控制器(HLD §10)动态调大的"有效批大小"。降级调的是下发 `TKafkaScanRange.max_batch_rows` 的运行时上限,**绝不进** `updateNumOfData` 的窗口公式——否则压力上升时 `maxErrorNum/(errorWindowRows*10)` 这个有效错误率门槛会被静默放大,`max_error_number` 语义随降级漂移。RL 注释明确该比值即 max error rate(`RoutineLoadJob.java:217`)。
> ⚠️ **必须从 commit 与 abort 两条路径喂(关键修复)**:见 §3.5——全过滤批 abort 后其 filtered 计数若不回传,绝对窗口永远收不到,毒批流永不停。

### 3.3 扩展 `InsertTxnCommitAttachment`（统计 + offset 权威）
`InsertTxnCommitAttachment.java` 加 `@SerializedName` 字段(**仅 Gson,非 thrift struct,无序号管理**;现有 `loadedRows`/`isVersionOverwrite`/`partitionVersion` 三字段及 `(loadedRows)`、`(loadedRows, partitionVersion)` 两 ctor,**version-overwrite ctor 须保留**):新增 `filteredRows / unselectedRows / receivedBytes / trackingUrl` **+ `partitionEndOffsets`(Map<Int,Long>)**,全部 additive。加 fluent setter 供 `StmtExecutor` 调,不加冲突位置 ctor(保持 `InsertOverwriteJobRunner`/`OlapDeleteJob` 等现有调用点兼容)。旧 edit-log 记录把新字段反序列化为 0/null,向后兼容,无序号复用。
> **offset 权威(修复)**:`partitionEndOffsets` 使 committedOffsets **与数据原子提交**——这是 HLD §11.2 "commit-attachment 是唯一持久权威" 的落地点。**不引入** `TKafkaConsumeReport`(本库不存在该 struct);统计与 offset 都经此 FE 内部 attachment 通道。

### 3.4 `StmtExecutor` 改动
`handleDMLStmt` counter 读块(`:3405-3414`):新增读 `coord.getLoadCounters().get(LoadJob.UNSELECTED_ROWS)`(key 已存在,BE 已发 `exec_state_reporter.cpp:117`,只是从不读);capture `trackingUrl = coord.getTrackingUrl()`。在 **commit 路径**(`:3540-3567`)与 **abort 路径**(见 §3.5)都构造/填充 attachment(`setFilteredRows/setUnselectedRows/setReceivedBytes/setTrackingUrl/setPartitionEndOffsets`)。门控到 streaming-insert 路径(普通 INSERT 无害)。`receivedBytes` 复用 sink 侧 `loaded.bytes`(§3.8)。
> **`partitionEndOffsets` 的 BE→FE 通道(修复 NI-2,必做)**:现有 fragment→coordinator 的 load 聚合通道 `QueryRuntimeProfile.updateLoadInformation` 只聚 4 个标量 string counter(`DPP_NORMAL_ALL`/`DPP_ABNORMAL_ALL`/`UNSELECTED_ROWS`/`LOADED_BYTES`),**无 per-partition offset 字段**。当 FE 下发固定区间(`end!=-1`)时 `partitionEndOffsets = piece.end`,FE 已知、无需回传;但**时间窗口模式(`end=-1`,主用的低延迟模式)下实际终止 offset 由 BE 决定,必须回传**。故须 **[新增]** `TReportExecStatusParams.optional map<i32,i64> kafka_partition_end_offsets`:BE pull operator 填本 instance 各 partition 实际消费到的 offset → coordinator 按 partition 取 max 聚合 → `StmtExecutor` 写进 attachment.partitionEndOffsets。`KafkaScanNode` 是该值的真实 producer。

### 3.5 恢复 + **abort 路径喂窗口（关键修复）**
- **回调注册(修复 EOC-2,关键)**:`KafkaPipeSource`(或 `Pipe`)**必须实现 `TxnStateChangeCallback`,以稳定 `callbackId = pipeId` 注册进 `TxnStateCallbackFactory`**,且**每批 txn 在 `beginTransaction` 时设 `callbackId = pipeId`**。`TransactionState` 的 live(`afterStateTransform`)与 replay(`replaySetTransactionStatus`)两条路径都按 `callbackId` 查回调,**查不到就静默跳过**——若 Pipe 未注册(今天 Pipe 不是 callback),replay 不应用 offset → 退化为 at-least-once。注册须在 **live 加载**与 **FE 重启/换届的 pipe 元数据加载**两处都做(否则换届后回调表为空)。
- **commit**:`afterCommitted(txnState)`(`TransactionState.java:816-844`)取 `InsertTxnCommitAttachment`(`:720`),调 `updateNumOfData(loaded+filtered+unselected, filtered, unselected, receivedBytes, false)`,并从 `partitionEndOffsets` 应用 committedOffsets;`trackingUrl!=null && filtered>0` 则入 `errorLogUrls`。仿 RL `afterCommitted`(`RoutineLoadJob.java:1047-1080`)。
  > **offset 应用是绝对赋值,非增量(修复 EOC-5)**:`committedOffsets[p] := attachment.partitionEndOffsets[p]`(乱序下取 `max`,见 §4.3 EOC-3),镜像 `KafkaProgress.update` 的 absolute-put 语义(`KafkaProgress.java:199-204`),使同一 attachment 被 live + replay 重复应用**幂等**。
- **abort(毒批,仅 `max_filter_ratio<1.0` 时)** **[修复 C1+F2+F4]**:**先厘清触发条件**——目标是 OlapTable 时 `ERR_NO_ROWS_IMPORTED` 被 gate 到非 OlapTable(`StmtExecutor.java:3306`),且**默认 `max_filter_ratio=1.0` 下全过滤批不触发比例门(`:3422`)→ 以空 commit-infos COMMIT 并前移,filtered 经 `afterCommitted` 入窗口**(无 livelock,见 §4.3)。**注(修复 VFF-4/EOC-1)**:`loaded==0` 的 commit **不产生 tablet 版本**——`OlapTableTxnStateListener.preCommit` 在 `tabletCommitInfos` 为空时不设脏分区、`preWriteCommitLog` 产出零 `PartitionCommitInfo`、无 publish 任务;故空/全过滤 commit **不计入 §10 的版本/compaction 预算**(churn 在 txn/edit-log 层而非版本层),HLD §10 应注明此豁免。**只有用户把 `max_filter_ratio` 配到 <1.0** 时,超比例批才以 `FILTER_DATA_ERR` abort → 此时须保证 filtered 进窗口:用 **6 参重载** `GlobalTransactionMgr.abortTransaction(dbId, txnId, reason, finishedTablets, failedTablets, txnCommitAttachment)`(**勿用 4 参重载**——它把 finishedTablets/failedTablets 硬编码为空,丢掉现有 abort 路径传的 `Coordinator.getCommitInfos/getFailInfos`),带上含 filtered/unselected/`partitionEndOffsets` 的 attachment;`KafkaPipeSource.afterAborted` 调 `updateNumOfData(...)` 把 filtered 计入绝对窗口。
- **replay**:实现 `replayOnCommitted`/`replayOnAborted`(仿 `RoutineLoadJob.java:1083-1093`),`updateNumOfData(..., isReplay=true)`,重启计数收敛而不二次 pause;offset 从 replay 的 attachment 应用。
- **持久化**:累加器 + 窗口计数随 `Pipe` 的 `@SerializedName`(随 Pipe 落日志)持久化;扩 `LoadStatus`(`Pipe.java:762-797`)加 `errorRows/unselectedRows/receivedBytes`。

### 3.6 latest offset / lag（不来自 attachment）
`committedOffsets` 来自 attachment(§3.3);**latest offset 由 FE 周期性高水位探测**(`KafkaUtil.getLatestOffsets`,同 RL 的 `latestPartitionOffsets`)获得,有界限陈旧窗口。`OffsetLag = max(0, latest-committed)`,`LatestSourcePosition = {p:latest}`。这一点修复了"attachment 无高水位却要算 lag"的缺口。

### 3.7 默认翻转告警（必须处理）
`insert_max_filter_ratio` 默认 **0**(`SessionVariable.java:1656`)——任一过滤行即在 `StmtExecutor.java:3422` 失败整批;RL `maxFilterRatio` 默认 **1.0**(`RoutineLoadJob.java:215`,不因比例 abort,把绝对窗口留作唯一 pause 触发)。**kafka pipe 生成的每批 INSERT 必须把比例设为 pipe 的 `maxFilterRatio`(默认 1.0)**,否则一条坏行就 abort,绝对窗口成死代码。
> **注入缝(NI-6)**:经 **DmlStmt 的 `LoadStmt.MAX_FILTER_RATIO_PROPERTY`**,而非全局会话变量——`StmtExecutor.getMaxFilterRatio`(`:4020-4029`)**优先读该 property**,缺省才回落 session var。提交器每批构造 INSERT 时把 `pipe.maxFilterRatio` 作为该 property 下发,天然 per-batch 隔离,不必为每批伪造/还原 `ConnectContext` 会话状态,也不与 `task.*` 透传的 `SET_VAR` 互相覆盖。

### 3.8 已定决策
1. `receivedBytes` 复用 sink 侧 `loaded.bytes`(语义注明为 loaded 非 source-consumed;需要时再加 source 计数)。
2. `max_error_number/max_batch_rows/max_filter_ratio` 做 **CREATE PIPE 属性**(`PipeAnalyzer`,持久化在 `KafkaPipeSource`),非会话变量;每批 `SET_VAR` 由属性派生。
3. `ERROR_LOG_URLS` 做 **SHOW PIPES 新列**(与 `SHOW ROUTINE LOAD.ErrorLogUrls` 1:1)。

---

## 4. 其余 net-new 项的实现

### 4.1 SHOW ROUTINE LOAD / SHOW ROUTINE LOAD TASK 逐列契约
**策略**:`TITLE_NAMES` 与 `RoutineLoadJob.getShowInfo()`(`:1590-1668`)字节不变,feeder 改读 **KafkaPipeSource 累加器 + Pipe 状态**。`SHOW PIPES` 的 8 列(`ShowResultMetaFactory.visitShowPipeStatement:670`)**保持不变**(两视图有意分流:SHOW PIPES 文件/字节,SHOW ROUTINE LOAD 行/offset)。

**列数口径(统一)**:`ShowRoutineLoadStmt.TITLE_NAMES:59-89` = **22 列 + SHARED_DATA 条件追加 `Warehouse`**(HLD §20.3 同口径)。三档:
- **直接映射(Pipe 元数据)**:`Id/Name/CreateTime/DbName/TableName` ← Pipe;`DataSourceType`=`KAFKA`;`State` ← `Pipe.State`→JobState(RUNNING→RUNNING;SUSPEND→PAUSED;ERROR→PAUSED+reason;FINISHED→STOPPED);`JobProperties/DataSourceProperties/CustomProperties`(后者复用 `getMaskedCustomProperties` 掩码);`Warehouse`。
- **由累加器重建**:`CurrentTaskNum`=`currentParallelism`(**语义变**,需文档化);`Statistic`=11-key JSON;`Progress`={p:committedOffset};`LatestSourcePosition`={p:latestOffset}(来自 §3.6 探测,非 attachment);`OffsetLag`={p:max(0,latest-committed)}(保留 `checkProgressVal` 特殊 offset 过滤);`ErrorLogUrls`←EvictingQueue(3);`TrackingSQL`=硬编码串 keyed by pipeId;`TimestampProgress`=`"{}"`(若未接时间戳→offset 查询,**必须空 JSON 非 null**)。
- **无对应→常量/弃用**:`PauseTime/EndTime`(加 `lastSuspendTime`/`endTime` 或空);`ReasonOfStateChanged` ← `Pipe.lastErrorInfo`;`OtherMsg`=空。

**SHOW ROUTINE LOAD TASK**:"task" → pipe **最近/在飞批次**;`TaskId`=批次 UUID、`TxnId/TxnStatus` ← 该批 txn、`BeId` ← 执行该批 fragment 的 CN、`DataSourceProperties`=`"Progress:{p:beginOff},LatestOffset:{p:latestOff}"`(同 `KafkaTaskInfo.getTaskDataSourceProperties:221`)。
**touch points**:`getShowInfo:1590`/`getStatistic`/`getSourceProgressString:675`/`getSourceLagString:677`[改];`KafkaRoutineLoadJob.getStatistic:490`[改];`RoutineLoadTaskInfo.getTaskShowInfo:276`[改];`KafkaPipeSource`[新增累加器];`TITLE_NAMES`[**不改**]。
**边界**:`totalTaskExecMs` 保 `>=1` floor(`RoutineLoadJob.java:293` 除零);`Warehouse` 条件列两处须同位置 emit;`getShowInfo` 持 readLock 单快照,累加器读须快照一致;State 坍缩(Pipe 4 态 vs JobState 5 态)使 grep `CANCELLED`/`NEED_SCHEDULE` 静默失配——文档化。

### 4.2 avro + Confluent Schema Registry
**决策:复用 `AvroScanner` + libserdes(Option A,零改动 scanner)**。pull operator 只需保证:(a) registry-framed 字节一条一 buffer 到解码器,(b) confluent URL 到解码器。不用 `AvroCppScanner`(容器读),不重写 magic-byte/schema-id 解析。
- **thrift**:`TKafkaScanNode` 加 `optional string confluent_schema_registry_url` + `optional TFileFormatType format` + jsonpaths/列映射(镜像 `TBrokerScanRangeParams` field 28 `PlanNodes.thrift:297`)。
- **FE**:kafka() analyzer 解析 `confluent.schema.registry.url`(复用 `CreateRoutineLoadStmt.CONFLUENT_SCHEMA_REGISTRY_URL`),`format=avro` 必填校验(同 `CreateRoutineLoadStmt.java:759-770`);`KafkaScanNode.toThrift()` 设 URL + `FORMAT_AVRO`(结构同 `StreamLoadScanNode.java:257-258`)。
- **BE pull operator**:持 `KafkaDataConsumer` + 内嵌 `StreamLoadPipe`;每条消息 `pipe->append_json(payload,len,'\n',partition,offset)`(`kafka_consumer_pipe.h:78`,保**一消息一 buffer** + partition/offset),再 `AvroScanner::get_next()` 抽干 chunk。合成 `TBrokerScanRange`(`params.confluent_schema_registry_url` 设上)构造 `AvroScanner`,`open` 从 URL 建 serdes handle(`avro_scanner.cpp:135-152`)零改动。净新增仅 operator 壳 + consumer→pipe pump。**测试**:喂两条 registry-framed 消息断言两行输出(防未来 buffer 合并优化破坏一消息一 buffer)。
- **registry 缓存/错误**:每 operator 实例一 `serdes_t`(按 schema id 缓存,首条新 id 同步 GET `/schemas/ids/{id}`);错误同 `avro_scanner.cpp:282-288`;凭证(URL 内 user:pass)显示走 `getPrintableConfluentSchemaRegistryUrl:708-722` + `PrintableMap.SENSITIVE_KEY` 掩码。
- **依赖**:libserdes 7.3.1 已是 thirdparty(`thirdparty/vars.sh:377`)且已链入(`be/CMakeLists.txt:673 serdes`),无新 thirdparty。
- **边界**:URL 在两处 thrift 字段;解码只读 scan 侧,若 operator 也做 partition discovery 则两处都设。缺 URL+avro → FE 分析期拒。

### 4.3 全错批次跳过（poison-skip）
**目标**:某批消费了行但**全被过滤/出错**(毒批)时推进 committedOffsets 跳过(受 `max_error_number`/`max_filter_ratio` 约束);真正失败则不前移、重消费。否则全过滤批"abort→重消费"会**永久 livelock**。

**BatchOutcome —— net-new 分类器(非 checkCommitInfo 的移植)** **[修复 C3]**:RL 的 `checkCommitInfo` 按 `TxnStatusChangeReason.fromString().contains(...)` 匹配(`TxnStatusChangeReason.java:34-36`),其字符串(如 "too many filtered rows")**不**等于 INSERT 路径的 abort 串。本分类器**直接 key 在 StmtExecutor 的 abort 符号**:`TransactionCommitFailedException.FILTER_DATA_ERR`("Insert has filtered data")、`ErrorCode.ERR_NO_ROWS_IMPORTED`。加测试断言 100% 过滤批 → SKIPPABLE。

三态(按 StmtExecutor 实际算术,`StmtExecutor.java:3422/3445`):
| outcome | 条件 | offset |
|---------|------|--------|
| `COMMITTED` | txn 提交。含两种:`loaded>0`;**以及默认 `max_filter_ratio=1.0` 下的全过滤批**(`loaded==0&&filtered>0` 不触发比例门 `:3422` → 以**空 commit-infos** 提交) | 推进到 endOffsets;filtered 经 `afterCommitted` 入窗口(§3.5) |
| `POISON`（SKIPPABLE,仅 `max_filter_ratio<1.0`） | abort `FILTER_DATA_ERR`(`filtered` 超配置比例)、`totalConsumed>0`、`loaded==0` | **推进到 endOffsets**(跳过);filtered 经 `afterAborted` 入窗口(§3.5) |
| `EMPTY` | `totalConsumed==0`(真空批):由 §1.7 FE 侧 pre-txn 短路**不开 txn**(BE 侧若零 poll 提交,则为 `loaded==0&&filtered==0` 的空 COMMIT) | **不前移**(无新数据,下轮续) |
| `HARD_FAILURE` | 其余 abort(coordinator 错/超时/`OFFSET_OUT_OF_RANGE`/不可重试) | **留 beginOffsets**,重消费,失败计数+1 |

> **修复 F3/F4**:对 kafka pipe 的 OlapTable 目标,`ERR_NO_ROWS_IMPORTED` 被 gate 到非 OlapTable(`StmtExecutor.java:3306`)→ **不可达**;空批靠 §1.7 的 FE pre-txn 短路处理,不依赖该 abort。且**默认 `max_filter_ratio=1.0` 下毒批走 COMMITTED(空版本)而非 abort** —— 无 livelock,filtered 经 `afterCommitted` 入窗口。`FILTER_DATA_ERR`/POISON/`afterAborted` 喂窗口这条路**只在用户把比例配到 <1.0 时**才生效。`BatchOutcome.classify` key on `StmtExecutor` 的真实 abort 符号(`TransactionCommitFailedException.FILTER_DATA_ERR`),不是 RL 的 `TxnStatusChangeReason`(其字符串不等于 INSERT 路径串)。

**关键**:FE **已知**本批 dispatched 的 `[begin,end)`(`KafkaPipePiece`),SKIPPABLE 时直接 advance 到 `piece.endOffsets()`,**无需** BE 回显(常见路径);**short-read**(BE 实消费 < 请求区间)以 attachment 的 `partitionEndOffsets`(§3.3)为准。`KafkaPipeSource.finishPiece(piece, outcome)` 据 outcome 切换 advance/stay,committedOffsets 与内存推进**同一 edit 持久化**。
> **乱序退役安全(修复 EOC-3)**:per-piece advance/stay 仅在批次**按序退役**时正确。§1.3-B/§1.6 对 join-free SELECT 放开 `max_inflight>1`,此时两批可乱序完成——若后批先 COMMIT 就 advance、而前批是 HARD_FAILURE,会造成 **offset 空洞(丢数)或回退(重复)**。规则:**committedOffsets 只前进到已提交的最长连续前缀(contiguous-prefix)**——某 COMMITTED 批若其前驱仍在飞,须**暂缓 advance** 直到前驱 resolve;HARD_FAILURE 批**阻断**其 `beginOffsets` 之后的任何 advance。Phase 1 `max_inflight=1` 天然满足;放开多批必须实现此前缀提交。
**双闸**:per-batch `max_filter_ratio`(`:3422`)决定本批 commit(默认 1.0)vs abort-as-poison(配 <1.0);cross-batch `max_error_number`(§3.2 窗口,filtered 经 `afterCommitted`(默认 1.0)或 `afterAborted`(<1.0)计入,§3.5)决定 quietly-skip-vs-pause-pipe(超阈值 → ERROR,offset 已前移但 pipe 停下)。
**touch points**:`KafkaPipeSource.finishPiece`[新增,核心门]、`KafkaPipePiece{beginOffsets,endOffsets}`[新增]、`BatchOutcome.classify(reason,loaded,filtered,unselected)`[新增,key on StmtExecutor 符号]。

### 4.4 ERROR 自动恢复（带退避）+ group_id 规则
**(a) 自动恢复算法**:复刻 `ScheduleRule`(`ScheduleRule.java:63-99`)——`autoResumeLock` 真则不恢复;`firstResumeTimestamp==0` → 置 now、`autoResumeCount=1`;在 `Config.period_of_auto_resume_min*60000` 窗口内 `autoResumeCount>=3` → `autoResumeLock=true`(锁定需手动),否则 `count++`;窗口过期重置 count=1。手动 `RESUME` 清零三者(`RoutineLoadMgr:420-422`)。`PipeScheduler` 对可重试错误自动 `ERROR→RUNNING`,`SHOW PIPES` 暴露 `AUTO_RESUME_COUNT` + 锁定原因。
**(b) 错误分类**(决策亦见 HLD §18.1):**可重试**=broker 不可达 / 全 BE 短暂掉线·`REPLICA_FEW` / txn `TASKS_ABORT_ERR` 瞬时(`RoutineLoadJob.java:1224/1239/1249`)+ §1.3-A 的 stale-location 写失败(`TABLET_NOT_FOUND`/副本不可达/NodeChannel 打开失败 → 强制 re-complete + 重试);**致命**=`TOO_MANY_FAILURE_ROWS_ERR`(§3.1)/schema 不兼容/认证失败。
**(c) group_id**(决策亦见 HLD 附录 B):现 RL 默认 `name+"_"+UUID`(`KafkaRoutineLoadJob.java:634`,key `group.id` `:103`)。`group.id` **已从 consumer 池 match key 移除**(§5,消费基于 `assign()`)。默认**缺省不设**→ BE 给监控用 `localhost_<uid>`;`group_id` TVF 参数**首选**,`property.group.id` 兼容别名,**二者都给且不同则 FE 报错**。

---

## 5. Consumer 池 / 反压 / 资源隔离（实现）

### 5.1 Consumer 池修复
现状:`routine_load_task_executor.h:64` 硬编码 cap `10` 的**全进程扁平池**,满了 `data_consumer_pool.cpp:142` **丢弃**返还的 consumer;idle 回收是**死代码**(`start_bg_worker` 只 sleep,从不调 `_clean_idle_consumer_bg`);`match()`(`data_consumer.cpp:469-489`)**全量比对 `_custom_properties`**,而 FE 把 per-job 唯一 `group.id`(`KafkaRoutineLoadJob.java:~627`,`name+UUID`)放进该 properties map → group.id **经 custom_properties 间接进 match key** → 跨 pipe 不能共享。**§11.3 的旧名 `kafka_consumer_pool_size_per_broker`/`kafka_consumer_idle_timeout_ms` 不存在。**
修复 [改]:① cap 做真配置 `routine_load_kafka_consumer_pool_size`(默认 ~64–128,0=不限);② 按 `(broker, topic, 连接相关属性 hash)` **分桶**;③ **`group.id` 移出 match key**(消费基于 `assign()`、offset 由 SR 管,安全);④ 满桶 LRU 驱逐**异 key** idle consumer,而非丢刚返还的热 consumer;⑤ 接上 idle 回收(`start_bg_worker` 调 `_clean_idle_consumer_bg`);⑥ 池迁到 `exec_env` 作用域,供 `KafkaScanNode`(exec 层)访问。

### 5.2 反压
复用 INSERT pipeline 背压:`OlapTableSink` 阻塞 → 引擎暂停上游 `KafkaScanNode.get_next()`(`pipeline_driver.cpp:529-531` 置 `OUTPUT_FULL`)。**非"零积压"**:数据先填 per-NodeChannel 发送缓冲(`send_channel_buffer_limit=64MB × N`)+ scan buffer,恢复由 `pipeline_driver_poller.cpp` ~10ms 粒度;最坏在途内存 ≈ `N_nodes×64MB + scan buffer`,受共享 `load_process_max_memory_limit`(§5.3)约束。默认 `replicated_storage` 开时 scan+sink 同 fragment 直接背压;关时 PK 表 `InsertPlanner` 插 `SHUFFLE_AGG` exchange 把 sink 放另一 fragment、加一层 SinkBuffer 滞后(建议低延迟 pipe 保持 replicated_storage 开)。
**stall watchdog [新增]**:`KafkaScanNode` 跟踪无 offset 进展时长,超 `batch_timeout` 以可重试错误 fail fragment → txn abort → 下批以更大 batch 重消费(与 HLD §10 降级联动);abort 须释放资源车道、per-load 内存、归还 consumer。当前无此 watchdog(driver 长期 `OUTPUT_FULL` 挂到通用超时)。

### 5.3 资源隔离
分层(取代 v0.x 的 §13.1 vs §13.2 矛盾):① **准入**不按集群利用率阻塞批次,进 Query Queue / Resource Group;② **并行度阻尼** = §1/HLD §9 的资源信号**仅抑制 scale-up**,从不阻塞批次;③ **专用车道** = `KafkaPipeSubmitter` 独立并发预算/优先级;④ 真正 receiver 背压是**共享的** `load_process_max_memory_limit`(默认 30% BE 内存,所有 load 共用,Resource Group 不细分),如需 per-pipe 隔离给每批设 `task.load_mem_limit`(由 `max_batch_size×DOP` 推导);⑤ commit-rate 背压纳入 HLD §10。
> "INSERT 天然受 Query Queue 保护" **默认是空头支票**(`enable_query_queue_load=false`、cpu/mem permille/pct 阈值默认 0 不生效),文档要求显式开启或由专用车道自管。

### 5.4 存算分离 scan 侧（KafkaScanNode）
对应 HLD §19 "Scan 侧" 行 defer 来的四点(此前 dangling,修复 SF-1):
- **CN 分配**:`KafkaScanNode.getScanRangeLocations()` 产出的每个 partition scan-range 须落到 **warehouse 的可用 CN**。落地经 `JobSpec` 携带的 `ComputeResource`(`JobSpec.java` 的 computeResource)→ `CoordinatorPreprocessor` 用 `jobSpec.getComputeResource()` + `WorkerProvider` 选址(同现有 MPP scan 在 shared-data 的选址路径)。
- **与 §9.3 sticky 协调**:现有 `StreamLoadScanNode.assignBackends` 是 shuffle 式(不 pin partition→CN);而本设计要 sticky(partition→instance 稳定以保暖 consumer)。须在 `KafkaScanNode` 的 location 产出处用**持久化的 partition→CN 映射**覆盖默认 shuffle,仅在并行度/节点集变更时增量再均衡。
- **`computeResource`/warehouse 流入**:`JobSpec.Factory.fromKafkaPipeBatch`(§1.5)须把 pipe 的 `warehouse`/`computeResource` 透传进 jobSpec,使 scan-range location 与 sink 走同一 warehouse。
- **storage-less CN**:CN 无本地存储,consumer 池(§5.1)按 exec_env 作用域在每个 CN 上独立存在,与 datacache **无关**(Kafka 消费不走 datacache;datacache 只服务 tablet 读)。
> warehouse/computeResource 路由完整集成属 **Phase 3**(HLD §23);Phase 1 可先用默认 warehouse + shuffle 选址,暂不保证 sticky(接受 consumer 冷启动,见 §5.1)。

---

## 6. 现状代码事实锚点（核实清单）

集中所有 file:line 证据(供实现/审阅定位;行号指示性):
- **版本墙**:shared-nothing `tablet_max_versions=1000`(`be/src/common/config.h:991`);超限补救建议"调大降频"(`delta_writer.cpp:194-202`)。
- **提交限速(shared-data)**:`CommitRateLimiter`(`CommitRateLimiter.java:64-130`);默认开 `lake_enable_ingest_slowdown=true`,`lake_ingest_slowdown_threshold=100`,`lake_compaction_score_upper_bound=2000`(`Config.java:3465/3471/3484`);限速器在 lake commit 路径(`LakeTableTxnStateListener.java:145`)。
- **publish 成本(shared-data)**:`transactions.cpp:186`(读基线 metadata)/`:192`(写新 tablet_metadata)、`delta_writer.cpp:897`(per-tablet txn_log);异步 `PublishVersionDaemon.java:124-165`;慢阈值 `lake_publish_version_slow_log_ms=1000`(`config.h:1326`)。
- **NO_ROWS_IMPORTED 推进**:`KafkaRoutineLoadJob.java:367-380`;INSERT 路径 abort 串 `FILTER_DATA_ERR`(`StmtExecutor.java:3436/3440`)、`ERR_NO_ROWS_IMPORTED`(`:3445/3300/3456`)。
- **凭证渲染/落盘点(SEC-1,§17 的"唯一脱敏点"今天不存在,须新建)**:`DESC PIPE` 第 8 列 `Pipe.getPropertiesJson()`(`Pipe.java:698-704`,纯 `new Gson().toJson(properties)`、**零脱敏**)与 originSql 原样 dump(`ShowExecutor.java:3037`)都是渲染点,**不止** TVF `toSql()`——三者都须经统一脱敏。**Kafka 连接/凭证属性不得进通用 `Pipe.properties` map**(否则经 `getPropertiesJson` 明文漏出),须存入 `KafkaPipeSource` 的结构化字段并在渲染处脱敏。审计 redactor `SqlCredentialRedactor` 不含 `sasl.jaas.config`/`ssl.*.password`,须扩。
- **凭证 at-rest(SEC-2)**:结构化凭证字段(BE 运行时认证需真值)随 `KafkaPipeSource`(`@SerializedName`)持久化进 image/WAL,**Phase 1 仍是明文**(同今天 RL 的 `customProperties`);§17 ④ 的 Phase-1 保证仅限**展示/审计/SQL 文本**路径,at-rest 加密留 Phase 3(AES)。
- **错误容忍默认**:`enable_insert_strict=true`(`SessionVariable.java:1650`)、`insert_max_filter_ratio=0`(`:1656`);INSERT ratio abort(`StmtExecutor.java:3422`)。RL 默认 `DEFAULT_MAX_ERROR_NUM=0`(`RoutineLoadJob.java:141`)、`DEFAULT_MAX_BATCH_ROWS=200000`(`:142`)、`maxFilterRatio=1.0`(`:215`)。
- **consumer 池**:硬编码 cap 10(`routine_load_task_executor.h:64`)、丢弃返还(`data_consumer_pool.cpp:142`)、idle 回收死代码、`match()` 含 group.id(`data_consumer.cpp:470-488`)。
- **调度地板**:TaskManager 1s tick(`TaskManager.java:142`)、`task_runs_concurrency=4`(`TaskRunScheduler.java:127`)、`pipe_scheduler_interval_millis=1000`(`Config.java:3766`)、无 INSERT plan cache(`PrepareStmtPlanner` 仅 point-query)。
- **自动建分区**:BE sink 运行时经 `createPartition` RPC(`tablet_sink.cpp:691/440` → `FrontendServiceImpl.createPartition:2156`,按 `(tableId, txn_id, partition_values)` 去重 `:2212`)。
