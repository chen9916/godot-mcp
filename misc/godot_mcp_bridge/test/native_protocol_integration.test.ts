import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { SessionLifecycle } from "../src/session_lifecycle.js";

const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const source = [
	"extends Node",
	"var target: int = 1",
	"func read_value() -> int:",
	"    # target appears only in this comment.",
	'    var quoted = "target"',
	'    var multiline = """target',
	'target"""',
	"    print(quoted, multiline)",
	"    return target",
	"",
].join("\n");

const plugin = `@tool
extends EditorPlugin

var probe_ready := false
var directory := ProjectSettings.globalize_path("res://")

func _enter_tree():
	_setup.call_deferred()

func _write(name, value):
	var file = FileAccess.open(directory.path_join(name), FileAccess.WRITE)
	file.store_string(JSON.stringify(value))
	file.close()

func _setup():
	await get_tree().create_timer(1.0).timeout
	EditorInterface.get_editor_settings().set_setting("network/godot_mcp/enabled", true)
	EditorInterface.open_scene_from_path("res://main.tscn")
	await get_tree().create_timer(0.5).timeout
	probe_ready = true
	_write("ready.json", {"ready": true})

func _owned(node, edited):
	var result = true
	for child in node.get_children():
		result = result and child.owner == edited and _owned(child, edited)
	return result

func _process(_delta):
	if not probe_ready:
		return
	var command_path = directory.path_join("command.json")
	if not FileAccess.file_exists(command_path):
		return
	var command = JSON.parse_string(FileAccess.get_file_as_string(command_path))
	DirAccess.remove_absolute(command_path)
	if command.action == "quit":
		get_tree().quit()
		return
	var edited = EditorInterface.get_edited_scene_root()
	var manager = get_undo_redo()
	var history = manager.get_history_undo_redo(manager.get_object_history_id(edited))
	if command.action == "undo":
		history.undo()
	elif command.action == "redo":
		history.redo()
	var packed = PackedScene.new()
	packed.pack(edited)
	var order = []
	for child in edited.get_node("Order").get_children():
		order.append(child.name)
	_write("snapshot.json", {"owned": _owned(edited, edited), "packed_count": packed.get_state().get_node_count(), "order": order})
`;

function data(result: unknown): Record<string, any> {
	const envelope = result as Record<string, any>;
	assert.equal(envelope.ok, true, JSON.stringify(envelope));
	return envelope.data;
}

async function rawCall(session: Record<string, any>, method: string, params: Record<string, unknown>, fragmented = false): Promise<Record<string, any>> {
	const socket = net.createConnection({ host: "127.0.0.1", port: session.port });
	socket.setNoDelay(true);
	socket.setEncoding("utf8");
	let text = "";
	const response = new Promise<Record<string, any>>((resolve, reject) => {
		socket.on("data", (chunk) => {
			text += chunk;
			const eol = text.indexOf("\n");
			if (eol >= 0) resolve(JSON.parse(text.slice(0, eol)));
		});
		socket.on("error", reject);
		socket.on("close", () => { if (!text.includes("\n")) reject(new Error("RPC closed before response")); });
	});
	response.catch(() => {});
	const timer = setTimeout(() => socket.destroy(new Error("Raw RPC timed out")), 15_000);
	try {
		await new Promise<void>((resolve, reject) => { socket.once("connect", resolve); socket.once("error", reject); });
		const request = Buffer.from(JSON.stringify({ jsonrpc: "2.0", id: 77, method, params: { ...params, _session_id: session.session_id } }) + "\n");
		if (fragmented) {
			// Force separate editor polls to see incomplete three- and four-byte characters.
			const offset = request.indexOf(Buffer.from("猫🙂"));
			assert.ok(offset >= 0);
			let start = 0;
			for (const end of [offset + 1, offset + 2, offset + 4, offset + 5, offset + 6, request.length]) {
				socket.write(request.subarray(start, end));
				start = end;
				await delay(100);
			}
		} else socket.write(request);
		return await response;
	} finally {
		clearTimeout(timer);
		socket.destroy();
	}
}

