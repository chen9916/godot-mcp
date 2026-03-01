# OpenCode MCP (Godot-side)

This module exposes an editor-hosted JSON-RPC endpoint for the OpenCode MCP bridge.
It is intended for local, live editing flows in the Godot editor (scene, node, and script operations).

## Lifecycle and settings

- Plugin type: `EditorPlugin` (`OpenCodeMCPServer`).
- Bind address: `127.0.0.1` only.
- Settings group: `network/opencode_mcp`.
  - `enabled` (`bool`, default `false`) starts/stops the server.
  - `Refresh MCP Session` button (in Editor Settings) rewrites session metadata immediately.
- Server state is refreshed when the plugin enters tree and whenever this settings group changes.
- While running, the server periodically rewrites session metadata so `expires_at` stays fresh during long idle editor sessions.

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

Scene graph:

- `read_scene`: `scene.get_active`, `scene.get_tree`, `scene.list`, `node.get_property`, `node.list_properties`, `node.get_properties`, `node.get_properties_batch`, `node.find`, `node.get_groups`, `signal.list`, `signal.get_connections`, `theme.get_overrides`
- `write_scene`: `scene.open`, `scene.create`, `scene.instantiate`, `node.create`, `node.create_batch`, `node.create_from_template`, `node.delete`, `node.delete_batch`, `node.duplicate`, `node.duplicate_batch`, `node.reparent`, `node.set_properties`, `node.set_properties_batch`, `node.set_groups`, `signal.connect`, `signal.disconnect`, `theme.set_overrides`, `script.attach_external`, `script.attach_built_in`, `script.attach` (legacy/disabled)

Script and shader:

- `read_script`: `script.get_active`, `script.get`, `shader.get`, `lsp.query`
- `write_script`: `script.apply_text_edits`, `shader.edit`

Resources:

- `save_resource`: `resource.save`, `resource.reload`
- `read_resource`: `resource.get`, `resource.list`
- `write_resource`: `resource.create`, `resource.set_properties`

Project and editor:

- `read_project`: `project.get_setting`, `editor.get_errors`
- `write_project`: `project.set_setting`

Documentation:

- `read_docs`: `docs.class_lookup`, `docs.member_lookup`, `docs.search`, `docs.inheritance`, `docs.examples`, `docs.list_versions`

## Methods (v1)

### Session

- `opencode.session.info`
  - Returns transport metadata and advertised capabilities.

### Scene management

- `scene.get_active`
  - Returns active edited scene identity (name, scene path, root node path, class).
- `scene.get_tree`
  - Returns bounded tree snapshot from edited scene root.
  - Optional params: `root_path` (default `"."`), `max_depth` (default `6`), `max_nodes` (default `1500`).
- `scene.list`
  - Lists all `.tscn`/`.scn` files under a directory.
  - Optional params: `directory` (default `res://`), `recursive` (default `true`).
- `scene.open`
  - Opens a scene by `res://` path and makes it the active edited scene.
  - Required param: `path`.
- `scene.create`
  - Creates a new in-memory scene with a given root node type (like File -> New Scene).
  - Required param: `root_type`. Optional param: `root_name`.
  - Scene must be saved with `resource.save` to persist.
- `scene.instantiate`
  - Instantiates a packed scene as a child of an existing node.
  - Required params: `scene_path`, `parent_path`.
  - Optional params: `name`, `position`.

### Node operations

- `node.create`
  - Required params: `parent_path`, `type`.
  - Optional params: `name`, `position`, `properties`.
- `node.get_properties`
  - Required params: `node_path`, non-empty `property_names` array.
  - Returns a dictionary of requested property values.
  - Rejects unknown property names.
  - Resource-valued object properties are serialized as `{ "class", "path", "uid" }` when available.
  - Non-resource object values remain non-serializable and return `INVALID_ARGUMENT`.
- `node.get_properties_batch`
  - Required param: non-empty `items` array of `{ node_path, property_names[] }`.
  - Optional per-item `item_id` is echoed in item results.
  - Read-only batch operation; returns per-item `ok/data/error` entries.
  - Resource-valued object properties are serialized as `{ "class", "path", "uid" }` when available.
- `node.get_property`
  - Required params: `node_path`, `property_name`.
  - Returns one property value with its Variant type name.
  - Rejects unknown properties.
  - Resource-valued object properties are serialized as `{ "class", "path", "uid" }` when available.
  - Non-resource object values remain non-serializable and return `INVALID_ARGUMENT`.
- `node.list_properties`
  - Required param: `node_path`.
  - Returns node property discovery metadata as `{ name, type }` entries.
  - Keeps `node.get_properties` unchanged for bulk value reads.
- `node.delete`
  - Required params: `node_path`, `force=true`.
  - Root/internal nodes are rejected.
