import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { pathToFileURL } from "node:url";

import { diagnose_connection } from "../src/setup_doctor.js";

const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));

test("doctor reports the bridge healthy when the real bridge has no editor session", { timeout: 15_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot doctor without editor "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const report = await diagnose_connection({ command: process.execPath, args: [bridge, "--session-file", path.join(directory, "missing-session.json")] });
	assert.equal(report.ok, false);
	assert.equal(report.bridge.ok, true);
	assert.ok((report.bridge.tool_count ?? 0) > 0);
	assert.equal(report.editor.ok, false);
	assert.match(report.editor.message, /Open the project in Godot/);
});

test("doctor confirms a real bridge connection to a fake Godot TCP editor endpoint", { timeout: 15_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot doctor connected "));
	const session_file = path.join(directory, "session.json");
	const sockets = new Set<net.Socket>();
	const session_id = "doctor-test-session";
	const project_path = path.join(directory, "Project with spaces");
	const marker_file = path.join(directory, "launch-context.json");
	const appdata_path = path.join(directory, "AppData fixture");
	let return_editor_failure = false;
	await fs.mkdir(project_path, { recursive: true });
	const endpoint = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk.toString("utf8");
			while (buffer.includes("\n")) {
				const end = buffer.indexOf("\n");
				const request = JSON.parse(buffer.slice(0, end));
				buffer = buffer.slice(end + 1);
				const result = request.method === "godot.session.info"
					? return_editor_failure && calls_to_session_info++ > 0
						? { ok: false, data: { project_path } }
						: { ok: true, data: { session_id, project_path } }
					: { ok: true, data: {} };
				socket.write(JSON.stringify({ jsonrpc: "2.0", id: request.id, result }) + "\n");
			}
		});
	});
	let calls_to_session_info = 0;
	t.after(async () => {
		for (const socket of sockets) socket.destroy();
		await new Promise<void>((resolve) => endpoint.close(() => resolve()));
		await fs.rm(directory, { recursive: true, force: true });
	});
	await new Promise<void>((resolve) => endpoint.listen(0, "127.0.0.1", resolve));
	const address = endpoint.address();
	assert.ok(address && typeof address === "object");
	await fs.writeFile(session_file, JSON.stringify({
		session_id,
		protocol_version: 2,
		port: address.port,
		pid: process.pid,
		project_path,
		started_at: Math.floor(Date.now() / 1000),
		expires_at: Math.floor(Date.now() / 1000) + 60,
		capabilities: { read_scene: true, read_project: true },
	}));
	const wrapper_file = path.join(directory, "configured-launch.mjs");
	const wrapper_source = [
		'import { spawn } from "node:child_process";',
		'import fs from "node:fs/promises";',
		`await fs.writeFile(${JSON.stringify(marker_file)}, JSON.stringify({ cwd: process.cwd(), appdata: process.env.APPDATA }));`,
		`const child = spawn(${JSON.stringify(process.execPath)}, [${JSON.stringify(bridge)}, "--session-file", ${JSON.stringify(session_file)}], { stdio: "inherit" });`,
		'child.on("error", (error) => { process.stderr.write(error.message); process.exit(1); });',
		'child.on("exit", (code) => process.exit(code ?? 1));',
	].join("\n");
	await fs.writeFile(wrapper_file, wrapper_source);

	const report = await diagnose_connection({
		command: process.execPath,
		args: [wrapper_file],
		cwd: project_path,
		env: { APPDATA: appdata_path },
	});
	assert.equal(report.ok, true);
	assert.equal(report.bridge.ok, true);
	assert.equal(report.editor.ok, true);
	assert.equal(report.editor.project_path, project_path);
	assert.equal(report.editor.session_id, session_id);
	assert.deepEqual(JSON.parse(await fs.readFile(marker_file, "utf8")), { cwd: project_path, appdata: appdata_path });

	return_editor_failure = true;
	calls_to_session_info = 0;
	const failed_editor = await diagnose_connection({ command: process.execPath, args: [bridge, "--session-file", session_file] });
	assert.equal(failed_editor.ok, false);
	assert.equal(failed_editor.bridge.ok, true);
	assert.equal(failed_editor.editor.ok, false);
});

test("doctor gives bounded actionable reports for missing executables and a child that never initializes", { timeout: 10_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot doctor failures "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const missing = await diagnose_connection({ command: path.join(directory, "no-such-executable"), args: [] }, 500);
	assert.equal(missing.ok, false);
	assert.equal(missing.bridge.ok, false);
	assert.match(missing.bridge.message, /configured command and arguments/);
	assert.equal(missing.editor.ok, false);

	const pid_file = path.join(directory, "hung-server.pid");
	const hung_code = "require('node:fs').writeFileSync(process.env.GODOT_MCP_TEST_PID_FILE, String(process.pid)); process.stdin.resume(); setInterval(() => {}, 1000);";
	const doctor_module = pathToFileURL(fileURLToPath(new URL("../src/setup_doctor.js", import.meta.url))).href;
	const subprocess_code = `import(${JSON.stringify(doctor_module)}).then(async ({ diagnose_connection }) => { const report = await diagnose_connection({ command: process.execPath, args: ["-e", ${JSON.stringify(hung_code)}], env: { GODOT_MCP_TEST_PID_FILE: ${JSON.stringify(pid_file)} } }, 300); process.stdout.write(JSON.stringify(report)); }).catch((error) => { process.stderr.write(String(error)); process.exitCode = 1; });`;
	const started = Date.now();
	const child = spawn(process.execPath, ["--input-type=module", "-e", subprocess_code], { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
	let stdout = "";
	let stderr = "";
	child.stdout.setEncoding("utf8").on("data", (chunk) => { stdout += chunk; });
	child.stderr.setEncoding("utf8").on("data", (chunk) => { stderr += chunk; });
	let child_timed_out = false;
	let child_timeout: NodeJS.Timeout | undefined;
	const exit = await Promise.race([
		new Promise<number | null>((resolve) => child.once("close", resolve)),
		new Promise<number | null>((resolve) => {
			child_timeout = setTimeout(() => {
				child_timed_out = true;
				child.kill();
				child.once("close", resolve);
			}, 4000);
		}),
	]);
	if (child_timeout) clearTimeout(child_timeout);
	if (child_timed_out) {
		try {
			const server_pid = Number(await fs.readFile(pid_file, "utf8"));
			if (Number.isInteger(server_pid) && server_pid > 0 && server_pid !== process.pid) process.kill(server_pid, "SIGTERM");
		} catch { /* Fixture process may already have exited. */ }
	}
	assert.equal(child_timed_out, false, `subprocess remained alive past the timeout (stderr: ${stderr})`);
	assert.equal(exit, 0, stderr);
	const hung = JSON.parse(stdout);
	assert.equal(hung.ok, false);
	assert.equal(hung.bridge.ok, false);
	assert.match(hung.bridge.message, /Timed out while starting the MCP bridge/);
	assert.ok(Date.now() - started < 4000, "diagnostic subprocess should exit promptly after its configured timeout");
});
