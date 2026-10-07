import assert from "node:assert/strict";
import { createHash, randomUUID } from "node:crypto";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const usage = "Usage: node scripts/verify-windows-export.mjs --package <Windows bundle with export templates>";
const output_limit = 2 * 1024 * 1024;
const owned_processes = new Set();
const interruption = new AbortController();
for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => interruption.abort(new Error(`Interrupted by ${signal}`)));

function contained_file(base, name) {
	assert.equal(typeof name, "string", "Manifest paths must be strings.");
	const filename = path.resolve(base, name);
	const relative = path.relative(base, filename);
	assert.ok(relative && relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative), `Unsafe manifest path: ${name}`);
	return filename;
}

async function sha256(filename) {
	return createHash("sha256").update(await fs.readFile(filename)).digest("hex");
}

async function assert_pe(filename) {
	const file = await fs.open(filename, "r");
	try {
		const header = Buffer.alloc(64);
		assert.equal((await file.read(header, 0, header.length, 0)).bytesRead, header.length, `Truncated executable: ${filename}`);
		assert.equal(header.toString("ascii", 0, 2), "MZ", `Not a Windows executable: ${filename}`);
		const pe = Buffer.alloc(6);
		assert.equal((await file.read(pe, 0, pe.length, header.readUInt32LE(0x3c))).bytesRead, pe.length, `Truncated PE header: ${filename}`);
		assert.equal(pe.toString("ascii", 0, 4), "PE\0\0", `Invalid PE header: ${filename}`);
		assert.equal(pe.readUInt16LE(4), 0x8664, `Expected Windows x86_64 executable: ${filename}`);
	} finally { await file.close(); }
}

async function run(label, command, args, cwd, env, timeout_ms = 60_000) {
	interruption.signal.throwIfAborted();
	process.stdout.write(`${label}\n`);
	let output = "";
	let spawn_error;
	let timed_out = false;
	let child;
	try { child = spawn(command, args, { cwd, env, windowsHide: true, stdio: ["ignore", "pipe", "pipe"] }); }
	catch (error) { throw new Error(`${label}\nCommand: ${command} ${JSON.stringify(args)}\nFailed to start: ${error.message}`, { cause: error }); }
	owned_processes.add(child);
	const append = (chunk) => { output = (output + chunk.toString()).slice(-output_limit); };
	child.stdout.on("data", append);
	child.stderr.on("data", append);
	child.once("error", (error) => { spawn_error = error; });
	const kill = () => { child.kill(); };
	interruption.signal.addEventListener("abort", kill, { once: true });
	const timer = setTimeout(() => { timed_out = true; kill(); }, timeout_ms);
	try {
		const result = await new Promise((resolve) => child.once("close", (code, signal) => resolve({ code, signal })));
		const details = `${label}\nCommand: ${command} ${JSON.stringify(args)}\n${output}`;
		if (spawn_error) throw new Error(`${details}\nFailed to start: ${spawn_error.message}`, { cause: spawn_error });
		if (timed_out) throw new Error(`${details}\nTimed out after ${timeout_ms} ms.`);
		interruption.signal.throwIfAborted();
		assert.equal(result.code, 0, `${details}\nExited with code ${result.code}, signal ${result.signal}.`);
		return output;
	} finally {
		clearTimeout(timer);
		interruption.signal.removeEventListener("abort", kill);
		owned_processes.delete(child);
	}
}

const project_config = `[application]
config/name="Windows Export Verification"
run/main_scene="res://main.tscn"
[rendering]
renderer/rendering_method="gl_compatibility"
renderer/rendering_method.mobile="gl_compatibility"
`;

// The empty custom-template options are deliberate: the portable editor must
// discover its versioned, preinstalled templates without any absolute overrides.
const export_presets = (include_d3d12) => `[preset.0]
name="Windows Desktop"
platform="Windows Desktop"
runnable=true
dedicated_server=false
custom_features=""
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
encryption_include_filters=""
encryption_exclude_filters=""
encrypt_pck=false
encrypt_directory=false
[preset.0.options]
custom_template/debug=""
custom_template/release=""
debug/export_console_wrapper=2
binary_format/architecture="x86_64"
binary_format/embed_pck=false
texture_format/s3tc_bptc=true
texture_format/etc2_astc=false
shader_baker/enabled=false
codesign/enable=false
application/modify_resources=false
application/export_angle=2
application/export_d3d12=${include_d3d12 ? 1 : 2}
application/d3d12_agility_sdk_multiarch=true
`;

const scene = `[gd_scene load_steps=2 format=3]
[ext_resource type="Script" path="res://main.gd" id="1"]
[node name="ExportProbe" type="Node"]
script = ExtResource("1")
`;