test("isolated native protocol guards ownership, resource lifetime, UTF-8 framing and GDScript queries", {
	skip: process.platform === "win32" && process.env.GODOT_MCP_PROTOCOL_EDITOR ? false :
		"Set GODOT_MCP_PROTOCOL_EDITOR to a rebuilt Windows editor for isolated native protocol regressions.",
	timeout: 240_000,
}, async (t) => {
	const temporaryRoot = process.env.LOCALAPPDATA ? path.join(process.env.LOCALAPPDATA, "Temp", "opencode") : os.tmpdir();
	await fs.mkdir(temporaryRoot, { recursive: true });
	const directory = await fs.mkdtemp(path.join(temporaryRoot, "godot-mcp-protocol-"));
	const bundle = path.join(directory, "bundle");
	const project = path.join(directory, "project");
	const profile = path.join(directory, "profile");
	const temporary = path.join(directory, "temp");
	const env = {
		...process.env,
		APPDATA: path.join(profile, "AppData", "Roaming"), LOCALAPPDATA: path.join(profile, "AppData", "Local"),
		USERPROFILE: profile, HOME: profile, TEMP: temporary, TMP: temporary, TMPDIR: temporary,
		XDG_CONFIG_HOME: path.join(profile, "config"), XDG_DATA_HOME: path.join(profile, "data"), XDG_CACHE_HOME: path.join(profile, "cache"),
	};
	for (const relative of ["AppData/Roaming", "AppData/Local", "config", "data", "cache"]) {
		await fs.mkdir(path.join(profile, relative), { recursive: true });
	}
	await fs.mkdir(temporary);
	await fs.mkdir(bundle);
	await fs.mkdir(path.join(project, "addons", "probe"), { recursive: true });
	await fs.copyFile(path.resolve(process.env.GODOT_MCP_PROTOCOL_EDITOR!), path.join(bundle, "Godot.exe"));
	await fs.writeFile(path.join(bundle, "_sc_"), "");
	await fs.writeFile(path.join(project, "project.godot"),
		'[application]\nconfig/name="MCP Protocol Regression"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/probe/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n');
	await fs.writeFile(path.join(project, "main.tscn"),
		'[gd_scene format=3]\n[node name="Main" type="Node3D"]\n[node name="ParentA" type="Node3D" parent="."]\n[node name="Moved" type="Node3D" parent="ParentA"]\n[node name="Descendant" type="Node3D" parent="ParentA/Moved"]\n[node name="ParentB" type="Node3D" parent="."]\n[node name="Order" type="Node" parent="."]\n[node name="A" type="Node" parent="Order"]\n[node name="B" type="Node" parent="Order"]\n[node name="C" type="Node" parent="Order"]\n');
	await fs.writeFile(path.join(project, "lsp.gd"), source);
	await fs.writeFile(path.join(project, "first_line.gd"), "var target: int = 1\nfunc read_value() -> int:\n    return target\n");
	await fs.writeFile(path.join(project, "addons", "probe", "plugin.cfg"),
		'[plugin]\nname="Protocol Probe"\ndescription="Disposable regression"\nauthor="Godot tests"\nversion="1"\nscript="plugin.gd"\n');
	await fs.writeFile(path.join(project, "addons", "probe", "plugin.gd"), plugin);
	const child = spawn(path.join(bundle, "Godot.exe"), ["--headless", "--editor", "--path", project], {
		windowsHide: true, stdio: ["ignore", "pipe", "pipe"], env,
	});
	let log = "";
	child.stdout.on("data", (chunk) => { log += chunk; });
	child.stderr.on("data", (chunk) => { log += chunk; });
	const finished = new Promise<void>((resolve, reject) => {
		child.once("exit", () => resolve());
		child.once("error", reject);
	});
	finished.catch(() => {});
	const lifecycle = new SessionLifecycle({ project_root: project, timeout_ms: 15_000 });
	const sendCommand = async (action: string) => {
		// Publish only a complete file: the editor polls independently and may
		// otherwise consume an empty/partial JSON command under concurrent load.
		const command = path.join(project, "command.json");
		await fs.writeFile(command + ".tmp", JSON.stringify({ action }));
		await fs.rename(command + ".tmp", command);
	};
	t.after(async () => {
		await lifecycle.close();
		if (child.exitCode === null) {
			await sendCommand("quit");
			await Promise.race([finished, delay(5000)]);
			if (child.exitCode === null) child.kill();
		}
		await finished.catch(() => {});
		await fs.rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
	});
	const waitFile = async (name: string): Promise<Record<string, any>> => {
		const deadline = Date.now() + 45_000;
		while (Date.now() < deadline) {
			try { return JSON.parse(await fs.readFile(path.join(project, name), "utf8")); }
			catch (error: any) { if (error.code !== "ENOENT" && !(error instanceof SyntaxError)) throw error; }
			if (child.exitCode !== null) throw new Error("Editor exited:\n" + log);
			await delay(100);
		}
		throw new Error("Timed out waiting for " + name + ":\n" + log);
	};
	const snapshot = async (action = "snapshot") => {
		await fs.rm(path.join(project, "snapshot.json"), { force: true });
		await sendCommand(action);
		return waitFile("snapshot.json");
	};
	const call = async (method: string, params: Record<string, unknown> = {}) => data(await lifecycle.call(method, params));

	try {
		await waitFile("ready.json");
		const session = await waitFile(".godot/godot_mcp/session.json");
		await lifecycle.refresh();
		const initial = await snapshot();
		assert.equal(initial.owned, true);
		assert.equal(initial.packed_count, 9);

		for (const node_path of ["/root", "..", "../.."]) {
			await assert.rejects(lifecycle.call("node.set_properties", { node_path, properties: { process_priority: 777 } }), /NOT_FOUND/);
			await assert.rejects(lifecycle.call("node.get_property", { node_path, property_name: "process_priority" }), /NOT_FOUND/);
		}
		const reparent = await call("node.reparent", { node_path: "ParentA/Moved", new_parent_path: "ParentB", new_name: "Renamed" });
		assert.equal(reparent.node_path, "ParentB/Renamed");
		assert.deepEqual(await snapshot(), initial);
		assert.deepEqual(await snapshot("undo"), initial);
		assert.deepEqual(await snapshot("redo"), initial);
		await snapshot("undo");

		await call("node.delete", { node_path: "ParentA/Moved", force: true });
		assert.equal((await snapshot()).packed_count, 7);
		assert.deepEqual(await snapshot("undo"), initial);
		assert.equal((await snapshot("redo")).packed_count, 7);
		assert.deepEqual(await snapshot("undo"), initial);

		// Both ancestor/descendant and adjacent siblings must restore in one undo action.
		const duplicateTargets = await call("node.delete_batch", { force: true, items: [
			{ node_path: "Order/A" }, { node_path: "Order/../Order/A" },
		] });
		assert.equal(duplicateTargets.applied, false);
		assert.equal(duplicateTargets.failure_count, 1);
		assert.deepEqual(await snapshot(), initial);
		await call("node.delete_batch", { force: true, items: [
			{ node_path: "ParentA/Moved/Descendant" }, { node_path: "ParentA/Moved" },
			{ node_path: "Order/A" }, { node_path: "Order/B" },
		] });
		assert.equal((await snapshot()).packed_count, 5);
		assert.deepEqual(await snapshot("undo"), initial);
		assert.equal((await snapshot("redo")).packed_count, 5);
		assert.deepEqual(await snapshot("undo"), initial);
		await call("resource.save", { path: "res://main.tscn" });
		const persisted = await fs.readFile(path.join(project, "main.tscn"), "utf8");
		assert.match(persisted, /name="Descendant"/);
		assert.match(persisted, /name="Moved"/);
		await call("resource.reload", { path: "res://main.tscn" });
		assert.deepEqual(await snapshot(), initial);

		for (const scenePath of ["res://new.tscn", "res://new.scn", "res://new.TSCN"]) {
			await assert.rejects(lifecycle.call("resource.create", { type: "PackedScene", path: scenePath }), /INVALID_ARGUMENT.*scene\.create/);
			await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: scenePath }), /INVALID_ARGUMENT.*scene\.create/);
		}
		await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: "res://../outside.tres" }), /INVALID_ARGUMENT/);
		await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: "res://material.tres", properties: { nonexistent: 1 } }), /INVALID_ARGUMENT/);
		await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: "res://material.tres", properties: { resource_path: "res://retargeted.tres" } }), /INVALID_ARGUMENT/);
		await call("resource.create", { type: "StandardMaterial3D", path: "res://material.tres", properties: { roughness: 0.7 } });
		await assert.rejects(lifecycle.call("resource.set_properties", { path: "res://material.tres", properties: { resource_path: "res://retargeted.tres" } }), /INVALID_ARGUMENT/);
		for (const alias of ["res://./material.tres", "res://unused/../material.tres", "res://material.tres/../material.tres"]) {
			await assert.rejects(lifecycle.call("resource.set_properties", { path: alias, properties: { resource_path: "res://retargeted.tres" } }), /INVALID_ARGUMENT/);
			await assert.rejects(lifecycle.call("resource.get", { path: alias, property_names: ["roughness"] }), /INVALID_ARGUMENT/);
		}
		await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: "res://material.tres" }), /CONFLICT/);
		assert.ok(Math.abs((await call("resource.get", { path: "res://material.tres", property_names: ["roughness"] })).properties.roughness - 0.7) < 0.001);
		await call("resource.set_properties", { path: "res://material.tres", properties: { roughness: 0.4 } });
		await call("resource.save", { path: "res://material.tres" });
		assert.ok(Math.abs((await call("resource.get", { path: "res://material.tres", property_names: ["roughness"] })).properties.roughness - 0.4) < 0.001);
		assert.match(await fs.readFile(path.join(project, "material.tres"), "utf8"), /roughness = 0.4/);
		const generated = await call("resource.create", { type: "StandardMaterial3D" });
		assert.match(generated.path, /^res:\/\/mcp_resource_.*\.tres$/);
		await assert.rejects(fs.access(path.join(project, generated.path.slice(6))), /ENOENT/);
		await call("resource.save", { path: generated.path });
		await fs.access(path.join(project, generated.path.slice(6)));
		await call("resource.create", { type: "StandardMaterial3D", path: "res://external.tres" });
		await fs.writeFile(path.join(project, "external.tres"), "external writer content");
		await assert.rejects(lifecycle.call("resource.save", { path: "res://external.tres" }), /CONFLICT/);
		for (const alias of ["res://./external.tres", "res://unused/../external.tres"]) {
			await assert.rejects(lifecycle.call("resource.save", { path: alias }), /INVALID_ARGUMENT/);
		}
		assert.equal(await fs.readFile(path.join(project, "external.tres"), "utf8"), "external writer content");
		// Once the unrelated file is removed, the pending resource can be saved.
		await fs.unlink(path.join(project, "external.tres"));
		await call("resource.save", { path: "res://external.tres" });
		for (let i = 0; i < 128; i++) await call("resource.create", { type: "StandardMaterial3D", path: `res://pending_${i}.tres` });
		await assert.rejects(lifecycle.call("resource.create", { type: "StandardMaterial3D", path: "res://overflow.tres" }), /CONFLICT/);
		await call("resource.save", { path: "res://pending_0.tres" });
		await call("resource.create", { type: "StandardMaterial3D", path: "res://overflow.tres" });

		const unicode = await rawCall(session, "node.create", { parent_path: ".", type: "Node", name: "猫🙂" }, true);
		assert.equal(unicode.result.ok, true, JSON.stringify(unicode));
		assert.equal(unicode.result.data.name, "猫🙂");

		const query = { script_path: "res://lsp.gd", line: 8, character: 13 };
		assert.equal((await call("lsp.query", { ...query, operation: "goToDefinition" })).locations[0].line, 1);
		assert.equal((await call("lsp.query", { ...query, operation: "hover" })).identifier, "target");
		assert.ok((await call("lsp.query", { script_path: "res://lsp.gd", operation: "documentSymbol" })).symbols.length > 0);
		const references = await call("lsp.query", { ...query, operation: "findReferences", include_declaration: true });
		assert.equal(references.approximate, true);
		assert.equal(references.truncated, false);
		assert.deepEqual(references.locations.map((item: Record<string, any>) => item.line), [1, 8]);
		const usages = await call("lsp.query", { ...query, operation: "findReferences", include_declaration: false });
		assert.deepEqual(usages.locations.map((item: Record<string, any>) => item.line), [8]);
		const firstLineUsages = await call("lsp.query", { script_path: "res://first_line.gd", line: 2, character: 13, operation: "findReferences", include_declaration: false });
		assert.deepEqual(firstLineUsages.locations.map((item: Record<string, any>) => item.line), [2]);

		const oversized = net.createConnection({ host: "127.0.0.1", port: session.port });
		const closed = new Promise<void>((resolve) => { oversized.once("close", () => resolve()); oversized.on("error", () => {}); });
		let oversizedTimedOut = false;
		const oversizedTimer = setTimeout(() => {
			oversizedTimedOut = true;
			oversized.destroy(new Error("Oversized request was not rejected"));
		}, 15_000);
		let oversizedResponse = "";
		oversized.on("data", (chunk) => { oversizedResponse += chunk; });
		await new Promise<void>((resolve) => oversized.once("connect", resolve));
		oversized.write(Buffer.from(JSON.stringify({ jsonrpc: "2.0", id: 99, method: "godot.session.info", params: { _session_id: session.session_id, padding: "猫".repeat(360_000) } }) + "\n"));
		await closed;
		clearTimeout(oversizedTimer);
		assert.equal(oversizedTimedOut, false, "Oversized request must be rejected before the timeout");
		assert.equal(oversizedResponse, "", "Oversized UTF-8 request must be bounded by bytes");
	} catch (error) {
		throw new Error(String(error) + "\n" + log, { cause: error });
	}
});
