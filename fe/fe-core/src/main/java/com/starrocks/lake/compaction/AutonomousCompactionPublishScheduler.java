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

package com.starrocks.lake.compaction;

import com.google.common.collect.Lists;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.PhysicalPartition;
import com.starrocks.catalog.Tablet;
import com.starrocks.common.Config;
import com.starrocks.common.util.Daemon;
import com.starrocks.common.util.concurrent.lock.LockType;
import com.starrocks.common.util.concurrent.lock.Locker;
import com.starrocks.proto.CompactRequest;
import com.starrocks.rpc.BrpcProxy;
import com.starrocks.rpc.LakeService;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.server.WarehouseManager;
import com.starrocks.service.FrontendOptions;
import com.starrocks.system.ComputeNode;
import com.starrocks.transaction.GlobalTransactionMgr;
import com.starrocks.transaction.TabletCommitInfo;
import com.starrocks.transaction.TransactionState;
import com.starrocks.warehouse.cngroup.ComputeResource;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Periodically scans partitions and triggers PUBLISH_AUTONOMOUS for autonomous compaction.
 * This scheduler works in parallel with the main CompactionScheduler.
 * 
 * <h2>Exception Recovery (Design Doc Section 6.2)</h2>
 * <p>FE Recovery After Crash - Recovery Mechanisms:</p>
 * <ul>
 *   <li><b>Stateless Design:</b> FE does not store intermediate scheduling state.
 *       After restart, it restores partition statistics from metadata.
 *       Resumes the periodic scheduling loop automatically.</li>
 *   <li><b>Incomplete Transaction Handling:</b> After restart, FE checks all incomplete
 *       compaction transactions. Expired transactions are rolled back.
 *       BE local results remain available for the next publish.</li>
 *   <li><b>BE-Side Data Protection:</b> FE rolling back a transaction does not delete
 *       BE local results. Next publish will reuse them. Avoids repeated compaction work.</li>
 * </ul>
 * 
 * <h2>Why No FE Periodic Patrol Is Needed (Design Doc Section 6.3.2)</h2>
 * <p>The scheduler already runs every second. partitionStatisticsHashMap tracks all
 * partition states. High-score partitions are naturally re-selected. Additional patrol
 * is redundant.</p>
 */
public class AutonomousCompactionPublishScheduler extends Daemon {
    private static final Logger LOG = LogManager.getLogger(AutonomousCompactionPublishScheduler.class);
    private static final String HOST_NAME = FrontendOptions.getLocalHostAddress();

    // Label prefix for autonomous compaction transactions
    private static final String TXN_LABEL_PREFIX = "autonomous_compaction_publish_";

    private final GlobalStateMgr stateMgr;
    private final GlobalTransactionMgr transactionMgr;
    private final CompactionMgr compactionMgr;

    // Track partition publish state
    private final ConcurrentHashMap<PartitionIdentifier, PartitionPublishState> partitionStates;

    // Track if recovery has been performed after startup
    private final AtomicBoolean recoveryPerformed = new AtomicBoolean(false);

    // Metrics
    private long totalPublishTriggered = 0;
    private long totalPublishSucceeded = 0;
    private long totalPublishFailed = 0;
    private long totalTransactionsRecovered = 0;
    private long totalTransactionsRolledBack = 0;

    public AutonomousCompactionPublishScheduler(GlobalStateMgr stateMgr,
                                                 GlobalTransactionMgr transactionMgr,
                                                 CompactionMgr compactionMgr) {
        super("AUTONOMOUS_COMPACTION_PUBLISH", Config.lake_compaction_periodic_publish_interval_ms);
        this.stateMgr = stateMgr;
        this.transactionMgr = transactionMgr;
        this.compactionMgr = compactionMgr;
        this.partitionStates = new ConcurrentHashMap<>();
    }

    @Override
    protected void runOneCycle() {
        if (!Config.enable_lake_autonomous_compaction || !Config.enable_lake_compaction_periodic_publish) {
            return;
        }

        // Only run on leader FE
        if (!stateMgr.isLeader() || !stateMgr.isReady()) {
            return;
        }

        try {
            // Perform recovery on first cycle after FE startup (Design Doc Section 6.2.1)
            if (recoveryPerformed.compareAndSet(false, true)) {
                recoverIncompleteTransactions();
            }

            scanAndPublish();
        } catch (Exception e) {
            LOG.error("Error in autonomous compaction publish cycle", e);
        }
    }

