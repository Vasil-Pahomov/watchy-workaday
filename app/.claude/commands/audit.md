---
description: Run the reviewer agent over the current working tree, with no coder involved
argument-hint: [optional: files or area to focus on]
---

Spawn the **reviewer** agent (`subagent_type: "reviewer"`, foreground) over the
current state of the working tree.

Focus: **$ARGUMENTS** — if that is empty, review everything that differs from
`HEAD`, or the whole app if there is no commit yet.

There is no coder handoff report to check against, so tell the reviewer to audit
the code directly against `CLAUDE.md` and `docs/review-checklist.md`, and to run
`./gradlew test` itself.

Relay the verdict and the findings to the user — they cannot see subagent
output. Do not fix anything yourself: if the findings should be acted on, offer
to run `/implement` with them.
