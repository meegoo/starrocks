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

import com.starrocks.catalog.Column;
import com.starrocks.catalog.OlapTable;
import com.starrocks.common.FeConstants;
import com.starrocks.qe.SimpleExecutor;
import com.starrocks.scheduler.history.TableKeeper;
import com.starrocks.server.LocalMetastore;
import com.starrocks.sql.ast.KeysType;
import com.starrocks.utframe.UtFrameUtils;
import mockit.Mock;
import mockit.MockUp;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicReference;

public class TabletWriteLogHistorySyncerTest {

    @BeforeAll
    public static void beforeAll() {
        UtFrameUtils.createMinStarRocksCluster();
        FeConstants.runningUnitTest = false;
    }

    @BeforeEach
    public void setUp() {
        FeConstants.runningUnitTest = false;
    }

    @Test
    public void testSyncDataWithSchemaMigrationTableNull() {
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                // Should not be called when table is null
                Assertions.fail("executeDDL should not be called when table is null");
            }
        };

        // Mock LocalMetastore to return null table
        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.empty();
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        syncer.syncData();

        // syncData should still execute DML even when table is null (ensureTableSchema returns early)
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataWithPrimaryKeyTableAllColumnsExist() {
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        // Already-PK table with all SST columns present. No DDL should fire.
        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.PRIMARY_KEYS;
            }

            @Mock
            public Column getColumn(String name) {
                if ("sst_input_files".equals(name) || "sst_input_bytes".equals(name) ||
                        "sst_output_files".equals(name) || "sst_output_bytes".equals(name)) {
                    return new Column();
                }
                return null;
            }
        };

        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        syncer.syncData();

        Assertions.assertEquals(0, executedDDLsRef.get().size());
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataWithPrimaryKeyTableMissingColumns() {
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        // Already-PK table missing all SST columns (older cluster upgraded post-fix).
        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.PRIMARY_KEYS;
            }

            @Mock
            public Column getColumn(String name) {
                return null;
            }
        };

        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        syncer.syncData();

        Assertions.assertEquals(4, executedDDLsRef.get().size());
        Assertions.assertTrue(executedDDLsRef.get().get(0).contains("sst_input_files"));
        Assertions.assertTrue(executedDDLsRef.get().get(1).contains("sst_input_bytes"));
        Assertions.assertTrue(executedDDLsRef.get().get(2).contains("sst_output_files"));
        Assertions.assertTrue(executedDDLsRef.get().get(3).contains("sst_output_bytes"));
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataMigratesLegacyDuplicateTable() {
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        // Legacy DUPLICATE table — should be renamed out of the way so TableKeeper
        // can recreate as PRIMARY KEY on the next pass. ADD COLUMN must NOT run.
        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.DUP_KEYS;
            }

            @Mock
            public Column getColumn(String name) {
                Assertions.fail("Should not attempt to inspect columns on legacy DUPLICATE table; "
                        + "we rename instead");
                return null;
            }
        };

        // Pristine state: the canonical table exists (DUPLICATE) but no stale legacy
        // is sitting in the way. Only one DDL — the RENAME — should fire.
        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                if (TabletWriteLogHistorySyncer.LEGACY_TABLE_NAME.equals(tableName)) {
                    return Optional.empty();
                }
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        syncer.syncData();

        Assertions.assertEquals(1, executedDDLsRef.get().size());
        String rename = executedDDLsRef.get().get(0);
        Assertions.assertTrue(rename.contains("RENAME"),
                "expected RENAME but got: " + rename);
        Assertions.assertTrue(rename.contains(TabletWriteLogHistorySyncer.LEGACY_TABLE_NAME),
                "rename should target the legacy table name; got: " + rename);
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataDropsStaleLegacyBeforeRename() {
        // Re-entrant migration: a prior attempt already renamed the table once, then
        // the cluster was downgraded (old code recreated the DUPLICATE table) and now
        // we're re-upgrading. The stale legacy from the first attempt would block the
        // RENAME, so we drop it first.
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.DUP_KEYS;
            }
        };

        // Both canonical and legacy tables exist.
        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        syncer.syncData();

        Assertions.assertEquals(2, executedDDLsRef.get().size(),
                "stale legacy must be dropped before rename; got DDLs: " + executedDDLsRef.get());
        Assertions.assertTrue(executedDDLsRef.get().get(0).contains("DROP TABLE IF EXISTS"),
                "first DDL should be DROP IF EXISTS for the stale legacy; got: "
                        + executedDDLsRef.get().get(0));
        Assertions.assertTrue(executedDDLsRef.get().get(0).contains(TabletWriteLogHistorySyncer.LEGACY_TABLE_NAME),
                "drop should target the legacy name; got: " + executedDDLsRef.get().get(0));
        Assertions.assertTrue(executedDDLsRef.get().get(1).contains("RENAME"),
                "second DDL should be the RENAME; got: " + executedDDLsRef.get().get(1));
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataRetriesMigrationWhenIncomplete() {
        // The previous draft set schemaMigrated=true unconditionally, which meant a
        // failed rename followed by a still-DUPLICATE table was never retried — and
        // worse, the new column-reordered INSERT could land in the wrong columns.
        // Verify that an unfinished migration leaves us in a retry state.
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        // Stay DUPLICATE across both calls — TableKeeper hasn't recreated yet.
        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.DUP_KEYS;
            }
        };

        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                if (TabletWriteLogHistorySyncer.LEGACY_TABLE_NAME.equals(tableName)) {
                    return Optional.empty();
                }
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();

        // First round: RENAME fires. Migration not yet complete.
        syncer.syncData();
        Assertions.assertEquals(1, executedDDLsRef.get().size());

        // Second round: because schemaMigrated stayed false, ensureTableSchema runs
        // again — and finds the table is still DUPLICATE (per the mock), so a fresh
        // RENAME attempt fires. This is the retry behavior the previous draft lacked.
        syncer.syncData();
        Assertions.assertEquals(2, executedDDLsRef.get().size(),
                "second round should retry the migration; DDLs: " + executedDDLsRef.get());
        Assertions.assertEquals(2, executedDMLsRef.get().size());
    }

    @Test
    public void testSyncDataSchemaMigrationRunsOnlyOnce() {
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.PRIMARY_KEYS;
            }

            @Mock
            public Column getColumn(String name) {
                return null;
            }
        };

        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();

        // First call - should trigger schema migration
        syncer.syncData();
        Assertions.assertEquals(4, executedDDLsRef.get().size());
        Assertions.assertEquals(1, executedDMLsRef.get().size());

        // Second call - should NOT trigger schema migration again
        syncer.syncData();
        Assertions.assertEquals(4, executedDDLsRef.get().size()); // No new DDLs
        Assertions.assertEquals(2, executedDMLsRef.get().size()); // But DML should still run
    }

    @Test
    public void testSyncDataWithDDLException() {
        AtomicReference<List<String>> executedDDLsRef = new AtomicReference<>(new ArrayList<>());
        AtomicReference<List<String>> executedDMLsRef = new AtomicReference<>(new ArrayList<>());

        new MockUp<TableKeeper>() {
            @Mock
            public boolean isReady() {
                return true;
            }
        };

        new MockUp<SimpleExecutor>() {
            @Mock
            public void executeDML(String sql) {
                executedDMLsRef.get().add(sql);
            }

            @Mock
            public void executeDDL(String sql) {
                executedDDLsRef.get().add(sql);
                throw new RuntimeException("ALTER TABLE failed");
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public KeysType getKeysType() {
                return KeysType.PRIMARY_KEYS;
            }

            @Mock
            public Column getColumn(String name) {
                return null;
            }
        };

        new MockUp<LocalMetastore>() {
            @Mock
            public Optional<com.starrocks.catalog.Table> mayGetTable(String dbName, String tableName) {
                return Optional.of(new OlapTable());
            }
        };

        TabletWriteLogHistorySyncer syncer = new TabletWriteLogHistorySyncer();
        // Should not throw even when DDL fails
        syncer.syncData();

        // DDL was attempted for all 4 columns (each fails but continues)
        Assertions.assertEquals(4, executedDDLsRef.get().size());
        // DML should still execute after DDL failure
        Assertions.assertEquals(1, executedDMLsRef.get().size());
    }

    @Test
    public void testBuildSyncSql() {
        String sql = TabletWriteLogHistorySyncer.SQLBuilder.buildSyncSql();
        Assertions.assertTrue(sql.contains("sst_input_files"));
        Assertions.assertTrue(sql.contains("sst_input_bytes"));
        Assertions.assertTrue(sql.contains("sst_output_files"));
        Assertions.assertTrue(sql.contains("sst_output_bytes"));
        Assertions.assertTrue(sql.contains("be_tablet_write_log"));
        Assertions.assertTrue(sql.contains("INSERT INTO"));
    }

    @Test
    public void testSyncSqlUsesInclusiveWatermarkWithOverlap() {
        // The whole point of the PR: the watermark must be inclusive (`>=`) and rewind
        // by an overlap window, so rows that share a second with the previous batch's
        // max(finish_time) are not silently dropped. PRIMARY KEY dedup makes the
        // overlapping re-scan safe.
        String sql = TabletWriteLogHistorySyncer.SQLBuilder.buildSyncSql();
        Assertions.assertTrue(sql.contains("finish_time >="),
                "watermark must be inclusive; old `>` causes data loss at second boundary. SQL: " + sql);
        Assertions.assertTrue(sql.contains("DATE_SUB") && sql.contains("INTERVAL"),
                "watermark must rewind by an overlap window. SQL: " + sql);
        Assertions.assertFalse(sql.contains("finish_time > ("),
                "raw `>` watermark must not be reintroduced. SQL: " + sql);
    }

    @Test
    public void testSyncSqlNamesInsertColumnsExplicitly() {
        // The destination column order differs from the legacy DUPLICATE schema's
        // column order. Without an explicit column list, INSERT INTO ... SELECT does
        // positional binding and would write data into the wrong columns whenever
        // the new SQL runs against a not-yet-migrated table — a real risk if the
        // rename DDL fails on the first round.
        String sql = TabletWriteLogHistorySyncer.SQLBuilder.buildSyncSql();
        int insertIdx = sql.indexOf("INSERT INTO");
        int selectIdx = sql.indexOf("SELECT");
        Assertions.assertTrue(insertIdx >= 0 && selectIdx > insertIdx,
                "expected INSERT followed by SELECT; SQL: " + sql);
        String betweenInsertAndSelect = sql.substring(insertIdx, selectIdx);
        Assertions.assertTrue(betweenInsertAndSelect.contains("(be_id"),
                "INSERT must name its target columns explicitly. SQL fragment: "
                        + betweenInsertAndSelect);
        Assertions.assertTrue(betweenInsertAndSelect.contains("log_type"),
                "INSERT column list must include log_type. SQL fragment: "
                        + betweenInsertAndSelect);
    }

    @Test
    public void testSyncSqlAvoidsDateSubUnderflowOnEmptyHistory() {
        // '0001-01-01 00:00:00' is the minimum datetime; DATE_SUB on it underflows.
        // The watermark must COALESCE *after* DATE_SUB so the sentinel is only used
        // when the subquery returns NULL (empty history).
        String sql = TabletWriteLogHistorySyncer.SQLBuilder.buildSyncSql();
        int dateSubIdx = sql.indexOf("DATE_SUB");
        int sentinelIdx = sql.indexOf("'0001-01-01 00:00:00'");
        Assertions.assertTrue(dateSubIdx >= 0, "SQL missing DATE_SUB. SQL: " + sql);
        Assertions.assertTrue(sentinelIdx >= 0, "SQL missing minimum-datetime sentinel. SQL: " + sql);
        Assertions.assertTrue(sentinelIdx > dateSubIdx,
                "DATE_SUB must precede the sentinel (i.e. COALESCE applied after the "
                        + "subtraction) to avoid underflow on empty history. SQL: " + sql);
    }

    @Test
    public void testCreateKeeper() {
        TableKeeper keeper = TabletWriteLogHistorySyncer.createKeeper();
        Assertions.assertNotNull(keeper);
    }
}
