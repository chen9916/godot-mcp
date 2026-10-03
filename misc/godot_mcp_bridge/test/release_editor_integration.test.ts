import assert from "node:assert/strict";
import { execFile, type ChildProcess } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";

import { SessionLifecycle } from "../src/session_lifecycle.js";

const run = promisify(execFile);
const delay = (milliseconds: number) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const bufferSource = "extends Node\nvar value: int = 1\n";
const sceneSource = '[gd_scene format=3]\n[node name="Root" type="Node3D"]\nmetadata/unsaved = 0\nmetadata/patch = 0\n';
const callbackScript = `@tool
extends Node3D

func _notification(what):
	if what == NOTIFICATION_EDITOR_PRE_SAVE:
		var content = FileAccess.get_file_as_string(scene_file_path)
		var file = FileAccess.open(scene_file_path, FileAccess.WRITE)
		file.store_string(content + '\\n[node name="ExternalDuringSave" type="Node3D" parent="."]\\n')
		file.close()
`;

function data(result: unknown): Record<string, any> {
	const envelope = result as Record<string, any>;
	assert.equal(envelope.ok, true, JSON.stringify(envelope));
	return envelope.data;
}

const plugin = `@tool
extends EditorPlugin

var code_editor
var probe_ready := false
var control_dir := OS.get_environment("GODOT_MCP_RELEASE_CONTROL")

func _enter_tree():
	_setup.call_deferred()

func _write(name, value):
	var file = FileAccess.open(control_dir.path_join(name), FileAccess.WRITE)
	file.store_string(JSON.stringify(value))
	file.close()

func _setup():
	await get_tree().create_timer(1.0).timeout
	var settings = EditorInterface.get_editor_settings()
	settings.set_setting("network/godot_mcp/enabled", true)
	settings.set_setting("text_editor/behavior/files/auto_reload_scripts_on_external_change", false)
	EditorInterface.open_scene_from_path("res://main.tscn")
	await get_tree().create_timer(0.5).timeout
	if OS.get_environment("GODOT_MCP_RELEASE_BUFFERS") == "1":
		EditorInterface.edit_script(load("res://buffer.gd"))
		await get_tree().create_timer(0.5).timeout
		code_editor = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
		code_editor.set_caret_line(code_editor.get_line_count() - 1)
		code_editor.insert_text_at_caret("# unsaved editor buffer\\n")
		EditorInterface.get_edited_scene_root().set_meta("unsaved", 42)
		EditorInterface.mark_scene_as_unsaved()
	probe_ready = true
	_write("ready.json", {"ready": true})

func _process(_delta):
	if not probe_ready:
		return
	var command_path = control_dir.path_join("command.json")
	if not FileAccess.file_exists(command_path):
		return
	var command = JSON.parse_string(FileAccess.get_file_as_string(command_path))
	DirAccess.remove_absolute(command_path)
	if command.action == "snapshot":
		_write("snapshot.json", {"buffer": code_editor.text, "unsaved": EditorInterface.get_edited_scene_root().get_meta("unsaved")})
	elif command.action == "quit":
		get_tree().quit()
`;

async function waitFile(file: string, editorFinished: Promise<unknown>): Promise<Record<string, any>> {
	const poll = async () => {
		const deadline = Date.now() + 45_000;
		while (Date.now() < deadline) {
			try { return JSON.parse(await fs.readFile(file, "utf8")); }
			catch (error: any) { if (error.code !== "ENOENT" && !(error instanceof SyntaxError)) throw error; }
			await delay(100);
		}
		throw new Error(`Editor report timed out: ${file}`);
	};
	return Promise.race([poll(), editorFinished.then((result) => {
		throw new Error(`Editor exited before reporting: ${JSON.stringify(result)}`);
	})]);
}

