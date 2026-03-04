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
# 使用环境变量 TSP_USERNAME 和 TSP_PASSWORD（或 TSP_PASSOWRD）登录并访问 cluster quick apply
# 用法: ./tools/tsp_quick_apply.sh [TSP_BASE_URL]
#       或 TSP_USERNAME=xxx TSP_PASSWORD=yyy ./tools/tsp_quick_apply.sh
# 默认 TSP 地址: http://47.92.23.11:8001

set -e

TSP_BASE="${1:-http://47.92.23.11:8001}"
COOKIE_FILE="/tmp/tsp_quick_apply_cookies_$$"

cleanup() {
    rm -f "$COOKIE_FILE"
}
trap cleanup EXIT

# 支持从 Secrets/环境变量读取，兼容 Cursor 等 IDE
# 兼容拼写 TSP_PASSOWRD（少一个 S）
TSP_USERNAME="${TSP_USERNAME:-}"
TSP_PASSWORD="${TSP_PASSWORD:-${TSP_PASSOWRD:-}}"

if [ -z "$TSP_USERNAME" ] || [ -z "$TSP_PASSWORD" ]; then
    echo "错误: 请设置环境变量 TSP_USERNAME 和 TSP_PASSWORD"
    echo "示例: TSP_USERNAME=your_email TSP_PASSWORD=your_pwd ./tools/tsp_quick_apply.sh"
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

echo "正在访问 StarRocks 集群快速申请页面 ..."
APPLY_RESP=$(curl -sL -b "$COOKIE_FILE" "$TSP_BASE/cluster/quick/apply/")

if echo "$APPLY_RESP" | grep -q "login\|Login"; then
    echo "警告: 可能被重定向到登录页，请检查凭据"
fi

# 在浏览器中打开（如支持）
if command -v xdg-open &>/dev/null; then
    echo "在浏览器中打开申请页面: $TSP_BASE/cluster/quick/apply/"
    xdg-open "$TSP_BASE/cluster/quick/apply/" 2>/dev/null || true
elif command -v open &>/dev/null; then
    open "$TSP_BASE/cluster/quick/apply/" 2>/dev/null || true
fi

echo ""
echo "申请页面 URL: $TSP_BASE/cluster/quick/apply/"
echo "已保存 session cookie，可直接在浏览器中访问上述 URL（若当前会话有效）"
echo ""
echo "页面内容摘要:"
echo "$APPLY_RESP" | grep -oP '<title>[^<]+</title>' || echo "(无法解析标题)"
