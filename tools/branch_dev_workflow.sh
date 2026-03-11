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

# 非 dev 分支的迁移工作流脚本
#
# 当需要修改的分支不是基于 dev 时，先创建基于 dev 的新分支，cherry-pick 原分支的提交到新分支，
# 在新分支完成开发后，再将新分支的修改 cherry-pick 回原分支。
#
# 用法:
#   --start TARGET_BRANCH    开始工作流：检查 TARGET_BRANCH 是否基于 dev，若非则创建新分支并 cherry-pick
#                            新分支名为 ${TARGET_BRANCH}-dev-base，切换到新分支供开发
#   --finish TARGET_BRANCH   完成工作流：将新分支上的新提交 cherry-pick 回 TARGET_BRANCH
#   --check TARGET_BRANCH    仅检查 TARGET_BRANCH 是否基于 dev，不执行任何操作
#
# 示例:
#   ./tools/branch_dev_workflow.sh --start my-feature-branch
#   # 若 my-feature-branch 非基于 dev，会创建 my-feature-branch-dev-base 并切换过去
#   # 在 my-feature-branch-dev-base 上完成开发并提交
#   ./tools/branch_dev_workflow.sh --finish my-feature-branch

set -e

BASE_BRANCH="${BASE_BRANCH:-origin/dev}"

usage() {
    echo "用法: $0 --start|--finish|--check TARGET_BRANCH" >&2
    echo "  --start TARGET_BRANCH  开始工作流：若分支非基于 dev，创建新分支并 cherry-pick" >&2
    echo "  --finish TARGET_BRANCH 完成工作流：将新分支的修改 cherry-pick 回原分支" >&2
    echo "  --check TARGET_BRANCH  检查分支是否基于 dev" >&2
    echo "" >&2
    echo "环境变量 BASE_BRANCH 可覆盖基准分支（默认 origin/dev）" >&2
    exit 1
}

# 检查 target 是否基于 BASE_BRANCH（即 BASE_BRANCH 是否为 target 的祖先）
check_based_on_dev() {
    local target="$1"
    git fetch origin "${target}" "${BASE_BRANCH#origin/}" 2>/dev/null || true
    git merge-base --is-ancestor "${BASE_BRANCH}" "${target}" 2>/dev/null
}

get_dev_base_branch_name() {
    local target="$1"
    local short_name="${target#origin/}"
    echo "${short_name}-dev-base"
}

# 解析分支引用：若本地存在则用本地，否则尝试 origin/
resolve_branch() {
    local name="$1"
    if git rev-parse "${name}" >/dev/null 2>&1; then
        echo "${name}"
        return
    fi
    local origin_ref="origin/${name}"
    if git rev-parse "${origin_ref}" >/dev/null 2>&1; then
        echo "${origin_ref}"
        return
    fi
    echo ""
}

# 获取用于 checkout 的本地分支名（若只有远程则创建本地跟踪分支）
branch_for_checkout() {
    local name="$1"
    local resolved
    resolved=$(resolve_branch "$name")
    [ -n "$resolved" ] || { echo ""; return; }
    if [[ "$resolved" == origin/* ]]; then
        echo "${resolved#origin/}"
    else
        echo "$resolved"
    fi
}

do_start() {
    local target
    target=$(resolve_branch "$1")
    [ -n "$target" ] || { echo "错误: 分支 $1 不存在，请先 fetch 或创建" >&2; exit 1; }

    if check_based_on_dev "$target"; then
        local checkout_ref
        checkout_ref=$(branch_for_checkout "$1")
        echo "分支 ${target} 已基于 ${BASE_BRANCH}，无需迁移，直接切换到该分支即可"
        git checkout "${checkout_ref}"
        git pull origin "${checkout_ref}" 2>/dev/null || true
        return 0
    fi

    local dev_branch
    dev_branch=$(get_dev_base_branch_name "$target")
    echo "分支 ${target} 非基于 ${BASE_BRANCH}，创建新分支 ${dev_branch} 并 cherry-pick 原分支提交"

    # 创建基于 dev 的新分支
    git fetch origin "${BASE_BRANCH#origin/}" 2>/dev/null || true
    git checkout -B "${dev_branch}" "${BASE_BRANCH}"

    # 获取原分支相对于 dev 的提交（从旧到新，便于 cherry-pick）
    local commits
    commits=$(git rev-list --reverse "${BASE_BRANCH}..${target}")
    if [ -z "$commits" ]; then
        echo "原分支无额外提交，新分支已就绪"
    else
        echo "Cherry-pick 原分支的提交到 ${dev_branch} ..."
        while IFS= read -r c; do
            if ! git cherry-pick "$c" 2>/dev/null; then
                echo "Cherry-pick 冲突，请手动解决后执行: git cherry-pick --continue" >&2
                exit 1
            fi
        done <<< "$commits"
        echo "Cherry-pick 完成"
    fi

    echo ""
    echo "已切换到 ${dev_branch}，请在此分支完成开发。完成后执行:"
    echo "  $0 --finish $1"
}

do_finish() {
    local target
    target=$(resolve_branch "$1")
    [ -n "$target" ] || { echo "错误: 分支 $1 不存在" >&2; exit 1; }
    local dev_branch
    dev_branch=$(get_dev_base_branch_name "$target")

    if ! git rev-parse "${dev_branch}" >/dev/null 2>&1; then
        echo "错误: 新分支 ${dev_branch} 不存在" >&2
        exit 1
    fi

    # 获取新分支上相对于原分支的提交（开发期间新增的）
    local commits
    commits=$(git rev-list --reverse "${target}..${dev_branch}")
    if [ -z "$commits" ]; then
        echo "新分支无新增提交，无需 cherry-pick 回原分支"
        return 0
    fi

    local checkout_ref
    checkout_ref=$(branch_for_checkout "$1")
    echo "将 ${dev_branch} 上的新提交 cherry-pick 回 ${target} ..."
    git checkout "${checkout_ref}"
    git pull origin "${checkout_ref}" 2>/dev/null || true

    while IFS= read -r c; do
        if ! git cherry-pick "$c" 2>/dev/null; then
            echo "Cherry-pick 冲突，请手动解决后执行: git cherry-pick --continue" >&2
            exit 1
        fi
    done <<< "$commits"

    echo "Cherry-pick 完成，修改已同步回 ${target}"
}

do_check() {
    local target
    target=$(resolve_branch "$1")
    [ -n "$target" ] || { echo "错误: 分支 $1 不存在" >&2; exit 1; }
    if check_based_on_dev "$target"; then
        echo "分支 ${target} 已基于 ${BASE_BRANCH}"
    else
        echo "分支 ${target} 非基于 ${BASE_BRANCH}，建议使用 --start 开始迁移工作流"
    fi
}

# 解析参数
MODE=""
TARGET=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --start)
            MODE="start"
            TARGET="${2:?请指定目标分支}"
            shift 2
            ;;
        --finish)
            MODE="finish"
            TARGET="${2:?请指定目标分支}"
            shift 2
            ;;
        --check)
            MODE="check"
            TARGET="${2:?请指定目标分支}"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -n "$MODE" ] && [ -n "$TARGET" ] || usage

case "$MODE" in
    start)  do_start "$TARGET" ;;
    finish) do_finish "$TARGET" ;;
    check)  do_check "$TARGET" ;;
esac
