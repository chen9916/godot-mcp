import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { parse as parse_json } from "jsonc-parser";
import { parse as parse_toml } from "smol-toml";
import { parse as parse_yaml } from "yaml";
import { CLIENT_IDS, config_snippet, read_client_registration, resolve_client_config, update_client_config, type ClientId, type ClientConfigOptions } from "../src/client_config.js";

const launch = { command: "C:/Users/Jane Doe/游戏/node.exe", args: ["E:/Godot MCP/桥/dist/main.js", "--project-root", "E:/Projects/My game"] };

test("every client supports empty config, round trip, idempotency, custom name and removal", () => {
	for (const client of CLIENT_IDS) {
		for (const original of ["", " \n\t", client === "codex" ? "# empty\n" : client === "hermes" ? "# empty\n" : "{}\n"]) {
			const options = { name: "godot 游戏.with-dots" };
			assert.deepEqual(read_client_registration(client, original, options), { present: false, enabled: false });
			const added = update_client_config(client, original, launch, options);
			assert.equal(added.changed, true, client);
			assert.deepEqual(read_client_registration(client, added.text, options), { present: true, enabled: true, launch }, client);
			assert.deepEqual(update_client_config(client, added.text, launch, options), { text: added.text, changed: false });
			assert.equal(read_client_registration(client, added.text).present, false);
			const removed = update_client_config(client, added.text, null, options);
			assert.equal(removed.changed, true);
			assert.deepEqual(read_client_registration(client, removed.text, options), { present: false, enabled: false });
			assert.deepEqual(update_client_config(client, removed.text, null, options), { text: removed.text, changed: false });
		}
		assert.deepEqual(read_client_registration(client, config_snippet(client, launch)), { present: true, enabled: true, launch });
	}
});

test("JSONC preserves unrelated comments, settings and server entries", () => {
	for (const client of ["claude-code", "claude-desktop", "antigravity"] as const) {
		const original = '{\n  // Keep settings\n  "theme": "dark",\n  "mcpServers": {\n    // Keep another server\n    "other": { "url": "https://example.test/mcp" },\n  },\n}\n';
		const added = update_client_config(client, original, launch);
		assert.match(added.text, /\/\/ Keep settings/);
		assert.match(added.text, /\/\/ Keep another server/);
		const parsed = parse_json(added.text);
		assert.equal(parsed.theme, "dark");
		assert.deepEqual(parsed.mcpServers.other, { url: "https://example.test/mcp" });
		const removed = update_client_config(client, added.text, null);
		assert.match(removed.text, /\/\/ Keep another server/);
		assert.deepEqual(parse_json(removed.text), { theme: "dark", mcpServers: { other: { url: "https://example.test/mcp" } } });
	}
});

test("JSONC replaces only the selected registration", () => {
	const original = '{"mcpServers":{"godot":{"url":"https://old.test/mcp"},"keep":{"command":"unchanged"}},"projects":{"elsewhere":{"mcpServers":{"godot":{"command":"project-server"}}}}}';
	assert.deepEqual(read_client_registration("claude-code", original), { present: true, enabled: true });
	const updated = update_client_config("claude-code", original, launch);
	const parsed = parse_json(updated.text);
	assert.deepEqual(parsed.mcpServers.godot, launch);
	assert.deepEqual(parsed.mcpServers.keep, { command: "unchanged" });
	assert.deepEqual(parsed.projects.elsewhere.mcpServers.godot, { command: "project-server" });
});

test("malformed, ambiguous and invalid root documents are rejected without producing a replacement", () => {
	for (const client of ["claude-code", "claude-desktop", "antigravity", "opencode"] as const) {
		for (const text of ['{"broken":', "null", "[]", '{"mcpServers": {}, "mcpServers":{}}', '{"mcp": { "same": 1, "same": 2 }}']) {
			assert.throws(() => read_client_registration(client, text));
			assert.throws(() => update_client_config(client, text, launch));
		}
	}
	for (const text of ["mcp_servers: [", "mcp_servers: []", "[]", "mcp_servers:\n  godot: 1\n  godot: 2\n"]) assert.throws(() => update_client_config("hermes", text, launch));
	for (const text of ["[bad", "mcp_servers = []", "key = 1\nkey = 2\n"]) assert.throws(() => update_client_config("codex", text, launch));
	for (const client of ["claude-code", "claude-desktop", "antigravity"] as const) {
		for (const value of [null, [], "wrong"]) assert.throws(() => update_client_config(client, JSON.stringify({ mcpServers: value }), launch));
	}
	assert.throws(() => update_client_config("opencode", '{"mcp":{"servers":[]}}', launch));
});

