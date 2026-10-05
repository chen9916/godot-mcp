import fs from "node:fs/promises";
import path from "node:path";
import { createInterface } from "node:readline/promises";

import { bridge_command } from "./connection_config.js";
import {
	CLIENT_IDS,
	config_snippet,
	read_client_registration,
	resolve_client_config,
	update_client_config,
	type ClientConfigOptions,
	type ClientId,
} from "./client_config.js";
import { diagnose_connection, type DoctorReport, type LaunchCommand } from "./setup_doctor.js";
import { read_config_file, write_config_file } from "./setup_storage.js";

interface SetupOptions extends ClientConfigOptions {
	clients: ClientId[];
	session_file?: string;
	replace: boolean;
	dry_run: boolean;
	json: boolean;
	no_check: boolean;
	help: boolean;
	timeout_ms: number;
}

const LABELS: Record<ClientId, string> = {
	codex: "Codex",
	opencode: "OpenCode",
	hermes: "Hermes Agent",
	antigravity: "Antigravity",
	"claude-code": "Claude Code",
	"claude-desktop": "Claude Desktop",
};

const RELOAD: Record<ClientId, string> = {
	codex: "Restart the Codex session/app; check codex mcp list or /mcp.",
	opencode: "Reload/reopen OpenCode; check opencode mcp list. OpenCode V2 also supports opencode reload.",
	hermes: "Run /reload-mcp in Hermes, or restart it; check hermes mcp test godot (use your chosen server name).",
	antigravity: "Refresh Installed MCP Servers in Antigravity, or reload through its /mcp manager.",
	"claude-code": "Restart the Claude Code session; check claude mcp list or /mcp. Project scope may need approval inside Claude.",
	"claude-desktop": "Restart Claude Desktop; check its Developer settings and MCP logs.",
};

function _parse_options(p_args: string[]): SetupOptions {
	const result: SetupOptions = { clients: [], replace: false, dry_run: false, json: false, no_check: false, help: false, timeout_ms: 10_000 };
	for (let i = 0; i < p_args.length; i++) {
		const option = p_args[i];
		const value = (): string => {
			const next = p_args[++i];
			if (!next || next.startsWith("--")) { throw new Error(`${option} requires a value.`); }
			return next;
		};
		switch (option) {
			case "--client": {
				for (const client of value().split(",")) {
					if (client === "all") { result.clients.push(...CLIENT_IDS); }
					else if (client === "claude") { result.clients.push("claude-code"); }
					else if (CLIENT_IDS.includes(client as ClientId)) { result.clients.push(client as ClientId); }
					else { throw new Error(`Unknown client '${client}'. Choose ${CLIENT_IDS.join(", ")}.`); }
				}
				break;
			}
			case "--scope": {
				const scope = value();
				if (scope !== "user" && scope !== "project") { throw new Error("--scope must be user or project."); }
				result.scope = scope;
				break;
			}
			case "--opencode-version": {
				const version = value();
				if (version !== "1" && version !== "2") { throw new Error("--opencode-version must be 1 or 2."); }
				result.opencode_version = version;
				break;
			}
			case "--project-root": result.project_root = value(); break;
			case "--session-file": result.session_file = value(); break;
			case "--config": result.config_path = value(); break;
			case "--name": result.name = value(); break;
			case "--timeout-ms": {
				const raw = value();
				if (!/^\d+$/.test(raw) || Number(raw) < 100 || Number(raw) > 120_000) { throw new Error("--timeout-ms must be an integer between 100 and 120000."); }
				result.timeout_ms = Number(raw);
				break;
			}
			case "--replace": result.replace = true; break;
			case "--dry-run": result.dry_run = true; break;
			case "--json": result.json = true; break;
			case "--no-check": result.no_check = true; break;
			case "--help": case "-h": result.help = true; break;
			default: throw new Error(`Unknown setup argument: ${option}`);
		}
	}
	result.clients = [...new Set(result.clients)];
	if (result.name && !/^[A-Za-z0-9_-]{1,64}$/.test(result.name)) { throw new Error("--name must contain 1-64 letters, digits, underscores, or hyphens."); }
	if (result.config_path && result.clients.length !== 1) { throw new Error("--config requires exactly one --client."); }
	return result;
}

