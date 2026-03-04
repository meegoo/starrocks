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
#
# Run SQL tests on remote machine (47.92.130.86) against cluster at SR_FE.
# Runs directly on remote host, NOT in Docker container.
# Supports all run.py parameters. Config is patched from SR_FE.
#
# Requires: SSH_USERNAME, SSH_PASSWORD, SR_FE (e.g. host or host:9030 or host:9030:8030)
#
# Usage: ./run_sql_test_remote.sh [run.py options...]
#
# run.py options (same as python3 run.py):
#   -d|--dir          Case dir (default: ./sql)
#   -r|--record       Record mode
#   -v|--validate     Validate mode (default)
#   -p|--part         Partial update, record mode only
#   -c|--concurrency  Concurrency (default: 8)
#   -t|--timeout      Timeout seconds (default: 600)
#   -l|--list         List cases only
#   -a|--attr         Attr filters, e.g. sequential,system
#   -C|--cluster      cloud|native (default: native)
#   --skip_reruns     Run each case once
#   --file_filter     File name regex
#   --case_filter     Case name regex
#   --keep_alive      Check cluster before each case
#   --run_info        Extra info
#   --arrow           Arrow protocol only
#   --check-status    Check cluster status before run
#
# Examples:
#   ./run_sql_test_remote.sh -d sql/test_parallel_compaction -C cloud -a sequential -c 1 -v -t 600
#   ./run_sql_test_remote.sh -d sql/test_optimize_table -a sequential -c 1 -v
#   ./run_sql_test_remote.sh -d sql -l

set -e

BRANCH=$(git branch --show-current)
AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
AGENT_DIR="/home/disk4/hujie/cursor/agents/${AGENT_ID}/starrocks"
BASE_REPO="/home/disk4/hujie/cursor/src/starrocks"
REMOTE_SSH="sshpass -p ${SSH_PASSWORD} ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 ${SSH_USERNAME}@47.92.130.86"

if [ -z "${SR_FE}" ]; then
    echo "Error: SR_FE environment variable is required (StarRocks FE address, e.g. host:9030 or host:9030:8030)"
    exit 1
fi

# Default run.py args when none given
if [ $# -eq 0 ]; then
    RUN_PY_ARGS=(-d sql -a sequential -c 1 -v -t 600)
else
    RUN_PY_ARGS=("$@")
fi

echo "=== Creating/updating Agent worktree on remote ==="
$REMOTE_SSH "
  cd $BASE_REPO && git fetch origin $BRANCH
  if [ ! -d $AGENT_DIR ]; then
    mkdir -p /home/disk4/hujie/cursor/agents/${AGENT_ID}
    git worktree add $AGENT_DIR $BRANCH
  else
    cd $AGENT_DIR && git checkout $BRANCH && git pull origin $BRANCH
  fi
"

echo "=== Patching config and running SQL tests on remote host ==="
IFS=: read -r SR_FE_HOST SR_FE_PORT SR_FE_HTTP_PORT _ <<< "$SR_FE"
[ -z "$SR_FE_PORT" ] && SR_FE_PORT=9030
[ -z "$SR_FE_HTTP_PORT" ] && SR_FE_HTTP_PORT=8030

# Build run.py cmd: --config conf/sr_sr_fe.conf + user args
RUN_CMD="python3 run.py --config conf/sr_sr_fe.conf ${RUN_PY_ARGS[*]}"

$REMOTE_SSH "
  cd ${AGENT_DIR}/test && \
  cp conf/sr.conf conf/sr_sr_fe.conf && \
  sed -i \"/^\\[cluster\\]/,/^\\[client\\]/{ s/^  host = .*/  host = ${SR_FE_HOST}/; s/^  port = .*/  port = ${SR_FE_PORT}/; s/^  http_port = .*/  http_port = ${SR_FE_HTTP_PORT}/; }\" conf/sr_sr_fe.conf && \
  ${RUN_CMD}
"

echo "=== SQL test finished ==="
