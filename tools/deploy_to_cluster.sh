#!/usr/bin/env bash
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

# Deploy compiled StarRocks BE/FE artifacts to a remote cluster.
#
# Prerequisites:
#   - SSH key-based auth from current machine to cluster nodes (user: sr)
#   - Compiled output in ./output/be/ and/or ./output/fe/
#
# Usage:
#   ./tools/deploy_to_cluster.sh --be --be-hosts 1.2.3.4,1.2.3.5,1.2.3.6
#   ./tools/deploy_to_cluster.sh --fe --fe-hosts 1.2.3.7
#   ./tools/deploy_to_cluster.sh --all --be-hosts ... --fe-hosts ...
#   ./tools/deploy_to_cluster.sh --setup-ssh --be-hosts ... --fe-hosts ...

set -euo pipefail

root_path=/home/disk1/sr
ssh_user=sr
ssh_password=sr

update_fe=0
update_be=0
restart_be=0
restart_fe=0
clean_log=0
setup_ssh=0

JAVA_HOME_REMOTE=${JAVA_HOME_REMOTE:-/usr/lib/jvm/java-17-openjdk-amd64}

usage() {
    cat <<'EOF'
Usage: deploy_to_cluster.sh [options]

Modules:
    --be                           Deploy BE only
    --fe                           Deploy FE only
    --all                          Deploy both FE and BE

Actions:
    --restart-be | --no-restart-be Restart BE (default: restart when updating)
    --restart-fe | --no-restart-fe Restart FE (default: restart when updating)
    --clean-log  | --no-clean-log  Clean logs when restarting (default: --clean-log)
    --setup-ssh                    Only set up SSH keys to cluster nodes, then exit

Hosts:
    --be-hosts <h1,h2,...>         Comma-separated BE hosts (required)
    --fe-hosts <h1,h2,...>         Comma-separated FE hosts (required for FE ops)

Options:
    --root <path>                  Remote install root (default: /home/disk1/sr)
    --ssh-user <user>              SSH user on cluster nodes (default: sr)
    --ssh-password <pwd>           Password for ssh-copy-id (default: sr)
    --java-home <path>             JAVA_HOME on remote nodes
    -h | --help                    Show this help

Examples:
    # Set up SSH keys first
    ./tools/deploy_to_cluster.sh --setup-ssh \
        --be-hosts 10.0.0.1,10.0.0.2,10.0.0.3 --fe-hosts 10.0.0.4

    # Deploy and restart BE
    ./tools/deploy_to_cluster.sh --be --be-hosts 10.0.0.1,10.0.0.2,10.0.0.3

    # Deploy FE + BE with log cleanup
    ./tools/deploy_to_cluster.sh --all --clean-log \
        --fe-hosts 10.0.0.4 --be-hosts 10.0.0.1,10.0.0.2,10.0.0.3
EOF
}

fe_hosts=()
be_hosts=()

parse_list() {
    local list="$1"
    [ -z "$list" ] && return 0
    IFS=',' read -r -a _arr <<< "$list"
    printf '%s\n' "${_arr[@]}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --be)           update_be=1; restart_be=1 ;;
        --fe)           update_fe=1; restart_fe=1 ;;
        --all|--both)   update_fe=1; update_be=1; restart_fe=1; restart_be=1 ;;
        --restart-be)   restart_be=1 ;;
        --no-restart-be) restart_be=0 ;;
        --restart-fe)   restart_fe=1 ;;
        --no-restart-fe) restart_fe=0 ;;
        --clean-log)    clean_log=1 ;;
        --no-clean-log) clean_log=0 ;;
        --setup-ssh)    setup_ssh=1 ;;
        --root|-r)      root_path="${2:-}"; shift ;;
        --be-hosts)     be_hosts=( $(parse_list "${2:-}") ); shift ;;
        --fe-hosts)     fe_hosts=( $(parse_list "${2:-}") ); shift ;;
        --ssh-user)     ssh_user="${2:-}"; shift ;;
        --ssh-password) ssh_password="${2:-}"; shift ;;
        --java-home)    JAVA_HOME_REMOTE="${2:-}"; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

all_hosts=()
for h in "${fe_hosts[@]+"${fe_hosts[@]}"}"; do all_hosts+=("$h"); done
for h in "${be_hosts[@]+"${be_hosts[@]}"}"; do all_hosts+=("$h"); done
# Deduplicate
mapfile -t all_hosts < <(printf '%s\n' "${all_hosts[@]}" | sort -u)

