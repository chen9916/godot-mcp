import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { read_config_file, write_config_file } from "../src/setup_storage.js";

test("atomic config saving backs up exact original contents and rejects concurrent edits", async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot config storage "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const filename = path.join(directory, "config.json");
	await fs.writeFile(filename, "original\r\n");
	const snapshot = await read_config_file(filename);
	await fs.writeFile(filename, "external edit\n");
	await assert.rejects(write_config_file(filename, snapshot, "replacement\n"), /changed during setup/);
	assert.equal(await fs.readFile(filename, "utf8"), "external edit\n");
	assert.deepEqual(await fs.readdir(directory), ["config.json"]);
	const saved = await write_config_file(filename, await read_config_file(filename), "replacement\n");
	assert.equal(await fs.readFile(saved.backup!, "utf8"), "external edit\n");
	assert.equal(await fs.readFile(filename, "utf8"), "replacement\n");
	assert.equal((await fs.readdir(directory)).some((name) => name.endsWith(".tmp")), false);
});

test("storage rejects directories and detects a file created after a missing-file snapshot", async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot config race "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	await assert.rejects(read_config_file(directory), /regular file/);
	const filename = path.join(directory, "new.json");
	const absent = await read_config_file(filename);
	await fs.writeFile(filename, "external new file");
	await assert.rejects(write_config_file(filename, absent, "ours"), /changed during setup/);
	assert.equal(await fs.readFile(filename, "utf8"), "external new file");
});
