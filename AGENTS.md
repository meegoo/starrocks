# AGENTS.md - StarRocks

> This file provides context for AI coding agents working with the StarRocks codebase.
> Compatible with: Claude Code, OpenCode, Cursor, Gemini CLI, Windsurf, Aider, Continue, Cline, and other MCP-compatible tools.

## Project Overview

StarRocks is a high-performance, real-time analytical database. It delivers sub-second query latency for complex analytics workloads through its MPP (Massively Parallel Processing) architecture.

**Key capabilities:**
- Real-time analytics with sub-second latency
- High-concurrency, low-latency queries
- Support for both shared-nothing and shared-data deployments
- Materialized views for query acceleration
- External data source federation (Hive, Iceberg, Hudi, Delta Lake, JDBC, etc.)

## Architecture

StarRocks uses a decoupled frontend-backend architecture:

```
┌─────────────────────────────────────────────────────────────┐
│                      Frontend (FE)                          │
│  Java │ SQL Parsing │ Query Planning │ Metadata Management  │
└────────────────────────────┬────────────────────────────────┘
                             │ Thrift RPC
┌────────────────────────────▼────────────────────────────────┐
│                      Backend (BE)                           │
│  C++ │ Query Execution │ Storage Engine │ Data Processing   │
└─────────────────────────────────────────────────────────────┘
```

- **FE (Frontend)**: Java-based. Handles SQL parsing, query optimization, metadata management, and cluster coordination.
- **BE (Backend)**: C++-based. Handles query execution, storage management, and data processing.

## Directory Structure

```
starrocks/
├── be/                    # Backend (C++) - Query execution & storage
├── fe/                    # Frontend (Java) - SQL & metadata
│   ├── fe-core/          # Core FE logic
│   ├── fe-parser/        # SQL parser
│   ├── fe-grammar/       # ANTLR grammar files
│   ├── fe-type/          # Type system
│   ├── fe-spi/           # Service Provider Interfaces
│   ├── connector/        # External data source connectors
│   └── plugin/           # FE plugins
├── java-extensions/       # JNI connectors for external sources
├── gensrc/               # Generated code (Thrift, Protobuf)
├── test/                 # SQL integration tests
├── docs/                 # Documentation (Docusaurus)
├── thirdparty/           # Third-party dependencies
└── docker/               # Docker build files
```

### Code Formatting

```bash
# Format C++ code (BE)
clang-format -i <file.cpp>

# Check Java code style (FE)
cd fe && mvn checkstyle:check
```

## Code Style Summary

### C++ (Backend)
- **Style**: Google C++ Style (with modifications)
- **Config**: `.clang-format` at project root
- **Indent**: 4 spaces
- **Line limit**: 120 characters
- **Pointer alignment**: Left (`int* ptr`)

### Java (Frontend)
- **Style**: Google Java Style (with modifications)
- **Config**: `fe/checkstyle.xml`
- **Indent**: 4 spaces
- **Line limit**: 130 characters
- **Import order**: Third-party, then Java standard, then static

### Protobuf
- Message names: `PascalCasePB` (e.g., `MyMessagePB`)
- Field names: `snake_case`
- **Never** use `required` fields
- **Never** change field ordinals

### Thrift
- Struct names: `TPascalCase` (e.g., `TMyStruct`)
- Field names: `snake_case`
- **Never** use `required` fields
- **Never** change field ordinals

## Documentation Sync Requirements

### Configuration Changes

**When you add, modify, or remove configuration parameters, you MUST update the corresponding documentation:**

| Component | Documentation File |
|-----------|-------------------|
| FE Config | `docs/en/administration/management/FE_configuration.md` |
| BE Config | `docs/en/administration/management/BE_configuration.md` |

**Required documentation for each config parameter:**
- Parameter name
- Default value
- Value range (if applicable)
- Description of what it controls
- When to use/modify it
- Whether it requires restart

### Metrics Changes

**When you add, modify, or remove metrics, you MUST update the corresponding documentation:**

