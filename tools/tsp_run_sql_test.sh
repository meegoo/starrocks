#!/bin/bash
# Copyright 2021-present StarRocks, Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# 从 TSP 获取集群地址并在远程机器运行 SQL 测试
# 需要: TSP_USERNAME, TSP_PASSWORD, SSH_USERNAME, SSH_PASSWORD, SSH_HOST
# 用法:
#   ./tools/tsp_run_sql_test.sh [CLUSTER_NAME] [run_sql_test_remote.sh 参数...]
#     CLUSTER_NAME: 可选，集群名（支持部分匹配）。缺省时取第一个 Running 集群
#   ./tools/tsp_run_sql_test.sh --apply-from HISTORY_ID [run_sql_test_remote.sh 参数...]
#     先申请新集群，等待部署完成后再运行测试
# 示例:
#   ./tools/tsp_run_sql_test.sh hujietest1-4u-benchmark-03040730 -d sql/test_stream_load/R/test_multi_statement_txn
#   ./tools/tsp_run_sql_test.sh --apply-from 7011

set -e

CLUSTER_NAME=""
APPLY_FROM_ID=""
if [[ "$1" = "--apply-from" ]]; then
    APPLY_FROM_ID="$2"
    shift 2
fi

if [ -n "$APPLY_FROM_ID" ]; then
    echo "=== 步骤 1/3: 申请集群 ==="
    APPLY_OUT=$(./tools/tsp_quick_apply.sh --apply-from "$APPLY_FROM_ID")
    echo "$APPLY_OUT"
    BRANCH=$(git branch --show-current 2>/dev/null || echo "unknown")
    AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
    CLUSTER_NAME=$(echo "$APPLY_OUT" | grep '^新集群名称:' | tail -1 | sed 's/^新集群名称: *//' | sed 's/ (agent_id:.*//')
    if [ -z "$CLUSTER_NAME" ]; then
        echo "错误: 无法解析新集群名称" >&2
        exit 1
    fi
    echo ""
    echo "=== 步骤 2/3: 等待集群部署完成（使用 agent_id 匹配: $AGENT_ID）==="
    ./tools/tsp_quick_apply.sh --wait-ready "$AGENT_ID" 900
    echo "集群就绪，等待 FE 服务完全启动（60s）..."
    sleep 60
    echo ""
fi

if [[ $# -gt 0 ]] && [[ ! "$1" =~ ^- ]] && [ -z "$CLUSTER_NAME" ]; then
    CLUSTER_NAME="$1"
    shift
fi

# 未指定集群时，使用 agent_id 匹配当前 Agent 申请的集群（支持部分匹配）
if [ -z "$CLUSTER_NAME" ]; then
    BRANCH=$(git branch --show-current 2>/dev/null || echo "unknown")
    AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
    CLUSTER_NAME="$AGENT_ID"
    echo "未指定集群，使用 agent_id 匹配: $AGENT_ID"
fi

# 默认测试 test_multi_statement_txn（不加 -a sequential，该用例无此标签）
RUN_ARGS=("$@")
if [ ${#RUN_ARGS[@]} -eq 0 ]; then
    RUN_ARGS=(-d sql/test_stream_load/R/test_multi_statement_txn -c 1 -v -t 600)
fi

[ -n "$APPLY_FROM_ID" ] && echo "=== 步骤 3/3: 获取地址并运行 SQL 测试 ===" || echo "=== 获取地址并运行 SQL 测试 ==="
# 使用 agent_id 匹配集群（集群名可能被 TSP 截断，agent_id 可正确匹配）
MATCH_NAME="${CLUSTER_NAME}"
if [ -n "$APPLY_FROM_ID" ] && [ -n "$AGENT_ID" ]; then
    MATCH_NAME="$AGENT_ID"
fi
SR_FE=$(./tools/tsp_quick_apply.sh --get-address "$MATCH_NAME" | grep '^SR_FE=' | cut -d= -f2-)
if [ -z "$SR_FE" ]; then
    echo "错误: 无法获取集群地址" >&2
    exit 1
fi

export SR_FE
echo "使用集群: $SR_FE"
./test/scripts/run_sql_test_remote.sh "${RUN_ARGS[@]}"
