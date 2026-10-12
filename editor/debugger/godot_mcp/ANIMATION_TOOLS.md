# Animation tools for coding agents

The animation suite operates on Godot's actual `AnimationPlayer`, `AnimationTree`,
`AnimationLibrary`, `Animation`, and `AnimationNode` resources. It does not drag
editor widgets, generate scene text, or run arbitrary editor code.

Use the standard stdio bridge and a matching rebuilt editor. The nine added tools
bring the registry to 83 tools. A running older editor must be restarted into the
new build, and the coding-agent MCP connection must reload the updated bridge.
Existing packaged archives are not updated by a source build.
The editor response to `godot.session.info` includes
`animation_schema_version: 1` in a build which implements this suite. An older
editor may still permit tool discovery through the new bridge but cannot execute
the new native methods until restarted into the matching build.

## Tools

| MCP tool | Purpose | Capability |
| --- | --- | --- |
| `godot.animation.schema` | Offline operation, value, and runtime-action schemas | `read_scene` |
| `godot.animation.list` | Discover mixers, libraries, clip names and metadata | `read_scene` |
| `godot.animation.get` | Read one clip's tracks and optionally keyframes | `read_scene` |
| `godot.animation.apply_patch` | Edit libraries, clips, tracks, and keys atomically | `write_scene` |
| `godot.animation.graph_get` | Read nested graph topology and settings | `read_scene` |
| `godot.animation.graph_apply_patch` | Build and edit animation graphs atomically | `write_scene` |
| `godot.animation.graph_validate` | Find graph/binding/clip problems | `read_scene` |
| `godot.runtime.animation_get` | Read playback, parameters, state-machine state, root motion | `runtime_inspection` |
| `godot.runtime.animation_control` | Control playback and typed animation parameters | `runtime_inspection` |

Call `godot.animation.schema` first when an agent needs operation details. It
returns both complete discriminated operation unions and runtime actions without
connecting to an editor. Individual MCP tool schemas also describe their inputs.
The bridge validates animation-tool arguments against these schemas before
forwarding them. Native handlers additionally enforce engine-specific types,
scene/run guards, containment, and bounded values.

## Safe authoring workflow

1. Inspect the active scene and available clips. Do not assume clip names, track
   paths, the skeleton, or an existing `AnimationTree` binding.
2. Use the ordinary `scene.apply_patch` tool to create scene nodes if necessary.
   An `AnimationNode` is a **Resource**, not a scene node.
3. Read the latest `scene_version` and submit an animation patch with
   `expected_scene_path`, `expected_scene_version`, and scene-relative `node_path`.
4. Preview with `dry_run: true`. A dry run makes no undo action or saved change.
5. Submit the real patch using the same guards if the scene has not changed.
   All operations execute in order on local resource copies. A late invalid
   operation rejects the entire patch. A successful patch creates one undo action.
6. Inspect and validate. Refresh the version before another mutation; the previous
   version is intentionally stale after a successful edit.
7. Save the **scene** explicitly with `godot.resource.save`. Animation patches do
   not silently save or overwrite external/imported animation assets.
8. Start an MCP-owned runtime and test playback. Runtime changes are not editor
   changes and are not saved back to the scene.

Local copies avoid modifying other characters which share an imported library or
animation graph. Undo restores the original resource references. Disk-save
conflict handling is the same as the existing guarded scene-save workflow.

Editor paths such as `AnimationTree` are relative to the edited scene root.
Runtime paths such as `/root/Character/AnimationTree` belong only to the specified
owned `run_id`. Graph paths such as `Locomotion/Walk` are relative to `tree_root`;
the empty graph path identifies the graph root. These path spaces are distinct.

## Graph operations

`graph_apply_patch` accepts up to 512 ordered operations:

- `set_root`: create a root graph resource by concrete `AnimationRootNode` type.
- `add_node`, `remove_node`, `rename_node`, `move_node`: edit BlendTree nodes or
  StateMachine states, including nested containers.
- `set_node_properties`: configure a graph resource by graph-relative `node_path`.
  Use this for clip selection, one-shot fades, blend-space limits, sync modes, and
  other declared properties. TimeScale values use parameter controls instead.
- `connect`, `disconnect`: edit a BlendTree target input. `from` is the source
  output node, `to` is the target node, and `input` is the target's input index.
- `add_transition`, `set_transition`, `remove_transition`: edit StateMachine
  transitions by their `from`/`to` states. Transition properties include blend
  time, switch/advance mode, condition, expression, and priority.
- `add_blend_point`, `set_blend_point`, `remove_blend_point`: edit named 1D/2D
  points. A 1D position is a number; a 2D position is `[x, y]`.
- `set_triangles`: set manual 2D triangles as arrays of three point indices.
- `set_filter`: replace a node's filter-path mask and enabled flag.

