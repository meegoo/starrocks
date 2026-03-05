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

# TSP StarRocks 集群快速申请脚本
# 使用环境变量 TSP_USERNAME 和 TSP_PASSWORD 登录
# 用法:
#   1. 打开 quick apply 页面: ./tools/tsp_quick_apply.sh [TSP_BASE_URL]
#   2. 参考历史记录申请新集群: ./tools/tsp_quick_apply.sh --apply-from HISTORY_ID [TSP_BASE_URL]
#      示例: ./tools/tsp_quick_apply.sh --apply-from 7011
#   3. 获取集群 FE 地址: ./tools/tsp_quick_apply.sh --get-address [CLUSTER_NAME]
#      输出 SR_FE=host:9030，可直接 export 后用于 run_sql_test_remote.sh
#   4. 等待集群部署完成: ./tools/tsp_quick_apply.sh --wait-ready CLUSTER_NAME [TIMEOUT_SEC]
#      轮询直到集群状态为 Running，默认超时 900 秒（15 分钟）
# TSP 地址通过环境变量 TSP_HOST 获取（必填）

set -e

if [ -z "${TSP_HOST}" ]; then
    echo "错误: 请设置环境变量 TSP_HOST（TSP 服务地址）" >&2
    exit 1
fi
TSP_BASE="${TSP_HOST}"
APPLY_FROM_ID=""
GET_ADDRESS=0
GET_ADDRESS_NAME=""
WAIT_READY=0
WAIT_READY_NAME=""
WAIT_READY_TIMEOUT=900

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --apply-from)
            APPLY_FROM_ID="$2"
            shift 2
            ;;
        --get-address)
            GET_ADDRESS=1
            GET_ADDRESS_NAME="${2:-}"
            shift 2
            ;;
        --wait-ready)
            WAIT_READY=1
            WAIT_READY_NAME="$2"
            shift 2
            if [[ "$1" =~ ^[0-9]+$ ]]; then
                WAIT_READY_TIMEOUT="$1"
                shift
            fi
            ;;
        http://*|https://*)
            TSP_BASE="$1"
            shift
            ;;
        *)
            shift
            ;;
    esac
done

COOKIE_FILE="/tmp/tsp_quick_apply_cookies_$$"

cleanup() {
    rm -f "$COOKIE_FILE"
}
trap cleanup EXIT

TSP_USERNAME="${TSP_USERNAME:-}"
# 优先使用 TSP_PASSWORD，兼容历史拼写 TSP_PASSOWRD
TSP_PASSWORD="${TSP_PASSWORD:-${TSP_PASSOWRD:-}}"

if [ -z "$TSP_USERNAME" ] || [ -z "$TSP_PASSWORD" ]; then
    echo "错误: 请设置环境变量 TSP_USERNAME 和 TSP_PASSWORD"
    echo "示例: TSP_USERNAME=your_email TSP_PASSWORD=your_pwd ./tools/tsp_quick_apply.sh --apply-from 7011"
    exit 1
fi

echo "正在登录 TSP ($TSP_BASE) ..."
CSRF=$(curl -sL -c "$COOKIE_FILE" "$TSP_BASE/cluster/login/" | grep -oP 'name="csrfmiddlewaretoken" value="\K[^"]+' || true)
if [ -z "$CSRF" ]; then
    echo "错误: 无法获取 CSRF token"
    exit 1
fi

LOGIN_RESP=$(curl -sL -b "$COOKIE_FILE" -c "$COOKIE_FILE" -X POST "$TSP_BASE/cluster/login/" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    -H "Referer: $TSP_BASE/cluster/login/" \
    -d "csrfmiddlewaretoken=${CSRF}&username=${TSP_USERNAME}&password=${TSP_PASSWORD}&keep_login=true")

if echo "$LOGIN_RESP" | grep -q '"status":\s*true'; then
    echo "登录成功"
else
    echo "登录失败: $LOGIN_RESP"
    exit 1
fi

if [ "$WAIT_READY" = "1" ]; then
    # --wait-ready: 轮询直到集群部署完成（状态为 Running）
    if [ -z "$WAIT_READY_NAME" ]; then
        echo "错误: --wait-ready 需要指定集群名称" >&2
        exit 1
    fi
    echo "等待集群 $WAIT_READY_NAME 部署完成（超时 ${WAIT_READY_TIMEOUT}s）..."
    START=$(date +%s)
    while true; do
        ELAPSED=$(($(date +%s) - START))
        if [ "$ELAPSED" -ge "$WAIT_READY_TIMEOUT" ]; then
            echo "错误: 等待超时（${WAIT_READY_TIMEOUT}s），集群可能仍在部署中" >&2
            exit 1
        fi
        LIST_HTML=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/list/")
        if echo "$LIST_HTML" | SEARCH="$WAIT_READY_NAME" python3 -c "
import re,sys,os
html=sys.stdin.read()
search=os.environ.get('SEARCH','')
pat=r'<tr>\s*<td[^>]*>\d+</td>\s*<td[^>]*name=\"cluster_name\"[^>]*>([^<]+)</td>.*?<td[^>]*name=\"fe\"[^>]*>([^<]+)</td>.*?Running'
for m in re.finditer(pat, html, re.DOTALL):
    name, fe = m.group(1).strip(), m.group(2).strip().split()[0]
    if fe and re.match(r'^\d+\.\d+\.\d+\.\d+$', fe) and search and search in name:
        sys.exit(0)
