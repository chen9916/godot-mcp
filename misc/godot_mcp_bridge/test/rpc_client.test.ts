import assert from "node:assert/strict";
import net from "node:net";
import test from "node:test";

import { GodotRpcClient, RpcMethodError, RpcTransportError } from "../src/rpc_client.js";

async function listen(p_on_request: (p_request: Record<string, unknown>, p_socket: net.Socket) => void): Promise<{
	port: number;
	close: () => Promise<void>;
}> {
	const sockets = new Set<net.Socket>();
	const server = net.createServer((socket) => {
		sockets.add(socket);
		socket.on("close", () => sockets.delete(socket));
		let buffer = "";
		socket.on("data", (chunk) => {
			buffer += chunk.toString("utf8");
			const newline = buffer.indexOf("\n");
			if (newline < 0) {
				return;
			}
			const request = JSON.parse(buffer.slice(0, newline)) as Record<string, unknown>;
			p_on_request(request, socket);
		});
	});
	await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
	const address = server.address();
	assert.ok(address && typeof address === "object");
	return {
		port: address.port,
		close: async () => {
			for (const socket of sockets) {
				socket.destroy();
			}
			await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
		},
	};
}

test("round-trips one line-delimited JSON-RPC request", async () => {
	const endpoint = await listen((request, socket) => {
		socket.write(`${JSON.stringify({ jsonrpc: "2.0", id: request.id, result: { ok: true, data: { pong: true } } })}\n`);
	});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 500 });
	try {
		assert.deepEqual(await client.call("test.ping", { value: 1 }), { ok: true, data: { pong: true } });
	} finally {
		await client.close();
		await endpoint.close();
	}
});

test("preserves structured Godot method errors", async () => {
	const endpoint = await listen((request, socket) => {
		socket.write(`${JSON.stringify({
			jsonrpc: "2.0",
			id: request.id,
			error: { code: -32009, message: "STALE_SESSION", data: { code: "STALE_SESSION", retryable: true } },
		})}\n`);
	});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 500 });
	try {
		await assert.rejects(client.call("test.stale", {}), (error: unknown) =>
			error instanceof RpcMethodError && error.code === -32009 && (error.data as Record<string, unknown>).retryable === true);
	} finally {
		await client.close();
		await endpoint.close();
	}
});

test("times out with a retryable transport error", async () => {
	const endpoint = await listen(() => {});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 30 });
	try {
		await assert.rejects(client.call("test.timeout", {}), (error: unknown) =>
			error instanceof RpcTransportError && error.code === "TIMEOUT" && error.retryable);
	} finally {
		await client.close();
		await endpoint.close();
	}
});