- `node.create_batch`
  - Required params: non-empty `items` array, each with `item_id`, `type`, and exactly one of `parent_path` or `parent_item_id`.
  - Optional top-level params: `mode` (`atomic` or `best_effort`, default `atomic`), `preview_only` (default `false`).
  - Optional item params: `name`, `position`, `properties`, `property_entries`.
  - Items are processed in order. `parent_item_id` can reference a previously successful item in the same batch.
  - Returns per-item results and aggregate counts (`success_count`, `failure_count`, `skipped_count`).
- `node.create_from_template`
  - Syntactic sugar over `node.create_batch` for composite objects.
  - Required params: `parent_path`, `root_name`, non-empty `nodes` array.
  - Optional params: `root_type` (default `Node`), `mode`, `preview_only`, root and per-item property payloads.
  - Returns the same batch response shape as `node.create_batch`.
- `node.duplicate`
  - Required param: `source_path`.
  - Optional params: `parent_path`, `new_name`, `position`, `property_overrides`, `property_entries`.
  - Duplicates in editor context and applies validated root-node overrides.
- `node.duplicate_batch`
  - Required params: non-empty `items` array, each with `item_id` and `source_path`.
  - Optional top-level param: `mode` (`atomic` or `best_effort`, default `atomic`).
  - Optional item params: `parent_path`, `parent_item_id`, `new_name`, `position`, `property_overrides`, `property_entries`.
  - Returns per-item results and aggregate counts.
- `node.delete_batch`
  - Required params: non-empty `items` array and `force=true`.
  - Optional top-level param: `mode` (`atomic` or `best_effort`, default `atomic`).
  - Optional per-item `item_id` is echoed in item results.
  - Deletes are applied deepest-first so child nodes are removed before their parents.
- `node.reparent`
  - Required params: `node_path`, `new_parent_path`.
  - Optional params: `position`, `new_name`.
- `node.set_properties`
  - Required param: `node_path` plus one of:
    - non-empty `properties` dictionary, or
    - non-empty `property_entries` array (`[{ "name": "prop", "value": ... }]`) for clients that cannot send free-form maps.
  - For `Color` properties, accepts native `Color`, `Color(r,g,b[,a])` string, html hex strings, `[r,g,b[,a]]`, or `{r,g,b[,a]}`.
  - Invalid color strings fail with `INVALID_ARGUMENT` (no silent fallback).
  - For `Vector3` properties (for example `CSGBox3D.size`), accepts native `Vector3`, `{ "x", "y", "z" }`, or `[x, y, z]` numeric inputs.
  - For `Node3D`, rejects invalid transform writes before applying:
    - `scale` with any zero component,
    - non-invertible `basis` / `global_basis`,
    - `transform` / `global_transform` with non-invertible basis.
- `node.set_properties_batch`
  - Required params: non-empty `items` array, each with `node_path` plus one of `properties` or `property_entries`.
  - Optional top-level param: `mode` (`atomic` or `best_effort`, default `atomic`).
  - Optional per-item `item_id` is echoed in item results.
  - Uses the same coercion and validation rules as `node.set_properties`.
  - Returns per-item `ok/data/error` plus aggregate counts.
- `node.find`
  - Searches nodes by name pattern, type, group across the edited scene tree.
  - Optional params: `pattern` (default `*`), `type`, `group`, `limit` (default `100`), `owned` (default `true`).
- `node.get_groups`
  - Lists groups a node belongs to (filters internal groups).
  - Required param: `node_path`.
- `node.set_groups`
  - Adds/removes groups on a node in one undoable operation.
  - Required param: `node_path`. Optional params: `add` (string array), `remove` (string array).

### Script operations

- `script.get`
  - If `script_path`/`node_path` are omitted, resolves from workspace:
    - `script_editor`: active script tab.
    - `scene_view`: script attached to the selected scene node.
    - `auto` (default): follows the currently selected main screen.
  - Optional param: `workspace` (`auto`, `scene_view`, `script_editor`).
  - In `script_editor` workspace, source/version come from the visible active editor buffer.
  - If `workspace=script_editor` and `script_path` is provided, it must match the active tab script path or returns conflict.
  - Outside `script_editor` workspace, with params resolves by `script_path` or `node_path` (for attached script lookup).
  - Returns source text and version hash (`md5`).
- `script.get_active`
  - Optional param: `workspace` (`auto`, `scene_view`, `script_editor`).
  - Returns `workspace` in the payload.
  - `script_editor` returns active script tab (`script_path`, `display_name`, `is_built_in`, `is_unsaved`) plus source text and version hash (`md5`) from the active editor buffer.
  - `scene_view` returns selected-node script (`script_path`, `node_path`, `is_built_in`, `is_unsaved=false`) plus source text and version hash (`md5`).
