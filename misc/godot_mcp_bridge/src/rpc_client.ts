import net from "node:net";
import { StringDecoder } from "node:string_decoder";

import { check_request_limits, RequestCancelledError, with_request_limits, type RequestLimits } from "./request_control.js";

export interface RpcClientOptions {
	host?: string;
	port: number;
	timeout_ms?: number;
}

export interface RpcClientLike {
	get_port(): number;
	call(p_method: string, p_params: Record<string, unknown>, p_options?: RequestLimits): Promise<unknown>;
	close(): Promise<void>;
}

export class RpcTransportError extends Error {
	readonly code: "DISCONNECTED" | "TIMEOUT";
	readonly retryable = true;

	constructor(p_message: string, p_code: "DISCONNECTED" | "TIMEOUT" = "DISCONNECTED") {
		super(p_message);
		this.name = "RpcTransportError";
		this.code = p_code;
	}
}

export class RpcMethodError extends Error {
	readonly code: number;
	readonly data?: unknown;

	constructor(p_code: number, p_message: string, p_data?: unknown) {
		super(p_message);
		this.name = "RpcMethodError";
		this.code = p_code;
		this.data = p_data;
	}
}

interface PendingRequest {
	cleanup: () => void;
	resolve: (p_value: unknown) => void;
	reject: (p_error: unknown) => void;
}

export class GodotRpcClient {
	private readonly host: string;
	private readonly port: number;
	private readonly timeout_ms: number;

	private socket: net.Socket | null = null;
	private connecting_socket: net.Socket | null = null;
	private read_buffer = "";
	private decoder = new StringDecoder("utf8");
	private next_id = 1;
	private connecting: Promise<void> | null = null;
	private pending = new Map<number, PendingRequest>();

	constructor(p_options: RpcClientOptions) {
		this.host = p_options.host ?? "127.0.0.1";
		this.port = p_options.port;
		this.timeout_ms = p_options.timeout_ms ?? 10_000;
	}

	get_port(): number {
		return this.port;
	}

	private _is_socket_connected(): boolean {
		return Boolean(this.socket && !this.socket.destroyed && this.socket.readyState === "open");
	}

	private _handle_message_line(p_line: string): void {
		let parsed: unknown;
		try {
			parsed = JSON.parse(p_line);
		} catch {
			return;
		}

		if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
			return;
		}

		const message = parsed as Record<string, unknown>;
		if (typeof message.id !== "number") {
			return;
		}

		const pending = this.pending.get(message.id);
		if (!pending) {
			return;
		}

		this.pending.delete(message.id);
		pending.cleanup();

		if (message.error && typeof message.error === "object" && !Array.isArray(message.error)) {
			const error = message.error as Record<string, unknown>;
			const code = typeof error.code === "number" ? error.code : -32006;
			const text = typeof error.message === "string" ? error.message : "Unknown RPC error";
			pending.reject(new RpcMethodError(code, text, error.data));
			return;
		}

