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
#            (参考 cluster/history/ 中的 hujietest1-4u-benchmark 申请新集群)
# 默认 TSP 地址: http://47.92.23.11:8001

set -e

TSP_BASE="http://47.92.23.11:8001"
APPLY_FROM_ID=""

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --apply-from)
            APPLY_FROM_ID="$2"
            shift 2
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

if [ -n "$APPLY_FROM_ID" ]; then
    # 基于历史记录申请新集群
    echo "正在获取历史记录 $APPLY_FROM_ID 的配置 ..."
    DETAIL=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/detail/?cluster_id=$APPLY_FROM_ID")
    if ! echo "$DETAIL" | grep -q '"status":\s*true'; then
        echo "错误: 无法获取历史记录 $APPLY_FROM_ID 的详情"
        echo "$DETAIL" | head -c 500
        exit 1
    fi

    # 从详情中提取 apply_infos，生成新集群名（加时间戳避免重名）
    SUFFIX=$(date +%m%d%H%M)
    BASE_NAME=$(echo "$DETAIL" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['detail']['apply_infos'].get('cluster_name','cluster'))
" 2>/dev/null || echo "cluster")
    NEW_CLUSTER_NAME="${BASE_NAME}-${SUFFIX}"
    echo "新集群名称: $NEW_CLUSTER_NAME"

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
