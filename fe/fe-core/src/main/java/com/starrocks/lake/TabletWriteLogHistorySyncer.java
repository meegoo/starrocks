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

import com.starrocks.catalog.CatalogUtils;
import com.starrocks.catalog.OlapTable;
import com.starrocks.common.Config;
import com.starrocks.common.FeConstants;
import com.starrocks.common.util.FrontendDaemon;
import com.starrocks.qe.SimpleExecutor;
import com.starrocks.scheduler.history.TableKeeper;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.ast.KeysType;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.LinkedHashMap;
import java.util.Map;

public class TabletWriteLogHistorySyncer extends FrontendDaemon {
    private static final Logger LOG = LogManager.getLogger(TabletWriteLogHistorySyncer.class);

    public static final String DB_NAME = "_statistics_";
    public static final String TABLE_NAME = "tablet_write_log_history";
    // Legacy DUPLICATE table left behind during migration. We never read from it again
    // and TableKeeper's partition_live_number=7 will let any remaining data age out.
    static final String LEGACY_TABLE_NAME = "tablet_write_log_history_legacy";

    // Default retention days: 7
    private static final int RETAINED_DAYS = 7;

    // Overlap window for the sync watermark. The previous DUPLICATE-table version used
    // a strict `finish_time > MAX(finish_time)` filter, which silently dropped entries
    // that shared a second with the previous batch's max (a real race at high TPS:
    // ~80-90% loss observed at 500 ops/sec because BE-side finish_time is at second
    // precision and many entries collide per second). Switching to PRIMARY KEY mode lets
    // us safely overlap windows — duplicates are merged at insert time.
    private static final int SYNC_OVERLAP_SECONDS = 120;

    // Stop pulling rows whose finish_time is within this many seconds of NOW(). Gives
    // BE side a small buffer to flush in-flight log entries before they're queried.
    // Kept low (was 60s) because PRIMARY KEY mode no longer needs the wide cushion that
    // the old `>` watermark used as a poor man's race guard.
    private static final int SYNC_FRESHNESS_BUFFER_SECONDS = 30;

    private static final String TABLE_CREATE =
            String.format("CREATE TABLE IF NOT EXISTS %s (" +
                            // The composite primary key is what makes the new sync safe.
                            // (be_id, finish_time, txn_id, tablet_id, log_type) is unique
                            // for any single write log entry, so overlapping syncs are
                            // idempotent.
                            "be_id bigint NOT NULL, " +
                            "finish_time datetime NOT NULL, " +
                            "txn_id bigint NOT NULL, " +
                            "tablet_id bigint NOT NULL, " +
                            "log_type varchar(64) NOT NULL, " +
                            "begin_time datetime, " +
                            "table_id bigint, " +
                            "partition_id bigint, " +
                            "input_rows bigint, " +
                            "input_bytes bigint, " +
                            "output_rows bigint, " +
                            "output_bytes bigint, " +
                            "input_segments int, " +
                            "output_segments int, " +
                            "label varchar(1024), " +
                            "compaction_score bigint, " +
                            "compaction_type varchar(64), " +
                            "sst_input_files int, " +
                            "sst_input_bytes bigint, " +
                            "sst_output_files int, " +
                            "sst_output_bytes bigint" +
                            ") " +
                            "PRIMARY KEY (be_id, finish_time, txn_id, tablet_id, log_type) " +
                            "PARTITION BY date_trunc('DAY', finish_time) " +
                            "DISTRIBUTED BY HASH(tablet_id) BUCKETS 3 " +
                            "PROPERTIES( " +
                            "'partition_live_number' = '" + RETAINED_DAYS + "', " +
                            "'enable_persistent_index' = 'true'" +
                            ")",
                    TABLE_NAME);

