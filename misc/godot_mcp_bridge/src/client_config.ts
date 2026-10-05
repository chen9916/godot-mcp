import { existsSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { isDeepStrictEqual } from "node:util";
import { applyEdits, modify, parse as parse_json, parseTree, ParseErrorCode, type ParseError, type Node as JsonNode } from "jsonc-parser";
import { parse as parse_toml, stringify as stringify_toml } from "smol-toml";
import { isMap, parseDocument } from "yaml";

export const CLIENT_IDS = ["codex", "opencode", "hermes", "antigravity", "claude-code", "claude-desktop"] as const;
export type ClientId = typeof CLIENT_IDS[number];
export interface StdioLaunch { command: string; args: string[]; env?: Record<string, string>; cwd?: string; }
export interface ClientConfigOptions {
	name?: string;
	scope?: "user" | "project";
	project_root?: string;
	config_path?: string;
	home?: string;
	platform?: NodeJS.Platform;
	env?: NodeJS.ProcessEnv;
	opencode_version?: "1" | "2";
}

function config_name(options: ClientConfigOptions): string {
	const name = options.name ?? "godot";
	if (!name || /[\r\n\0]/u.test(name)) throw new Error("MCP server name must be nonempty and contain no line breaks or NUL characters.");
	return name;
}
function object(value: unknown): value is Record<string, any> {
	return typeof value === "object" && value !== null && !Array.isArray(value);
}
function project_directory(options: ClientConfigOptions): string {
	if (!options.project_root) throw new Error("Project scope requires a project_root.");
	return options.project_root;
}
export function resolve_client_config(client: ClientId, options: ClientConfigOptions = {}): string {
	const platform = options.platform ?? process.platform;
	const paths = platform === "win32" ? path.win32 : path.posix;
	const home = options.home ?? os.homedir();
	const env = options.env ?? process.env;
	if (options.config_path) return paths.resolve(options.config_path);
	if (client === "opencode" && env.OPENCODE_CONFIG_CONTENT) {
		throw new Error("OPENCODE_CONFIG_CONTENT supplies an in-memory configuration. Specify --config explicitly to choose a file to edit.");
	}
	if (client === "opencode" && env.OPENCODE_CONFIG_DIR) throw new Error("OPENCODE_CONFIG_DIR supplies an additional configuration directory. Specify --config explicitly to select the file to edit.");
	let result: string;
	if (options.scope === "project") {
		const root = project_directory(options);
		switch (client) {
			case "codex": result = paths.join(root, ".codex", "config.toml"); break;
			case "claude-code": result = paths.join(root, ".mcp.json"); break;
			case "antigravity": result = paths.join(root, ".agents", "mcp_config.json"); break;
			case "opencode": result = paths.join(root, "opencode.json"); break;
			default: throw new Error(`${client} has no supported project configuration path; use user scope or an explicit --config.`);
		}
	} else {
		switch (client) {
			case "codex": result = paths.join(env.CODEX_HOME || paths.join(home, ".codex"), "config.toml"); break;
			case "claude-code":
				if (env.CLAUDE_CONFIG_DIR) throw new Error("CLAUDE_CONFIG_DIR is set; specify --config explicitly to select the Claude Code MCP configuration file.");
				result = paths.join(home, ".claude.json"); break;
			case "claude-desktop":
				if (platform === "win32") {
					if (!env.APPDATA) throw new Error("APPDATA is unavailable; specify --config for Claude Desktop.");
					result = paths.join(env.APPDATA, "Claude", "claude_desktop_config.json");
				} else if (platform === "darwin") result = paths.join(home, "Library", "Application Support", "Claude", "claude_desktop_config.json");
				else throw new Error("Claude Desktop configuration on this platform requires an explicit --config path.");
				break;
			case "hermes":
				if (env.HERMES_HOME) result = paths.join(env.HERMES_HOME, "config.yaml");
				else if (platform === "win32") {
					if (!env.LOCALAPPDATA) throw new Error("LOCALAPPDATA is unavailable; specify --config or HERMES_HOME for Hermes.");
					result = paths.join(env.LOCALAPPDATA, "hermes", "config.yaml");
				} else result = paths.join(home, ".hermes", "config.yaml");
				break;
			case "antigravity": result = paths.join(home, ".gemini", "config", "mcp_config.json"); break;
			case "opencode": result = env.OPENCODE_CONFIG || paths.join(env.XDG_CONFIG_HOME || paths.join(home, ".config"), "opencode", "opencode.json"); break;
		}
	}
	if (client === "opencode" && (options.scope === "project" || !env.OPENCODE_CONFIG) && !existsSync(result) && existsSync(`${result}c`)) result += "c";
	return result;
}

function json_document(text: string): Record<string, any> {
	text = text.replace(/^\uFEFF/u, "");
	if (!text.trim()) return {};
	const errors: ParseError[] = [];
	const value: unknown = parse_json(text, errors, { allowTrailingComma: true });
	if (value === undefined && errors.length === 1 && errors[0].error === ParseErrorCode.ValueExpected && errors[0].offset === text.length) return {};
	if (errors.length || !object(value)) throw new Error("Invalid MCP configuration: expected a JSON/JSONC object.");
	function check_duplicates(node: JsonNode | undefined): void {
		if (!node) return;
		if (node.type === "object") {
			const keys = new Set<string>();
			for (const property of node.children ?? []) {
				const key = property.children?.[0].value as string;
				if (keys.has(key)) throw new Error(`Ambiguous MCP configuration: duplicate JSON key ${JSON.stringify(key)}.`);
				keys.add(key);
			}
		}
		for (const child of node.children ?? []) check_duplicates(child);
	}
	check_duplicates(parseTree(text, [], { allowTrailingComma: true }));
	return value;
}
function json_entry_path(client: ClientId, value: Record<string, any>, options: ClientConfigOptions): string[] {
	if (client !== "opencode") return ["mcpServers", config_name(options)];
	const servers_entry = object(value.mcp) && object(value.mcp.servers) && (value.mcp.servers.type === "local" || value.mcp.servers.type === "remote");
	const v2 = options.opencode_version === "2" || (options.opencode_version !== "1" && object(value.mcp) && Object.hasOwn(value.mcp, "servers") && !servers_entry);
	return v2 ? ["mcp", "servers", config_name(options)] : ["mcp", config_name(options)];
}
function validate_containers(value: Record<string, any>, keys: string[]): void {
	let current: unknown = value;
	for (const key of keys.slice(0, -1)) {
		if (!object(current)) throw new Error("Invalid MCP configuration: server container must be an object.");
		if (!Object.hasOwn(current, key)) return;
		current = current[key];
		if (!object(current)) throw new Error(`Invalid MCP configuration: ${key} must be an object.`);
	}
}
function entry_at(value: Record<string, any>, keys: string[]): unknown {
	let current: any = value;
	for (const key of keys) {
		if (!object(current) || !Object.hasOwn(current, key)) return undefined;
		current = current[key];
	}
	return current;
}
function launch_from_entry(client: ClientId, entry: unknown): StdioLaunch | undefined {
	if (!object(entry)) return undefined;
	let launch: StdioLaunch;
	if (client === "opencode") {
		if (entry.type !== "local" || !Array.isArray(entry.command) || !entry.command.length || !entry.command.every((arg: unknown) => typeof arg === "string") || !entry.command[0]) return undefined;
		launch = { command: entry.command[0], args: entry.command.slice(1) };
	} else {
		if ((entry.type !== undefined && entry.type !== "stdio") || entry.url !== undefined || typeof entry.command !== "string" || !entry.command || (entry.args !== undefined && (!Array.isArray(entry.args) || !entry.args.every((arg: unknown) => typeof arg === "string")))) return undefined;
		launch = { command: entry.command, args: entry.args ?? [] };
	}
	const environment = client === "opencode" ? entry.environment : entry.env;
	if (environment !== undefined) {
		if (!object(environment) || !Object.values(environment).every(value => typeof value === "string")) throw new Error("Invalid MCP stdio environment: expected an object containing string values.");
		launch.env = { ...environment };
	}
	if (entry.cwd !== undefined) {
		if (typeof entry.cwd !== "string" || !entry.cwd) throw new Error("Invalid MCP stdio cwd: expected a nonempty string.");
		launch.cwd = entry.cwd;
	}
	return launch;
}
function yaml_document(text: string) {
	let document = parseDocument(text || "{}\n", { keepSourceTokens: true });
	if (document.errors.length || (document.contents !== null && !isMap(document.contents))) throw new Error("Invalid MCP configuration: expected a YAML mapping without duplicate keys.");
	if (!document.contents) document = parseDocument(`${text}\n{}\n`, { keepSourceTokens: true });
	const servers = document.get("mcp_servers", true);
	if (servers !== undefined && !isMap(servers)) throw new Error("Invalid MCP configuration: mcp_servers must be a YAML mapping.");
	return document;
}
function toml_document(text: string): Record<string, any> {
	let value: unknown;
	try { value = parse_toml(text); } catch (error) { throw new Error(`Invalid TOML MCP configuration: ${(error as Error).message}`); }
	if (!object(value) || (Object.hasOwn(value, "mcp_servers") && !object(value.mcp_servers))) throw new Error("Invalid TOML MCP configuration: mcp_servers must be a table.");
	return value;
}

export function read_client_registration(client: ClientId, text: string, options: ClientConfigOptions = {}): { present: boolean; enabled: boolean; launch?: StdioLaunch } {
	const name = config_name(options);
	let entry: unknown;
	let present = false;
	if (client === "codex") {
		const servers = toml_document(text).mcp_servers;
		present = !!servers && Object.hasOwn(servers, name);
		entry = servers?.[name];
	} else if (client === "hermes") {
		const document = yaml_document(text);
		present = document.hasIn(["mcp_servers", name]);
		entry = document.getIn(["mcp_servers", name]);
		if (entry && typeof (entry as any).toJSON === "function") entry = (entry as any).toJSON();
	} else {
		const value = json_document(text);
		const keys = json_entry_path(client, value, options);
		validate_containers(value, keys);
		const container = entry_at(value, keys.slice(0, -1));
		present = object(container) && Object.hasOwn(container, name);
		entry = entry_at(value, keys);
	}
	const launch = launch_from_entry(client, entry);
	const enabled = present && (!object(entry) || (entry.enabled !== false && entry.disabled !== true));
	return launch ? { present, enabled, launch } : { present, enabled };
}

interface TableSection { start: number; end: number; keys: string[]; }
function toml_sections(text: string): TableSection[] {
	const sections: TableSection[] = [];
	let offset = 0;
	let multiline: '"""' | "'''" | undefined;
	for (const line of text.match(/[^\n]*\n|[^\n]+$/gu) ?? []) {
		if (!multiline && /^\s*\[/u.test(line)) {
			const parsed = parse_toml(`${line}\n__godot_mcp_marker = true\n`) as Record<string, any>;
			const keys: string[] = [];
			let node: any = parsed;
			while (object(node) && !Object.hasOwn(node, "__godot_mcp_marker")) {
				const key = Object.keys(node)[0];
				keys.push(key);
				node = node[key];
			}
			if (sections.length) sections[sections.length - 1].end = offset;
			sections.push({ start: offset, end: text.length, keys });
		}
		// Track multiline strings so apparent headers inside string values are never edited.
		let quote: '"' | "'" | undefined;
		for (let index = 0; index < line.length; index++) {
			if (multiline) {
				if (line.slice(index, index + 3) === multiline && (multiline === "'''" || line[index - 1] !== "\\")) { multiline = undefined; index += 2; }
				continue;
			}
			const character = line[index];
			if (quote) {
				if (quote === '"' && character === "\\") { index++; continue; }
				if (character === quote) quote = undefined;
				continue;
			}
			if (character === "#") break;
			if (line.slice(index, index + 3) === '"""' || line.slice(index, index + 3) === "'''") { multiline = line.slice(index, index + 3) as '"""' | "'''"; index += 2; }
			else if (character === '"' || character === "'") quote = character;
		}
		offset += line.length;
	}
	return sections;
}
function unrelated_configuration(value: Record<string, any>, keys: string[]): unknown {
	const copy = structuredClone(value);
	const parents: Array<[Record<string, any>, string]> = [];
	let current = copy;
	for (const key of keys.slice(0, -1)) {
		if (!object(current[key])) return copy;
		parents.push([current, key]);
		current = current[key];
	}
	delete current[keys[keys.length - 1]];
	for (const [parent, key] of parents.reverse()) if (Object.keys(parent[key]).length === 0) delete parent[key];
	return copy;
}
function verify_unrelated(before: Record<string, any>, after: Record<string, any>, keys: string[]): void {
	if (!isDeepStrictEqual(unrelated_configuration(before, keys), unrelated_configuration(after, keys))) throw new Error("Cannot safely edit this MCP configuration: the change would affect unrelated settings.");
}
function codex_update(text: string, entry: Record<string, any> | null, options: ClientConfigOptions): string {
	const value = toml_document(text);
	const name = config_name(options);
	const sections = toml_sections(text);
	const target = sections.filter(section => section.keys[0] === "mcp_servers" && section.keys[1] === name);
	if (value.mcp_servers && Object.hasOwn(value.mcp_servers, name) && !target.some(section => section.keys.length === 2)) {
		throw new Error("Cannot safely edit this Codex registration: inline or dotted MCP entries require manual conversion to [mcp_servers.NAME] tables.");
	}
	// An inline parent table cannot be extended using a new table header.
	if (value.mcp_servers && !target.length && !sections.some(section => section.keys[0] === "mcp_servers")) {
		throw new Error("Cannot safely extend an inline or dotted mcp_servers table; convert it to ordinary TOML table headers first.");
	}
	let result = text;
	for (const section of [...target].reverse()) {
		const comments = (result.slice(section.start, section.end).match(/^[ \t]*#[^\n]*(?:\n|$)/gmu) ?? []).join("");
		result = result.slice(0, section.start) + comments + result.slice(section.end);
	}
	if (entry) {
		const newline = text.includes("\r\n") ? "\r\n" : "\n";
		const table = stringify_toml({ mcp_servers: { [name]: entry } }).replace(/\n/gu, newline);
		result += (result && !result.endsWith("\n") ? newline : "") + (result && !result.endsWith(`${newline}${newline}`) ? newline : "") + table;
	}
	verify_unrelated(value, toml_document(result), ["mcp_servers", name]);
	return result;
}
export function update_client_config(client: ClientId, text: string, launch: StdioLaunch | null, options: ClientConfigOptions = {}): { text: string; changed: boolean } {
	if (launch && (!launch.command || typeof launch.command !== "string" || !Array.isArray(launch.args) || !launch.args.every(arg => typeof arg === "string"))) throw new Error("Invalid stdio launch command or arguments.");
	if (launch?.env !== undefined && (!object(launch.env) || !Object.values(launch.env).every(value => typeof value === "string"))) throw new Error("Invalid MCP stdio environment: expected an object containing string values.");
	if (launch?.cwd !== undefined && (typeof launch.cwd !== "string" || !launch.cwd)) throw new Error("Invalid MCP stdio cwd: expected a nonempty string.");
	const registration = read_client_registration(client, text, options);
	if (!launch && !registration.present) return { text, changed: false };
	const keys = client === "codex" || client === "hermes" ? ["mcp_servers", config_name(options)] : json_entry_path(client, json_document(text), options);
	const value = client === "codex" ? toml_document(text) : client === "hermes" ? yaml_document(text).toJS() as Record<string, any> : json_document(text);
	const existing = entry_at(value, keys);
	let entry: Record<string, any> | null = null;
	if (launch) {
		const matching = registration.launch && registration.launch.command === launch.command && isDeepStrictEqual(registration.launch.args, launch.args);
		entry = matching && object(existing) ? { ...existing } : client === "opencode" ? { type: "local", command: [launch.command, ...launch.args], ...(keys.length === 3 ? { disabled: false } : { enabled: true }) } : { command: launch.command, args: launch.args };
		if (launch.env !== undefined) entry[client === "opencode" ? "environment" : "env"] = launch.env;
		if (launch.cwd !== undefined) entry.cwd = launch.cwd;
		if (matching && !registration.enabled) {
			if (client === "opencode") {
				delete entry.enabled;
				delete entry.disabled;
				entry[keys.length === 3 ? "disabled" : "enabled"] = keys.length !== 3;
			} else if (client === "codex") { delete entry.disabled; entry.enabled = true; }
			else { delete entry.enabled; delete entry.disabled; }
		}
		if (isDeepStrictEqual(structuredClone(existing), structuredClone(entry))) return { text, changed: false };
	}
	let result: string;
	if (client === "codex") result = codex_update(text, entry, options);
	else if (client === "hermes") {
		const document = yaml_document(text);
		if (entry) document.setIn(keys, entry);
		else document.deleteIn(keys);
		result = document.toString();
		verify_unrelated(value, yaml_document(result).toJS() as Record<string, any>, keys);
	} else {
		let base = text.replace(/^\uFEFF/u, "");
		if (!parseTree(base, [], { allowTrailingComma: true })) base += `${base && !base.endsWith("\n") ? "\n" : ""}{}\n`;
		result = (text.startsWith("\uFEFF") ? "\uFEFF" : "") + applyEdits(base, modify(base, keys, entry || undefined, { formattingOptions: { insertSpaces: true, tabSize: 2, eol: text.includes("\r\n") ? "\r\n" : "\n" } }));
		verify_unrelated(value, json_document(result), keys);
	}
	return { text: result, changed: result !== text };
}
export function config_snippet(client: ClientId, launch: StdioLaunch, options: ClientConfigOptions = {}): string {
	return update_client_config(client, "", launch, options).text;
}
