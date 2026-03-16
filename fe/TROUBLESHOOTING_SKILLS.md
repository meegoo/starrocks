# FE Troubleshooting Skills

> 从实际排查中提炼的可复用排查方法论。

## 1. 先区分概念

- 不要把不同机制混为一谈：用户显式操作（如 `ALTER TABLE`）与系统内部自动操作（如导入触发的自动分区创建）往往是**不同代码路径**。
- 用错误文案、异常类名在代码里搜索，先确认**是谁在抛错、属于哪条链路**。

## 2. 按链路追踪

1. **定位抛出点**：用错误信息或异常类型（如 `InvalidOlapTableStateException`）在代码里搜索。
2. **回溯调用链**：从抛出点往上看，理清调用栈和分支条件。
3. **结合运行时验证**：用 `SHOW`、`EXPLAIN`、日志等确认实际走到哪条分支、处于何种状态。

## 3. 日志与 SQL 命令配合

- **SHOW 类命令**：`SHOW ALTER TABLE COLUMN`、`SHOW BACKENDS`、`SHOW PROC` 等，用于查看任务状态、拓扑、元信息。
- **日志关键字**：根据错误提示选取对应关键字（如 `cancel failed`、`create partition`、`schema change`）在 FE/BE 日志中搜索。
- **日志路径**：FE 通常 `fe/log/fe.log`、`fe/log/fe.warn.log`；BE 通常 `be/log/be.INFO`、`be/log/be.WARNING`（按实际部署调整）。

## 4. 理解状态机与阻塞条件

- 对有状态流程（如 ALTER、分区创建、物化视图刷新），先弄清**状态流转**和**各状态的进入/退出条件**。
- 特别关注**不可取消、必须等待**的状态：这类状态会让表/任务长时间“卡住”，无法通过 cancel 绕过。
- 用 `git log -S "状态名"` 或 `git blame` 查引入该状态的 PR/Commit，有助于理解设计意图和变更背景。

## 5. 集群与跳板机

- 若需登集群查日志，可按 AGENTS.md 中的说明通过跳板机（`SSH_HOST`）访问 FE/BE 节点。
- 先用 `SHOW BACKENDS` 等获取节点 IP，再 SSH 到对应机器查看日志。
