import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const run = promisify(execFile);

// This plugin drives the real settings menu, category tree, and setup button.
const plugin = `@tool
extends EditorPlugin

func _enter_tree():
	_run.call_deferred()

func _fail(message):
	push_error(message)
	get_tree().quit(1)

func _write(path, content):
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(content)
	file.close()

func _select_section(item):
	if item == null:
		return false
	if item.get_metadata(0) == "network/godot_mcp":
		if item.get_text(0) != "Godot MCP" or item.get_tooltip_text(0) != "Godot MCP":
			_fail("MCP settings category still uses legacy branding: " + item.get_text(0))
			return false
		item.select(0)
		return true
	for child in item.get_children():
		if _select_section(child):
			return true
	return false

func _select_mcp_category(dialog):
	for tree in dialog.find_children("*", "Tree", true, false):
		if _select_section(tree.get_root()):
			return true
	return false

func _exported_config(dialog):
	for popup in dialog.find_children("*", "AcceptDialog", true, false):
		if popup.title == "Godot MCP Ready":
			var parts = popup.dialog_text.split("\\n\\n", false, 1)
			if parts.size() == 2:
				return parts[1]
	return ""

func _wait_for_setup(button):
	var deadline = Time.get_ticks_msec() + 35000
	while button.disabled and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.05).timeout
	return not button.disabled

func _run():
	await get_tree().create_timer(1.0).timeout
	var settings = EditorInterface.get_editor_settings()
	settings.set_setting("network/godot_mcp/enabled", false)
	settings.set_setting("network/godot_mcp/bridge_script", "")
	settings.set_setting("network/godot_mcp/node_executable", "")
	var root = get_tree().root
	root.size = Vector2i(1600, 1000)
	root.gui_embed_subwindows = true
	settings.set_project_metadata("dialog_bounds", "editor_settings", Rect2())
	for menu in root.find_children("*", "PopupMenu", true, false):
		for index in menu.item_count:
			if menu.get_item_text(index) == "Editor Settings...":
				menu.emit_signal("id_pressed", menu.get_item_id(index))
	var dialogs = root.find_children("*", "EditorSettingsDialog", true, false)
	if dialogs.is_empty() or not dialogs[0].visible:
		_fail("Editor Settings menu did not open the dialog")
		return
	var dialog = dialogs[0]
	var settings_search
	for edit in dialog.find_children("*", "LineEdit", true, false):
		if edit.placeholder_text == "Filter Settings":
			settings_search = edit
	if settings_search == null:
		_fail("Settings filter is missing")
		return
	for localized in [false, true]:
		settings.set_setting("interface/editor/localization/localize_settings", localized)
		for query in ["", "Godot MCP", "MCP"]:
			settings_search.text = query
			settings_search.emit_signal("text_changed", query)
			await get_tree().process_frame
			if not _select_mcp_category(dialog):
				_fail("Godot MCP settings category is missing for filter: " + query)
				return
			await get_tree().process_frame
			var enabled_visible = false
			for property in dialog.find_children("*", "EditorProperty", true, false):
				if property.get_edited_property() == "enabled" and property.is_visible_in_tree():
					enabled_visible = true
			if not enabled_visible:
				_fail("Godot MCP filter hid the Enabled setting")
				return
	settings_search.text = ""
	settings_search.emit_signal("text_changed", "")
	var selected = _select_mcp_category(dialog)
	var button = dialog.find_child("CopyMCPConfiguration", true, false)
	var status = dialog.find_child("GodotMCPStatus", true, false)
	if not selected or button == null or not button.is_visible_in_tree():
		_fail("MCP setup button is not visible in its settings category")
		return
	button.emit_signal("pressed")
	if not await _wait_for_setup(button) or not status.text.begins_with("MCP configuration copied."):
		_fail("Setup failed: " + status.text)
		return
	if not settings.get_setting("network/godot_mcp/enabled") or not FileAccess.file_exists("res://.godot/godot_mcp/session.json"):
		_fail("Setup did not enable the MCP session")
		return
	var exported = _exported_config(dialog)
	var connection = JSON.parse_string(exported)
	if connection == null or not connection.has("mcpServers") or not connection.mcpServers.has("godot"):
		_fail("Generic MCP export is invalid")
		return
	button.emit_signal("pressed")
	if not await _wait_for_setup(button) or _exported_config(dialog) != exported:
		_fail("Repeated export changed the connection")
		return
	settings.set_setting("network/godot_mcp/enabled", false)
	settings.set_setting("network/godot_mcp/bridge_script", "res://missing-bridge.js")
	button.emit_signal("pressed")
	if button.disabled or not status.text.begins_with("Bridge setup failed.") or settings.get_setting("network/godot_mcp/enabled"):
		_fail("Missing bridge did not leave setup retryable and MCP disabled")
		return
	settings.set_setting("network/godot_mcp/bridge_script", "")
	button.emit_signal("pressed")
	if not await _wait_for_setup(button) or not status.text.begins_with("MCP configuration copied."):
		_fail("Export did not recover after a missing bridge")
		return
	_write(OS.get_environment("GODOT_MCP_SETUP_RESULT"), JSON.stringify({"ok": true, "exported": _exported_config(dialog)}))
	for popup in dialog.find_children("*", "AcceptDialog", true, false):
		popup.hide()
	dialog.get_ok_button().emit_signal("pressed")
	await get_tree().create_timer(0.5).timeout
	var deadline = Time.get_ticks_msec() + 15000
	while not FileAccess.file_exists(OS.get_environment("GODOT_MCP_SETUP_RESULT") + ".done") and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.05).timeout
	get_tree().quit()
`;

