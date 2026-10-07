import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

import { get_session_identity, load_session, resolve_session_file_path, SessionError } from "../src/session.js";

test("loads protocol v2 identity and capability metadata", () => {
	const directory = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-session-"));
	const session_file = path.join(directory, "session.json");
	try {
		fs.writeFileSync(session_file, JSON.stringify({
			session_id: "session-uuid",
			protocol_version: 2,
			port: 6501,
			pid: process.pid,
			project_path: directory,
			started_at: 1,
			expires_at: Math.floor(Date.now() / 1000) + 60,
			capabilities: { runtime_control: true },
		}));

		const loaded = load_session({ session_file });
		assert.equal(get_session_identity(loaded), "session-uuid");
		assert.equal(loaded.protocol_version, 2);
		assert.equal(loaded.capabilities.runtime_control, true);
	} finally {
		fs.rmSync(directory, { recursive: true, force: true });
	}
});

test("retains a backward-compatible identity for v1 session files", () => {
	const directory = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-session-"));
	const session_file = path.join(directory, "session.json");
	try {
		fs.writeFileSync(session_file, JSON.stringify({
			port: 6502,
			pid: process.pid,
			capabilities: { read_project: true },
		}));
		const loaded = load_session({ session_file });
		assert.equal(get_session_identity(loaded), `legacy:${process.pid}:6502`);
	} finally {
		fs.rmSync(directory, { recursive: true, force: true });
	}
});

test("reports malformed session metadata with a stable code", () => {
	const directory = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-session-"));
	const session_file = path.join(directory, "session.json");
	try {
		fs.writeFileSync(session_file, "{");
		assert.throws(
			() => load_session({ session_file }),
			(error: unknown) => error instanceof SessionError && error.code === "SESSION_INVALID" && error.retryable,
		);
	} finally {
		fs.rmSync(directory, { recursive: true, force: true });
	}
});

// Exercise the renamed discovery path with actual metadata and a nested working directory.
test("discovers neutral session metadata from a project and its child directories", () => {
	const directory = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-discovery-"));
	const previous_cwd = process.cwd();
	try {
		const session_file = path.join(directory, ".godot", "godot_mcp", "session.json");
		const nested = path.join(directory, "assets", "scenes");
		fs.mkdirSync(path.dirname(session_file), { recursive: true });
		fs.mkdirSync(nested, { recursive: true });
		fs.writeFileSync(session_file, JSON.stringify({
			session_id: "discovered-editor", protocol_version: 2, port: 6501, pid: process.pid,
			project_path: directory, expires_at: Math.floor(Date.now() / 1000) + 60,
			capabilities: { read_project: true },
		}));
		assert.equal(resolve_session_file_path({ project_root: directory }), session_file);
		assert.equal(load_session({ project_root: directory }).session_id, "discovered-editor");
		process.chdir(nested);
		assert.equal(resolve_session_file_path(), session_file);
		assert.equal(load_session().session_id, "discovered-editor");
		assert.equal(resolve_session_file_path({ session_file }), session_file);
	} finally {
		process.chdir(previous_cwd);
		fs.rmSync(directory, { recursive: true, force: true });
	}
});

test("matching portable and source editor registries precede global recent projects, while explicit pins and project cwd win", async () => {
	const directory = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-portable-discovery-"));
	const previous_cwd = process.cwd();
	const previous_appdata = process.env.APPDATA;
	const previous_home = process.env.HOME;
	try {
		process.env.APPDATA = path.join(directory, "AppData");
		process.env.HOME = path.join(directory, "Home");
		const global_registry = process.platform === "win32" ? path.join(process.env.APPDATA, "Godot", "projects.cfg")
			: process.platform === "darwin" ? path.join(process.env.HOME, "Library", "Application Support", "Godot", "projects.cfg")
				: path.join(process.env.HOME, ".config", "godot", "projects.cfg");
		const make_session = (name: string): string => {
			const filename = path.join(directory, name, ".godot", "godot_mcp", "session.json");
			fs.mkdirSync(path.dirname(filename), { recursive: true });
			fs.writeFileSync(filename, "{}");
			return filename;
		};
		const portable_session = make_session("Portable Game");
		const global_session = make_session("Global Game");
		fs.utimesSync(global_session, new Date(Date.now() + 60_000), new Date(Date.now() + 60_000));
		const registry_contents = (filename: string) => `[${path.resolve(filename, "../../..").split(path.sep).join("/")}]\nfavorite=false\n`;
		fs.mkdirSync(path.dirname(global_registry), { recursive: true });
		fs.writeFileSync(global_registry, registry_contents(global_session));
		for (const layout of ["portable", "source"] as const) {
			const root = path.join(directory, layout);
			const editor_directory = layout === "portable" ? root : path.join(root, "bin");
			const module_directory = layout === "portable" ? path.join(root, "bridge", "dist") : path.join(root, "misc", "godot_mcp_bridge", "dist");
			fs.mkdirSync(module_directory, { recursive: true });
			fs.mkdirSync(path.join(editor_directory, "editor_data"), { recursive: true });
			const marker = path.join(editor_directory, layout === "portable" ? "_sc_" : "._sc_");
			fs.writeFileSync(marker, "");
			const registry = path.join(editor_directory, "editor_data", "projects.cfg");
			fs.writeFileSync(registry, registry_contents(portable_session));
			fs.writeFileSync(path.join(module_directory, "package.json"), '{"type":"module"}');
			const copied_module = path.join(module_directory, "session.js");
			fs.copyFileSync(fileURLToPath(new URL("../src/session.js", import.meta.url)), copied_module);
			const copied = await import(pathToFileURL(copied_module).href) as typeof import("../src/session.js");
			process.chdir(directory); // The MCP client need not run in the bundle.
			assert.equal(copied.resolve_session_file_path(), portable_session);
			assert.equal(copied.resolve_session_file_path({ project_root: path.resolve(global_session, "../../..") }), global_session);
			assert.equal(copied.resolve_session_file_path({ session_file: global_session }), global_session);
			process.chdir(path.resolve(global_session, "../../.."));
			assert.equal(copied.resolve_session_file_path(), global_session);
			process.chdir(directory);
			fs.writeFileSync(registry, "");
			assert.equal(copied.resolve_session_file_path(), global_session);
			fs.writeFileSync(registry, registry_contents(portable_session));
			fs.unlinkSync(marker);
			assert.equal(copied.resolve_session_file_path(), global_session);
		}
	} finally {
		process.chdir(previous_cwd);
		if (previous_appdata === undefined) delete process.env.APPDATA; else process.env.APPDATA = previous_appdata;
		if (previous_home === undefined) delete process.env.HOME; else process.env.HOME = previous_home;
		fs.rmSync(directory, { recursive: true, force: true });
	}
});