    // The watermark uses `>=` with a SYNC_OVERLAP_SECONDS rewind. PRIMARY KEY dedup
    // makes the overlap free (rows already present are merged on insert). Two effects:
    //   1. Eliminates the second-precision race on the watermark boundary.
    //   2. Recovers from transient sync failures: the next successful sync re-scans
    //      the overlap window and fills any gap.
    //
    // INSERT names every target column explicitly so positional ordering between the
    // SELECT and the destination is never assumed. This keeps us safe if the migration
    // is still in flight (legacy DUPLICATE schema had a different column order) and
    // also when columns are added in a future version.
    //
    // The watermark COALESCEs *after* DATE_SUB, not before, so we never subtract from
    // the '0001-01-01 00:00:00' sentinel — that's the minimum representable datetime
    // and DATE_SUB would underflow on the first sync against an empty history table.
    private static final String SYNC_SQL =
            "INSERT INTO %s " +
            "(be_id, finish_time, txn_id, tablet_id, log_type, " +
            "begin_time, table_id, partition_id, " +
            "input_rows, input_bytes, output_rows, output_bytes, input_segments, output_segments, " +
            "label, compaction_score, compaction_type, " +
            "sst_input_files, sst_input_bytes, sst_output_files, sst_output_bytes) " +
            "SELECT " +
            "be_id, finish_time, txn_id, tablet_id, log_type, " +
            "begin_time, table_id, partition_id, " +
            "input_rows, input_bytes, output_rows, output_bytes, input_segments, output_segments, " +
            "label, compaction_score, compaction_type, " +
            "sst_input_files, sst_input_bytes, sst_output_files, sst_output_bytes " +
            "FROM information_schema.be_tablet_write_log " +
            "WHERE finish_time >= " +
            "    COALESCE((SELECT DATE_SUB(MAX(finish_time), INTERVAL " + SYNC_OVERLAP_SECONDS + " SECOND) " +
            "              FROM %s), " +
            "             '0001-01-01 00:00:00') " +
            "AND finish_time < NOW() - INTERVAL " + SYNC_FRESHNESS_BUFFER_SECONDS + " SECOND";

    // Columns added in newer versions that may be missing on upgraded clusters.
    // LinkedHashMap preserves insertion order for deterministic ALTER TABLE statements.
    private static final Map<String, String> EXPECTED_COLUMNS = new LinkedHashMap<>();
    static {
        EXPECTED_COLUMNS.put("sst_input_files", "int");
        EXPECTED_COLUMNS.put("sst_input_bytes", "bigint");
        EXPECTED_COLUMNS.put("sst_output_files", "int");
        EXPECTED_COLUMNS.put("sst_output_bytes", "bigint");
    }

