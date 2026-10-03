import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const run = promisify(execFile);
const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));

test("generic exports launch the same bridge, pin project/session paths, and never write configuration", async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot MCP export "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const project = path.join(directory, "Project with spaces");
	const session = path.join(directory, "Session with spaces.json");
	const original = '{"otherServer":{"command":"example"}}';
	await fs.writeFile(path.join(directory, "config.json"), original);
	const expected = { command: process.execPath, args: [bridge, "--project-root", project, "--session-file", session] };
	for (const format of ["mcp", "connection"]) {
		const { stdout, stderr } = await run(process.execPath, [bridge, "--print-config", format, "--project-root", project, "--session-file", session], { cwd: directory, windowsHide: true });
		assert.equal(stderr, "");
		const config = JSON.parse(stdout);
		assert.deepEqual(format === "mcp" ? config.mcpServers.godot : config, expected);
	}
	assert.deepEqual(await fs.readdir(directory), ["config.json"]);
	assert.equal(await fs.readFile(path.join(directory, "config.json"), "utf8"), original);
});

test("export rejects unknown formats and missing arguments before starting an MCP server", async () => {
	for (const args of [["--print-config"], ["--print-config", "unknown"], ["--project-root"], ["--print-config", "mcp", "--print-config", "connection"]]) {
		await assert.rejects(run(process.execPath, [bridge, ...args], { windowsHide: true }), (error: any) => {
			assert.equal(error.code, 1);
			assert.equal(error.stdout, "");
			assert.match(error.stderr, /Fatal bridge error:/);
			return true;
		});
	}
});
