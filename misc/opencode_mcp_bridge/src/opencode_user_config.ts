import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

export interface InstallConfigOptions {
	project_root?: string;
	bridge_script_path: string;
	config_path?: string;
	session_file?: string;
}

export interface InstallConfigResult {
	changed: boolean;
	config_path: string;
	backup_path?: string;
	entry_key: string;
}

const MCP_ENTRY_KEY = "godot-opencode-test";

function _timestamp_id(): string {
	const now = new Date();
	const pad = (p_value: number): string => String(p_value).padStart(2, "0");
	return `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
}

function _default_opencode_config_path(): string {
	return path.join(os.homedir(), ".config", "opencode", "opencode.json");
}

function _stable_json(p_value: unknown): string {
	return JSON.stringify(p_value);
}

async function _load_config_json(p_config_path: string): Promise<Record<string, unknown>> {
	try {
		const raw = await fs.readFile(p_config_path, "utf8");
		const parsed: unknown = JSON.parse(raw);
		if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
			throw new Error("Root JSON value must be an object.");
		}
		return parsed as Record<string, unknown>;
	} catch (error: unknown) {
		if (error && typeof error === "object" && "code" in error && (error as { code?: string }).code === "ENOENT") {
			return {};
		}
		throw new Error(`Failed reading OpenCode config: ${String(error)}`);
	}
}

export async function install_opencode_config(p_options: InstallConfigOptions): Promise<InstallConfigResult> {
	const config_path = path.resolve(p_options.config_path ?? _default_opencode_config_path());
	const config_dir = path.dirname(config_path);

	await fs.mkdir(config_dir, { recursive: true });

	const current = await _load_config_json(config_path);
	const next: Record<string, unknown> = JSON.parse(JSON.stringify(current));

	if (!next.mcp || typeof next.mcp !== "object" || Array.isArray(next.mcp)) {
		next.mcp = {};
	}

	const command = ["node", path.resolve(p_options.bridge_script_path)];
	if (p_options.project_root) {
		command.push("--project-root", path.resolve(p_options.project_root));
	}
	if (p_options.session_file) {
		command.push("--session-file", path.resolve(p_options.session_file));
	}

	const desired_entry: Record<string, unknown> = {
		type: "local",
		command,
		enabled: true,
	};

	const mcp = next.mcp as Record<string, unknown>;
	const previous_entry = mcp[MCP_ENTRY_KEY];
	if (_stable_json(previous_entry) === _stable_json(desired_entry)) {
		return {
			changed: false,
			config_path,
			entry_key: MCP_ENTRY_KEY,
		};
	}
	mcp[MCP_ENTRY_KEY] = desired_entry;

	let backup_path: string | undefined;
	try {
		await fs.access(config_path);
		backup_path = `${config_path}.bak-${_timestamp_id()}`;
		await fs.copyFile(config_path, backup_path);
	} catch (error: unknown) {
		if (error && typeof error === "object" && "code" in error && (error as { code?: string }).code === "ENOENT") {
			backup_path = undefined;
		} else {
			throw error;
		}
	}

	const tmp_path = `${config_path}.tmp-${process.pid}-${Date.now()}`;
	const serialized = `${JSON.stringify(next, null, 2)}\n`;

	try {
		await fs.writeFile(tmp_path, serialized, "utf8");
		JSON.parse(await fs.readFile(tmp_path, "utf8"));

		try {
			await fs.rename(tmp_path, config_path);
		} catch {
			await fs.rm(config_path, { force: true });
			await fs.rename(tmp_path, config_path);
		}

		JSON.parse(await fs.readFile(config_path, "utf8"));
	} catch (error: unknown) {
		await fs.rm(tmp_path, { force: true });
		if (backup_path) {
			await fs.copyFile(backup_path, config_path);
		}
		throw new Error(`Failed updating OpenCode config: ${String(error)}`);
	}

	return {
		changed: true,
		config_path,
		backup_path,
		entry_key: MCP_ENTRY_KEY,
	};
}