sys.exit(1)
" 2>/dev/null; then
            echo "集群已就绪: $WAIT_READY_NAME"
            exit 0
        fi
        echo "  [${ELAPSED}s] 集群尚未就绪，30s 后重试..."
        sleep 30
    done
elif [ "$GET_ADDRESS" = "1" ]; then
    # --get-address: 获取集群 FE 地址
    LIST_HTML=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/list/")
    FE_ADDR=$(GET_SEARCH="$GET_ADDRESS_NAME" python3 -c "
import re,sys,os
html=sys.stdin.read()
search=os.environ.get('GET_SEARCH','')
pat=r'<tr>\s*<td[^>]*>\d+</td>\s*<td[^>]*name=\"cluster_name\"[^>]*>([^<]+)</td>.*?<td[^>]*name=\"fe\"[^>]*>([^<]+)</td>.*?Running'
for m in re.finditer(pat, html, re.DOTALL):
    name, fe = m.group(1).strip(), m.group(2).strip().split()[0]
    if fe and re.match(r'^\d+\.\d+\.\d+\.\d+$', fe):
        if not search or search in name:
            print(fe)
            break
" 2>/dev/null <<< "$LIST_HTML")
    if [ -z "$FE_ADDR" ]; then
        FE_ADDR=$(echo "$LIST_HTML" | grep -oP '<td name="fe">\K[\d.]+' | head -1)
    fi
    if [ -n "$FE_ADDR" ]; then
        echo "SR_FE=${FE_ADDR}:9030"
    else
        echo "错误: 未找到集群地址" >&2
        exit 1
    fi
elif [ -n "$APPLY_FROM_ID" ]; then
    # 基于历史记录申请新集群
    echo "正在获取历史记录 $APPLY_FROM_ID 的配置 ..."
    DETAIL=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/detail/?cluster_id=$APPLY_FROM_ID")
    if ! echo "$DETAIL" | grep -q '"status":\s*true'; then
        echo "错误: 无法获取历史记录 $APPLY_FROM_ID 的详情"
        echo "$DETAIL" | head -c 500
        exit 1
    fi

    # 从详情中提取 apply_infos，生成新集群名（以 agent_id 为后缀，便于当前 Agent 后续直接使用）
    BRANCH=$(git branch --show-current 2>/dev/null || echo "unknown")
    AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
    BASE_NAME=$(echo "$DETAIL" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['detail']['apply_infos'].get('cluster_name','cluster'))
" 2>/dev/null || echo "cluster")
    NEW_CLUSTER_NAME="${BASE_NAME}-${AGENT_ID}"
    echo "新集群名称: $NEW_CLUSTER_NAME (agent_id: $AGENT_ID)"

    CSRF_APPLY=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/quick/apply/$APPLY_FROM_ID" | grep -oP 'name="csrfmiddlewaretoken" value="\K[^"]+' | head -1)
    [ -z "$CSRF_APPLY" ] && CSRF_APPLY=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/quick/apply/" | grep -oP 'name="csrfmiddlewaretoken" value="\K[^"]+' | head -1)

    APPLY_PAYLOAD=$(echo "$DETAIL" | python3 -c "
import json,sys,urllib.parse
d=json.load(sys.stdin)
ai=d['detail']['apply_infos'].copy()
ai['cluster_name']='${NEW_CLUSTER_NAME}'
ai['description']=ai.get('description','')[:20]+'-clone'
ai['csrfmiddlewaretoken']='${CSRF_APPLY}'
ai.pop('username',None)
print(urllib.parse.urlencode(ai))
" 2>/dev/null)

    if [ -z "$APPLY_PAYLOAD" ]; then
        echo "错误: 无法构建申请参数"
        exit 1
    fi

    echo "正在提交集群申请 ..."
    APPLY_RESP=$(curl -sL -b "$COOKIE_FILE" -c "$COOKIE_FILE" -X POST "$TSP_BASE/cluster/quick/apply/" \
        -H "Content-Type: application/x-www-form-urlencoded" \
        -H "Referer: $TSP_BASE/cluster/quick/apply/$APPLY_FROM_ID" \
        -d "$APPLY_PAYLOAD")

    if echo "$APPLY_RESP" | grep -q '"status":\s*true'; then
        MSG=$(echo "$APPLY_RESP" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('msg',''))" 2>/dev/null || true)
        echo "申请成功: $MSG"
        echo "新集群名称: $NEW_CLUSTER_NAME"
    else
        echo "申请失败: $APPLY_RESP"
        exit 1
    fi
else
    # 仅打开 quick apply 页面
    echo "正在访问 StarRocks 集群快速申请页面 ..."
    APPLY_RESP=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/quick/apply/")

    if echo "$APPLY_RESP" | grep -q "login\|Login"; then
        echo "警告: 可能被重定向到登录页，请检查凭据"
    fi

    if command -v xdg-open &>/dev/null; then
        echo "在浏览器中打开申请页面: $TSP_BASE/cluster/quick/apply/"
        xdg-open "$TSP_BASE/cluster/quick/apply/" 2>/dev/null || true
    elif command -v open &>/dev/null; then
        open "$TSP_BASE/cluster/quick/apply/" 2>/dev/null || true
    fi

    echo ""
    echo "申请页面 URL: $TSP_BASE/cluster/quick/apply/"
    echo "参考历史记录申请: ./tools/tsp_quick_apply.sh --apply-from 7011"
fi
