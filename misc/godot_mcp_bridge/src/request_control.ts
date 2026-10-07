export interface RequestLimits {
	signal?: AbortSignal;
	deadline_ms?: number;
}

export class RequestCancelledError extends Error {
	readonly code = "CANCELLED";
	readonly retryable = false;

	constructor() {
		super("Request cancelled. An operation already sent to the editor may still complete.");
		this.name = "RequestCancelledError";
	}
}

export function check_request_limits(p_limits: RequestLimits, p_timeout_error: () => Error): void {
	if (p_limits.signal?.aborted) {
		throw new RequestCancelledError();
	}
	if (p_limits.deadline_ms !== undefined && Date.now() >= p_limits.deadline_ms) {
		throw p_timeout_error();
	}
}

// Bound the caller even when a custom RpcClientLike does not implement signals.
// The operation receives the same limits separately so real transports clean up
// their pending request, rather than merely abandoning its promise.
export async function with_request_limits<T>(
	p_operation: () => Promise<T>,
	p_limits: RequestLimits,
	p_timeout_error: () => Error,
): Promise<T> {
	check_request_limits(p_limits, p_timeout_error);
	return await new Promise<T>((resolve, reject) => {
		let timer: NodeJS.Timeout | undefined;
		const cleanup = (): void => {
			if (timer) {
				clearTimeout(timer);
			}
			p_limits.signal?.removeEventListener("abort", abort);
		};
		const abort = (): void => {
			cleanup();
			reject(new RequestCancelledError());
		};
		p_limits.signal?.addEventListener("abort", abort, { once: true });
		if (p_limits.deadline_ms !== undefined) {
			timer = setTimeout(() => {
				cleanup();
				reject(p_timeout_error());
			}, Math.max(0, p_limits.deadline_ms - Date.now()));
		}
		Promise.resolve().then(() => {
			check_request_limits(p_limits, p_timeout_error);
			return p_operation();
		}).then((result) => {
			cleanup();
			try {
				check_request_limits(p_limits, p_timeout_error);
				resolve(result);
			} catch (error: unknown) {
				reject(error);
			}
		}, (error: unknown) => {
			cleanup();
			reject(error);
		});
	});
}

export async function request_delay(p_delay_ms: number, p_limits: RequestLimits, p_timeout_error: () => Error): Promise<void> {
	let timer: NodeJS.Timeout | undefined;
	try {
		await with_request_limits(() => new Promise<void>((resolve) => {
			timer = setTimeout(resolve, p_delay_ms);
		}), p_limits, p_timeout_error);
	} finally {
		if (timer) {
			clearTimeout(timer);
		}
	}
}
