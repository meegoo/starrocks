---
name: rca
description: Use when the user asks to write a Root Cause Analysis / RCA / postmortem for a production incident, or asks to convert an existing diagnostic write-up into RCA form. Triggers on phrases like "write an RCA", "RCA for <incident>", "postmortem", "事故复盘", "根因分析". Applies whether the user wants Markdown or HTML output, and regardless of language (English/Chinese).
---

# Root Cause Analysis Template

A stable RCA template distilled from production incident write-ups (CG TurboTax MV refresh, CHOP auto-statistics, Metica OOM, Mailchimp publish stall, Conductor FE restart, Finaloop publish-log-version deadlock, and similar). Use it to produce RCAs that are field-aligned and reviewable.

## When to invoke this skill

- User says "write an RCA", "RCA for X", "postmortem", "事故复盘", "根因分析", "复盘文档".
- User asks to convert an existing diagnostic chat into RCA form.
- User explicitly references this template ("use the Slack RCA template", "按 RCA 模版").

If the user has not yet decided on output format (Markdown vs HTML, English vs Chinese), ask once via `AskUserQuestion`. Default to Markdown if the user does not have a preference.

## Hard rules

- **Verdict goes first, one sentence.** It must answer "is this customer workload or internal?" in the first clause. Everything else is supporting evidence.
- **Evidence uses concrete IDs.** Never "that table / that tablet". Always `db id / table id / tablet id / txn id / partition id / BE IP`. If the source data does not have an ID, mark it `<TBD>` rather than fabricate.
- **Logs are minimal and labeled.** Quote only the smallest reproducible slice. Tag each block with the log source (`fe.warn.log`, `be.WARNING`, audit log, client message), and ideally a timestamp.
- **Recommended actions are ordered, not a menu.** Step 1 fixes the root cause. Subsequent steps are mitigations or long-term hardening. Do not produce a flat list of parallel options.
- **Long-term hardening is tightly scoped.** Step 3 should be the one or two items that directly prevent recurrence of *this* incident — not a wishlist of refactors that occurred to you while reading the code.
- **Do not fabricate.** PR numbers, JIRA tickets, version numbers, and timestamps that you do not know go in as `<TBD>` placeholders. Never invent them.
- **Match scope to evidence.** If you only have one incident, do not generalize to "all customers". If you have field logs but no fix PR, leave the status as `<TBD>`.
- **No personal attribution in technical sections.** Quoting individuals by name in Evidence chain, Causal chain, or Why X failed reads as blame and dates badly. Engineers' names belong at most in the header metadata or the incident timeline.
- **Do not coin operator-deliverables the company does not actually offer.** If "hot patch" is not a standard release path, describe the fix as "ships in version X" / "upgrade to version X" instead of "hotfix" or "hot patch". Same caution for "force-cancel", "metadata edit", etc. — only mention them if they are real, documented procedures.

## Template structure

Use these section numbers and order. Two sections (Stakeholders, Verification) are **optional** — include them when the audience explicitly needs them, omit when the RCA is for internal engineering review.

