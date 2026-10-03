import { randomUUID } from "node:crypto";

import { RpcMethodError, RpcTransportError } from "./rpc_client.js";

export interface RpcCaller {
	call(
		p_method: string,
		p_args: Record<string, unknown>,
		p_options?: { retry_safe?: boolean },
	): Promise<unknown>;
}

function _bounded_timeout(p_args: Record<string, unknown>): number {
	const requested_timeout = typeof p_args.timeout_ms === "number" ? p_args.timeout_ms : 10_000;
	return Math.max(100, Math.min(60_000, Math.floor(requested_timeout)));
}

export function filesystem_wait_complete(p_payload: unknown): boolean {
	if (!p_payload || typeof p_payload !== "object" || Array.isArray(p_payload)) {
		return false;
	}
	const data = (p_payload as Record<string, unknown>).data;
	return Boolean(data && typeof data === "object" && !Array.isArray(data) && (data as Record<string, unknown>).complete === true);
}

export function runtime_operation_pending(p_payload: unknown): boolean {
	if (!p_payload || typeof p_payload !== "object" || Array.isArray(p_payload)) {
		return false;
	}
	const data = (p_payload as Record<string, unknown>).data;
	return Boolean(data && typeof data === "object" && !Array.isArray(data) && (data as Record<string, unknown>).pending === true);
}

export function is_async_runtime_method(p_method: string): boolean {
	return p_method.startsWith("runtime.") && ![
		"runtime.start",
		"runtime.stop",
		"runtime.status",
		"runtime.pause",
		"runtime.resume",
	].includes(p_method);
}

export async function wait_for_filesystem(
	p_caller: RpcCaller,
	p_args: Record<string, unknown>,
): Promise<unknown> {
	const timeout_ms = _bounded_timeout(p_args);
	const started_at = Date.now();
	while (true) {
		const result = await p_caller.call("filesystem.wait", p_args, { retry_safe: true });
		if (filesystem_wait_complete(result)) {
			return result;
		}
		const elapsed = Date.now() - started_at;
		if (elapsed >= timeout_ms) {
			throw new RpcTransportError(`Filesystem synchronization timed out after ${timeout_ms}ms.`, "TIMEOUT");
		}
		await new Promise((resolve) => setTimeout(resolve, Math.min(100, timeout_ms - elapsed)));
	}
}

export async function wait_for_runtime_operation(
	p_caller: RpcCaller,
	p_method: string,
	p_args: Record<string, unknown>,
	p_request_id: string = randomUUID(),
): Promise<unknown> {
	const timeout_ms = _bounded_timeout(p_args);
	const args = { ...p_args, _request_id: p_request_id };
	const started_at = Date.now();
	const acknowledge = async (): Promise<void> => {
		try {
			await p_caller.call(p_method, { ...args, _request_ack: true }, { retry_safe: true });
		} catch {
			// The final response is already in hand. A lost acknowledgment only
			// leaves a bounded, expiring deduplication record in the editor.
		}
	};

	while (true) {
		// Runtime requests are deduplicated by _request_id in the editor. Repeating
		// one after a transport reconnect cannot apply the mutation twice.
		let result: unknown;
		try {
			result = await p_caller.call(p_method, args, { retry_safe: true });
		} catch (error: unknown) {
			if (error instanceof RpcMethodError) {
				await acknowledge();
			}
			throw error;
		}
		if (!runtime_operation_pending(result)) {
			await acknowledge();
			return result;
		}
		const elapsed = Date.now() - started_at;
		if (elapsed >= timeout_ms) {
			throw new RpcTransportError(`Runtime operation '${p_method}' timed out after ${timeout_ms}ms.`, "TIMEOUT");
		}
		await new Promise((resolve) => setTimeout(resolve, Math.min(25, timeout_ms - elapsed)));
	}
}
