# Shared workflow for contributors and agents

Use this workflow for local tools, cloud runners, chat-based reviews and
handoffs between them. The shared inputs are the task, the current revision
and [repository instructions](../../AGENTS.md). The shared output is a diff or
review with evidence a second person can reproduce.

## Start or resume a task

1. Capture the requested outcome, the normal entry point where it should
   work, the expected result and any actions already authorized. For a
   bug, record the environment where it occurs and the smallest observable
   symptom. A request about an add-on, server or deployment can span
   several repositories. Find the owner of each proposed change first.
2. Open the repository instructions and [briefing](README.md), then read
   the applicable area docs and existing tests. When resuming, read the
   previous handoff and verify its branch, revision and diff against the
   actual workspace. Read any instructions that apply to a sibling
   repository before working there.
3. From the candidate checkout, identify its location, current HEAD,
   status and linked worktrees. Run these in the repository and keep
   private paths and remote URLs local.

   ```sh
   git rev-parse --show-toplevel
   git status --short --branch
   git log -1 --oneline
   git worktree list
   ```

4. Set up the worktree and branch as [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow)
   says. For an existing change, compare against its intended base.
   Inspect staged and unstaged changes and preserve both. A checkout's
   `main` name alone says nothing about whether it contains the public base
   or work from a maintainer's local integration branch.
5. Write down the likely owner, source evidence and minimum checks.
   Update this as you trace the code. Pause at decisions that would widen
   repository scope, change a documented contract or require an external
   action beyond the task's authorization.

## Trace behavior and record evidence

Start at the boundary a user or system actually uses. For an affected
button, command or packet, follow registration, decoding or dispatch, then
the state mutation and consumer. Check initialization and teardown, and
follow the same data through any Lua or compiled add-on APIs that expose it.
Use the [codebase map](codebase.md) and [worked recipes](recipes.md) to find
entry points.

Keep these questions distinct when a report involves parity.

| Question | Evidence to record |
| --- | --- |
| What does Hailstone do at this revision? | The owning source path and symbol, the entry point and a test or observation that exercises it |
| What behavior should it match? | The documented contract or an independently observed RoF2 rule, with the reference or its sanitized finding |
| What does a server provide? | Which server profile or capability took the path and what is defined for an unmodified RoF2 server |
| How far does the evidence go? | Whether it came from inspection, a unit test, mock session, fixture replay or a live session, plus any unresolved inference |

Check implementation and executable tests together. A reference can show
intended behavior while a test covers just one input. If tests, docs and
implementation disagree, identify the discrepancy in the review or
change. Unresolved RoF2 behavior follows "No guessing", and reference
material, captures and player assets follow "Clean room", both in
[AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).

Send security findings through the project's disclosure process.
[Security policy](../../SECURITY.md)

## Use the tools you have

| Environment | Entry point | Equivalent when unavailable |
| --- | --- | --- |
| Repository-aware shell or editor | [AGENTS.md](../../AGENTS.md) | Open the file at the repository root and follow the [briefing](README.md) |
| Claude Code | [CLAUDE.md](../../CLAUDE.md) imports the shared instructions | Open AGENTS.md directly and select the relevant guide from the briefing |
| Gemini CLI | [GEMINI.md](../../GEMINI.md) imports the shared instructions | Open AGENTS.md directly and select the relevant guide from the briefing |
| GitHub Copilot | [Repository instructions](../../.github/copilot-instructions.md) link to the shared instructions | Attach or open AGENTS.md and the briefing, including for review sessions |
| Chat or read-only review | Provide the task, base and head revisions, diff and relevant excerpts | Name missing files or context when reporting what you could establish |
| Shell without a compiler, GPU or local fixtures | Inspect the diff and run the available repo checks | Transfer the exact build or test command and prerequisite needed to a contributor who has them |
| Tool without delegation or persistent history | Work through the same source and check stages in one session | Save a handoff before the task crosses a session or worktree boundary |
| Tool without GitHub access | Prepare and review the local diff against the intended base | Record the commit or worktree state and intended PR base for whoever publishes it |

