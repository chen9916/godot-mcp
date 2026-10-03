import assert from "node:assert/strict";
import fs from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { CallToolResultSchema } from "@modelcontextprotocol/sdk/types.js";

const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));

test("independent stdio MCP clients discover and call the same Godot tools independent of agent branding", { timeout: 20_000 }, async (t) => {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot stdio MCP "));
	const session_file = path.join(directory, "session.json");
	const sockets = new Set<net.Socket>();
	const calls: string[] = [];
	const endpoint = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		socket.on("error", (error: NodeJS.ErrnoException) => { assert.equal(error.code, "ECONNRESET"); });
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk.toString("utf8");
			while (buffer.includes("\n")) {
				const end = buffer.indexOf("\n");
				const request = JSON.parse(buffer.slice(0, end));
				buffer = buffer.slice(end + 1);
				calls.push(request.method);
				assert.equal(request.params._session_id, "independent-editor");
				const result = request.method === "godot.session.info"
					? { ok: true, data: { session_id: "independent-editor", protocol_version: 2 } }
					: request.method === "scene.get_active"
						? { ok: true, data: { scene_path: "res://example.tscn" } }
						: 42; // Older custom endpoints can return scalar JSON-RPC results.
				socket.write(JSON.stringify({ jsonrpc: "2.0", id: request.id, result }) + "\n");
			}
		});
	});
	t.after(async () => {
		for (const socket of sockets) {
			socket.destroy();
		}
		await new Promise<void>((resolve) => endpoint.close(() => resolve()));
		await fs.rm(directory, { recursive: true, force: true });
	});
	await new Promise<void>((resolve) => endpoint.listen(0, "127.0.0.1", resolve));
	const address = endpoint.address();
	assert.ok(address && typeof address === "object");
	await fs.writeFile(session_file, JSON.stringify({
		session_id: "independent-editor", protocol_version: 2, port: address.port, pid: process.pid,
		project_path: directory, started_at: Math.floor(Date.now() / 1000), expires_at: Math.floor(Date.now() / 1000) + 60,
		capabilities: { read_scene: true, read_project: true, write_scene: false },
	}));
	let expected_names: string[] | undefined;
	// These are independent SDK clients, not invocations of either agent app.
	for (const name of ["client-a", "client-b", "unrelated-mcp-client"]) {
		const transport = new StdioClientTransport({ command: process.execPath, args: [bridge, "--session-file", session_file], cwd: directory, stderr: "pipe" });
		const client = new Client({ name, version: "1.0.0" }, { capabilities: {} });
		try {
			await client.connect(transport);
			assert.equal(client.getServerVersion()?.name, "godot-mcp");
			const { tools } = await client.listTools();
			const names = tools.map((tool) => tool.name);
			assert.ok(names.includes("godot.scene.get_active"));
			assert.ok(names.includes("godot.scene.patch_schema"));
			assert.ok(names.length >= 74);
			if (expected_names) {
				assert.deepEqual(names, expected_names);
			}
			expected_names = names;
			const response = CallToolResultSchema.parse(await client.callTool({ name: "godot.scene.get_active", arguments: {} }));
			assert.equal(response.isError, false);
			assert.deepEqual(response.structuredContent, { ok: true, data: { scene_path: "res://example.tscn" } });
			assert.equal(response.content[0].type, "text");
			if (response.content[0].type === "text") {
				assert.deepEqual(JSON.parse(response.content[0].text), response.structuredContent);
			}
			const denied = CallToolResultSchema.parse(await client.callTool({ name: "godot.node.delete", arguments: { node_path: "/root/Child" } }));
			assert.equal(denied.isError, true);
			assert.equal((denied.structuredContent?.error as { code: string }).code, "CAPABILITY_DENIED");
			const scalar = CallToolResultSchema.parse(await client.callTool({ name: "godot.project.get_setting", arguments: { key: "example" } }));
			assert.deepEqual(scalar.content, [{ type: "text", text: "42" }]);
			assert.equal(scalar.structuredContent, undefined);
		} finally {
			await client.close();
		}
	}
	assert.ok(calls.includes("scene.get_active"));
	assert.equal(calls.includes("node.delete"), false);
});

for (const version of ["2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"]) {
	test(`standard MCP initialization and offline tool discovery work with protocol ${version}`, { timeout: 10_000 }, async () => {
		const transport = new StdioClientTransport({ command: process.execPath, args: [bridge], stderr: "pipe" });
		const pending = new Map<number, { resolve(value: any): void; reject(error: unknown): void }>();
		transport.onmessage = (message) => {
			if ("id" in message && typeof message.id === "number") {
				const waiter = pending.get(message.id);
				pending.delete(message.id);
				if ("error" in message) {
					waiter?.reject(message.error);
				} else if ("result" in message) {
					waiter?.resolve(message.result);
				}
			}
		};
		let id = 0;
		async function request(method: string, params: Record<string, unknown>): Promise<any> {
			const request_id = ++id;
			const result = new Promise((resolve, reject) => pending.set(request_id, { resolve, reject }));
			await transport.send({ jsonrpc: "2.0", id: request_id, method, params });
			return result;
		}
		await transport.start();
		try {
			const initialized = await request("initialize", { protocolVersion: version, capabilities: {}, clientInfo: { name: "generic-client", version: "1" } });
			assert.equal(initialized.protocolVersion, version);
			assert.equal(initialized.serverInfo.name, "godot-mcp");
			await transport.send({ jsonrpc: "2.0", method: "notifications/initialized" });
			const listed = await request("tools/list", {});
			assert.ok(listed.tools.some((tool: { name: string }) => tool.name === "godot.scene.patch_schema"));
			const schema = await request("tools/call", { name: "godot.scene.patch_schema", arguments: {} });
			assert.equal(schema.isError, false);
			assert.equal(schema.content[0].type, "text");
		} finally {
			await transport.close();
		}
	});
}
