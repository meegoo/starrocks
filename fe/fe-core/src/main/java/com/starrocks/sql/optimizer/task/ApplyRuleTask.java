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

package com.starrocks.sql.optimizer.task;

import com.google.common.base.Stopwatch;
import com.google.common.collect.Lists;
import com.starrocks.common.Pair;
import com.starrocks.common.profile.Timer;
import com.starrocks.common.profile.Tracers;
import com.starrocks.sql.common.ErrorType;
import com.starrocks.sql.common.StarRocksPlannerException;
import com.starrocks.sql.optimizer.GroupExpression;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.OptimizerTraceUtil;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.rule.Binder;
import com.starrocks.sql.optimizer.rule.Rule;

import java.util.List;

/**
 * ApplyRuleTask firstly applies a rule, then
 * <p>
 * If the rule is transformation rule and isExplore is true:
 * We need to explore (apply logical rules)
 * <p>
 * If the rule is transformation rule and isExplore is false:
 * We need to optimize (apply logical & physical rules)
 * <p>
 * If the rule is implementation rule:
 * We directly enforce and cost the physical expression
 */

public class ApplyRuleTask extends OptimizerTask {
    private final GroupExpression groupExpression;
    private final Rule rule;
    private final boolean isExplore;

    ApplyRuleTask(TaskContext context, GroupExpression groupExpression, Rule rule, boolean isExplore) {
        super(context);
        this.groupExpression = groupExpression;
        this.rule = rule;
        this.isExplore = isExplore;
    }

    @Override
    public String toString() {
        return "ApplyRuleTask" + (this.isExplore ? "[explore]" : "") + " for groupExpression " + groupExpression +
                "\n rule " + rule;
    }

    @Override
    public void execute() {
        if (groupExpression.hasRuleExplored(rule) || groupExpression.isUnused()) {
            return;
        }
        // Apply rule and get all new OptExpressions
        final Pattern pattern = rule.getPattern();
        final OptimizerContext optimizerContext = context.getOptimizerContext();
        final Stopwatch ruleStopWatch = optimizerContext.getStopwatch(rule.type());
        final Binder binder = new Binder(optimizerContext, pattern, groupExpression, ruleStopWatch);
        final List<OptExpression> newExpressions = Lists.newArrayList();
        OptExpression extractExpr = binder.next();
        while (extractExpr != null) {
            // Check if the rule has exhausted or not to avoid optimization time exceeding the limit.:
            // 1. binder.next() may be infinite loop if something is wrong.
            // 2. rule.transform() may cost a lot of time.
            if (rule.exhausted(context.getOptimizerContext())) {
                OptimizerTraceUtil.logRuleExhausted(context.getOptimizerContext(), rule);
                break;
            }

            if (!rule.check(extractExpr, context.getOptimizerContext())) {
                extractExpr = binder.next();
                continue;
            }
            List<OptExpression> targetExpressions;
            OptimizerTraceUtil.logApplyRuleBefore(context.getOptimizerContext(), rule, extractExpr);
            try (Timer ignore = Tracers.watchScope(Tracers.Module.OPTIMIZER, rule.getClass().getSimpleName())) {
                targetExpressions = rule.transform(extractExpr, context.getOptimizerContext());
            } catch (StarRocksPlannerException e) {
                if (e.getType() == ErrorType.RULE_EXHAUSTED) {
                    break;
                } else {
                    throw e;
                }
            }

            newExpressions.addAll(targetExpressions);
            OptimizerTraceUtil.logApplyRuleAfter(targetExpressions);

            extractExpr = binder.next();
        }

        for (OptExpression expression : newExpressions) {
            // Insert new OptExpression to memo
            Pair<Boolean, GroupExpression> result = context.getOptimizerContext().getMemo().
                    copyIn(groupExpression.getGroup(), expression);

            // The group has been merged
            if (groupExpression.hasEmptyRootGroup()) {
                return;
            }

            GroupExpression newGroupExpression = result.second;

            // Add this rule into `appliedRules` to mark rules which have already been applied.
            {
                // new bitset should derive old bitset's info to track the lineage of applied rules.
                newGroupExpression.mergeAppliedRules(groupExpression.getAppliedRuleMasks());
                // new bitset add new rule which it's derived from.
                newGroupExpression.addNewAppliedRule(rule);
            }

            if (newGroupExpression.getOp().isLogical()) {
                // For logic newGroupExpression, optimize it
                pushTask(new OptimizeExpressionTask(context, newGroupExpression, isExplore));
            } else {
                // For physical newGroupExpression, enforce and cost it,
                // Optimize its inputs if needed
                pushTask(new EnforceAndCostTask(context, newGroupExpression));
            }
        }

        groupExpression.setRuleExplored(rule);
    }
}
