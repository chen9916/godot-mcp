import assert from "node:assert/strict";
import test from "node:test";

import {
	filesystem_wait_complete,
	is_async_runtime_method,
	runtime_operation_pending,
	wait_for_filesystem,
	wait_for_runtime_operation,
	type RpcCaller,
} from "../src/operation_waiter.js";
import { RpcMethodError, RpcTransportError } from "../src/rpc_client.js";

class SequenceCaller implements RpcCaller {
	readonly calls: Array<{ method: string; args: Record<string, unknown>; retry_safe?: boolean }> = [];

	constructor(private readonly responses: unknown[]) {}

	async call(p_method: string, p_args: Record<string, unknown>, p_options?: { retry_safe?: boolean }): Promise<unknown> {
		this.calls.push({ method: p_method, args: { ...p_args }, retry_safe: p_options?.retry_safe });
		return this.responses.shift();
	}
}

test("detects only completed filesystem and pending runtime envelopes", () => {
	assert.equal(filesystem_wait_complete({ data: { complete: true } }), true);
	assert.equal(filesystem_wait_complete({ data: { complete: false } }), false);
	assert.equal(runtime_operation_pending({ data: { pending: true } }), true);
	assert.equal(runtime_operation_pending({ data: { pending: false } }), false);
	assert.equal(is_async_runtime_method("runtime.get_tree"), true);
	assert.equal(is_async_runtime_method("runtime.start"), false);
});

test("polls runtime requests with one stable deduplication ID", async () => {
	const caller = new SequenceCaller([
		{ ok: true, data: { pending: true } },
		{ ok: true, data: { pending: true } },
		{ ok: true, data: { pending: false, value: 42 } },
	]);

	const result = await wait_for_runtime_operation(caller, "runtime.get_property", {
		run_id: "run-a",
		timeout_ms: 1000,
	}, "request-a");

	assert.deepEqual(result, { ok: true, data: { pending: false, value: 42 } });
	assert.equal(caller.calls.length, 4);
	assert.deepEqual(caller.calls.map((call) => call.args._request_id), ["request-a", "request-a", "request-a", "request-a"]);
	assert.equal(caller.calls[3].args._request_ack, true);
	assert.ok(caller.calls.every((call) => call.retry_safe === true));
});

test("waits for filesystem completion without blocking the editor thread", async () => {
	const caller = new SequenceCaller([
		{ ok: true, data: { complete: false } },
		{ ok: true, data: { complete: true, scan_id: "scan-a" } },
	]);
	const result = await wait_for_filesystem(caller, { scan_id: "scan-a", timeout_ms: 1000 });
	assert.deepEqual(result, { ok: true, data: { complete: true, scan_id: "scan-a" } });
	assert.equal(caller.calls.length, 2);
});

test("returns a stable retryable timeout for a runtime that never responds", async () => {
	const caller: RpcCaller = {
		async call() {
			return { ok: true, data: { pending: true } };
		},
	};
	await assert.rejects(
		wait_for_runtime_operation(caller, "runtime.get_tree", { timeout_ms: 100 }, "request-timeout"),
		(error: unknown) => error instanceof RpcTransportError && error.code === "TIMEOUT" && error.retryable,
	);
});

test("acknowledges a completed runtime error before returning it", async () => {
	const calls: Record<string, unknown>[] = [];
	const caller: RpcCaller = {
		async call(_method, args) {
			calls.push(args);
			if (args._request_ack === true) {
				return { ok: true, data: { acknowledged: true } };
			}
			throw new RpcMethodError(-32003, "runtime rejected the call");
		},
	};
	await assert.rejects(
		wait_for_runtime_operation(caller, "runtime.call", { run_id: "run-a" }, "request-error"),
		RpcMethodError,
	);
	assert.equal(calls.length, 2);
	assert.equal(calls[1]._request_ack, true);
});
