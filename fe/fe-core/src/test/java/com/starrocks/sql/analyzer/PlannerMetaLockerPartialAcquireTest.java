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

package com.starrocks.sql.analyzer;

import com.starrocks.common.ErrorReportException;
import com.starrocks.common.util.concurrent.lock.LockException;
import com.starrocks.common.util.concurrent.lock.LockManager;
import com.starrocks.common.util.concurrent.lock.LockType;
import com.starrocks.common.util.concurrent.lock.Locker;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.ast.StatementBase;
import com.starrocks.sql.plan.PlanTestBase;
import com.starrocks.utframe.UtFrameUtils;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.concurrent.TimeUnit;

/**
 * Regression test for the bug where a partial-acquire failure inside
 * {@link PlannerMetaLocker#lock()} caused the mirroring {@code unlock()} in
 * {@code StatementPlanner.plan}'s finally to throw
 * {@code IllegalMonitorStateException("Attempt to unlock lock, not locked by current locker")}.
 *
 * <p>The fix tracks {@code heldEntries} so {@code unlock()} only releases what
 * {@code lock()}/{@code tryLock()} actually acquired.
 */
public class PlannerMetaLockerPartialAcquireTest extends PlanTestBase {

    private LockManager originalLockManager;

    @AfterEach
    public void restoreLockManager() {
        if (originalLockManager != null) {
            GlobalStateMgr.getCurrentState().setLockManager(originalLockManager);
            originalLockManager = null;
        } else {
            GlobalStateMgr.getCurrentState().setLockManager(new LockManager());
        }
    }

    @Test
    public void testUnlockIsNoOpAfterLockFailure() throws Exception {
        // Pick a statement that touches a single table so PlannerMetaLocker has exactly one entry.
        String sql = "select * from t0";
        StatementBase stmt = UtFrameUtils.parseStmtWithNewParser(sql, connectContext);
        PlannerMetaLocker plannerMetaLocker = new PlannerMetaLocker(connectContext, stmt);

        originalLockManager = GlobalStateMgr.getCurrentState().getLockManager();
        // Throw on the very first inner lock call (the db IS) so lock() rolls back to a clean state.
        GlobalStateMgr.getCurrentState().setLockManager(new FailingLockManager(0));

        Assertions.assertThrows(ErrorReportException.class, plannerMetaLocker::lock);

        // The bug: without heldEntries bookkeeping, this throws
        // IllegalMonitorStateException("Attempt to unlock lock, not locked by current locker").
        // With the fix: heldEntries is empty, so unlock() is a no-op.
        Assertions.assertDoesNotThrow(plannerMetaLocker::unlock);
    }

    @Test
    public void testCloseIsNoOpAfterLockFailure() throws Exception {
        String sql = "select * from t0";
        StatementBase stmt = UtFrameUtils.parseStmtWithNewParser(sql, connectContext);
        PlannerMetaLocker plannerMetaLocker = new PlannerMetaLocker(connectContext, stmt);

        originalLockManager = GlobalStateMgr.getCurrentState().getLockManager();
        GlobalStateMgr.getCurrentState().setLockManager(new FailingLockManager(0));

        Assertions.assertThrows(ErrorReportException.class, plannerMetaLocker::lock);
        // try-with-resources path: close() must not mask the original exception with an
        // IllegalMonitorStateException from unlock().
        Assertions.assertDoesNotThrow(plannerMetaLocker::close);
    }

    @Test
    public void testUnlockBeforeLockIsNoOp() throws Exception {
        String sql = "select * from t0";
        StatementBase stmt = UtFrameUtils.parseStmtWithNewParser(sql, connectContext);
        PlannerMetaLocker plannerMetaLocker = new PlannerMetaLocker(connectContext, stmt);

        // No lock() call. unlock() must not try to release anything.
        Assertions.assertDoesNotThrow(plannerMetaLocker::unlock);
    }

    @Test
    public void testTryLockFailureLeavesUnlockAsNoOp() throws Exception {
        String sql = "select * from t0";
        StatementBase stmt = UtFrameUtils.parseStmtWithNewParser(sql, connectContext);
        PlannerMetaLocker plannerMetaLocker = new PlannerMetaLocker(connectContext, stmt);

        originalLockManager = GlobalStateMgr.getCurrentState().getLockManager();
        // Make every tryLock-style timeout fail (callsBeforeFailure=0 throws on first call;
        // tryLockTablesWithIntensiveDbLock returns false on the IS LockException).
        GlobalStateMgr.getCurrentState().setLockManager(new FailingLockManager(0));

        Assertions.assertFalse(plannerMetaLocker.tryLock(1, TimeUnit.MILLISECONDS));
        Assertions.assertDoesNotThrow(plannerMetaLocker::unlock);
    }

    /**
     * LockManager that throws {@link LockException} on the Nth call to {@code lock}. Used to
     * deterministically trigger the partial-acquire path under test.
     */
    private static class FailingLockManager extends LockManager {
        private final int callsBeforeFailure;
        private int callCount;

        FailingLockManager(int callsBeforeFailure) {
            this.callsBeforeFailure = callsBeforeFailure;
        }

        @Override
        public void lock(long rid, Locker locker, LockType lockType, long timeout) throws LockException {
            if (callCount++ >= callsBeforeFailure) {
                throw new LockException("injected failure for rid " + rid);
            }
            super.lock(rid, locker, lockType, timeout);
        }
    }
}
