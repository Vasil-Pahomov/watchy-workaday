# The two-agent workflow

Development on this app runs as a loop between two subagents with deliberately
different jobs and deliberately different powers.

| | coder | reviewer |
|---|---|---|
| Writes code | yes | **no** — has no Write/Edit tool |
| Runs the test suite | yes, before reporting | yes, independently |
| Decides a change is done | no | yes |
| Defined in | `.claude/agents/coder.md` | `.claude/agents/reviewer.md` |

The split is the point. The coder is invested in what it just wrote; a fresh
reviewer with no memory of the reasoning, no ability to "just fix it", and an
explicit checklist catches things the author cannot see. Taking away the
reviewer's edit tools is what keeps it reviewing instead of quietly rewriting.

## Running it

```
/implement <what to build or fix>
```

Runs the whole loop. Or drive it by hand from any session:

- spawn `coder` with the task,
- spawn `reviewer` with the task **and the coder's report**,
- on `CHANGES REQUIRED`, continue the *same* coder with `SendMessage` (not a new
  `Agent` call — a fresh coder loses why it made each choice and tends to
  re-argue settled ground),
- re-review with a fresh reviewer.

```
/audit [area]
```

Reviewer only, over the current working tree. Use it on code that arrived some
other way — your own edits, a session that skipped the loop, or a scaffold.

## The handoff contract

The coder ends with a fixed report: Task, Changes (`file:line`), Design
decisions, Law compliance (one concrete line per law), Tests (cases plus real
`./gradlew test` output), Risks and unverified claims.

The reviewer returns `PASS` or `CHANGES REQUIRED`, what it verified and how, and
numbered findings — each with a severity, a `file:line`, the law it violates, a
**concrete failure scenario**, and the required change.

Both formats are specified in the agent files. They exist so the loop converges:
a vague report produces a vague review, and a review without a failure scenario
produces an argument instead of a fix.

The reviewer treats the report as **claims to verify**, never as evidence. It
re-runs the tests itself. This matters more than it sounds — a stale or
selectively-quoted test result is the easiest way for a broken change to look
finished.

## Stopping rules

- Reviewer returns `PASS` → done. Nothing is committed automatically; the user
  commits.
- **3 rounds without a PASS** → stop and escalate. Three rounds means the task
  is underspecified or a law is wrong, and another round will not fix either.
- A missing fact — an undefined GATT UUID, an unspecified wire format, an
  unanswered "what should the app actually do here" — escalates **immediately**,
  without burning a round. See "Open items" in `CLAUDE.md`. An invented constant
  that looks deliberate is the single most expensive thing this loop can produce.
- A disagreement about Android platform behaviour that the repo docs cannot
  settle escalates too. Add the answer to `docs/background-execution.md` with its
  source so it is settled permanently.

## Keeping the loop honest

- **When a bug escapes the loop, fix the checklist, not just the bug.** Add the
  case to `docs/review-checklist.md` so it is caught by construction next time.
  A review process that never learns is theatre.
- **Watch for review inflation.** A reviewer that always finds something trains
  the coder to discount it. `PASS` on a clean change is a correct and expected
  outcome.
- **Watch for rubber-stamping too.** If several changes pass without the
  reviewer running `./gradlew test` or naming a file it opened, the "What I
  verified" section is doing no work — say so and re-run the audit.
- The laws in `CLAUDE.md` are the shared ground truth. Changing one is a
  deliberate act, done with the user, not a thing the loop decides on its own.

## Which model to use

Both agents inherit the session's model. If you want to split them — a cheaper
coder, a careful reviewer, or the reverse — set `model:` in the agent's
frontmatter, or pass `model` on the `Agent` call. Review is the step where
carelessness is expensive and the cost is one pass over a diff; spend there
before you spend on the coder.
