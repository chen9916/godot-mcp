# OpenCode MCP automation tools (protocol v2)

This document is the public contract for the v2 automation tools. MCP names use `godot.*`; the bridge maps them to the Godot JSON-RPC method shown beside each tool. Every input object rejects unknown fields.

## Common envelopes and lifecycle

A successful tool call has this structured content:

```json
{"ok":true,"data":{},"warnings":[]}
```

An error has `isError: true` and this structured content:

```json
{
  "ok": false,
  "error": {
    "code": "DISCONNECTED",
    "message": "The editor connection closed.",
    "operation": "runtime.get_tree",
    "session_id": "editor-uuid",
    "run_id": "run-uuid",
    "retryable": true,
    "conflict": {},
    "godot_error": null,
    "details": {}
  }
}
```

`session_id`, `run_id`, `conflict`, `godot_error`, and `details` are present only when relevant. Stable server codes are `CAPABILITY_DENIED`, `INVALID_ARGUMENT`, `CONFLICT`, `NOT_FOUND`, `INTERNAL`, `DISCONNECTED`, `TIMEOUT`, `STALE_SESSION`, and `STALE_RUN`. Discovery can additionally return `SESSION_NOT_FOUND`, `SESSION_INVALID`, `SESSION_EXPIRED`, or `SESSION_PROCESS_EXITED`. `DISCONNECTED`, `TIMEOUT`, session discovery failures, and `STALE_SESSION` are retryable unless the response says otherwise.

Every call requires a live editor session with the named capability. Protocol v2 rejects a request whose `_session_id` no longer owns the endpoint. The bridge discovers a replacement editor and uses bounded exponential backoff without restarting the MCP client. It may replay only tools marked read-safe below. A mutation whose response was lost is not silently repeated.

Runtime tools additionally require the current `run_id`. The bridge supplies a private stable `_request_id`, polls pending responses, and enforces `timeout_ms` (default `10000`, range `100..60000`). Runtime node paths must begin with `/root`; they never address editor nodes.

The following shared output records are referenced below.

### `Diagnostic`

| Field | Type | Meaning |
| --- | --- | --- |
| `sequence` | integer | Monotonic store sequence. |
| `timestamp_msec` | integer | Unix time in milliseconds. |
| `timestamp` | integer | Unix time in seconds (v1 compatibility). |
| `severity` / `type` | `error \| warning \| info` | Severity (`type` is the v1 alias). |
| `source` | `editor \| runtime` | Producing process. |
| `message` / `text` | string | Message (`text` is the v1 alias). |
| `session_id` | string | Owning editor session. |
| `run_id` | string, optional | Owning runtime. |
| `file`, `function` | string | Source location when known. |
| `line`, `column` | integer | Source coordinates when known. |
| `stack_trace` | array | Serializable stack entries when available. |
| `truncated` | boolean | Whether this record was shortened to its message, location, or stack bound. |

### `BatchResult<T>`

| Field | Type | Meaning |
| --- | --- | --- |
| `items` | array | Each entry has its correlation field (`key`, `path`, or `index`), `ok`, and either `value: T` or structured `error`; skipped entries also have `skipped: true`. |
| `count` | integer | Total item results. |
| `success_count`, `failure_count`, `skipped_count` | integer | Aggregate counts. |

### `RuntimeStatus`

| Field | Type | Meaning |
| --- | --- | --- |
| `run_id` | string | Owned run UUID, or empty before the first run. |
| `state` | `stopped \| starting \| running \| paused \| disconnected \| crashed` | Current lifecycle state. |
| `process_id` | integer | Child PID, or `0` after exit. |
| `scene_path` | string | Explicit scene path, otherwise empty. |
| `headless` | boolean | Whether headless mode was requested. |
| `runtime_connected` | boolean | Runtime debugger handshake state. |
| `started_at_msec` | integer | Run start Unix timestamp. |
| `diagnostics_sequence_start` | integer | Sequence immediately before launch. |
| `process_frame`, `physics_frame` | integer | Last observed runtime counters. |
| `diagnostics` | `Diagnostic[]` | Bounded messages created since launch. |
| `diagnostics_truncated` | boolean | Whether the embedded list exceeded 100. |
| `exit_code` | integer, conditional | Present for stopped or crashed runs. |