function _help(): void {
	process.stdout.write([
		"Godot MCP: shared setup for MCP clients",
		"",
		"godot-mcp setup [--client <id>]         Select clients interactively, or register named clients.",
		"godot-mcp config [--client <id>]        Print client config, or generic command/args JSON.",
		"godot-mcp doctor [--client <id>]        Check the launch registered in a client, or this bridge.",
		"godot-mcp remove --client <id>          Remove this bridge's named registration.",
		"godot-mcp clients                      List supported clients and resolved config files.",
		"godot-mcp serve [server options]       Serve MCP over stdio (also the default).",
		"",
		`Clients: ${CLIENT_IDS.join(", ")}. 'claude' is an alias for claude-code.`,
		"Repeat --client or use a comma-separated list; --client all explicitly selects every client.",
		"--project-root <path> pins a Godot project; --session-file <path> pins its session metadata.",
		"--scope user|project selects config scope (default user). Project scope needs --project-root.",
		"--config <file> overrides the target file for one client. --name <id> defaults to godot.",
		"--opencode-version 1|2 selects a schema for new OpenCode config; existing V2 configs are detected.",
		"--dry-run previews setup/removal without writing. --replace permits replacing a different registration.",
		"--no-check registers without a connection test. --timeout-ms <ms> bounds diagnostics (default 10000).",
		"--json emits structured setup/doctor results. Setup creates backups before changing existing files.",
		"",
		"Legacy --print-config mcp|connection and server flags remain supported.",
		"",
	].join("\n"));
}

async function _clients(p_options: SetupOptions): Promise<Array<Record<string, unknown>>> {
	return await Promise.all(CLIENT_IDS.map(async (client) => {
		try {
			const config_path = resolve_client_config(client, { ...p_options, config_path: undefined });
			const config_exists = await fs.access(config_path).then(() => true, () => false);
			return { client, label: LABELS[client], config_path, config_exists };
		} catch (error: unknown) {
			return { client, label: LABELS[client], error: error instanceof Error ? error.message : String(error) };
		}
	}));
}

async function _choose_clients(p_options: SetupOptions): Promise<void> {
	if (!process.stdin.isTTY || !process.stdout.isTTY) { throw new Error("Choose a client with --client. Run 'godot-mcp clients' for targets, or 'godot-mcp setup' in an interactive terminal."); }
	const available = await _clients(p_options);
	available.forEach((entry, index) => process.stdout.write(`${index + 1}. ${entry.label} (${entry.config_exists ? "configuration found" : "configuration not found"})${entry.error ? `: ${entry.error}` : ""}\n`));
	const prompt = createInterface({ input: process.stdin, output: process.stdout });
	try {
		const answer = await prompt.question("Select client numbers separated by commas (blank cancels): ");
		if (!answer.trim()) { return; }
		for (const raw of answer.split(",")) {
			const index = Number(raw.trim()) - 1;
			if (!Number.isInteger(index) || index < 0 || index >= CLIENT_IDS.length) { throw new Error("Choose numbers from the supported-client list."); }
			p_options.clients.push(CLIENT_IDS[index]);
		}
		p_options.clients = [...new Set(p_options.clients)];
	} finally { prompt.close(); }
}

function _same_launch(p_a: LaunchCommand | undefined, p_b: LaunchCommand): boolean {
	return !!p_a && p_a.command === p_b.command && JSON.stringify(p_a.args) === JSON.stringify(p_b.args);
}

function _print_report(p_report: DoctorReport): void {
	process.stdout.write(`MCP server: ${p_report.bridge.ok ? "OK" : "FAILED"} — ${p_report.bridge.message}\n`);
	process.stdout.write(`Godot editor: ${p_report.editor.ok ? "OK" : "FAILED"} — ${p_report.editor.message}\n`);
}

