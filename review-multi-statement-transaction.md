# StarRocks Multi-Statement Transaction 实现 Review 报告

## 一、概述

StarRocks 的 Multi-Statement Transaction（多语句显式事务）允许用户在一个会话中通过 `BEGIN`/`COMMIT`/`ROLLBACK` 显式控制事务边界，在事务内执行多条 DML 语句（INSERT/UPDATE/DELETE），最终统一提交或回滚。

该功能通过 Session Variable `enable_sql_transaction`（默认 `true`）控制开关。

---

## 二、架构总览

```
用户 SQL 请求
     │
     ▼
┌─────────────────────────────────────────┐
│         ConnectProcessor                │  解析多条语句，调用 ExplicitTxnStatementValidator
│         (ConnectContext.txnId)           │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│           StmtExecutor                  │  分发 BEGIN/COMMIT/ROLLBACK/DML
│   if (txnId != 0) → 走事务路径          │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│      TransactionStmtExecutor            │  核心执行逻辑
│  beginStmt / loadData / commitStmt /    │
│  rollbackStmt                           │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│      GlobalTransactionMgr               │
│  explicitTxnStateMap (ConcurrentMap)    │  存储 ExplicitTxnState
│  ┌───────────────────────────────────┐  │
│  │ ExplicitTxnState                  │  │
│  │  ├─ TransactionState              │  │  事务元数据
│  │  ├─ List<ExplicitTxnStateItem>    │  │  每条 DML 的执行结果
│  │  ├─ modifiedTableIds              │  │  已修改表 ID（用于 SELECT 校验）
│  │  └─ tableHasExplicitStmt          │  │  语句顺序校验
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

---

## 三、核心文件清单

| 文件 | 职责 |
|------|------|
| `fe/fe-grammar/StarRocks.g4:2344-2356` | BEGIN/COMMIT/ROLLBACK 语法定义 |
| `fe/fe-parser/.../ast/txn/BeginStmt.java` | BEGIN AST 节点，支持 WITH LABEL |
| `fe/fe-parser/.../ast/txn/CommitStmt.java` | COMMIT AST 节点 |
| `fe/fe-parser/.../ast/txn/RollbackStmt.java` | ROLLBACK AST 节点 |
| `fe/fe-core/.../qe/ConnectContext.java:265` | `txnId` 字段，标识当前会话是否处于事务中 |
| `fe/fe-core/.../qe/SessionVariable.java:1288` | `enable_sql_transaction` 开关 |
| `fe/fe-core/.../qe/StmtExecutor.java:1122-1142` | BEGIN/COMMIT/ROLLBACK 分发逻辑 |
| `fe/fe-core/.../qe/StmtExecutor.java:2994` | DML 进入事务路径的判断 `if (context.getTxnId() != 0)` |
| `fe/fe-core/.../qe/ConnectProcessor.java:517` | 多语句解析时调用 `ExplicitTxnStatementValidator` |
| `fe/fe-core/.../transaction/TransactionStmtExecutor.java` | **核心**：事务的 begin/load/commit/rollback 执行 |
| `fe/fe-core/.../transaction/ExplicitTxnState.java` | 显式事务状态，积累多条 DML 的中间结果 |
| `fe/fe-core/.../transaction/ExplicitTxnStatementValidator.java` | 事务内语句合法性校验 |
| `fe/fe-core/.../transaction/GlobalTransactionMgr.java:102` | `explicitTxnStateMap` 全局存储 |
| `fe/fe-spi/.../common/ErrorCode.java:5300-5307` | 事务相关错误码 |
| `fe/fe-core/.../http/rest/transaction/MultiStatementTransactionHandler.java` | HTTP REST 多语句事务（Stream Load 场景） |

---

## 四、执行流程详解

### 4.1 BEGIN

**入口**: `StmtExecutor.java:1122` → `TransactionStmtExecutor.beginStmt()`

1. 检查 `enable_sql_transaction` 是否为 true，否则仅保持语法兼容返回 OK
2. 若 `context.getTxnId() != 0`（已在事务中），**不创建新事务**，返回已有事务信息
   - 如果指定了不同的 label，抛出 SemanticException
3. 生成 `transactionId`（通过 `TransactionIdGenerator`）
4. Label 优先级：`stmt.getLabel()` > `labelOverride` > `executionId`
5. 校验 label 合法性（`FeNameFormat.checkLabel`）和全局唯一性（`checkLabelUsedInAnyDatabase`）
6. 创建 `TransactionState`（状态为 PREPARE）
7. 创建 `ExplicitTxnState`，放入 `GlobalTransactionMgr.explicitTxnStateMap`
8. 设置 `context.setTxnId(transactionId)`

### 4.2 DML（INSERT/UPDATE/DELETE）

**入口**: `StmtExecutor.java:2994` → `TransactionStmtExecutor.loadData()`

1. 判断 `context.getTxnId() != 0`，走事务路径
2. **跨库校验**：若事务已绑定 database，新 DML 的目标库必须一致，否则报 `ERR_TXN_FORBID_CROSS_DB`
3. **同表校验**：对于非 Cloud Native 表，不允许多次 DML 同一张表（`ERR_TXN_IMPORT_SAME_TABLE`）
4. **语句顺序校验**：同一表上，UPDATE/DELETE 必须在 INSERT 之前执行（`ERR_EXPLICIT_TXN_NOT_SUPPORT_STMT_ORDER`）
5. 记录 `modifiedTableId` 到 ExplicitTxnState（后续 SELECT 校验用）
6. 调用 `load()` 执行实际数据加载：
   - 创建 Coordinator 执行 Fragment
   - 收集 TabletCommitInfo 和 TabletFailInfo
   - 统计 loadedRows / filteredRows / loadedBytes
   - 校验 filter ratio
7. 结果封装为 `ExplicitTxnStateItem`，添加到 `ExplicitTxnState`

### 4.3 SELECT

**入口**: `StmtExecutor.java:510-514` + `ConnectProcessor.java:517`

1. **Leader 转发**：事务内 SELECT 强制转发到 Leader FE 执行，确保能校验已修改表的一致性
2. **语句校验**：`ExplicitTxnStatementValidator.validate()` 检查 SELECT 是否读取了事务内已修改的表
   - 若 SELECT 引用了 `modifiedTableIds` 中的表，报 `ERR_EXPLICIT_TXN_SELECT_ON_MODIFIED_TABLE`
   - INSERT ... SELECT 的源查询也会被校验
3. 正常执行查询（不受事务影响，不参与事务提交）

### 4.4 COMMIT

**入口**: `StmtExecutor.java:1130` → `TransactionStmtExecutor.commitStmt()`

1. 获取 `ExplicitTxnState`，若为 null 则忽略（BEGIN 未执行）
2. 若无任何 DML（items 为空），直接返回 VISIBLE 状态
3. 聚合所有 DML 的 `TabletCommitInfo` 和 `TabletFailInfo`
4. 调用 `retryCommitOnRateLimitExceeded()` 提交事务（带速率限制重试）
5. 等待 Publish 完成：
   - 成功则状态为 `VISIBLE`
   - 超时则状态为 `COMMITTED`（数据最终会 VISIBLE，但可能有延迟）
6. 记录 Load Job 完成状态和 Metrics
7. **finally 块**中清理：`clearExplicitTxnState()` + `context.setTxnId(0)`

### 4.5 ROLLBACK

**入口**: `StmtExecutor.java:1137` → `TransactionStmtExecutor.rollbackStmt()`

1. 获取 `ExplicitTxnState`，若为 null 则忽略
2. 若无任何 DML，直接返回 ABORTED 状态
3. 聚合所有 DML 的 commit/fail 信息
4. 调用 `transactionMgr.abortTransaction()` 中止事务
5. 记录 Load Job 取消状态
6. **finally 块**中清理：`clearExplicitTxnState()` + `context.setTxnId(0)`

### 4.6 FE Leader-Follower 转发

**入口**: `ConnectProcessor.java:1022-1090`

- Follower 转发请求到 Leader 时，通过 Thrift RPC 传递 `txn_id`
- Leader 执行后将更新后的 `txn_id` 返回给 Follower
- Follower 更新本地 `ConnectContext.txnId`

---

## 五、约束与限制

| 约束 | 错误码 | 说明 |
|------|--------|------|
| 支持的语句类型 | `ERR_EXPLICIT_TXN_NOT_SUPPORT_STMT` (5305) | 仅支持 BEGIN/COMMIT/ROLLBACK/INSERT/UPDATE/DELETE/SET/SELECT |
| INSERT OVERWRITE 不支持 | `ERR_EXPLICIT_TXN_NOT_SUPPORT_STMT` (5305) | OVERWRITE 语义与事务冲突 |
| 禁止跨库 | `ERR_TXN_FORBID_CROSS_DB` (5304) | 所有 DML 目标表须属于同一数据库 |
| 禁止同表多次 DML | `ERR_TXN_IMPORT_SAME_TABLE` (5303) | 非 Cloud Native 表不允许同一表多次 DML |
| 语句顺序 | `ERR_EXPLICIT_TXN_NOT_SUPPORT_STMT_ORDER` (5306) | 同表上 UPDATE/DELETE 必须在 INSERT 之前 |
| SELECT 不可读已修改表 | `ERR_EXPLICIT_TXN_SELECT_ON_MODIFIED_TABLE` (5307) | 防止读取未提交的脏数据 |

---

## 六、发现的问题与改进建议

### 6.1 [严重] 连接断开时显式事务状态未清理 — 内存泄漏风险

**问题描述**: `ConnectContext.cleanup()` 方法（`ConnectContext.java:891`）在连接关闭时**不会清理 `explicitTxnStateMap` 中的条目**。如果客户端在 BEGIN 之后、COMMIT/ROLLBACK 之前断开连接，`explicitTxnStateMap` 中的 `ExplicitTxnState` 将永远不会被清理。

**影响**:
- 内存泄漏：每个未完成的事务在 `explicitTxnStateMap` 中残留
- `checkLabelUsedInAnyDatabase()` 遍历时性能逐步退化
- 残留的 label 可能阻止新事务使用相同 label

**建议**: 在 `ConnectContext.cleanup()` 中添加显式事务清理逻辑：
```java
public synchronized void cleanup() {
    if (closed) return;
    closed = true;
    // 清理显式事务状态
    if (txnId != 0) {
        GlobalStateMgr.getCurrentState().getGlobalTransactionMgr()
            .clearExplicitTxnState(txnId);
        txnId = 0;
    }
    mysqlChannel.close();
    // ...
}
```

### 6.2 [严重] 无超时清理机制

**问题描述**: `GlobalTransactionMgr.abortTimeoutTxns()` 仅处理 `DatabaseTransactionMgr` 中的事务，**不覆盖 `explicitTxnStateMap`**。由于 BEGIN 时 `TransactionState` 仅在首次 DML 时才注册到 `DatabaseTransactionMgr`，若事务只执行了 BEGIN 但未执行任何 DML，该事务将永远不会被超时清理。

**建议**: 在 `abortTimeoutTxns()` 中增加对 `explicitTxnStateMap` 的超时扫描：
```java
public void abortTimeoutTxns() {
    long currentMillis = System.currentTimeMillis();
    // 已有逻辑：清理 DatabaseTransactionMgr 中的超时事务
    for (DatabaseTransactionMgr dbTransactionMgr : dbIdToDatabaseTransactionMgrs.values()) {
        dbTransactionMgr.abortTimeoutTxns(currentMillis);
    }
    // 新增：清理 explicitTxnStateMap 中的超时事务
    Iterator<Map.Entry<Long, ExplicitTxnState>> it = explicitTxnStateMap.entrySet().iterator();
    while (it.hasNext()) {
        Map.Entry<Long, ExplicitTxnState> entry = it.next();
        TransactionState txnState = entry.getValue().getTransactionState();
        if (txnState != null && txnState.isTimeout(currentMillis)) {
            it.remove();
        }
    }
}
```

### 6.3 [中等] DML 执行失败时事务未自动中止

**问题描述**: 在 `TransactionStmtExecutor.loadData()` 中（第 215-217 行），DML 执行出错时仅 `context.getState().setError(e.getMessage())`，但**事务仍然保持 PREPARE 状态**，`txnId` 不清零。后续用户仍可继续在这个"部分失败"的事务中执行其他语句或尝试 COMMIT。

这与传统数据库（如 PostgreSQL/MySQL InnoDB）的行为不同——通常 DML 失败会将事务标记为需要回滚。

**影响**: 部分失败的事务如果被 COMMIT，可能导致数据不一致（部分 DML 成功，部分失败）。

**建议**: 考虑在 DML 失败时自动标记事务为 "需要回滚" 状态，或者至少在 COMMIT 时检查是否有失败的 DML。

### 6.4 [中等] `ExplicitTxnState.ExplicitTxnStateItem` 持有 DmlStmt 引用

**问题描述**: `ExplicitTxnStateItem` 中的 `dmlStmt` 字段（`ExplicitTxnState.java:86`）持有完整的 DML 语句 AST 对象。在长事务中，多条 DML 的 AST 对象会一直驻留内存直到事务结束。

**影响**: 对于包含大量 VALUES 的 INSERT 语句，AST 对象可能占用显著内存。

**建议**: 考虑仅保留必要的元信息（如 table name、DML type），而非整个 AST 对象。`dmlStmt` 仅在 `commitStmt` 中被用于 `instanceof InsertStmt` 判断和获取 `tableRef`，可以将所需信息提前提取。

### 6.5 [中等] `tableHasExplicitStmt` 使用表名而非表 ID

**问题描述**: `ExplicitTxnState.tableHasExplicitStmt` 使用 `String tableName` 作为 key（`ExplicitTxnState.java:35`），而 `modifiedTableIds` 使用 `Long tableId`（`ExplicitTxnState.java:38`）。两套机制不一致。

**影响**: 如果在事务执行期间表被 RENAME，基于 tableName 的校验可能产生误判。

**建议**: 统一使用 table ID 进行校验。

### 6.6 [低] 同表限制对 Cloud Native 表的特殊处理缺乏注释

**问题描述**: `TransactionStmtExecutor.loadData()` 第 188 行：
```java
if (transactionState.getTableIdList().contains(table.getId()) && !table.isCloudNativeTableOrMaterializedView()) {
    throw ErrorReportException.report(ErrorCode.ERR_TXN_IMPORT_SAME_TABLE);
}
```
Cloud Native 表被豁免同表多次 DML 的限制，但缺乏注释说明原因。

**建议**: 添加注释说明 Cloud Native 表为何可以支持同表多次 DML。

### 6.7 [低] BEGIN 重复执行的 NullPointerException 风险

**问题描述**: `TransactionStmtExecutor.beginStmt()` 第 103-104 行：
```java
ExplicitTxnState explicitTxnState = globalTransactionMgr.getExplicitTxnState(context.getTxnId());
String existingLabel = explicitTxnState.getTransactionState().getLabel();
```
若 `explicitTxnState` 为 null（理论上不应发生，但在并发清理场景下可能出现），会抛出 NPE。

**建议**: 添加空值检查。

### 6.8 [低] commitStmt 中日志消息不准确

**问题描述**: `TransactionStmtExecutor.commitStmt()` 第 349 行：
```java
LOG.warn("errors when abort txn", e);
```
这是在 commit 路径中的异常处理，但日志消息写的是 "abort txn"，容易造成排查困扰。

**建议**: 修正为 `"errors when commit txn"`。

### 6.9 [低] `checkLabelUsedInAnyDatabase` 线性扫描性能

**问题描述**: `GlobalTransactionMgr.checkLabelUsedInAnyDatabase()` 会遍历所有 `DatabaseTransactionMgr` 和整个 `explicitTxnStateMap`。在高并发显式事务场景下可能成为性能瓶颈。

**建议**: 考虑为显式事务维护一个 label → txnId 的索引。

### 6.10 [建议] HA/Failover 场景下事务状态丢失

**问题描述**: `explicitTxnStateMap` 是纯内存结构，**不持久化到 BDBJE/Journal**。当 FE Leader 发生切换时，所有进行中的显式事务状态会丢失。客户端后续发送 COMMIT/ROLLBACK 时，`getExplicitTxnState()` 返回 null，操作被静默忽略。

**影响**: 用户可能不知道事务已丢失，COMMIT 被忽略而非报错。

**建议**:
- 至少在 Leader 切换后，COMMIT/ROLLBACK 应返回明确的错误信息而非静默成功
- 当前 `commitStmt()` 在 `explicitTxnState == null` 时直接 return，应改为报错

---

## 七、测试覆盖分析

**测试文件**: `fe/fe-core/src/test/java/com/starrocks/transaction/ExplicitTxnTest.java`

### 已覆盖场景

| 场景 | 测试方法 |
|------|---------|
| 基本 BEGIN/COMMIT/ROLLBACK 流程 | `testBegin`, `testCommitEmptyInsert` |
| BEGIN WITH LABEL | `testBeginWithLabel`, `testBeginWithoutLabel` |
| 无效 Label 校验 | `testBeginWithInvalidLabel` |
| 重复 BEGIN（相同/不同 label） | `testBeginWithDifferentLabelWhenTxnExists` |
| 不支持的语句类型 | `testNotSupportStmt` |
| 同表重复 DML | `testInsertSameTable` |
| 空事务 COMMIT/ROLLBACK | `testCommitEmptyInsert` |
| 数据库不存在时的 COMMIT/ROLLBACK | `testCommitDatabaseNotExist` |

### 缺失的测试场景

| 缺失场景 | 重要性 |
|----------|--------|
| **跨库事务拒绝** (`ERR_TXN_FORBID_CROSS_DB`) | 高 |
| **SELECT 读取已修改表** (`ERR_EXPLICIT_TXN_SELECT_ON_MODIFIED_TABLE`) | 高 |
| **INSERT ... SELECT 源查询校验** | 高 |
| **语句顺序校验**（UPDATE/DELETE 在 INSERT 之后） | 高 |
| **DML 执行失败后继续事务** | 高 |
| **连接断开后的事务清理** | 高 |
| **FE Leader 切换后的事务行为** | 中 |
| **事务超时场景** | 中 |
| **并发显式事务（多会话）** | 中 |
| **Cloud Native 表的同表多次 DML** | 低 |
| **大事务性能（100+ DML）** | 低 |

---

## 八、HTTP REST 多语句事务（Stream Load 场景）

除了 SQL 显式事务外，StarRocks 还支持通过 HTTP REST API 进行多语句事务，用于 Stream Load 场景：

- `MultiStatementTransactionHandler.java`: 处理 `TXN_BEGIN`/`TXN_COMMIT`/`TXN_ROLLBACK`/`TXN_LOAD` 操作
- 底层调用 `StreamLoadMgr.beginMultiStatementLoadTask()` 等方法
- 与 SQL 显式事务是**独立的两套实现**，互不影响

---

## 九、总结

### 优点
1. **清晰的分层设计**: AST 解析 → 语句分发 → 事务执行 → 状态管理，职责明确
2. **合理的约束校验**: 跨库、同表、语句顺序、SELECT 读脏等限制考虑较全面
3. **ConcurrentHashMap 保证线程安全**: `explicitTxnStateMap` 使用并发容器
4. **Leader 转发机制**: 事务内 SELECT 转发到 Leader，保证一致性校验
5. **Label 支持**: BEGIN WITH LABEL 提供了灵活的事务标识

### 主要风险
1. **内存泄漏**: 连接断开、事务超时时 `explicitTxnStateMap` 无清理机制（严重）
2. **部分失败事务可提交**: DML 失败不中止事务，可能导致数据不一致（中等）
3. **HA 场景状态丢失**: 纯内存状态不持久化，Leader 切换后事务静默丢失（中等）
4. **测试覆盖不足**: 多个重要场景缺少测试用例（中等）
