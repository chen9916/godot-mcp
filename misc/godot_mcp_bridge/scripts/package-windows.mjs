import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const bridge = fileURLToPath(new URL("../", import.meta.url));
const engine = path.resolve(bridge, "../..");
const args = new Map();
let allow_dirty = false;
for (let i = 2; i < process.argv.length; i++) {
	const key = process.argv[i];
	if (key === "--allow-dirty") { assert.equal(allow_dirty, false); allow_dirty = true; continue; }
	assert.ok(["--editor", "--output", "--node", "--node-license", "--npm-cli"].includes(key), `Unknown option: ${key}`);
	assert.ok(process.argv[i + 1] && !args.has(key), `Missing or duplicate option: ${key}`);
	args.set(key, path.resolve(process.argv[++i]));
}
assert.equal(process.platform, "win32", "This packager currently supports Windows only.");
for (const required of ["--editor", "--output", "--node-license"]) assert.ok(args.has(required), `${required} is required.`);
const output = args.get("--output");
const artifacts = path.join(engine, "artifacts");
const relative = path.relative(artifacts, output);
assert.ok(relative && !relative.startsWith("..") && !path.isAbsolute(relative), "Output must be a NEW directory inside this checkout's artifacts directory.");
await assert.rejects(fs.lstat(output), { code: "ENOENT" }, "Refusing to reuse or overwrite an existing output.");
const editor = args.get("--editor");
const node = args.get("--node") ?? process.execPath;
const license = args.get("--node-license");
const npm_cli = args.get("--npm-cli") ?? process.env.npm_execpath ?? path.join(path.dirname(process.execPath), "node_modules/npm/bin/npm-cli.js");
for (const input of [editor, node, license, npm_cli]) assert.ok((await fs.stat(input)).isFile(), `Missing input: ${input}`);
for (const executable of [editor, node]) {
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
await fs.mkdir(path.join(output, "runtime"), { recursive: true });
await fs.mkdir(path.join(output, "bridge", "dist"), { recursive: true });
await fs.copyFile(editor, path.join(output, "Godot.exe"));
await fs.copyFile(node, path.join(output, "runtime", "node.exe"));
await fs.copyFile(license, path.join(output, "runtime", "LICENSE"));
await fs.writeFile(path.join(output, "_sc_"), "");
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
await fs.writeFile(path.join(output, "README.txt"), "Godot MCP for Windows\n\nLaunch Godot.exe and open a project. In Editor > Editor Settings > Network > Godot MCP, choose Copy MCP Configuration. Paste the connection into your MCP client's settings. This bundle includes the bridge and Node runtime. Export configuration again after moving the bundle. The _sc_ file keeps editor settings inside editor_data beside Godot.exe. No client configuration is written automatically.\n\nThis is a custom development build of Godot. RELEASE_MANIFEST.json records source identity and file hashes. Consult the source checkout's MCP_STATUS.md for executed release checks and support boundaries.\n");
const sha256 = async (filename) => createHash("sha256").update(await fs.readFile(filename)).digest("hex");
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
for (const directory of ["editor/debugger/godot_mcp", "tests/editor/godot_mcp", "misc/godot_mcp_bridge/src", "misc/godot_mcp_bridge/scripts", "misc/godot_mcp_bridge/test"]) {
	for (const [name, digest] of Object.entries(await hashes(path.join(engine, directory)))) source_hashes[`${directory}/${name}`] = digest;
}
for (const name of [
	"misc/godot_mcp_bridge/package.json", "misc/godot_mcp_bridge/package-lock.json", "misc/godot_mcp_bridge/tsconfig.json", "misc/godot_mcp_bridge/tsconfig.test.json",
	"editor/editor_data.h", "editor/editor_data.cpp", "editor/editor_node.h", "editor/editor_node.cpp",
	"editor/editor_undo_redo_manager.h", "editor/editor_undo_redo_manager.cpp",
	"editor/settings/editor_settings_dialog.h", "editor/settings/editor_settings_dialog.cpp", "editor/settings/editor_settings.h", "editor/settings/editor_settings.cpp",
	"editor/inspector/editor_property_name_processor.h", "editor/inspector/editor_property_name_processor.cpp",
	"editor/debugger/SCsub", "editor/register_editor_types.cpp", "scene/register_scene_types.cpp", "scene/debugger/SCsub",
	"scene/debugger/godot_mcp_runtime_debugger.h", "scene/debugger/godot_mcp_runtime_debugger.cpp",
]) source_hashes[name] = await sha256(path.join(engine, name));
assert.equal(run("git", ["rev-parse", "HEAD"], engine).trim(), git_revision, "Source commit changed during packaging.");
if (!allow_dirty) assert.equal(run("git", ["status", "--porcelain", "--untracked-files=normal"], engine).trim(), "", "Source changed during packaging.");
const manifest = { format_version: 1, platform: "windows", architecture: "x86_64", editor_version, node_version, git_revision, git_dirty, source_sha256: source_hashes, files_sha256: await hashes(output) };
await fs.writeFile(path.join(output, "RELEASE_MANIFEST.json"), JSON.stringify(manifest, null, 2) + "\n");
process.stdout.write(`Created ${output}\nRun: node scripts/verify-package.mjs --package "${output}"\n`);