Graph creation uses the structural Godot APIs, rather than writing undocumented
`nodes/Name/node` or `blend_point_0/node` serialization properties. Invalid
classes, duplicate names, invalid ports and indices, and unsafe property writes
are rejected before committing.

The graph limits are 2,000 inspected nodes, 32 levels, 64 ports/blend points per
container, and 512 transitions/triangles/filter paths. Native graph classes are
supported; custom scripted graph nodes and executable/cyclic subresource data
are rejected rather than run during planning. Existing non-executable graph
subresources are copied, but graph patches do not accept object/resource
retargeting (object properties may be cleared with null). Runtime blend/TimeScale
values are parameters, not graph resource properties. Nested resource-property
and skeleton-bone target validation is partial; inspect the actual rig separately.
Ordinary non-`@tool` controller scripts on AnimationTree nodes are supported:
their non-executing editor placeholders, script references, and exported values
remain attached during graph edits, dry runs, and undo/redo. Parameter inspection
also supports these controllers. Editor-executing scripts (`@tool` or other live
script instances) on any scene AnimationTree are rejected before graph authoring
can enumerate scene properties; custom scripted AnimationNode resources remain
unsupported. Renaming nodes or named blend-point subtrees preserves writable
authored parameter values in the same undo action; runtime playback/observer
handles are never copied. Scripted or
dynamic track-target properties are reported as partial validation without
executing their getters. Scene-version preflight rejects cyclic, scripted, or
over-limit graph hierarchies before dynamic parameter enumeration.
Manual blend-space topology replaces its serialized value on reload. Loaded
Transition nodes initialize their default input per AnimationTree instance,
including graphs shared by multiple trees, without requiring a synthetic request.

### Locomotion blend with an attack one-shot

After inspecting that the player has clips `idle`, `run`, and `attack`, submit:

```json
{
  "expected_scene_path": "res://character.tscn",
  "expected_scene_version": "<latest scene_version>",
  "node_path": "AnimationTree",
  "dry_run": true,
  "operations": [
    {"op": "set_root", "type": "AnimationNodeBlendTree"},
    {"op": "add_node", "name": "Locomotion", "type": "AnimationNodeBlendSpace1D", "position": [0, 0], "properties": {"min_space": 0, "max_space": 1}},
    {"op": "add_blend_point", "graph_path": "Locomotion", "name": "Idle", "position": 0, "properties": {"animation": "idle"}},
    {"op": "add_blend_point", "graph_path": "Locomotion", "name": "Run", "position": 1, "properties": {"animation": "run"}},
    {"op": "add_node", "name": "Attack", "type": "AnimationNodeAnimation", "position": [0, 200], "properties": {"animation": "attack"}},
    {"op": "add_node", "name": "AttackOneShot", "type": "AnimationNodeOneShot", "position": [300, 0], "properties": {"fadein_time": 0.1, "fadeout_time": 0.15}},
    {"op": "connect", "from": "Locomotion", "to": "AttackOneShot", "input": 0},
    {"op": "connect", "from": "Attack", "to": "AttackOneShot", "input": 1},
    {"op": "connect", "from": "AttackOneShot", "to": "output", "input": 0}
  ]
}
```

Then configure the scene's `AnimationTree.anim_player`, `root_node`, `active`, and
default `parameters/Locomotion/blend_position` using the existing node-property
or scene-patch tools. Bindings and animation track paths must match the actual
scene hierarchy; validation is not a substitute for inspecting the character.

## Clip, track, and key operations

`animation.apply_patch` edits the target `AnimationMixer` (normally an
`AnimationPlayer`) with the same scene guards and atomic/dry-run semantics:

- Libraries: `add_library`, `remove_library`, `rename_library`.
- Clips: `create_animation`, `remove_animation`, `rename_animation`,
  `duplicate_animation`, `set_animation_properties`, `clear_animation`.
- Tracks: `add_track`, `remove_track`, `set_track`, `move_track`.
- Keys: `insert_key`, `set_key`, `remove_key`, `set_bezier_handles`.

Qualified animation names are `library/clip`; the unnamed library uses just
`clip`. Library operations use `library: ""` for that unnamed library. Renames
within a library take a new unqualified clip name. Graph clip references must be
updated separately when clips are renamed or deleted.

`move_track.to_index` means the final zero-based position. `insert_key` replaces a
key at an existing time; changing a key's time with `set_key` rejects a collision.
`clear_animation` follows native semantics: removes all tracks, resets length to
1 second, and resets loop mode to `NONE`. Compressed imported clips are copied to
editable local key data before editing. Clips are bounded to 2,000 tracks;
key inspection's `key_limit` applies across all tracks, not separately per track.
Before cloning, decoded-key work is capped at 10,000 keys per track and 25,000
across each unique resource graph, including compressed or imported sources.
Compressed-track interpolation/loop-wrap metadata is preserved. Clip/library
renames remap autoplay, blend mappings, and next links in the same transaction;
graph clip references still require explicit updates.