| Metrics Type | Documentation File |
|--------------|-------------------|
| General Metrics | `docs/en/administration/management/monitoring/metrics.md` |
| Shared-Data Metrics | `docs/en/administration/management/monitoring/metrics-shared-data.md` |
| MV Metrics | `docs/en/administration/management/monitoring/metrics-materialized_view.md` |
| BE Metrics (SQL) | `docs/en/sql-reference/information_schema/be_metrics.md` |
| FE Metrics (SQL) | `docs/en/sql-reference/information_schema/fe_metrics.md` |

**Required documentation for each metric:**
- Metric name
- Type (Counter, Gauge, Histogram)
- Labels (if any)
- Description of what it measures
- Unit (if applicable)

### PR Checklist for Config/Metrics Changes

When your PR includes configuration or metrics changes:

- [ ] Updated corresponding documentation in `docs/en/`
- [ ] Updated Chinese documentation in `docs/zh/` (if exists)
- [ ] Documented default values and valid ranges
- [ ] Added deprecation notice if replacing old config/metric

## Commit & PR Guidelines

### Commit Messages
- Write in English
- Start with a verb in imperative mood (e.g., "Fix", "Add", "Update")
- Be concise but descriptive
- Example: `Fix null pointer exception in tablet compaction`

### PR Title Format

```
[Type] Brief description
```

**Types:**
| Type | Usage |
|------|-------|
| `[BugFix]` | Bug fixes |
| `[Feature]` | New features |
| `[Enhancement]` | Improvements to existing features |
| `[Refactor]` | Code refactoring (no behavior change) |
| `[UT]` | Unit test additions/fixes |
| `[Doc]` | Documentation changes |
| `[Tool]` | Tooling/build changes |

**Examples:**
- `[BugFix] Fix memory leak in hash join operator`
- `[Feature] Add support for ARRAY_AGG function`
- `[Enhancement] Improve partition pruning performance`

### PR Body Template

Every PR must follow this template:

```markdown
## Why I'm doing:
<!-- Explain the motivation and context -->
Describe why this change is needed.

## What I'm doing:
<!-- Describe what changes you made -->
Explain what this PR does.

Fixes #issue_number

## What type of PR is this:
- [ ] BugFix
- [ ] Feature
- [ ] Enhancement
- [ ] Refactor
- [ ] UT
- [ ] Doc
- [ ] Tool

## Does this PR entail a change in behavior?
- [ ] Yes, this PR will result in a change in behavior.
- [ ] No, this PR will not result in a change in behavior.

## If yes, please specify the type of change:
- [ ] Interface/UI changes: syntax, type conversion, expression evaluation, display information
- [ ] Parameter changes: default values, similar parameters but with different default values
- [ ] Policy changes: use new policy to replace old one, functionality automatically enabled
- [ ] Feature removed
- [ ] Miscellaneous: upgrade & downgrade compatibility, etc.

## Checklist:
- [ ] I have added test cases for my bug fix or my new feature
- [ ] This PR needs user documentation (for new or modified features or behaviors)
  - [ ] I have added documentation for my new feature or new function
- [ ] This is a backport PR

## Bugfix cherry-pick branch check:
- [ ] I have checked the version labels which the PR will be auto-backported to the target branch
  - [ ] 4.1
  - [ ] 4.0
  - [ ] 3.5
  - [ ] 3.4
```

### PR Requirements Checklist

1. **One Commit per PR**: Squash multiple commits before merging
2. **Link Issue**: Reference related issue with `Fixes #issue_number`
3. **Tests Required**:
   - Bug fixes must include regression tests
   - New features must include unit tests
4. **Documentation**: Update docs for user-facing changes
5. **Fill Template**: Complete all sections of PR template
6. **CI Must Pass**: All automated checks must be green

### Behavior Change Classification

If your PR changes behavior, classify the change type:

| Change Type | Examples |
|-------------|----------|
| **Interface/UI** | New SQL syntax, changed output format, type conversion changes |
| **Parameter** | Changed default values, new config parameters |
| **Policy** | Auto-enabled features, changed default policies |
| **Feature removed** | Deprecated functionality removed |
| **Compatibility** | Upgrade/downgrade impacts |