## Validation and filesystem tools

### `godot.script.validate` (`script.validate`)

Purpose: validate disk source or an unsaved buffer without opening a scene, changing tabs, or starting a game. Capability: `script_validation`. Read-safe: yes.

Input schema:

| Field | Type | Required | Rules/default |
| --- | --- | --- | --- |
| `script_path` | string | yes | External `res://` script path; built-in `::` paths are rejected. |
| `source` | string | no | Unsaved source; omission reads disk. |
| `expected_version` | string | no | Must equal the selected source's MD5. |
| `validate_dependencies` | boolean | no | Default `true`; dependency diagnostics are suppressed when false, while unresolved dependency metadata remains available. |

Output schema: `parse_success: boolean`, `diagnostics: Array<{severity,message,file,line,column,end_line,end_column,code?}>`, `script_path: string`, `script_version: string`, `source_origin: "disk" | "editor_buffer"`, `dependency_validation: boolean`, `unresolved_dependencies: Array<{path,message}>`, `unresolved_global_classes: Array<{name,message,file,line,column}>`, and `language: string`.

Errors: `INVALID_ARGUMENT`, `NOT_FOUND`, `CONFLICT`, `INTERNAL`, plus common session errors.

Example: `godot.script.validate({"script_path":"res://player.gd","source":"extends Node\n"})`.

Lifecycle: it uses the connected editor's registered script language only; it creates no scene, runtime, editor tab, or child validation process.

### `godot.filesystem.scan` (`filesystem.scan`)

Purpose: start an editor filesystem scan. Capability: `filesystem_sync`. Read-safe: no, because a lost response could hide an already-started scan.

Input schema: `full?: boolean` (default `false`; false uses change scanning).

Output schema: `scan_id: string`, `scanning: boolean`, `importing: boolean`, `busy: boolean`, `complete: boolean`, `progress: number`, `started_at_msec: integer`, `timestamp_msec: integer`, `full: boolean`, and `already_running: boolean`. If a scan/import is already active, its status is returned with `already_running: true`.

Errors: `INTERNAL`, plus common session errors.

Example: `godot.filesystem.scan({"full":false})`.

Lifecycle: the operation starts work and returns immediately; use `godot.filesystem.wait` with the returned `scan_id`.

### `godot.filesystem.wait` (`filesystem.wait`)

Purpose: wait without blocking the editor thread until scanning and importing are idle. Capability: `filesystem_sync`. Read-safe: yes.

Input schema: `scan_id?: string`; `timeout_ms?: integer` (`100..60000`, default `10000`).

Output schema: the scan status fields documented for `filesystem.scan` (without `full` or `already_running`), with `complete: true` on bridge return.

Errors: `NOT_FOUND` for an obsolete scan ID, `TIMEOUT`, plus common session errors.

Example: `godot.filesystem.wait({"scan_id":"scan-1-1234","timeout_ms":15000})`.

Lifecycle: the bridge repeatedly performs read-safe status requests; an editor replacement cannot be mistaken for the old scan because its scan ID will not match.

### `godot.resource.import` (`resource.import`)

Purpose: register and import new or changed images, SVGs, scripts, scenes, or custom resources without restarting the editor. Capability: `filesystem_sync`. Read-safe: no.

Input schema: exactly one of `path: string` or `paths: string[]` (`1..64` items); `atomic?: boolean` (default `false`, fail the request if any preflight or post-import load check fails). Importer side effects that completed before a post-import failure are not rolled back.