export async function run_setup_cli(p_args: string[], p_bridge_script: string): Promise<void> {
	const command = p_args[0];
	const options = _parse_options(p_args.slice(1));
	if (options.help) { _help(); return; }
	const launch = bridge_command({ ...options, bridge_script_path: p_bridge_script });
	if (command === "clients") {
		const targets = await _clients(options);
		if (options.json) { process.stdout.write(JSON.stringify(targets, null, 2) + "\n"); }
		else { for (const target of targets) { process.stdout.write(`${target.client}: ${target.config_path ?? target.error} (${target.config_exists ? "configuration found" : "not configured"})\n`); } }
		return;
	}
	if (command === "config") {
		if (options.clients.length > 1) { throw new Error("config accepts one --client; omit it for generic command/args JSON."); }
		process.stdout.write(options.clients.length ? config_snippet(options.clients[0], launch, options) : JSON.stringify(launch, null, 2) + "\n");
		return;
	}
	if (command === "setup" && options.project_root) {
		const project_file = path.resolve(options.project_root, "project.godot");
		const valid = await fs.stat(project_file).then((stat) => stat.isFile(), () => false);
		if (!valid) { throw new Error(`The selected Godot project has no project.godot: ${project_file}. Select the game project folder, or omit --project-root for automatic discovery.`); }
	}
	if (command === "setup" && !options.clients.length) { await _choose_clients(options); if (!options.clients.length) { return; } }
	if (command === "remove" && !options.clients.length) { throw new Error("remove requires --client."); }
	if (command === "doctor" && !options.clients.length) {
		const report = await diagnose_connection(launch, options.timeout_ms);
		if (options.json) { process.stdout.write(JSON.stringify(report, null, 2) + "\n"); } else { _print_report(report); }
		if (!report.ok) { process.exitCode = 2; }
		return;
	}
	const results: Array<Record<string, unknown>> = [];
	let bridge_report: DoctorReport | undefined;
	for (const client of options.clients) {
		let config_path: string | undefined;
		try {
			config_path = resolve_client_config(client, options);
			const original = await read_config_file(config_path);
			const existing = read_client_registration(client, original.text, options);
			if (command === "doctor") {
				if (!existing.launch) { throw new Error(`No usable '${options.name ?? "godot"}' stdio registration in ${config_path}. Run setup for this client.`); }
				if (existing.enabled === false) { throw new Error(`The '${options.name ?? "godot"}' registration is disabled in ${config_path}. Run setup to enable it.`); }
				const report = await diagnose_connection(existing.launch, options.timeout_ms);
				results.push({ client, config_path, registered: true, ...report, reload: RELOAD[client] });
				if (!options.json) { process.stdout.write(`${LABELS[client]}: ${config_path}\n`); _print_report(report); }
				if (!report.ok && process.exitCode !== 1) { process.exitCode = 2; }
				continue;
			}
			if (existing.present && !options.replace) {
				const owns_entry = command === "remove"
					? existing.launch?.command === launch.command && existing.launch.args[0] === launch.args[0]
					: _same_launch(existing.launch, launch);
				if (!owns_entry) { throw new Error(`'${options.name ?? "godot"}' already points to another launch. Use --replace to change it, or choose --name.`); }
			}
			const updated = update_client_config(client, original.text, command === "remove" ? null : launch, options);
			if (command === "setup" && !options.no_check && !options.dry_run) {
				bridge_report ??= await diagnose_connection(launch, options.timeout_ms);
				if (!bridge_report.bridge.ok) { throw new Error(`MCP server check failed; configuration was not changed: ${bridge_report.bridge.message}`); }
			}
			const saved = updated.changed && !options.dry_run ? await write_config_file(config_path, original, updated.text) : {};
			let registered_report = bridge_report;
			if (command === "setup" && !options.no_check && !options.dry_run) {
				const confirmed = read_client_registration(client, (await read_config_file(config_path)).text, options);
				if (!_same_launch(confirmed.launch, launch) || confirmed.enabled === false) { throw new Error(`Could not verify the saved registration in ${config_path}.`); }
				if (confirmed.launch?.env || confirmed.launch?.cwd) {
					registered_report = await diagnose_connection(confirmed.launch, options.timeout_ms);
				}
			}
			const result = {
				client, config_path, action: command, changed: updated.changed, dry_run: options.dry_run,
				registered: command === "setup" && !options.dry_run,
				...saved, ...(command === "setup" ? { connection: launch, checks: registered_report } : {}),
				reload: RELOAD[client],
			};
			results.push(result);
			if (!options.json) {
				process.stdout.write(`${LABELS[client]}: ${options.dry_run ? "preview" : updated.changed ? command === "remove" ? "removed" : "registered" : "unchanged"} — ${config_path}\n`);
				if ("backup" in saved) { process.stdout.write(`Backup: ${saved.backup}\n`); }
				process.stdout.write(RELOAD[client] + "\n");
				if (command === "setup" && registered_report) { _print_report(registered_report); }
			}
			if (command === "setup" && registered_report && !registered_report.ok && process.exitCode !== 1) { process.exitCode = 2; }
		} catch (error: unknown) {
			const message = error instanceof Error ? error.message : String(error);
			results.push({ client, config_path, error: message });
			if (!options.json) { process.stderr.write(`${LABELS[client]}: ${message}\n`); }
			process.exitCode = 1;
		}
	}
	if (options.json) { process.stdout.write(JSON.stringify(results, null, 2) + "\n"); }
}