### Backport Guidelines

For bug fixes that need to be backported to release branches:

1. Add version labels (e.g., `4.1`, `4.0`, `3.5`) to your PR
2. The CI will auto-create backport PRs after merge
3. Verify backport PRs are created and merged

### Review Process

1. **Minimum 2 Approvals**: At least 2 committers must approve
2. **CI Checks**: All automated checks must pass:
   - Code style (checkstyle, clang-format)
   - Unit tests (FE and BE)
   - Build verification
3. **Address Feedback**: Respond to all review comments
4. **CLA Required**: Sign CLA once: https://cla-assistant.io/StarRocks/starrocks

### CI Pipeline

Your PR will trigger these checks:

| Check | Description |
|-------|-------------|
| `PR CHECKER` | Basic PR validation |
| `FE UT` | Frontend unit tests |
| `BE UT` | Backend unit tests |
| `Build` | Full build verification |
| `Checkstyle` | Java code style |
| `Clang-format` | C++ code style |

### Common PR Issues

| Issue | Solution |
|-------|----------|
| CI timeout | Re-run failed jobs; check for flaky tests |
| Checkstyle failure | 在远程编译容器内执行 checkstyle 检查，参见 [Cursor Cloud specific instructions](#cursor-cloud-specific-instructions) |
| Build failure | 在远程编译容器内执行构建，参见 [Cursor Cloud specific instructions](#cursor-cloud-specific-instructions) |
| Merge conflicts | Rebase on latest main branch |

## Testing Guidelines

> **强制要求**：在 Cursor Cloud 环境下，所有单元测试必须在远程编译服务器的 Docker 容器内执行，参见 [Cursor Cloud specific instructions](#cursor-cloud-specific-instructions)。

### Unit Tests
- **BE**: Use Google Test framework. Tests in `be/test/` mirror source structure.
- **FE**: Use JUnit 5. Tests in `fe/fe-core/src/test/java/`.

### SQL Integration Tests
- Use the SQL-tester framework in `test/`
- See `test/README.md` for detailed documentation

**SQL 测试标准流程（Cursor Cloud）**：必须通过 TSP 申请 StarRocks 集群，然后在远程机器上运行。详见 [TSP 集群申请与 SQL 测试完整流程](#tsp-集群申请与-sql-测试完整流程)。

### Test Requirements
- All new features must have corresponding tests
- Bug fixes should include regression tests
- Maintain or improve test coverage

## Performance Considerations

### Backend (C++)
- Use SIMD instructions where applicable (AVX2/AVX512)
- Prefer vectorized processing over row-by-row
- Use `Column` and `Chunk` abstractions for data processing
- Be mindful of memory allocations in hot paths
- Profile with `perf` or async-profiler before optimization

### Frontend (Java)
- Avoid creating unnecessary objects in hot paths
- Use appropriate data structures for the use case
- Be cautious with synchronized blocks

## Security Guidelines

- Validate all user inputs at system boundaries
- Never log sensitive information (passwords, tokens, PII)
- Follow existing authentication/authorization patterns
- Use parameterized queries to prevent SQL injection

## Nested AGENTS.md Files

For module-specific guidelines, refer to:
- `be/AGENTS.md` - Backend C++ development
- `fe/AGENTS.md` - Frontend Java development
- `java-extensions/AGENTS.md` - JNI connector development
- `gensrc/AGENTS.md` - Generated code handling
- `test/AGENTS.md` - SQL integration testing
- `docs/AGENTS.md` - Documentation contribution

## Useful Resources

- [Official Documentation](https://docs.starrocks.io/)
- [Contributing Guide](./CONTRIBUTING.md)
- [GitHub Issues](https://github.com/StarRocks/starrocks/issues)
- [Slack Community](https://try.starrocks.com/join-starrocks-on-slack)

## License

StarRocks is licensed under the Apache License 2.0.

All source files must include the appropriate license header:
```
// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...
```

## Cursor Cloud specific instructions

### ⚠️ 强制要求：编译、单测与 SQL 测试的执行方式

- **编译与单测**：必须在远程编译服务器（`SSH_HOST`）的 Docker 容器内执行，禁止在本地 Cloud VM 中运行。编译：`./build.sh --fe` / `./build.sh --be --enable-shared-data`；单测：`./run-fe-ut.sh` / `./run-be-ut.sh`；Checkstyle：`mvn checkstyle:check`。
- **SQL 测试**：必须通过 TSP 申请 StarRocks 集群，然后在远程机器上运行（详见 [TSP 集群申请与 SQL 测试完整流程](#tsp-集群申请与-sql-测试完整流程)）。

具体操作步骤见下方 [Remote Build Server & Multi-Agent Isolation](#remote-build-server--multi-agent-isolation)。

### Remote Build Server & Multi-Agent Isolation

代码修改在 Cloud VM 中完成，**编译和单测必须在远程编译服务器（`SSH_HOST`）的 Docker 容器内执行**。每个 Agent 使用独立的工作目录和容器，避免多个 Agent 同时执行时互相干扰。

**隔离机制**：
- 基线 repo：`/home/disk4/hujie/cursor/src/starrocks`（始终保持在 main，仅用于 fetch 和共享 .git 对象）
- Agent 工作目录：`/home/disk4/hujie/cursor/agents/<AGENT_ID>/starrocks`（通过 `git worktree` 创建，共享 .git 对象）
- Agent 容器：`hj-cursor-<AGENT_ID>`（每个 Agent 专属，挂载各自工作目录）
- AGENT_ID 由分支名派生：`echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40`

#### 辅助函数

在执行以下步骤前，需设置 `SSH_HOST`、`SSH_USERNAME`、`SSH_PASSWORD`，并定义 Agent 标识：

```bash
BRANCH=$(git branch --show-current)
AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
CONTAINER="hj-cursor-${AGENT_ID}"
AGENT_DIR="/home/disk4/hujie/cursor/agents/${AGENT_ID}/starrocks"
BASE_REPO="/home/disk4/hujie/cursor/src/starrocks"
BASE_GIT="${BASE_REPO}/.git"
REMOTE_SSH="sshpass -p \$SSH_PASSWORD ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 \${SSH_USERNAME}@\${SSH_HOST}"
```

#### 1. 本地修改 & Push

**1a. 本地提交并推送**（用户每次修改后）：

```bash
git add . && git commit -m "your message" && git push
```

**1b. 当用户要求 push 时**：在远程机器的 Agent 工作目录内，将**当前分支相对于 origin/dev 的所有 commit** 合并为一个，并使用 `-s`（signoff）和远程 git 用户作为 author。此步骤仅在用户明确要求 push 时执行，不作为 step 2 的一部分。

```bash
$REMOTE_SSH "
  cd $BASE_REPO && git fetch origin $BRANCH dev
  cd $AGENT_DIR && git checkout $BRANCH && git pull origin $BRANCH
  BASE=\$(git merge-base origin/dev HEAD)
  MSG=\$(git log -1 --pretty=%B)
  git reset --soft \$BASE
  git commit -s -m \"\$MSG\" --author=\"\$(git config user.name) <\$(git config user.email)>\"
  git push --force origin $BRANCH
"
```

**前置条件**：远程主机上需已配置 `git config user.name` 和 `git config user.email`。若 Agent 工作目录尚不存在，需先执行 step 2。

#### 2. 创建/更新 Agent 工作目录（宿主机 git worktree）

在远程主机的基线 repo 中 fetch，然后为当前 Agent 创建或更新独立的 worktree：

```bash
$REMOTE_SSH "
  cd $BASE_REPO && git fetch origin $BRANCH
  if [ ! -d $AGENT_DIR ]; then
    mkdir -p /home/disk4/hujie/cursor/agents/${AGENT_ID}
    git worktree add $AGENT_DIR $BRANCH
  else
    cd $AGENT_DIR && git checkout $BRANCH && git pull origin $BRANCH
  fi"
```

#### 3. 执行编译或单测（容器即用即删，避免残留）

不预先启动常驻容器，而是每次执行命令时直接 `docker run --rm`：启动容器、执行命令、结束后自动删除容器，避免容器残留。

**通用容器启动参数**（需先执行上方辅助函数以定义 `AGENT_DIR`、`BASE_GIT` 等）：

```bash
DOCKER_RUN="sudo docker run --rm \
  -v /home/disk4/hujie/m2:/root/.m2 \
  -v ${AGENT_DIR}:/root/src/starrocks \
  -v ${BASE_GIT}:${BASE_GIT}:ro \
  -v /home/disk4/hujie/tmp:/root/tmp \
  -e TMPDIR=/root/tmp \
  --oom-score-adj -300 \
  172.26.92.142:5000/starrocks/dev-env-ubuntu:latest"
```

**关键**：必须额外挂载 `${BASE_GIT}:${BASE_GIT}:ro`，因为 worktree 的 `.git` 文件指向基线 repo 的 `.git/worktrees/` 目录。

**编译**：

```bash
# FE 编译
$REMOTE_SSH "$DOCKER_RUN bash -c 'git config --global --add safe.directory /root/src/starrocks 2>/dev/null; cd /root/src/starrocks && ./build.sh --fe'"

# BE 编译（默认开启 shared-data 模式）
$REMOTE_SSH "$DOCKER_RUN bash -c 'git config --global --add safe.directory /root/src/starrocks 2>/dev/null; cd /root/src/starrocks && ./build.sh --be --enable-shared-data'"
```

**单测**：

```bash
# FE 单测
$REMOTE_SSH "$DOCKER_RUN bash -c 'git config --global --add safe.directory /root/src/starrocks 2>/dev/null; cd /root/src/starrocks && ./run-fe-ut.sh --test com.starrocks.sql.plan.TPCHPlanTest'"

# BE 单测
$REMOTE_SSH "$DOCKER_RUN bash -c 'git config --global --add safe.directory /root/src/starrocks 2>/dev/null; cd /root/src/starrocks && ./run-be-ut.sh --test CompactionUtilsTest'"
```

**Checkstyle**（Java 代码风格检查）：

```bash
$REMOTE_SSH "$DOCKER_RUN bash -c 'git config --global --add safe.directory /root/src/starrocks 2>/dev/null; cd /root/src/starrocks/fe && mvn checkstyle:check'"
```

#### 4. 清理（可选）

Agent 完成后可清理 worktree（容器已自动删除，无需清理）：

```bash
$REMOTE_SSH "cd $BASE_REPO && git worktree remove $AGENT_DIR --force 2>/dev/null || true"
```

### TSP 集群申请与 SQL 测试完整流程

**SQL 测试的标准流程**：通过 TSP 申请 StarRocks 集群 → 在远程机器上运行 SQL 测试。SQL 测试需要连接真实 StarRocks 集群，本地通常无可用集群，因此必须按此流程执行。

**环境变量**：

| 变量 | 说明 |
|------|------|
| `TSP_HOST` | TSP 地址（必填） |
| `TSP_USERNAME` | TSP 登录账号 |
| `TSP_PASSWORD` | TSP 登录密码 |
| `SSH_HOST` | 远程机器地址（必填） |
| `SSH_USERNAME` | 远程机器 SSH 用户名 |
| `SSH_PASSWORD` | 远程机器 SSH 密码 |

#### 步骤 1：申请新集群

参考 `cluster/history/` 中已有记录克隆配置并申请：

```bash
./tools/tsp_quick_apply.sh --apply-from 7011
```

示例中 7011 对应 `hujietest1-4u-benchmark`。脚本会生成新集群名，**后缀为当前 Agent 的 agent_id**（由分支名派生），便于同一 Agent 后续直接通过 agent_id 匹配使用该集群。

#### 步骤 2：等待集群部署完成（必须）

集群申请后需要一定时间部署，**必须等待状态为 Running 后再执行 SQL 测试**：

```bash
./tools/tsp_quick_apply.sh --wait-ready hujietest1-4u-benchmark-03040730 900
```

默认超时 900 秒（15 分钟），可省略最后一个参数使用默认值。

#### 步骤 3：获取集群 FE 地址

```bash
./tools/tsp_quick_apply.sh --get-address hujietest1-4u-benchmark-03040730
```

输出示例：`SR_FE=<fe_host>:9030`。不指定集群名时取第一个 Running 集群。

#### 步骤 4：推送当前分支（前置条件）

远程脚本会拉取当前分支，因此需先推送：

```bash
git push origin $(git branch --show-current)
```

#### 步骤 5：在远程机器上运行 SQL 测试

SQL 测试在远程主机（`SSH_HOST`）上**直接执行**（不使用 Docker 容器）：

```bash
export SR_FE="<fe_host>:9030"   # 从步骤 2 获取
export SSH_HOST="..."
export SSH_USERNAME="..."
export SSH_PASSWORD="..."

# 运行指定测试（例如 test_multi_statement_txn）
./test/scripts/run_sql_test_remote.sh -d sql/test_stream_load/R/test_multi_statement_txn -c 1 -v -t 600
```

`run_sql_test_remote.sh` 会：拉取当前分支、在远程主机上更新 worktree、按 `SR_FE` 修改 `test/conf/sr.conf`、在远程直接执行 `python3 run.py`。

#### 一键流程

**方式 A：已有 Running 集群**。若集群名以 agent_id 为后缀（本 Agent 申请），可直接运行（自动按 agent_id 匹配）：

```bash
./tools/tsp_run_sql_test.sh
```

或显式指定集群名/部分匹配：

```bash
./tools/tsp_run_sql_test.sh hujietest1-4u-benchmark-03040730
```

**方式 B：从申请到测试一气呵成**（自动完成申请 → 等待部署 → 运行测试）：

```bash
./tools/tsp_run_sql_test.sh --apply-from 7011
```

脚本会轮询直到集群状态为 Running 后再执行 SQL 测试。

指定其他测试：

```bash
./tools/tsp_run_sql_test.sh 集群名 -d sql/test_xxx -c 1 -v -t 600
./tools/tsp_run_sql_test.sh --apply-from 7011 -d sql/test_xxx -c 1 -v -t 600
```

**相关脚本**：

| 脚本 | 功能 |
|------|------|
| `tools/tsp_quick_apply.sh` | 登录 TSP、申请集群（`--apply-from`）、等待就绪（`--wait-ready`）、获取 FE 地址（`--get-address`） |
| `tools/tsp_run_sql_test.sh` | 获取集群地址并调用远程 SQL 测试；支持 `--apply-from` 自动申请并等待就绪 |
| `test/scripts/run_sql_test_remote.sh` | 在远程主机执行 SQL 测试（支持 run.py 全部参数） |

### Key Details

- **SSH 凭据**：`SSH_HOST`、`SSH_USERNAME` 和 `SSH_PASSWORD` 来自环境变量 Secrets，需安装 `sshpass`。
- **容器镜像**：`172.26.92.142:5000/starrocks/dev-env-ubuntu:latest`。
- **容器即用即删**：使用 `docker run --rm`，命令结束后容器自动删除，无残留。
- **容器 git safe.directory**：每次启动容器时执行 `git config --global --add safe.directory /root/src/starrocks`。
- **Maven 缓存**：`/home/disk4/hujie/m2:/root/.m2`，跨 Agent 共享（Maven 支持并发读取）。
- **基线 repo 应始终保持在 main**：不要在基线 repo 上 checkout 其他分支，否则会与 worktree 冲突。
- **长时间 SSH 命令**：使用 `-o ServerAliveInterval=30` 防止超时断开。
- **git worktree + Docker 注意事项**：worktree 目录中的 `.git` 是一个指向基线 `.git/worktrees/` 的文件，因此容器启动时必须同时挂载基线 `.git` 目录（以只读模式 `:ro`）。

