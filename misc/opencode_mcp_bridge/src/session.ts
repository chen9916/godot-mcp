import fs from "node:fs";
import path from "node:path";

import type { GodotCapability } from "./tool_registry.js";

export interface SessionCapabilities {
	read_scene?: boolean;
	write_scene?: boolean;
	read_script?: boolean;
	write_script?: boolean;
	save_resource?: boolean;
	[key: string]: unknown;
}

export interface SessionData {
	port: number;
	token: string;
	pid: number;
	expires_at?: number;
	capabilities: SessionCapabilities;
}

export interface SessionLoadOptions {
	project_root?: string;
	session_file?: string;
	now_unix?: number;
}

export class SessionError extends Error {
	constructor(p_message: string) {
		super(p_message);
		this.name = "SessionError";
	}
}

function _default_now_unix(): number {
	return Math.floor(Date.now() / 1000);
}

function _is_process_alive(p_pid: number): boolean {
	if (!Number.isInteger(p_pid) || p_pid <= 0) {
		return false;
	}

	try {
		process.kill(p_pid, 0);
		return true;
	} catch (error: unknown) {
		if (error && typeof error === "object" && "code" in error) {
			const code = (error as { code?: string }).code;
			if (code === "EPERM") {
				return true;
			}
		}
		return false;
	}
}

function _ensure_object(p_value: unknown, p_name: string): Record<string, unknown> {
	if (!p_value || typeof p_value !== "object" || Array.isArray(p_value)) {
		throw new SessionError(`${p_name} must be an object.`);
	}
	return p_value as Record<string, unknown>;
}

function _to_integer(p_value: unknown, p_name: string): number {
	if (typeof p_value !== "number" || !Number.isInteger(p_value)) {
		throw new SessionError(`${p_name} must be an integer.`);
	}
	return p_value;
}

export function resolve_session_file_path(p_options: SessionLoadOptions = {}): string {
	if (p_options.session_file) {
		return path.resolve(p_options.session_file);
	}

	const project_root = path.resolve(p_options.project_root ?? process.cwd());
	return path.join(project_root, ".godot", "opencode_mcp", "session.json");
}

export function load_session(p_options: SessionLoadOptions = {}): SessionData {
	const session_file = resolve_session_file_path(p_options);

	if (!fs.existsSync(session_file)) {
		throw new SessionError(
			`Session file not found at '${session_file}'. Start Godot editor and enable network/opencode_mcp/enabled.`,
		);
	}

	const raw = fs.readFileSync(session_file, "utf8");
	let decoded: unknown;
	try {
		decoded = JSON.parse(raw);
	} catch {
		throw new SessionError(`Session file is not valid JSON: '${session_file}'.`);
	}

	const root = _ensure_object(decoded, "Session payload");
	const port = _to_integer(root.port, "port");
	if (port <= 0 || port > 65535) {
		throw new SessionError("Session port must be in range 1..65535.");
	}

	if (typeof root.token !== "string" || root.token.length === 0) {
		throw new SessionError("Session token must be a non-empty string.");
	}

	const pid = _to_integer(root.pid, "pid");
	if (!_is_process_alive(pid)) {
		throw new SessionError("Session process is no longer running. Restart Godot editor.");
	}

	const capabilities = _ensure_object(root.capabilities, "capabilities") as SessionCapabilities;

	let expires_at: number | undefined;
	if (root.expires_at !== undefined) {
		expires_at = _to_integer(root.expires_at, "expires_at");
		const now_unix = p_options.now_unix ?? _default_now_unix();
		if (expires_at <= now_unix) {
			throw new SessionError("Session token is expired. Refresh from Godot editor.");
		}
	}

	return {
		port,
		token: root.token,
		pid,
		expires_at,
		capabilities,
	};
}

export function session_has_capability(p_session: SessionData, p_capability: GodotCapability): boolean {
	return Boolean(p_session.capabilities[p_capability]);
}
