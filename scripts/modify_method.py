file_path = '/workspace/fe/fe-core/src/main/java/com/starrocks/service/FrontendServiceImpl.java'
with open(file_path, 'r') as f:
    content = f.read()

old_part = '''        // Fast path: if all partitions are cached, return directly without acquiring partition creation locks
        if (areAllPartitionsCached(cachedPartitions, creatingPartitionNames)) {
            return buildResponseWithLock(db, olapTable, txnState, partitionColNames, isTemp);
        }

        // Slow path: need to create partitions
        try {
            acquirePartitionLocks(olapTable, creatingPartitionNames);

            // Double-check after acquiring lock (another request may have created the partition)
            if (!areAllPartitionsCached(cachedPartitions, creatingPartitionNames)) {
                createPartitionsIfNeeded(state, db, olapTable, txnState, request.getTxn_id(),
                        addPartitionClause, creatingPartitionNames);
            }
        } catch (Exception e) {
            LOG.warn("failed to add partitions", e);
            txnState.setIsCreatePartitionFailed(true);
            return buildErrorResult(String.format("automatic create partition failed. error:%s", e.getMessage()));
        } finally {
            releasePartitionLocks(olapTable, creatingPartitionNames);
        }

        // Check transaction status before building response
        if (txnState.getTransactionStatus().isFinalStatus()) {
            return buildErrorResult(String.format("automatic create partition failed. error: txn %d is %s",
                    request.getTxn_id(), txnState.getTransactionStatus().name()));
        }

        return buildResponseWithLock(db, olapTable, txnState, partitionColNames, isTemp);
    }'''

new_part = '''        // Fast path: if all partitions are cached, return directly without acquiring any locks
        if (areAllPartitionsCached(cachedPartitions, creatingPartitionNames)) {
            return buildResponseWithLock(db, olapTable, txnState, partitionColNames, isTemp);
        }

        // Build request key for deduplicating identical concurrent requests
        String requestKey = buildPartitionRequestKey(tableId, creatingPartitionNames);
        int timeoutMs = getPartitionRequestTimeout(request);

        // Use CompletableFuture to deduplicate identical concurrent requests
        // Only the first request will execute creation, others will wait for the result
        AtomicBoolean isCreator = new AtomicBoolean(false);
        CompletableFuture<TCreatePartitionResult> future = PENDING_PARTITION_REQUESTS.computeIfAbsent(requestKey, k -> {
            isCreator.set(true);
            return new CompletableFuture<>();
        });

        if (!isCreator.get()) {
            // This is a duplicate request - wait for the first request to complete
            try {
                return future.get(timeoutMs, TimeUnit.MILLISECONDS);
            } catch (TimeoutException e) {
                LOG.warn("Timeout waiting for partition creation: {}", requestKey);
                return buildErrorResult("Timeout waiting for partition creation");
            } catch (ExecutionException e) {
                LOG.warn("Partition creation failed for requestKey: {}", requestKey, e.getCause());
                return buildErrorResult("Partition creation failed: " + e.getCause().getMessage());
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                LOG.warn("Partition creation interrupted for requestKey: {}", requestKey);
                return buildErrorResult("Partition creation interrupted");
            }
        }

        // This is the first request - execute partition creation
        try {
            TCreatePartitionResult result = doCreatePartitionWithLock(state, db, olapTable, txnState,
                    request.getTxn_id(), addPartitionClause, creatingPartitionNames, cachedPartitions,
                    partitionColNames, isTemp);
            future.complete(result);
            return result;
        } catch (Exception e) {
            LOG.warn("Failed to create partitions for requestKey: {}", requestKey, e);
            txnState.setIsCreatePartitionFailed(true);
            TCreatePartitionResult errResult = buildErrorResult(
                    String.format("automatic create partition failed. error:%s", e.getMessage()));
            future.complete(errResult);
            return errResult;
        } finally {
            PENDING_PARTITION_REQUESTS.remove(requestKey);
        }
    }'''

if old_part in content:
    content = content.replace(old_part, new_part)
    with open(file_path, 'w') as f:
        f.write(content)
    print("Method part updated successfully")
else:
    print("ERROR: Could not find the old part to replace")
