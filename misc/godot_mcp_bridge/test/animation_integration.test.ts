import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { CallToolResultSchema } from "@modelcontextprotocol/sdk/types.js";

import { wait_for_runtime_operation } from "../src/operation_waiter.js";
import { SessionLifecycle } from "../src/session_lifecycle.js";

const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));
const delay = (milliseconds: number) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const scenePath = "res://main.tscn";
const runtimeRoot = "/root/AnimationFixture";
const animationTools = [
	"godot.animation.schema", "godot.animation.list", "godot.animation.get", "godot.animation.apply_patch",
	"godot.animation.graph_get", "godot.animation.graph_apply_patch", "godot.animation.graph_validate",
	"godot.runtime.animation_get", "godot.runtime.animation_control",
];
type ObjectData = Record<string, any>;
type Operation = Record<string, unknown>;
const variant = (type: string, value: unknown) => ({ __type: type, value });

const controllerScript = `extends AnimationTree

@export var controller_tag: String = "default-controller"

func controller_marker() -> String:
	return controller_tag
`;

// These snapshots are a test-owned oracle, not assumptions about native result
// layouts. In particular, packed resources and runtime effects are inspected
// independently of the animation tools being tested.
const fixtureScript = `@tool
extends Node3D

func _plain(value):
	if value is Vector2:
		return [value.x, value.y]
	if value is Vector3:
		return [value.x, value.y, value.z]
	if value is Quaternion:
		return [value.x, value.y, value.z, value.w]
	if value is NodePath or value is StringName:
		return str(value)
	if value is Array or value is PackedStringArray:
		var items = []
		for item in value:
			items.append(_plain(item))
		return items
	if value is Dictionary:
		var items = {}
		for key in value:
			items[str(key)] = _plain(value[key])
		return items
	return value

func _graph(node):
	if node == null:
		return null
	var result = {"type": node.get_class()}
	if node is AnimationNodeAnimation:
		result.animation = str(node.animation)
	elif node is AnimationNodeBlendTree or node is AnimationNodeStateMachine:
		result.nodes = {}
		var names = node.get_node_list()
		names.sort()
		for child_name in names:
			result.nodes[str(child_name)] = {"position": _plain(node.get_node_position(child_name)), "graph": _graph(node.get_node(child_name))}
		if node is AnimationNodeBlendTree:
			# Serialized target iteration follows a native HashMap and may reorder
			# on rename. Connection tuples, unlike transition/track order, are unordered.
			var connections = _plain(node.get("node_connections"))
			var triples = []
			for index in range(0, connections.size(), 3):
				triples.append(connections.slice(index, index + 3))
			triples.sort_custom(func(left, right): return JSON.stringify(left) < JSON.stringify(right))
			result.connections = []
			for triple in triples:
				result.connections.append_array(triple)
		else:
			result.transitions = []
			for index in node.get_transition_count():
				var transition = node.get_transition(index)
				result.transitions.append({"from": str(node.get_transition_from(index)), "to": str(node.get_transition_to(index)), "condition": str(transition.advance_condition), "advance_mode": transition.advance_mode, "switch_mode": transition.switch_mode, "xfade_time": transition.xfade_time, "reset": transition.reset})
	elif node is AnimationNodeBlendSpace1D or node is AnimationNodeBlendSpace2D:
		result.points = []
		for index in node.get_blend_point_count():
			result.points.append({"position": _plain(node.get_blend_point_position(index)), "graph": _graph(node.get_blend_point_node(index))})
		if node is AnimationNodeBlendSpace2D:
			result.auto_triangles = node.auto_triangles
			result.triangles = []
			for index in node.get_triangle_count():
				result.triangles.append([node.get_triangle_point(index, 0), node.get_triangle_point(index, 1), node.get_triangle_point(index, 2)])
	if node is AnimationNodeOneShot:
		result.filter_enabled = node.filter_enabled
		result.target_filtered = node.is_path_filtered(NodePath("Target"))
		result.fadein_time = node.fadein_time
		result.fadeout_time = node.fadeout_time
	return result

func animation_fixture_snapshot():
	var player = get_node("AnimationPlayer")
	var libraries = {}
	for library_name in player.get_animation_library_list():
		var library = player.get_animation_library(library_name)
		var clips = {}
		for clip_name in library.get_animation_list():
			var clip = library.get_animation(clip_name)
			var tracks = []
			for track in clip.get_track_count():
				var keys = []
				for key in clip.track_get_key_count(track):
					var value = clip.track_get_key_value(track, key)
					keys.append({"time": clip.track_get_key_time(track, key), "type": type_string(typeof(value)), "value": _plain(value), "transition": clip.track_get_key_transition(track, key)})
				tracks.append({"type": clip.track_get_type(track), "path": str(clip.track_get_path(track)), "enabled": clip.track_is_enabled(track), "keys": keys})
			clips[str(clip_name)] = {"length": clip.length, "loop_mode": clip.loop_mode, "tracks": tracks}
		libraries[str(library_name)] = clips
	var tree = get_node("AnimationTree")
	var parameters = {}
	for property in tree.get_property_list():
		if str(property.name).begins_with("parameters/") and (property.usage & PROPERTY_USAGE_STORAGE) != 0 and property.type != TYPE_OBJECT:
			parameters[str(property.name)] = _plain(tree.get(property.name))
	return {"libraries": libraries, "graph": _graph(tree.tree_root), "parameters": parameters, "controller_script": tree.get_script().resource_path, "controller_tag": tree.get("controller_tag")}

func runtime_fixture_snapshot(inspect_player_position = false):
	var player = get_node("AnimationPlayer")
	var tree = get_node("AnimationTree")
	var playback = tree.get("parameters/Controller/playback")
	var player_position = 0.0
	# is_valid() is native-only in this engine build. The caller requests position
	# only after play/seek/advance or pause, never after a resetting stop.
	if inspect_player_position:
		player_position = player.current_animation_position
	var state = {}
	if playback is AnimationNodeStateMachinePlayback:
		state = {"current": str(playback.get_current_node()), "playing": playback.is_playing(), "travel": _plain(playback.get_travel_path()), "position": playback.get_current_play_position()}
	return {"animation": str(player.current_animation), "assigned_animation": str(player.assigned_animation), "position": player_position, "playing": player.is_playing(), "queue": _plain(player.get_queue()), "target_position": _plain(get_node("Target").position), "active": tree.active, "state": state, "blend2d": _plain(tree.get("parameters/Controller/Direction/blend_position")), "oneshot_active": tree.get("parameters/OneShot/active"), "root_motion": _plain(tree.get_root_motion_position()), "switch_state": tree.get("parameters/Switch/current_state"), "switch_index": tree.get("parameters/Switch/current_index"), "physics_processing": tree.is_physics_processing_internal(), "callback_mode": tree.callback_mode_process, "user_data": OS.get_user_data_dir()}

func runtime_freed_reference():
	var object = Node.new()
	var result = {"freed_node": object}
	object.free()
	return result
`;

