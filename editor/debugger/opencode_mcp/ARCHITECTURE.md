# OpenCode MCP automation architecture

The integration has three process boundaries: an MCP client such as OpenCode, the Node.js stdio bridge, and the Godot editor. Runtime inspection adds a fourth boundary, the game process started by that editor.

```text
OpenCode/MCP client
        |
        | MCP over stdio
        v
ToolRegistry -> SessionLifecycle -> line-delimited JSON-RPC/TCP
                                      |
                                      v
OpenCodeMCPServer -> OpenCodeMCPProtocol
                         |       |
              editor modules     RuntimeController
                                      |
                              editor debugger protocol
                                      |
                                      v
                           runtime-only debugger endpoint
```

## Ownership

| Module | Process | Responsibility |
| --- | --- | --- |
| `ToolRegistry` | bridge | MCP names, input schemas, capability requirements, and replay-safety metadata. |
| `SessionLifecycle` | bridge | Session discovery, process checks, v2 handshake, connection state, bounded retry/backoff, and safe read replay. |
| `OperationWaiter` | bridge | Polling asynchronous filesystem/runtime operations without blocking Godot's editor thread. Runtime retries use one deduplication ID. |
| `OpenCodeMCPServer` | editor | Localhost endpoint lifecycle, unique session UUID, capability advertisement, and ownership-safe session metadata. |
| `OpenCodeMCPProtocol` | editor | JSON-RPC framing/dispatch, session and capability guards, and delegation to focused modules. Existing scene/editor methods remain here. |
| `OpenCodeMCPError` | editor | Stable error names and structured error fields. |
| `OpenCodeMCPBatchResult` | editor | Successful and failed per-item results with aggregate counts. |
| `OpenCodeMCPScriptValidation` | editor | Editor-language validation of disk or supplied source without opening a scene or editor tab. |
| `OpenCodeMCPFilesystemSync` | editor | Scan/import orchestration, including `update_files()` before import so newly created imported resources become visible. |
| `OpenCodeMCPResourceVersioning` | editor | Disk, editor-buffer, cache, UID, importer, dependency, and scene-reference comparison; conflict-safe reload. |
| `OpenCodeMCPDiagnosticsStore` | editor | Bounded sequence-numbered editor and run-scoped diagnostics. |
| `OpenCodeMCPRuntimeController` | editor | Ownership of one MCP-started run, run IDs, lifecycle/crash state, and debugger requests. It never stops an unrelated run. |
| `OpenCodeMCPRuntimeInspector` | editor | Godot debugger-session handshake, request deduplication, pending responses, and runtime diagnostic routing. |
| `OpenCodeMCPViewportCapture` | editor | Focused capture entry point delegated through the owned runtime. |
| `OpenCodeMCPRuntimeDebugger` | game | Runtime-only node access, calls, input, bounded frame waits, diagnostics, and PNG capture. It is installed only when the editor supplies an MCP run ID. |

## Request flow

1. The bridge reads `.godot/opencode_mcp/session.json`, verifies its PID/expiry, and handshakes `opencode.session.info` against the advertised session UUID.
2. The bridge checks the tool's advertised capability, adds `_session_id`, and sends one JSON-RPC request to the editor.
3. The protocol rejects stale session IDs before dispatch, then delegates the request to its owning module.
4. Editor-only work returns directly. Runtime work is sent through Godot's debugger protocol with both `run_id` and a bridge-generated `_request_id`.
5. The game process rejects stale runs and editor targets, executes a bounded operation, and returns a serializable response. The editor retains that response until the bridge acknowledges it (or the bounded retention expires). Polling and reconnects use the same `_request_id`, so a lost final TCP response cannot apply the operation twice.

## Lifecycle contract

Each editor process owns a UUID. A replacement editor supersedes the session file, and an older editor removes that file only if it still owns the UUID. The bridge tolerates transient reads while metadata is being refreshed, remains alive across editor restarts, and reports `disconnected`, `reconnecting`, `connected`, or `failed` through `godot.session.info`.

Read-only calls marked replay-safe may be retried once after transport loss. Mutations are not blindly replayed because loss of the response is ambiguous. Asynchronous runtime operations are the exception: the runtime inspector deduplicates their stable request ID.

Every runtime has a separate UUID. Runtime paths are `/root` paths and are resolved only inside that game process; editor object IDs never cross the boundary. A normal game, imported-resource helper, or headless validator has no MCP run argument and therefore does not install the runtime endpoint or alter the connected editor session.

## Bounds and safety

- Requests are limited to 1 MiB; imports to 64 paths; runtime arguments to 16 values.
- Trees, serialized containers (2000 items, eight levels, and 256 Ki characters), diagnostics, pending requests, screenshots, frame waits, timeouts, and reconnect attempts are bounded.
- Disk reload refuses an open editor buffer whose version differs from disk. Writes retain optimistic version checks.
- Stopping a run and runtime `free`/`queue_free` calls require explicit confirmation.
- The controller records a debugger disconnect separately from a process exit and retains ownership so the requested run can still be stopped.

See [AUTOMATION_TOOLS_V2.md](AUTOMATION_TOOLS_V2.md) for the public schemas and [README.md](README.md) for the legacy editor methods.