Output schema: `BatchResult<ResourceImport>` plus `atomic: boolean`, `diagnostics: Diagnostic[]`, and `filesystem` scan status. A successful `ResourceImport` has `path`, `importer`, `imported`, `import_valid`, `loadable`, `load_error` (Godot integer), `resource_type`, and `resource_uid`.

Errors: request-level `INVALID_ARGUMENT`, `CONFLICT` while the filesystem is busy, or `INTERNAL`; individual paths can return `INVALID_ARGUMENT`, `NOT_FOUND`, or `INTERNAL`; plus common session errors.

Example: `godot.resource.import({"paths":["res://icons/new.svg","res://logic/new.gd"]})`.

Lifecycle: the editor calls `update_files` before importer reimport, which fixes the newly-created SVG/no-loader cache gap. Import diagnostics are selected after a pre-operation sequence instead of clearing global logs.

### `godot.resource.status` (`resource.status`)

Purpose: explain differences among disk, editor buffer, loaded cache, importer, dependencies, and open scenes. Capability: `resource_versioning`. Read-safe: yes.

Input schema: `path: string` (required external `res://` path); `load?: boolean` (default `false`, load only when absent from cache).

Output schema:

| Field | Type |
| --- | --- |
| `path`, `resource_uid`, `disk_version`, `editor_buffer_version`, `loaded_resource_version`, `stale_state` | string |
| `disk_modified_time` | integer |
| `editor_buffer_open`, `editor_buffer_unsaved`, `loaded`, `loaded_resource_edited`, `stale`, `dependencies_ok`, `scene_reference_scan_truncated` | boolean |
| `dependencies` | `Array<{path:string,exists:boolean}>` |
| `importer_status` | `{required:boolean,name:string,imported:boolean,valid:boolean,modified_time:integer}` |
| `active_scene_references` | `Array<{scene_path:string,node_path:string}>` |

`stale_state` is `fresh`, `editor_buffer_conflict`, `loaded_resource_unsaved`, `loaded_resource_stale`, `import_stale`, or `dependency_missing`.

Errors: `INVALID_ARGUMENT`, `NOT_FOUND`, plus common session errors.

Example: `godot.resource.status({"path":"res://player.gd","load":true})`.

Lifecycle: versions and references are editor-session state. Re-query after reconnect; never carry cached object identities into a replacement session.

## Runtime control tools

### `godot.runtime.start` (`runtime.start`)

Purpose: start one run owned by this editor session. Capability: `runtime_control`. Read-safe: no.

