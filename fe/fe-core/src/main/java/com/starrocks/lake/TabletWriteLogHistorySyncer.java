// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.lake;

import com.google.common.collect.ImmutableList;
import com.starrocks.catalog.CatalogUtils;
import com.starrocks.catalog.Table;
import com.starrocks.common.Config;
import com.starrocks.common.FeConstants;
import com.starrocks.common.util.FrontendDaemon;
import com.starrocks.qe.SimpleExecutor;
import com.starrocks.scheduler.history.TableKeeper;
import com.starrocks.server.GlobalStateMgr;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.List;
import java.util.Optional;

public class TabletWriteLogHistorySyncer extends FrontendDaemon {
    private static final Logger LOG = LogManager.getLogger(TabletWriteLogHistorySyncer.class);

    public static final String DB_NAME = "_statistics_";
    public static final String TABLE_NAME = "tablet_write_log_history";

    // Default retention days: 7
    private static final int RETAINED_DAYS = 7;

    // New columns added for compaction observability enhancement.
    // Used for schema migration on upgrade: if the table already exists with the old schema,
    // these columns will be added via ALTER TABLE ADD COLUMN.
    private static final List<String> ADDED_COLUMNS = ImmutableList.of(
            "read_bytes_local bigint",
            "read_bytes_remote bigint",
            "read_time_local_ms bigint",
            "read_time_remote_ms bigint",
            "write_time_remote_ms bigint",
            "in_queue_time_ms bigint",
            "peak_memory_bytes bigint",
            "error_message varchar(1024)",
            "success boolean"
    );

    private static final String TABLE_CREATE =
            String.format("CREATE TABLE IF NOT EXISTS %s (" +
                            "be_id bigint NOT NULL, " +
                            "begin_time datetime NOT NULL, " +
                            "finish_time datetime NOT NULL, " +
                            "txn_id bigint, " +
                            "tablet_id bigint, " +
                            "table_id bigint, " +
                            "partition_id bigint, " +
                            "log_type varchar(64), " +
                            "input_rows bigint, " +
                            "input_bytes bigint, " +
                            "output_rows bigint, " +
                            "output_bytes bigint, " +
                            "input_segments int, " +
                            "output_segments int, " +
                            "label varchar(1024), " +
                            "compaction_score bigint, " +
                            "compaction_type varchar(64), " +
                            "read_bytes_local bigint, " +
                            "read_bytes_remote bigint, " +
                            "read_time_local_ms bigint, " +
                            "read_time_remote_ms bigint, " +
                            "write_time_remote_ms bigint, " +
                            "in_queue_time_ms bigint, " +
                            "peak_memory_bytes bigint, " +
                            "error_message varchar(1024), " +
                            "success boolean" +
                            ") " +
                            "PARTITION BY date_trunc('DAY', finish_time) " +
                            "DISTRIBUTED BY HASH(tablet_id) BUCKETS 3 " +
                            "PROPERTIES( " +
                            "'partition_live_number' = '" + RETAINED_DAYS + "'" +
                            ")",
                    TABLE_NAME);

    private static final String SYNC_SQL =
            "INSERT INTO %s " +
            "SELECT " +
            "be_id, begin_time, finish_time, txn_id, tablet_id, table_id, partition_id, log_type, " +
            "input_rows, input_bytes, output_rows, output_bytes, input_segments, output_segments, " +
            "label, compaction_score, compaction_type, " +
            "read_bytes_local, read_bytes_remote, read_time_local_ms, read_time_remote_ms, " +
            "write_time_remote_ms, in_queue_time_ms, peak_memory_bytes, error_message, success " +
            "FROM information_schema.be_tablet_write_log " +
            "WHERE finish_time > (SELECT COALESCE(MAX(finish_time), '0001-01-01 00:00:00') FROM %s) " +
            "AND finish_time < NOW() - INTERVAL 1 MINUTE";

    private boolean firstSync = true;
    private boolean schemaMigrated = false;

    private static final TableKeeper KEEPER =
            new TableKeeper(DB_NAME, TABLE_NAME, TABLE_CREATE, () -> RETAINED_DAYS);

    public static TableKeeper createKeeper() {
        return KEEPER;
    }

    public TabletWriteLogHistorySyncer() {
        super("TabletWriteLogHistorySyncer", Config.tablet_write_log_history_sync_interval_sec * 1000L);
    }

    @Override
    protected void runAfterCatalogReady() {
        if (FeConstants.runningUnitTest) {
            return;
        }
        try {
            // wait table keeper to create table
            if (firstSync) {
                firstSync = false;
                return;
            }
            if (!schemaMigrated) {
                migrateSchemaIfNeeded();
                schemaMigrated = true;
            }
            syncData();
        } catch (Throwable e) {
            LOG.warn("Failed to process one round of TabletWriteLogHistorySyncer with error message {}", e.getMessage(), e);
        }
    }

    /**
     * Migrate the existing tablet_write_log_history table schema by adding new columns
     * that were introduced for compaction observability enhancement.
     * This handles the upgrade scenario where the table was created with the old 17-column schema.
     */
    private void migrateSchemaIfNeeded() {
        Optional<Table> tableOpt = GlobalStateMgr.getCurrentState()
                .getLocalMetastore().mayGetTable(DB_NAME, TABLE_NAME);
        if (tableOpt.isEmpty()) {
            return;
        }
        Table table = tableOpt.get();
        String qualifiedName = CatalogUtils.normalizeTableName(DB_NAME, TABLE_NAME);
        for (String columnDef : ADDED_COLUMNS) {
            String columnName = columnDef.split(" ")[0];
            if (table.getColumn(columnName) == null) {
                String alterSql = String.format("ALTER TABLE %s ADD COLUMN %s", qualifiedName, columnDef);
                try {
                    SimpleExecutor.getRepoExecutor().executeDDL(alterSql);
                    LOG.info("Added column {} to {}", columnName, TABLE_NAME);
                } catch (Exception e) {
                    LOG.warn("Failed to add column {} to {}: {}", columnName, TABLE_NAME, e.getMessage());
                }
            }
        }
    }

    public void syncData() {
        // TODO: Can add a switch to control whether to enable synchronization
        try {
            SimpleExecutor.getRepoExecutor().executeDML(SQLBuilder.buildSyncSql());
        } catch (Exception e) {
            LOG.error("Failed to sync tablet write log history", e);
        }
    }

    static class SQLBuilder {
        public static String buildSyncSql() {
            String tableName = CatalogUtils.normalizeTableName(DB_NAME, TABLE_NAME);
            return String.format(SYNC_SQL, tableName, tableName);
        }
    }
}