const script = `extends Node
func _ready() -> void:
	var marker := OS.get_environment("GODOT_EXPORT_VERIFY_MARKER")
	var payload := {
		"token": OS.get_environment("GODOT_EXPORT_VERIFY_TOKEN"),
		"version": Engine.get_version_info(),
		"debug_build": OS.is_debug_build(),
		"editor": OS.has_feature("editor"),
		"template": OS.has_feature("template"),
		"scene_ready": is_inside_tree() and get_tree().current_scene == self,
		"scene_name": str(name),
		"mcp_capture": EngineDebugger.has_capture("godot_mcp"),
		"debugger_active": EngineDebugger.is_active(),
	}
	var file := FileAccess.open(marker, FileAccess.WRITE)
	if file == null:
		printerr("Export probe cannot write marker: ", marker)
		get_tree().quit(70)
		return
	file.store_string(JSON.stringify(payload))
	file.close()
	get_tree().quit(0)
`;

async function verify(package_directory) {
	assert.equal(process.platform, "win32", "Windows export verification must run on Windows.");
	const manifest = JSON.parse(await fs.readFile(path.join(package_directory, "RELEASE_MANIFEST.json"), "utf8"));
	assert.equal(manifest.format_version, 1);
	assert.equal(manifest.platform, "windows");
	assert.equal(manifest.architecture, "x86_64");
	assert.ok(manifest.files_sha256 && typeof manifest.files_sha256 === "object", "Package manifest must contain file hashes.");
	const templates = manifest.export_templates;
	assert.ok(templates, "Package has no export_templates metadata; package all four Windows debug/release templates first.");
	assert.equal(templates.architecture, "x86_64");
	assert.equal(templates.editor_version, manifest.editor_version);
	assert.match(templates.version, /^[a-zA-Z0-9_.-]+$/, "Invalid template version directory.");
	assert.equal(templates.directory, `editor_data/export_templates/${templates.version}`);
	assert.ok(manifest.editor_version.startsWith(`${templates.version}.`), "Template version must match the editor version.");
	const expected_files = {
		debug: `${templates.directory}/windows_debug_x86_64.exe`,
		release: `${templates.directory}/windows_release_x86_64.exe`,
		debug_console: `${templates.directory}/windows_debug_x86_64_console.exe`,
		release_console: `${templates.directory}/windows_release_x86_64_console.exe`,
	};
	assert.deepEqual(templates.files, expected_files, "Unexpected template paths; builtin Windows template discovery must work.");
	const runtime_files = templates.runtime_files;
	if (runtime_files !== undefined) assert.deepEqual(runtime_files, {
		d3d12_core: `${templates.directory}/D3D12Core.x86_64.dll`,
		d3d12_layers: `${templates.directory}/d3d12SDKLayers.x86_64.dll`,
		license: `${templates.directory}/D3D12-LICENSE.txt`,
	}, "Unexpected Direct3D 12 runtime paths.");
	for (const name of ["Godot.exe", "_sc_", ...Object.values(expected_files), ...Object.values(runtime_files ?? {})]) {
		assert.ok(name in manifest.files_sha256, `Missing package component in manifest: ${name}`);
		const filename = contained_file(package_directory, name);
		assert.equal(await sha256(filename), manifest.files_sha256[name], `Package hash mismatch: ${name}`);
		if (name.endsWith(".exe") || name.endsWith(".dll")) await assert_pe(filename);
	}

	// All settings, caches, user data, markers and processes belong to this one
	// temporary directory. Paths with spaces exercise real portable output paths.
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot Windows export verification "));
	const bundle = path.join(directory, "Portable Godot");
	const project = path.join(directory, "Source Project");
	const standalone = path.join(directory, "Standalone Games");
	const profile = path.join(directory, "Disposable Profile");
	const inherited_env = Object.fromEntries(Object.entries(process.env).filter(([name]) => {
		const upper = name.toUpperCase();
		return !upper.startsWith("GODOT_MCP_") && !upper.startsWith("GODOT_EXPORT_VERIFY_") && !["APPDATA", "LOCALAPPDATA", "TEMP", "TMP"].includes(upper);
	}));
	const env = { ...inherited_env, APPDATA: path.join(profile, "Roaming"), LOCALAPPDATA: path.join(profile, "Local"), TEMP: path.join(directory, "Temp"), TMP: path.join(directory, "Temp") };
	try {
		await fs.mkdir(project);
		await fs.mkdir(standalone);
		for (const name of [env.APPDATA, env.LOCALAPPDATA, env.TEMP]) await fs.mkdir(name, { recursive: true });
		await fs.cp(package_directory, bundle, { recursive: true });
		await fs.writeFile(path.join(project, "project.godot"), project_config);
		await fs.writeFile(path.join(project, "export_presets.cfg"), export_presets(runtime_files !== undefined));
		await fs.writeFile(path.join(project, "main.tscn"), scene);
		await fs.writeFile(path.join(project, "main.gd"), script);
		const editor = path.join(bundle, "Godot.exe");
		assert.equal((await run("Check packaged editor version", editor, ["--version"], bundle, env)).trim(), manifest.editor_version);
		await run("Import disposable export project", editor, ["--headless", "--path", project, "--editor", "--import"], project, env, 120_000);
		for (const mode of ["debug", "release"]) {
			const output = path.join(standalone, `${mode} output`);
			await fs.mkdir(output);
			const executable = path.join(output, "ExportProbe.exe");
			await run(`Export ${mode} using installed templates`, editor,
				["--headless", "--path", project, "--export-" + mode, "Windows Desktop", executable], project, env, 120_000);
			const wrapper = path.join(output, "ExportProbe.console.exe");
			await assert_pe(executable);
			await assert_pe(wrapper);
			assert.equal(await sha256(executable), manifest.files_sha256[expected_files[mode]], `${mode} main executable must be the installed template.`);
			assert.equal(await sha256(wrapper), manifest.files_sha256[expected_files[`${mode}_console`]], `${mode} wrapper must be the installed template.`);
			const pack = await fs.readFile(path.join(output, "ExportProbe.pck"));
			assert.equal(pack.toString("ascii", 0, 4), "GDPC", `${mode} export must include a valid standalone PCK.`);
			assert.ok(pack.length > 16, `${mode} PCK must contain project data.`);
			if (runtime_files) {
				// Verify export copying independently of the OpenGL/headless smoke;
				// this does not exercise a Direct3D 12 driver or GPU.
				assert.equal(await sha256(path.join(output, "x86_64", "D3D12Core.dll")), manifest.files_sha256[runtime_files.d3d12_core], `${mode} exported Direct3D 12 core must match the installed runtime.`);
				assert.equal(await sha256(path.join(output, "x86_64", "d3d12SDKLayers.dll")), manifest.files_sha256[runtime_files.d3d12_layers], `${mode} exported Direct3D 12 debug layers must match the installed runtime.`);
			}
		}
		// Games must start from their own output directories with the source
		// project and portable editor removed from their original locations.
		await fs.rename(project, path.join(directory, "Unused Source Project"));
		await fs.rename(bundle, path.join(directory, "Unused Portable Godot"));
		const results = [];
		for (const mode of ["debug", "release"]) {
			const output = path.join(standalone, `${mode} output`);
			for (const kind of ["main", "console"]) {
				const marker = path.join(directory, `${mode}-${kind}-${randomUUID()}.json`);
				const token = randomUUID();
				const executable = path.join(output, kind === "main" ? "ExportProbe.exe" : "ExportProbe.console.exe");
				await run(`Run standalone ${mode} ${kind}`, executable, ["--headless"], output,
					{ ...env, GODOT_EXPORT_VERIFY_MARKER: marker, GODOT_EXPORT_VERIFY_TOKEN: token }, 30_000);
				let payload;
				try { payload = JSON.parse(await fs.readFile(marker, "utf8")); }
				catch (error) { throw new Error(`${mode} ${kind} did not write its unique scene-ready marker.`, { cause: error }); }
				assert.equal(payload.token, token, `${mode} ${kind} marker must belong to this run.`);
				assert.equal(payload.debug_build, mode === "debug", `${mode} ${kind} has the wrong debug build flag.`);
				assert.equal(payload.editor, false, `${mode} ${kind} must be an exported game, not an editor.`);
				assert.equal(payload.template, true, `${mode} ${kind} must report the template feature.`);
				assert.equal(payload.scene_ready, true);
				assert.equal(payload.scene_name, "ExportProbe");
				assert.equal(payload.mcp_capture, false, "Ordinary exported games must not install an MCP runtime capture.");
				assert.equal(payload.debugger_active, false, "Exported smoke runs must not depend on an editor debugger.");
				const version = payload.version;
				assert.equal(typeof version.build, "string");
				assert.equal(typeof version.hash, "string");
				const full_version = `${templates.version}.${version.build}${version.hash === "unknown" ? "" : "." + version.hash.slice(0, 9)}`;
				assert.equal(full_version, manifest.editor_version, `${mode} ${kind} engine version does not match the packaged editor.`);
				const numeric = `${version.major}.${version.minor}${version.patch ? "." + version.patch : ""}.${version.status}`;
				assert.ok(templates.version === numeric || templates.version.startsWith(`${numeric}.`), `${mode} ${kind} reports a mismatched engine version.`);
				results.push({ mode, executable: kind, version: full_version, debug_build: payload.debug_build, scene_ready: true, mcp_capture: false });
			}
		}
		process.stdout.write(`PASS: builtin Windows debug/release template discovery, both exports and PCKs${runtime_files ? ", Direct3D 12 runtime export hashes" : ""}, all four standalone headless main/console game runs, engine versions, build flags and inactive MCP capture.\n${JSON.stringify({ package: package_directory, d3d12_runtime_exported: runtime_files !== undefined, results }, null, 2)}\n`);
	} finally {
		// Each run awaits its owned process's close event, including failure and
		// timeout paths. Never delete a directory while an owned process is live.
		assert.equal(owned_processes.size, 0, "An export verification process is still running; temporary files preserved.");
		assert.equal(path.dirname(directory), os.tmpdir());
		await fs.rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
	}
}

try {
	if (process.argv.length === 3 && ["--help", "-h"].includes(process.argv[2])) process.stdout.write(usage + "\n");
	else {
		assert.ok(process.argv.length === 4 && process.argv[2] === "--package" && process.argv[3], usage);
		await verify(path.resolve(process.argv[3]));
	}
} catch (error) {
	process.stderr.write(`FAIL: ${error instanceof Error ? error.message : String(error)}\n`);
	process.exitCode = interruption.signal.aborted ? 130 : 1;
}