Input schema: `mode?: "main" | "current" | "scene"` (defaults to `scene` when `scene_path` is supplied, otherwise `main`); `scene_path?: string` (required for `scene`); `headless?: boolean` (default `false`); `allow_unsaved?: boolean` (default `false`, permits Godot's normal autosave-before-run behavior).

Output schema: `RuntimeStatus`.

Errors: `INVALID_ARGUMENT`, `NOT_FOUND`, `CONFLICT` for unsaved work, another active run, or a non-single run-instance configuration, `INTERNAL`, plus common session errors.

Example: `godot.runtime.start({"mode":"scene","scene_path":"res://tests/input_test.tscn","headless":true})`.

Lifecycle: only this call creates a `run_id` and runtime debugger endpoint. It refuses to stop or replace an unrelated editor run. `state` can initially be `starting`; poll `runtime.status` until `runtime_connected` is true.

### `godot.runtime.stop` (`runtime.stop`)

Purpose: stop exactly one owned run. Capability: `runtime_control`. Read-safe: no.

Input schema: `run_id: string` and `confirm: true`, both required.

Output schema: `RuntimeStatus` with `state: "stopped"`.

Errors: `INVALID_ARGUMENT`, `STALE_RUN`, plus common session errors.

Example: `godot.runtime.stop({"run_id":"<run-uuid>","confirm":true})`.

Lifecycle: a mismatched ID never affects the active or unrelated run. A runtime whose debugger disconnected remains owned and stoppable while its process is alive.

### `godot.runtime.status` (`runtime.status`)

Purpose: inspect run state, process/debugger identity, frame counters, exit code, and launch-scoped diagnostics. Capability: `runtime_control`. Read-safe: yes.

Input schema: `run_id?: string`; omission reports the current or last owned run.

Output schema: `RuntimeStatus`.

Errors: `STALE_RUN` when a supplied ID is not current, plus common session errors.

Example: `godot.runtime.status({"run_id":"<run-uuid>"})`.

Lifecycle: unexpected process exit becomes `crashed`; debugger loss while the process survives becomes `disconnected` rather than silently discarding ownership.

### `godot.runtime.pause` / `godot.runtime.resume` (`runtime.pause` / `runtime.resume`)

Purpose: suspend or resume the owned runtime through the Godot debugger. Capability: `runtime_control`. Read-safe: no.

Input schema for each: `run_id: string` (required).

Output schema: `RuntimeStatus`, normally with `state: "paused"` or `"running"` respectively.

Errors: `INVALID_ARGUMENT`, `STALE_RUN`, `CONFLICT` for an invalid state, `DISCONNECTED`, plus common session errors.

Examples: `godot.runtime.pause({"run_id":"<run-uuid>"})`; `godot.runtime.resume({"run_id":"<run-uuid>"})`.

Lifecycle: these calls address only the debugger session that handshook the same run UUID.

### `godot.runtime.advance_frames` (`runtime.advance_frames`)

Purpose: wait for exact bounded process or physics frame progress; paused runs are stepped through the debugger. Capability: `runtime_control`. Read-safe via request-ID deduplication.

Input schema: `run_id: string`, `frames: integer` (`1..600`), `frame_type?: "physics" | "process"` (default `physics`), and `timeout_ms?: integer` (`100..60000`, default `10000`).

Output schema: `frame_type`, `frame_number`, `process_frame`, `physics_frame`, `pending: false`, `request_sent: false`, `request_id`, and `run_id`.

Errors: `INVALID_ARGUMENT`, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT` when the runtime request queue is full, plus common session errors.

Example: `godot.runtime.advance_frames({"run_id":"<run-uuid>","frames":4,"frame_type":"physics"})`.

Lifecycle: the bridge keeps one request ID across polls and reconnects, so frames are requested once rather than once per poll.

## Runtime interaction tools

### `godot.runtime.get_tree` (`runtime.get_tree`)

Purpose: obtain a bounded game-process tree snapshot. Capability: `runtime_inspection`. Read-safe via request-ID deduplication.

Input schema: `run_id: string`; `root_path?: string` (default `/root`); `max_depth?: integer` (`0..12`, default `6`); `max_nodes?: integer` (`1..2000`, default `500`); `timeout_ms?: integer` (`100..60000`).

Output schema: `tree: RuntimeNode`, `node_count: integer`, `truncated: boolean`, `process_frame`, `physics_frame`, and common completed runtime request fields. `RuntimeNode` has `name`, `class`, `path`, `process_mode`, `child_count`, and recursive `children`.

Errors: `NOT_FOUND`, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.get_tree({"run_id":"<run-uuid>","root_path":"/root/Main","max_depth":3,"max_nodes":200})`.

Lifecycle: only `/root` paths inside the matching game process resolve; editor scene nodes and stale run objects are rejected.

### `godot.runtime.get_property` (`runtime.get_property`)

Purpose: read one serializable property from a runtime node. Capability: `runtime_inspection`. Read-safe via request-ID deduplication.

Input schema: required `run_id`, `node_path`, and `property_name` strings; optional `timeout_ms` (`100..60000`).

Output schema: `node_path`, `property_name`, `value`, `type`, and common completed runtime request fields. Node/resource object values become bounded references; unsupported objects, callables, signals, excessive depth, or excessive container items are rejected.

Errors: `NOT_FOUND`, `INVALID_ARGUMENT` for a non-serializable value, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.get_property({"run_id":"<run-uuid>","node_path":"/root/Main/Player","property_name":"health"})`.

Lifecycle: property reads never fall back to the editor tree or an older runtime.

### `godot.runtime.set_property` (`runtime.set_property`)

Purpose: set one runtime node property and return its resulting serializable value. Capability: `runtime_inspection`. Read-safe only through request-ID deduplication.

Input schema: required `run_id`, `node_path`, `property_name`, and arbitrary serializable `value`; optional `timeout_ms` (`100..60000`).

Output schema: `node_path`, `property_name`, `value`, and common completed runtime request fields.

Errors: `NOT_FOUND`, `INVALID_ARGUMENT`, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.set_property({"run_id":"<run-uuid>","node_path":"/root/Main/Player","property_name":"health","value":50})`.

Lifecycle: the mutation is deduplicated by the bridge-generated request ID; it is never redirected to an editor object.

### `godot.runtime.call` (`runtime.call`)

Purpose: call a runtime node method with serializable values. Capability: `runtime_inspection`. Read-safe only through request-ID deduplication.

Input schema: required `run_id`, `node_path`, and `method`; `arguments?: any[]` (default empty, at most 16); `confirm_destructive?: boolean` (must be true for `free` or `queue_free`); `timeout_ms?: integer` (`100..60000`).

Output schema: `node_path`, `method`, `result`, and common completed runtime request fields.

Errors: `NOT_FOUND`, `INVALID_ARGUMENT` (including confirmation, call signature, or non-serializable result), `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.call({"run_id":"<run-uuid>","node_path":"/root/Main/Player","method":"take_damage","arguments":[5]})`.

Lifecycle: method lookup and invocation occur only in the matching game process. Destructive calls require explicit per-call confirmation.

### `godot.runtime.input_action` (`runtime.input_action`)

Purpose: press, release, or hold an InputMap action. Capability: `runtime_inspection`. Read-safe only through request-ID deduplication.

Input schema: required `run_id` and `action`; `pressed?: boolean` (default `true`); `strength?: number` (`0..1`, default `1`); `hold_frames?: integer` (`0..600`, default `0`); `timeout_ms?: integer` (`100..60000`).

Output schema: immediate operations return `action`, `pressed`, `strength`, and `physics_frame`; a hold returns `action`, `pressed: false`, and `held_until_physics_frame`; both include common completed runtime request fields.

Errors: `NOT_FOUND`, `INVALID_ARGUMENT`, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.input_action({"run_id":"<run-uuid>","action":"jump","hold_frames":2})`.

Lifecycle: holds release automatically at the target physics frame, including when a paused run is explicitly stepped. Pending actions are released when the runtime endpoint shuts down.

### `godot.runtime.input_event` (`runtime.input_event`)

Purpose: inject bounded mouse motion or mouse-button input. Capability: `runtime_inspection`. Read-safe only through request-ID deduplication.

Input schema: `run_id: string`; `type: "mouse_motion" | "mouse_button"`; `position?: {x:number,y:number}`; `relative?: {x:number,y:number}`; `button_index?: integer` (`1..9`, required by `mouse_button`); `pressed?: boolean` (default `true`); `double_click?: boolean` (default `false`); `timeout_ms?: integer` (`100..60000`).

Output schema: `type`, `process_frame`, and common completed runtime request fields.

Errors: `INVALID_ARGUMENT`, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.input_event({"run_id":"<run-uuid>","type":"mouse_motion","position":{"x":320,"y":180},"relative":{"x":4,"y":0}})`.

Lifecycle: events enter the matching runtime's `Input` singleton; they cannot target the editor viewport.

### `godot.runtime.capture_viewport` (`runtime.capture_viewport`)

Purpose: capture a bounded runtime viewport PNG for visual validation. Capability: `viewport_capture`. Read-safe via request-ID deduplication.

Input schema: `run_id: string`; `viewport?: string` (default `/root`, must resolve to a runtime `Viewport`); `crop?: {x:integer,y:integer,width:integer>=1,height:integer>=1}`; `scale?: number` (`0.05..4`, default `1`); `frame_wait?: integer` (`0..600`, default `0`); `timeout_ms?: integer` (`100..60000`).

Output schema: `path: string`, `width: integer`, `height: integer`, `capture_timestamp_msec: integer`, `frame_number: integer`, `viewport: string`, and common completed runtime request fields. Each output dimension is capped at 4096 pixels; the result is a path rather than an unbounded encoded payload.

Errors: `NOT_FOUND`, `INVALID_ARGUMENT`, `INTERNAL` for capture/save failure, `STALE_RUN`, `DISCONNECTED`, `TIMEOUT`, `CONFLICT`, plus common session errors.

Example: `godot.runtime.capture_viewport({"run_id":"<run-uuid>","viewport":"/root","crop":{"x":0,"y":0,"width":640,"height":360},"scale":0.5,"frame_wait":1})`.

Lifecycle: the PNG is written to the runtime host's temporary directory. Consumers should copy it if long-term retention is required.

## Diagnostics tool

### `godot.diagnostics.get` (`diagnostics.get`)

Purpose: query bounded editor- or run-scoped messages without destructive global clearing. Capability: `diagnostics_v2`. Read-safe: yes.

Input schema: `after_sequence?: integer >= 0` (default `0`); `limit?: integer` (`1..1000`, default `100`); `session_id?: string`; `run_id?: string`; `source?: "editor" | "runtime"`; `severities?: Array<"error" | "warning" | "info">`.

Output schema: `messages: Diagnostic[]`, `count: integer`, `truncated: boolean`, and `latest_sequence: integer`.

Errors: `INVALID_ARGUMENT`, plus common session errors.

Example: `godot.diagnostics.get({"run_id":"<run-uuid>","after_sequence":120,"severities":["error","warning"]})`.

Lifecycle: the store is bounded to 1000 entries and belongs to one editor session. Runtime messages carry their run ID, so messages from two runs cannot be confused. Save `latest_sequence` and use it as the next `after_sequence`; clearing is unnecessary.

## Updated existing tools

### `godot.session.info`

Input schema: empty object. Output is `{bridge, editor}`. `bridge` contains `state`, optional `session_id`, `pid`, `port`, `protocol_version`, `project_path`, `capabilities`, `reconnect_attempt`, and optional `last_error`. `editor` is the editor's normal success envelope containing `session_id`, `state`, `port`, `capabilities`, `transport`, `protocol`, and `protocol_version`; it is `null` while disconnected. The tool itself returns success with a structured warning when no editor is currently reachable, allowing clients to observe `disconnected`, `reconnecting`, or `failed` state.

Example: `godot.session.info({})`.

### `godot.resource.reload`

Input schema: optional `path` (defaults to active scene), `expected_scene_path`, and `expected_version` (disk MD5). Scenes refuse reload when the open scene is unsaved. Scripts/resources refuse reload when an open buffer version differs from disk and return `CONFLICT` with UID, disk, buffer, and loaded versions. Non-scene output contains `path`, `before`/`after` resource status records, `script_specific_reload`, `reload_verified`, `existing_scene_instances_updated`, `existing_scene_instance_count`, `runtime_reload_requested`, and `stale_editor_buffer`. No unsaved editor source is discarded automatically.

Example: `godot.resource.reload({"path":"res://player.gd","expected_version":"<md5>"})`.

### Partial reads

`godot.project.get_setting`, `godot.node.get_properties`, `godot.node.get_properties_batch`, and `godot.resource.get` accept `atomic?: boolean` (default `false`). Non-atomic reads return successful values plus per-key/property errors using `BatchResult`; compatibility dictionaries such as `settings` or `properties` remain populated with successes. `atomic: true` retains fail-fast behavior. Node batch results can have `status: "partial"` and an inner property `BatchResult` while still retaining the readable values.

Example: `godot.project.get_setting({"keys":["display/window/size/viewport_width","missing/key"],"atomic":false})`.
