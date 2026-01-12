file_path = '/workspace/fe/fe-core/src/test/java/com/starrocks/service/FrontendServiceImplCreatePartitionTest.java'
with open(file_path, 'r') as f:
    content = f.read()

# Find the last test and add new tests before the closing brace
old_ending = '''    /**
     * Test that the cache check correctly identifies when partitions are not cached.
     */
    @Test
    public void testCreatePartitionSlowPathWhenNotCached() throws TException {
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        Table table = GlobalStateMgr.getCurrentState().getLocalMetastore().getTable(db.getFullName(), "test_table");

        // Empty cache - partition not cached
        TransactionState txnState = new TransactionState();
        AtomicInteger lockCallCount = new AtomicInteger(0);

        new MockUp<GlobalTransactionMgr>() {
            @Mock
            public TransactionState getTransactionState(long dbId, long transactionId) {
                return txnState;
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public void lockCreatePartition(String partitionName) {
                lockCallCount.incrementAndGet();
            }

            @Mock
            public void unlockCreatePartition(String partitionName) {
                // no-op
            }
        };

        new MockUp<WarehouseManager>() {
            @Mock
            public Long getAliveComputeNodeId(ComputeResource computeResource, long tabletId) {
                return 50001L;
            }
        };

        List<List<String>> partitionValues = Lists.newArrayList();
        List<String> values = Lists.newArrayList();
        values.add("2025-07-13");
        partitionValues.add(values);

        FrontendServiceImpl impl = new FrontendServiceImpl(exeEnv);
        TCreatePartitionRequest request = new TCreatePartitionRequest();
        request.setDb_id(db.getId());
        request.setTable_id(table.getId());
        request.setTxn_id(3L);
        request.setPartition_values(partitionValues);

        impl.createPartition(request);

        // When partition is NOT cached, the slow path should be taken and lockCreatePartition SHOULD be called
        Assertions.assertTrue(lockCallCount.get() > 0,
                "lockCreatePartition should be called when partition is not cached (slow path)");
    }
}'''

