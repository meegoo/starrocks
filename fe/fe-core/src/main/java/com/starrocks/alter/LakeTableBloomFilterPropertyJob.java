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

import com.google.common.collect.Sets;
import com.google.gson.annotations.SerializedName;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.ColumnId;
import com.starrocks.catalog.OlapTable;
import com.starrocks.task.AlterReplicaTask;
import com.starrocks.thrift.TDropIndexInfo;
import com.starrocks.thrift.TIndexType;
import com.starrocks.thrift.TOlapTableIndex;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Lake-only fast-path Job for {@code ALTER TABLE ... SET
 * ("bloom_filter_columns" = "...")} where the diff is a pure add or a pure
 * drop (no mixing, no bloom_filter_fpp change).
 *
 * <p>Unlike {@link LakeTableAddIndexJob} / {@link LakeTableDropIndexJob}
 * (which manage explicit {@code CREATE INDEX} objects), this job carries
 * table-level bloom-filter property mutations. BE builds / tombstones the
 * per-segment BF payload into the IDG via the same
 * {@code do_process_add_index_only} / {@code do_process_drop_index_only}
 * entry points, keyed on {@code IndexType::BLOOM_FILTER}.
 *
 * <p>Invariant: exactly one of {@code addedColumns}, {@code removedColumns}
 * is non-empty per instance. The classifier in {@link SchemaChangeHandler}
 * enforces this; mixed (add + drop in the same ALTER) falls back to the
 * legacy rewrite path because BE dispatches {@code only_add_index} and
 * {@code only_drop_index} as mutually exclusive task modes.
 *
 * <p>Catalog mutation at finish: merge {@code addedColumns} into
 * {@code OlapTable.bfColumns} (or remove {@code removedColumns} from it)
 * and keep {@code bfFpp} unchanged (a separate FPP-only change is NOT
 * eligible for this fast path).
 */
public class LakeTableBloomFilterPropertyJob extends LakeTableIndexFastPathJobBase {

    /**
     * Column unique ids being added to bloom_filter_columns. Serialized so a
     * replayed job (FE cold start after crash) can reapply the catalog
     * mutation idempotently. Non-empty implies {@link #removedColumnUniqueIds}
     * is empty.
     */
    @SerializedName(value = "addedColumnUniqueIds")
    private List<Integer> addedColumnUniqueIds = new ArrayList<>();

    /** Column unique ids being removed from bloom_filter_columns. */
    @SerializedName(value = "removedColumnUniqueIds")
    private List<Integer> removedColumnUniqueIds = new ArrayList<>();

    /**
     * Final bloom_filter_columns set after applying the diff, captured at
     * job creation time. Used at catalog-mutation time to restore
     * {@code OlapTable.bfColumns} to the post-alter state idempotently even
     * if the live catalog has been mutated concurrently.
     */
    @SerializedName(value = "finalBfColumnIds")
    private Set<String> finalBfColumnIdStrings = new HashSet<>();

    /** Final bfFpp (unchanged for this fast path). */
    @SerializedName(value = "finalBfFpp")
    private double finalBfFpp;

    /**
     * Thrift payload for BE ADD INDEX path. One entry per added column,
     * index_type=BLOOM_FILTER. index_properties carries bloom_filter_fpp
     * so BE's BloomFilterOptions picks it up during build.
     */
    @SerializedName(value = "indexesToAdd")
    private List<TOlapTableIndex> indexesToAdd = new ArrayList<>();

    /**
     * Thrift payload for BE DROP INDEX path. index_id is a sentinel (-1)
     * because BF-property bloom has no catalog Index object; BE's
     * apply_drop_index keys IDG tombstones on (col_unique_id, index_type),
     * independent of index_id.
     */
    @SerializedName(value = "dropInfos")
    private List<TDropIndexInfo> dropInfos = new ArrayList<>();

    /** For deserialization / GSON. */
    public LakeTableBloomFilterPropertyJob() {
        super(JobType.SCHEMA_CHANGE);
    }

    public LakeTableBloomFilterPropertyJob(long jobId, long dbId, long tableId, String tableName, long timeoutMs,
                                           List<Integer> addedColumnUniqueIds,
                                           List<Integer> removedColumnUniqueIds,
                                           Set<ColumnId> finalBfColumns, double finalBfFpp,
                                           List<TOlapTableIndex> indexesToAdd,
                                           List<TDropIndexInfo> dropInfos) {
        super(jobId, JobType.SCHEMA_CHANGE, dbId, tableId, tableName, timeoutMs);
        this.addedColumnUniqueIds = new ArrayList<>(addedColumnUniqueIds);
        this.removedColumnUniqueIds = new ArrayList<>(removedColumnUniqueIds);
        this.finalBfColumnIdStrings = new HashSet<>();
        if (finalBfColumns != null) {
            for (ColumnId c : finalBfColumns) {
                this.finalBfColumnIdStrings.add(c.getId());
            }
        }
        this.finalBfFpp = finalBfFpp;
        this.indexesToAdd = new ArrayList<>(indexesToAdd);
        this.dropInfos = new ArrayList<>(dropInfos);
    }

    protected LakeTableBloomFilterPropertyJob(LakeTableBloomFilterPropertyJob other) {
        super(other);
        this.addedColumnUniqueIds =
                other.addedColumnUniqueIds == null ? null : new ArrayList<>(other.addedColumnUniqueIds);
        this.removedColumnUniqueIds =
                other.removedColumnUniqueIds == null ? null : new ArrayList<>(other.removedColumnUniqueIds);
        this.finalBfColumnIdStrings =
                other.finalBfColumnIdStrings == null ? null : new HashSet<>(other.finalBfColumnIdStrings);
        this.finalBfFpp = other.finalBfFpp;
        this.indexesToAdd = other.indexesToAdd == null ? null : new ArrayList<>(other.indexesToAdd);
        this.dropInfos = other.dropInfos == null ? null : new ArrayList<>(other.dropInfos);
    }

