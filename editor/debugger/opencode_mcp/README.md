# OpenCode MCP (Godot-side)

This module exposes an editor-hosted JSON-RPC endpoint for the OpenCode MCP bridge.
It is intended for local, live editing flows in the Godot editor (scene, node, and script operations).

## Lifecycle and settings

- Plugin type: `EditorPlugin` (`OpenCodeMCPServer`).
- Bind address: `127.0.0.1` only.
- Settings group: `network/opencode_mcp`.
  - `enabled` (`bool`, default `false`) starts/stops the server.
  - `remote_port` (`int`, default `0`) requests a port; `0` means OS-assigned ephemeral port.
- Server state is refreshed when the plugin enters tree and whenever this settings group changes.

## Transport and protocol

- Protocol: JSON-RPC 2.0.
- Framing: one JSON object per line (`\n` delimited).
- Connection model: multiple localhost TCP clients are accepted and polled from the editor main thread.

Example request:

```json
{"jsonrpc":"2.0","id":1,"method":"scene.get_active","params":{}}
```

Example successful response:

```json
{"jsonrpc":"2.0","id":1,"result":{"ok":true,"data":{"name":"Main"},"warnings":[]}}
```

## Session discovery

When the server starts, it writes session metadata to:

- `res://.godot/opencode_mcp/session.json`

Session file payload includes:

- `port`
- `pid`
- `expires_at`
- `capabilities`

The session file is removed when the server stops.

## Capability checks

- Every method is available on localhost without per-request auth.
- A method still requires its capability flag to be enabled in the current session.

Capability map:

- `read_scene`: `scene.get_active`, `scene.get_tree`
- `write_scene`: `node.create`, `node.delete`, `node.reparent`, `node.set_properties`
- `read_script`: `script.get`
- `write_script`: `script.apply_text_edits`, `script.attach`
- `save_resource`: `resource.save`

## Methods (v1)

- `opencode.session.info`
  - Returns transport metadata and advertised capabilities.
- `scene.get_active`
  - Returns active edited scene identity (name, scene path, root node path, class).
- `scene.get_tree`
  - Returns bounded tree snapshot from edited scene root.
  - Optional params: `max_depth` (default `6`), `max_nodes` (default `1500`).
- `node.create`
  - Required params: `parent_path`, `type`.
  - Optional params: `name`, `position`, `properties`.
- `node.delete`
  - Required params: `node_path`, `force=true`.
  - Root/internal nodes are rejected.
- `node.reparent`
  - Required params: `node_path`, `new_parent_path`.
  - Optional params: `position`, `new_name`.
- `node.set_properties`
  - Required params: `node_path`, non-empty `properties` dictionary.
- `script.get`
  - Required either `script_path` or `node_path` (for attached script lookup).
  - Returns source text and version hash (`md5`).
- `script.apply_text_edits`
  - Required params: `script_path`, `edits`.
  - Optional `expected_version` enables optimistic concurrency check.
  - Edit coordinates are zero-based line/column (`start_line`, `start_col`, `end_line`, `end_col`).
- `script.attach`
  - Required params: `node_path`, `script_path` (`res://`).
- `resource.save`
  - Optional param: `path` (`res://`).
  - If omitted, active scene path is used.

All mutating operations are wrapped through `EditorUndoRedoManager` actions.

## Error model

Method failures return JSON-RPC errors. Besides standard JSON-RPC codes, MCP-oriented server codes are:

- `-32002`: capability denied
- `-32003`: invalid argument
- `-32004`: conflict
- `-32005`: not found
- `-32006`: internal error

## OpenCode bridge setup (local testing)

The Godot endpoint is JSON-RPC over localhost TCP and is meant to be consumed by a stdio MCP bridge.

Bridge package location in this repository:

- `misc/opencode_mcp_bridge`

Typical workflow:

1. Build the bridge package (`npm ci && npm run build` in `misc/opencode_mcp_bridge`).
2. Install the OpenCode test config entry:
   - `node dist/main.js --install-opencode-config`
   - Optional: add `--project-root "<path-to-godot-project>"` when OpenCode does not start from your project root.
3. Restart OpenCode to discover the new MCP server entry.

The installed config entry key is `mcp.godot-opencode-test` and launches the bridge in stdio mode.
