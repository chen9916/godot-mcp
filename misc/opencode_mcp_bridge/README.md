# OpenCode Godot MCP bridge

This package exposes a local MCP stdio server for OpenCode and forwards tool calls to Godot's editor-hosted JSON-RPC endpoint.

## What it does

- Reads Godot session metadata from `.godot/opencode_mcp/session.json`.
- Connects to `127.0.0.1:<session-port>` and forwards tool calls to Godot JSON-RPC.
- Exposes `godot.*` MCP tools for scene/node/script/resource operations.
- Can install a local OpenCode MCP config entry for testing.

## Tool mapping

- `godot.scene.get_active` -> `scene.get_active`
- `godot.scene.get_tree` -> `scene.get_tree`
- `godot.node.create` -> `node.create`
- `godot.node.delete` -> `node.delete`
- `godot.node.reparent` -> `node.reparent`
- `godot.node.set_properties` -> `node.set_properties`
- `godot.script.get` -> `script.get`
- `godot.script.apply_text_edits` -> `script.apply_text_edits`
- `godot.script.attach` -> `script.attach`
- `godot.resource.save` -> `resource.save`

## Prerequisites

- Node.js 18+
- Godot editor running with OpenCode MCP enabled:
  - `Editor Settings -> network/opencode_mcp/enabled = true`

## Install and build

```bash
npm ci
npm run typecheck
npm run build
```

## Run bridge manually

From this folder:

```bash
node dist/main.js
```

Session discovery defaults to current working directory and walks upward to find `.godot/opencode_mcp/session.json`.

Optional flags:

- `--project-root "<path-to-godot-project>"`
- `--session-file "<absolute-path-to-session.json>"`
- `--timeout-ms 15000`

## Install OpenCode MCP config (testing)

This command upserts `mcp.godot-opencode-test` into `%USERPROFILE%/.config/opencode/opencode.json` with a local command entry.

```bash
node dist/main.js --install-opencode-config
```

Optional: add `--project-root "<path-to-godot-project>"` to pin a project explicitly.

Optional config target override:

```bash
node dist/main.js --install-opencode-config --config-path "C:/Users/<you>/.config/opencode/opencode.json"
```

The installer is idempotent and creates a timestamped backup before writing.

## Manual OpenCode config entry (fallback)

```json
{
  "mcp": {
    "godot-opencode-test": {
      "type": "local",
      "command": [
        "node",
        "C:/.../misc/opencode_mcp_bridge/dist/main.js"
      ],
      "enabled": true
    }
  }
}
```

## Troubleshooting

- `SESSION_ERROR: Session file not found`
  - Open the project in Godot and enable `network/opencode_mcp/enabled`.
- `SESSION_ERROR: Session metadata is expired`
  - Restart or toggle the MCP setting in the editor.
- `TRANSPORT_ERROR: Could not connect`
  - Confirm the editor is running and `session.json` port matches the active session.

## Security notes

- Godot endpoint is localhost-only (`127.0.0.1`).