All nine track types are supported: `value`, `position_3d`, `rotation_3d`,
`scale_3d`, `blend_shape`, `method`, `bezier`, `audio`, and `animation`.

Vectors, quaternions, colors, and other supported values use the typed Variant
encodings returned by `animation.schema`, for example:

```json
{"__type": "Vector3", "value": [0, 1, 0]}
```

Method keys use `{"method": "method_name", "args": [...]}`. Audio keys use
`{"stream": {"external_resource": "res://sound.wav"}, "start_offset": 0,
"end_offset": 0}` (or a null stream for a silent key). Native Bezier keys use
`[value, in_x, in_y, out_x, out_y]`; handle edits also have a dedicated operation. For
complex key formats, inspect a corresponding clip with `include_keys: true` and
use the published `key_formats` schemas rather than guessing its representation.

Generic dictionaries export as `{"__type":"Dictionary","value":[[key,value],...]}`
to preserve non-string keys and literal reserved field names. Array, packed
vector/color, matrix, and nested typed values are also described by the schema.
Native integer keys/values use `{"__type":"int","value":"decimal"}` so JSON
preserves integer identity and full signed 64-bit precision. PackedInt64Array
elements outside JavaScript's safe integer range must use these integer wrappers.
Truncated, unsupported, or non-finite inspection placeholders are not valid
mutation values; callers must check truncation flags before reinserting data.

Example value-track clip creation, assuming the unnamed library is absent:

```json
{
  "expected_scene_path": "res://character.tscn",
  "expected_scene_version": "<latest scene_version>",
  "node_path": "AnimationPlayer",
  "operations": [
    {"op": "add_library", "library": ""},
    {"op": "create_animation", "animation": "bob", "properties": {"length": 1, "loop_mode": 1}},
    {"op": "add_track", "animation": "bob", "type": "value", "path": "Visual:position"},
    {"op": "insert_key", "animation": "bob", "track": 0, "time": 0, "value": {"__type": "Vector3", "value": [0, 0, 0]}},
    {"op": "insert_key", "animation": "bob", "track": 0, "time": 0.5, "value": {"__type": "Vector3", "value": [0, 0.1, 0]}},
    {"op": "insert_key", "animation": "bob", "track": 0, "time": 1, "value": {"__type": "Vector3", "value": [0, 0, 0]}}
  ]
}
```

The `Visual` target and player root path must already exist. Imported animation
compression or reimport settings remain asset-pipeline concerns; this tool
authors editable local animation data, not an importer configuration.

## Runtime controls

Both runtime tools require an active MCP-owned `run_id` and a runtime node path.
They use the existing bounded asynchronous request/deduplication mechanism.
Repeating polling requests does not apply a command twice.

`animation_control.action` supports:

- Player: `player_play`, `player_stop`, `player_pause`, `player_seek`,
  `player_queue`, `player_advance`.
- StateMachine: `state_start`, `state_travel`, `state_stop`, `state_next`.
- Parameters: `set_parameter`, `set_active`.
- Requests: `oneshot_fire`, `oneshot_abort`, `oneshot_fadeout`,
  `transition_request`.

`player_play` may omit `animation` to resume the currently assigned clip.
`player_queue` requires an existing clip name. Seeking defaults to `update: true`;
`player_advance` accepts a signed delta. Read-only parameter values, playback
resources, and observer resources cannot be replaced by parameter writes.

State commands resolve the actual `AnimationNodeStateMachinePlayback` resource,
including a nested `playback_path` such as `parameters/Locomotion/playback`.
They do not require a gameplay helper script to expose `travel()`.

```json
{
  "run_id": "<owned run_id>",
  "node_path": "/root/Character/AnimationTree",
  "action": "set_parameter",
  "parameter": "parameters/Locomotion/blend_position",
  "value": 0.7
}
```

```json
{
  "run_id": "<owned run_id>",
  "node_path": "/root/Character/AnimationTree",
  "action": "oneshot_fire",
  "parameter": "parameters/AttackOneShot"
}
```

Use `runtime.pause` and `runtime.advance_frames` to observe deterministic state
changes, then `runtime.animation_get` to read parameters, playback state, and
root motion. A headless runtime can validate data and transitions; visual quality
still requires a rendered preview/capture on a suitable graphics driver.

## Verification

See the [test guide](../../../misc/godot_mcp_bridge/test/README.md). Native tests
cover transactions and resource APIs. The gated isolated animation workflow
exercises the standard MCP bridge, rebuilt editor, saved scene, and owned runtime
without touching an existing user project or editor.
