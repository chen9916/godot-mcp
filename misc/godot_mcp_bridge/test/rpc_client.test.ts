import assert from "node:assert/strict";
import net from "node:net";
import test from "node:test";

import { GodotRpcClient, RpcMethodError, RpcTransportError } from "../src/rpc_client.js";
import { RequestCancelledError } from "../src/request_control.js";

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
			while (buffer.includes("\n")) {
				const newline = buffer.indexOf("\n");
				const request = JSON.parse(buffer.slice(0, newline)) as Record<string, unknown>;
				buffer = buffer.slice(newline + 1);
				p_on_request(request, socket);
			}
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

test("preserves multibyte Unicode split across TCP response chunks", async () => {
	const text = "你好 🌍 café";
	const endpoint = await listen((request, socket) => {
		const response = Buffer.from(JSON.stringify({ jsonrpc: "2.0", id: request.id, result: { text } }) + "\n");
		const first = response.indexOf(Buffer.from("你")) + 1;
		const second = response.indexOf(Buffer.from("🌍")) + 2;
		socket.write(response.subarray(0, first));
		setTimeout(() => socket.write(response.subarray(first, second)), 20);
		setTimeout(() => socket.write(response.subarray(second)), 40);
	});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 1000 });
	try {
		assert.deepEqual(await client.call("test.unicode", {}), { text });
	} finally {
		await client.close();
		await endpoint.close();
	}
});

test("cancels only one pending request and ignores its late response", async () => {
	let release_request: (() => void) | undefined;
	let request_seen: (() => void) | undefined;
	const seen = new Promise<void>((resolve) => { request_seen = resolve; });
	const endpoint = await listen((request, socket) => {
		const respond = (): void => { socket.write(JSON.stringify({ jsonrpc: "2.0", id: request.id, result: request.method }) + "\n"); };
		if (request.method === "test.cancel") {
			release_request = respond;
			request_seen?.();
		} else {
			respond();
		}
	});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 1000 });
	try {
		const controller = new AbortController();
		const rejected = assert.rejects(client.call("test.cancel", {}, { signal: controller.signal }), RequestCancelledError);
		await seen;
		controller.abort();
		await rejected;
		release_request?.();
		assert.equal(await client.call("test.next", {}), "test.next");
	} finally {
		await client.close();
		await endpoint.close();
	}
});

test("honors an absolute deadline shorter than the configured RPC timeout", async () => {
	const endpoint = await listen(() => {});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 1000 });
	try {
		const started = Date.now();
		await assert.rejects(client.call("test.deadline", {}, { deadline_ms: started + 100 }),
			(error: unknown) => error instanceof RpcTransportError && error.code === "TIMEOUT");
		assert.ok(Date.now() - started < 700, "The per-call deadline must precede the configured 1000ms timeout.");
	} finally {
		await client.close();
		await endpoint.close();
	}
});

test("resets incomplete UTF-8 and framing after reconnect", async () => {
	let first = true;
	const endpoint = await listen((request, socket) => {
		if (first) {
			first = false;
			socket.write(Buffer.from([0xe4]));
			setTimeout(() => socket.destroy(), 20);
		} else {
			socket.write(JSON.stringify({ jsonrpc: "2.0", id: request.id, result: "你好" }) + "\n");
		}
	});
	const client = new GodotRpcClient({ port: endpoint.port, timeout_ms: 1000 });
	try {
		await assert.rejects(client.call("test.old", {}), RpcTransportError);
		assert.equal(await client.call("test.new", {}), "你好");
	} finally {
		await client.close();
		await endpoint.close();
	}
});