    private boolean firstSync = true;
    // Stays false until ensureTableSchema() has observed the table in its final shape
    // (PRIMARY KEY with all expected columns present). A failed rename or ADD COLUMN
    // leaves it false so the next round retries. Setting this unconditionally — as an
    // earlier draft did — combined with the new column-reordered INSERT could write
    // data into the wrong columns of a still-DUPLICATE table.
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
            syncData();
        } catch (Throwable e) {
            LOG.warn("Failed to process one round of TabletWriteLogHistorySyncer with error message {}", e.getMessage(), e);
        }
    }

    public void syncData() {
        // Ensure the table schema is up-to-date before syncing. ensureTableSchema()
        // returns true once the table is in its final PRIMARY KEY shape with all
        // expected columns; until then we keep retrying on each round.
        if (!schemaMigrated) {
            schemaMigrated = ensureTableSchema();
        }
        try {
            SimpleExecutor.getRepoExecutor().executeDML(SQLBuilder.buildSyncSql());
        } catch (Exception e) {
            LOG.error("Failed to sync tablet write log history", e);
        }
    }

    /**
     * Brings the persistent table to the current schema shape. Returns true only when
     * the table is observed as PRIMARY KEY with every expected column present. Returns
     * false when work remains (table not yet created, mid-migration, or a DDL failed)
     * so the caller will retry on the next sync round.
     */
    private boolean ensureTableSchema() {
        try {
            OlapTable table = (OlapTable) GlobalStateMgr.getCurrentState()
                    .getLocalMetastore().mayGetTable(DB_NAME, TABLE_NAME).orElse(null);
            if (table == null) {
                // TableKeeper hasn't (re)created the table yet. Retry next round.
                return false;
            }
            if (table.getKeysType() != KeysType.PRIMARY_KEYS) {
                migrateLegacyDuplicateTable();
                // The rename (if it succeeded) leaves no current table; TableKeeper
                // will recreate it with the new PRIMARY KEY schema. Either way, this
                // round is not yet "complete" — retry next pass.
                return false;
            }
            return addMissingColumns(table);
        } catch (Exception e) {
            LOG.warn("Failed to ensure table schema for {}.{}", DB_NAME, TABLE_NAME, e);
            return false;
        }
    }

    /**
     * Rename the legacy DUPLICATE table out of the way. PRIMARY KEY tables can't be
     * reached by ADD COLUMN from a DUPLICATE schema, so we have to swap. Any stale
     * legacy table from a prior incomplete migration is dropped first so RENAME
     * doesn't get stuck on a name collision; that data was already incomplete (the
     * bug we're fixing) so an even older copy isn't worth preserving.
     */
    private void migrateLegacyDuplicateTable() {
        boolean legacyExists = GlobalStateMgr.getCurrentState()
                .getLocalMetastore().mayGetTable(DB_NAME, LEGACY_TABLE_NAME).isPresent();
        if (legacyExists) {
            String dropSql = String.format("DROP TABLE IF EXISTS %s.%s", DB_NAME, LEGACY_TABLE_NAME);
            try {
                SimpleExecutor.getRepoExecutor().executeDDL(dropSql);
                LOG.info("Dropped stale {}.{} left from a previous incomplete migration",
                        DB_NAME, LEGACY_TABLE_NAME);
            } catch (Exception e) {
                LOG.warn("Failed to drop stale {}.{} (will retry next round): {}",
                        DB_NAME, LEGACY_TABLE_NAME, e.getMessage());
                return;
            }
        }
        String renameSql = String.format("ALTER TABLE %s.%s RENAME %s",
                DB_NAME, TABLE_NAME, LEGACY_TABLE_NAME);
        LOG.info("Migrating {}.{} from DUPLICATE to PRIMARY KEY; renaming old table to {}",
                DB_NAME, TABLE_NAME, LEGACY_TABLE_NAME);
        try {
            SimpleExecutor.getRepoExecutor().executeDDL(renameSql);
        } catch (Exception e) {
            LOG.warn("Rename for migration failed (will retry next round): {}", e.getMessage());
        }
    }

    private boolean addMissingColumns(OlapTable table) {
        boolean allColumnsOk = true;
        for (Map.Entry<String, String> entry : EXPECTED_COLUMNS.entrySet()) {
            if (table.getColumn(entry.getKey()) == null) {
                String sql = String.format("ALTER TABLE %s.%s ADD COLUMN %s %s",
                        DB_NAME, TABLE_NAME, entry.getKey(), entry.getValue());
                try {
                    SimpleExecutor.getRepoExecutor().executeDDL(sql);
                    LOG.info("Added missing column {} to {}.{}", entry.getKey(), DB_NAME, TABLE_NAME);
                } catch (Exception e) {
                    LOG.warn("Failed to add column {} to {}.{}: {}", entry.getKey(), DB_NAME, TABLE_NAME,
                            e.getMessage());
                    allColumnsOk = false;
                }
            }
        }
        return allColumnsOk;
    }

    static class SQLBuilder {
        public static String buildSyncSql() {
            String tableName = CatalogUtils.normalizeTableName(DB_NAME, TABLE_NAME);
            return String.format(SYNC_SQL, tableName, tableName);
        }
    }
}
