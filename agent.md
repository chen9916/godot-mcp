# Agent handoff: Godot OpenCode MCP v2

Last updated: 2026-08-27

## Current state

The reliability and editor-automation expansion for the Godot 4.8 OpenCode MCP integration is implemented and committed.

- Pre-implementation checkpoint: `33d39bdf3 Checkpoint OpenCode MCP editor improvements`
- Implementation commit: `84b52f18d Improve OpenCode MCP automation reliability`
- The bridge registers 72 tools in total, including all 19 new v2 automation tools.
- Protocol v2 uses editor-session UUIDs, capability negotiation, stale-session rejection, bounded reconnection/backoff, structured errors, and stable runtime request IDs.
- Existing v1 session metadata remains supported by the bridge as a compatibility fallback.

## Implemented tool surface

- Validation: `godot.script.validate`
- Filesystem/import: `godot.filesystem.scan`, `godot.filesystem.wait`, `godot.resource.import`
- Resource diagnostics: `godot.resource.status` and safer `godot.resource.reload`
- Runtime lifecycle: `godot.runtime.start`, `stop`, `status`, `pause`, `resume`, `advance_frames`
- Runtime interaction: `godot.runtime.get_tree`, `get_property`, `set_property`, `call`, `input_action`, `input_event`
- Capture: `godot.runtime.capture_viewport`
- Diagnostics: `godot.diagnostics.get`
- Existing project, node, and resource batch reads now retain successful per-item results unless atomic behavior is requested.

OpenCode displays MCP tool names with underscores, for example `godot_script_validate`, while the bridge registry uses dotted names.

## Architecture

Focused Godot-side modules live under `editor/debugger/opencode_mcp/`:

- `OpenCodeMCPError`
- `OpenCodeMCPBatchResult`
- `OpenCodeMCPDiagnosticsStore`
- `OpenCodeMCPScriptValidation`
- `OpenCodeMCPFilesystemSync`
- `OpenCodeMCPResourceVersioning`
- `OpenCodeMCPRuntimeController`
- `OpenCodeMCPRuntimeInspector`
- `OpenCodeMCPViewportCapture`

The game-process endpoint is `scene/debugger/opencode_mcp_runtime_debugger.*`. It is installed only for an MCP-owned run carrying `--opencode-mcp-run-id`, so ordinary games and headless validators do not replace or invalidate the editor session.

Bridge lifecycle, error translation, polling/deduplication, and registration are separated into:

- `misc/opencode_mcp_bridge/src/session_lifecycle.ts`
- `misc/opencode_mcp_bridge/src/protocol_error.ts`
- `misc/opencode_mcp_bridge/src/operation_waiter.ts`
- `misc/opencode_mcp_bridge/src/tool_registry.ts`

Detailed contracts are in:

- `editor/debugger/opencode_mcp/ARCHITECTURE.md`
- `editor/debugger/opencode_mcp/AUTOMATION_TOOLS_V2.md`

## Verification completed

- Strict MSVC Godot editor build with `/WX`: passed.
- Focused native tests: 8/8 cases and 49/49 assertions passed.
- Bridge tests: 21 passed; the destructive live-editor workflow remained intentionally gated and skipped.
- TypeScript typecheck and production build: passed.
- `git diff --check`: passed before the implementation commit.
- Built editor version: `4.8.dev.custom_build.84b52f18d`.
- Verified binaries:
  - `bin/godot.windows.editor.x86_64.mcp_latest.verify.exe`
  - `bin/godot.windows.editor.x86_64.mcp_latest.verify.console.exe`

The `.verify` suffix was used because a running editor locked the normal `mcp_latest` executable. Do not terminate an unrelated editor merely to replace that file.

## Live OpenCode discovery check

OpenCode 1.18.23 reported `godot-opencode` connected to this repository's rebuilt `misc/opencode_mcp_bridge/dist/main.js`.

`godot_session_info` confirmed:

- connection state `connected`
- protocol `opencode-godot-v2`
- protocol version 2
- all new capability flags enabled, including validation, filesystem sync, resource versioning, runtime control/inspection, viewport capture, diagnostics v2, and partial batch results

An OpenCode agent initially produced an incomplete natural-language tool list. This was not an MCP registration failure: the rebuilt bridge directly reports all 72 definitions, and OpenCode successfully invoked the omitted `godot_diagnostics_get` tool against the live editor.

## Verification still pending

Do not claim that every feature has been verified end to end yet. The gated workflow that creates temporary project resources, imports a fresh SVG, reloads a script, starts two runs, advances frames, injects input, captures a viewport, and checks run-scoped diagnostics has not been executed against a disposable live project.

To run it after starting a newly built editor with MCP enabled:

```powershell
$env:GODOT_MCP_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\disposable-test-project"
cd misc\opencode_mcp_bridge
npm.cmd test
```

The test refuses to overwrite an existing fixture directory and removes its temporary resources in an `after` hook.

Useful focused commands:

```powershell
cd misc\opencode_mcp_bridge
npm.cmd test
npm.cmd run typecheck
npm.cmd run build

cd ..\..
.\bin\godot.windows.editor.x86_64.mcp_latest.verify.console.exe --headless --test --test-case="*[OpenCodeMCP]*"
```

## Workspace hygiene

The root `artifacts/` directory is pre-existing, untracked user data. It was deliberately left untouched and must not be staged with MCP changes.
