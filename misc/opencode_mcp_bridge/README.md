# OpenCode Godot MCP bridge

This package exposes a local MCP stdio server for OpenCode and forwards tool calls to Godot's editor-hosted JSON-RPC endpoint.

## What it does

- Reads Godot session metadata from `.godot/opencode_mcp/session.json`.
- Connects to `127.0.0.1:<session-port>` and forwards tool calls to Godot JSON-RPC.
- Exposes `godot.*` MCP tools for scene/node/script/resource operations.
- Can install a local OpenCode MCP config entry.

## Tool mapping

### Scene management
- `godot.scene.get_active` -> `scene.get_active`
- `godot.scene.get_tree` -> `scene.get_tree`
- `godot.scene.list` -> `scene.list`
- `godot.scene.open` -> `scene.open`
- `godot.scene.create` -> `scene.create`
- `godot.scene.instantiate` -> `scene.instantiate`

### Node operations
- `godot.node.get_property` -> `node.get_property`
- `godot.node.list_properties` -> `node.list_properties`
- `godot.node.get_properties` -> `node.get_properties`
- `godot.node.create` -> `node.create`
- `godot.node.delete` -> `node.delete`
- `godot.node.reparent` -> `node.reparent`
- `godot.node.set_properties` -> `node.set_properties`
- `godot.node.find` -> `node.find`
- `godot.node.get_groups` -> `node.get_groups`
- `godot.node.set_groups` -> `node.set_groups`

### Script operations
- `godot.script.get_active` -> `script.get_active`
- `godot.script.get` -> `script.get`
- `godot.script.apply_text_edits` -> `script.apply_text_edits`
- `godot.script.attach` -> `script.attach`
- `godot.lsp` -> `lsp.query`

### Resource operations
- `godot.resource.save` -> `resource.save`
- `godot.resource.get` -> `resource.get`
- `godot.resource.list` -> `resource.list`
- `godot.resource.create` -> `resource.create`
- `godot.resource.set_properties` -> `resource.set_properties`

### Signal operations
- `godot.signal.list` -> `signal.list`
- `godot.signal.get_connections` -> `signal.get_connections`
- `godot.signal.connect` -> `signal.connect`
- `godot.signal.disconnect` -> `signal.disconnect`

### Project and editor
- `godot.project.get_setting` -> `project.get_setting`
- `godot.project.set_setting` -> `project.set_setting`
- `godot.editor.get_errors` -> `editor.get_errors`

### Shader and theme
- `godot.shader.get` -> `shader.get`
- `godot.shader.edit` -> `shader.edit`
- `godot.theme.get_overrides` -> `theme.get_overrides`
- `godot.theme.set_overrides` -> `theme.set_overrides`

## Capabilities

Tools are gated by session capabilities:

- `read_scene`: scene/node/signal/theme reading
- `write_scene`: scene/node/signal/theme mutations
- `read_script`: script/shader reading and GDScript LSP queries
- `write_script`: script and shader editing
- `save_resource`: resource saving
- `read_resource`: resource inspection and listing
- `write_resource`: resource creation and property modification
- `read_project`: project settings and editor errors
- `write_project`: project settings modification

## Workspace-aware operations

`godot.script.get_active` and `godot.script.get` support workspace-aware resolution via `workspace` (`auto`, `scene_view`, `script_editor`).
In `script_editor` workspace, source/version come from the active tab buffer.
`godot.script.apply_text_edits` requires non-empty `edits`, and can resolve the target from `script_path`, `node_path`, or `workspace` when both are omitted.
In `script_editor` workspace, edits apply to the active tab buffer first, then sync to the script resource.
Edit and LSP coordinates are zero-based (`line`, `character`, `start_line`, `start_col`, `end_line`, `end_col`).
When `workspace=script_editor` and `script_path` is provided, it must match the active tab script path or the call returns conflict.
`godot.script.attach` supports workspace-aware defaults (`node_path` from selected node, `script_path` from active tab in `script_editor` workspace).
`godot.lsp` supports `goToDefinition`, `findReferences`, `hover`, and `documentSymbol` for GDScript, and can resolve target scripts through `script_path`/`filePath`, `node_path`, or `workspace`.
`godot.node.set_properties` accepts either `properties` (free-form object) or `property_entries` (`[{ name, value }]`) for clients that cannot send dynamic maps.
After successful script/shader edits, call `godot.resource.save` to persist changes to disk.

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

## Install OpenCode MCP config

This command upserts `mcp.godot-opencode` into `%USERPROFILE%/.config/opencode/opencode.json` with a local command entry.

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
    "godot-opencode": {
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
  - In Editor Settings (`network/opencode_mcp`), click `Refresh MCP Session`, or toggle `enabled` off/on.
- `TRANSPORT_ERROR: Could not connect`
  - Confirm the editor is running and `session.json` port matches the active session.

## Security notes

- Godot endpoint is localhost-only (`127.0.0.1`).
