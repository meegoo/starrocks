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

import com.starrocks.catalog.Column;
import com.starrocks.catalog.ColumnId;
import com.starrocks.catalog.OlapTable;
import com.starrocks.thrift.TDropIndexInfo;
import com.starrocks.thrift.TIndexType;
import com.starrocks.thrift.TOlapTableIndex;
import mockit.Expectations;
import mockit.Mocked;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Unit coverage for {@link LakeTableBloomFilterPropertyJob}. Focuses on
 * construction invariants, populateAlterRequest dispatch, and catalog
 * mutation semantics. Full AlterJobV2 lifecycle is covered by e2e Lake
 * schema-change tests.
 */
public class LakeTableBloomFilterPropertyJobTest {

    private static TOlapTableIndex makeAddPayload(String colName) {
        return LakeTableBloomFilterPropertyJob.buildAddIndexPayload(colName, 0.05);
    }

    private static TDropIndexInfo makeDropPayload(int uid) {
        return LakeTableBloomFilterPropertyJob.buildDropInfoPayload(uid);
    }

    @Test
    public void testAddPayloadShape() {
        TOlapTableIndex t = LakeTableBloomFilterPropertyJob.buildAddIndexPayload("c1", 0.02);
        assertEquals(TIndexType.BLOOM_FILTER, t.getIndex_type());
        assertEquals(LakeTableBloomFilterPropertyJob.SENTINEL_INDEX_ID, t.getIndex_id());
        assertEquals(Collections.singletonList("c1"), t.getColumns());
        assertTrue(t.getIndex_properties().containsKey("bloom_filter_fpp"));
        assertEquals("0.02", t.getIndex_properties().get("bloom_filter_fpp"));
    }

    @Test
    public void testDropPayloadShape() {
        TDropIndexInfo d = LakeTableBloomFilterPropertyJob.buildDropInfoPayload(42);
        assertEquals(TIndexType.BLOOM_FILTER, d.getIndex_type());
        assertEquals(LakeTableBloomFilterPropertyJob.SENTINEL_INDEX_ID, d.getIndex_id());
        assertEquals(42, d.getCol_unique_id());
    }

    @Test
    public void testAddOnlyJobConstruction() {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        finalBf.add(ColumnId.create("c1"));
        finalBf.add(ColumnId.create("c2"));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.singletonList(11),
                Collections.emptyList(),
                finalBf, 0.05,
                Collections.singletonList(makeAddPayload("c2")),
                Collections.emptyList());

