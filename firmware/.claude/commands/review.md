---
description: Audit the current changes (or named files) with the reviewer agent. Read-only, no fixes applied.
argument-hint: [files or area to review; defaults to the working-tree diff]
---

Spawn the `reviewer` subagent to audit: **$ARGUMENTS**

If no argument was given, review the uncommitted working-tree diff
(`git diff HEAD`); if the tree is clean, review the most recent commit.

Pass the findings back to me verbatim, grouped by priority. Do **not** fix
anything and do not spawn the coder — this command is an audit only. If I want
the findings applied I will ask, or run `/feature` instead.
