import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import type { GodotCapability } from "./tool_registry.js";

export interface SessionCapabilities {
	read_scene?: boolean;
	write_scene?: boolean;
	read_script?: boolean;
	write_script?: boolean;
	save_resource?: boolean;
	read_resource?: boolean;
	write_resource?: boolean;
	read_project?: boolean;
	write_project?: boolean;
	read_docs?: boolean;
	script_validation?: boolean;
	filesystem_sync?: boolean;
	resource_versioning?: boolean;
	runtime_control?: boolean;
	runtime_inspection?: boolean;
	viewport_capture?: boolean;
	diagnostics_v2?: boolean;
	partial_batch_results?: boolean;
	[key: string]: unknown;
}

export interface SessionData {
	session_id?: string;
	protocol_version?: number;
	port: number;
	token?: string;
	pid: number;
	project_path?: string;
	started_at?: number;
	expires_at?: number;
	capabilities: SessionCapabilities;
}

export interface SessionLoadOptions {
	project_root?: string;
	session_file?: string;
	now_unix?: number;
}

export type SessionErrorCode =
	| "SESSION_NOT_FOUND"
	| "SESSION_INVALID"
	| "SESSION_EXPIRED"
	| "SESSION_PROCESS_EXITED";

export class SessionError extends Error {
	readonly code: SessionErrorCode;
	readonly retryable: boolean;
	readonly session_file?: string;

	constructor(p_code: SessionErrorCode, p_message: string, p_retryable: boolean, p_session_file?: string) {
		super(p_message);
		this.name = "SessionError";
		this.code = p_code;
		this.retryable = p_retryable;
		this.session_file = p_session_file;
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
		throw new SessionError("SESSION_INVALID", `${p_name} must be an object.`, true);
	}
	return p_value as Record<string, unknown>;
}

function _to_integer(p_value: unknown, p_name: string): number {
	if (typeof p_value !== "number" || !Number.isInteger(p_value)) {
		throw new SessionError("SESSION_INVALID", `${p_name} must be an integer.`, true);
	}
	return p_value;
}

function _to_unix_seconds(p_value: unknown, p_name: string): number {
	if (typeof p_value !== "number" || !Number.isFinite(p_value)) {
		throw new SessionError("SESSION_INVALID", `${p_name} must be a number.`, true);
	}
	if (p_value <= 0) {
		throw new SessionError("SESSION_INVALID", `${p_name} must be greater than 0.`, true);
	}

	// Godot may serialize unix time with sub-second precision.
	return Math.floor(p_value);
}

function _get_godot_projects_cfg_candidates(): string[] {
	const candidates: string[] = [];
	if (process.platform === "win32") {
		const appdata = process.env.APPDATA;
		if (appdata) {
			candidates.push(path.join(appdata, "Godot", "projects.cfg"));
		}
	} else if (process.platform === "darwin") {
		candidates.push(path.join(os.homedir(), "Library", "Application Support", "Godot", "projects.cfg"));
	} else {
		candidates.push(path.join(os.homedir(), ".config", "godot", "projects.cfg"));
	}

	return candidates;
}

function _read_recent_project_roots(): string[] {
	for (const cfg_path of _get_godot_projects_cfg_candidates()) {
		if (!fs.existsSync(cfg_path)) {
			continue;
		}

		let content = "";
		try {
			content = fs.readFileSync(cfg_path, "utf8");
		} catch {
			continue;
		}

		const roots: string[] = [];
		for (const raw_line of content.split(/\r?\n/)) {
			const line = raw_line.trim();
			if (!line.startsWith("[") || !line.endsWith("]") || line.length < 3) {
				continue;
			}
			const section = line.slice(1, -1).trim();
			if (!section || !path.isAbsolute(section)) {
				continue;
			}
			roots.push(path.resolve(section));
		}

		if (roots.length > 0) {
			return roots;
		}
	}

	return [];
}

function _auto_detect_session_file_from_recent_projects(): string | null {
	const candidates: Array<{ session_path: string; mtime_ms: number }> = [];
	for (const project_root of _read_recent_project_roots()) {
		const session_path = path.join(project_root, ".godot", "godot_mcp", "session.json");
		if (!fs.existsSync(session_path)) {
			continue;
		}

		let mtime_ms = 0;
		try {
			mtime_ms = fs.statSync(session_path).mtimeMs;
		} catch {
			mtime_ms = 0;
		}

		candidates.push({ session_path, mtime_ms });
	}

	if (candidates.length === 0) {
		return null;
	}

	candidates.sort((p_a, p_b) => p_b.mtime_ms - p_a.mtime_ms);
	return candidates[0].session_path;
}

