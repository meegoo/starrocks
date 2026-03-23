---
name: starrocks-tsp-be-sql-test
description: 在远程编译机 Docker 内编译 StarRocks BE、部署到 TSP 集群并运行 SQL 集成测试（按类别组织的流程与约定）。
---

> **存放**：仓库路径 `skills/starrocks-tsp-be-sql-test/SKILL.md`。若 Cursor 从 `.cursor/skills/` 加载，可复制或 symlink（`.cursor` 可能被 gitignore）。

# StarRocks：TSP + BE + SQL 测试（分类 Skill）

## 分类索引

| 类别 | 抽象含义 | 本文档章节 |
|------|----------|------------|
| **I. 场景** | 何时使用、端到端目标 | [§1](#1-场景与范围) |
| **II. 源码协作** | 分支、合并、远程引用、worktree | [§2](#2-源码与-git协作) |
| **III. 运行时环境** | 凭据、网络、Docker 挂载拓扑 | [§3](#3-运行时环境与凭据) |
| **IV. 构建系统** | 唯一入口 `build.sh`、clean/并行策略 | [§4](#4-构建系统) |
| **V. 制品与发布** | `output/be` 语义、部署到集群 | [§5](#5-制品与发布部署) |
| **VI. 质量验证** | SQL 集成测试、T/R 模式、结果解读 | [§6](#6-质量验证-sql-测试) |
| **VII. 诊断** | 常见失败模式与判定 | [§7](#7-诊断与排错) |
| **VIII. 仓库政策** | commit 规范、skill 维护 | [§8](#8-仓库政策) |

---

## 1. 场景与范围

**适用**：Cursor Cloud / Agent 在 **远程编译机 + Docker** 中编 **BE（`--enable-shared-data`）**，将产物部署到 **TSP StarRocks 集群**，并在 **远程机** 上跑 **sql-tester**（非本地 Cloud VM 直连集群）。

**典型动机**：验证 **chunk-level random distribution** 等仅改 BE 的特性；用 **新增 T 用例** 在真实集群上压测/录制。

**不在此 skill 范围**：纯 FE 改动、无 TSP 的本地单测、在 Cloud VM 内直接 `./build.sh`（违反 `AGENTS.md` 时以仓库规则为准）。

---

## 2. 源码与 Git 协作

### 2.1 分支模型（实例）

- **功能线**：`claude/chunk-random-distribution-RfcL7`（可替换为其它 feature 分支名）。
- **集成线（推荐）**：`meegoo/chunk-random-sql-verify` = 功能线最新 + 从 `dev` 拾取的运维脚本（TSP、远程 SQL、`deploy_to_cluster.sh`），避免功能分支缺脚本。

### 2.2 同步上游功能

- `git fetch origin --prune` 后 `git merge origin/<feature>`。
- 若 feature 已 **merge main**，合并体积大，后续构建宜按 [§4](#4-构建系统) 做一次 **clean 全量**。

### 2.3 远端跟踪引用损坏

- 现象：`cannot lock ref` / `expected <old-sha>`。
- 处理：强制写跟踪分支，例如  
  `git fetch origin +claude/chunk-random-distribution-RfcL7:refs/remotes/origin/claude/chunk-random-distribution-RfcL7`

### 2.4 Worktree 与多 Agent

- 现象：远程 `git worktree add` 报 **branch already checked out**。
- 原因：同分支在另一 Agent 目录已检出。
- 处理：使用 **独立分支名** 推送后再跑 `run_sql_test_remote.sh`；或协调避免多 worktree 抢同一分支。

---

## 3. 运行时环境与凭据

| 变量 | 用途 |
|------|------|
| `TSP_HOST`, `TSP_USERNAME`, `TSP_PASSWORD` | TSP 登录、申请集群、`--get-address` |
| `SSH_HOST`, `SSH_USERNAME`, `SSH_PASSWORD` | 远程编译机；`run_sql_test_remote.sh` / 手工 ssh |

**网络**：以 **远程编译机** 能否 `mysql -h<FE>:9030` 为准；Cloud VM 能通不代表远程机能通。

**Docker 挂载（与 `AGENTS.md` 一致）**：worktree → `/root/src/starrocks`；基线 `${BASE_REPO}/.git` **只读** 同挂；`.m2`、`TMPDIR` 等按环境文档。

---

## 4. 构建系统

### 4.1 唯一入口

- **只使用** `./build.sh --be --enable-shared-data`（及本 skill 允许的变体）。
- **禁止**把在 `be/build_Release` 下直接 `make` 作为常规编译路径（排错时亦优先通过 `build.sh` / 清理目录解决）。

### 4.2 `--clean` 策略（抽象规则）

| 条件 | 是否加 `--clean` |
|------|------------------|
| 首次编 BE / 大合并后 / 链接或 `.o truncated` 等树损坏 | **加** |
| 小范围增量改动、树健康 | **不加** |

### 4.3 并行度 `-j`

- 默认由 `build.sh` 解析；用户未要求时可不传。
- 出现 **并行相关损坏** 时：降低 `-j` 或回到 **clean 全量**（仍通过 `build.sh`）。

### 4.4 宿主机预处理（非 make）

- 若容器内 `build.sh --clean` 因目录无法删净失败：在 **宿主机** 对 `be/build_Release`、`be/output`、`output/be` 做 `chmod`/删除后再进容器跑 `build.sh`。

---

## 5. 制品与发布部署

### 5.1 制品路径语义

- **部署脚本消费**：仓库根 `output/be/lib`、`output/be/bin`（由 **完整成功** 的 `build.sh` 流水线写出）。
- **仅存在** `be/output/lib/starrocks_be` **但无** 规范 `output/be/**`：视为 **流水线未跑完**（install / java-ext / 拷贝阶段失败），**不应部署**。

### 5.2 部署步骤（抽象）

1. **拓扑**：在能连 FE 的环境执行 `show backends\G` 取 BE IP。
2. **密钥**：`deploy_to_cluster.sh --setup-ssh`（首次），默认 `sr` / `sr`，根路径 `/home/disk1/sr`。
3. **更新 BE**：`--be --clean-log --be-hosts ...`（在 worktree 根、且 `output/be` 已就绪）。

---

## 6. 质量验证（SQL 测试）

### 6.1 用例类型

- **T 文件**：待执行语句；无对应 **R** 时先用 **录制** `-r`。
- **R 文件**：期望输出；用 **校验** `-v` 与 T 成对使用。

### 6.2 示例（chunk random）

- 路径：`sql/test_random_distribution/T/test_chunk_random_distribution`
- 常配：`-a sequential -c 1`（与用例 `@sequential` 一致）。

```bash
export SR_FE=<fe_host>:9030
./test/scripts/run_sql_test_remote.sh \
  -r -d sql/test_random_distribution/T/test_chunk_random_distribution \
  -a sequential -c 1 -t 600
```

- `SR_FE` 可由 `./tools/tsp_quick_apply.sh --get-address <agent_id 或集群名片段>` 得到。

### 6.3 一键 TSP（可选）

```bash
./tools/tsp_run_sql_test.sh --apply-from 7011 [其它 run_sql_test_remote 参数...]
```

非默认目录需显式 `-d`。

### 6.4 结果语义

- **`-r` 成功**：框架无异常；**不等于** 业务断言为真。
- 例：录制里 `compression_ratio_acceptable` 为 `0` 表示 **该 SQL 表达式为假**，需在 **用例设计或产品逻辑** 上解读，而非仅看 “OK”。

---

## 7. 诊断与排错

| 现象 | 归类 | 常见处理方向 |
|------|------|----------------|
| `file truncated` / `ranlib` / 链接 undefined | 构建树 / 并行 | `build.sh` + `--clean`；必要时宿主机清目录；降 `-j` |
| 无 `output/be` | 制品 | 确认 `build.sh` 全程 exit 0 |
| 远程 `Connection refused` FE | 网络 / 就绪 | 等集群 Running；在 **远程机** 测 `mysql` |
| worktree 冲突 | Git / 多 Agent | 换独立分支名 |
| `git fetch` ref 异常 | Git 元数据 | 强制 fetch 更新 `refs/remotes/...` |

---

## 8. 仓库政策

- **Commit author**：`meegoo <meegoo.sr@gmail.com>`
- **Signed-off-by**：`Signed-off-by: meegoo <meegoo.sr@gmail.com>`
- **Skill 维护**：新增类别时同步更新 [分类索引](#分类索引) 表与对应章节，避免重复叙述（细节可引用 `AGENTS.md`）。
