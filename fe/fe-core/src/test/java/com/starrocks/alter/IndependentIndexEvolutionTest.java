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

package com.starrocks.alter;

import com.starrocks.catalog.Database;
import com.starrocks.catalog.Index;
import com.starrocks.catalog.OlapTable;
import com.starrocks.common.Config;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.ast.IndexDef;
import com.starrocks.sql.ast.IndexDef.IndexType;
import com.starrocks.utframe.TestWithFeService;
import com.starrocks.utframe.UtFrameUtils;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.List;

public class IndependentIndexEvolutionTest extends TestWithFeService {

    @Override
    protected void createStarrocksCluster() {
        UtFrameUtils.createMinStarRocksCluster(false, runMode);
    }

    @Override
    protected void runBeforeAll() throws Exception {
        Config.tablet_sched_checker_interval_seconds = 1;
        Config.tablet_sched_repair_delay_factor_second = 1;
        Config.enable_new_publish_mechanism = true;
        Config.alter_scheduler_interval_millisecond = 100;
        Config.enable_experimental_gin = true;
        Config.enable_independent_index_evolution = true;

        createDatabase("test");

        String createDupTblStmtStr = "CREATE TABLE IF NOT EXISTS test.t_indie_idx (\n"
                + "timestamp DATETIME,\n"
                + "type INT,\n"
                + "error_code INT,\n"
                + "error_msg VARCHAR(1024),\n"
                + "op_id BIGINT,\n"
                + "op_time DATETIME)\n"
                + "DUPLICATE KEY(timestamp, type)\n"
                + "DISTRIBUTED BY HASH(type) BUCKETS 1\n"
                + "PROPERTIES ('replication_num' = '1', 'fast_schema_evolution' = 'true');";
        createTable(createDupTblStmtStr);
    }

    @Test
    public void testIsIndependentIndex() {
        // All index types are now independent (stored in separate files)
        Assertions.assertTrue(IndexDef.IndexType.isIndependentIndex(IndexType.GIN));
        Assertions.assertTrue(IndexDef.IndexType.isIndependentIndex(IndexType.VECTOR));
        Assertions.assertTrue(IndexDef.IndexType.isIndependentIndex(IndexType.BITMAP));
        Assertions.assertTrue(IndexDef.IndexType.isIndependentIndex(IndexType.NGRAMBF));
    }

    @Test
    public void testAddGINIndexUseFastPath() throws Exception {
        // Add a GIN index - should use fast schema evolution (no data rewriting)
        String alterStmt = "ALTER TABLE test.t_indie_idx ADD INDEX idx_gin_msg (error_msg) USING GIN";
        alterTableStmt(alterStmt);

        // After independent index evolution, table state should be NORMAL immediately
        // (no pending schema change job)
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        OlapTable olapTable = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                .getTable(db.getId(), "t_indie_idx");
        Assertions.assertNotNull(olapTable);
        Assertions.assertEquals(OlapTable.OlapTableState.NORMAL, olapTable.getState());

        // Verify the GIN index was added
        List<Index> indexes = olapTable.getIndexes();
        boolean foundGIN = false;
        for (Index idx : indexes) {
            if (idx.getIndexName().equalsIgnoreCase("idx_gin_msg") &&
                    idx.getIndexType() == IndexType.GIN) {
                foundGIN = true;
                break;
            }
        }
        Assertions.assertTrue(foundGIN, "GIN index should be added to table metadata");
    }