test("isolated release editor preserves unsaved buffers and runs complete live workflows", {
	skip: process.platform === "win32" && (process.env.GODOT_MCP_RELEASE_EDITOR || process.env.GODOT_MCP_RELEASE_BUNDLE) ? false :
		"Set GODOT_MCP_RELEASE_EDITOR or GODOT_MCP_RELEASE_BUNDLE for isolated Windows release verification.",
	timeout: 360_000,
}, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot-mcp-release-"));
	let activeChild: ChildProcess | undefined;
	let activeFinished: Promise<unknown> | undefined;
	t.after(async () => {
		if (activeChild && activeChild.exitCode === null) activeChild.kill();
		await activeFinished?.catch(() => {});
		await fs.rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
	});
	const bundle = path.join(directory, "Portable Editor");
	if (process.env.GODOT_MCP_RELEASE_BUNDLE) {
		await fs.cp(path.resolve(process.env.GODOT_MCP_RELEASE_BUNDLE), bundle, { recursive: true });
	} else {
		await fs.mkdir(bundle);
		await fs.copyFile(path.resolve(process.env.GODOT_MCP_RELEASE_EDITOR!), path.join(bundle, "Godot.exe"));
	}
	await fs.writeFile(path.join(bundle, "_sc_"), "");
	for (const phase of ["buffers", "callbacks", "scene_patch", "runtime"]) {
		const buffers = phase === "buffers";
		const project = path.join(directory, `${phase} Project`);
		const control = path.join(project, "control");
		await fs.mkdir(control, { recursive: true });
		await fs.mkdir(path.join(project, "addons", "release_probe"), { recursive: true });
		await fs.writeFile(path.join(project, "project.godot"), '[application]\nconfig/name="MCP Release Verification"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/release_probe/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n');
		await fs.writeFile(path.join(project, "main.tscn"), sceneSource);
		if (phase === "callbacks") {
			await fs.writeFile(path.join(project, "callback.gd"), callbackScript);
			await fs.writeFile(path.join(project, "main.tscn"), '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://callback.gd" id="1"]\n[node name="Root" type="Node3D"]\nscript = ExtResource("1")\nmetadata/patch = 0\n');
		}
		await fs.writeFile(path.join(project, "buffer.gd"), bufferSource);
		await fs.writeFile(path.join(project, "addons", "release_probe", "plugin.cfg"), '[plugin]\nname="Release Probe"\ndescription="Disposable live verification"\nauthor="Godot tests"\nversion="1.0"\nscript="plugin.gd"\n');
		await fs.writeFile(path.join(project, "addons", "release_probe", "plugin.gd"), plugin);
		const editorFinished = run(path.join(bundle, "Godot.exe"), ["--headless", "--editor", "--path", project], {
			windowsHide: true, timeout: 250_000, maxBuffer: 4 * 1024 * 1024,
			env: { ...process.env, GODOT_MCP_RELEASE_CONTROL: control, GODOT_MCP_RELEASE_BUFFERS: buffers ? "1" : "0" },
		});
		// Attach a handler immediately so a launch failure never becomes unhandled.
		editorFinished.catch(() => {});
		activeChild = editorFinished.child;
		activeFinished = editorFinished;
		let editorLog = "";
		editorFinished.child.stdout?.on("data", (chunk) => { editorLog += chunk; });
		editorFinished.child.stderr?.on("data", (chunk) => { editorLog += chunk; });
		const lifecycle = new SessionLifecycle({ project_root: project, timeout_ms: 10_000 });
		try {
			await waitFile(path.join(control, "ready.json"), editorFinished);
			await lifecycle.refresh();
			if (buffers) {
				const script = path.join(project, "buffer.gd");
				const scene = path.join(project, "main.tscn");
				const scriptTime = await fs.stat(script);
				const sceneTime = await fs.stat(scene);
				const externalScript = bufferSource + "# external disk edit\n";
				const externalScene = sceneSource + '\n[node name="External" type="Node3D" parent="."]\n';
				await fs.writeFile(script, externalScript);
				await fs.utimes(script, scriptTime.atime, scriptTime.mtime);
				await fs.writeFile(scene, externalScene);
				await fs.utimes(scene, sceneTime.atime, sceneTime.mtime);
				const status = data(await lifecycle.call("resource.status", { path: "res://buffer.gd" }));
				assert.equal(status.editor_buffer_open, true);
				assert.equal(status.editor_buffer_unsaved, true);
				assert.equal(status.stale_state, "editor_buffer_conflict");
				await assert.rejects(lifecycle.call("resource.reload", { path: "res://buffer.gd" }), /CONFLICT/);
				await assert.rejects(lifecycle.call("resource.reload", { path: "res://main.tscn" }), /CONFLICT/);
				await assert.rejects(lifecycle.call("resource.save", { path: "res://main.tscn" }), /CONFLICT/);
				const active = data(await lifecycle.call("scene.get_active", {}));
				await assert.rejects(lifecycle.call("scene.apply_patch", {
					expected_scene_path: active.path, expected_scene_version: active.scene_version, save: true,
					operations: [{ op: "update_node", node: ".", properties: { "metadata/patch": 1 } }],
				}), /CONFLICT/);
				assert.equal(await fs.readFile(script, "utf8"), externalScript);
				assert.equal(await fs.readFile(scene, "utf8"), externalScene);
				await fs.writeFile(path.join(control, "command.json"), JSON.stringify({ action: "snapshot" }));
				const snapshot = await waitFile(path.join(control, "snapshot.json"), editorFinished);
				assert.match(snapshot.buffer, /# unsaved editor buffer/);
				assert.doesNotMatch(snapshot.buffer, /# external disk edit/);
				assert.equal(snapshot.unsaved, 42);
				await fs.unlink(scene);
				await assert.rejects(lifecycle.call("resource.save", { path: "res://main.tscn" }), /CONFLICT/);
				await fs.writeFile(scene, externalScene);
				await fs.utimes(scene, sceneTime.atime, sceneTime.mtime);
				await assert.rejects(lifecycle.call("resource.save", { path: "res://main.tscn" }), /CONFLICT/);
				assert.equal(await fs.readFile(scene, "utf8"), externalScene);
			} else if (phase === "callbacks") {
				const active = data(await lifecycle.call("scene.get_active", {}));
				const result = data(await lifecycle.call("scene.apply_patch", {
					expected_scene_path: active.path, expected_scene_version: active.scene_version, save: true,
					operations: [{ op: "update_node", node: ".", properties: { "metadata/patch": 1 } }],
				}));
				assert.equal(result.applied, true);
				assert.equal(result.save_status, "failed_external_disk_conflict");
				const preserved = await fs.readFile(path.join(project, "main.tscn"), "utf8");
				assert.match(preserved, /ExternalDuringSave/);
				assert.match(preserved, /metadata\/patch = 0/);
			} else {
				const workflows = phase === "scene_patch" ?
					[["scene_patch_integration.test.js", "GODOT_MCP_SCENE_PATCH_INTEGRATION"]] :
					[["editor_integration.test.js", "GODOT_MCP_INTEGRATION"]];
				for (const [file, flag] of workflows) {
					const childEnvironment = { ...process.env, [flag]: "1", GODOT_MCP_INTEGRATION_PROJECT: project };
					delete childEnvironment.NODE_TEST_CONTEXT;
					const result = await run(process.execPath, ["--test", "--test-reporter=tap", path.join(".test-dist", "test", file)], {
						windowsHide: true, timeout: 150_000, maxBuffer: 2 * 1024 * 1024,
						env: childEnvironment,
					});
					assert.match(result.stdout, /# pass 1/);
					assert.match(result.stdout, /# skipped 0/);
				}
			}
		} catch (error) {
			const childOutput = error && typeof error === "object" ? (error as any).stdout ?? "" : "";
			throw new Error(`${String(error)}\n${childOutput}\n${editorLog}`, { cause: error });
		} finally {
			await lifecycle.close();
			await fs.writeFile(path.join(control, "command.json"), JSON.stringify({ action: "quit" }));
			await Promise.race([editorFinished.catch(() => {}), delay(5_000)]);
			if (editorFinished.child.exitCode === null) editorFinished.child.kill();
			await editorFinished.catch(() => {});
			activeChild = undefined;
			activeFinished = undefined;
		}
	}
});
