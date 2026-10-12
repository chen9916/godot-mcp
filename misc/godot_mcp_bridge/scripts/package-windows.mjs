import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const bridge = fileURLToPath(new URL("../", import.meta.url));
const engine = path.resolve(bridge, "../..");
const args = new Map();
const template_options = ["--template-debug", "--template-release", "--template-debug-console", "--template-release-console"];
const d3d12_options = ["--template-d3d12-core", "--template-d3d12-layers", "--template-d3d12-license"];
let allow_dirty = false;
for (let i = 2; i < process.argv.length; i++) {
	const key = process.argv[i];
	if (key === "--allow-dirty") { assert.equal(allow_dirty, false); allow_dirty = true; continue; }
	assert.ok(["--editor", "--output", "--node", "--node-license", "--npm-cli", ...template_options, ...d3d12_options].includes(key), `Unknown option: ${key}`);
	assert.ok(process.argv[i + 1] && !process.argv[i + 1].startsWith("--") && !args.has(key), `Missing or duplicate option: ${key}`);
	args.set(key, path.resolve(process.argv[++i]));
}
assert.equal(process.platform, "win32", "This packager currently supports Windows only.");
for (const required of ["--editor", "--output", "--node-license"]) assert.ok(args.has(required), `${required} is required.`);
const include_templates = template_options.some((key) => args.has(key));
if (include_templates) assert.ok(template_options.every((key) => args.has(key)), "Windows templates require all four inputs: --template-debug, --template-release, --template-debug-console, and --template-release-console.");
const include_d3d12 = d3d12_options.some((key) => args.has(key));
if (include_d3d12) assert.ok(include_templates && d3d12_options.every((key) => args.has(key)), "Direct3D 12 runtime requires the four templates and all three inputs: --template-d3d12-core, --template-d3d12-layers, and --template-d3d12-license.");
const output = args.get("--output");
const artifacts = path.join(engine, "artifacts");
const relative = path.relative(artifacts, output);
assert.ok(relative && !relative.startsWith("..") && !path.isAbsolute(relative), "Output must be a NEW directory inside this checkout's artifacts directory.");
await assert.rejects(fs.lstat(output), { code: "ENOENT" }, "Refusing to reuse or overwrite an existing output.");
const editor = args.get("--editor");
const node = args.get("--node") ?? process.execPath;
const license = args.get("--node-license");
const npm_cli = args.get("--npm-cli") ?? process.env.npm_execpath ?? path.join(path.dirname(process.execPath), "node_modules/npm/bin/npm-cli.js");
const template_inputs = include_templates ? template_options.map((key) => args.get(key)) : [];
const d3d12_inputs = include_d3d12 ? d3d12_options.map((key) => args.get(key)) : [];
for (const input of [editor, node, license, npm_cli, ...template_inputs, ...d3d12_inputs]) assert.ok((await fs.stat(input)).isFile(), `Missing input: ${input}`);
for (const executable of [editor, node, ...template_inputs, ...d3d12_inputs.slice(0, 2)]) {
	const binary = await fs.open(executable, "r");
	try {
		const header = Buffer.alloc(64);
		await binary.read(header, 0, header.length, 0);
		assert.equal(header.toString("ascii", 0, 2), "MZ", `Not a Windows executable: ${executable}`);
		const pe = Buffer.alloc(6);
		await binary.read(pe, 0, pe.length, header.readUInt32LE(0x3c));
		assert.equal(pe.toString("ascii", 0, 4), "PE\u0000\u0000", `Invalid executable header: ${executable}`);
		assert.equal(pe.readUInt16LE(4), 0x8664, `Expected a Windows x86_64 executable: ${executable}`);
	} finally { await binary.close(); }
}
const license_text = await fs.readFile(license, "utf8");
assert.ok(license_text.includes("Node.js") && license_text.includes("Copyright"), "Provide the complete LICENSE from the selected Node distribution.");
if (include_d3d12) assert.ok((await fs.readFile(d3d12_inputs[2], "utf8")).trim().length > 0, "Provide the Direct3D 12 Agility SDK license notice.");
const run = (command, argv, cwd = bridge) => execFileSync(command, argv, { cwd, windowsHide: true, encoding: "utf8", maxBuffer: 8 * 1024 * 1024 });
const git_revision = run("git", ["rev-parse", "HEAD"], engine).trim();
const git_dirty = !!run("git", ["status", "--porcelain", "--untracked-files=normal"], engine).trim();
assert.ok(!git_dirty || allow_dirty, "Commit the release source first. Use --allow-dirty only for a tooling probe, which is recorded as dirty in the manifest.");
const npm = (argv, cwd) => process.stdout.write(run(process.execPath, [npm_cli, ...argv], cwd));
npm(["run", "typecheck"], bridge);
npm(["run", "build"], bridge);
const editor_version = run(editor, ["--version"]).trim();
const node_version = run(node, ["--version"]).trim();
assert.ok(Number(node_version.replace(/^v/, "").split(".")[0]) >= 18, "Node.js 18 or later is required.");
let export_templates;
if (include_templates) {
	for (const key of ["--template-debug", "--template-release"]) assert.equal(run(args.get(key), ["--version"]).trim(), editor_version, `Template version must exactly match the editor: ${key}`);
	// find_export_template() uses GODOT_VERSION_FULL_CONFIG (core/version.h),
	// while --version appends GODOT_VERSION_BUILD and an optional 9-digit hash.
	const generated_version = await fs.readFile(path.join(engine, "core/version_generated.gen.h"), "utf8");
	const define = (name) => {
		const match = generated_version.match(new RegExp(`^#define GODOT_VERSION_${name} ("[^"\\r\\n]*")\\r?$`, "m"));
		assert.ok(match, `Missing generated version field: ${name}`);
		return JSON.parse(match[1]);
	};
	const without_hash = editor_version.replace(/\.[0-9a-f]{9}$/, "");
	const build_suffix = `.${define("BUILD")}`;
	assert.ok(without_hash.endsWith(build_suffix), "Editor version must match the current generated build identity.");
	const version = without_hash.slice(0, -build_suffix.length);
	const source_version = await fs.readFile(path.join(engine, "version.py"), "utf8");
	const number = (name) => Number(source_version.match(new RegExp(`^${name} = (\\d+)\\r?$`, "m"))?.[1]);
	const status = source_version.match(/^status = "([^"]+)"\r?$/m)?.[1];
	assert.ok(["major", "minor", "patch"].every((name) => Number.isInteger(number(name))) && status, "Cannot read source version identity.");
	const base = `${number("major")}.${number("minor")}${number("patch") ? `.${number("patch")}` : ""}.${status}${define("MODULE_CONFIG")}`;
	assert.ok(version === base || version === `${base}.double`, `Editor configuration ${version} does not match source ${base}.`);
	assert.match(version, /^[A-Za-z0-9][A-Za-z0-9_.-]*$/, "Unsafe template version directory.");
	const directory = `editor_data/export_templates/${version}`;
	export_templates = { version, directory, architecture: "x86_64", editor_version, build: define("BUILD"), files: {
		debug: `${directory}/windows_debug_x86_64.exe`,
		release: `${directory}/windows_release_x86_64.exe`,
		debug_console: `${directory}/windows_debug_x86_64_console.exe`,
		release_console: `${directory}/windows_release_x86_64_console.exe`,
	} };
	if (include_d3d12) export_templates.runtime_files = {
		d3d12_core: `${directory}/D3D12Core.x86_64.dll`,
		d3d12_layers: `${directory}/d3d12SDKLayers.x86_64.dll`,
		license: `${directory}/D3D12-LICENSE.txt`,
	};
}
await fs.mkdir(path.join(output, "runtime"), { recursive: true });
await fs.mkdir(path.join(output, "bridge", "dist"), { recursive: true });
await fs.copyFile(editor, path.join(output, "Godot.exe"));
await fs.copyFile(node, path.join(output, "runtime", "node.exe"));
await fs.writeFile(path.join(output, "godot-mcp.cmd"), '@echo off\r\n"%~dp0runtime\\node.exe" "%~dp0bridge\\dist\\main.js" %*\r\nexit /b %errorlevel%\r\n');
for (const name of ["Setup.cmd", "Setup-OpenCode.cmd", "Check-Connection.cmd"]) {
	const launcher = await fs.readFile(path.join(bridge, "windows", name), "utf8");
	await fs.writeFile(path.join(output, name), launcher.replace(/\r?\n/g, "\r\n"));
}
await fs.copyFile(path.join(engine, "MCP_QUICKSTART.md"), path.join(output, "QUICKSTART.md"));
await fs.copyFile(license, path.join(output, "runtime", "LICENSE"));
await fs.writeFile(path.join(output, "_sc_"), "");
if (export_templates) {
	await fs.mkdir(path.join(output, export_templates.directory), { recursive: true });
	for (const [index, filename] of Object.values(export_templates.files).entries()) await fs.copyFile(template_inputs[index], path.join(output, filename));
	if (export_templates.runtime_files) for (const [index, filename] of Object.values(export_templates.runtime_files).entries()) await fs.copyFile(d3d12_inputs[index], path.join(output, filename));
}
for (const name of ["LICENSE.txt", "COPYRIGHT.txt"]) await fs.copyFile(path.join(engine, name), path.join(output, name));
for (const name of ["package.json", "package-lock.json"]) await fs.copyFile(path.join(bridge, name), path.join(output, "bridge", name));
// Include compiled files belonging to current sources, excluding stale dist outputs.
for (const name of await fs.readdir(path.join(bridge, "src"))) {
	if (!name.endsWith(".ts")) continue;
	const compiled = name.replace(/\.ts$/, ".js");
	await fs.copyFile(path.join(bridge, "dist", compiled), path.join(output, "bridge", "dist", compiled));
}
npm(["ci", "--omit=dev", "--ignore-scripts", "--no-audit", "--no-fund", "--prefer-offline"], path.join(output, "bridge"));
await fs.writeFile(path.join(output, "THIRD_PARTY_NOTICES.txt"), "Godot Engine copyright and bundled third-party license texts: COPYRIGHT.txt\nNode.js and its bundled dependencies: runtime/LICENSE\nGodot MCP bridge production dependencies: bridge/node_modules/<package>/LICENSE*, COPYING*, or package metadata.\nProduction dependencies are installed from the included package-lock.json; their original notices remain included.\n");
await fs.copyFile(path.join(bridge, "README.md"), path.join(output, "bridge", "README.md"));
await fs.writeFile(path.join(output, "README.txt"), "Godot MCP for Windows\n\nLaunch Godot.exe and open a game project. Enable Network > Godot MCP in Editor Settings. From a terminal in this folder, run godot-mcp.cmd setup and select the client(s), or run godot-mcp.cmd setup --client antigravity --project-root \"<game-project>\". Supported: Codex, OpenCode, Hermes, Antigravity, Claude Code, Claude Desktop. Run godot-mcp.cmd clients to see config paths; use --config for an alternate path, --dry-run to preview, or config --client <id> to export without writing. Setup backs up existing config, preserves unrelated settings, and checks the MCP bridge separately from its editor connection. Refresh/restart the client after registration. Exit 2 means the bridge works but the editor is unavailable; open the selected project and run godot-mcp.cmd doctor --client <id>. See bridge/README.md for scope, overrides, removal, and diagnostics.\n\nThis bundle includes the bridge and Node runtime. Rerun setup --replace after moving the bundle to update registered absolute paths. The _sc_ file keeps editor settings inside editor_data beside Godot.exe. Copy MCP Configuration remains available at the bottom of Editor Settings for manual configuration.\n\nThis is a custom development build of Godot. RELEASE_MANIFEST.json records source identity and file hashes. Consult the source checkout's MCP_STATUS.md for executed release checks and support boundaries.\n");
await fs.appendFile(path.join(output, "README.txt"), "\nEASY SETUP: Double-click Setup-OpenCode.cmd for OpenCode, or Setup.cmd to choose an agent. Check-Connection.cmd diagnoses the bridge and editor. Read QUICKSTART.md for the five-step guide and upgrade instructions. Node.js is bundled: no npm install, compiler, or administrator access is needed. New OpenCode configurations use V2; existing V1/V2 layouts are preserved.\n");
const sha256 = async (filename) => createHash("sha256").update(await fs.readFile(filename)).digest("hex");
if (export_templates) await fs.appendFile(path.join(output, "README.txt"), `\nWindows x86_64 debug and release export templates, including console companions, are included for this exact editor build. They are installed in ${export_templates.directory}; normal Windows export presets discover them without custom template paths. Templates for other platforms are not included.\n`);
if (include_d3d12) {
	await fs.appendFile(path.join(output, "README.txt"), "Direct3D 12 Agility SDK runtime and debug layers are included beside the export templates. The Windows exporter copies them when the export preset enables Direct3D 12 dependency export.\n");
	await fs.appendFile(path.join(output, "THIRD_PARTY_NOTICES.txt"), `Direct3D 12 Agility SDK runtime and debug layers: ${export_templates.runtime_files.license}\n`);
}
async function hashes(directory, base = directory) {
	const result = {};
	for (const entry of (await fs.readdir(directory, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
		const filename = path.join(directory, entry.name);
		if (entry.isDirectory()) Object.assign(result, await hashes(filename, base));
		else if (entry.isFile()) result[path.relative(base, filename).split(path.sep).join("/")] = await sha256(filename);
		else throw new Error(`Unexpected filesystem entry: ${filename}`);
	}
	return result;
}
const source_hashes = {};
for (const directory of ["editor/debugger/godot_mcp", "tests/editor/godot_mcp", "misc/godot_mcp_bridge/src", "misc/godot_mcp_bridge/scripts", "misc/godot_mcp_bridge/test", "misc/godot_mcp_bridge/windows"]) {
	for (const [name, digest] of Object.entries(await hashes(path.join(engine, directory)))) source_hashes[`${directory}/${name}`] = digest;
}
for (const name of [
	"misc/godot_mcp_bridge/package.json", "misc/godot_mcp_bridge/package-lock.json", "misc/godot_mcp_bridge/tsconfig.json", "misc/godot_mcp_bridge/tsconfig.test.json",
	"version.py", "core/version.h", "methods.py", "SConstruct", "platform/windows/SCsub", "platform/windows/detect.py",
	"editor/export/editor_export_platform.cpp", "editor/export/editor_export_platform_pc.cpp", "platform/windows/export/export_plugin.cpp", "platform/windows/console_wrapper_windows.cpp",
	"editor/editor_data.h", "editor/editor_data.cpp", "editor/editor_node.h", "editor/editor_node.cpp",
	"editor/editor_undo_redo_manager.h", "editor/editor_undo_redo_manager.cpp",
	"editor/settings/editor_settings_dialog.h", "editor/settings/editor_settings_dialog.cpp", "editor/settings/editor_settings.h", "editor/settings/editor_settings.cpp",
	"editor/inspector/editor_property_name_processor.h", "editor/inspector/editor_property_name_processor.cpp",
	"editor/debugger/SCsub", "editor/register_editor_types.cpp", "scene/register_scene_types.cpp", "scene/debugger/SCsub",
	"scene/debugger/godot_mcp_runtime_debugger.h", "scene/debugger/godot_mcp_runtime_debugger.cpp",
	"scene/animation/animation_blend_tree.cpp", "scene/animation/animation_blend_space_2d.cpp",
	"README.md", "MCP_QUICKSTART.md", "MCP_STATUS.md",
]) source_hashes[name] = await sha256(path.join(engine, name));
assert.equal(run("git", ["rev-parse", "HEAD"], engine).trim(), git_revision, "Source commit changed during packaging.");
if (!allow_dirty) assert.equal(run("git", ["status", "--porcelain", "--untracked-files=normal"], engine).trim(), "", "Source changed during packaging.");
const manifest = { format_version: 1, platform: "windows", architecture: "x86_64", editor_version, node_version, git_revision, git_dirty, source_sha256: source_hashes, files_sha256: await hashes(output) };
if (export_templates) manifest.export_templates = export_templates;
await fs.writeFile(path.join(output, "RELEASE_MANIFEST.json"), JSON.stringify(manifest, null, 2) + "\n");
process.stdout.write(`Created ${output}\nRun: node scripts/verify-package.mjs --package "${output}"\n`);
