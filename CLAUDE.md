# Workaday — repository root

Two implementations of one system, in one repository:

- **`firmware/`** — the watch. PlatformIO / C++17 for the SQFMI Watchy v2.0.
- **`app/`** — the phone. Kotlin / Gradle Android companion.

They share no code and no build. They share a **contract** — and that is the
only reason they live in one repository: a change on the wire has to land on
both sides at once, and here it can.

## Read the laws for the side you are touching

Each side has its own `CLAUDE.md` carrying five non-negotiable project laws, its
own build commands, its own layout, and its own `.claude/` agents and
permissions. **This file repeats none of that** — it covers only what neither
side owns alone.

| Working on | Read first |
|---|---|
| the watch | `firmware/CLAUDE.md` |
| the phone | `app/CLAUDE.md` |
| anything on the wire | `PROTOCOL.md`, then both of the above |

The two sets of laws are deliberately different: the watch is spending
microamps against a 200 mAh cell, the phone is spending the user's battery and
fighting Android's background limits. Do not carry a rule across because it
sounded good on the other side.

## Root documents

| File | What it is |
|---|---|
| `PROTOCOL.md` | The watch ⇄ phone contract — UUIDs, byte layouts, timing, failure handling, golden vectors. Single source of truth; §8 names the one file on each side that mirrors it. |
| `BRINGUP.md` | The staged first-contact procedure, written while neither side had ever run against real hardware. Each stage adds exactly one thing that has never worked before. Do them in order. |
| `Backlog.txt` | Raw idea list spanning both sides. Not a plan and not a commitment — per-side work is tracked in each sub-project (e.g. `firmware/docs/backlog.md`). |

## A change on the wire is one commit, in this order

Both sides forbid inventing protocol material locally, so any protocol change is
inherently cross-cutting. This repository exists to make it atomic:

1. **Change `PROTOCOL.md` first** — including the §7 golden vectors, and
   `PROTO_VERSION` if a byte layout or a meaning moved.
2. **Update both mirrors in the same commit** — `firmware/src/core/protocol.*`
   and `app/core/…/protocol/WatchProtocol.kt`. Neither side may carry a UUID,
   field offset or protocol timeout anywhere else.
3. **Both gates pass before it is committed** — `pio test -e native` in
   `firmware/`, `./gradlew test` in `app/`. The golden-vector test on each side,
   not the prose, is what stops the implementations drifting.

Never leave one side ahead of the other in the history. Both sides are tested
only against fakes, so a desync does not fail a test — it surfaces on real
hardware as silence, which is the most expensive way to find it.

## Working in this repo

- **Start the session in the sub-directory you are working in** (`app/` or
  `firmware/`). The slash commands, subagents and tool permissions live in each
  sub-project's `.claude/`, and the build commands assume that working
  directory. A root session is for `PROTOCOL.md`, `BRINGUP.md`, and changes that
  span both sides.
- `git` sees the whole tree from anywhere inside it, so a diff taken in `app/`
  shows firmware changes too. That is intentional — it is how a cross-cutting
  change stays visible.
- **Nothing is shared but the document.** No shared build, no generated code
  crossing the boundary, no `common/` directory. C++17 and Kotlin do not meet.
