import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

import { CLIENT_IDS, read_client_registration } from "../src/client_config.js";

const run = promisify(execFile);
const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));

test("shared setup registers all supported client formats, backs up, repeats, and removes only Godot", { timeout: 30_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot setup spaces Unicode 项目 "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	await fs.writeFile(path.join(directory, "project.godot"), '[application]\nconfig/name="Setup fixture"\n');
	for (const client of CLIENT_IDS) {
		const filename = path.join(directory, `${client}.config`);
		const original = client === "codex" ? '# Keep this comment\nmodel = "keep"\n[mcp_servers.other]\ncommand = "other"\n'
			: client === "hermes" ? '# Keep this comment\nmodel: keep\nmcp_servers:\n  other:\n    command: other\n'
				: '{\n  // Keep this comment\n  "model": "keep",\n  "unrelated": { "command": "other" }\n}\n';
		await fs.writeFile(filename, original);
		const flags = ["--client", client, "--config", filename, "--project-root", directory, "--no-check", "--json"];
		const first = JSON.parse((await run(process.execPath, [bridge, "setup", ...flags], { windowsHide: true })).stdout)[0];
		assert.equal(first.changed, true);
		assert.equal(first.registered, true);
		assert.equal(first.config_path, filename);
		assert.equal(await fs.readFile(first.backup, "utf8"), original);
		const installed = await fs.readFile(filename, "utf8");
		assert.match(installed, /Keep this comment/);
		assert.match(installed, /keep/);
		assert.match(installed, /other/);
		const registration = read_client_registration(client, installed);
		assert.equal(registration.launch?.command, process.execPath);
		assert.deepEqual(registration.launch?.args, [bridge, "--project-root", directory]);
		const second = JSON.parse((await run(process.execPath, [bridge, "setup", ...flags], { windowsHide: true })).stdout)[0];
		assert.equal(second.changed, false);
		assert.equal(second.backup, undefined);
		assert.equal(await fs.readFile(filename, "utf8"), installed);
		const removed = JSON.parse((await run(process.execPath, [bridge, "remove", ...flags], { windowsHide: true })).stdout)[0];
		assert.equal(removed.changed, true);
		const final = await fs.readFile(filename, "utf8");
		assert.equal(read_client_registration(client, final).present, false);
		assert.match(final, /keep/);
		assert.match(final, /other/);
	}
});

test("dry-run, malformed config, and existing foreign registration do not write files", { timeout: 15_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot setup no writes "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const filename = path.join(directory, "absent", "mcp_config.json");
	const flags = ["--client", "antigravity", "--config", filename, "--no-check", "--json"];
	const preview = JSON.parse((await run(process.execPath, [bridge, "setup", ...flags, "--dry-run"], { windowsHide: true })).stdout)[0];
	assert.equal(preview.dry_run, true);
	assert.equal(preview.registered, false);
	await assert.rejects(fs.access(path.dirname(filename)), { code: "ENOENT" });
	await fs.mkdir(path.dirname(filename));
	for (const content of ['{"mcpServers":', '{"mcpServers":{"godot":{"command":"another-server","args":[]}}}']) {
		await fs.writeFile(filename, content);
		await assert.rejects(run(process.execPath, [bridge, "setup", ...flags], { windowsHide: true }), (error: any) => {
			assert.equal(error.code, 1);
			assert.ok(JSON.parse(error.stdout)[0].error);
			return true;
		});
		assert.equal(await fs.readFile(filename, "utf8"), content);
		assert.deepEqual(await fs.readdir(path.dirname(filename)), ["mcp_config.json"]);
	}
});

test("configuration export is read-only and non-interactive setup fails with a useful instruction", async () => {
	for (const client of CLIENT_IDS) {
		const output = await run(process.execPath, [bridge, "config", "--client", client], { windowsHide: true });
		assert.equal(read_client_registration(client, output.stdout).launch?.command, process.execPath);
	}
	await assert.rejects(run(process.execPath, [bridge, "setup"], { windowsHide: true }), (error: any) => {
		assert.equal(error.code, 1);
		assert.match(error.stderr, /Choose a client with --client/);
		return true;
	});
});

test("setup refuses a wrong Godot project and doctor reports a missing registration", async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot setup project checks "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const filename = path.join(directory, "mcp_config.json");
	await assert.rejects(run(process.execPath, [bridge, "setup", "--client", "antigravity", "--config", filename, "--project-root", directory, "--no-check"], { windowsHide: true }), (error: any) => {
		assert.match(error.stderr, /no project.godot/);
		return true;
	});
	await assert.rejects(fs.access(filename), { code: "ENOENT" });
	await assert.rejects(run(process.execPath, [bridge, "doctor", "--client", "antigravity", "--config", filename, "--json"], { windowsHide: true }), (error: any) => {
		assert.equal(error.code, 1);
		assert.match(JSON.parse(error.stdout)[0].error, /No usable/);
		return true;
	});
});
