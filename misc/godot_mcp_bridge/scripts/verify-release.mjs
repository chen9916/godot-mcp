import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

assert.equal(process.argv.length, 4, "Usage: node scripts/verify-release.mjs --package <new Windows bundle>");
assert.equal(process.argv[2], "--package");
assert.equal(process.platform, "win32", "This release gate currently supports Windows x64 only.");
const bridge = fileURLToPath(new URL("../", import.meta.url));
const engine = path.resolve(bridge, "../..");
const bundle = path.resolve(process.argv[3]);
const artifacts = path.join(engine, "artifacts");
const relative = path.relative(artifacts, bundle);
assert.ok(relative && relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative), "Bundle must be inside this checkout's artifacts directory.");
const manifest_bytes = await fs.readFile(path.join(bundle, "RELEASE_MANIFEST.json"));
const manifest = JSON.parse(manifest_bytes);
assert.equal(manifest.git_dirty, false, "Commit and rebuild the source before the release gate.");
assert.ok(manifest.export_templates, "Ready-to-ship Windows releases must include matching export templates.");
const report_file = `${bundle}.verification.json`;
await assert.rejects(fs.lstat(report_file), { code: "ENOENT" }, "Refusing to overwrite an existing verification report.");
const log_directory = `${bundle}.verification-logs-${Date.now()}`;
await fs.mkdir(log_directory);
const temporary_base = path.join(process.env.LOCALAPPDATA, "Temp", "opencode");
await fs.mkdir(temporary_base, { recursive: true });
const inherited = Object.fromEntries(Object.entries(process.env).filter(([name]) => !name.toUpperCase().startsWith("GODOT_MCP_")));
const base_env = { ...inherited, TEMP: temporary_base, TMP: temporary_base };
const editor = path.join(bundle, "Godot.exe");
const node = path.join(bundle, "runtime", "node.exe");
const checks = [];
const report = {
	status: "running", platform: "windows-x86_64", scope: "Custom Godot 4.8-dev MCP distribution; Windows x64 only",
	git_revision: manifest.git_revision, editor_version: manifest.editor_version, node_version: manifest.node_version,
	manifest_sha256: createHash("sha256").update(manifest_bytes).digest("hex"),
	export_templates: manifest.export_templates, checks,
};

async function run(name, executable, args, cwd = bridge, environment = {}, timeout_ms = 300_000) {
	console.log(`Checking ${name}...`);
	const started = Date.now();
	const output = await fs.open(path.join(log_directory, `${name}.log`), "wx");
	const child = spawn(executable, args, { cwd, env: { ...base_env, ...environment }, windowsHide: true, stdio: ["ignore", output.fd, output.fd] });
	let timed_out = false;
	const timer = setTimeout(() => { timed_out = true; child.kill(); }, timeout_ms);
	const result = await new Promise((resolve) => {
		child.once("error", (error) => resolve({ code: null, error: error.message }));
		child.once("close", (code, signal) => resolve({ code, signal }));
	});
	clearTimeout(timer);
	await output.close();
	const pass = !timed_out && !result.error && result.code === 0;
	checks.push({ name, pass, duration_ms: Date.now() - started, log: `${path.basename(log_directory)}/${name}.log`, ...(pass ? {} : { ...result, timed_out }) });
	assert.ok(pass, `${name} failed. Evidence: ${path.join(log_directory, `${name}.log`)}`);
}

try {
	await run("compile-bridge-tests", process.execPath, [path.join(path.dirname(process.execPath), "node_modules/npm/bin/npm-cli.js"), "run", "build:test"]);
	await run("bridge-default", process.execPath, ["scripts/run-tests.mjs"]);
	await run("native-units", node, ["misc/godot_mcp_bridge/scripts/run-native-tests.mjs", editor], engine);
	await run("package-smoke-source", node, ["scripts/verify-package.mjs", "--package", bundle, "--check-source"]);
	await run("windows-export", node, ["scripts/verify-windows-export.mjs", "--package", bundle]);
	for (const [name, file, environment] of [
		["animation-workflow", "animation_integration.test.js", { GODOT_MCP_ANIMATION_EDITOR: editor }],
		["native-protocol", "native_protocol_integration.test.js", { GODOT_MCP_PROTOCOL_EDITOR: editor }],
		["release-workflow", "release_editor_integration.test.js", { GODOT_MCP_RELEASE_BUNDLE: bundle }],
		["setup-workflow", "editor_setup_integration.test.js", { GODOT_MCP_SETUP_EDITOR: editor }],
	]) await run(name, node, ["--test", `.test-dist/test/${file}`], bridge, environment);
	// Detect package modification during the gate before certifying its manifest.
	assert.deepEqual(await fs.readFile(path.join(bundle, "RELEASE_MANIFEST.json")), manifest_bytes);
	await run("package-final-inventory", node, ["scripts/verify-package.mjs", "--package", bundle, "--check-source"]);
	report.status = "verified";
	report.checked_at = new Date().toISOString();
	await fs.writeFile(report_file, JSON.stringify(report, null, 2) + "\n", { flag: "wx" });
	console.log(`PASS: all release checks. Report: ${report_file}`);
} catch (error) {
	report.status = "failed";
	report.checked_at = new Date().toISOString();
	report.error = error.message;
	const failed_report = `${bundle}.verification-failed-${Date.now()}.json`;
	await fs.writeFile(failed_report, JSON.stringify(report, null, 2) + "\n", { flag: "wx" });
	throw new Error(`${error.message}\nFailed report: ${failed_report}`, { cause: error });
}