```markdown
# Root Cause Analysis — <incident short name>

**Cluster:** <cluster name> (<cluster id>)
**Version:** <e.g. 3.5.14-ee shared-data>
**Time window:** <UTC window, e.g. 2026-05-11 13:35–13:45 UTC>
**Severity / Ticket:** <S1/S2/S3, Zendesk #xxx, JIRA POST-xxxx>
**Author / Date:** <author, date>

---

## 1. Verdict
> One sentence. State the root cause, the customer-visible impact, and whether it is customer workload.
> If recovery requires anything destructive or non-standard (DROP TABLE FORCE, manual metadata edit,
> upgrade), put that in the verdict too — operators read this first.
> Example: "Not user workload; an internal auto-statistics loop combined with one corrupted S3 meta file
> pinned the AutoStatistic daemon thread into an infinite loop."

---

## 2. Evidence chain
Numbered, each line one fact + one short interpretation:
1. Cluster baseline state (CPU/Mem/Disk, QPS, whether under user load)
2. Hot / failing objects with concrete IDs (db / table / tablet / txn / partition)
3. Trigger cadence (every few seconds, per minute, etc.)
4. Key log lines (smallest reproducible slice, tagged with source and timestamp)
5. Resource / thread-pool state (saturated? reject policy fired?)
6. (Optional) audit-log shape if user pipeline matters

Writing rule: after each piece of evidence, one short sentence on "what this tells us".
Do not paste logs without interpretation. State technical facts directly; do not attribute them
to specific engineers.

---

## 3. Causal chain (internal anomaly → customer-visible symptom)
Numbered chain that walks from the root anomaly to what the customer sees:
1. <root anomaly occurs>
2. <internal component A reacts>  (why and how)
3. <internal component B is dragged in>  (resource saturation, retry storm, …)
4. <customer sees X>  (query timeout / reject / lag spike / alert)

Each step is one sentence; aim for 6–10 steps total. The last step is a customer-visible
symptom, not an internal state.

---

## 4. Contributing factors
Bullet list of things that made the situation worse or harder to diagnose, but are not the root cause:
- Schedule too aggressive (e.g. refresh interval too short)
- Grain mismatch (base table hourly, MV daily)
- Disabled fallback (e.g. `enable_spill = false`)
- Simultaneous external incident (e.g. AWS outage) that misled triage

Keep this to technical/process factors. Pre-incident customer guidance, sales promises, and
similar interpersonal items belong in a separate retrospective, not in the RCA.

---

## 5. Why X failed (optional)
Explain why "standard safety nets" did not catch this:
- Resource Group quotas — why they did not contain the load
- Spill — why it did not engage
- Alerts — why the dashboard looked healthy
- Retry / idempotency — why repeated attempts did not converge

Helps prevent others from re-trying the same dead-end remediation.

---

## 6. Recommended actions (priority-ordered, MUST be ordered)

### Step 1 — <highest priority, usually fix root cause>
```sql
-- Concrete SQL / config / patch command, or the version to upgrade to
```
Expected effect: <e.g. score drops below 100 within 30 min>

### Step 2 — <next: customer/operator workaround if the root-cause fix needs a release>
```
Concrete command sequence
```

### Step 3 — <one tightly-scoped long-term item that directly prevents this incident>

### Verification (optional — include when the success criterion is non-obvious)
- Which metric should return to which value
- Which log line should stop appearing
- Expected timescale (minutes / hours)

---

## 7. Stakeholder communication (optional — include when there is a customer-facing audience)
One neutral paragraph a TAM/CSM can forward as-is to the customer. Cover:
- Was it customer workload?
- Is there data loss?
- What does the customer need to do (upgrade / pause behavior / nothing)?
- Concrete fix version and ETA.

Omit this section entirely for internal-engineering RCAs.

---

## 8. Fix status / Follow-ups
- Existing PR / target release (link + version, e.g. v4.1.1)
- JIRA: POST-xxxx
- Backport scope (which branches?)
- Owners for outstanding action items

---

## 9. Sources
- Admin Console: <link>
- Grafana dashboard: <link, with cluster variable>
- Zendesk: <#ticket>
- JIRA: <POST-xxxx>
- Related PR: <github link>
- Code references: <file:line> entries the analysis depended on
- Historical Slack thread: <permalink>
```

If §7 is omitted, renumber the remaining sections so the document goes 1–8.

## Writing habits (from production-quality RCAs)