- `script.apply_text_edits`
  - Required param: `edits`.
  - Script target can be provided with `script_path` or `node_path`.
  - If both are omitted, target resolves from `workspace` (`auto`, `scene_view`, `script_editor`; default `auto`).
  - In `script_editor` workspace, edits are applied to the active editor buffer first, then synced to the script resource.
  - If `workspace=script_editor` and `script_path` is provided, it must match the active tab script path or returns conflict.
  - Optional `expected_version` enables optimistic concurrency check.
  - In `script_editor` workspace, `expected_version` compares against the active editor buffer version hash.
  - Edit coordinates are zero-based line/column (`start_line`, `start_col`, `end_line`, `end_col`).
  - Rejects empty edit arrays and no-op edits (returns conflict when resulting source is unchanged).
  - On success, source is updated in editor state; call `resource.save` to persist to disk.
- `script.attach`
  - Unified attach entrypoint.
  - If `built_in` is provided, routes to `script.attach_built_in`.
  - Otherwise routes to `script.attach_external`.
  - In `script_editor` workspace, if `script_path` is omitted, it defaults to the active script tab path.
- `script.attach_external`
  - Attaches an existing external script file to a node.
  - Required param: `script_path` (`res://` file path, must not be `::` built-in path).
  - Optional params: `node_path`, `workspace` (`auto`, `scene_view`, `script_editor`; default `auto`).
- `script.attach_built_in`
  - Creates and attaches a built-in script to a node.
  - Required param: non-empty `built_in` dictionary.
  - Optional params: `node_path`, `workspace` (`auto`, `scene_view`, `script_editor`; default `auto`).
  - Refuses replacing an existing external script with a built-in script.
  - Built-in script attach requires the scene to already be saved.
  - Returns warning to save the scene with `resource.save` to persist.
- Editing vs attaching
  - Use `script.apply_text_edits` to change script source text.
  - Use `script.attach_external` / `script.attach_built_in` only to change script assignment on a node.
- `lsp.query`
  - Runs GDScript-aware LSP-style lookups against the current editor script source.
  - Required param: `operation` (`goToDefinition`, `findReferences`, `hover`, `documentSymbol`).
  - Target script resolution accepts `script_path` (or `filePath`), `node_path`, and `workspace` (`auto`, `scene_view`, `script_editor`).
  - `line` and `character` are required for position-based operations and are zero-based.
  - `findReferences` optional params:
    - `include_declaration` (default `true`),
    - `workspace_search` (default `false`),
    - `search_root` (default `res://`),
    - `file_limit` (default `300`).
  - Currently supports GDScript targets only.

### Resource operations

- `resource.save`
  - Optional param: `path` (`res://`).
  - If omitted, active scene path is used.
  - Returns `CONFLICT` if disk contents changed since editor-loaded state; call `resource.reload` first.
- `resource.reload`
  - Optional param: `path` (`res://`).
  - If omitted, active scene path is used.
  - Reloads scene/resources from disk into editor state.
- `resource.get`
  - Inspects a resource's serializable properties.
  - Required param: `path`. Optional param: `property_names` (array; omit for all serializable properties).
  - Resource-valued properties are returned as references: `{ "class", "path", "uid" }` when available.
- `resource.list`
  - Lists resource files in a directory with optional extension filter.
  - Optional params: `directory` (default `res://`), `extensions` (string array), `recursive` (default `true`), `limit` (default `500`).
- `resource.create`
  - Creates a new resource in memory by class name.
  - Required param: `type`. Optional params: `path`, `properties`, `property_entries`.
  - `properties` and `property_entries` are mutually exclusive.
  - Type coercion accepts deterministic shapes for common types:
    - `Vector2`/`Vector3`/`Vector4`: native, array, or `{x,y[,z[,w]]}` dictionary.
    - `Color`: native, `Color(r,g,b[,a])`, html hex string, array, or `{r,g,b[,a]}` dictionary.
    - Resource refs: `res://`/`uid://` string, `{path}`, `{uid}`, or `null`.
  - Must call `resource.save` to persist.
- `resource.set_properties`
  - Modifies flat properties on a loaded resource.
  - Required param: `path` plus one of:
    - non-empty `properties` dictionary, or
    - non-empty `property_entries` array (`[{ "name": "prop", "value": ... }]`).
  - `properties` and `property_entries` are mutually exclusive.
  - Uses the same deterministic coercion rules as `resource.create`.
  - Wrapped in undo/redo. Call `resource.save` to persist.

### Signal operations

- `signal.list`
  - Lists all signals available on a node (built-in and script-defined).
  - Required param: `node_path`.
  - Returns signal names with argument metadata (name, type).
