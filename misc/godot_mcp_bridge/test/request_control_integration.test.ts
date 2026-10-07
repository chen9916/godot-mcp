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

interface Request {
	id: number;
	method: string;
	params: Record<string, unknown>;
}

async function fixture(p_handler: (p_request: Request, p_respond: (p_data: unknown) => void) => void): Promise<{
	client: Client;
	requests: Request[];
	close: () => Promise<void>;
}> {
	const directory = await fs.mkdtemp(path.join(os.tmpdir(), "godot-mcp-request-control-"));
	const session_file = path.join(directory, "session.json");
	const sockets = new Set<net.Socket>();
	const requests: Request[] = [];
	const endpoint = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		socket.on("error", () => {});
		socket.setEncoding("utf8");
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk;
			while (buffer.includes("\n")) {
				const end = buffer.indexOf("\n");
				const request = JSON.parse(buffer.slice(0, end)) as Request;
				buffer = buffer.slice(end + 1);
				requests.push(request);
				p_handler(request, (data) => {
					if (!socket.destroyed) {
						socket.write(JSON.stringify({ jsonrpc: "2.0", id: request.id, result: { ok: true, data } }) + "\n");
					}
				});
			}
		});
	});
	await new Promise<void>((resolve) => endpoint.listen(0, "127.0.0.1", resolve));
	const address = endpoint.address();
	assert.ok(address && typeof address === "object");
	await fs.writeFile(session_file, JSON.stringify({
		session_id: "request-control-editor", protocol_version: 2, port: address.port, pid: process.pid,
		capabilities: { read_project: true, read_scene: true, write_scene: true, filesystem_sync: true, runtime_inspection: true },
	}));
	const bridge = fileURLToPath(new URL("../src/main.js", import.meta.url));
	const client = new Client({ name: "request-control-client", version: "1" }, { capabilities: {} });
	const transport = new StdioClientTransport({ command: process.execPath, args: [bridge, "--session-file", session_file], stderr: "pipe" });
	const close = async (): Promise<void> => {
		await client.close();
		for (const socket of sockets) { socket.destroy(); }
		await new Promise<void>((resolve) => endpoint.close(() => resolve()));
		await fs.unlink(session_file);
		await fs.rmdir(directory);
	};
	try {
		await client.connect(transport);
		return { client, requests, close };
	} catch (error: unknown) {
		await close();
		throw error;
	}
}

test("SDK cancellation during editor discovery prevents later mutation dispatch", { timeout: 10_000 }, async () => {
	let seen: (() => void) | undefined;
	const handshake_seen = new Promise<void>((resolve) => { seen = resolve; });
	let release: (() => void) | undefined;
	let first = true;
	const f = await fixture((request, respond) => {
		if (request.method === "godot.session.info") {
			if (first) {
				first = false;
				release = () => respond({ session_id: "request-control-editor" });
				seen?.();
			} else { respond({ session_id: "request-control-editor" }); }
		} else { respond({ created: true }); }
	});
	try {
		const controller = new AbortController();
		const rejected = assert.rejects(f.client.callTool({ name: "godot.node.create", arguments: { parent_path: ".", type: "Node" } }, CallToolResultSchema, { signal: controller.signal }));
		await handshake_seen;
		controller.abort();
		await rejected;
		await f.client.callTool({ name: "godot.scene.patch_schema", arguments: {} });
		release?.();
		await new Promise((resolve) => setTimeout(resolve, 100));
		assert.equal(f.requests.filter((request) => request.method === "node.create").length, 0);
		const result = await f.client.callTool({ name: "godot.node.create", arguments: { parent_path: ".", type: "Node" } });
		assert.equal(result.isError, false);
		assert.equal(f.requests.filter((request) => request.method === "node.create").length, 1);
	} finally { await f.close(); }
});

test("SDK cancellation stops filesystem polling while keeping the bridge usable", { timeout: 10_000 }, async () => {
	let seen: (() => void) | undefined;
	const poll_seen = new Promise<void>((resolve) => { seen = resolve; });
	const f = await fixture((request, respond) => {
		if (request.method === "godot.session.info") { respond({ session_id: "request-control-editor" }); }
		else { respond({ complete: false }); seen?.(); }
	});
	try {
		const controller = new AbortController();
		const rejected = assert.rejects(f.client.callTool({ name: "godot.filesystem.wait", arguments: { timeout_ms: 1000 } }, CallToolResultSchema, { signal: controller.signal }));
		await poll_seen;
		controller.abort();
		await rejected;
		await f.client.callTool({ name: "godot.scene.patch_schema", arguments: {} });
		const count = f.requests.filter((request) => request.method === "filesystem.wait").length;
		await new Promise((resolve) => setTimeout(resolve, 250));
		assert.equal(f.requests.filter((request) => request.method === "filesystem.wait").length, count);
	} finally { await f.close(); }
});

test("MCP operation deadline covers an unresponsive editor RPC", { timeout: 10_000 }, async () => {
	const f = await fixture((request, respond) => {
		if (request.method === "godot.session.info") { respond({ session_id: "request-control-editor" }); }
	});
	try {
		const started = Date.now();
		const result = await f.client.callTool({ name: "godot.filesystem.wait", arguments: { timeout_ms: 100 } });
		assert.equal(result.isError, true);
		assert.equal((result.structuredContent as { error: { code: string } }).error.code, "TIMEOUT");
		assert.ok(Date.now() - started < 1000, "Operation deadline must precede the default 10-second RPC timeout.");
		assert.equal(f.requests.filter((request) => request.method === "filesystem.wait").length, 1);
	} finally { await f.close(); }
});

test("runtime polling preserves arbitrary arguments and does not wait past the deadline for acknowledgment", { timeout: 10_000 }, async () => {
	const f = await fixture((request, respond) => {
		if (request.method === "godot.session.info") { respond({ session_id: "request-control-editor" }); }
		else if (!request.params._request_ack) { respond({ pending: false, result: 42 }); }
	});
	try {
		const values = [5, false, { nested: ["你好", null] }];
		const started = Date.now();
		const result = await f.client.callTool({ name: "godot.runtime.call", arguments: { run_id: "fixture-run", node_path: "/root", method: "probe", arguments: values, timeout_ms: 100 } });
		assert.equal(result.isError, false);
		assert.ok(Date.now() - started < 1000);
		const calls = f.requests.filter((request) => request.method === "runtime.call");
		assert.equal(calls.length, 2);
		assert.deepEqual(calls[0].params.arguments, values);
		assert.equal(calls[0].params._request_id, calls[1].params._request_id);
		assert.equal(calls[1].params._request_ack, true);
	} finally { await f.close(); }
});
