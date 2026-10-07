import { GodotRpcClient, RpcTransportError, type RpcClientLike } from "./rpc_client.js";
import { check_request_limits, request_delay, with_request_limits, type RequestLimits } from "./request_control.js";
import {
	get_session_identity,
	load_session,
	type SessionData,
	type SessionLoadOptions,
} from "./session.js";

export type ConnectionState = "disconnected" | "reconnecting" | "connected" | "failed";

export interface SessionLifecycleOptions extends SessionLoadOptions {
	timeout_ms?: number;
	max_reconnect_attempts?: number;
	initial_backoff_ms?: number;
	max_backoff_ms?: number;
	load_session?: (p_options: SessionLoadOptions) => SessionData;
	create_client?: (p_session: SessionData) => RpcClientLike;
	sleep?: (p_delay_ms: number) => Promise<void>;
}

export interface SessionCallOptions extends RequestLimits {
	retry_safe?: boolean;
}

export interface SessionLifecycleInfo {
	state: ConnectionState;
	session_id?: string;
	pid?: number;
	port?: number;
	protocol_version?: number;
	project_path?: string;
	capabilities?: Record<string, unknown>;
	reconnect_attempt: number;
	last_error?: string;
}

export class SessionLifecycleError extends Error {
	readonly code: "STALE_SESSION" | "RECONNECT_FAILED";
	readonly retryable: boolean;

	constructor(p_code: "STALE_SESSION" | "RECONNECT_FAILED", p_message: string, p_retryable = true) {
		super(p_message);
		this.name = "SessionLifecycleError";
		this.code = p_code;
		this.retryable = p_retryable;
	}
}

function _default_sleep(p_delay_ms: number, p_signal?: AbortSignal): Promise<void> {
	return request_delay(p_delay_ms, { signal: p_signal }, () => new RpcTransportError("Session discovery timed out.", "TIMEOUT"));
}

function _extract_handshake_session_id(p_result: unknown): string | undefined {
	if (!p_result || typeof p_result !== "object" || Array.isArray(p_result)) {
		return undefined;
	}
	const result = p_result as Record<string, unknown>;
	const data = result.data;
	if (!data || typeof data !== "object" || Array.isArray(data)) {
		return undefined;
	}
	const session_id = (data as Record<string, unknown>).session_id;
	return typeof session_id === "string" ? session_id : undefined;
}

export class SessionLifecycle {
	private readonly load_options: SessionLoadOptions;
	private readonly timeout_ms: number;
	private readonly max_reconnect_attempts: number;
	private readonly initial_backoff_ms: number;
	private readonly max_backoff_ms: number;
	private readonly session_loader: (p_options: SessionLoadOptions) => SessionData;
	private readonly client_factory: (p_session: SessionData) => RpcClientLike;
	private readonly sleep: (p_delay_ms: number, p_signal?: AbortSignal) => Promise<void>;

	private state: ConnectionState = "disconnected";
	private active_session: SessionData | null = null;
	private active_identity: string | null = null;
	private client: RpcClientLike | null = null;
	private connection_attempt: {
		promise: Promise<{ client: RpcClientLike; session: SessionData }>;
		controller: AbortController;
		waiters: number;
	} | null = null;
	private reconnect_attempt = 0;
	private last_error: string | undefined;

	constructor(p_options: SessionLifecycleOptions = {}) {
		this.load_options = {
			project_root: p_options.project_root,
			session_file: p_options.session_file,
			now_unix: p_options.now_unix,
		};
		this.timeout_ms = p_options.timeout_ms ?? 10_000;
		this.max_reconnect_attempts = Math.max(0, p_options.max_reconnect_attempts ?? 3);
		this.initial_backoff_ms = Math.max(0, p_options.initial_backoff_ms ?? 75);
		this.max_backoff_ms = Math.max(this.initial_backoff_ms, p_options.max_backoff_ms ?? 750);
		this.session_loader = p_options.load_session ?? load_session;
		this.client_factory = p_options.create_client ?? ((p_session) => new GodotRpcClient({
			port: p_session.port,
			timeout_ms: this.timeout_ms,
		}));
		this.sleep = p_options.sleep ?? _default_sleep;
	}

	get_active_session(): SessionData | null {
		return this.active_session;
	}

	get_info(): SessionLifecycleInfo {
		return {
			state: this.state,
			session_id: this.active_session?.session_id,
			pid: this.active_session?.pid,
			port: this.active_session?.port,
			protocol_version: this.active_session?.protocol_version,
			project_path: this.active_session?.project_path,
			capabilities: this.active_session?.capabilities,
			reconnect_attempt: this.reconnect_attempt,
			last_error: this.last_error,
		};
	}

	private async _discard_client(p_state: ConnectionState, p_expected_client?: RpcClientLike): Promise<void> {
		// An old request failing must not close a newly connected client.
		if (p_expected_client && this.client !== p_expected_client) {
			return;
		}
		const client = this.client;
		this.client = null;
		this.active_session = null;
		this.active_identity = null;
		this.state = p_state;
		if (client) {
			await client.close();
		}
	}

