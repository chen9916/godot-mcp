# OpenCode MCP tests

Run bridge/unit and transport integration tests with:

```powershell
npm test
```

The default suite uses temporary localhost endpoints and does not require Godot. It covers session parsing, a real replacement-endpoint reconnect, stale handshakes, safe versus unsafe replay, bounded backoff/timeouts, structured JSON-RPC errors, runtime request deduplication, filesystem polling, capability/tool registration, and partial-read schemas.

Godot-side primitive/module tests live in `tests/editor/opencode_mcp/test_opencode_mcp_primitives.cpp` and are included in builds with `tests=yes`.

## Live editor workflow

The live test is skipped by default because it starts/stops game runs and temporarily adds resources to a chosen project. Build and run an editor containing these changes, enable `network/opencode_mcp/enabled`, close unrelated game runs, then use:

```powershell
$env:GODOT_MCP_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\test-project"
npm test
```

The test refuses to overwrite an existing fixture directory. Its uniquely named scripts, scene, SVG, and captured PNG are removed in an `after` hook, followed by an editor filesystem scan.

## Regression coverage

| Regression | Coverage |
| --- | --- |
| Editor restart and reconnect | Real session-file + TCP replacement-endpoint integration test; lifecycle unit tests also cover transport-loss replay rules. |
| Valid/invalid GDScript without opening it | Godot C++ module test and gated live workflow. |
| Fresh SVG scan/import/load | Gated live workflow creates an SVG externally, scans/imports it, and asserts it is loadable. |
| Editor buffer differs from disk | `OpenCodeMCPResourceVersioning` conflict logic plus version/status module tests; live use requires an actual unsaved editor tab and is intentionally not fabricated or discarded by the harness. |
| Reload changed GDScript without losing unsaved work | Gated disk-reload workflow plus the conflict guard above. |
| Start, advance physics, inspect, stop | Gated live workflow. |
| Input action changes runtime state | Gated live workflow. |
| Viewport capture | Gated live workflow, including output existence and cleanup. |
| Partial settings with a missing key | Gated live workflow; batch primitive and registry unit tests. |
| Diagnostics separated across two runs | Godot diagnostics-store unit test and two-run gated workflow. |
| Validation does not invalidate editor | In-process validator architecture and gated assertion that the editor session UUID is unchanged. |
| Timeouts/disconnected sessions are structured and retryable | Bridge transport, waiter, and lifecycle unit tests. |