Instruction discovery depends on the tool and its configuration. Verify
that it read the shared instructions and the relevant task page. Repository
links are sufficient for a tool with file access. For a tool that sees only
an excerpt, supply those pages or the sections it needs. Keep unfamiliar
filenames, missing diagnostics and commands that could not be run visible
in the handoff. Product references for instruction loading are
[Claude Code](https://code.claude.com/docs/en/memory),
[Gemini CLI](https://geminicli.com/docs/cli/gemini-md/) and
[GitHub Copilot](https://docs.github.com/en/copilot/reference/custom-instructions-support).

Hailstone has a [Claude cloud session hook](../../.claude/hooks/session-start.sh)
that prepares a build directory when its cloud prerequisites are available.
For a runner without that setup, configure the build using the
[validation guide](validation.md). A prepared build directory only proves
configuration ran. Run the build and checks you need on the intended revision.

For a new session, this prompt works as a starting point in any tool with
repository access.

```text
Work in the Hailstone checkout at <path> on <task>.
Read AGENTS.md and docs/agents/README.md, then open the pages for this task.
Check the branch, base and existing diff before editing.
Trace the normal entry point and identify the affected compatibility contract.
Verify the result with the documented checks that this environment supports.
Report the revision, evidence, skipped coverage and next check.
```

## Implement and review

1. Follow existing extension points and place tests at the lowest layer
   that proves the change through its real entry point. Use the
   [validation guide](validation.md) to add higher-level checks where a
   unit test cannot see the failure.
2. Check old profiles, fixtures, UI packs, SDK consumers or server paths
   when the change touches their contract. Generated files:
   [AGENTS.md, Adding code](../../AGENTS.md#adding-code). Docs, known gaps and changelog:
   [AGENTS.md, Docs travel with the code](../../AGENTS.md#docs-travel-with-the-code).
3. When splitting work, give each contributor the same base, task scope,
   expected evidence and a non-overlapping write area. Keep integration
   and final verification with one owner. If changes overlap, inspect
   both diffs and rerun the affected checks on the combined revision.
4. Review the complete diff against the intended base, including generated
   files and new files. Check the normal entry point, failure cases,
   dependencies and repository boundaries. Check for private data before
   any external handoff.
5. For an authorized draft PR, state the behavior or documentation change,
   why it belongs in this repository, how it was checked and what the
   maintainers are being asked to verify. Use the public base and the
   authorized head remote. Pushing, approving and merging follow
   [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow): do them only
   when the maintainer explicitly asks, each time.

An independent reviewer can use the same steps. Begin with the original
request and the diff. Trace the change and verify its assumptions against
the source, then flag actionable findings with a file, symbol or line,
affected behavior and a way to confirm it. Inspect both the stock path
and the path the change adds or fixes.

## Pushing, approving and merging

The maintainer keeps control of pushing, approving and merging. A review
ends in findings or local commits, not a push, an approval or a merge,
unless the maintainer explicitly asks for that step, each time. The rules
are in [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow).

## Handoff

Keep a handoff with the task or PR. Use repo-relative paths in a shared
report. Give a local recipient a separate worktree location when they
need it. Recheck evidence when HEAD, fixtures or the build configuration
change.

```text
Outcome and scope:
Repository, base and head:
Local changes or dependent PRs:
Normal entry point and expected behavior:
Evidence and source pointers (path, symbol, or permitted reference):
Checks run (command, revision, platform, configuration, fixture state, result):
Checks skipped or blocked (requirement and how to run the check):
Open decisions and who owns them:
Next action:
```

For example, a settings change might report a passing restart test in
Modern on Windows with a disposable profile and a passing Lua fixture for
both packs. The remaining check can be a Gamepad interaction on the same
revision with the in-game controller, with the exact menu path, expected
default and saved state. That gives a second contributor enough
information to run the interaction and update the result.

## Maintaining the handbook

- Add shared policy to [AGENTS.md](../../AGENTS.md). Keep the tool entry
  files as pointers to that file and the briefing.
- For a new workflow, add a route to the [briefing](README.md) when it
  helps readers find it. Describe the task, owning entry point, contract,
  example, test commands and what success looks like. Link to existing
  reference docs and code. Use relative paths and stable symbols.
- For a tool difference, report the tool and version, environment, what
  you asked it to read or run, what you observed, and a fallback that
  leaves equivalent evidence. Keep one-off observations in the PR
  discussion until they support a durable handbook change.
- Keep private setup notes and credentials in their respective local or
  private systems. Use public or synthetic examples in this handbook.
- Add new pages to `PAGES` in [tools/sync_wiki.py](../../tools/sync_wiki.py).
  Run `python tools/sync_wiki.py --check` and try the path from the briefing
  through the new page to its source and check. The [wiki guide](../wiki.md)
  covers preview and publishing.
