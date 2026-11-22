# Hailstone agent briefing

Hailstone is a C++20 and Vulkan client for EQEmu. It speaks the RoF2 protocol,
reads the player's own EverQuest installation and ships no game assets. The
client has its own game model, renderers, Lua interfaces and add-on host. The
same change often crosses a boundary between two of those systems. Identify
the owner of the behavior before choosing where to edit.

Read [repository instructions](../../AGENTS.md) and the
[contributing guide](../../CONTRIBUTING.md). The handbook adds source routes,
examples and verification steps to those rules. Its Markdown lives with the
code so a fresh checkout can be used by a person, a local coding tool or a
managed runner with the same starting context.

## Find the right page

| Task | Start here | Then check |
| --- | --- | --- |
| Start from an issue, resume a branch or review another contributor's work | [Shared workflow](workflow.md) | [Plan](../plan.md), [to-do list](../todo.md) and [known gaps](../unknowns.md) |
| Trace a symptom to its owning layer | [Codebase map](codebase.md) | [Code overview](../code-overview.md) and the linked implementation |
| Change a packet, command, timer or gameplay rule | [Compatibility contracts](contracts.md) | [Mechanics](../mechanics.md), [protocol](../protocol/session.md) and [worked recipes](recipes.md) |
| Update a window, key binding or persisted option | [Worked recipes](recipes.md) | [Developer UI](../dev-ui.md), [UI API](../ui-api.md) and [folder layout](../layout.md) |
| Extend the host API, write an add-on or support a server | [Compatibility contracts](contracts.md) | [Plugins and scripts](../plugins.md), [add-ons](../addons.md) and [server support](../../server/README.md) |
| Fix assets, rendering, audio or platform behavior | [Codebase map](codebase.md) | [Graphics](../graphics.md), [file formats](../formats/) and [validation](validation.md) |
| Configure a build, choose tests or report a skipped fixture | [Validation](validation.md) | [CI and local builds](../ci.md), [parser hardening](../hardening.md) and [mock server](../../tests/mock_server/README.md) |
| Prepare a draft PR or pick up work in a different tool | [Shared workflow](workflow.md#handoff) | [Validation](validation.md) and the exact branch, revision and diff |

Use the [glossary](glossary.md) for terms shared across protocol, UI, add-ons
and test reports.

## Decisions that matter early

The rules themselves live in [AGENTS.md](../../AGENTS.md). Each decision
below points to the section that states it.

1. **Identify the baseline.** "RoF2 is the baseline, not the ceiling" and
   "Compatible with stock EQEmu" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).
2. **Anchor each claim.** "No guessing" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).
   Gaps go in [known gaps](../unknowns.md) and the [to-do list](../todo.md).
3. **Keep ownership clear.** "Compatible with stock EQEmu" in
   [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most), and
   [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons).
   [Server support](../../server/README.md)
4. **Keep the project clean-room.** "Clean room", "Licence" and "No secrets
   and no real identifiers" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).
   [Contributor rules](../../CONTRIBUTING.md#clean-room-rule)
5. **Protect existing work.** [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow), and "The
   player's game folder is read-only" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).
   [Folder layout](../layout.md)
6. **Match checks to the claim.** A build, a test with local fixtures, a
   mock session and a live check answer different questions. Report them as
   [AGENTS.md, Build and test](../../AGENTS.md#build-and-test) says.
7. **Follow through on the interfaces.** UI packs, generated files and new
   sources: [AGENTS.md, Adding code](../../AGENTS.md#adding-code). Other compilers:
   [AGENTS.md, Build and test](../../AGENTS.md#build-and-test). Also check legacy callers for
   persistent state and extension points ([contracts](contracts.md)).

## Finish checklist

- Branch, base and other people's work: [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow).
- Build lists, generated files and tests: [AGENTS.md, Adding code](../../AGENTS.md#adding-code).
- Docs, known gaps and changelog: [AGENTS.md, Docs travel with the code](../../AGENTS.md#docs-travel-with-the-code).
- Checks and how to report them: [AGENTS.md, Build and test](../../AGENTS.md#build-and-test)
  and [AGENTS.md, When you finish](../../AGENTS.md#when-you-finish).
- Regression checks go through the normal affected entry point. A reviewer
  can start from the PR or handoff, identify what ran on which revision,
  and reproduce or pick up the outstanding checks.

## Add to this handbook

Use the [maintenance guide](workflow.md#maintaining-the-handbook) to add a
workflow, example or observed tool difference. Add lasting project rules to
[AGENTS.md](../../AGENTS.md) and link to the relevant handbook section.
