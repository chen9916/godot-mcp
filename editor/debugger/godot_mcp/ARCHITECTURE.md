# Godot MCP automation architecture

The integration has three process boundaries: a coding agent with an MCP client, the Node.js stdio bridge, and the Godot editor. Runtime inspection adds a fourth boundary, the game process started by that editor.

This document describes module ownership and request flow. Supported release
scope and verification evidence are recorded in
[MCP_STATUS.md](../../../MCP_STATUS.md).

```text
Coding agent / MCP client
        |
        | MCP over stdio
        v
ToolRegistry -> SessionLifecycle -> line-delimited JSON-RPC/TCP
                                      |
                                      v
GodotMCPServer -> GodotMCPProtocol
                         |       |
              editor modules     RuntimeController
                                      |
                              editor debugger protocol
                                      |
                                      v
                           runtime-only debugger endpoint
```

## MCP boundary and client setup

The standard MCP server is the TypeScript/Node bridge. Its MCP SDK handles initialization, protocol-version negotiation, tool discovery, and tool calls over stdio. The registry advertises 83 Godot tools, including the animation suite. The editor's localhost TCP port speaks the Godot-specific JSON-RPC API; coding agents connect through the bridge rather than pointing an HTTP MCP client at that port.

The Godot protocol version 2 refers to the internal editor/bridge contract. It is separate from the date-based MCP protocol versions negotiated by the SDK.

Each coding agent starts its own bridge process. The bridge does not identify or branch on the calling agent. It closes its editor connection when the client's stdin ends. Several bridge processes can connect to one editor, but they share that editor's active scene, resources, and runtime; scene-version and expected-scene-path checks help avoid stale edits.

Generic configuration export provides the executable and argument details without accessing client configuration files. The editor invokes this CLI asynchronously and enables the local endpoint after success.

Source paths, C++ classes, stored settings, session metadata, runtime flags, and internal RPC identifiers all use Godot MCP naming. Rebuild the editor and bridge together when upgrading from previous identifiers.

## Ownership

| Module | Process | Responsibility |
| --- | --- | --- |
| `ToolRegistry` | bridge | MCP names, input schemas, capability requirements, and replay-safety metadata. |
| `SessionLifecycle` | bridge | Session discovery, process checks, v2 handshake, connection state, bounded retry/backoff, and safe read replay. |
| `OperationWaiter` | bridge | Polling asynchronous filesystem/runtime operations without blocking Godot's editor thread. Runtime retries use one deduplication ID. |
| `GodotMCPServer` | editor | Localhost endpoint lifecycle, unique session UUID, capability advertisement, and ownership-safe session metadata. |
| `GodotMCPProtocol` | editor | JSON-RPC framing/dispatch, session and capability guards, and delegation to focused modules. Existing scene/editor methods remain here. |
| `GodotMCPScenePatch` | editor | Declarative scene/resource graph validation, optimistic scene-version checks, request-local reference resolution, and one-action undo/redo planning. It never writes scene text directly. |
| `GodotMCPAnimationGraph` | editor | Nested AnimationTree inspection, structural graph edits, local-copy atomic undo transactions, and graph validation. |
| `GodotMCPAnimationLibrary` | editor | Animation library/clip discovery and local-copy transactional editing of all animation track types and keyframes. |
| `GodotMCPError` | editor | Stable error names and structured error fields. |
| `GodotMCPBatchResult` | editor | Successful and failed per-item results with aggregate counts. |
| `GodotMCPScriptValidation` | editor | Editor-language validation of disk or supplied source without opening a scene or editor tab. |
| `GodotMCPFilesystemSync` | editor | Scan/import orchestration, including `update_files()` before import so newly created imported resources become visible. |
| `GodotMCPResourceVersioning` | editor | Disk, editor-buffer, cache, UID, importer, dependency, and scene-reference comparison; conflict-safe reload. |
| `GodotMCPDiagnosticsStore` | editor | Bounded sequence-numbered editor and run-scoped diagnostics. |
| `GodotMCPRuntimeController` | editor | Ownership of one MCP-started run, run IDs, lifecycle/crash state, and debugger requests. It never stops an unrelated run. |
| `GodotMCPRuntimeInspector` | editor | Godot debugger-session handshake, request deduplication, pending responses, and runtime diagnostic routing. |
| `GodotMCPViewportCapture` | editor | Focused capture entry point delegated through the owned runtime. |
| `GodotMCPRuntimeDebugger` | game | Runtime-only node access, calls, animation playback/state/parameter controls, input, bounded frame waits, diagnostics, and PNG capture. It is installed only when the editor supplies an MCP run ID. |

