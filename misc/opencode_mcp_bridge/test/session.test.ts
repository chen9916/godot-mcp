import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { get_session_identity, load_session, SessionError } from "../src/session.js";

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
