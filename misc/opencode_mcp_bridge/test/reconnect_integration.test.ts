import assert from "node:assert/strict";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { SessionLifecycle } from "../src/session_lifecycle.js";

interface Endpoint {
	port: number;
	close(): Promise<void>;
}

async function start_endpoint(p_session_id: string, p_label: string): Promise<Endpoint> {
	const sockets = new Set<net.Socket>();
	const server = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk.toString("utf8");
			while (buffer.includes("\n")) {
				const newline = buffer.indexOf("\n");
				const line = buffer.slice(0, newline);
				buffer = buffer.slice(newline + 1);
				if (!line.trim()) {
					continue;
				}
				const request = JSON.parse(line) as { id: number; method: string };
				const data = request.method === "opencode.session.info"
					? { session_id: p_session_id, protocol_version: 2 }
					: { endpoint: p_label };
				socket.write(`${JSON.stringify({ jsonrpc: "2.0", id: request.id, result: { ok: true, data } })}\n`);
			}
		});
	});
	await new Promise<void>((resolve, reject) => {
		server.once("error", reject);
		server.listen(0, "127.0.0.1", resolve);
	});
	const address = server.address();
	assert.ok(address && typeof address === "object");
	return {
		port: address.port,
		async close() {
			for (const socket of sockets) {
				socket.destroy();
			}
			await new Promise<void>((resolve) => server.close(() => resolve()));
		},
	};
}

test("one bridge lifecycle reconnects to a replacement JSON-RPC editor endpoint", async (t) => {
	const temp_dir = fs.mkdtempSync(path.join(os.tmpdir(), "godot-mcp-reconnect-"));
	const session_file = path.join(temp_dir, "session.json");
	let first: Endpoint | undefined;
	let second: Endpoint | undefined;
	const lifecycle = new SessionLifecycle({ session_file, initial_backoff_ms: 1, max_backoff_ms: 2 });

	t.after(async () => {
		await lifecycle.close();
		if (first) {
			await first.close().catch(() => undefined);
		}
		if (second) {
			await second.close().catch(() => undefined);
		}
		fs.rmSync(temp_dir, { recursive: true, force: true });
	});

	const write_session = (p_session_id: string, p_port: number) => {
		fs.writeFileSync(session_file, JSON.stringify({
			session_id: p_session_id,
			protocol_version: 2,
			port: p_port,
			pid: process.pid,
			project_path: temp_dir,
			started_at: Math.floor(Date.now() / 1000),
			expires_at: Math.floor(Date.now() / 1000) + 60,
			capabilities: { read_scene: true },
		}));
	};

	first = await start_endpoint("session-a", "editor-a");
	write_session("session-a", first.port);
	assert.deepEqual(await lifecycle.call("scene.get_active", {}, { retry_safe: true }), {
		ok: true,
		data: { endpoint: "editor-a" },
	});
	assert.equal(lifecycle.get_info().state, "connected");
	assert.equal(lifecycle.get_info().session_id, "session-a");

	await first.close();
	first = undefined;
	second = await start_endpoint("session-b", "editor-b");
	write_session("session-b", second.port);

	assert.deepEqual(await lifecycle.call("scene.get_active", {}, { retry_safe: true }), {
		ok: true,
		data: { endpoint: "editor-b" },
	});
	assert.equal(lifecycle.get_info().state, "connected");
	assert.equal(lifecycle.get_info().session_id, "session-b");
});