test("Godot settings exports a client-neutral connection and retries in portable and source layouts", {
	skip: process.env.GODOT_MCP_SETUP_EDITOR && process.platform === "win32" ? false :
		"Set GODOT_MCP_SETUP_EDITOR to a rebuilt Windows editor executable to run isolated UI setup tests.",
	timeout: 180_000,
}, async (t) => {
	const editor = path.resolve(process.env.GODOT_MCP_SETUP_EDITOR!);
	const source = path.resolve(".");
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot setup UI 婕㈠瓧 "));
	t.after(() => fs.rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }));
	let bundle = path.join(directory, "Portable Godot");
	await fs.mkdir(path.join(bundle, "runtime"), { recursive: true });
	await fs.mkdir(path.join(bundle, "bridge"));
	await fs.copyFile(editor, path.join(bundle, "Godot.exe"));
	await fs.copyFile(process.execPath, path.join(bundle, "runtime", "node.exe"));
	await fs.writeFile(path.join(bundle, "_sc_"), "");
	await fs.cp(path.join(source, "dist"), path.join(bundle, "bridge", "dist"), { recursive: true });
	await fs.cp(path.join(source, "node_modules"), path.join(bundle, "bridge", "node_modules"), { recursive: true });
	await fs.copyFile(path.join(source, "package.json"), path.join(bundle, "bridge", "package.json"));
	const project = path.join(directory, "Test Project");
	await fs.mkdir(path.join(project, "addons", "setup_test"), { recursive: true });
	await fs.writeFile(path.join(project, "project.godot"), '[application]\nconfig/name="Bridge Setup Test"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/setup_test/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n');
	await fs.writeFile(path.join(project, "addons", "setup_test", "plugin.cfg"), '[plugin]\nname="Bridge Setup Test"\ndescription="Isolated setup verification"\nauthor="Godot tests"\nversion="1.0"\nscript="plugin.gd"\n');
	await fs.writeFile(path.join(project, "addons", "setup_test", "plugin.gd"), plugin);
	for (let attempt = 0; attempt < 3; attempt++) {
		const report = path.join(directory, `result-${attempt}.json`);
		const editor_run = run(path.join(bundle, "Godot.exe"), ["--headless", "--editor", "--path", project], {
			timeout: 70_000,
			windowsHide: true,
			maxBuffer: 2 * 1024 * 1024,
			env: { ...process.env, PATH: path.dirname(process.execPath) + path.delimiter + (process.env.PATH ?? ""), GODOT_MCP_SETUP_RESULT: report },
		});
		const wait_for_report = async () => {
			const deadline = Date.now() + 50_000;
			while (Date.now() < deadline) {
				try {
					return JSON.parse(await fs.readFile(report, "utf8"));
				} catch (error: any) {
					if (error.code !== "ENOENT") throw error;
				}
				await new Promise((resolve) => setTimeout(resolve, 100));
			}
			throw new Error("Editor setup report timed out.");
		};
		const result = await Promise.race([wait_for_report(), editor_run.then(({ stdout, stderr }) => {
			throw new Error("Editor exited before reporting setup: " + stdout + stderr);
		})]);
		try {
			const launch = JSON.parse(result.exported).mcpServers.godot;
			for (const command of [[launch.command, ...launch.args]]) {
				const transport = new StdioClientTransport({ command: command[0], args: command.slice(1), cwd: project, stderr: "pipe" });
				const client = new Client({ name: "setup-verification", version: "1" }, { capabilities: {} });
				try {
					await client.connect(transport);
					assert.equal(client.getServerVersion()?.name, "godot-mcp");
					assert.ok((await client.listTools()).tools.length >= 74);
					const info = await client.callTool({ name: "godot.session.info", arguments: {} });
					assert.equal((info.structuredContent as any).data.bridge.state, "connected");
					assert.ok((info.structuredContent as any).data.editor);
				} finally {
					await client.close();
				}
			}
		} finally {
			await fs.writeFile(report + ".done", "");
			const { stdout, stderr } = await editor_run;
			assert.doesNotMatch(stdout + stderr, /SCRIPT ERROR:|ERROR:/);
		}
		assert.equal(result.ok, true);
		const launch = JSON.parse(result.exported).mcpServers.godot;
		assert.deepEqual([launch.command, ...launch.args],
			[attempt < 2 ? path.join(bundle, "runtime", "node.exe") : process.execPath,
			attempt < 2 ? path.join(bundle, "bridge", "dist", "main.js") : path.resolve(bundle, "../misc/godot_mcp_bridge/dist/main.js")]);
		if (attempt === 0) {
			const moved = path.join(directory, "Moved Godot");
			await fs.rename(bundle, moved);
			bundle = moved;
		} else if (attempt === 1) {
			const checkout = path.join(directory, "Source Checkout");
			await fs.mkdir(path.join(checkout, "bin"), { recursive: true });
			await fs.mkdir(path.join(checkout, "misc"));
			await fs.rename(path.join(bundle, "bridge"), path.join(checkout, "misc", "godot_mcp_bridge"));
			await fs.rename(path.join(bundle, "Godot.exe"), path.join(checkout, "bin", "Godot.exe"));
			bundle = path.join(checkout, "bin");
			await fs.writeFile(path.join(bundle, "_sc_"), "");
		}
	}
});