		pending.resolve(message.result);
	}

	private _on_socket_data = (p_chunk: Buffer | string): void => {
		this.read_buffer += typeof p_chunk === "string" ? p_chunk : this.decoder.write(p_chunk);

		while (true) {
			const eol = this.read_buffer.indexOf("\n");
			if (eol < 0) {
				break;
			}

			const line = this.read_buffer.slice(0, eol).trim();
			this.read_buffer = this.read_buffer.slice(eol + 1);
			if (line.length === 0) {
				continue;
			}
			this._handle_message_line(line);
		}
	};

	private _reject_all_pending(p_error: unknown): void {
		for (const pending of this.pending.values()) {
			pending.cleanup();
			pending.reject(p_error);
		}
		this.pending.clear();
	}

	private _detach_socket(p_error: unknown): void {
		if (this.socket) {
			this.socket.removeListener("data", this._on_socket_data);
			this.socket.removeAllListeners("error");
			this.socket.removeAllListeners("close");
			this.socket.removeAllListeners("end");
			this.socket.destroy();
		}
		this.socket = null;
		this.connecting_socket = null;
		this.connecting = null;
		this.read_buffer = "";
		this.decoder = new StringDecoder("utf8");
		this._reject_all_pending(p_error);
	}

	private async _connect(): Promise<void> {
		if (this._is_socket_connected()) {
			return;
		}
		if (this.connecting) {
			await this.connecting;
			return;
		}

		this.connecting = new Promise<void>((resolve, reject) => {
			const socket = net.createConnection({ host: this.host, port: this.port });
			this.connecting_socket = socket;
			socket.setNoDelay(true);

			const fail = (p_error: unknown) => {
				clearTimeout(connect_timer);
				socket.removeAllListeners("connect");
				socket.removeAllListeners("data");
				socket.removeAllListeners("close");
				socket.removeAllListeners("end");
				socket.removeAllListeners("error");
				socket.destroy();
				if (this.connecting_socket === socket) {
					this.connecting_socket = null;
				}
				this.socket = null;
				this.connecting = null;
				reject(p_error instanceof RpcTransportError ? p_error :
					new RpcTransportError(`Could not connect to Godot RPC (${this.host}:${this.port}). ${String(p_error)}`));
			};
			const connect_timer = setTimeout(() => {
				fail(new RpcTransportError(
					`Timed out connecting to Godot RPC (${this.host}:${this.port}) after ${this.timeout_ms}ms.`,
					"TIMEOUT",
				));
			}, this.timeout_ms);

			socket.once("error", fail);
			socket.once("connect", () => {
				clearTimeout(connect_timer);
				socket.removeListener("error", fail);
				this.connecting_socket = null;

				socket.on("data", this._on_socket_data);
				socket.on("error", (p_error: unknown) => {
					this._detach_socket(new RpcTransportError(`Godot RPC socket error: ${String(p_error)}`));
				});
				socket.on("close", () => {
					this._detach_socket(new RpcTransportError("Godot RPC socket closed."));
				});
				socket.on("end", () => {
					this._detach_socket(new RpcTransportError("Godot RPC socket ended."));
				});

				this.socket = socket;
				this.connecting = null;
				resolve();
			});
		});

		await this.connecting;
	}

	async close(): Promise<void> {
		const connection = this.connecting;
		if (this.connecting_socket) {
			this.connecting_socket.destroy(new RpcTransportError("RPC client closed while connecting."));
			this.connecting_socket = null;
		}
		if (connection) {
			await connection.catch(() => undefined);
		}

		if (!this.socket) {
			return;
		}
		const socket = this.socket;
		this.socket = null;
		this.connecting = null;
		this.read_buffer = "";
		this.decoder = new StringDecoder("utf8");
		socket.removeListener("data", this._on_socket_data);
		socket.removeAllListeners("error");
		socket.removeAllListeners("close");
		socket.removeAllListeners("end");
		socket.destroy();
		this._reject_all_pending(new RpcTransportError("RPC client closed."));
	}

	private async _send_request_once(p_method: string, p_params: Record<string, unknown>, p_options: RequestLimits): Promise<unknown> {
		const limits = {
			signal: p_options.signal,
			deadline_ms: Math.min(p_options.deadline_ms ?? Infinity, Date.now() + this.timeout_ms),
		};
		const timeout_error = (): RpcTransportError => new RpcTransportError(
			`Godot RPC deadline exceeded for method '${p_method}'. An operation already sent may still complete.`, "TIMEOUT",
		);
		await with_request_limits(() => this._connect(), limits, timeout_error);
		check_request_limits(limits, timeout_error);
		if (!this.socket || this.socket.destroyed) {
			throw new RpcTransportError("Socket is not connected.");
		}

		const request_id = this.next_id++;
		const payload = JSON.stringify({
			jsonrpc: "2.0",
			id: request_id,
			method: p_method,
			params: p_params,
		}) + "\n";

		return await new Promise<unknown>((resolve, reject) => {
			const cleanup = (): void => {
				clearTimeout(timer);
				limits.signal?.removeEventListener("abort", abort);
			};
			const fail = (p_error: unknown): void => {
				this.pending.delete(request_id);
				cleanup();
				reject(p_error);
			};
			const abort = (): void => fail(new RequestCancelledError());
			const timer = setTimeout(() => {
				fail(timeout_error());
			}, Math.max(0, limits.deadline_ms - Date.now()));

			this.pending.set(request_id, { cleanup, resolve, reject });
			limits.signal?.addEventListener("abort", abort, { once: true });
			try {
				check_request_limits(limits, timeout_error);
			} catch (error: unknown) {
				fail(error);
				return;
			}

			this.socket?.write(payload, (p_error?: Error | null) => {
				if (!p_error) {
					return;
				}

				const pending = this.pending.get(request_id);
				if (!pending) {
					return;
				}
				this.pending.delete(request_id);
				pending.cleanup();
				pending.reject(new RpcTransportError(`Failed to write RPC request: ${p_error.message}`));
			});
		});
	}

	async call(p_method: string, p_params: Record<string, unknown>, p_options: RequestLimits = {}): Promise<unknown> {
		// Retrying here can execute a mutation twice when the first response is
		// lost. SessionLifecycle owns reconnect policy and retries only tools that
		// are explicitly marked safe.
		return await this._send_request_once(p_method, p_params, p_options);
	}
}
