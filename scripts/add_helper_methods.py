file_path = '/workspace/fe/fe-core/src/main/java/com/starrocks/service/FrontendServiceImpl.java'
with open(file_path, 'r') as f:
    content = f.read()

old_code = '''        } finally {
            PENDING_PARTITION_REQUESTS.remove(requestKey);
        }
    }

    private static TCreatePartitionResult validateCreatePartitionRequest(TCreatePartitionRequest request,'''

new_code = '''        } finally {
            PENDING_PARTITION_REQUESTS.remove(requestKey);
        }
    }

    /**
     * Build a unique key for partition creation request based on tableId and partition names.
     * This key is used to deduplicate identical concurrent requests.
     */
    private static String buildPartitionRequestKey(long tableId, Set<String> partitionNames) {
        List<String> sortedNames = partitionNames.stream().sorted().collect(Collectors.toList());
        return tableId + "_" + String.join(",", sortedNames);
    }

    /**
     * Get the timeout for partition creation request.
     * Uses the request timeout if set, otherwise defaults to 30 seconds.
     */
    private static int getPartitionRequestTimeout(TCreatePartitionRequest request) {
        if (request.isSetTimeout()) {
            // Convert seconds to milliseconds
            return request.getTimeout() * 1000;
        }
        // Default timeout: 30 seconds
        return 30000;
    }

    /**
     * Execute partition creation with proper locking.
     * This method acquires partition-level locks, performs double-check, and creates partitions if needed.
     */
    private static TCreatePartitionResult doCreatePartitionWithLock(
            GlobalStateMgr state, Database db, OlapTable olapTable, TransactionState txnState, long txnId,
            AddPartitionClause addPartitionClause, Set<String> creatingPartitionNames,
            ConcurrentMap<String, TOlapTablePartition> cachedPartitions,
            List<String> partitionColNames, boolean isTemp) throws Exception {

        try {
            acquirePartitionLocks(olapTable, creatingPartitionNames);

            // Double-check after acquiring lock (another request may have created the partition)
            if (!areAllPartitionsCached(cachedPartitions, creatingPartitionNames)) {
                createPartitionsIfNeeded(state, db, olapTable, txnState, txnId,
                        addPartitionClause, creatingPartitionNames);
            }
        } finally {
            releasePartitionLocks(olapTable, creatingPartitionNames);
        }

        // Check transaction status before building response
        if (txnState.getTransactionStatus().isFinalStatus()) {
            return buildErrorResult(String.format("automatic create partition failed. error: txn %d is %s",
                    txnId, txnState.getTransactionStatus().name()));
        }

        return buildResponseWithLock(db, olapTable, txnState, partitionColNames, isTemp);
    }

    private static TCreatePartitionResult validateCreatePartitionRequest(TCreatePartitionRequest request,'''

if old_code in content:
    content = content.replace(old_code, new_code)
    with open(file_path, 'w') as f:
        f.write(content)
    print("Helper methods added successfully")
else:
    print("ERROR: Could not find the insertion point")
    # Debug: check if partial match exists
    if "PENDING_PARTITION_REQUESTS.remove(requestKey);" in content:
        print("Found PENDING_PARTITION_REQUESTS.remove")
    if "private static TCreatePartitionResult validateCreatePartitionRequest" in content:
        print("Found validateCreatePartitionRequest")
