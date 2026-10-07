import { randomUUID } from "node:crypto";

import { RpcMethodError, RpcTransportError } from "./rpc_client.js";
import { request_delay, with_request_limits, type RequestLimits } from "./request_control.js";

export interface RpcCaller {
	call(
		p_method: string,
		p_args: Record<string, unknown>,
		p_options?: RequestLimits & { retry_safe?: boolean },
	): Promise<unknown>;
}

export function operation_timeout_ms(p_args: Record<string, unknown>): number {
	const requested_timeout = typeof p_args.timeout_ms === "number" ? p_args.timeout_ms : 10_000;
	if (!Number.isFinite(requested_timeout)) {
		throw new RpcMethodError(-32003, "timeout_ms must be finite.");
	}
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
	p_options: RequestLimits = {},
): Promise<unknown> {
	const timeout_ms = operation_timeout_ms(p_args);
	const limits = { ...p_options, deadline_ms: Math.min(p_options.deadline_ms ?? Infinity, Date.now() + timeout_ms) };
	const timeout_error = (): RpcTransportError => new RpcTransportError(`Filesystem synchronization timed out after ${timeout_ms}ms.`, "TIMEOUT");
	while (true) {
		const result = await with_request_limits(
			() => p_caller.call("filesystem.wait", p_args, { ...limits, retry_safe: true }), limits, timeout_error,
		);
		if (filesystem_wait_complete(result)) {
			return result;
		}
		await request_delay(100, limits, timeout_error);
	}
}

export async function wait_for_runtime_operation(
	p_caller: RpcCaller,
	p_method: string,
	p_args: Record<string, unknown>,
	p_request_id: string = randomUUID(),
	p_options: RequestLimits = {},
): Promise<unknown> {
	const timeout_ms = operation_timeout_ms(p_args);
	const args = { ...p_args, _request_id: p_request_id };
	const limits = { ...p_options, deadline_ms: Math.min(p_options.deadline_ms ?? Infinity, Date.now() + timeout_ms) };
	const timeout_error = (): RpcTransportError => new RpcTransportError(
		`Runtime operation '${p_method}' timed out after ${timeout_ms}ms. An operation already sent may still complete.`, "TIMEOUT",
	);
	const call = (p_call_args: Record<string, unknown>): Promise<unknown> => with_request_limits(
		() => p_caller.call(p_method, p_call_args, { ...limits, retry_safe: true }), limits, timeout_error,
	);
	const acknowledge = async (): Promise<void> => {
		try {
			await call({ ...args, _request_ack: true });
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
			result = await call(args);
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
		await request_delay(25, limits, timeout_error);
	}
}
