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

package com.starrocks.common.lock;

import com.google.common.collect.ImmutableList;
import com.starrocks.common.Config;
import com.starrocks.common.ErrorReportException;
import com.starrocks.common.util.concurrent.lock.LockException;
import com.starrocks.common.util.concurrent.lock.LockManager;
import com.starrocks.common.util.concurrent.lock.LockType;
import com.starrocks.common.util.concurrent.lock.Locker;
import com.starrocks.server.GlobalStateMgr;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.HashSet;
import java.util.Set;

/**
 * Verifies that {@link Locker#lockTablesWithIntensiveDbLock} and
 * {@link Locker#lockTableWithIntensiveDbLock} roll back any partially-acquired locks
 * if an inner lock() call throws. Without rollback, the caller's mirroring unLock would
 * later trip "Attempt to unlock lock, not locked by current locker".
 */
public class LockerPartialAcquireTest {
    private boolean origLockManagerEnabled;

    @BeforeEach
    public void setUp() {
        origLockManagerEnabled = Config.lock_manager_enabled;
        Config.lock_manager_enabled = true;
    }

    @AfterEach
    public void tearDown() {
        Config.lock_manager_enabled = origLockManagerEnabled;
        GlobalStateMgr.getCurrentState().setLockManager(new LockManager());
    }

    @Test
    public void testLockTablesRollsBackWhenTableLockFails() {
        // Succeeds on db IS + first table, fails on second table.
        FailingLockManager fm = new FailingLockManager(2);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        Assertions.assertThrows(ErrorReportException.class, () ->
                locker.lockTablesWithIntensiveDbLock(dbId, ImmutableList.of(1L, 2L, 3L), LockType.READ));

        Assertions.assertTrue(fm.acquiredRids.isEmpty(),
                "expected no rids held after rollback, but had: " + fm.acquiredRids);
    }

    @Test
    public void testLockTablesRollsBackWhenDbLockFails() {
        // Fails on the very first inner call (the db IS).
        FailingLockManager fm = new FailingLockManager(0);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        Assertions.assertThrows(ErrorReportException.class, () ->
                locker.lockTablesWithIntensiveDbLock(dbId, ImmutableList.of(1L, 2L), LockType.READ));

        Assertions.assertTrue(fm.acquiredRids.isEmpty());
    }

    @Test
    public void testLockTablesSuccessPathStillHoldsAllLocks() {
        FailingLockManager fm = new FailingLockManager(Integer.MAX_VALUE);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        locker.lockTablesWithIntensiveDbLock(dbId, ImmutableList.of(1L, 2L), LockType.READ);

        // db IS + 2 table READ locks = 3 rids held.
        Assertions.assertEquals(3, fm.acquiredRids.size(), "expected db + 2 tables held");

        locker.unLockTablesWithIntensiveDbLock(dbId, ImmutableList.of(1L, 2L), LockType.READ);
        Assertions.assertTrue(fm.acquiredRids.isEmpty());
    }

    @Test
    public void testLockTableRollsBackWhenTableLockFails() {
        // db IS ok, table fails.
        FailingLockManager fm = new FailingLockManager(1);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        Assertions.assertThrows(ErrorReportException.class, () ->
                locker.lockTableWithIntensiveDbLock(dbId, 5L, LockType.READ));

        Assertions.assertTrue(fm.acquiredRids.isEmpty(),
                "expected no rids held after rollback, but had: " + fm.acquiredRids);
    }

    @Test
    public void testLockTableRollsBackWhenDbLockFails() {
        FailingLockManager fm = new FailingLockManager(0);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        Assertions.assertThrows(ErrorReportException.class, () ->
                locker.lockTableWithIntensiveDbLock(dbId, 5L, LockType.READ));

        Assertions.assertTrue(fm.acquiredRids.isEmpty());
    }

    @Test
    public void testLockTablesWriteIntentRollback() {
        // Same scenario as readers, but with WRITE / IX. The rollback path must release the IX
        // (not an IS that was never taken), or the lock state stays inconsistent.
        FailingLockManager fm = new FailingLockManager(2);
        GlobalStateMgr.getCurrentState().setLockManager(fm);

        Locker locker = new Locker();
        long dbId = 100L;
        Assertions.assertThrows(ErrorReportException.class, () ->
                locker.lockTablesWithIntensiveDbLock(dbId, ImmutableList.of(1L, 2L, 3L), LockType.WRITE));

        Assertions.assertTrue(fm.acquiredRids.isEmpty());
    }

    /**
     * LockManager wrapper that delegates to a real manager but throws on the Nth lock() call
     * and tracks which rids the current invocation has acquired so the test can assert rollback.
     * Single-threaded use only.
     */
    private static class FailingLockManager extends LockManager {
        private final int callsBeforeFailure;
        private int callCount;
        final Set<Long> acquiredRids = new HashSet<>();

        FailingLockManager(int callsBeforeFailure) {
            this.callsBeforeFailure = callsBeforeFailure;
        }

        @Override
        public void lock(long rid, Locker locker, LockType lockType, long timeout) throws LockException {
            if (callCount++ >= callsBeforeFailure) {
                throw new LockException("injected failure for rid " + rid);
            }
            super.lock(rid, locker, lockType, timeout);
            acquiredRids.add(rid);
        }

        @Override
        public void release(long rid, Locker locker, LockType lockType) throws LockException {
            super.release(rid, locker, lockType);
            acquiredRids.remove(rid);
        }
    }
}