const pluginScript = `@tool
extends EditorPlugin

var probe_ready := false
var control_dir := OS.get_environment("GODOT_MCP_ANIMATION_CONTROL")

func _enter_tree():
	_setup.call_deferred()

func _write(name, value):
	var file = FileAccess.open(control_dir.path_join(name), FileAccess.WRITE)
	file.store_string(JSON.stringify(value))
	file.close()

func _setup():
	await get_tree().create_timer(1.0).timeout
	EditorInterface.get_editor_settings().set_setting("network/godot_mcp/enabled", true)
	EditorInterface.open_scene_from_path("res://main.tscn")
	await get_tree().create_timer(0.5).timeout
	probe_ready = true
	_write("ready.json", {"ready": true, "pid": OS.get_process_id(), "project": ProjectSettings.globalize_path("res://"), "user_data": OS.get_user_data_dir()})

func _scene_menu_action(action):
	for menu in get_tree().root.find_children("Scene", "PopupMenu", true, false):
		for index in menu.item_count:
			if menu.get_item_text(index).to_lower() == action:
				menu.emit_signal("about_to_popup")
				menu.emit_signal("id_pressed", menu.get_item_id(index))
				return true
	return false

func _process(_delta):
	if not probe_ready:
		return
	var command_path = control_dir.path_join("command.json")
	if not FileAccess.file_exists(command_path):
		return
	var command = JSON.parse_string(FileAccess.get_file_as_string(command_path))
	DirAccess.remove_absolute(command_path)
	if command.action == "quit":
		get_tree().quit()
		return
	var edited = EditorInterface.get_edited_scene_root()
	if command.action == "undo" or command.action == "redo":
		if not _scene_menu_action(command.action):
			_write("snapshot.json", {"id": command.id, "menu_error": "Scene undo/redo command not found"})
			return
	var packed = PackedScene.new()
	var error = packed.pack(edited)
	var instance = packed.instantiate()
	var snapshot = {"id": command.id, "pack_error": error, "packed_count": packed.get_state().get_node_count(), "live": edited.animation_fixture_snapshot(), "packed": instance.animation_fixture_snapshot()}
	instance.free()
	_write("snapshot.json", snapshot)
`;

const sceneSource = `[gd_scene load_steps=7 format=3]

[ext_resource type="Script" path="res://fixture.gd" id="1_script"]
[ext_resource type="Script" path="res://tree_controller.gd" id="2_controller"]

[sub_resource type="Animation" id="Animation_reset"]
resource_name = "RESET"
length = 0.001
tracks/0/type = "position_3d"
tracks/0/path = NodePath("Target")
tracks/0/keys = PackedFloat32Array(0, 1, 0, 0, 0)

[sub_resource type="AnimationLibrary" id="Library_initial"]
_data = {&"RESET": SubResource("Animation_reset")}

[sub_resource type="AnimationNodeBlendTree" id="Graph_initial"]

[sub_resource type="AnimationNodeAnimation" id="Graph_missing"]
animation = &"motion/missing_clip"

[node name="AnimationFixture" type="Node3D"]
script = ExtResource("1_script")

[node name="Target" type="Node3D" parent="."]

[node name="AnimationPlayer" type="AnimationPlayer" parent="."]
callback_mode_process = 2
libraries = {&"": SubResource("Library_initial")}

[node name="AnimationTree" type="AnimationTree" parent="."]
script = ExtResource("2_controller")
controller_tag = "scene-controller"
active = false
callback_mode_process = 0
root_motion_track = NodePath("Target")
tree_root = SubResource("Graph_initial")
anim_player = NodePath("../AnimationPlayer")

[node name="MissingClipTree" type="AnimationTree" parent="."]
active = false
tree_root = SubResource("Graph_missing")
anim_player = NodePath("../AnimationPlayer")

[node name="UnboundTree" type="AnimationTree" parent="."]
active = false
tree_root = SubResource("Graph_missing")
anim_player = NodePath("../MissingPlayer")
`;

function data(result: unknown): ObjectData {
	assert.ok(result && typeof result === "object" && !Array.isArray(result), JSON.stringify(result));
	const envelope = result as ObjectData;
	assert.equal(envelope.ok, true, JSON.stringify(envelope));
	assert.ok(envelope.data && typeof envelope.data === "object" && !Array.isArray(envelope.data), JSON.stringify(envelope));
	return envelope.data;
}

function assertFinite(value: unknown): void {
	if (typeof value === "number") assert.ok(Number.isFinite(value), "Snapshots must not contain non-finite numbers");
	else if (Array.isArray(value)) value.forEach(assertFinite);
	else if (value && typeof value === "object") Object.values(value).forEach(assertFinite);
}

// The diagnostic collection's exact output key is intentionally not fixed by
// this test. Require a diagnostic, not just an echoed node/animation path.
function diagnosticText(value: ObjectData): string {
	const diagnostics: string[] = [];
	const visit = (item: unknown): void => {
		if (Array.isArray(item)) { item.forEach(visit); return; }
		if (!item || typeof item !== "object") return;
		for (const [key, child] of Object.entries(item)) {
			if (/diagnostic|issue|error|warning|problem|message/i.test(key)) diagnostics.push(JSON.stringify(child));
			else visit(child);
		}
	};
	visit(value);
	return diagnostics.join("\n");
}

function hasStringValue(value: unknown, expected: string): boolean {
	if (typeof value === "string") return value === expected;
	if (Array.isArray(value)) return value.some((item) => hasStringValue(item, expected));
	if (value && typeof value === "object") return Object.values(value).some((item) => hasStringValue(item, expected));
	return false;
}