    @Override
    protected void populateAlterRequest(AlterReplicaTask task) {
        // Pure-add and pure-drop are mutually exclusive by construction; BE
        // dispatches the two fast-path modes as if-else, so we must set
        // exactly one. Mixed diffs are rejected earlier by the classifier.
        if (addedColumnUniqueIds != null && !addedColumnUniqueIds.isEmpty()) {
            task.setOnlyAddIndex(indexesToAdd);
        } else if (removedColumnUniqueIds != null && !removedColumnUniqueIds.isEmpty()) {
            task.setOnlyDropIndex(dropInfos);
        }
    }

    @Override
    protected void applyCatalogMutation(OlapTable table) {
        // Reconstruct the post-alter bfColumns set from the serialized list
        // of column id strings, filtered through the live schema so dropped
        // columns (if any concurrent alter happened) are skipped. fpp is
        // carried through unchanged for this fast path.
        Set<ColumnId> newBfColumns = Sets.newTreeSet(ColumnId.CASE_INSENSITIVE_ORDER);
        if (finalBfColumnIdStrings != null) {
            for (String s : finalBfColumnIdStrings) {
                ColumnId cid = ColumnId.create(s);
                Column col = table.getColumn(cid);
                if (col != null) {
                    newBfColumns.add(cid);
                }
            }
        }
        if (newBfColumns.isEmpty()) {
            table.setBloomFilterInfo(null, 0);
        } else {
            table.setBloomFilterInfo(newBfColumns, finalBfFpp);
        }
    }

    @Override
    public AlterJobV2 copyForPersist() {
        LakeTableBloomFilterPropertyJob copy = new LakeTableBloomFilterPropertyJob();
        copyBaseFields(copy);
        copy.watershedTxnId = this.watershedTxnId;
        copy.partitionToTablets = this.partitionToTablets;
        copy.tabletToIndexMetaId = this.tabletToIndexMetaId;
        copy.commitVersionMap = this.commitVersionMap;
        copySubclassFields(copy);
        return copy;
    }

    @Override
    protected void copySubclassFields(LakeTableIndexFastPathJobBase copy) {
        LakeTableBloomFilterPropertyJob c = (LakeTableBloomFilterPropertyJob) copy;
        c.addedColumnUniqueIds =
                this.addedColumnUniqueIds == null ? null : new ArrayList<>(this.addedColumnUniqueIds);
        c.removedColumnUniqueIds =
                this.removedColumnUniqueIds == null ? null : new ArrayList<>(this.removedColumnUniqueIds);
        c.finalBfColumnIdStrings =
                this.finalBfColumnIdStrings == null ? null : new HashSet<>(this.finalBfColumnIdStrings);
        c.finalBfFpp = this.finalBfFpp;
        c.indexesToAdd = this.indexesToAdd == null ? null : new ArrayList<>(this.indexesToAdd);
        c.dropInfos = this.dropInfos == null ? null : new ArrayList<>(this.dropInfos);
    }

    // Accessors for tests / tooling.
    public List<Integer> getAddedColumnUniqueIds() {
        return addedColumnUniqueIds == null ? Collections.emptyList() : addedColumnUniqueIds;
    }

    public List<Integer> getRemovedColumnUniqueIds() {
        return removedColumnUniqueIds == null ? Collections.emptyList() : removedColumnUniqueIds;
    }

    public Set<String> getFinalBfColumnIdStrings() {
        return finalBfColumnIdStrings == null ? Collections.emptySet()
                : Collections.unmodifiableSet(finalBfColumnIdStrings);
    }

    public double getFinalBfFpp() {
        return finalBfFpp;
    }

    public List<TOlapTableIndex> getIndexesToAdd() {
        return indexesToAdd == null ? Collections.emptyList() : indexesToAdd;
    }

    public List<TDropIndexInfo> getDropInfos() {
        return dropInfos == null ? Collections.emptyList() : dropInfos;
    }

    /**
     * Sentinel index_id used on TDropIndexInfo for BF-property drops.
     * apply_drop_index on BE keys IDG tombstones by (col_uid, index_type)
     * independent of index_id, and BF property has no catalog Index object
     * to match against, so any non-colliding value works; -1 makes the
     * "no associated Index object" intent explicit.
     */
    public static final long SENTINEL_INDEX_ID = -1L;

    /**
     * Build a TOlapTableIndex for the BE add-index payload.
     */
    public static TOlapTableIndex buildAddIndexPayload(String columnName, double bfFpp) {
        TOlapTableIndex t = new TOlapTableIndex();
        t.setIndex_id(SENTINEL_INDEX_ID);
        t.setIndex_type(TIndexType.BLOOM_FILTER);
        t.setColumns(new ArrayList<>(Collections.singletonList(columnName)));
        java.util.Map<String, String> props = new java.util.HashMap<>();
        // index_properties propagates fpp to BloomFilterIndexWriter via
        // BloomFilterOptions. Key spelling matches the one consumed by
        // build_bloom_for_column on BE.
        props.put("bloom_filter_fpp", String.valueOf(bfFpp));
        t.setIndex_properties(props);
        return t;
    }

    /**
     * Build a TDropIndexInfo for the BE drop-index payload.
     */
    public static TDropIndexInfo buildDropInfoPayload(int columnUniqueId) {
        TDropIndexInfo info = new TDropIndexInfo();
        info.setIndex_id(SENTINEL_INDEX_ID);
        info.setCol_unique_id(columnUniqueId);
        info.setIndex_type(TIndexType.BLOOM_FILTER);
        return info;
    }

}
