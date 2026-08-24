---
description: Run the coder → reviewer loop on a change until the reviewer passes it
argument-hint: <what to build or fix>
---

Run the two-agent loop described in `docs/workflow.md` for this task:

**$ARGUMENTS**

You are the orchestrator. You do not write the code and you do not review it —
you route between the two agents and you decide when to stop.

1. Read `CLAUDE.md` so you can judge the handoffs.
2. Spawn the **coder** agent (`subagent_type: "coder"`, foreground —
   `run_in_background: false`, since the review depends on its result) with the
   task above and any constraints from the conversation.
3. Spawn the **reviewer** agent (`subagent_type: "reviewer"`, foreground) with
   the task statement and the coder's full handoff report. Tell it explicitly
   that the report is a set of claims to verify against the code.
4. On `CHANGES REQUIRED`: send the findings back to the **same coder instance**
   with `SendMessage` — not a fresh `Agent` call — so it keeps the context of
   what it wrote and why. Then re-review with a fresh reviewer.
5. Stop when the reviewer returns `PASS`, or after **3 rounds**. Three rounds
   without convergence means the task or the laws are wrong, not the code — stop
   and bring it to the user with both positions stated.

Escalate to the user immediately, without burning a round, if:

- the change needs a fact that does not exist yet — a GATT UUID, the wire
  format, what the app is meant to do with the watch (see "Open items" in
  `CLAUDE.md`);
- the coder and reviewer disagree on a point of fact about Android platform
  behaviour that neither can settle from the docs in this repo;
- the right fix is to change a law.

Then report back: what changed, what the reviewer caught, what is still open.
Relay it yourself — the user does not see subagent output. Do not commit.