test("isolated animation editor and standard stdio runtime workflow", {
	skip: process.env.GODOT_MCP_ANIMATION_EDITOR ? false : "Set GODOT_MCP_ANIMATION_EDITOR to an absolute rebuilt editor binary path.",
	timeout: 420_000,
}, async (t) => {
	const suppliedEditor = process.env.GODOT_MCP_ANIMATION_EDITOR!;
	assert.ok(path.isAbsolute(suppliedEditor), "GODOT_MCP_ANIMATION_EDITOR must be an absolute binary path.");
	assert.ok((await fs.stat(suppliedEditor)).isFile(), "The supplied editor must be a file.");
	const temporaryRoot = process.platform === "win32" ? "C:\\Users\\l3312\\AppData\\Local\\Temp\\opencode" : os.tmpdir();
	await fs.mkdir(temporaryRoot, { recursive: true });
	const directory = await fs.mkdtemp(path.join(temporaryRoot, "godot-mcp-animation-"));
	const bundle = path.join(directory, "Self Contained Editor");
	const project = path.join(directory, "Animation Project");
	const control = path.join(directory, "control");
	// _sc_ isolates editor settings, but user:// and the platform's temporary
	// directories must also be redirected for BOTH the editor and its runtime.
	const isolatedEnvironment = Object.fromEntries(Object.entries(process.env).filter((entry): entry is [string, string] => entry[1] !== undefined));
	Object.assign(isolatedEnvironment, {
		APPDATA: path.join(directory, "profile", "AppData", "Roaming"),
		LOCALAPPDATA: path.join(directory, "profile", "AppData", "Local"),
		USERPROFILE: path.join(directory, "profile"), HOME: path.join(directory, "profile"),
		TEMP: path.join(directory, "temp"), TMP: path.join(directory, "temp"), TMPDIR: path.join(directory, "temp"),
		XDG_CONFIG_HOME: path.join(directory, "profile", "config"),
		XDG_DATA_HOME: path.join(directory, "profile", "data"),
		XDG_CACHE_HOME: path.join(directory, "profile", "cache"),
		GODOT_MCP_ANIMATION_CONTROL: control,
	});
	const lifecycle = new SessionLifecycle({ project_root: project, timeout_ms: 20_000 });
	let child: ChildProcess | undefined;
	let finished: Promise<void> | undefined;
	let editorExited = false;
	let client: Client | undefined;
	let transport: StdioClientTransport | undefined;
	let activeRun: string | undefined;
	let inspectPlayerPosition = false;
	let ownedRuntimePid: number | undefined;
	let editorLog = "";
	let bridgeLog = "";
	let phase = "setup";
	const transcript: string[] = [];
	const successfulRuntimeActions = new Set<string>();
	const assertOwnedPath = (file: string) => {
		const relative = path.relative(directory, path.resolve(file));
		assert.ok(relative && relative !== ".." && !relative.startsWith(".." + path.sep) && !path.isAbsolute(relative), `Expected an owned path, got ${file}`);
	};
	t.after(async () => {
		try {
			if (activeRun) {
				try {
					await lifecycle.call("runtime.stop", { run_id: activeRun, confirm: true });
					ownedRuntimePid = undefined;
				} catch {
					// Only the process ID returned for our own run is eligible for
					// fallback cleanup. Never search for/kill other Godot processes.
					if (ownedRuntimePid && ownedRuntimePid !== process.pid && ownedRuntimePid !== child?.pid) {
						try { process.kill(ownedRuntimePid); } catch { /* Already exited. */ }
					}
				}
			}
			await client?.close().catch(() => {});
			await transport?.close().catch(() => {});
		} finally {
			await lifecycle.close();
			if (child && !editorExited) {
				await fs.writeFile(path.join(control, "command.json"), JSON.stringify({ action: "quit" })).catch(() => {});
				await Promise.race([finished?.catch(() => {}), delay(5_000)]);
				if (!editorExited) child.kill();
			}
			await finished?.catch(() => {});
			await fs.rm(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
		}
	});

	const waitFile = async (file: string): Promise<ObjectData> => {
		const deadline = Date.now() + 60_000;
		while (Date.now() < deadline && !t.signal.aborted) {
			try { return JSON.parse(await fs.readFile(file, "utf8")); }
			catch (error: any) { if (error.code !== "ENOENT" && !(error instanceof SyntaxError)) throw error; }
			if (editorExited) throw new Error("Owned editor exited before reporting " + file);
			await delay(100);
		}
		throw new Error("Timed out waiting for " + file);
	};
	let commandId = 0;
	const snapshot = async (action = "snapshot"): Promise<ObjectData> => {
		const id = ++commandId;
		await fs.rm(path.join(control, "snapshot.json"), { force: true });
		const command = path.join(control, "command.json");
		await fs.writeFile(command + ".tmp", JSON.stringify({ action, id }));
		await fs.rename(command + ".tmp", command);
		const report = await waitFile(path.join(control, "snapshot.json"));
		assert.equal(report.id, id);
		assert.equal(report.pack_error, 0);
		assert.deepEqual(report.packed, report.live, "Packing must preserve animation resources, keys, and graph settings.");
		return report.live;
	};
	const request = async (name: string, args: Record<string, unknown> = {}) => {
		assert.ok(client);
		transcript.push(`${name} ${JSON.stringify(args)}`);
		if (transcript.length > 40) transcript.shift();
		const result = CallToolResultSchema.parse(await client.callTool({ name, arguments: args }, CallToolResultSchema, { timeout: 65_000 }));
		// Validate the standard SDK's structured/text representation as well as
		// testing the native endpoint behind it.
		assert.ok(result.structuredContent, JSON.stringify(result));
		const text = result.content.find((item) => item.type === "text");
		assert.ok(text && text.type === "text");
		if (result.isError) {
			const error = (result.structuredContent as ObjectData).error;
			assert.equal(text.text, `${error.code}: ${error.message}`);
		} else {
			assert.deepEqual(JSON.parse(text.text), result.structuredContent);
		}
		return result;
	};
	const call = async (name: string, args: Record<string, unknown> = {}): Promise<ObjectData> => {
		const result = await request(name, args);
		assert.equal(result.isError, false, `${name}: ${JSON.stringify(result.structuredContent)}`);
		return data(result.structuredContent);
	};
	const rejected = async (name: string, args: Record<string, unknown>, reason?: RegExp) => {
		const result = await request(name, args);
		assert.equal(result.isError, true, `${name} unexpectedly accepted ${JSON.stringify(args)}`);
		const envelope = result.structuredContent as ObjectData;
		assert.equal(envelope.ok, false, JSON.stringify(envelope));
		assert.ok(envelope.error && typeof envelope.error.code === "string", JSON.stringify(envelope));
		assert.ok(["INVALID_ARGUMENT", "NOT_FOUND", "CONFLICT", "STALE_RUN", "NON_SERIALIZABLE"].includes(envelope.error.code), `A transport, internal, or capability failure is not input rejection: ${JSON.stringify(envelope.error)}`);
		if (reason) assert.match(JSON.stringify(envelope.error), reason);
	};
	const inspect = async (name: string, node_path: string, extra: Record<string, unknown> = {}): Promise<ObjectData> => {
		const result = await call(name, { expected_scene_path: scenePath, node_path, ...extra });
		assert.equal(result.scene_path, scenePath);
		assert.ok(typeof result.scene_version === "string" && result.scene_version.length > 0);
		return result;
	};
	const guards = async (node_path: string) => {
		const current = await inspect("godot.animation.list", "AnimationPlayer");
		return { expected_scene_path: scenePath, expected_scene_version: current.scene_version, node_path };
	};
	const patch = async (name: string, node_path: string, operations: Operation[], dry_run = false) => {
		const result = await call(name, { ...await guards(node_path), operations, dry_run });
		assert.equal(result.scene_path, scenePath);
		assert.ok(typeof result.scene_version === "string" && result.scene_version.length > 0);
		return result;
	};
	const runtimeSnapshot = async (): Promise<ObjectData> => {
		assert.ok(activeRun);
		const result = data(await wait_for_runtime_operation(lifecycle, "runtime.call", {
			run_id: activeRun, node_path: runtimeRoot, method: "runtime_fixture_snapshot", arguments: [inspectPlayerPosition], timeout_ms: 20_000,
		})).result;
		assertOwnedPath(result.user_data);
		return result;
	};
	const startRuntime = async () => {
		const start = await call("godot.runtime.start", { mode: "scene", scene_path: scenePath, headless: true });
		assert.ok(typeof start.run_id === "string" && start.run_id.length > 0);
		activeRun = start.run_id;
		if (typeof start.process_id === "number" && start.process_id > 0) ownedRuntimePid = start.process_id;
		const deadline = Date.now() + 30_000;
		while (Date.now() < deadline && !t.signal.aborted) {
			const status = data(await lifecycle.call("runtime.status", { run_id: activeRun }, { retry_safe: true }));
			if (status.runtime_connected === true && (status.state === "running" || status.state === "paused")) {
				await call("godot.runtime.pause", { run_id: activeRun });
				return;
			}
			assert.ok(status.state !== "crashed" && status.state !== "stopped", JSON.stringify(status));
			await delay(100);
		}
		throw new Error("Owned runtime debugger did not connect within 30 seconds.");
	};
	const runtimeControl = async (node: string, action: string, extra: Record<string, unknown> = {}): Promise<ObjectData> => {
		const result = await call("godot.runtime.animation_control", { run_id: activeRun, node_path: `${runtimeRoot}/${node}`, action, timeout_ms: 20_000, ...extra });
		successfulRuntimeActions.add(action);
		if (node === "AnimationPlayer" && action === "player_play") inspectPlayerPosition = true;
		if (node === "AnimationPlayer" && action === "player_stop") inspectPlayerPosition = false;
		return result;
	};
	const step = async (frames = 2) => {
		assert.ok(activeRun);
		data(await wait_for_runtime_operation(lifecycle, "runtime.advance_frames", { run_id: activeRun, frames, frame_type: "physics", timeout_ms: 20_000 }));
	};

	try {
		await fs.mkdir(bundle);
		await fs.mkdir(control);
		for (const key of ["APPDATA", "LOCALAPPDATA", "USERPROFILE", "TEMP", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"]) {
			await fs.mkdir(isolatedEnvironment[key], { recursive: true });
		}
		await fs.mkdir(path.join(project, "addons", "animation_probe"), { recursive: true });
		const executable = path.join(bundle, process.platform === "win32" ? "Godot.exe" : path.basename(suppliedEditor));
		await fs.copyFile(suppliedEditor, executable);
		if (process.platform !== "win32") await fs.chmod(executable, 0o700);
		await fs.writeFile(path.join(bundle, "_sc_"), "");
		await fs.writeFile(path.join(project, "project.godot"), '[application]\nconfig/name="MCP Isolated Animation Regression"\nrun/main_scene="res://main.tscn"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/animation_probe/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n');
		await fs.appendFile(path.join(project, "project.godot"), `\n[debug]\nfile_logging/log_path=${JSON.stringify(path.join(control, "runtime.log").replaceAll("\\", "/"))}\nfile_logging/max_log_files=2\n`);
		await fs.writeFile(path.join(project, "main.tscn"), sceneSource);
		await fs.writeFile(path.join(project, "fixture.gd"), fixtureScript);
		await fs.writeFile(path.join(project, "tree_controller.gd"), controllerScript);
		await fs.writeFile(path.join(project, "addons", "animation_probe", "plugin.cfg"), '[plugin]\nname="Animation Integration Probe"\ndescription="Owned disposable animation test"\nauthor="Godot tests"\nversion="1"\nscript="plugin.gd"\n');
		await fs.writeFile(path.join(project, "addons", "animation_probe", "plugin.gd"), pluginScript);

		phase = "offline SDK discovery/schema";
		transport = new StdioClientTransport({ command: process.execPath, args: [bridge, "--project-root", project], cwd: project, env: isolatedEnvironment, stderr: "pipe" });
		transport.stderr?.on("data", (chunk) => { bridgeLog += chunk; });
		client = new Client({ name: "isolated-animation-integration", version: "1" }, { capabilities: {} });
		await client.connect(transport);
		const { tools } = await client.listTools();
		for (const name of animationTools) assert.ok(tools.some((tool) => tool.name === name), name);
		const schema = await call("godot.animation.schema");
		assert.equal(schema.schema_version, 1);
		assert.equal(schema.operation_limit, 512);
		assert.equal(schema.graph_operations.oneOf.length, 16);
		assert.equal(schema.library_operations.oneOf.length, 17);
		assert.equal(schema.runtime_actions.length, 16);
		assert.ok(schema.$defs.typedVariant);
		const checkReferences = (value: unknown): void => {
			if (Array.isArray(value)) { value.forEach(checkReferences); return; }
			if (!value || typeof value !== "object") return;
			const item = value as ObjectData;
			if (typeof item.$ref === "string") {
				assert.ok(item.$ref.startsWith("#/$defs/"), item.$ref);
				assert.ok(schema.$defs[item.$ref.slice("#/$defs/".length)], item.$ref);
			}
			Object.values(item).forEach(checkReferences);
		};
		checkReferences(schema);
		await assert.rejects(fs.access(path.join(project, ".godot", "godot_mcp", "session.json")), /ENOENT/, "Schema must work before any editor/session exists.");

		phase = "owned editor startup";
		child = spawn(executable, ["--headless", "--editor", "--path", project], {
			windowsHide: true, stdio: ["ignore", "pipe", "pipe"], cwd: project, env: isolatedEnvironment,
		});
		child.stdout?.on("data", (chunk) => { editorLog += chunk; });
		child.stderr?.on("data", (chunk) => { editorLog += chunk; });
		finished = new Promise<void>((resolve, reject) => {
			child!.once("close", () => { editorExited = true; resolve(); });
			child!.once("error", (error) => { editorExited = true; reject(error); });
		});
		finished.catch(() => {});
		const ready = await waitFile(path.join(control, "ready.json"));
		assert.equal(ready.pid, child.pid);
		assertOwnedPath(ready.user_data);
		await waitFile(path.join(project, ".godot", "godot_mcp", "session.json"));
		await lifecycle.refresh();
		assert.equal(lifecycle.get_info().pid, child.pid, "The lifecycle must be pinned to our editor, never the user's editor.");
		assert.equal(path.resolve(lifecycle.get_info().project_path!), path.resolve(project));
		await fs.access(path.join(bundle, "editor_data"));
		const active = await call("godot.scene.get_active");
		assert.equal(active.path, scenePath);
		const session = await call("godot.session.info");
		assert.equal(data(session.editor).animation_schema_version, 1);

		phase = "clip patches, typed tracks, and one-action undo";
		const initial = await snapshot();
		assert.equal(initial.controller_script, "res://tree_controller.gd");
		assert.equal(initial.controller_tag, "scene-controller", "A controller export must retain its authored value, not just its default.");
		const before = await inspect("godot.animation.list", "AnimationPlayer");
		assert.match(JSON.stringify(before), /RESET/);
		assert.doesNotMatch(JSON.stringify(before), /motion\/walk/);
		await inspect("godot.animation.get", "AnimationPlayer", { animation: "RESET", include_keys: true });
		await inspect("godot.animation.graph_get", "AnimationTree");
		const libraryOperations: Operation[] = [{ op: "add_library", library: "motion" }];
		for (const [clip, distance, loop] of [["idle", 0, 1], ["walk", 2, 1], ["shot", 4, 0]] as const) {
			const animation = `motion/${clip}`;
			libraryOperations.push(
				{ op: "create_animation", animation, properties: { length: 1, loop_mode: loop } },
				{ op: "add_track", animation, type: "position_3d", path: "Target" },
				{ op: "insert_key", animation, track: 0, time: 0, value: variant("Vector3", [0, 0, 0]) },
				{ op: "insert_key", animation, track: 0, time: 1, value: variant("Vector3", [distance, 0, 0]) },
			);
		}
		libraryOperations.push(
			{ op: "add_track", animation: "motion/walk", type: "rotation_3d", path: "Target" },
			{ op: "insert_key", animation: "motion/walk", track: 1, time: 0, value: variant("Quaternion", [0, 0, 0, 1]) },
			{ op: "add_track", animation: "motion/walk", type: "scale_3d", path: "Target" },
			{ op: "insert_key", animation: "motion/walk", track: 2, time: 0, value: variant("Vector3", [1, 1, 1]) },
			{ op: "add_track", animation: "motion/walk", type: "value", path: "Target:visible" },
			{ op: "insert_key", animation: "motion/walk", track: 3, time: 0, value: true },
			{ op: "duplicate_animation", animation: "motion/idle", new_name: "idle_copy" },
			{ op: "duplicate_animation", animation: "motion/walk", new_name: "temporary_copy" },
			{ op: "rename_animation", animation: "motion/temporary_copy", new_name: "renamed_copy" },
			{ op: "set_key", animation: "motion/renamed_copy", track: 0, key: 1, value: variant("Vector3", [9, 0, 0]) },
			{ op: "remove_animation", animation: "motion/renamed_copy" },
			{ op: "set_key", animation: "motion/idle_copy", track: 0, key: 1, value: variant("Vector3", [0.25, 0, 0]) },
		);
		const libraryResult = await patch("godot.animation.apply_patch", "AnimationPlayer", libraryOperations);
		assert.notEqual(libraryResult.scene_version, before.scene_version);
		const clips = await snapshot();
		assert.deepEqual(Object.keys(clips.libraries.motion).sort(), ["idle", "idle_copy", "shot", "walk"]);
		assert.equal(clips.libraries.motion.walk.tracks.length, 4);
		assert.deepEqual(clips.libraries.motion.walk.tracks[0].keys[1].value, [2, 0, 0]);
		assert.equal(clips.libraries.motion.walk.tracks[1].keys[0].type, "Quaternion");
		assert.deepEqual(clips.libraries.motion.walk.tracks[1].keys[0].value, [0, 0, 0, 1]);
		assert.deepEqual(clips.libraries.motion.idle_copy.tracks[0].keys[1].value, [0.25, 0, 0]);
		assert.deepEqual(await snapshot("undo"), initial, "A complete library patch must undo in one action.");
		assert.deepEqual(await snapshot("redo"), clips);
		const listed = await inspect("godot.animation.list", "AnimationPlayer");
		assert.match(JSON.stringify(listed), /walk/);
		assert.doesNotMatch(JSON.stringify(listed), /temporary_copy|renamed_copy/);
		const inspected = await inspect("godot.animation.get", "AnimationPlayer", { animation: "motion/walk", include_keys: true, key_limit: 20 });
		assert.match(JSON.stringify(inspected), /Target/);
		assert.match(JSON.stringify(inspected), /Quaternion/);

		phase = "clip dry-run, late rollback, and stale guards";
		const clipGuard = await guards("AnimationPlayer");
		const dryClip = await call("godot.animation.apply_patch", { ...clipGuard, dry_run: true, operations: [{ op: "create_animation", animation: "motion/dry_probe" }] });
		assert.equal(dryClip.scene_version, clipGuard.expected_scene_version);
		assert.deepEqual(await snapshot(), clips);
		await rejected("godot.animation.apply_patch", { ...clipGuard, operations: [
			{ op: "create_animation", animation: "motion/rollback_probe" },
			{ op: "insert_key", animation: "motion/walk", track: 999, time: 0, value: 1 },
		] });
		assert.deepEqual(await snapshot(), clips, "A late invalid operation must roll back earlier clip edits.");
		assert.equal((await guards("AnimationPlayer")).expected_scene_version, clipGuard.expected_scene_version);
		await rejected("godot.animation.apply_patch", {
			...clipGuard, expected_scene_version: before.scene_version,
			operations: [{ op: "remove_animation", animation: "motion/walk" }],
		}, /version|stale|conflict/i);
		assert.deepEqual(await snapshot(), clips);
		// Neither a dry run nor a failed patch may add an undo entry.
		assert.deepEqual(await snapshot("undo"), initial);
		assert.deepEqual(await snapshot("redo"), clips);

		phase = "typed dictionary, packed values, and inspection reinsertion through MCP";
		await patch("godot.animation.apply_patch", "AnimationPlayer", [
			{ op: "create_animation", animation: "motion/schema_probe" },
			{ op: "add_track", animation: "motion/schema_probe", type: "value", path: "Target:metadata/probe" },
			{ op: "insert_key", animation: "motion/schema_probe", track: 0, time: 0, value: {
				__type: "Dictionary", value: [
					["__type", "literal"],
					[4.5, variant("PackedVector3Array", [variant("Vector3", [1, 2, 3])])],
					[variant("int", "4"), variant("PackedInt64Array", [variant("int", "9223372036854775807"), variant("int", "-9223372036854775808")])],
					["bytes", variant("PackedByteArray", [0, 255])],
					["int32", variant("PackedInt32Array", [-2_147_483_648, 2_147_483_647])],
					["matrix", variant("Basis", [1, 2, 3, 4, 5, 6, 7, 8, 9])],
					["transform", variant("Transform3D", [1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 3, 4])],
					["color", variant("Color", "#ff0000")],
				],
			} },
		]);
		const typedRead = await inspect("godot.animation.get", "AnimationPlayer", { animation: "motion/schema_probe", include_keys: true });
		assert.equal(typedRead.truncated, false);
		assert.equal(typedRead.tracks[0].keys[0].value.__type, "Dictionary");
		const typedEntries = typedRead.tracks[0].keys[0].value.value as unknown[][];
		assert.ok(typedEntries.some(([key, entry]) => JSON.stringify(key) === JSON.stringify(variant("int", "4")) && JSON.stringify(entry) === JSON.stringify(variant("PackedInt64Array", [variant("int", "9223372036854775807"), variant("int", "-9223372036854775808")]))), "Integer and float Dictionary keys, and full-range int64 arrays, must remain lossless.");
		await patch("godot.animation.apply_patch", "AnimationPlayer", [{ op: "insert_key", animation: "motion/schema_probe", track: 0, time: 0.5, value: typedRead.tracks[0].keys[0].value }]);
		const typedAfter = await inspect("godot.animation.get", "AnimationPlayer", { animation: "motion/schema_probe", include_keys: true });
		assert.equal(typedAfter.returned_key_count, 2);
		assert.deepEqual(typedAfter.tracks[0].keys[1].value, typedAfter.tracks[0].keys[0].value);
		await patch("godot.animation.apply_patch", "AnimationPlayer", [{ op: "remove_animation", animation: "motion/schema_probe" }]);
		assert.deepEqual(await snapshot(), clips);

		phase = "nested graph patches, transitions, points, connections, and filters";
		const graphOperations: Operation[] = [
			{ op: "set_root", type: "AnimationNodeBlendTree" },
			{ op: "add_node", name: "Controller", type: "AnimationNodeStateMachine", position: [0, 0] },
			{ op: "add_node", graph_path: "Controller", name: "Idle", type: "AnimationNodeAnimation", position: [0, 0], properties: { animation: "motion/idle" } },
			{ op: "add_node", graph_path: "Controller", name: "Stride", type: "AnimationNodeBlendSpace1D", position: [200, 0], properties: { min_space: 0, max_space: 1 } },
			{ op: "add_blend_point", graph_path: "Controller/Stride", name: "idle", position: 0, properties: { animation: "motion/idle" } },
			{ op: "add_blend_point", graph_path: "Controller/Stride", name: "walk", position: 1, properties: { animation: "motion/walk" } },
			{ op: "add_node", graph_path: "Controller", name: "Direction", type: "AnimationNodeBlendSpace2D", position: [400, 0], properties: { auto_triangles: false } },
			{ op: "add_blend_point", graph_path: "Controller/Direction", name: "left", position: [-1, -1], properties: { animation: "motion/idle" } },
			{ op: "add_blend_point", graph_path: "Controller/Direction", name: "right", position: [1, -1], properties: { animation: "motion/walk" } },
			{ op: "add_blend_point", graph_path: "Controller/Direction", name: "forward", position: [0, 1], properties: { animation: "motion/idle_copy" } },
			{ op: "set_triangles", graph_path: "Controller/Direction", triangles: [[0, 1, 2]] },
			{ op: "add_transition", graph_path: "Controller", from: "Start", to: "Idle", properties: { advance_mode: 2 } },
			{ op: "add_transition", graph_path: "Controller", from: "Idle", to: "Stride", properties: { advance_mode: 2, advance_condition: "moving" } },
			{ op: "set_transition", graph_path: "Controller", from: "Idle", to: "Stride", properties: { switch_mode: 0, xfade_time: 0.05, reset: true } },
			{ op: "add_transition", graph_path: "Controller", from: "Stride", to: "Direction", properties: { advance_mode: 1, xfade_time: 0.05 } },
			{ op: "add_transition", graph_path: "Controller", from: "Direction", to: "Idle", properties: { advance_mode: 2, advance_condition: "stopped" } },
			{ op: "add_transition", graph_path: "Controller", from: "Stride", to: "Idle", properties: { advance_mode: 1, advance_condition: "playback" } },
			{ op: "add_transition", graph_path: "Controller", from: "Idle", to: "Direction", properties: { advance_mode: 1, advance_condition: "observer" } },
			{ op: "add_transition", graph_path: "Controller", from: "Direction", to: "Stride", properties: { advance_mode: 1, advance_condition: "current_position" } },
			{ op: "add_node", name: "Shot", type: "AnimationNodeAnimation", position: [0, 200], properties: { animation: "motion/shot" } },
			{ op: "add_node", name: "OneShot", type: "AnimationNodeOneShot", position: [600, 0], properties: { fadein_time: 0, fadeout_time: 0 } },
			{ op: "set_filter", node_path: "OneShot", enabled: true, paths: ["Target"] },
			{ op: "add_node", name: "Rest", type: "AnimationNodeAnimation", properties: { animation: "motion/idle" } },
			{ op: "add_node", name: "Switch", type: "AnimationNodeTransition", properties: { input_count: 2, xfade_time: 0 } },
			{ op: "set_node_properties", node_path: "Switch", properties: { "input_0/name": "locomotion", "input_1/name": "rest" } },
			{ op: "add_node", name: "Speed", type: "AnimationNodeTimeScale" },
			{ op: "connect", from: "Controller", to: "Speed", input: 0 },
			{ op: "connect", from: "Speed", to: "OneShot", input: 0 },
			{ op: "connect", from: "Shot", to: "OneShot", input: 1 },
			{ op: "connect", from: "OneShot", to: "Switch", input: 0 },
			{ op: "connect", from: "Rest", to: "Switch", input: 1 },
			{ op: "connect", from: "Switch", to: "output", input: 0 },
			{ op: "add_node", name: "scratch", type: "AnimationNodeAnimation", position: [0, 0], properties: { animation: "motion/idle" } },
			{ op: "rename_node", name: "scratch", new_name: "scratch_renamed" },
			{ op: "move_node", name: "scratch_renamed", position: [25, 50] },
			{ op: "remove_node", name: "scratch_renamed" },
			{ op: "move_node", name: "OneShot", position: [650, 25] },
		];
		const oldGraphGuard = await guards("AnimationTree");
		const graphResult = await patch("godot.animation.graph_apply_patch", "AnimationTree", graphOperations);
		assert.notEqual(graphResult.scene_version, oldGraphGuard.expected_scene_version);
		const graph = await snapshot();
		assert.equal(graph.graph.type, "AnimationNodeBlendTree");
		assert.equal(graph.graph.nodes.Controller.graph.type, "AnimationNodeStateMachine");
		assert.equal(graph.graph.nodes.Controller.graph.nodes.Stride.graph.points.length, 2);
		assert.equal(graph.graph.nodes.Controller.graph.nodes.Direction.graph.points.length, 3);
		assert.deepEqual(graph.graph.nodes.Controller.graph.nodes.Direction.graph.triangles, [[0, 1, 2]]);
		assert.equal(graph.graph.connections.length, 18, "Six connections must survive packing (three entries per connection).");
		assert.equal(graph.graph.nodes.OneShot.graph.filter_enabled, true);
		assert.equal(graph.graph.nodes.OneShot.graph.target_filtered, true);
		assert.deepEqual(graph.graph.nodes.OneShot.position, [650, 25]);
		assert.ok(graph.graph.nodes.Controller.graph.transitions.some((item: ObjectData) => item.condition === "moving" && Math.abs(item.xfade_time - 0.05) < 0.001));
		assert.deepEqual(await snapshot("undo"), clips, "The whole nested graph patch must undo in one action.");
		assert.deepEqual(await snapshot("redo"), graph);
		const graphRead = await inspect("godot.animation.graph_get", "AnimationTree", { max_depth: 12, max_nodes: 100, include_properties: true });
		assert.equal(graphRead.parameters_partial, false, "Ordinary controller scripts must not block native parameter inspection.");
		assert.equal(graphRead.parameters["parameters/Speed/scale"], 1);
		for (const type of ["AnimationNodeBlendTree", "AnimationNodeStateMachine", "AnimationNodeBlendSpace1D", "AnimationNodeBlendSpace2D", "AnimationNodeOneShot"]) assert.match(JSON.stringify(graphRead), new RegExp(type));
		assert.match(JSON.stringify(graphRead), /moving/);
		const validation = await inspect("godot.animation.graph_validate", "AnimationTree");
		if ("valid" in validation) assert.equal(validation.valid, true, JSON.stringify(validation));
		assert.doesNotMatch(diagnosticText(validation), /"severity"\s*:\s*"error"/i, JSON.stringify(validation));
		const missingClip = await inspect("godot.animation.graph_validate", "MissingClipTree");
		assert.match(diagnosticText(missingClip), /missing_clip|missing.*animation|animation.*missing/i, JSON.stringify(missingClip));
		const invalidBinding = await inspect("godot.animation.graph_validate", "UnboundTree");
		assert.match(diagnosticText(invalidBinding), /player|binding|MissingPlayer/i, JSON.stringify(invalidBinding));

		phase = "graph dry-run, late rollback, and stale guards";
		const graphGuard = await guards("AnimationTree");
		const dryGraph = await call("godot.animation.graph_apply_patch", { ...graphGuard, dry_run: true, operations: [{ op: "set_node_properties", node_path: "Controller/Idle", properties: { animation: "motion/walk" } }] });
		assert.equal(dryGraph.scene_version, graphGuard.expected_scene_version);
		assert.deepEqual(await snapshot(), graph);
		await rejected("godot.animation.graph_apply_patch", { ...graphGuard, operations: [
			{ op: "add_node", name: "rollback_probe", type: "AnimationNodeAnimation", properties: { animation: "motion/idle" } },
			{ op: "connect", from: "rollback_probe", to: "OneShot", input: 999 },
		] });
		assert.deepEqual(await snapshot(), graph);
		assert.equal((await guards("AnimationTree")).expected_scene_version, graphGuard.expected_scene_version);
		await rejected("godot.animation.graph_apply_patch", { ...oldGraphGuard, operations: [{ op: "remove_node", name: "Shot" }] }, /version|stale|conflict/i);
		assert.deepEqual(await snapshot(), graph);
		assert.deepEqual(await snapshot("undo"), clips);
		assert.deepEqual(await snapshot("redo"), graph);

		phase = "graph node renames retain authored parameters across undo and redo";
		const setSpeed = async (speed: number) => {
			const current = await guards("AnimationTree");
			await call("godot.scene.apply_patch", {
				expected_scene_path: current.expected_scene_path, expected_scene_version: current.expected_scene_version,
				operations: [{ op: "update_node", node: "AnimationTree", properties: { "parameters/Speed/scale": speed } }],
			});
		};
		await setSpeed(2);
		const parameterizedGraph = await snapshot();
		assert.equal(parameterizedGraph.parameters["parameters/Speed/scale"], 2);
		await patch("godot.animation.graph_apply_patch", "AnimationTree", [{ op: "rename_node", name: "Speed", new_name: "Fast" }]);
		const renamedGraph = await snapshot();
		assert.equal(renamedGraph.parameters["parameters/Fast/scale"], 2);
		assert.equal("parameters/Speed/scale" in renamedGraph.parameters, false);
		assert.deepEqual(await snapshot("undo"), parameterizedGraph);
		assert.deepEqual(await snapshot("redo"), renamedGraph);
		await patch("godot.animation.graph_apply_patch", "AnimationTree", [{ op: "rename_node", name: "Fast", new_name: "Speed" }]);
		await setSpeed(1);
		assert.deepEqual(await snapshot(), graph);

		phase = "saved, reloaded, and packed animation persistence";
		await call("godot.resource.save", { path: scenePath });
		const saved = await fs.readFile(path.join(project, "main.tscn"), "utf8");
		assert.match(saved, /AnimationNodeStateMachine/);
		assert.match(saved, /AnimationNodeBlendSpace2D/);
		assert.match(saved, /motion\/walk/);
		assert.match(saved, /tree_controller\.gd/);
		assert.match(saved, /controller_tag = "scene-controller"/);
		await call("godot.resource.reload", { path: scenePath });
		assert.deepEqual(await snapshot(), graph);
		await inspect("godot.animation.get", "AnimationPlayer", { animation: "motion/walk", include_keys: true });
		await inspect("godot.animation.graph_get", "AnimationTree");

		phase = "owned runtime AnimationPlayer play/seek/queue/advance";
		await startRuntime();
		assert.equal(data(await wait_for_runtime_operation(lifecycle, "runtime.call", {
			run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, method: "controller_marker", timeout_ms: 20_000,
		})).result, "scene-controller", "The preserved controller must still execute in the owned game runtime.");
		await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationPlayer`, timeout_ms: 20_000 });
		const played = await runtimeControl("AnimationPlayer", "player_play", { animation: "motion/walk" });
		assert.ok(hasStringValue(played, "motion/walk"), "Player control must return its resulting playback snapshot.");
		assert.equal((await runtimeSnapshot()).animation, "motion/walk");
		const playerRead = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationPlayer`, timeout_ms: 20_000 });
		assert.equal(playerRead.player.current_animation, "motion/walk");
		assert.equal(playerRead.player.assigned_animation, "motion/walk");
		assert.equal(playerRead.player.playing, true);
		assert.equal(playerRead.player.valid, true);
		assert.equal(playerRead.player.length, 1);
		assert.equal(playerRead.player.position, 0);
		const boundedPlayer = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationPlayer`, limit: 1, timeout_ms: 20_000 });
		assert.equal(boundedPlayer.animations.length, 1);
		assert.equal(boundedPlayer.truncated, true);
		await runtimeControl("AnimationPlayer", "player_seek", { time: 0.25, update: true });
		let runtime = await runtimeSnapshot();
		assert.ok(Math.abs(runtime.position - 0.25) < 0.001, JSON.stringify(runtime));
		assert.ok(Math.abs(runtime.target_position[0] - 0.5) < 0.01, JSON.stringify(runtime));
		await runtimeControl("AnimationPlayer", "player_advance", { delta: 0.25 });
		runtime = await runtimeSnapshot();
		assert.ok(Math.abs(runtime.position - 0.5) < 0.001, JSON.stringify(runtime));
		assert.ok(Math.abs(runtime.target_position[0] - 1) < 0.01, JSON.stringify(runtime));
		await runtimeControl("AnimationPlayer", "player_queue", { animation: "motion/shot" });
		assert.deepEqual((await runtimeSnapshot()).queue, ["motion/shot"]);
		const pausedPlayer = await runtimeControl("AnimationPlayer", "player_pause");
		assert.equal(pausedPlayer.player.valid, true);
		runtime = await runtimeSnapshot();
		assert.equal(runtime.playing, false);
		assert.equal(runtime.assigned_animation, "motion/walk");
		assert.ok(Math.abs(runtime.position - 0.5) < 0.001);
		await runtimeControl("AnimationPlayer", "player_play");
		assert.ok(Math.abs((await runtimeSnapshot()).position - 0.5) < 0.001);
		const stoppedPlayer = await runtimeControl("AnimationPlayer", "player_stop");
		assert.equal(stoppedPlayer.player.valid, false);
		runtime = await runtimeSnapshot();
		assert.equal(runtime.playing, false);
		assert.deepEqual(runtime.target_position, [0, 0, 0]);
		const freed = await call("godot.runtime.call", { run_id: activeRun, node_path: runtimeRoot, method: "runtime_freed_reference", arguments: [], timeout_ms: 20_000 });
		assert.deepEqual(freed.result, { freed_node: null }, "Retained references freed during gameplay callbacks must serialize safely.");

		phase = "typed tree parameters, nested state playback, and one-shot requests";
		const playbackPath = "parameters/Controller/playback";
		const blendParameter = "parameters/Controller/Direction/blend_position";
		const oneshotParameter = "parameters/OneShot";
		await runtimeControl("AnimationTree", "set_parameter", { parameter: blendParameter, value: variant("Vector2", [0.25, 0.25]) });
		assert.deepEqual((await runtimeSnapshot()).blend2d, [0.25, 0.25]);
		await runtimeControl("AnimationTree", "set_parameter", { parameter: "parameters/Controller/Stride/blend_position", value: 0.5 });
		const conditionNames = ["playback", "observer", "current_position"].map((name) => `parameters/Controller/conditions/${name}`);
		for (const parameter of conditionNames) await runtimeControl("AnimationTree", "set_parameter", { parameter, value: true });
		const conditionRead = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, playback_path: playbackPath, parameter_names: conditionNames, timeout_ms: 20_000 });
		for (const parameter of conditionNames) {
			assert.equal(conditionRead.parameters[parameter].value, true);
			assert.equal(conditionRead.parameters[parameter].read_only, false);
			await runtimeControl("AnimationTree", "set_parameter", { parameter, value: false });
		}
		await runtimeControl("AnimationTree", "set_active", { active: true });
		await runtimeControl("AnimationTree", "state_start", { playback_path: playbackPath, state: "Idle", reset: true });
		await step();
		runtime = await runtimeSnapshot();
		assert.equal(runtime.state.current, "Idle", JSON.stringify(runtime));
		await runtimeControl("AnimationTree", "set_parameter", { parameter: "parameters/Controller/conditions/moving", value: true });
		await step(12);
		assert.equal((await runtimeSnapshot()).state.current, "Stride", "A true advance condition must drive an automatic transition.");
		await runtimeControl("AnimationTree", "state_travel", { playback_path: playbackPath, state: "Direction" });
		await step(12);
		runtime = await runtimeSnapshot();
		assert.equal(runtime.state.current, "Direction");
		assert.equal(runtime.state.playing, true);
		const stateRead = await call("godot.runtime.animation_get", {
			run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, playback_path: playbackPath,
			parameter_names: [blendParameter, "parameters/OneShot/active"], limit: 4, timeout_ms: 20_000,
		});
		assertFinite(stateRead);
		assert.equal(stateRead.playback.current_node, "Direction");
		assert.equal(stateRead.playback.playing, true);
		assert.deepEqual(Object.keys(stateRead.parameters).sort(), [blendParameter, "parameters/OneShot/active"].sort());
		assert.deepEqual(stateRead.parameters[blendParameter].value, [0.25, 0.25]);
		for (const [field, size] of Object.entries({ position: 3, rotation: 4, scale: 3, position_accumulator: 3, rotation_accumulator: 4, scale_accumulator: 3 })) {
			assert.equal(stateRead.root_motion[field].length, size, field);
			assert.ok(stateRead.root_motion[field].every((component: unknown) => typeof component === "number" && Number.isFinite(component)), field);
		}
		assert.match(JSON.stringify(stateRead), /root_motion/i, "Tree inspection must include bounded root-motion data.");
		assert.ok(Buffer.byteLength(JSON.stringify(stateRead)) < 64 * 1024, "Bounded tree inspection must not dump unbounded graph/resource data.");
		assert.ok(runtime.root_motion.every((component: number) => Number.isFinite(component) && Math.abs(component) < 10));
		await runtimeControl("AnimationTree", "oneshot_fire", { parameter: oneshotParameter });
		await step(1);
		assert.equal((await runtimeSnapshot()).oneshot_active, true);
		await runtimeControl("AnimationTree", "oneshot_abort", { parameter: oneshotParameter });
		await step(1);
		assert.equal((await runtimeSnapshot()).oneshot_active, false);
		await runtimeControl("AnimationTree", "set_parameter", { parameter: `${oneshotParameter}/request`, value: variant("int", "1") });
		await step(1);
		assert.equal((await runtimeSnapshot()).oneshot_active, true);
		await runtimeControl("AnimationTree", "oneshot_fadeout", { parameter: oneshotParameter });
		await step(1);
		assert.equal((await runtimeSnapshot()).oneshot_active, false);
		await runtimeControl("AnimationTree", "transition_request", { parameter: "parameters/Switch", transition: "rest" });
		await step();
		const switchRead = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, parameter_names: ["parameters/Switch/current_state"], timeout_ms: 20_000 });
		assert.equal(switchRead.parameters["parameters/Switch/current_state"].value, "rest");
		assert.equal(switchRead.parameters["parameters/Switch/current_state"].read_only, true);
		await runtimeControl("AnimationTree", "transition_request", { parameter: "parameters/Switch", transition: "locomotion" });
		await runtimeControl("AnimationTree", "set_parameter", { parameter: "parameters/Controller/conditions/moving", value: false });
		await runtimeControl("AnimationTree", "state_next", { playback_path: playbackPath });
		await step();
		assert.equal((await runtimeSnapshot()).state.playing, true);
		await runtimeControl("AnimationTree", "state_stop", { playback_path: playbackPath });
		await step();
		assert.equal((await runtimeSnapshot()).state.playing, false);
		await runtimeControl("AnimationTree", "set_active", { active: false });

		phase = "owned runtime suspension and resumption";
		const beforeResume = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, timeout_ms: 20_000 });
		assert.equal(beforeResume.suspended, true);
		await call("godot.runtime.resume", { run_id: activeRun });
		await step(3);
		const resumed = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, timeout_ms: 20_000 });
		assert.equal(resumed.suspended, false);
		assert.ok(resumed.physics_frame > beforeResume.physics_frame);
		await call("godot.runtime.pause", { run_id: activeRun });
		const suspended = await call("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/AnimationTree`, timeout_ms: 20_000 });
		assert.equal(suspended.suspended, true);

		phase = "runtime input rejection and stale-run protection";
		const validRuntime = await runtimeSnapshot();
		const invalidControls = [
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "not_an_action" },
			{ node_path: `${runtimeRoot}/Target`, action: "player_play", animation: "motion/walk" },
			{ node_path: `${runtimeRoot}/MissingPlayer`, action: "player_play", animation: "motion/walk" },
			{ node_path: `${runtimeRoot}/AnimationPlayer`, action: "player_play", animation: "motion/missing_clip" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "state_start", playback_path: "parameters/missing/playback", state: "Idle" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "state_start", playback_path: playbackPath, state: "MissingState" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "set_parameter", parameter: blendParameter, value: "not a Vector2" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "set_parameter", parameter: `${oneshotParameter}/request`, value: variant("int", "9223372036854775808") },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "set_parameter", parameter: "parameters/Unknown/value", value: 1 },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "oneshot_fire", parameter: "parameters/Unknown/request" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "set_parameter", parameter: "parameters/Switch/current_state", value: "rest" },
			{ node_path: `${runtimeRoot}/AnimationTree`, action: "transition_request", parameter: "parameters/Switch", transition: "missing_input" },
		];
		for (const invalid of invalidControls) await rejected("godot.runtime.animation_control", { run_id: activeRun, timeout_ms: 20_000, ...invalid });
		await rejected("godot.runtime.animation_get", { run_id: activeRun, node_path: `${runtimeRoot}/Target`, timeout_ms: 20_000 });
		assert.deepEqual(await runtimeSnapshot(), validRuntime, "Rejected runtime controls must not mutate playback or parameters.");
		const staleRun = activeRun!;
		await call("godot.runtime.stop", { run_id: staleRun, confirm: true });
		activeRun = undefined;
		ownedRuntimePid = undefined;
		await startRuntime();
		assert.notEqual(activeRun, staleRun);
		const replacement = await runtimeSnapshot();
		await rejected("godot.runtime.animation_control", { run_id: staleRun, node_path: `${runtimeRoot}/AnimationPlayer`, action: "player_play", animation: "motion/walk", timeout_ms: 20_000 }, /run|stale|owned/i);
		await rejected("godot.runtime.animation_get", { run_id: staleRun, node_path: `${runtimeRoot}/AnimationTree`, timeout_ms: 20_000 }, /run|stale|owned/i);
		assert.deepEqual(await runtimeSnapshot(), replacement, "An old run ID must not target a replacement runtime.");
		assert.deepEqual([...successfulRuntimeActions].sort(), [...schema.runtime_actions].sort(), "Every published runtime animation action must execute successfully in the owned runtime.");

		const runtimeLog = await fs.readFile(path.join(control, "runtime.log"), "utf8");
		assert.doesNotMatch(editorLog, /(?:^|\n)(?:SCRIPT ERROR|ERROR|WARNING):/, "The isolated editor workflow must finish without native or script diagnostics.");
		assert.doesNotMatch(runtimeLog, /(?:^|\n)(?:SCRIPT ERROR|ERROR|WARNING):/, "The owned runtime workflow must finish without native or script diagnostics.");
		t.diagnostic(`Verified all ${successfulRuntimeActions.size} runtime actions, typed round trips, controller script/export preservation, editor undo/redo, persistence, and stale-run guards without native or script diagnostics.`);
	} catch (error) {
		const runtimeLog = await fs.readFile(path.join(control, "runtime.log"), "utf8").catch(() => "(No runtime log was created.)");
		throw new Error(`Animation integration failed during ${phase}: ${String(error)}\nRecent SDK requests:\n${transcript.join("\n")}\nBridge stderr:\n${bridgeLog}\nOwned editor log:\n${editorLog}\nOwned runtime log:\n${runtimeLog}`, { cause: error });
	}
});
