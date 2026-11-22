# Contributing

## Build the plugin

The plugin builds through a host project.

```bat
Build.bat -Build Plugin -Project=<Project>\<Project>.uproject
```

`-Target=<name>` selects the editor target when it is not `<Project>Editor`. The prebuilt libraries are
in `Source/ThirdParty/lib`. The build picks v142 for UE 4.x and v143 for UE 5.x.

## Run the suite

```bat
Scripts\Run-AutomationSuite.ps1 -Engine 5.8 -Project=<Project>\<Project>.uproject
```

The suite runs in the editor only (`WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR`). It uses an ephemeral
loopback port, so a running editor on 19021 does not block a run.

The 5.0 to 5.2 legs need the engine's bundled .NET 6. Set `DOTNET_ROOT` in the same PowerShell session
that starts the leg:

```powershell
$env:DOTNET_ROOT = "D:\UnrealEngine\UE_5.1\Engine\Binaries\ThirdParty\DotNet\6.0.302\windows"
$env:PATH = "$env:DOTNET_ROOT;$env:PATH"
```

## Where the code lives

The subsystem wires the collaborators together. Each collaborator owns one job, one header, and its
own lock when it has state.

| File | Owns |
|---|---|
| `BlueprintInspectorSubsystem.{h,cpp}` | Lifecycle, the detour handlers, the DAP event wiring, and the calls that bind the rest together. |
| `BlueprintClassRegistry.{h,cpp}` | Class handles, rendered source, class listing and the PIE-class preference. |
| `BlueprintPauseSession.{h,cpp}` | The paused call chain, its variable groups, expression evaluation and variable edits, and the pause/write/breakpoint handshake. |
| `BlueprintBreakpointStore.{h,cpp}` | Breakpoint records, break/continue arms and the per-function address index. |
| `BlueprintHotCapture.{h,cpp}` | Capture counters, their generation, the run metadata cache, and the hot-lines/coverage reports. |
| `BlueprintStepState.{h,cpp}` | The debugging mode and the frame the current step started from. |
| `BlueprintInspectorDAP.{h,cpp}` | cppdap transport: requests and responses, the callback bundle, authentication and the connection lifecycle. |
| `BlueprintSourceCache.{h,cpp}` | The rendered-source cache behind the class handles. |
| `BlueprintInspectionTypes.h` | The plain data the debugger and the adapter exchange (frames, variables, breakpoints, captures). No cppdap. |
| `BlueprintStepRules.h`, `BlueprintExceptionFilter.h`, `BlueprintPausedFrame.h`, `BlueprintPauseState.h`, `BlueprintBreakpointTypes.h`, `BlueprintKeys.h`, `BlueprintSourceMap.h` | Small types, predicates and key shapes. |

## Threading model

- **The game thread owns `UObject` access.** Rendering, source-map builds, the audit, variable writes
  and capture entry points all run there. `BlueprintInspectorVariableEdit::Apply` refuses to run
  elsewhere, and `FBlueprintSourceMapBuilder::Build` asserts it.
- **The DAP thread never touches `UObject`s.** Callbacks marshal through `RunOnGameThread`, bounded at
  5 s so a parked VM cannot deadlock the request. While the VM is parked, a request that needs the
  game thread returns an empty result instead of waiting.
- **A dispatch reads the step state once.** `FBlueprintStepState::Observe` returns the mode and the
  start frame as one locked snapshot, so a concurrent `continue` or `step` cannot half-apply.
- **The pause is one object with two locks.** `FBlueprintPauseSession::RequestLock` guards the paused
  flag, the setVariable queue, the queued setBreakpoints requests and the event handle. A write or a
  breakpoint request enqueues only while the flag is set, and the resume clears the flag and takes the
  queues in one step. `FBlueprintPauseSession::Lock` guards the published frames and variable groups.
  A read copies under it and never renders.
- **Breakpoints are game-thread only.** `FBlueprintBreakpointStore` has no lock. Every reader runs on
  the game thread, and the address index is rebuilt on each mutation.
- **Capture counters are shared.** `FBlueprintHotCapture` keeps them under `HotLock`. Reports snapshot
  under it and resolve outside it, so a counting VM never waits on the source cache.
- **Lock order is acyclic.** `FBlueprintPauseSession::Lock` may be held while taking
  `FBlueprintClassRegistry::ClassHandleLock`, which may be held while taking
  `FBlueprintSourceCache::SourcesLock`. Never the reverse. `RequestLock`, `HotLock`, `MetadataLock` and
  `FBlueprintStepState::Lock` are leaves.
- **Callbacks are gated.** `SetCallbacks` installs a gate that `ClearCallbacks` closes. A handler
  holding a copy of a callback cannot run against a session that is going away.

## Porting notes

These compile on 5.x and fail on 4.25:

- **`TFieldIterator`, `CastField` and the `FScript*Helper` classes come from `UObject/UnrealType.h`.**
  4.25's `UObject/FieldIterator.h` only uses `TFieldIterator`. Including it alone compiles only where
  another header already pulled `UnrealType.h` in.
- **`FFrame` is defined in `UObject/Stack.h`.** `UObject/Class.h` only forward-declares it.
- **Avoid member names that Win32 `#define`s.** `CreateEvent` becomes `CreateEventW` wherever
  `windows.h` is in scope. `FBlueprintPauseSession::CreatePauseEvent` is named for that reason.

## The VS Code extension

```bat
cd VSCodeExtension\Extension
npm install
npm test              :: tsc, then node --test over the snapshot, coverage and symbols tests
npm run package:vsix  :: writes blueprint-inspector-<version>.vsix
```