    @Test
    public void testDropGINIndexUseFastPath() throws Exception {
        // First add a GIN index
        String addStmt = "ALTER TABLE test.t_indie_idx ADD INDEX idx_gin_drop (error_msg) USING GIN";
        alterTableStmt(addStmt);

        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        OlapTable olapTable = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                .getTable(db.getId(), "t_indie_idx");
        Assertions.assertTrue(olapTable.getIndexes().stream()
                .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_gin_drop")));

        // Drop the GIN index - should also use fast path
        String dropStmt = "ALTER TABLE test.t_indie_idx DROP INDEX idx_gin_drop";
        alterTableStmt(dropStmt);

        // Table state should be NORMAL immediately
        Assertions.assertEquals(OlapTable.OlapTableState.NORMAL, olapTable.getState());

        // Verify the GIN index was removed
        Assertions.assertFalse(olapTable.getIndexes().stream()
                .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_gin_drop")));
    }

    @Test
    public void testAddBitmapIndexUseFastPath() throws Exception {
        // Add a BITMAP index - should use fast path (standalone index files)
        String alterStmt = "ALTER TABLE test.t_indie_idx ADD INDEX idx_bitmap_type (type) USING BITMAP";
        alterTableStmt(alterStmt);

        // After independent index evolution, table state should be NORMAL immediately
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        OlapTable olapTable = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                .getTable(db.getId(), "t_indie_idx");
        Assertions.assertNotNull(olapTable);
        Assertions.assertEquals(OlapTable.OlapTableState.NORMAL, olapTable.getState());

        // Verify the BITMAP index was added
        Assertions.assertTrue(olapTable.getIndexes().stream()
                .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_bitmap_type")
                        && idx.getIndexType() == IndexType.BITMAP));
    }

    @Test
    public void testDropBitmapIndexUseFastPath() throws Exception {
        // First add a BITMAP index
        String addStmt = "ALTER TABLE test.t_indie_idx ADD INDEX idx_bitmap_drop (error_code) USING BITMAP";
        alterTableStmt(addStmt);

        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        OlapTable olapTable = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                .getTable(db.getId(), "t_indie_idx");
        Assertions.assertTrue(olapTable.getIndexes().stream()
                .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_bitmap_drop")));

        // Drop the BITMAP index - should also use fast path
        String dropStmt = "ALTER TABLE test.t_indie_idx DROP INDEX idx_bitmap_drop";
        alterTableStmt(dropStmt);

        // Table state should be NORMAL immediately
        Assertions.assertEquals(OlapTable.OlapTableState.NORMAL, olapTable.getState());

        // Verify the BITMAP index was removed
        Assertions.assertFalse(olapTable.getIndexes().stream()
                .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_bitmap_drop")));
    }

    @Test
    public void testDisabledIndependentIndexEvolution() throws Exception {
        // Create a separate table for this test
        String createTblStmt = "CREATE TABLE IF NOT EXISTS test.t_indie_disabled (\n"
                + "timestamp DATETIME,\n"
                + "type INT,\n"
                + "msg VARCHAR(1024))\n"
                + "DUPLICATE KEY(timestamp, type)\n"
                + "DISTRIBUTED BY HASH(type) BUCKETS 1\n"
                + "PROPERTIES ('replication_num' = '1', 'fast_schema_evolution' = 'true');";
        createTable(createTblStmt);

        // Disable independent index evolution
        boolean origValue = Config.enable_independent_index_evolution;
        try {
            Config.enable_independent_index_evolution = false;

            // Add GIN index - should trigger full schema change since feature is disabled
            String alterStmt = "ALTER TABLE test.t_indie_disabled ADD INDEX idx_gin_disabled (msg) USING GIN";
            alterTableStmt(alterStmt);

            Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
            OlapTable olapTable = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                    .getTable(db.getId(), "t_indie_disabled");

            // Wait for schema change to complete
            int maxWait = 30;
            while (maxWait > 0) {
                if (olapTable.getState() == OlapTable.OlapTableState.NORMAL) {
                    break;
                }
                Thread.sleep(1000);
                maxWait--;
            }

            // Verify index was added
            Assertions.assertTrue(olapTable.getIndexes().stream()
                    .anyMatch(idx -> idx.getIndexName().equalsIgnoreCase("idx_gin_disabled")
                            && idx.getIndexType() == IndexType.GIN));
        } finally {
            Config.enable_independent_index_evolution = origValue;
        }
    }
}
