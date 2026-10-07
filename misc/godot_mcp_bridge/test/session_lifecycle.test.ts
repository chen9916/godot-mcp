import assert from "node:assert/strict";
import test from "node:test";

import { RpcTransportError, type RpcClientLike } from "../src/rpc_client.js";
import type { SessionData } from "../src/session.js";
import { SessionLifecycle } from "../src/session_lifecycle.js";
import { RequestCancelledError } from "../src/request_control.js";

type CallHandler = (p_method: string, p_params: Record<string, unknown>) => Promise<unknown>;

class FakeClient implements RpcClientLike {
	readonly calls: Array<{ method: string; params: Record<string, unknown> }> = [];
	closed = false;

	constructor(private readonly port: number, private readonly handler: CallHandler) {}

	get_port(): number {
		return this.port;
	}

	async call(p_method: string, p_params: Record<string, unknown>): Promise<unknown> {
		this.calls.push({ method: p_method, params: p_params });
		return await this.handler(p_method, p_params);
	}

	async close(): Promise<void> {
		this.closed = true;
	}
}

function session(p_session_id: string, p_port: number, p_pid = 100): SessionData {
	return {
		session_id: p_session_id,
		protocol_version: 2,
		port: p_port,
		pid: p_pid,
		capabilities: { read_project: true },
	};
}

function handshake(p_session_id: string): unknown {
	return { ok: true, data: { session_id: p_session_id } };
}

test("replaces the cached client when a new editor session is discovered", async () => {
	let current = session("editor-a", 6101);
	const clients: FakeClient[] = [];
	const lifecycle = new SessionLifecycle({
		load_session: () => current,
		create_client: (metadata) => {
			const client = new FakeClient(metadata.port, async () => handshake(metadata.session_id!));
			clients.push(client);
			return client;
		},
		max_reconnect_attempts: 0,
	});

	await lifecycle.refresh();
	assert.equal(lifecycle.get_info().session_id, "editor-a");

	current = session("editor-b", 6102, 101);
	await lifecycle.refresh();
	assert.equal(lifecycle.get_info().session_id, "editor-b");
	assert.equal(clients.length, 2);
	assert.equal(clients[0].closed, true);
});

test("rejects an endpoint whose handshake does not match the session file", async () => {
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-new", 6201),
		create_client: (metadata) => new FakeClient(metadata.port, async () => handshake("editor-obsolete")),
		max_reconnect_attempts: 0,
	});

	await assert.rejects(lifecycle.refresh(), /editor-obsolete/);
	assert.equal(lifecycle.get_info().state, "failed");
});

test("retries a read against a replacement session after transport loss", async () => {
	let current = session("editor-a", 6301);
	let target_calls = 0;
	const lifecycle = new SessionLifecycle({
		load_session: () => current,
		create_client: (metadata) => new FakeClient(metadata.port, async (method, params) => {
			if (method === "godot.session.info") {
				return handshake(metadata.session_id!);
			}
			target_calls++;
			if (metadata.session_id === "editor-a") {
				current = session("editor-b", 6302, 101);
				throw new RpcTransportError("editor exited");
			}
			assert.equal(params._session_id, "editor-b");
			return { ok: true, data: 42 };
		}),
		max_reconnect_attempts: 0,
	});

	const result = await lifecycle.call("project.get_setting", { keys: ["answer"] }, { retry_safe: true });
	assert.deepEqual(result, { ok: true, data: 42 });
	assert.equal(target_calls, 2);
	assert.equal(lifecycle.get_info().session_id, "editor-b");
});

test("does not replay a mutation after an ambiguous disconnect", async () => {
	let target_calls = 0;
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-a", 6401),
		create_client: (metadata) => new FakeClient(metadata.port, async (method) => {
			if (method === "godot.session.info") {
				return handshake(metadata.session_id!);
			}
			target_calls++;
			throw new RpcTransportError("response was lost");
		}),
		max_reconnect_attempts: 0,
	});

	await assert.rejects(
		lifecycle.call("node.set_properties", { node_path: "." }, { retry_safe: false }),
		/response was lost/,
	);
	assert.equal(target_calls, 1);
	assert.equal(lifecycle.get_info().state, "disconnected");
});

test("uses bounded exponential reconnect delays", async () => {
	const delays: number[] = [];
	let attempts = 0;
	const lifecycle = new SessionLifecycle({
		load_session: () => {
			attempts++;
			throw new Error("not ready");
		},
		max_reconnect_attempts: 3,
		initial_backoff_ms: 10,
		max_backoff_ms: 25,
		sleep: async (delay) => {
			delays.push(delay);
		},
	});

	await assert.rejects(lifecycle.refresh(), /not ready/);
	assert.equal(attempts, 4);
	assert.deepEqual(delays, [10, 20, 25]);
	assert.equal(lifecycle.get_info().state, "failed");
});