        assertEquals(AlterJobV2.JobState.PENDING, job.getJobState());
        assertEquals(AlterJobV2.JobType.SCHEMA_CHANGE, job.getType());
        assertEquals(Collections.singletonList(11), job.getAddedColumnUniqueIds());
        assertTrue(job.getRemovedColumnUniqueIds().isEmpty());
        assertEquals(1, job.getIndexesToAdd().size());
        assertTrue(job.getDropInfos().isEmpty());
        assertEquals(0.05, job.getFinalBfFpp(), 1e-9);
        Set<String> ids = new HashSet<>(job.getFinalBfColumnIdStrings());
        assertTrue(ids.contains("c1"));
        assertTrue(ids.contains("c2"));
    }

    @Test
    public void testDropOnlyJobConstruction() {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        finalBf.add(ColumnId.create("c1"));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                10L, 2L, 3L, "t", 60_000L,
                Collections.emptyList(),
                Collections.singletonList(22),
                finalBf, 0.05,
                Collections.emptyList(),
                Collections.singletonList(makeDropPayload(22)));

        assertTrue(job.getAddedColumnUniqueIds().isEmpty());
        assertEquals(Collections.singletonList(22), job.getRemovedColumnUniqueIds());
        assertTrue(job.getIndexesToAdd().isEmpty());
        assertEquals(1, job.getDropInfos().size());
        assertEquals(22, job.getDropInfos().get(0).getCol_unique_id());
    }

    @Test
    public void testCopyForPersistAddOnly() {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        finalBf.add(ColumnId.create("c1"));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                100L, 2L, 3L, "t", 60_000L,
                Collections.singletonList(11),
                Collections.emptyList(),
                finalBf, 0.05,
                Collections.singletonList(makeAddPayload("c1")),
                Collections.emptyList());

        AlterJobV2 copy = job.copyForPersist();
        assertInstanceOf(LakeTableBloomFilterPropertyJob.class, copy);
        LakeTableBloomFilterPropertyJob lc = (LakeTableBloomFilterPropertyJob) copy;
        assertEquals(job.getJobId(), lc.getJobId());
        assertEquals(job.getAddedColumnUniqueIds(), lc.getAddedColumnUniqueIds());
        assertEquals(job.getIndexesToAdd().size(), lc.getIndexesToAdd().size());
        // Defensive copy — distinct list instances.
        assertNotSame(job.getAddedColumnUniqueIds(), lc.getAddedColumnUniqueIds());
        assertNotSame(job.getIndexesToAdd(), lc.getIndexesToAdd());
        assertNotSame(job.getFinalBfColumnIdStrings(), lc.getFinalBfColumnIdStrings());
    }

    @Test
    public void testPopulateAlterRequestAddBranch(@Mocked com.starrocks.task.AlterReplicaTask task) {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        finalBf.add(ColumnId.create("c1"));
        List<TOlapTableIndex> adds = new ArrayList<>();
        adds.add(makeAddPayload("c1"));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.singletonList(11),
                Collections.emptyList(),
                finalBf, 0.05,
                adds,
                Collections.emptyList());

        new Expectations() {
            {
                task.setOnlyAddIndex(adds);
                times = 1;
                task.setOnlyDropIndex((List<TDropIndexInfo>) any);
                times = 0;
            }
        };
        job.populateAlterRequest(task);
    }

    @Test
    public void testPopulateAlterRequestDropBranch(@Mocked com.starrocks.task.AlterReplicaTask task) {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        List<TDropIndexInfo> drops = new ArrayList<>();
        drops.add(makeDropPayload(11));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.emptyList(),
                Collections.singletonList(11),
                finalBf, 0.05,
                Collections.emptyList(),
                drops);

        new Expectations() {
            {
                task.setOnlyDropIndex(drops);
                times = 1;
                task.setOnlyAddIndex((List<TOlapTableIndex>) any);
                times = 0;
            }
        };
        job.populateAlterRequest(task);
    }

    @Test
    public void testApplyCatalogMutationAdd(@Mocked OlapTable table, @Mocked Column c1, @Mocked Column c2) {
        Set<ColumnId> finalBf = new TreeSet<>(ColumnId.CASE_INSENSITIVE_ORDER);
        finalBf.add(ColumnId.create("c1"));
        finalBf.add(ColumnId.create("c2"));

        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.singletonList(11),
                Collections.emptyList(),
                finalBf, 0.05,
                Collections.singletonList(makeAddPayload("c2")),
                Collections.emptyList());

        new Expectations() {
            {
                table.getColumn(ColumnId.create("c1"));
                result = c1;
                table.getColumn(ColumnId.create("c2"));
                result = c2;
                table.setBloomFilterInfo((Set<ColumnId>) any, 0.05);
                times = 1;
            }
        };
        job.applyCatalogMutation(table);
    }

    @Test
    public void testApplyCatalogMutationDropToEmpty(@Mocked OlapTable table) {
        // finalBf is empty — drop removed all BF columns; apply should call
        // setBloomFilterInfo(null, 0) to fully disable the bloom property.
        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.emptyList(),
                Collections.singletonList(11),
                Collections.emptySet(), 0.05,
                Collections.emptyList(),
                Collections.singletonList(makeDropPayload(11)));

        new Expectations() {
            {
                table.setBloomFilterInfo(null, 0.0);
                times = 1;
            }
        };
        job.applyCatalogMutation(table);
    }

    @Test
    public void testTransactionIdUnsetBeforeWatershed() {
        LakeTableBloomFilterPropertyJob job = new LakeTableBloomFilterPropertyJob(
                1L, 2L, 3L, "t", 60_000L,
                Collections.emptyList(),
                Collections.emptyList(),
                Collections.emptySet(), 0.05,
                Collections.emptyList(),
                Collections.emptyList());
        // watershedTxnId defaults to -1 before runPendingJob allocates it.
        assertTrue(job.getTransactionId().isEmpty());
    }
}
