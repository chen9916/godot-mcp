# Godot MCP bridge

This package exposes a local MCP stdio server for compatible MCP clients. It forwards tool calls to the Godot editor's localhost JSON-RPC endpoint. Each client launches its own bridge process; all connections share editor state.

```text
MCP client -> stdio MCP (Node bridge) -> localhost JSON-RPC -> Godot editor
```

For supported release scope, executed checks, and remaining work, see [MCP status](../../MCP_STATUS.md).

## Requirements and build

- Node.js 18 or later.
- An editor built with the Godot MCP source changes. Rebuild the editor and bridge together when upgrading from previous identifiers; old portable archives retain their original code until repackaged.
- MCP enabled through `network/godot_mcp/enabled`, or through **Copy MCP Configuration** below.

Use the [bridge build commands](test/README.md#bridge-build-and-default-tests) from this directory. The same guide owns native build commands and optional live workflows.

## Connect a client

Open **Editor > Editor Settings > Network > Godot MCP > Copy MCP Configuration**. This enables the endpoint and copies JSON using the common `mcpServers` convention. Add the connection to your client's MCP settings. Clients using another format can use the exported `command` and `args`.

Portable layouts discover `bridge/dist/main.js` beside the editor executable and bundled Node at `runtime/node.exe` on Windows or `runtime/bin/node` on other platforms. Source builds in `bin/` discover `../misc/godot_mcp_bridge/dist/main.js` and fall back to Node on PATH. Advanced **Bridge Script** and **Node Executable** settings override discovery. Build the bridge first for a source checkout.

Copy the configuration again after moving the editor or bridge. Export runs asynchronously with a 30-second timeout and invokes Node directly. It copies connection details without editing client configuration files.

For command-line export, run from the built bridge directory:

```bash
node dist/main.js --print-config mcp --project-root "<game-project>"
node dist/main.js --print-config connection --project-root "<game-project>"
```

The `mcp` format contains a complete `mcpServers` JSON object. The `connection` format contains only `command` and `args`. Both use the running Node executable's absolute path. Choose stdio/local-process transport: the editor TCP port is an internal JSON-RPC endpoint, not an HTTP MCP URL.

## Project selection and manual launch

The game project contains `project.godot`; its session metadata is `.godot/godot_mcp/session.json`. Use `--project-root` when several editors are open, or `--session-file` to select an explicit metadata file. Without either pin, discovery walks upward from the working directory, then checks Godot's recent projects for the most recently modified session file.

```bash
node dist/main.js --project-root "<game-project>"
```

Optional launch arguments:

- `--project-root "<path>"`: selects the project's session file.
- `--session-file "<absolute-path>"`: overrides project discovery.
- `--timeout-ms 15000`: changes the RPC timeout; the default is 10000 milliseconds.

The MCP SDK handles initialization, version negotiation, discovery, and calls. Server stdout carries MCP messages; diagnostics go to stderr. Closing stdin releases the editor connection and exits the bridge. Independent stdio tests exercise four protocol versions; they do not launch agent applications. See [test coverage](test/README.md#coverage-and-limits).

## Tools and contracts

The public server identity is `godot-mcp`, and tool names use the `godot.*` prefix. The bridge verifies session UUID/PID metadata, reconnects with bounded backoff, and checks advertised capabilities before forwarding calls. Tool discovery can run without an editor; `godot.scene.patch_schema` returns the recursive patch schema offline. `godot.session.info` reports bridge state even while the editor is unavailable.

Use these references instead of a second tool/capability catalog:

- [Editor endpoint overview](../../editor/debugger/godot_mcp/README.md): settings and source entry points.
- [Automation tools v2](../../editor/debugger/godot_mcp/AUTOMATION_TOOLS_V2.md): tool contracts, capabilities, concurrency guards, runtime ownership, and result/error shapes.
- [Architecture](../../editor/debugger/godot_mcp/ARCHITECTURE.md): transport, sessions, resource state, and runtime modules.

The contracts describe intended behavior. Check [MCP status](../../MCP_STATUS.md) for known deviations before relying on mutations, save conflicts, or best-effort patches.

## Troubleshooting

- `SESSION_ERROR: Session file not found`: open the selected project in the rebuilt editor and enable `network/godot_mcp/enabled`.
- `SESSION_ERROR: Session metadata is expired`: restart or toggle the editor MCP setting.
- `TRANSPORT_ERROR: Could not connect`: confirm a running editor owns the UUID/port in the selected session file. The bridge retries with bounded backoff.
- `STALE_SESSION`: endpoint and metadata identify different editor sessions. Let discovery settle and refresh editor object state.
- `STALE_RUN`: start a new runtime and use its returned `run_id`; identities do not survive another run or editor session.
- Configuration export cannot find the bridge or Node: build the source bridge, check the portable layout, or set the advanced paths explicitly.

The editor endpoint binds to `127.0.0.1`.
