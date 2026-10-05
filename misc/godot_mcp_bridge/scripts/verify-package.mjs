import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFile, execFileSync, spawn } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

assert.ok(process.argv.length === 4 || (process.argv.length === 5 && process.argv[4] === "--check-source"), "Usage: node scripts/verify-package.mjs --package <new bundle directory> [--check-source]");
assert.equal(process.argv[2], "--package");
const package_directory = path.resolve(process.argv[3]);
const exec_file_async = promisify(execFile);
const manifest = JSON.parse(await fs.readFile(path.join(package_directory, "RELEASE_MANIFEST.json"), "utf8"));
assert.equal(manifest.format_version, 1);
assert.equal(manifest.platform, "windows");
const sha256 = async (filename) => createHash("sha256").update(await fs.readFile(filename)).digest("hex");
function contained_file(base, name) {
	const filename = path.resolve(base, name);
	const relative = path.relative(base, filename);
	assert.ok(relative && !relative.startsWith("..") && !path.isAbsolute(relative), `Unsafe manifest path: ${name}`);
	return filename;
}
for (const [name, digest] of Object.entries(manifest.files_sha256)) {
	assert.equal(await sha256(contained_file(package_directory, name)), digest, `Package hash mismatch: ${name}`);
}
async function file_names(directory, base = directory) {
	const names = [];
	for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
		const filename = path.join(directory, entry.name);
		if (entry.isDirectory()) names.push(...await file_names(filename, base));
		else { assert.ok(entry.isFile(), `Unexpected package entry: ${filename}`); names.push(path.relative(base, filename).split(path.sep).join("/")); }
	}
	return names;
}
assert.deepEqual((await file_names(package_directory)).sort(), [...Object.keys(manifest.files_sha256), "RELEASE_MANIFEST.json"].sort(), "Package contains files outside its manifest.");
if (process.argv.includes("--check-source")) {
	assert.equal(manifest.git_dirty, false, "Release source must have been committed before packaging.");
	const engine = fileURLToPath(new URL("../../../", import.meta.url));
	const git = (args) => execFileSync("git", args, { cwd: engine, windowsHide: true, encoding: "utf8" }).trim();
	assert.equal(git(["rev-parse", "HEAD"]), manifest.git_revision, "Package does not match the checked-out commit.");
	assert.equal(git(["status", "--porcelain", "--untracked-files=normal"]), "", "Commit the source before final package verification.");
	for (const [name, digest] of Object.entries(manifest.source_sha256)) assert.equal(await sha256(contained_file(engine, name)), digest, `Source hash mismatch: ${name}`);
}
for (const name of ["Godot.exe", "godot-mcp.cmd", "_sc_", "LICENSE.txt", "COPYRIGHT.txt", "runtime/node.exe", "runtime/LICENSE", "bridge/dist/main.js", "bridge/dist/setup.js", "bridge/dist/client_config.js", "bridge/dist/setup_doctor.js", "bridge/dist/setup_storage.js", "bridge/README.md", "bridge/package-lock.json"]) assert.ok(name in manifest.files_sha256, `Missing release component: ${name}`);
await assert.rejects(fs.stat(path.join(package_directory, "bridge/node_modules/typescript")), { code: "ENOENT" });
await assert.rejects(fs.stat(path.join(package_directory, "bridge/.test-dist")), { code: "ENOENT" });
const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot MCP package 婕㈠瓧 "));
let bundle = path.join(directory, "Portable Godot");
const project = path.join(directory, "Disposable Project");
const stop_file = path.join(directory, "stop-editor");
let running;
let active_client;
async function stop_editor() {
	if (!running) return;
	const owned = running;
	await fs.writeFile(stop_file, "");
	let timeout;
	await Promise.race([owned.done, new Promise((resolve) => { timeout = setTimeout(() => { owned.child.kill(); resolve(); }, 10_000); })]);
	clearTimeout(timeout);
	await owned.done;
	running = undefined;
}
async function start_editor(previous_id) {
	await fs.rm(stop_file, { force: true });
	let output = "";
	const child = spawn(path.join(bundle, "Godot.exe"), ["--headless", "--editor", "--path", project], {
		windowsHide: true, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, GODOT_MCP_PACKAGE_STOP: stop_file },
	});
	child.stdout.on("data", (chunk) => { output += chunk; });
	child.stderr.on("data", (chunk) => { output += chunk; });
	const done = new Promise((resolve) => { child.once("exit", resolve); child.once("error", resolve); });
	running = { child, done };
	const deadline = Date.now() + 40_000;
	while (Date.now() < deadline) {
		assert.equal(child.exitCode, null, `Editor exited before publishing its session: ${output}`);
		try {
			const session = JSON.parse(await fs.readFile(path.join(project, ".godot/godot_mcp/session.json"), "utf8"));
			if (session.session_id && session.session_id !== previous_id && session.pid === child.pid) return session;
		} catch (error) { if (error.code !== "ENOENT" && !(error instanceof SyntaxError)) throw error; }
		await new Promise((resolve) => setTimeout(resolve, 100));
	}
	throw new Error(`Session publication timed out: ${output}`);
}
async function connect() {
	const node = path.join(bundle, "runtime/node.exe");
	const bridge = path.join(bundle, "bridge/dist/main.js");
	// Keep draining the owned editor's log pipes while its MCP endpoint is used.
	const cli_options = { encoding: "utf8", windowsHide: true, timeout: 30_000 };
	const exported = JSON.parse((await exec_file_async(node, [bridge, "--print-config", "mcp", "--project-root", project], cli_options)).stdout);
	assert.deepEqual(exported.mcpServers.godot, { command: node, args: [bridge, "--project-root", project] });
	// Exercise the actual CMD launcher, including quoted Unicode/space paths.
	const launcher_export = JSON.parse((await exec_file_async(process.env.ComSpec ?? "cmd.exe", ["/d", "/s", "/c", `""${path.join(bundle, "godot-mcp.cmd")}" config --client antigravity --project-root "${project}""`], { ...cli_options, windowsVerbatimArguments: true })).stdout);
	assert.deepEqual(launcher_export, exported);
	const setup_file = path.join(directory, "disposable-antigravity.json");
	const installed = JSON.parse((await exec_file_async(node, [bridge, "setup", "--client", "antigravity", "--config", setup_file, "--project-root", project, "--replace", "--json"], cli_options)).stdout);
	assert.equal(installed[0].registered, true);
	assert.equal(installed[0].checks.bridge.ok, true);
	assert.equal(installed[0].checks.editor.ok, true);
	assert.deepEqual(JSON.parse(await fs.readFile(setup_file, "utf8")).mcpServers.godot, exported.mcpServers.godot);
	const checked = JSON.parse((await exec_file_async(node, [bridge, "doctor", "--client", "antigravity", "--config", setup_file, "--json"], cli_options)).stdout);
	assert.equal(checked[0].ok, true);
	assert.equal(checked[0].editor.ok, true);
	const transport = new StdioClientTransport({ ...exported.mcpServers.godot, cwd: project, stderr: "pipe" });
	const client = new Client({ name: "independent-package-verification", version: "1" }, { capabilities: {} });
	await client.connect(transport);
	assert.equal(client.getServerVersion()?.name, "godot-mcp");
	const tools = (await client.listTools()).tools;
	assert.ok(tools.length >= 74);
	assert.ok(tools.some((tool) => tool.name === "godot.scene.patch_schema"));
	return client;
}
async function connected_session(client, expected_id) {
	const deadline = Date.now() + 25_000;
	while (Date.now() < deadline) {
		const info = await client.callTool({ name: "godot.session.info", arguments: {} });
		const data = info.structuredContent?.data;
		if (data?.bridge?.state === "connected" && data.bridge.session_id === expected_id) {
			assert.ok(data.editor);
			return;
		}
		await new Promise((resolve) => setTimeout(resolve, 100));
	}
	throw new Error(`Bridge did not connect to editor session ${expected_id}`);
}
async function tool(client, name, arguments_ = {}) {
	const result = await client.callTool({ name, arguments: arguments_ }, undefined, { timeout: 15_000 });
	assert.ok(!result.isError, `${name}: ${JSON.stringify(result)}`);
	const envelope = result.structuredContent;
	assert.equal(envelope?.ok, true, `${name}: ${JSON.stringify(result)}`);
	return envelope.data;
}
async function open_fixture(client) {
	await tool(client, "godot.scene.open", { path: "res://main.tscn" });
	const deadline = Date.now() + 15_000;
	while (Date.now() < deadline) {
		const result = await client.callTool({ name: "godot.scene.get_active", arguments: {} }, undefined, { timeout: 15_000 });
		if (!result.isError && result.structuredContent?.ok === true && result.structuredContent.data?.path === "res://main.tscn") return result.structuredContent.data;
		await new Promise((resolve) => setTimeout(resolve, 100));
	}
	throw new Error("Package fixture scene did not finish opening.");
}
async function assert_saved_value(client) {
	await open_fixture(client);
	const property = await tool(client, "godot.node.get_property", { node_path: ".", property_name: "metadata/value" });
	assert.equal(property.value, 1, "Saved scene property must survive editor restart and package move.");
}
try {
	await fs.cp(package_directory, bundle, { recursive: true });
	await fs.mkdir(path.join(project, "addons/package_probe"), { recursive: true });
	await fs.writeFile(path.join(project, "project.godot"), '[application]\nconfig/name="Package verification"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/package_probe/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n');
	await fs.writeFile(path.join(project, "addons/package_probe/plugin.cfg"), '[plugin]\nname="Package probe"\nauthor="Godot tests"\nversion="1"\nscript="plugin.gd"\n');
	await fs.writeFile(path.join(project, "addons/package_probe/plugin.gd"), '@tool\nextends EditorPlugin\nfunc _enter_tree():\n\tEditorInterface.get_editor_settings().set_setting("network/godot_mcp/enabled", true)\nfunc _process(_delta):\n\tif FileAccess.file_exists(OS.get_environment("GODOT_MCP_PACKAGE_STOP")):\n\t\tget_tree().quit()\n');
	await fs.writeFile(path.join(project, "main.tscn"), '[gd_scene format=3]\n[node name="PackageFixture" type="Node"]\nmetadata/value = 0\n');
	const first = await start_editor();
	active_client = await connect();
	await connected_session(active_client, first.session_id);
	const scene = await open_fixture(active_client);
	const saved = await tool(active_client, "godot.scene.apply_patch", {
		expected_scene_path: "res://main.tscn", expected_scene_version: scene.scene_version, save: true,
		operations: [{ op: "update_node", node: ".", properties: { "metadata/value": 1 } }],
	});
	assert.equal(saved.save_status, "saved");
	assert.equal(saved.success_count, 1);
	assert.match(await fs.readFile(path.join(project, "main.tscn"), "utf8"), /metadata\/value = 1(?:\.0)?(?:\r?\n|$)/);
	await assert_saved_value(active_client);
	await stop_editor();
	const second = await start_editor(first.session_id);
	// Keep the same stdio process alive while its real editor endpoint is replaced.
	await connected_session(active_client, second.session_id);
	await assert_saved_value(active_client);
	await active_client.close();
	active_client = undefined;
	await stop_editor();
	const moved = path.join(directory, "Moved Portable Godot");
	await fs.rename(bundle, moved);
	bundle = moved;
	const third = await start_editor(second.session_id);
	active_client = await connect();
	await connected_session(active_client, third.session_id);
	await assert_saved_value(active_client);
	await active_client.close();
	active_client = undefined;
	await stop_editor();
	process.stdout.write("PASS: package hashes and production layout; CMD launcher, setup registration, and doctor; independent SDK initialization/tools/session; packaged bridge mutation/save; same bridge reconnect and saved scene after real editor restart; moved package setup, connection, and saved scene.\n");
} finally {
	if (active_client) await active_client.close().catch(() => {});
	await stop_editor();
	// Only this mkdtemp directory is removed after its owned processes have exited.
	assert.equal(path.dirname(directory), os.tmpdir());
	await fs.rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
}
