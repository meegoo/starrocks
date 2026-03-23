---
name: starrocks-tsp-be-sql-test
description: 在远程编译机 Docker 内编译 StarRocks BE、部署到 TSP 集群并运行 SQL 集成测试（含分支与 worktree 约定）。
---

> **存放说明**：本文件在仓库 `skills/` 下可被 git 跟踪。若本地 Cursor 从 `.cursor/skills/` 加载 skill，可复制或 symlink 至该目录（注意 `.gitignore` 可能忽略 `.cursor`）。

# StarRocks：TSP 集群 BE 编译部署与 SQL 测试

## 目标

在 **Cursor Cloud / Agent** 场景下：拉取含 **chunk random distribution** 等改动的分支，于 **远程编译服务器** 的 Docker 中编译 **BE（shared-data）**，将产物部署到 **TSP 申请的集群**，并在远程执行 **新增 SQL 测试**。

## 分支与代码

- **功能分支**：`claude/chunk-random-distribution-RfcL7`（或其它 feature 分支）。
- **集成分支（推荐）**：`meegoo/chunk-random-sql-verify` = 功能分支最新提交 + 从 `dev` 并入的脚本：
  - `tools/tsp_quick_apply.sh`、`tools/tsp_run_sql_test.sh`
  - `test/scripts/run_sql_test_remote.sh`
  - `tools/deploy_to_cluster.sh`
- **同步上游功能**：`git fetch origin --prune` 后合并 `origin/claude/chunk-random-distribution-RfcL7`（若其已 merge `main`，变更面会很大）。
- **`git fetch` 报 ref 锁/expected 旧 SHA**：用强制更新远端跟踪引用，例如：
  `git fetch origin +claude/chunk-random-distribution-RfcL7:refs/remotes/origin/claude/chunk-random-distribution-RfcL7`

## Git worktree 冲突

远程执行 `run_sql_test_remote.sh` 时，`git worktree add` 可能失败：**同一分支已在另一 Agent 目录检出**。

- **处理**：使用**独立分支名**（如 `meegoo/chunk-random-sql-verify`）推送后再跑远程脚本；或避免多 Agent 共用同一分支 worktree。

## 环境变量（Secrets）

- **TSP**：`TSP_HOST`、`TSP_USERNAME`、`TSP_PASSWORD`
- **远程编译机 SSH**：`SSH_HOST`、`SSH_USERNAME`、`SSH_PASSWORD`
- 远程机需能访问 TSP 集群 FE（`mysql -h<FE> -P9030` 成功）；Cloud VM 与远程机网络可能不同，以**远程机**实测为准。

## 编译（只用 build.sh，禁止裸 make）

在**远程机**上通过 Docker（与 `AGENTS.md` 一致）挂载：

- Agent worktree：`/home/disk4/hujie/cursor/agents/<AGENT_ID>/starrocks`
- 基线 `.git`：只读挂载 `${BASE_REPO}/.git`
- Maven 缓存、`TMPDIR` 等

容器内：

```bash
cd /root/src/starrocks
./build.sh --be --enable-shared-data
```

### `--clean` 策略

- **首次编译 / 大合并后 / 构建树损坏**：使用 `./build.sh --be --enable-shared-data --clean`。
- **仅小改动后的增量编译**：不加 `--clean`。
- **不要**在 `be/build_Release` 下直接 `make` 作为常规编译手段；若宿主机上 `build.sh --clean` 因目录非空失败，可在**宿主机**对 `be/build_Release`、`be/output`、`output/be` 做权限与删除后再进容器跑 `build.sh`。

### `-j` 策略

- 未特别要求时可用 `build.sh` 默认并行度；若遇 `.o file truncated` / 链接异常，可降低并行或配合 `--clean` 全量重建。

## 部署

1. 在**能连 FE** 的机器上取拓扑：`mysql -h<FE_IP> -P9030 -uroot -e 'show backends\G'` 得到 BE IP 列表。
2. 在 **worktree 根目录**（已有 `output/be/lib`、`output/be/bin`，由成功结束的 `build.sh` 生成）执行：
   - 首次：`./tools/deploy_to_cluster.sh --setup-ssh --fe-hosts <FE> --be-hosts <BE1>,<BE2>,...`
   - 更新 BE：`./tools/deploy_to_cluster.sh --be --clean-log --be-hosts <BE1>,<BE2>,...`
3. 集群节点默认用户 `sr`、密码 `sr`、安装根 `/home/disk1/sr`（与脚本默认一致）。

## SQL 测试（chunk random 用例）

- **用例路径**：`test/sql/test_random_distribution/T/test_chunk_random_distribution`
- 若仅有 **T 文件**、无对应 **R 文件**：先用 **录制模式** `-r` 跑通；有 R 后再用 `-v` 校验。
- **推荐参数**：`-a sequential -c 1`（用例带 `@sequential`）。

```bash
export SR_FE=<fe_host>:9030   # 或 tsp_quick_apply.sh --get-address <agent_id 或集群名片段>
./test/scripts/run_sql_test_remote.sh \
  -r -d sql/test_random_distribution/T/test_chunk_random_distribution \
  -a sequential -c 1 -t 600
```

`--get-address` 可用 **agent_id**（由当前 `git branch` 派生）匹配集群名后缀。

## 一键申请集群并跑默认用例（可选）

```bash
./tools/tsp_run_sql_test.sh --apply-from 7011 [run_sql_test_remote 的额外参数...]
```

自定义目录时需显式传 `-d` 等参数。

## 本次会话中的关键结论

- 部署脚本读取的是仓库根下 **`output/be/lib` 与 `output/be/bin`**；仅 `be/output/lib/starrocks_be` 而无完整 `output/be` 时说明 **`build.sh` 未全程成功**（如 `make install` / Java ext / 打包阶段失败），需修复编译后再部署。
- **录制结果**里 `compression_ratio_acceptable` 为 `0` 表示业务条件未满足；`-r` 成功只代表执行无异常，不等于断言通过。

## Git 提交规范（本仓库）

- Author：`meegoo <meegoo.sr@gmail.com>`
- Message 末尾：`Signed-off-by: meegoo <meegoo.sr@gmail.com>`
