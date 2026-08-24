---
description: Implement a firmware change through the full coder → reviewer → coder loop until the reviewer approves.
argument-hint: <what to build, e.g. "daily step counter from the BMA423">
---

Run the two-agent development loop for: **$ARGUMENTS**

Orchestrate it yourself; do not implement the change directly.

## Round 1 — implement

Spawn the `coder` subagent with the task above. Give it enough context to start
cold: the request, the relevant files you already know about, and a reminder
that `CLAUDE.md`'s five laws are the acceptance criteria.

Wait for it to finish and read its report.

## Round 2 — review

Spawn the `reviewer` subagent. Tell it:
- what was requested,
- what the coder reports it changed,
- to review the working-tree diff (`git diff HEAD`).

Do **not** pass the coder's own self-assessment along as fact — the reviewer
verifies independently. Its job includes catching a coder that claimed green
tests it never ran.

## Round 3 — fix

If the verdict is **CHANGES REQUIRED**, send the findings back to the *same*
coder agent with `SendMessage` so it keeps its context, rather than spawning a
fresh one. Instruct it to fix or push back on each finding individually.

Then re-review. Prefer messaging the same reviewer so it remembers what it
already checked.

## Loop control

- Iterate until **APPROVE**, or at most **3** review rounds.
- If a finding is disputed twice by the coder and re-raised twice by the
  reviewer, stop looping and bring it to me with both arguments — that is a
  design decision, not a defect.
- Stop early and ask me if the change turns out to need a new hardware
  dependency, a radio wake, or anything that alters the power budget shape.

## Before you report back to me

Verify the gate yourself — do not take either agent's word for it:

```bash
pio test -e native
pio run -e watchy_v20
```

Then summarise: what was built, the review rounds it took, what the reviewer
caught, actual test and build results, and the energy impact. Flag anything left
unresolved.
