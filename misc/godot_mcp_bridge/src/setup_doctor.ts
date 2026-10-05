import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

export type LaunchCommand = { command: string; args: string[]; env?: Record<string, string>; cwd?: string };

export interface DoctorReport {
	ok: boolean;
	bridge: { ok: boolean; message: string; tool_count?: number };
	editor: { ok: boolean; message: string; project_path?: string; session_id?: string };
}

function _report(bridge_ok: boolean, bridge_message: string, editor_message: string, tool_count?: number): DoctorReport {
	return {
		ok: bridge_ok,
		bridge: { ok: bridge_ok, message: bridge_message, ...(tool_count === undefined ? {} : { tool_count }) },
		editor: { ok: false, message: editor_message },
	};
}

function _editor_failure(tool_count: number, message: string): DoctorReport {
	return {
		ok: false,
		bridge: { ok: true, message: "Godot MCP bridge started and exposed its tools.", tool_count },
		editor: { ok: false, message },
	};
}

function _object(value: unknown): Record<string, unknown> | undefined {
	return value && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : undefined;
}

function _timeout<T>(promise: Promise<T>, timeout_ms: number, stage: string, abort: AbortController): Promise<T> {
	let timer: NodeJS.Timeout | undefined;
	return Promise.race([
		promise,
		new Promise<T>((_resolve, reject) => {
			timer = setTimeout(() => {
				reject(new Error(`Timed out while ${stage}.`));
				abort.abort();
			}, timeout_ms);
		}),
	]).finally(() => clearTimeout(timer));
}

/**
 * Check that a stdio MCP launch works and, separately, whether its Godot editor
 * endpoint is available. Only initialize, tools/list, and session.info are used.
 */
export async function diagnose_connection(launch: LaunchCommand, timeout_ms = 10_000): Promise<DoctorReport> {
	if (!launch.command || !Array.isArray(launch.args) || launch.args.some((arg) => typeof arg !== "string")) {
		return _report(false, "Invalid MCP launch command.", "Open a Godot project and enable Godot MCP, then retry.");
	}
	const bounded_timeout = Number.isFinite(timeout_ms) && timeout_ms > 0 ? Math.max(1, Math.floor(timeout_ms)) : 10_000;
	const deadline = Date.now() + bounded_timeout;
	const remaining = (stage: string): number => {
		const value = deadline - Date.now();
		if (value <= 0) throw new Error(`Timed out while ${stage}.`);
		return value;
	};
	const abort = new AbortController();
	const inherited_environment: Record<string, string> = {};
	for (const name of ["PATH", "SYSTEMROOT", "WINDIR", "TEMP", "TMP", "APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME"]) {
		const value = process.env[name];
		if (value !== undefined) inherited_environment[name] = value;
	}
	const transport_options: ConstructorParameters<typeof StdioClientTransport>[0] = {
		command: launch.command,
		args: [...launch.args],
		...(launch.cwd ? { cwd: launch.cwd } : {}),
		env: { ...inherited_environment, ...launch.env },
		stderr: "pipe",
	};
	const transport = new StdioClientTransport(transport_options);
	// Consume stderr so the child cannot block on a full pipe. Never retain or
	// report its contents because startup logs may include paths or credentials.
	transport.stderr?.on("data", () => {});
	const client = new Client({ name: "godot-mcp-doctor", version: "1.0.0" }, { capabilities: {} });
	let report: DoctorReport;
	let listed_tool_count: number | undefined;
	try {
		const connect_timeout = remaining("starting the MCP bridge");
		await _timeout(client.connect(transport, { timeout: connect_timeout + 100, signal: abort.signal }), connect_timeout, "starting the MCP bridge", abort);
		const identity = client.getServerVersion();
		if (identity?.name !== "godot-mcp") {
			report = _report(false, "The command started an MCP server, but it did not identify itself as Godot MCP.", "Open a Godot project and enable Godot MCP, then retry.");
			return report;
		}

		const list_timeout = remaining("listing MCP tools");
		const listed = await _timeout(client.listTools(undefined, { timeout: list_timeout + 100, signal: abort.signal }), list_timeout, "listing MCP tools", abort);
		const tools = listed.tools;
		if (!Array.isArray(tools) || tools.length === 0 || !tools.some((tool) => tool.name === "godot.session.info")) {
			report = _report(false, "The Godot MCP bridge started but did not expose the expected session tool.", "Open a Godot project and enable Godot MCP, then retry.", Array.isArray(tools) ? tools.length : 0);
			return report;
		}
		listed_tool_count = tools.length;

		const session_timeout = remaining("checking the Godot session");
		const result = await _timeout(
			client.callTool({ name: "godot.session.info", arguments: {} }, undefined, { timeout: session_timeout + 100, signal: abort.signal }),
			session_timeout,
			"checking the Godot session",
			abort,
		);
		const envelope = _object(result.structuredContent);
		if (result.isError || envelope?.ok !== true) {
			report = _report(false, "The Godot MCP bridge started, but its session check failed.", "Open a Godot project, enable Godot MCP, and retry. If it is already enabled, check the editor log for MCP errors.", tools.length);
			return report;
		}

		const data = _object(envelope.data);
		const bridge_state = _object(data?.bridge);
		const editor_result = _object(data?.editor);
		const editor_data = _object(editor_result?.data);
		if (bridge_state?.state !== "connected" || editor_result?.ok !== true || !editor_data) {
			return _editor_failure(tools.length, "The bridge is healthy, but no Godot editor session answered. Open the project in Godot, enable Godot MCP, and retry.");
		}

		report = {
			ok: true,
			bridge: { ok: true, message: "Godot MCP bridge started and exposed its tools.", tool_count: tools.length },
			editor: {
				ok: true,
				message: "Connected to the Godot editor session.",
				...(typeof editor_data.project_path === "string" ? { project_path: editor_data.project_path } : {}),
				...(typeof bridge_state.session_id === "string" ? { session_id: bridge_state.session_id } : {}),
			},
		};
		return report;
	} catch (error: unknown) {
		const message = error instanceof Error ? error.message : "The MCP process could not be started.";
		const timeout = /timed out/i.test(message);
		if (listed_tool_count !== undefined) {
			return _editor_failure(listed_tool_count,
				timeout
					? "The bridge is healthy, but the Godot editor did not answer its session check in time. Open the project in Godot, enable Godot MCP, and retry."
					: "The bridge is healthy, but its Godot editor session check failed. Open the project in Godot, enable Godot MCP, and retry.",
			);
		}
		report = _report(false,
			timeout ? message : "Could not start or communicate with the Godot MCP bridge. Check the configured command and arguments.",
			"Open a Godot project and enable Godot MCP, then retry.",
		);
		return report;
	} finally {
		// Always close the client. The transport owns the child process, and close
		// ends it on normal completion or timeout. Bound cleanup so doctor itself
		// cannot hang forever on a broken server process.
		abort.abort();
		const closing = client.close().catch(() => {});
		closing.catch(() => {});
		let cleanup_timer: NodeJS.Timeout | undefined;
		await Promise.race([closing, new Promise<void>((resolve) => {
			cleanup_timer = setTimeout(resolve, 1000);
		})]);
		if (cleanup_timer) clearTimeout(cleanup_timer);
	}
}