	private async _connect_once(p_signal: AbortSignal): Promise<{ client: RpcClientLike; session: SessionData }> {
		const limits = { signal: p_signal };
		check_request_limits(limits, () => new RpcTransportError("Session discovery timed out.", "TIMEOUT"));
		const session = this.session_loader(this.load_options);
		const identity = get_session_identity(session);

		if (this.client && this.active_identity === identity) {
			this.active_session = session;
			this.state = "connected";
			return { client: this.client, session };
		}

		if (this.client) {
			await this._discard_client("reconnecting");
		}

		const candidate = this.client_factory(session);
		try {
			const handshake = await with_request_limits(
				() => candidate.call("godot.session.info", session.session_id ? { _session_id: session.session_id } : {}, limits),
				limits, () => new RpcTransportError("Session handshake timed out.", "TIMEOUT"),
			);
			const handshake_session_id = _extract_handshake_session_id(handshake);
			if (session.session_id && handshake_session_id !== session.session_id) {
				throw new SessionLifecycleError(
					"STALE_SESSION",
					`Session file identifies '${session.session_id}', but the editor endpoint identifies '${handshake_session_id ?? "unknown"}'.`,
				);
			}
		} catch (error: unknown) {
			await candidate.close();
			throw error;
		}

		this.client = candidate;
		this.active_session = session;
		this.active_identity = identity;
		this.state = "connected";
		this.last_error = undefined;
		return { client: candidate, session };
	}

	private async _perform_connect_with_backoff(p_signal: AbortSignal): Promise<{ client: RpcClientLike; session: SessionData }> {
		let last_error: unknown;
		for (let attempt = 0; attempt <= this.max_reconnect_attempts; attempt++) {
			this.reconnect_attempt = attempt;
			this.state = attempt === 0 && !this.active_session ? "disconnected" : "reconnecting";
			try {
				const connection = await this._connect_once(p_signal);
				this.reconnect_attempt = 0;
				return connection;
			} catch (error: unknown) {
				check_request_limits({ signal: p_signal }, () => new RpcTransportError("Session discovery timed out.", "TIMEOUT"));
				last_error = error;
				this.last_error = error instanceof Error ? error.message : String(error);
				if (attempt >= this.max_reconnect_attempts) {
					break;
				}
				this.state = "reconnecting";
				const delay = Math.min(this.max_backoff_ms, this.initial_backoff_ms * (2 ** attempt));
				await with_request_limits(() => this.sleep(delay, p_signal), { signal: p_signal },
					() => new RpcTransportError("Session discovery timed out.", "TIMEOUT"));
			}
		}

		this.state = "failed";
		if (last_error instanceof Error) {
			throw last_error;
		}
		throw new SessionLifecycleError("RECONNECT_FAILED", "Could not discover and connect to a Godot editor session.");
	}

	private async _connect_with_backoff(p_options: RequestLimits = {}): Promise<{ client: RpcClientLike; session: SessionData }> {
		const timeout_error = (): RpcTransportError => new RpcTransportError("Session discovery exceeded the request deadline.", "TIMEOUT");
		check_request_limits(p_options, timeout_error);
		if (!this.connection_attempt) {
			const controller = new AbortController();
			const attempt = { controller, waiters: 0, promise: this._perform_connect_with_backoff(controller.signal) };
			this.connection_attempt = attempt;
			void attempt.promise.finally(() => {
				if (this.connection_attempt === attempt) {
					this.connection_attempt = null;
				}
			}).catch(() => undefined);
		}
		const attempt = this.connection_attempt;
		attempt.waiters++;
		try {
			return await with_request_limits(() => attempt.promise, p_options, timeout_error);
		} finally {
			attempt.waiters--;
			if (attempt.waiters === 0 && this.connection_attempt === attempt) {
				this.connection_attempt = null;
				attempt.controller.abort();
				this.state = this.client ? "connected" : "disconnected";
			}
		}
	}

	async refresh(p_options: RequestLimits = {}): Promise<SessionLifecycleInfo> {
		await this._connect_with_backoff(p_options);
		return this.get_info();
	}

	async call(p_method: string, p_params: Record<string, unknown>, p_options: SessionCallOptions = {}): Promise<unknown> {
		const timeout_error = (): RpcTransportError => new RpcTransportError(`Request deadline exceeded for '${p_method}'. An operation already sent may still complete.`, "TIMEOUT");
		let connection = await this._connect_with_backoff(p_options);
		const params = { ...p_params };
		if (connection.session.session_id) {
			params._session_id = connection.session.session_id;
		}

		try {
			return await with_request_limits(() => connection.client.call(p_method, params, p_options), p_options, timeout_error);
		} catch (error: unknown) {
			if (!(error instanceof RpcTransportError)) {
				throw error;
			}
			check_request_limits(p_options, timeout_error);

			this.last_error = error.message;
			await with_request_limits(() => this._discard_client("disconnected", connection.client), p_options, timeout_error);
			if (!p_options.retry_safe) {
				throw error;
			}

			connection = await this._connect_with_backoff(p_options);
			const retry_params = { ...p_params };
			if (connection.session.session_id) {
				retry_params._session_id = connection.session.session_id;
			}
			return await with_request_limits(() => connection.client.call(p_method, retry_params, p_options), p_options, timeout_error);
		}
	}

	async close(): Promise<void> {
		const attempt = this.connection_attempt;
		attempt?.controller.abort();
		if (attempt) {
			await attempt.promise.catch(() => undefined);
		}
		await this._discard_client("disconnected");
		this.last_error = undefined;
		this.reconnect_attempt = 0;
	}
}