- **Lead with verdict.** Operators and reviewers read §1 first; everything else is supporting material.
- **Speak in IDs.** `partition 40571175 / tablet 40571453 / BE 10.68.170.85` lets the next responder reproduce; "that table" does not.
- **Quote, don't dump.** A 3-line log excerpt with one line of interpretation beats 30 lines of raw output.
- **Order actions, never enumerate.** A flat list of "you could do A, B, or C" pushes the decision onto the reader. Pick the order.
- **Be honest about scope.** If you have one cluster's logs, do not generalize. If you do not know the fix version, say `<TBD>`.
- **Don't editorialize about customer choices.** If the customer accepted a destructive recovery path, state it as a fact in the timeline; do not wrap it in a "data-loss warning" callout that reads as customer blame.
- **Drop sections when in doubt.** A tight 6-section RCA is more useful than a padded 9-section one. Stakeholder communication and Verification are the two most often safely omitted.

## Output format choice

- **Markdown** is the default; use the scaffold verbatim.
- **HTML** when the user asks for a customer-facing document. Use a clean style sheet: max-width ~980px, system font stack, monospace blocks for logs/code, color-coded callouts (yellow for verdict, blue for stakeholder paragraph if present, red for impact/deadlock). Keep the same section numbers and headings.
- **Chinese vs English**: write fully in one language; do not interleave. Section numbers and headings translate (`1. Verdict` → `1. 定性结论`, etc.) but keep code, log lines, and IDs verbatim.

## Filling the template from context

When the user has just walked through a diagnosis in the conversation, do not ask them to re-supply every field. Instead:

1. Reuse all concrete IDs and code citations already present in the chat.
2. For metadata you don't have (cluster id, version, Zendesk ticket, fix PR), insert `<TBD>` and surface a short list at the end: "Please fill in: cluster id, version, Zendesk ticket, fix PR."
3. If the diagnosis identified additional latent hazards (e.g. neighboring buggy functions), include them only if the user asked for it. Default RCA stays scoped to the current incident.
4. If you found the incident in a Slack thread, Zendesk ticket, or Jira issue, fetch it before writing — it usually carries the customer name, exact ALTER/query text, timeline, and recovery path that you cannot reconstruct from BE logs alone.

## Common mistakes to avoid

- Treating §6 like a wish list ("we should refactor X, Y, Z"). It is a remediation plan; long-term items go in Step 3 and Step 3 is usually one item.
- Skipping §5 when the bug bypassed obvious safety nets. Reviewers always ask "why didn't your retry / quota / alert save us?"; answer it.
- Putting a "recommended" tag on a destructive action (e.g. `DROP TABLE FORCE`) without spelling out the cost in §1.
- Burying the cancel-rejection / non-recoverable state inside §3. If the deadlock cannot be cleared by the standard CLI, that fact belongs in §1 (Verdict) too.
- Mixing post-incident hardening with the hotfix step. Step 1 ships in the next release; Step 3 ships in a future release. Don't bundle them.
- Describing the fix as a "hot patch" or "hotfix binary" when the actual delivery is a normal point release. Use the version number.
- Adding a stakeholder paragraph and verification subsection by default when the audience is internal engineering. They padding the doc and the reviewer will ask you to remove them.

## Closing checklist

Before handing the RCA off, verify:

- [ ] §1 Verdict answers "customer workload?" in the first clause
- [ ] §1 Verdict mentions any non-standard recovery requirement (FORCE, upgrade-only, manual edit)
- [ ] §2 Evidence chain has at least one concrete ID per line
- [ ] §3 Causal chain ends with a customer-visible symptom, not an internal state
- [ ] No engineer-by-name quotations in §2 / §3 / §5
- [ ] §6 Step 1 is the root-cause fix (typically: upgrade to version X), not a workaround
- [ ] §6 Step 3 is one or two items that directly prevent this incident
- [ ] §7 Stakeholders is present only if there is a customer-facing audience
- [ ] §8 PR/JIRA/version fields are either filled or `<TBD>` (never invented)
- [ ] §9 Sources includes file:line citations for every code path mentioned
- [ ] No mention of "hot patch" / "hotfix binary" unless that is a real, documented offering