test("shares one handshake across concurrent refreshes", async () => {
	let client_count = 0;
	let release_handshake: (() => void) | undefined;
	const handshake_gate = new Promise<void>((resolve) => {
		release_handshake = resolve;
	});
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-a", 6501),
		create_client: (metadata) => {
			client_count++;
			return new FakeClient(metadata.port, async () => {
				await handshake_gate;
				return handshake(metadata.session_id!);
			});
		},
		max_reconnect_attempts: 0,
	});

	const first = lifecycle.refresh();
	const second = lifecycle.refresh();
	release_handshake?.();
	await Promise.all([first, second]);
	assert.equal(client_count, 1);
	assert.equal(lifecycle.get_info().state, "connected");
});

test("cancelled discovery cannot later dispatch a mutation", async () => {
	let release: (() => void) | undefined;
	let handshake_seen: (() => void) | undefined;
	const gate = new Promise<void>((resolve) => { release = resolve; });
	const seen = new Promise<void>((resolve) => { handshake_seen = resolve; });
	let mutations = 0;
	let candidate: FakeClient | undefined;
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-a", 6601), max_reconnect_attempts: 0,
		create_client: (metadata) => candidate = new FakeClient(metadata.port, async (method) => {
			if (method === "godot.session.info") {
				handshake_seen?.();
				await gate;
				return handshake("editor-a");
			}
			mutations++;
			return { ok: true };
		}),
	});
	const controller = new AbortController();
	const rejected = assert.rejects(lifecycle.call("node.create", {}, { signal: controller.signal }), RequestCancelledError);
	await seen;
	controller.abort();
	await rejected;
	release?.();
	await new Promise((resolve) => setImmediate(resolve));
	assert.equal(mutations, 0);
	assert.equal(candidate?.closed, true);
	assert.equal(lifecycle.get_info().state, "disconnected");
	await lifecycle.close();
});

test("one cancelled caller preserves another caller's shared discovery", async () => {
	let release: (() => void) | undefined;
	const gate = new Promise<void>((resolve) => { release = resolve; });
	let clients = 0;
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-a", 6701), max_reconnect_attempts: 0,
		create_client: (metadata) => {
			clients++;
			return new FakeClient(metadata.port, async (method) => {
				if (method === "godot.session.info") { await gate; return handshake("editor-a"); }
				return { data: method };
			});
		},
	});
	const controller = new AbortController();
	const rejected = assert.rejects(lifecycle.call("read.cancel", {}, { signal: controller.signal }), RequestCancelledError);
	const survivor = lifecycle.call("read.survivor", {});
	await new Promise((resolve) => setImmediate(resolve));
	controller.abort();
	await rejected;
	release?.();
	assert.deepEqual(await survivor, { data: "read.survivor" });
	assert.equal(clients, 1);
	await lifecycle.close();
});

test("a request deadline also bounds discovery backoff", async () => {
	let attempts = 0;
	const lifecycle = new SessionLifecycle({
		load_session: () => { attempts++; throw new Error("not ready"); },
		initial_backoff_ms: 1000, max_backoff_ms: 1000, max_reconnect_attempts: 3,
	});
	const started = Date.now();
	await assert.rejects(lifecycle.call("node.create", {}, { deadline_ms: started + 100 }),
		(error: unknown) => error instanceof RpcTransportError && error.code === "TIMEOUT");
	await lifecycle.close();
	assert.equal(attempts, 1);
	assert.ok(Date.now() - started < 700);
});

test("closing during a pending handshake cannot restore a connected client", async () => {
	let release: (() => void) | undefined;
	let seen: (() => void) | undefined;
	const gate = new Promise<void>((resolve) => { release = resolve; });
	const handshake_seen = new Promise<void>((resolve) => { seen = resolve; });
	let candidate: FakeClient | undefined;
	const lifecycle = new SessionLifecycle({
		load_session: () => session("editor-a", 6801), max_reconnect_attempts: 0,
		create_client: (metadata) => candidate = new FakeClient(metadata.port, async () => {
			seen?.();
			await gate;
			return handshake("editor-a");
		}),
	});
	const rejected = assert.rejects(lifecycle.refresh(), RequestCancelledError);
	await handshake_seen;
	await lifecycle.close();
	await rejected;
	release?.();
	await new Promise((resolve) => setImmediate(resolve));
	assert.equal(candidate?.closed, true);
	assert.equal(lifecycle.get_active_session(), null);
	assert.equal(lifecycle.get_info().state, "disconnected");
});