- `signal.get_connections`
  - Returns existing signal connections on a node.
  - Required param: `node_path`. Optional param: `signal_name` (filter).
  - Returns signal name, target path, method name, and connection flags.
- `signal.connect`
  - Connects a signal to a method on a target node.
  - Required params: `node_path`, `signal_name`, `target_path`, `method`.
  - Optional param: `flags` (default `0`).
  - Rejects if signal does not exist or connection already exists.
- `signal.disconnect`
  - Removes a signal connection.
  - Required params: `node_path`, `signal_name`, `target_path`, `method`.
  - Rejects if connection does not exist.

### Project and editor

- `project.get_setting`
  - Reads project settings by key.
  - Required param: `keys` (non-empty string array).
  - Returns a dictionary of key-value pairs.
- `project.set_setting`
  - Modifies project settings and auto-saves to `project.godot`.
  - Required param: `settings` (non-empty dictionary).
  - No undo support (matches editor behavior).
- `editor.get_errors`
  - Returns recent errors and warnings captured by the error handler.
  - Optional params: `types` (default `["error", "warning"]`), `limit` (default `50`), `clear` (default `false`).
  - Messages include type, text, file, function, line number, and timestamp.

### Documentation

- `docs.list_versions`
  - Lists docs version aliases accepted by this runtime.
  - Optional param: `version` (validation-only alias check).
- `docs.class_lookup`
  - Returns class docs from Godot's runtime help database (`EditorHelp`/`DocTools`).
  - Required param: `class_name`. Optional param: `version`.
  - Includes class payload, inheritance children (`inherited_by`), member counts, and docs URL.
- `docs.member_lookup`
  - Returns member details for a class/member pair, with optional inherited lookup.
  - Required params: `class_name`, `member_name`.
  - Optional params: `kind` (`method`, `constructor`, `operator`, `signal`, `property`, `constant`, `annotation`, `theme_item`, `enum`), `include_inherited` (default `true`), `version`.
- `docs.search`
  - Fuzzy-ish search across classes and members using help-style matching rules.
  - Required param: `query`.
  - Optional params: `limit` (default `50`), `include_members` (default `true`), `version`.
- `docs.inheritance`
  - Returns parent chain, children/descendants, and inherited member summaries.
  - Required param: `class_name`.
  - Optional params: `include_descendants` (default `true`), `descendants_limit` (default `500`), `include_inherited_members` (default `true`), `version`.
- `docs.examples`
  - Extracts short code snippets from class/member docs BBCode blocks.
  - Requires one of: `class_name` or `topic`.
  - Optional params: `language` (`any`, `gdscript`, `csharp`, `text`; default `any`), `limit` (default `8`), `version`.

### Shader and theme

- `shader.get`
  - Reads shader source code from a `.gdshader` or shader resource.
  - Required param: `path`.
  - Returns source, shader type (spatial/canvas_item/particles/sky/fog), and version hash.
- `shader.edit`
  - Replaces shader source code (full replacement).
  - Required params: `path`, `source`.
  - Optional param: `expected_version` (md5 conflict check).
  - Wrapped in undo/redo. Call `resource.save` to persist.
- `theme.get_overrides`
  - Inspects theme overrides on a Control node.
  - Required param: `node_path`. Optional param: `override_type` (filter by category).
  - Returns overrides grouped by category: colors, constants, font_sizes, fonts, icons, styleboxes.
- `theme.set_overrides`
  - Sets or removes theme overrides on a Control node.
  - Required params: `node_path`, `overrides` (nested object with categories).
  - Set a value to `null` to remove an override.
  - Wrapped in undo/redo.

Scene/node mutations, resource property changes, signal connections, shader edits, and theme overrides are wrapped through `EditorUndoRedoManager` actions.

## Error model

Method failures return JSON-RPC errors. Besides standard JSON-RPC codes, MCP-oriented server codes are:

- `-32002`: capability denied
- `-32003`: invalid argument
- `-32004`: conflict
- `-32005`: not found
- `-32006`: internal error

## OpenCode bridge setup

The Godot endpoint is JSON-RPC over localhost TCP and is meant to be consumed by a stdio MCP bridge.

Bridge package location in this repository:

- `misc/opencode_mcp_bridge`

Typical workflow:

1. Build the bridge package (`npm ci && npm run build` in `misc/opencode_mcp_bridge`).
2. Install the OpenCode config entry:
   - `node dist/main.js --install-opencode-config`
   - Optional: add `--project-root "<path-to-godot-project>"` when OpenCode does not start from your project root.
3. Restart OpenCode to discover the new MCP server entry.

The installed config entry key is `mcp.godot-opencode` and launches the bridge in stdio mode.