# --setup-ssh: set up SSH keys and exit
if [ "$setup_ssh" -eq 1 ]; then
    if [ ${#all_hosts[@]} -eq 0 ]; then
        echo "Error: --setup-ssh requires --be-hosts and/or --fe-hosts" >&2
        exit 1
    fi
    echo "Setting up SSH keys to ${#all_hosts[@]} hosts..."
    for h in "${all_hosts[@]}"; do
        echo "  ssh-copy-id $ssh_user@$h"
        sshpass -p "$ssh_password" ssh-copy-id -f -o StrictHostKeyChecking=no "$ssh_user@$h" 2>&1 | grep -v "^$"
    done
    echo "Verifying connectivity..."
    for h in "${all_hosts[@]}"; do
        if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ssh_user@$h" 'hostname' 2>/dev/null; then
            echo "  $h: OK"
        else
            echo "  $h: FAILED" >&2
        fi
    done
    exit 0
fi

if [ "$update_be" -eq 0 ] && [ "$update_fe" -eq 0 ] && [ "$restart_be" -eq 0 ] && [ "$restart_fe" -eq 0 ]; then
    echo "Error: specify --be, --fe, or --all" >&2
    usage
    exit 1
fi

run_bg() {
    local host="$1"; shift
    [ -z "$host" ] && return 0
    ssh "$ssh_user@$host" "$*" &
}

copy_bg() {
    local src="$1" host="$2" dest="$3"
    [ -z "$host" ] && return 0
    scp -r "$src" "$ssh_user@$host:$dest" &
}

echo "=== Step 1: Stop services ==="
if [ "$restart_fe" -eq 1 ] && [ ${#fe_hosts[@]} -gt 0 ]; then
    for h in "${fe_hosts[@]}"; do
        run_bg "$h" "$root_path/fe/bin/stop_fe.sh && sleep 5"
    done
fi
if [ "$restart_be" -eq 1 ] && [ ${#be_hosts[@]} -gt 0 ]; then
    for h in "${be_hosts[@]}"; do
        run_bg "$h" "$root_path/be/bin/stop_be.sh && sleep 5"
    done
fi
wait || true

echo "=== Step 2: Remove old artifacts ==="
if [ "$update_fe" -eq 1 ] && [ ${#fe_hosts[@]} -gt 0 ]; then
    for h in "${fe_hosts[@]}"; do
        run_bg "$h" "rm -rf $root_path/fe/lib"
    done
fi
if [ "$update_be" -eq 1 ] && [ ${#be_hosts[@]} -gt 0 ]; then
    for h in "${be_hosts[@]}"; do
        run_bg "$h" "rm -rf $root_path/be/lib"
    done
fi
wait || true

echo "=== Step 3: Copy new artifacts ==="
if [ "$update_fe" -eq 1 ] && [ ${#fe_hosts[@]} -gt 0 ]; then
    for h in "${fe_hosts[@]}"; do
        copy_bg "output/fe/lib" "$h" "$root_path/fe"
    done
fi
if [ "$update_be" -eq 1 ] && [ ${#be_hosts[@]} -gt 0 ]; then
    for h in "${be_hosts[@]}"; do
        copy_bg "output/be/lib" "$h" "$root_path/be"
    done
    for h in "${be_hosts[@]}"; do
        copy_bg "output/be/bin" "$h" "$root_path/be"
    done
fi
wait || true

echo "=== Step 4: Clean logs ==="
if [ "$clean_log" -eq 1 ]; then
    if { [ "$update_fe" -eq 1 ] || [ "$restart_fe" -eq 1 ]; } && [ ${#fe_hosts[@]} -gt 0 ]; then
        for h in "${fe_hosts[@]}"; do run_bg "$h" "rm -f $root_path/fe/log/*"; done
    fi
    if { [ "$update_be" -eq 1 ] || [ "$restart_be" -eq 1 ]; } && [ ${#be_hosts[@]} -gt 0 ]; then
        for h in "${be_hosts[@]}"; do run_bg "$h" "rm -f $root_path/be/log/*"; done
    fi
    wait || true
fi

echo "=== Step 5: Start services ==="
if [ "$restart_fe" -eq 1 ] && [ ${#fe_hosts[@]} -gt 0 ]; then
    for h in "${fe_hosts[@]}"; do
        run_bg "$h" "nohup env JAVA_HOME=$JAVA_HOME_REMOTE $root_path/fe/bin/start_fe.sh --daemon </dev/null >>$root_path/fe/log/fe_start.log 2>&1 &"
    done
fi
if [ "$restart_be" -eq 1 ] && [ ${#be_hosts[@]} -gt 0 ]; then
    for h in "${be_hosts[@]}"; do
        run_bg "$h" "nohup env JAVA_HOME=$JAVA_HOME_REMOTE $root_path/be/bin/start_be.sh --daemon </dev/null >>$root_path/be/log/be_start.log 2>&1 &"
    done
fi
wait || true

echo "=== Step 6: Verify ==="
failed=0
if [ "$restart_fe" -eq 1 ] && [ ${#fe_hosts[@]} -gt 0 ]; then
    sleep 5
    for h in "${fe_hosts[@]}"; do
        if ssh "$ssh_user@$h" "test -f $root_path/fe/bin/fe.pid && kill -0 \$(cat $root_path/fe/bin/fe.pid) 2>/dev/null" 2>/dev/null; then
            pid=$(ssh "$ssh_user@$h" "cat $root_path/fe/bin/fe.pid")
            echo "  $h FE: running (pid $pid)"
        else
            echo "  $h FE: may not be running" >&2; failed=1
        fi
    done
fi
if [ "$restart_be" -eq 1 ] && [ ${#be_hosts[@]} -gt 0 ]; then
    sleep 5
    for h in "${be_hosts[@]}"; do
        if ssh "$ssh_user@$h" "test -f $root_path/be/bin/be.pid && kill -0 \$(cat $root_path/be/bin/be.pid) 2>/dev/null" 2>/dev/null; then
            pid=$(ssh "$ssh_user@$h" "cat $root_path/be/bin/be.pid")
            echo "  $h BE: running (pid $pid)"
        else
            echo "  $h BE: may not be running" >&2; failed=1
        fi
    done
fi

echo "Deploy finished. FE(update=$update_fe,restart=$restart_fe) BE(update=$update_be,restart=$restart_be)"
[ $failed -eq 1 ] && exit 1
exit 0
