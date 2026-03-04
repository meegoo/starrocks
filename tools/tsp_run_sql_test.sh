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
# 需要: TSP_USERNAME, TSP_PASSWORD, SSH_USERNAME, SSH_PASSWORD
# 用法: ./tools/tsp_run_sql_test.sh [CLUSTER_NAME] [run_sql_test_remote.sh 参数...]
#   CLUSTER_NAME: 可选，集群名（支持部分匹配）。缺省时取第一个 Running 集群
# 示例:
#   ./tools/tsp_run_sql_test.sh hujietest1-4u-benchmark-03040730 -d sql/test_stream_load/R/test_multi_statement_txn -a sequential -c 1 -v -t 600

set -e

CLUSTER_NAME=""
if [[ $# -gt 0 ]] && [[ ! "$1" =~ ^- ]]; then
    CLUSTER_NAME="$1"
    shift
fi

# 默认测试 test_multi_statement_txn（不加 -a sequential，该用例无此标签）
RUN_ARGS=("$@")
if [ ${#RUN_ARGS[@]} -eq 0 ]; then
    RUN_ARGS=(-d sql/test_stream_load/R/test_multi_statement_txn -c 1 -v -t 600)
fi

SR_FE=$(./tools/tsp_quick_apply.sh --get-address "$CLUSTER_NAME" | grep '^SR_FE=' | cut -d= -f2-)
if [ -z "$SR_FE" ]; then
    echo "错误: 无法获取集群地址" >&2
    exit 1
fi

export SR_FE
echo "使用集群: $SR_FE"
./test/scripts/run_sql_test_remote.sh "${RUN_ARGS[@]}"