test("invalid and remote registrations are present but cannot be mistaken for the requested stdio launch", () => {
	for (const value of [null, {}, { command: 123 }, { command: "node", args: [123] }, { command: "node", type: "http" }, { command: "node", url: "https://example.test" }]) {
		assert.deepEqual(read_client_registration("antigravity", JSON.stringify({ mcpServers: { godot: value } })), { present: true, enabled: true });
	}
	assert.deepEqual(read_client_registration("claude-code", '{"mcpServers":{"godot":{"command":"node"}}}'), { present: true, enabled: true, launch: { command: "node", args: [] } });
	assert.deepEqual(read_client_registration("hermes", 'mcp_servers:\n  godot:\n    transport: http\n    url: https://example.test\n'), { present: true, enabled: true });
	assert.deepEqual(read_client_registration("codex", '[mcp_servers.godot]\nurl = "https://example.test"\n'), { present: true, enabled: true });
	for (const value of [{ type: "remote", url: "https://example.test" }, { type: "local", command: [] }, { type: "local", command: ["node", 123] }]) assert.deepEqual(read_client_registration("opencode", JSON.stringify({ mcp: { godot: value } })), { present: true, enabled: true });
});

test("Hermes preserves YAML settings, comments and other servers across replacement/removal", () => {
	const original = '# Keep config header\nmodel: example # Keep model comment\nmcp_servers:\n  other: # Keep other server\n    command: other\n    args: [a, b]\n  godot:\n    command: old\n    args: []\n';
	const added = update_client_config("hermes", original, launch);
	assert.match(added.text, /# Keep config header/);
	assert.match(added.text, /# Keep model comment/);
	assert.match(added.text, /# Keep other server/);
	const parsed = parse_yaml(added.text);
	assert.equal(parsed.model, "example");
	assert.deepEqual(parsed.mcp_servers.other, { command: "other", args: ["a", "b"] });
	assert.deepEqual(parsed.mcp_servers.godot, launch);
	const removed = update_client_config("hermes", added.text, null);
	assert.match(removed.text, /# Keep other server/);
	assert.deepEqual(parse_yaml(removed.text), { model: "example", mcp_servers: { other: { command: "other", args: ["a", "b"] } } });
});

test("Codex preserves unrelated TOML bytes and removes old registration subtables", () => {
	const before = '# Keep header\nmodel = "example"\n\n[mcp_servers.other]\ncommand = "other" # Keep other command\nargs = ["a", "b"]\n\n';
	const after = '[projects."E:/Projects/游戏"]\ntrust_level = "trusted"\n';
	const original = before + '[mcp_servers.godot]\ncommand = "old"\nargs = []\n\n[mcp_servers.godot.env]\nTOKEN = "old"\n\n' + after;
	const added = update_client_config("codex", original, launch);
	assert.ok(added.text.startsWith(before));
	assert.ok(added.text.includes(after));
	const parsed = JSON.parse(JSON.stringify(parse_toml(added.text)));
	assert.deepEqual(parsed.mcp_servers.godot, launch);
	assert.deepEqual(parsed.mcp_servers.other, { command: "other", args: ["a", "b"] });
	assert.equal(parsed.projects["E:/Projects/游戏"].trust_level, "trusted");
	const removed = update_client_config("codex", added.text, null);
	assert.ok(removed.text.startsWith(before));
	assert.ok(removed.text.includes(after));
	assert.equal((parse_toml(removed.text) as any).mcp_servers.godot, undefined);
});

test("Codex table-looking lines in multiline values stay untouched", () => {
	const original = 'description = """\n[mcp_servers.godot]\ncommand = not a table\n"""\n\n[mcp_servers.other]\ncommand = "other"\n';
	const added = update_client_config("codex", original, launch);
	assert.ok(added.text.startsWith(original));
	assert.deepEqual(read_client_registration("codex", added.text), { present: true, enabled: true, launch });
	const removed = update_client_config("codex", added.text, null);
	assert.ok(removed.text.startsWith(original));
});

test("Codex rejects inline/dotted layouts rather than rewriting unrelated tables", () => {
	for (const text of [
		'mcp_servers = { godot = { command = "old" } }\n',
		'mcp_servers.godot.command = "old"\n',
		'[mcp_servers]\ngodot = { command = "old" }\n',
	]) {
		assert.equal(read_client_registration("codex", text).present, true);
		assert.throws(() => update_client_config("codex", text, launch), /inline or dotted/);
		assert.throws(() => update_client_config("codex", text, null), /inline or dotted/);
	}
	assert.throws(() => update_client_config("codex", 'mcp_servers = { other = { command = "keep" } }\n', launch), /inline or dotted/);
});

test("OpenCode V1 and V2 use command arrays and preserve sibling settings", () => {
	for (const version of ["1", "2"] as const) {
		const options = { opencode_version: version };
		const original = version === "1" ? '{\n// Keep config\n"model":"example", "mcp":{"other":{"type":"remote","url":"https://example.test"}}\n}' : '{\n// Keep config\n"model":"example", "mcp":{"timeout":123,"servers":{"other":{"type":"remote","url":"https://example.test"}}}\n}';
		const added = update_client_config("opencode", original, launch, options);
		assert.match(added.text, /\/\/ Keep config/);
		const parsed = parse_json(added.text);
		const servers = version === "1" ? parsed.mcp : parsed.mcp.servers;
		assert.equal(parsed.model, "example");
		assert.deepEqual(servers.other, { type: "remote", url: "https://example.test" });
		assert.deepEqual(servers.godot, { type: "local", command: [launch.command, ...launch.args], ...(version === "1" ? { enabled: true } : { disabled: false }) });
		assert.deepEqual(read_client_registration("opencode", added.text), { present: true, enabled: true, launch });
		if (version === "2") assert.equal(parsed.mcp.timeout, 123);
		assert.equal(read_client_registration("opencode", update_client_config("opencode", added.text, null).text).present, false);
	}
});

test("user paths and environment overrides follow each supported client", () => {
	const options: ClientConfigOptions = { platform: "linux", home: "/home/Jane Doe/游戏", env: {} };
	const expected: Record<ClientId, string | null> = {
		codex: "/home/Jane Doe/游戏/.codex/config.toml",
		opencode: "/home/Jane Doe/游戏/.config/opencode/opencode.json",
		hermes: "/home/Jane Doe/游戏/.hermes/config.yaml",
		antigravity: "/home/Jane Doe/游戏/.gemini/config/mcp_config.json",
		"claude-code": "/home/Jane Doe/游戏/.claude.json",
		"claude-desktop": null,
	};
	for (const client of CLIENT_IDS) {
		if (expected[client]) assert.equal(resolve_client_config(client, options), expected[client]);
		else assert.throws(() => resolve_client_config(client, options), /explicit --config/);
	}
	assert.equal(resolve_client_config("codex", { ...options, env: { CODEX_HOME: "/custom/codex" } }), "/custom/codex/config.toml");
	assert.equal(resolve_client_config("hermes", { ...options, env: { HERMES_HOME: "/custom/hermes" } }), "/custom/hermes/config.yaml");
	assert.equal(resolve_client_config("opencode", { ...options, env: { XDG_CONFIG_HOME: "/custom/config" } }), "/custom/config/opencode/opencode.json");
	assert.equal(resolve_client_config("opencode", { ...options, env: { OPENCODE_CONFIG: "/explicit/opencode.jsonc" } }), "/explicit/opencode.jsonc");
	assert.throws(() => resolve_client_config("opencode", { ...options, env: { OPENCODE_CONFIG_CONTENT: "{}" } }), /--config/);
	assert.throws(() => resolve_client_config("claude-code", { ...options, env: { CLAUDE_CONFIG_DIR: "/custom/claude" } }), /--config/);
	assert.equal(resolve_client_config("claude-desktop", { ...options, platform: "darwin" }), "/home/Jane Doe/游戏/Library/Application Support/Claude/claude_desktop_config.json");
});

test("Windows paths use APPDATA, LOCALAPPDATA, and explicit config bypasses unsupported defaults", () => {
	const options: ClientConfigOptions = { platform: "win32", home: "C:\\Users\\Jane Doe\\游戏", env: { APPDATA: "C:\\Roaming", LOCALAPPDATA: "C:\\Local" } };
	assert.equal(resolve_client_config("claude-desktop", options), "C:\\Roaming\\Claude\\claude_desktop_config.json");
	assert.equal(resolve_client_config("hermes", options), "C:\\Local\\hermes\\config.yaml");
	assert.equal(resolve_client_config("antigravity", options), "C:\\Users\\Jane Doe\\游戏\\.gemini\\config\\mcp_config.json");
	assert.throws(() => resolve_client_config("claude-desktop", { ...options, env: {} }), /APPDATA/);
	assert.throws(() => resolve_client_config("hermes", { ...options, env: {} }), /LOCALAPPDATA/);
	for (const client of CLIENT_IDS) assert.equal(resolve_client_config(client, { ...options, scope: "project", config_path: "D:\\Selected config\\游戏.json" }), "D:\\Selected config\\游戏.json");
});

test("project scope resolves documented paths and refuses unsupported clients", () => {
	const options: ClientConfigOptions = { platform: "linux", home: "/home/test", project_root: "/projects/My game 游戏", scope: "project", env: {} };
	assert.equal(resolve_client_config("codex", options), "/projects/My game 游戏/.codex/config.toml");
	assert.equal(resolve_client_config("opencode", options), "/projects/My game 游戏/opencode.json");
	assert.equal(resolve_client_config("antigravity", options), "/projects/My game 游戏/.agents/mcp_config.json");
	assert.equal(resolve_client_config("claude-code", options), "/projects/My game 游戏/.mcp.json");
	for (const client of ["hermes", "claude-desktop"] as const) assert.throws(() => resolve_client_config(client, options), /no supported project/);
	assert.throws(() => resolve_client_config("codex", { ...options, project_root: undefined }), /requires a project_root/);
});

test("OpenCode picks existing JSONC for isolated user and project fixtures without modifying files", async t => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "Godot config 游戏 "));
	t.after(() => fs.rm(directory, { recursive: true, force: true }));
	const options: ClientConfigOptions = { platform: process.platform, home: directory, env: {} };
	const config = resolve_client_config("opencode", options);
	await fs.mkdir(path.dirname(config), { recursive: true });
	await fs.writeFile(`${config}c`, "// original\n{}");
	assert.equal(resolve_client_config("opencode", options), `${config}c`);
	await fs.writeFile(config, "{}");
	assert.equal(resolve_client_config("opencode", options), config);
	const project_options = { ...options, scope: "project" as const, project_root: directory };
	await fs.writeFile(path.join(directory, "opencode.jsonc"), "{}");
	assert.equal(resolve_client_config("opencode", project_options), path.join(directory, "opencode.jsonc"));
	assert.equal(await fs.readFile(`${config}c`, "utf8"), "// original\n{}");
});

test("matching registrations preserve environment, cwd and other entry fields", () => {
	for (const client of CLIENT_IDS) {
		const extended = { ...launch, env: { GODOT_MCP_SESSION_FILE: "E:/Sessions/游戏.json" }, cwd: "E:/My game" };
		const original = config_snippet(client, extended);
		assert.deepEqual(read_client_registration(client, original), { present: true, enabled: true, launch: extended });
		assert.deepEqual(update_client_config(client, original, launch), { text: original, changed: false });
	}
	const original = JSON.stringify({ mcp: { godot: { type: "local", command: [launch.command, ...launch.args], enabled: false, disabled: true, environment: { SESSION: "keep" }, cwd: "E:/My game", timeout: 12000 } } });
	const updated = update_client_config("opencode", original, launch);
	const entry = parse_json(updated.text).mcp.godot;
	assert.equal(entry.enabled, true);
	assert.equal(Object.hasOwn(entry, "disabled"), false);
	assert.deepEqual(entry.environment, { SESSION: "keep" });
	assert.equal(entry.cwd, "E:/My game");
	assert.equal(entry.timeout, 12000);
});

test("setup enables disabled Codex and both OpenCode shapes without changing their launch settings", () => {
	const codex = 'model = "keep"\n[mcp_servers.godot]\ncommand = "node"\nargs = ["bridge.js"]\nenabled = false\ncwd = "E:/Game"\n[mcp_servers.godot.env]\nSESSION = "keep"\n';
	const expected_launch = { command: "node", args: ["bridge.js"], cwd: "E:/Game", env: { SESSION: "keep" } };
	assert.deepEqual(read_client_registration("codex", codex), { present: true, enabled: false, launch: expected_launch });
	const enabled = update_client_config("codex", codex, { command: "node", args: ["bridge.js"] });
	assert.equal(enabled.changed, true);
	assert.deepEqual(read_client_registration("codex", enabled.text), { present: true, enabled: true, launch: expected_launch });
	assert.equal((parse_toml(enabled.text) as any).model, "keep");
	for (const version of ["1", "2"] as const) {
		const entry = { type: "local", command: ["node", "bridge.js"], enabled: false, disabled: true, environment: { SESSION: "keep" } };
		const original = JSON.stringify({ mcp: version === "1" ? { godot: entry } : { servers: { godot: entry } } });
		assert.equal(read_client_registration("opencode", original).enabled, false);
		const updated = update_client_config("opencode", original, { command: "node", args: ["bridge.js"] });
		assert.equal(read_client_registration("opencode", updated.text).enabled, true);
		const parsed = parse_json(updated.text);
		const target = version === "1" ? parsed.mcp.godot : parsed.mcp.servers.godot;
		assert.deepEqual(target.environment, { SESSION: "keep" });
		assert.equal(Object.hasOwn(target, version === "1" ? "disabled" : "enabled"), false);
		assert.equal(target[version === "1" ? "enabled" : "disabled"], version === "1");
	}
});

test("invalid stored and requested launch environments and cwd fail clearly", () => {
	for (const env of [null, [], { TOKEN: 123 }]) {
		assert.throws(() => read_client_registration("claude-code", JSON.stringify({ mcpServers: { godot: { command: "node", env } } })), /environment/);
		assert.throws(() => read_client_registration("opencode", JSON.stringify({ mcp: { godot: { type: "local", command: ["node"], environment: env } } })), /environment/);
		assert.throws(() => update_client_config("antigravity", "", { ...launch, env } as any), /environment/);
	}
	for (const cwd of [null, 123, ""]) {
		assert.throws(() => read_client_registration("claude-code", JSON.stringify({ mcpServers: { godot: { command: "node", cwd } } })), /cwd/);
		assert.throws(() => update_client_config("antigravity", "", { ...launch, cwd } as any), /cwd/);
	}
});

test("JSON BOM and comment-only documents remain editable without dropping comments", () => {
	for (const original of ["// Keep comment", "/* Keep comment */\n", "\uFEFF{}\r\n", "\uFEFF// Keep comment\n"]) {
		assert.equal(read_client_registration("antigravity", original).present, false);
		const updated = update_client_config("antigravity", original, launch);
		if (original.includes("Keep comment")) assert.match(updated.text, /Keep comment/);
		assert.equal(updated.text.startsWith("\uFEFF"), original.startsWith("\uFEFF"));
		assert.deepEqual(read_client_registration("antigravity", updated.text), { present: true, enabled: true, launch });
		assert.deepEqual(update_client_config("antigravity", updated.text, launch), { text: updated.text, changed: false });
	}
	assert.throws(() => update_client_config("antigravity", "/* unfinished", launch));
});

test("OpenCode server named servers does not get mistaken for the V2 container", () => {
	const options = { name: "servers" };
	const original = '{"mcp":{"servers":{"type":"local","command":["node","old.js"],"enabled":true},"other":{"type":"remote","url":"https://example.test"}}}';
	assert.deepEqual(read_client_registration("opencode", original, options), { present: true, enabled: true, launch: { command: "node", args: ["old.js"] } });
	const updated = update_client_config("opencode", original, launch, options);
	assert.deepEqual(parse_json(updated.text).mcp.servers, { type: "local", command: [launch.command, ...launch.args], enabled: true });
	assert.equal(Object.hasOwn(parse_json(updated.text).mcp.servers, "servers"), false);
	assert.deepEqual(parse_json(updated.text).mcp.other, { type: "remote", url: "https://example.test" });
});

test("OpenCode project path stays in project despite global override and config-dir needs explicit selection", () => {
	const options: ClientConfigOptions = { platform: "linux", home: "/home/test", env: { OPENCODE_CONFIG: "/global/opencode.json" }, scope: "project", project_root: "/projects/game" };
	assert.equal(resolve_client_config("opencode", options), "/projects/game/opencode.json");
	assert.throws(() => resolve_client_config("opencode", { ...options, env: { OPENCODE_CONFIG_DIR: "/custom/config" } }), /--config/);
	assert.equal(resolve_client_config("opencode", { ...options, env: { OPENCODE_CONFIG_DIR: "/custom/config", OPENCODE_CONFIG_CONTENT: "{}" }, config_path: "/selected/config.json" }), "/selected/config.json");
});

test("TOML removal preserves comments immediately preceding unrelated tables", () => {
	const original = '[mcp_servers.godot]\ncommand = "old"\n\n# Keep next table explanation\n[projects.game]\ntrust_level = "trusted"\n';
	const updated = update_client_config("codex", original, launch);
	assert.match(updated.text, /# Keep next table explanation\n\[projects.game\]/);
	const removed = update_client_config("codex", updated.text, null);
	assert.match(removed.text, /# Keep next table explanation\n\[projects.game\]/);
});
