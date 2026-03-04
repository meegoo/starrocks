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
# Run test_optimize_table SQL tests on remote machine against cluster at SR_FE.
# Requires: SSH_USERNAME, SSH_PASSWORD, SR_FE (e.g. host or host:9030 or host:9030:8030)
#
# Usage: ./run_sql_test_optimize_table_remote.sh

set -e

BRANCH=$(git branch --show-current)
AGENT_ID=$(echo "$BRANCH" | sed 's/[^a-zA-Z0-9]/-/g' | cut -c1-40)
CONTAINER="hj-cursor-${AGENT_ID}"
AGENT_DIR="/home/disk4/hujie/cursor/agents/${AGENT_ID}/starrocks"
BASE_REPO="/home/disk4/hujie/cursor/src/starrocks"
BASE_GIT="${BASE_REPO}/.git"
REMOTE_SSH="sshpass -p ${SSH_PASSWORD} ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 ${SSH_USERNAME}@47.92.130.86"

if [ -z "${SR_FE}" ]; then
    echo "Error: SR_FE environment variable is required (StarRocks FE address, e.g. host:9030 or host:9030:8030)"
    exit 1
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

echo "=== Starting container if not running ==="
$REMOTE_SSH "
  sudo docker rm -f $CONTAINER 2>/dev/null || true
  sudo docker run -itd \
    -v /home/disk4/hujie/m2:/root/.m2 \
    -v ${AGENT_DIR}:/root/src/starrocks \
    -v ${BASE_GIT}:${BASE_GIT}:ro \
    -v /home/disk4/hujie/tmp:/root/tmp \
    -e SR_FE=${SR_FE} \
    --oom-score-adj -300 \
    -e TMPDIR=/root/tmp \
    --name $CONTAINER \
    172.26.92.142:5000/starrocks/dev-env-ubuntu:latest
  sudo docker exec $CONTAINER bash -c 'git config --global --add safe.directory /root/src/starrocks'
"

echo "=== Installing test deps, patching config from SR_FE, running test_optimize_table ==="
# Parse SR_FE: host or host:port or host:port:http_port
IFS=: read -r SR_FE_HOST SR_FE_PORT SR_FE_HTTP_PORT _ <<< "$SR_FE"
[ -z "$SR_FE_PORT" ] && SR_FE_PORT=9030
[ -z "$SR_FE_HTTP_PORT" ] && SR_FE_HTTP_PORT=8030

$REMOTE_SSH "sudo docker exec $CONTAINER bash -c 'cd /root/src/starrocks/test && pip3 install -q -r requirements.txt 2>/dev/null; cp conf/sr.conf conf/sr_sr_fe.conf && sed -i \"/^\\[cluster\\]/,/^\\[client\\]/{ s/^  host = .*/  host = ${SR_FE_HOST}/; s/^  port = .*/  port = ${SR_FE_PORT}/; s/^  http_port = .*/  http_port = ${SR_FE_HTTP_PORT}/; }\" conf/sr_sr_fe.conf && python3 run.py --config conf/sr_sr_fe.conf -d sql/test_optimize_table -a sequential -c 1 -v -t 600'"

echo "=== test_optimize_table finished ==="
