# The two-agent workflow

Development runs as a loop between two subagents with deliberately asymmetric
powers.

```
  you ──/feature "…"──▶ orchestrator
                             │
                    ┌────────▼────────┐
                    │  coder          │  writes code + tests
                    │  can edit       │  runs pio test / pio run
                    └────────┬────────┘
                             │  git diff HEAD
                    ┌────────▼────────┐
                    │  reviewer       │  audits against the 5 laws
                    │  CANNOT edit    │  verifies independently
                    └────────┬────────┘
                             │
              APPROVE ◀──────┴──────▶ CHANGES REQUIRED
                 │                          │
              done                   back to the same coder
                                     (max 3 rounds)
```

## Why the reviewer cannot edit

The reviewer's tool list has no `Write` or `Edit` — this is the point of the
design, not an oversight.

An agent that can fix what it finds tends to fix the easy things quietly and stop
looking. Taking away the ability to edit forces the finding to be *articulated*:
file, line, consequence. That is what makes it reviewable by you, arguable by the
coder, and useful later.

It also keeps the two roles honestly independent. The reviewer re-runs
`pio test -e native` itself rather than trusting the coder's report, which is
what catches the most common agent failure mode — claiming a green gate that was
never run.

## Commands

| Command | Does |
|---|---|
| `/feature <what to build>` | Full loop: coder → reviewer → coder, until APPROVE or 3 rounds |
| `/review [files]` | Audit only. No fixes applied. Defaults to the working-tree diff |
| `/power-audit` | Whole-firmware energy audit, P1-focused, ignores the diff |

Either agent can also be invoked directly by name in normal conversation — "have
the reviewer look at the sleep path".

## What each agent is optimised for

**coder** (`.claude/agents/coder.md`) — implements changes. Its standing
instructions push it to decide *where* code belongs before writing it
(decisions → `core/`, effects → `board/`), to write tests in the same change
rather than promising them later, and to self-audit its own diff against the
energy and reliability questions before reporting.

**reviewer** (`.claude/agents/reviewer.md`) — audits. Priorities are ordered by
device impact: hangs first, energy second, missing tests third, v2.0 pin
correctness fourth. It is told to verify with real commands, to cite `file:line`,
to state consequences in device terms ("adds 100 µA across sleep", not "not best
practice"), and explicitly **not** to invent findings to look thorough.

Both inherit `CLAUDE.md`, and both work from `docs/review-checklist.md` — one
shared rubric, so the coder can pre-empt what the reviewer will look for.

## Loop control

- Maximum **3** review rounds, then it escalates to you.
- Findings go back to the **same** coder agent via `SendMessage`, so it keeps its
  context instead of re-deriving the change from scratch.
- The coder is explicitly allowed to **push back** on a finding with evidence.
  A finding disputed twice and re-raised twice stops the loop and comes to you —
  that pattern means it is a design decision, not a defect.
- The loop stops early and asks you before anything that changes the shape of the
  power budget: a new hardware dependency, a radio wake, a new periodic wake.

## Working without the loop

The loop is for changes of real substance. For a typo or a doc fix, just make the
edit. For an existing-code question, ask directly.

The one thing that should never be skipped, loop or no loop:

```bash
pio test -e native
```