## Request flow

1. The bridge reads `.godot/godot_mcp/session.json`, verifies its PID/expiry, and handshakes `godot.session.info` against the advertised session UUID.
2. The bridge checks the tool's advertised capability, adds `_session_id`, and sends one JSON-RPC request to the editor.
3. The protocol rejects stale session IDs before dispatch, then delegates the request to its owning module.
4. Editor-only work returns directly. Runtime work is sent through Godot's debugger protocol with both `run_id` and a bridge-generated `_request_id`.
5. The game process rejects stale runs and editor targets, executes a bounded operation, and returns a serializable response. The editor retains that response until the bridge acknowledges it (or the bounded retention expires). Polling and reconnects use the same `_request_id`, so a lost final TCP response cannot apply the operation twice.

## Lifecycle contract

Each editor process owns a UUID. A replacement editor supersedes the session file, and an older editor removes that file only if it still owns the UUID. The bridge tolerates transient reads while metadata is being refreshed, remains alive across editor restarts, and reports `disconnected`, `reconnecting`, `connected`, or `failed` through `godot.session.info`.

Read-only calls marked replay-safe may be retried once after transport loss. Mutations are not blindly replayed because loss of the response is ambiguous. Asynchronous runtime operations are the exception: the runtime inspector deduplicates their stable request ID.

Every runtime has a separate UUID. Runtime paths are `/root` paths and are resolved only inside that game process; editor object IDs never cross the boundary. A normal game, imported-resource helper, or headless validator has no MCP run argument and therefore does not install the runtime endpoint or alter the connected editor session.

## Bounds and safety

- Requests are limited to 1 MiB; imports to 64 paths; scene patches to 512 operations; runtime arguments to 16 values.
- Trees, serialized containers (2000 items, eight levels, and 256 Ki characters), diagnostics, pending requests, screenshots, frame waits, timeouts, and reconnect attempts are bounded.
- Disk reload checks for conflicting editor buffers. Mutation guards vary by
  method; some expected-version fields are optional. Scene saves compare disk
  content with the baseline accepted on load or save, including after pre-save
  callbacks and packing. These optimistic checks do not lock external processes.
- Scene patches compare both the active scene path and an opaque token derived from its history plus live serializable graph. Atomic patches finish validation before creating their single undo action; dry runs create no action, and saving requires an explicit `save: true` plus `save_resource` capability.
- Animation patches require both scene guards, use local resource copies and one
  undo action, and never save implicitly. Native graph preflight runs before
  animation scene-version enumeration; key/resource cloning has decoded-key,
  semantic Variant, and packed-buffer budgets. The bridge validates the animation
  inputs against the same schemas published by its offline schema tool.
- Stopping a run and runtime `free`/`queue_free` calls require explicit confirmation.
- The controller records a debugger disconnect separately from a process exit and retains ownership so the requested run can still be stopped.

See [AUTOMATION_TOOLS_V2.md](AUTOMATION_TOOLS_V2.md) for automation schemas,
[ANIMATION_TOOLS.md](ANIMATION_TOOLS.md) for animation authoring and playback,
[README.md](README.md) for other editor methods, and the
[test guide](../../../misc/godot_mcp_bridge/test/README.md) for verification procedures.