    /**
     * Recover incomplete transactions after FE restart.
     * 
     * Design Doc Section 6.2.1 - Incomplete Transaction Handling:
     * - After restart, FE checks all incomplete compaction transactions
     * - Expired transactions are rolled back
     * - BE local results remain available for the next publish
     */
    private void recoverIncompleteTransactions() {
        LOG.info("Starting recovery of incomplete autonomous compaction transactions...");

        try {
            // Get all incomplete transactions with our label prefix
            List<TransactionState> incompleteTransactions = findIncompleteCompactionTransactions();
            
            int recovered = 0;
            int rolledBack = 0;
            
            for (TransactionState txnState : incompleteTransactions) {
                try {
                    if (isTransactionExpired(txnState)) {
                        // Rollback expired transaction
                        // Note: BE local results are NOT deleted during rollback (BE-Side Data Protection)
                        rollbackExpiredTransaction(txnState);
                        rolledBack++;
                        LOG.info("Rolled back expired autonomous compaction transaction: txnId={}, label={}",
                                txnState.getTransactionId(), txnState.getLabel());
                    } else {
                        // Transaction is still valid - let it continue or retry
                        recovered++;
                        LOG.info("Recovered autonomous compaction transaction: txnId={}, label={}, state={}",
                                txnState.getTransactionId(), txnState.getLabel(), txnState.getTransactionStatus());
                    }
                } catch (Exception e) {
                    LOG.warn("Error processing transaction {}: {}", txnState.getTransactionId(), e.getMessage());
                }
            }
            
            totalTransactionsRecovered = recovered;
            totalTransactionsRolledBack = rolledBack;
            
            LOG.info("Completed recovery of autonomous compaction transactions: recovered={}, rolledBack={}",
                    recovered, rolledBack);
            
        } catch (Exception e) {
            LOG.error("Error during autonomous compaction transaction recovery", e);
        }
    }

    /**
     * Find all incomplete compaction transactions created by this scheduler.
     */
    private List<TransactionState> findIncompleteCompactionTransactions() {
        List<TransactionState> result = new ArrayList<>();
        
        // Get all databases and check their transactions
        List<Long> dbIds = stateMgr.getLocalMetastore().getDbIds();
        for (Long dbId : dbIds) {
            try {
                // Get transactions in PREPARE or COMMITTED state (incomplete)
                List<TransactionState> dbTransactions = transactionMgr.getDatabaseTransactionMgr(dbId)
                        .getReadyToPublishTxnList();
                
                for (TransactionState txnState : dbTransactions) {
                    // Check if this is an autonomous compaction transaction
                    if (txnState.getLabel() != null && 
                            txnState.getLabel().startsWith(TXN_LABEL_PREFIX) &&
                            txnState.getSourceType() == TransactionState.LoadJobSourceType.LAKE_COMPACTION) {
                        result.add(txnState);
                    }
                }
            } catch (Exception e) {
                LOG.warn("Error getting transactions for database {}: {}", dbId, e.getMessage());
            }
        }
        
        return result;
    }

    /**
     * Check if a transaction has expired based on its timeout.
     */
    private boolean isTransactionExpired(TransactionState txnState) {
        long currentTimeMs = System.currentTimeMillis();
        long txnTimeoutMs = Config.lake_compaction_publish_timeout_seconds * 1000L;
        
        // Check if transaction has exceeded its timeout
        return (currentTimeMs - txnState.getPrepareTime()) > txnTimeoutMs;
    }

    /**
     * Rollback an expired transaction.
     * 
     * Note: This does NOT delete BE local results (BE-Side Data Protection).
     * The next publish cycle will reuse those results.
     */
    private void rollbackExpiredTransaction(TransactionState txnState) throws Exception {
        long dbId = txnState.getDbId();
        long txnId = txnState.getTransactionId();
        
        transactionMgr.abortTransaction(
                dbId,
                txnId,
                "Autonomous compaction transaction expired after FE restart",
                Lists.newArrayList(),
                Lists.newArrayList(),
                null);
        
        LOG.info("Aborted expired autonomous compaction transaction: dbId={}, txnId={}, label={}",
                dbId, txnId, txnState.getLabel());
    }

