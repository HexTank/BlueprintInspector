# Blueprint Inspector (VS Code extension)

This is the VS Code client for BlueprintInspector, a DAP server that an Unreal Engine game hosts on
`127.0.0.1:19021`. It opens Blueprint functions as `blueprint-source:` documents with breakpoints,
stepping and variables.

**Start Game and Attach** launches the packaged game configured in
`blueprintInspector.gameExecutable`, waits for the listener and attaches. The **Attach to Blueprint
Inspector server** launch configuration attaches to a game that is already running.

## Breakpoints and logpoints

Conditional breakpoints and hit counts use the normal VS Code UI. **Add Logpoint** prints a message
without stopping and evaluates `{expr}` placeholders. Watch and hover resolve property paths against
the paused frame.

## Exception stops

You can make Blueprint runtime errors stop execution. Turn on **Accessed None**, **Blueprint runtime
errors** or **Runaway loops** in the **Breakpoints** panel's exception filters. None is selected by
default, and the selection lasts for the debug session.

A stop has reason `exception` and shows the VM's message. The stack, scopes, variables and watch work
as at any breakpoint. **Continue** resumes just after the error, and the error still reaches the log.
An animation Blueprint's error on a worker thread is logged, not stopped.

## Navigation

The pseudo-source is a virtual document, so the extension parses it and hands the result to VS Code.

- **Outline** and **Ctrl+Shift+O** list each function and the variables it declares. Typing filters the
  list, and selecting an entry jumps to it.
- **F12** or **Ctrl+click** on a variable goes to its definition: the parameter list, or the first
  statement that assigns it. The renderer writes the same name two ways (`HitActor` in a signature,
  `Hit Actor` in the body), so matching ignores spaces, underscores and case.
- **Blueprint Inspector: Go to Variable…** narrows the picker to variables in the open document.

Navigation is read-only. The rendered source is a view of the Blueprint, so editing the text does not
change the asset. To change a value at a stop, edit it in the **Variables** view, which writes through
`setVariable`.

## Hot-path capture

Hot-path capture counts executed statements and break/continue arms for the running game. It is off by
default and scoped to the debug session.

Toggle it from the status bar (`Blueprint capture`), the debug toolbar, or **Blueprint Inspector: Start
Capture**, **Stop Capture** and **Reset Capture**. Rendered lines get an orange heat background and a
`×count` inlay hint, and the overview ruler marks them.

The **Hot Paths** view in Run and Debug ranks classes, functions and lines by hits. Select a line to
open it. Capture start renders every loaded Blueprint by default, so unopened classes resolve
(`blueprintInspector.resolveAllOnCapture`).

The status bar shows hits that resolved to rendered lines. Hover it for the raw dispatch count. **Stop
Capture** freezes the counters and keeps the heat. **Reset** or a new capture clears them, and ending
the debug session clears everything.

Only executed statements count, and only classes whose source has been opened can resolve. The numbers
are counts, not timings.

## Coverage

Coverage shows which statements a run never reached, using the same counters as Hot Paths.

Start a capture, play and stop. The **Coverage** view lists classes, functions and lines with
`covered/total` labels, least-covered class first. Select a line to open it.

**Show Only Uncovered Statements** narrows the tree to lines with no hits. **Refresh Coverage**
re-reads the counters. Stopping a capture refreshes an open snapshot, and **Reset Capture** discards
it.

**Export Coverage (JSON)** and **Export Coverage (LCOV)** write the snapshot for QA or CI. LCOV feeds
`genhtml` and coverage hosts directly.

Only rendered classes resolve, and a line is covered when its counter is above zero. The numbers are
counts, not timings.

## Capture snapshots

**Save Capture Snapshot** writes the current counters, with map, build, engine and timestamps, to a
JSON file. **Compare Capture With Snapshot** diffs this run against a saved file.

The **Hot Path Diff** view lists regressions first, then new lines, improvements and lines the run
stopped reaching. Select one to open it. Unchanged lines are counted in the summary only.

Snapshots are plain JSON, comparable across sessions, and capped at 20,000 lines per capture. Only
rendered classes are comparable.

## Audit

**Blueprint Inspector: Run Audit** checks the loaded Blueprints for hardcoded secrets, debug calls,
console and level control, risky Tick work, unchecked casts and duplicate class names.

Findings appear in the **Audit** view grouped by severity. Select one to open its line. **Export Audit
Report** writes Markdown for a pre-ship checklist.

The audit is a review aid, not a security guarantee. Values that look like secrets are redacted.

Settings: `blueprintInspector.gameExecutable`, `blueprintInspector.gameArguments`,
`blueprintInspector.tokenFile`, `blueprintInspector.resolveAllOnCapture`. See the
[plugin documentation](https://github.com/HexTank/BlueprintInspector#readme) for setup.
