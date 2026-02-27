export interface GodotRpcErrorLike {
	code?: number;
	message?: string;
	data?: unknown;
}

const ERROR_PREFIX_PATTERN = /^([A-Z_]+):\s+/;

function _coerce_error_code(p_code: unknown): number | undefined {
	return typeof p_code === "number" && Number.isFinite(p_code) ? p_code : undefined;
}

function _has_error_prefix(p_message: string): boolean {
	return ERROR_PREFIX_PATTERN.test(p_message);
}

function _with_error_prefix(p_prefix: string, p_message: string): string {
	if (_has_error_prefix(p_message)) {
		return p_message;
	}
	return `${p_prefix}: ${p_message}`;
}

export function map_godot_error_message(p_error: GodotRpcErrorLike): string {
	const code = _coerce_error_code(p_error.code);
	const message = typeof p_error.message === "string" ? p_error.message : "Unknown error";

	switch (code) {
		case -32001:
			return "AUTH_FAILED: Session token rejected. Restart Godot or refresh session.";
		case -32002:
			return "CAPABILITY_DENIED: Requested operation is disabled in current session.";
		case -32003:
			return _with_error_prefix("INVALID_ARGUMENT", message);
		case -32004:
			return _with_error_prefix("CONFLICT", message);
		case -32005:
			return _with_error_prefix("NOT_FOUND", message);
		case -32006:
			return _with_error_prefix("INTERNAL", message);
		case -32700:
			return "PROTOCOL_ERROR: Godot returned parse error.";
		case -32600:
			return "PROTOCOL_ERROR: Godot rejected invalid JSON-RPC request.";
		case -32601:
			return "METHOD_NOT_FOUND: Godot method is unavailable.";
		case -32602:
			return _with_error_prefix("INVALID_ARGUMENT", message);
		default:
			return `GODOT_RPC_ERROR${code !== undefined ? `(${code})` : ""}: ${message}`;
	}
}

export function as_error_text(p_error: unknown): string {
	if (p_error instanceof Error) {
		return p_error.message;
	}
	if (typeof p_error === "string") {
		return p_error;
	}
	try {
		return JSON.stringify(p_error);
	} catch {
		return String(p_error);
	}
}