Press F5 in `Extension/` and pick **Launch Extension** to debug the extension. The tracked
`launch.json` compiles first, then opens an Extension Development Host on `Launch/`.

## Adding an audit rule

The audit lives in `Source/BlueprintInspector/Private/BlueprintAudit.{h,cpp}`. It renders each loaded
Blueprint function to pseudo-source and matches on that text. A rule is a regex or a line test over the
rendering. Everything runs on the game thread.

### Current rules

| Rule | Severity | Category | Flags |
|---|---|---|---|
| `BPI001` | error in Shipping, else warning | Shipping | The plugin is loaded in this build, so the DAP listener is present. Project-level. |
| `BPI010` | error | Security | A string literal or class default that looks like a credential. Evidence redacted. |
| `BPI020` | warning | Shipping | `PrintString`, `PrintText`, `DrawDebug*` or `Breakpoint` left in a Blueprint. |
| `BPI021` | error | Security | `ExecuteConsoleCommand`, `QuitGame`, `SetGamePaused`, `OpenLevel` or `LoadStreamLevel`. |
| `BPI030` | warning, info in a ubergraph | Performance | An expensive call in Event Tick, or in the event ubergraph of a class that implements Tick. |
| `BPI031` | info | Correctness | One dynamic cast whose result no later condition tests. One finding per cast, at the cast's own line. |
| `BPI040` | warning | Hygiene | One class short name used by several packages. |
| `BPI041` | warning | Hygiene | The audit skipped part of the input because a function could not be disassembled or a class default nested too deeply. One summary finding from `Run`. |

Pick the next free number in the family and never reuse one. `BPI001` is reserved for project-level
findings. Per-class and per-function rules use `BPI010` and up. Gaps are fine.

### Emit a finding

Every rule goes through `FScanContext::Add`:

```cpp
Context.Add(TEXT("BPI020"), EBlueprintAuditSeverity::Warning, TEXT("Shipping"),
            ClassName, FunctionName, LineNumber,
            TEXT("Debug-only call left in a Blueprint."), RedactedEvidence(Line));
```

- **Severity.** `Info`, `Warning` or `Error`. Findings sort by severity descending, then category,
  rule, class, function, line, evidence and message. The last two keys make the order total, so two
  runs over the same project cannot disagree. A rule that reports several findings under one class and
  line, such as `BPI040`, relies on them.
- **Line.** 1-based rendered line for statement findings. `0` for class-level and project-level
  findings.
- **Evidence.** What the client shows next to the finding. Redact it before it gets here. Pass
  `BlueprintAudit::MaskSecret(Value)` for anything from a secret pattern, and `RedactedEvidence(Line)`
  for a rendered source line. Raw literals must never reach a finding.
- **Extra.** Optional. It keeps the dedupe key unique. The key is
  `Rule|ClassName|FunctionName|Line|Extra`, so two findings that share all of those collapse into one.
  `BPI040` puts the package path there. `BPI010` on a class default puts the property name there.

### Choose where to scan

| Hook | Use it for |
|---|---|
| `ScanFunction` | The common case: line-by-line inspection of the rendered pseudo-source, with `Class`, `Function`, `LineNumber` and per-function counters. `BPI010`, `BPI020`, `BPI021`, `BPI030`, `BPI031` live here. |
| `ScanClassDefaults` | Class default object values, including arrays of string/name/text. Line is `0`. |
| `ScanDuplicateNames` | Findings that need the whole requested class list, such as `BPI040`. |
| `Run` | Project-level findings that are not about one class, such as `BPI001`, and the `BPI041` scan-completeness summary. |

Declare regexes as `static const FRegexPattern` next to the existing helpers (`MatchesDebugCall`,
`MatchesConsoleCall`, `MatchesExpensiveCall`) and test with `Matches(Pattern, Line)`.

### Add a secret pattern

`FSecretPatterns` and `SecretPatternName` are in the anonymous namespace of `BlueprintAudit.cpp`.
`SecretPatternName` returns a display name, and the first match wins. Put more specific patterns above
`Assignment`, the broad `key=value` catch-all. A new pattern affects `BPI010` and the masking applied
to every other rule's evidence.

### Test it

- Unit level: `Private/Tests/BlueprintAuditTests.cpp` (`BlueprintInspector.Audit.SecretPatterns`)
  covers the pattern helpers. Add the positive and the near-miss negative.
- End to end: `Private/Tests/BlueprintDapAuditTest.cpp` (`BlueprintInspector.Debugger.DapAudit`)
  drives the audit over DAP with `maxFindings = 100` and asserts that classes and functions were
  scanned, that findings came back, and that the project-level finding is present. Assert your rule's
  ID from the response rather than from the C++ return value.

Run both through `Scripts\Run-AutomationSuite.ps1`.

### Where it surfaces

`FBlueprintAuditResult` reaches the client as DAP findings. The severity string comes from
`BlueprintAudit::SeverityToString` in `BlueprintInspectorDAP.cpp`. The extension groups findings by
severity in the **Audit** view and exports them as Markdown. The audit renders every loaded Blueprint
on the game thread and does not run automatically, so expect a hitch on large projects.

## Pull requests

CI runs a scripts job and the extension tests on hosted runners. The engine legs run on a self-hosted
runner and stay disabled unless the `ENABLE_ENGINE_CI` repository variable is `true`. They refuse fork
pull requests. If you change engine-facing code, run the suite locally against at least one 4.x and one
5.x engine before opening the PR.
