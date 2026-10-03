import { as_error_text } from "./error_map.js";
import { RpcMethodError, RpcTransportError } from "./rpc_client.js";
import { SessionError } from "./session.js";

export interface StructuredError {
	code: string;
	message: string;
	operation: string;
	session_id?: string;
	run_id?: string;
	retryable: boolean;
	conflict?: Record<string, unknown>;
	godot_error?: unknown;
	details?: Record<string, unknown>;
}

const RPC_ERROR_CODES = new Map<number, string>([
	[-32001, "AUTH_FAILED"],
	[-32002, "CAPABILITY_DENIED"],
	[-32003, "INVALID_ARGUMENT"],
	[-32004, "CONFLICT"],
	[-32005, "NOT_FOUND"],
	[-32006, "INTERNAL"],
	[-32007, "DISCONNECTED"],
	[-32008, "TIMEOUT"],
	[-32009, "STALE_SESSION"],
	[-32010, "STALE_RUN"],
	[-32700, "PROTOCOL_ERROR"],
	[-32600, "PROTOCOL_ERROR"],
	[-32601, "METHOD_NOT_FOUND"],
	[-32602, "INVALID_ARGUMENT"],
]);

function _record_or_undefined(p_value: unknown): Record<string, unknown> | undefined {
	if (!p_value || typeof p_value !== "object" || Array.isArray(p_value)) {
		return undefined;
	}
	return p_value as Record<string, unknown>;
}

function _without_prefix(p_message: string): string {
	return p_message.replace(/^[A-Z_]+:\s*/, "");
}

export function to_structured_error(
	p_error: unknown,
	p_operation: string,
	p_context: { session_id?: string; run_id?: string } = {},
): StructuredError {
	if (p_error instanceof SessionError) {
		return {
			code: p_error.code,
			message: p_error.message,
			operation: p_operation,
			session_id: p_context.session_id,
			run_id: p_context.run_id,
			retryable: p_error.retryable,
			details: p_error.session_file ? { session_file: p_error.session_file } : undefined,
		};
	}

	if (p_error instanceof RpcMethodError) {
		const data = _record_or_undefined(p_error.data);
		const code = typeof data?.code === "string" ? data.code : (RPC_ERROR_CODES.get(p_error.code) ?? "GODOT_RPC_ERROR");
		return {
			code,
			message: typeof data?.message === "string" ? data.message : _without_prefix(p_error.message),
			operation: typeof data?.operation === "string" ? data.operation : p_operation,
			session_id: typeof data?.session_id === "string" ? data.session_id : p_context.session_id,
			run_id: typeof data?.run_id === "string" ? data.run_id : p_context.run_id,
			retryable: typeof data?.retryable === "boolean" ? data.retryable : code === "DISCONNECTED" || code === "TIMEOUT",
			conflict: _record_or_undefined(data?.conflict),
			godot_error: data?.godot_error,
			details: data,
		};
	}

	if (p_error instanceof RpcTransportError) {
		return {
			code: p_error.code,
			message: p_error.message,
			operation: p_operation,
			session_id: p_context.session_id,
			run_id: p_context.run_id,
			retryable: p_error.retryable,
		};
	}

	if (p_error && typeof p_error === "object" && !Array.isArray(p_error)) {
		const error = p_error as Record<string, unknown>;
		if (typeof error.code === "string" && typeof error.message === "string") {
			return {
				code: error.code,
				message: error.message,
				operation: p_operation,
				session_id: p_context.session_id,
				run_id: p_context.run_id,
				retryable: typeof error.retryable === "boolean" ? error.retryable : false,
			};
		}
	}

	return {
		code: "INTERNAL",
		message: as_error_text(p_error),
		operation: p_operation,
		session_id: p_context.session_id,
		run_id: p_context.run_id,
		retryable: false,
	};
}

export function to_mcp_error(p_error: StructuredError): Record<string, unknown> {
	const payload = { ok: false, error: p_error };
	return {
		content: [{ type: "text", text: `${p_error.code}: ${p_error.message}` }],
		structuredContent: payload,
		isError: true,
	};
}