    private void scanAndPublish() {
        long currentTimeMs = System.currentTimeMillis();
        List<PartitionIdentifier> partitionsToPublish = new ArrayList<>();

        // Scan all databases and partitions
        List<Long> dbIds = stateMgr.getLocalMetastore().getDbIds();
        for (Long dbId : dbIds) {
            Database db = stateMgr.getLocalMetastore().getDb(dbId);
            if (db == null) {
                continue;
            }

            Locker locker = new Locker();
            locker.lockDatabase(db.getId(), LockType.READ);
            try {
                for (OlapTable table : db.getTables().stream()
                        .filter(t -> t.isOlapTableOrMaterializedView() && t.isCloudNativeTableOrMaterializedView())
                        .map(t -> (OlapTable) t)
                        .collect(java.util.stream.Collectors.toList())) {

                    for (PhysicalPartition partition : table.getAllPhysicalPartitions()) {
                        PartitionIdentifier partitionId = new PartitionIdentifier(
                                db.getId(), table.getId(), partition.getId());

                        if (shouldPublish(partitionId, partition, currentTimeMs)) {
                            partitionsToPublish.add(partitionId);
                        }
                    }
                }
            } finally {
                locker.unLockDatabase(db.getId(), LockType.READ);
            }
        }

        // Trigger publish for selected partitions
        LOG.info("Found {} partitions to publish for autonomous compaction", partitionsToPublish.size());
        for (PartitionIdentifier partitionId : partitionsToPublish) {
            try {
                triggerPublish(partitionId);
            } catch (Exception e) {
                LOG.error("Failed to trigger publish for partition {}", partitionId, e);
                totalPublishFailed++;
            }
        }
    }

    private boolean shouldPublish(PartitionIdentifier partitionId, PhysicalPartition partition, long currentTimeMs) {
        PartitionPublishState state = partitionStates.computeIfAbsent(partitionId, k -> new PartitionPublishState());

        long timeSinceLastPublish = currentTimeMs - state.lastPublishTimeMs;

        // Strategy 1: High score priority
        PartitionStatistics statistics = compactionMgr.getStatistics(partitionId);
        if (statistics != null) {
            double score = statistics.getCompactionScore().getMax();
            long versionDelta = partition.getVisibleVersion() - state.lastPublishedVersion;

            // High score with minimum version delta
            if (score >= Config.lake_compaction_high_score_threshold &&
                    versionDelta >= Config.lake_compaction_min_version_delta_for_high_score) {
                LOG.info("Partition {} triggers publish by high score: score={}, versionDelta={}",
                        partitionId, score, versionDelta);
                state.lastScore = score;
                return true;
            }

            // Version delta threshold
            if (versionDelta >= Config.lake_compaction_version_delta_threshold) {
                LOG.info("Partition {} triggers publish by version delta: versionDelta={}, score={}",
                        partitionId, versionDelta, score);
                state.lastScore = score;
                return true;
            }

            state.lastScore = score;
        }

        // Strategy 2: Maximum interval
        if (timeSinceLastPublish >= Config.lake_compaction_max_interval_ms) {
            LOG.info("Partition {} triggers publish by max interval: intervalMs={}",
                    partitionId, timeSinceLastPublish);
            return true;
        }

        return false;
    }

