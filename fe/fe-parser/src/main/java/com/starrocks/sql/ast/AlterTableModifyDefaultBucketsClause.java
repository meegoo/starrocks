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

package com.starrocks.sql.ast;

import com.google.common.base.Preconditions;
import com.starrocks.sql.parser.NodePosition;

import java.util.List;

/**
 * ALTER TABLE tbl [DISTRIBUTED BY HASH(col1, col2, ...) | DISTRIBUTED BY RANDOM] DEFAULT BUCKETS N;
 * Or simplified: ALTER TABLE tbl DEFAULT BUCKETS N;
 * Only modifies the table's defaultDistributionInfo bucket number (must be > 0) for future partitions.
 * Does not affect already created partitions.
 * - With DISTRIBUTED BY HASH: requires columns to match existing distribution columns.
 * - With DISTRIBUTED BY RANDOM: for random distribution tables.
 * - With DEFAULT BUCKETS only: uses table's current distribution type.
 */
public class AlterTableModifyDefaultBucketsClause extends AlterTableClause {
    public enum DistributionType {
        HASH,      // DISTRIBUTED BY HASH(cols) DEFAULT BUCKETS N
        RANDOM,    // DISTRIBUTED BY RANDOM DEFAULT BUCKETS N
        USE_EXISTING  // DEFAULT BUCKETS N only, use table's current distribution
    }

    private final List<String> distributionColumns;  // null for RANDOM and USE_EXISTING
    private final int bucketNum;
    private final DistributionType distributionType;

    public AlterTableModifyDefaultBucketsClause(List<String> distributionColumns, int bucketNum, NodePosition pos) {
        this(distributionColumns, bucketNum, DistributionType.HASH, pos);
    }

    public AlterTableModifyDefaultBucketsClause(int bucketNum, DistributionType distributionType, NodePosition pos) {
        this(null, bucketNum, distributionType, pos);
    }

    private AlterTableModifyDefaultBucketsClause(List<String> distributionColumns, int bucketNum,
                                                 DistributionType distributionType, NodePosition pos) {
        super(pos);
        Preconditions.checkArgument(bucketNum > 0, "bucket num must > 0");
        this.distributionColumns = distributionColumns;
        this.bucketNum = bucketNum;
        this.distributionType = distributionType;
    }

    public List<String> getDistributionColumns() {
        return distributionColumns;
    }

    public int getBucketNum() {
        return bucketNum;
    }

    public DistributionType getDistributionType() {
        return distributionType;
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitAlterTableModifyDefaultBucketsClause(this, context);
    }
}