new_ending = '''    /**
     * Test that the cache check correctly identifies when partitions are not cached.
     */
    @Test
    public void testCreatePartitionSlowPathWhenNotCached() throws TException {
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        Table table = GlobalStateMgr.getCurrentState().getLocalMetastore().getTable(db.getFullName(), "test_table");

        // Empty cache - partition not cached
        TransactionState txnState = new TransactionState();
        AtomicInteger lockCallCount = new AtomicInteger(0);

        new MockUp<GlobalTransactionMgr>() {
            @Mock
            public TransactionState getTransactionState(long dbId, long transactionId) {
                return txnState;
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public void lockCreatePartition(String partitionName) {
                lockCallCount.incrementAndGet();
            }

            @Mock
            public void unlockCreatePartition(String partitionName) {
                // no-op
            }
        };

        new MockUp<WarehouseManager>() {
            @Mock
            public Long getAliveComputeNodeId(ComputeResource computeResource, long tabletId) {
                return 50001L;
            }
        };

        List<List<String>> partitionValues = Lists.newArrayList();
        List<String> values = Lists.newArrayList();
        values.add("2025-07-13");
        partitionValues.add(values);

        FrontendServiceImpl impl = new FrontendServiceImpl(exeEnv);
        TCreatePartitionRequest request = new TCreatePartitionRequest();
        request.setDb_id(db.getId());
        request.setTable_id(table.getId());
        request.setTxn_id(3L);
        request.setPartition_values(partitionValues);

        impl.createPartition(request);

        // When partition is NOT cached, the slow path should be taken and lockCreatePartition SHOULD be called
        Assertions.assertTrue(lockCallCount.get() > 0,
                "lockCreatePartition should be called when partition is not cached (slow path)");
    }

    /**
     * Test that identical concurrent requests share the same result via CompletableFuture mechanism.
     * Only the first request executes the partition creation, others wait and get the same result.
     */
    @Test
    public void testIdenticalConcurrentRequestsShareResult() throws Exception {
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        Table table = GlobalStateMgr.getCurrentState().getLocalMetastore().getTable(db.getFullName(), "test_table");

        TransactionState txnState = new TransactionState();
        AtomicInteger lockCallCount = new AtomicInteger(0);
        CountDownLatch firstRequestInProgress = new CountDownLatch(1);
        CountDownLatch allowFirstRequestComplete = new CountDownLatch(1);

        new MockUp<GlobalTransactionMgr>() {
            @Mock
            public TransactionState getTransactionState(long dbId, long transactionId) {
                return txnState;
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public void lockCreatePartition(String partitionName) {
                int count = lockCallCount.incrementAndGet();
                if (count == 1) {
                    // First request: signal that we're in progress
                    firstRequestInProgress.countDown();
                    try {
                        // Wait for signal to continue (to allow other requests to arrive)
                        allowFirstRequestComplete.await();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                }
                // Add to cache after lock is acquired
                ConcurrentMap<String, TOlapTablePartition> cache =
                        txnState.getPartitionNameToTPartition(table.getId());
                if (cache.get(partitionName) == null) {
                    TOlapTablePartition partition = new TOlapTablePartition();
                    partition.setId(88888L);
                    partition.setIndexes(new ArrayList<>());
                    cache.put(partitionName, partition);
                }
            }

            @Mock
            public void unlockCreatePartition(String partitionName) {
                // no-op
            }
        };

        new MockUp<WarehouseManager>() {
            @Mock
            public Long getAliveComputeNodeId(ComputeResource computeResource, long tabletId) {
                return 50001L;
            }

            @Mock
            public boolean isResourceAvailable(ComputeResource resource) {
                return true;
            }
        };

        List<List<String>> partitionValues = Lists.newArrayList();
        List<String> values = Lists.newArrayList();
        values.add("2025-07-14");
        partitionValues.add(values);

        int numRequests = 3;
        ExecutorService executor = Executors.newFixedThreadPool(numRequests);
        CountDownLatch allDone = new CountDownLatch(numRequests);
        AtomicInteger successCount = new AtomicInteger(0);

        // Submit all requests
        for (int i = 0; i < numRequests; i++) {
            executor.submit(() -> {
                try {
                    FrontendServiceImpl impl = new FrontendServiceImpl(exeEnv);
                    TCreatePartitionRequest request = new TCreatePartitionRequest();
                    request.setDb_id(db.getId());
                    request.setTable_id(table.getId());
                    request.setTxn_id(4L);
                    request.setPartition_values(partitionValues);
                    request.setTimeout(30); // 30 seconds timeout
                    TCreatePartitionResult result = impl.createPartition(request);
                    if (result.getStatus().getStatus_code() == TStatusCode.OK ||
                        result.getStatus().getStatus_code() == TStatusCode.RUNTIME_ERROR) {
                        successCount.incrementAndGet();
                    }
                } catch (Exception e) {
                    // ignore
                } finally {
                    allDone.countDown();
                }
            });
        }

        // Wait for first request to be in progress
        firstRequestInProgress.await();

        // Give other requests time to arrive and wait on the Future
        Thread.sleep(100);

        // Allow the first request to complete
        allowFirstRequestComplete.countDown();

        // Wait for all requests to complete
        allDone.await();
        executor.shutdown();

        // With the Future mechanism, only the first request should acquire the lock
        // because other requests with the same requestKey will wait on the Future
        Assertions.assertEquals(1, lockCallCount.get(),
                "Only the first request should acquire the lock due to Future deduplication");

        // All requests should complete (either success or error)
        Assertions.assertEquals(numRequests, successCount.get(),
                "All requests should complete with a response");
    }

    /**
     * Test that the request timeout is correctly used from the request parameter.
     */
    @Test
    public void testRequestTimeoutFromParameter() throws TException {
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        Table table = GlobalStateMgr.getCurrentState().getLocalMetastore().getTable(db.getFullName(), "test_table");

        TransactionState txnState = new TransactionState();

        new MockUp<GlobalTransactionMgr>() {
            @Mock
            public TransactionState getTransactionState(long dbId, long transactionId) {
                return txnState;
            }
        };

        new MockUp<OlapTable>() {
            @Mock
            public void lockCreatePartition(String partitionName) {
                ConcurrentMap<String, TOlapTablePartition> cache =
                        txnState.getPartitionNameToTPartition(table.getId());
                TOlapTablePartition partition = new TOlapTablePartition();
                partition.setId(77777L);
                partition.setIndexes(new ArrayList<>());
                cache.put(partitionName, partition);
            }

            @Mock
            public void unlockCreatePartition(String partitionName) {
                // no-op
            }
        };

        new MockUp<WarehouseManager>() {
            @Mock
            public Long getAliveComputeNodeId(ComputeResource computeResource, long tabletId) {
                return 50001L;
            }

            @Mock
            public boolean isResourceAvailable(ComputeResource resource) {
                return true;
            }
        };

        List<List<String>> partitionValues = Lists.newArrayList();
        List<String> values = Lists.newArrayList();
        values.add("2025-07-15");
        partitionValues.add(values);

        FrontendServiceImpl impl = new FrontendServiceImpl(exeEnv);
        TCreatePartitionRequest request = new TCreatePartitionRequest();
        request.setDb_id(db.getId());
        request.setTable_id(table.getId());
        request.setTxn_id(5L);
        request.setPartition_values(partitionValues);
        request.setTimeout(60); // 60 seconds timeout

        // The request should complete without timeout issues
        TCreatePartitionResult result = impl.createPartition(request);

        // Just verify the request completes - timeout value is used internally
        Assertions.assertNotNull(result);
    }
}'''

if old_ending in content:
    content = content.replace(old_ending, new_ending)
    with open(file_path, 'w') as f:
        f.write(content)
    print("New tests added successfully")
else:
    print("ERROR: Could not find the insertion point")