    private void triggerPublish(PartitionIdentifier partitionId) throws Exception {
        Database db = stateMgr.getLocalMetastore().getDb(partitionId.getDbId());
        if (db == null) {
            LOG.warn("Database {} not found", partitionId.getDbId());
            return;
        }

        OlapTable table;
        PhysicalPartition partition;
        List<Long> tabletIds = new ArrayList<>();

        Locker locker = new Locker();
        locker.lockDatabase(db.getId(), LockType.READ);
        try {
            table = (OlapTable) db.getTable(partitionId.getTableId());
            if (table == null) {
                LOG.warn("Table {} not found", partitionId.getTableId());
                return;
            }

            partition = table.getPhysicalPartition(partitionId.getPartitionId());
            if (partition == null) {
                LOG.warn("Partition {} not found", partitionId.getPartitionId());
                return;
            }

            // Collect all tablet IDs in this partition
            for (MaterializedIndex index : partition.getMaterializedIndices(MaterializedIndex.IndexExtState.VISIBLE)) {
                for (Tablet tablet : index.getTablets()) {
                    tabletIds.add(tablet.getId());
                }
            }
        } finally {
            locker.unLockDatabase(db.getId(), LockType.READ);
        }

        if (tabletIds.isEmpty()) {
            LOG.warn("No tablets found in partition {}", partitionId);
            return;
        }

        // Begin transaction
        TransactionState.TxnCoordinator coordinator = new TransactionState.TxnCoordinator(
                TransactionState.TxnSourceType.FE, HOST_NAME);
        TransactionState.LoadJobSourceType sourceType = TransactionState.LoadJobSourceType.LAKE_COMPACTION;
        String label = "autonomous_compaction_publish_" + System.currentTimeMillis();
        
        long txnId = transactionMgr.beginTransaction(
                db.getId(),
                Lists.newArrayList(partitionId.getTableId()),
                label,
                coordinator,
                sourceType,
                Config.lake_compaction_publish_timeout_seconds,
                null);

        LOG.info("Started autonomous compaction publish for partition {}, txnId={}, tablets={}",
                partitionId, txnId, tabletIds.size());

        // Group tablets by backend using actual tablet replica locations
        Map<Long, List<Long>> backendToTablets = groupTabletsByBackend(tabletIds, table.getId());

        // Send PUBLISH_AUTONOMOUS request to each backend
        boolean hasFailure = false;
        for (Map.Entry<Long, List<Long>> entry : backendToTablets.entrySet()) {
            Long backendId = entry.getKey();
            List<Long> tablets = entry.getValue();

            ComputeNode node = GlobalStateMgr.getCurrentState().getNodeMgr().getClusterInfo().getBackendOrComputeNode(backendId);
            if (node == null) {
                LOG.warn("Backend {} not found", backendId);
                hasFailure = true;
                continue;
            }

            try {
                CompactRequest request = new CompactRequest();
                request.tabletIds = tablets;
                request.txnId = txnId;
                request.version = partition.getVisibleVersion();
                request.visibleVersion = partition.getVisibleVersion();
                request.timeoutMs = Config.lake_compaction_publish_timeout_seconds * 1000L;
                request.allowPartialSuccess = true;
                request.publishAutonomous = true; // Key flag for PUBLISH_AUTONOMOUS

                LakeService lakeService = BrpcProxy.getLakeService(node.getHost(), node.getBrpcPort());
                lakeService.compact(request).get();

                LOG.info("Sent PUBLISH_AUTONOMOUS request to backend {}, tablets={}", backendId, tablets.size());
            } catch (Exception e) {
                LOG.error("Failed to send PUBLISH_AUTONOMOUS to backend {}", backendId, e);
                hasFailure = true;
            }
        }

        // Commit or abort transaction
        if (hasFailure) {
            transactionMgr.abortTransaction(db.getId(), txnId, "PUBLISH_AUTONOMOUS failed", 
                    Lists.newArrayList(), Lists.newArrayList(), null);
            totalPublishFailed++;
            LOG.warn("Aborted autonomous compaction publish for partition {}, txnId={}", partitionId, txnId);
        } else {
            // Build commit info - use the backend where tablets are located
            List<TabletCommitInfo> commitInfos = new ArrayList<>();
            for (Map.Entry<Long, List<Long>> entry : backendToTablets.entrySet()) {
                Long beId = entry.getKey();
                for (Long tabletId : entry.getValue()) {
                    commitInfos.add(new TabletCommitInfo(tabletId, beId));
                }
            }

            CompactionTxnCommitAttachment attachment = new CompactionTxnCommitAttachment(true);
            transactionMgr.commitTransaction(db.getId(), txnId, commitInfos,
                    Lists.newArrayList(), attachment);

            // Update partition state
            PartitionPublishState state = partitionStates.get(partitionId);
            if (state != null) {
                state.lastPublishTimeMs = System.currentTimeMillis();
                state.lastPublishedVersion = partition.getVisibleVersion() + 1; // Will be incremented by publish
            }

            totalPublishSucceeded++;
            totalPublishTriggered++;

            LOG.info("Successfully triggered autonomous compaction publish for partition {}, txnId={}",
                    partitionId, txnId);
        }
    }

    private Map<Long, List<Long>> groupTabletsByBackend(List<Long> tabletIds, long tableId) {
        Map<Long, List<Long>> result = new HashMap<>();
        WarehouseManager warehouseManager = GlobalStateMgr.getCurrentState().getWarehouseMgr();
        ComputeResource computeResource = warehouseManager.getCompactionComputeResource(tableId);

        for (Long tabletId : tabletIds) {
            try {
                ComputeNode computeNode = warehouseManager.getComputeNodeAssignedToTablet(computeResource, tabletId);
                if (computeNode != null) {
                    result.computeIfAbsent(computeNode.getId(), k -> new ArrayList<>()).add(tabletId);
                } else {
                    LOG.warn("No compute node found for tablet {}", tabletId);
                }
            } catch (Exception e) {
                LOG.warn("Failed to get compute node for tablet {}: {}", tabletId, e.getMessage());
            }
        }

        return result;
    }

    public long getTotalPublishTriggered() {
        return totalPublishTriggered;
    }

    public long getTotalPublishSucceeded() {
        return totalPublishSucceeded;
    }

    public long getTotalPublishFailed() {
        return totalPublishFailed;
    }

    /**
     * Get the number of transactions recovered during startup.
     */
    public long getTotalTransactionsRecovered() {
        return totalTransactionsRecovered;
    }

    /**
     * Get the number of transactions rolled back during startup recovery.
     */
    public long getTotalTransactionsRolledBack() {
        return totalTransactionsRolledBack;
    }

    /**
     * Check if recovery has been performed.
     */
    public boolean isRecoveryPerformed() {
        return recoveryPerformed.get();
    }

    // Track partition publish state
    private static class PartitionPublishState {
        long lastPublishTimeMs = 0;
        long lastPublishedVersion = 0;
        double lastScore = 0.0;
    }
}