export function resolve_session_file_path(p_options: SessionLoadOptions = {}): string {
	if (p_options.session_file) {
		return path.resolve(p_options.session_file);
	}

	if (p_options.project_root) {
		const project_root = path.resolve(p_options.project_root);
		return path.join(project_root, ".godot", "godot_mcp", "session.json");
	}

	let cursor = path.resolve(process.cwd());
	while (true) {
		const candidate = path.join(cursor, ".godot", "godot_mcp", "session.json");
		if (fs.existsSync(candidate)) {
			return candidate;
		}

		const parent = path.dirname(cursor);
		if (parent === cursor) {
			break;
		}
		cursor = parent;
	}

	const auto_detected_session_file = _auto_detect_session_file_from_recent_projects();
	if (auto_detected_session_file) {
		return auto_detected_session_file;
	}

	return path.join(path.resolve(process.cwd()), ".godot", "godot_mcp", "session.json");
}

export function load_session(p_options: SessionLoadOptions = {}): SessionData {
	const session_file = resolve_session_file_path(p_options);

	if (!fs.existsSync(session_file)) {
		throw new SessionError(
			"SESSION_NOT_FOUND",
			`Session file not found at '${session_file}'. Start Godot editor and enable network/godot_mcp/enabled.`,
			true,
			session_file,
		);
	}

	let raw = "";
	try {
		raw = fs.readFileSync(session_file, "utf8");
	} catch (error: unknown) {
		throw new SessionError(
			"SESSION_INVALID",
			`Session file could not be read: '${session_file}'. ${String(error)}`,
			true,
			session_file,
		);
	}
	let decoded: unknown;
	try {
		decoded = JSON.parse(raw);
	} catch {
		throw new SessionError("SESSION_INVALID", `Session file is not valid JSON: '${session_file}'.`, true, session_file);
	}

	const root = _ensure_object(decoded, "Session payload");
	const port = _to_integer(root.port, "port");
	if (port <= 0 || port > 65535) {
		throw new SessionError("SESSION_INVALID", "Session port must be in range 1..65535.", true, session_file);
	}

	let session_id: string | undefined;
	if (root.session_id !== undefined) {
		if (typeof root.session_id !== "string" || root.session_id.length === 0) {
			throw new SessionError("SESSION_INVALID", "session_id must be a non-empty string.", true, session_file);
		}
		session_id = root.session_id;
	}

	let protocol_version: number | undefined;
	if (root.protocol_version !== undefined) {
		protocol_version = _to_integer(root.protocol_version, "protocol_version");
		if (protocol_version < 1) {
			throw new SessionError("SESSION_INVALID", "protocol_version must be >= 1.", true, session_file);
		}
	}

	let token: string | undefined;
	if (root.token !== undefined) {
		if (typeof root.token !== "string" || root.token.length === 0) {
			throw new SessionError("SESSION_INVALID", "Session token must be a non-empty string.", true, session_file);
		}
		token = root.token;
	}

	const pid = _to_integer(root.pid, "pid");
	if (!_is_process_alive(pid)) {
		throw new SessionError("SESSION_PROCESS_EXITED", "Session process is no longer running; waiting for a replacement Godot editor.", true, session_file);
	}

	const capabilities = _ensure_object(root.capabilities, "capabilities") as SessionCapabilities;

	let expires_at: number | undefined;
	if (root.expires_at !== undefined) {
		expires_at = _to_unix_seconds(root.expires_at, "expires_at");
		const now_unix = p_options.now_unix ?? _default_now_unix();
		if (expires_at <= now_unix) {
			throw new SessionError("SESSION_EXPIRED", "Session metadata is expired; waiting for Godot to refresh or replace it.", true, session_file);
		}
	}

	let project_path: string | undefined;
	if (root.project_path !== undefined) {
		if (typeof root.project_path !== "string" || root.project_path.length === 0) {
			throw new SessionError("SESSION_INVALID", "project_path must be a non-empty string.", true, session_file);
		}
		project_path = root.project_path;
	}

	let started_at: number | undefined;
	if (root.started_at !== undefined) {
		started_at = _to_unix_seconds(root.started_at, "started_at");
	}

	return {
		session_id,
		protocol_version,
		port,
		token,
		pid,
		project_path,
		started_at,
		expires_at,
		capabilities,
	};
}

export function get_session_identity(p_session: SessionData): string {
	// Protocol v2 editors always provide a UUID. The legacy fallback keeps older
	// editor builds usable, but cannot protect against PID and port reuse.
	return p_session.session_id ?? `legacy:${p_session.pid}:${p_session.port}`;
}

export function session_has_capability(p_session: SessionData, p_capability: GodotCapability): boolean {
	return Boolean(p_session.capabilities[p_capability]);
}
