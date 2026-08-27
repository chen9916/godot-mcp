import path from "node:path";

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";

import { as_error_text } from "./error_map.js";
import { install_opencode_config } from "./opencode_user_config.js";
import { is_async_runtime_method, wait_for_filesystem, wait_for_runtime_operation } from "./operation_waiter.js";
import { to_mcp_error, to_structured_error } from "./protocol_error.js";
import { session_has_capability } from "./session.js";
import { SessionLifecycle } from "./session_lifecycle.js";
import { get_tool_definition, get_tool_list_for_mcp, is_tool_retry_safe } from "./tool_registry.js";

interface CliOptions {
	project_root?: string;
	session_file?: string;
	config_path?: string;
	timeout_ms: number;
	install_opencode_config: boolean;
	help: boolean;
}

function _parse_cli_options(p_argv: string[]): CliOptions {
	const options: CliOptions = {
		timeout_ms: 10_000,
		install_opencode_config: false,
		help: false,
	};

	for (let i = 0; i < p_argv.length; i++) {
		const arg = p_argv[i];
		switch (arg) {
			case "--project-root": {
				i++;
				if (i >= p_argv.length) {
					throw new Error("--project-root requires a path value.");
				}
				options.project_root = path.resolve(p_argv[i]);
			} break;
			case "--session-file": {
				i++;
				if (i >= p_argv.length) {
					throw new Error("--session-file requires a path value.");
				}
				options.session_file = path.resolve(p_argv[i]);
			} break;
			case "--config-path": {
				i++;
				if (i >= p_argv.length) {
					throw new Error("--config-path requires a path value.");
				}
				options.config_path = path.resolve(p_argv[i]);
			} break;
			case "--timeout-ms": {
				i++;
				if (i >= p_argv.length) {
					throw new Error("--timeout-ms requires a numeric value.");
				}
				const value = Number.parseInt(p_argv[i], 10);
				if (!Number.isFinite(value) || value <= 0) {
					throw new Error("--timeout-ms must be a positive integer.");
				}
				options.timeout_ms = value;
			} break;
			case "--install-opencode-config": {
				options.install_opencode_config = true;
			} break;
			case "--help":
			case "-h": {
				options.help = true;
			} break;
			default:
				throw new Error(`Unknown argument: ${arg}`);
		}
	}

	return options;
}

function _print_help(): void {
	const help_text = [
		"OpenCode Godot MCP bridge",
		"",
		"Usage:",
		"  node dist/main.js [options]",
		"",
		"Options:",
		"  --project-root <path>          Optional project root for session discovery (auto-detected by default).",
		"  --session-file <path>          Explicit session file path override.",
		"  --timeout-ms <number>          RPC timeout in milliseconds (default: 10000).",
		"  --install-opencode-config      Upsert mcp.godot-opencode in OpenCode user config and exit.",
		"  --config-path <path>           Optional override for OpenCode config file path.",
		"  --help                         Show this help text.",
	].join("\n");

	process.stderr.write(`${help_text}\n`);
}

function _to_mcp_success(p_payload: unknown): Record<string, unknown> {
	const text = typeof p_payload === "string" ? p_payload : JSON.stringify(p_payload, null, 2);
	return {
		content: [{ type: "text", text }],
		structuredContent: p_payload,
		isError: false,
	};
}

async function _run_config_install(p_options: CliOptions): Promise<void> {
	const bridge_script_path = path.resolve(process.argv[1]);
	const result = await install_opencode_config({
		project_root: p_options.project_root,
		bridge_script_path,
		config_path: p_options.config_path,
		session_file: p_options.session_file,
	});

	process.stderr.write(`Updated OpenCode config: ${result.config_path}\n`);
	if (result.backup_path) {
		process.stderr.write(`Backup created: ${result.backup_path}\n`);
	}
	if (!result.changed) {
		process.stderr.write(`Entry '${result.entry_key}' already up to date.\n`);
	} else {
		process.stderr.write(`Entry '${result.entry_key}' installed. Restart OpenCode to pick up MCP tools.\n`);
	}
}

async function _run_mcp_server(p_options: CliOptions): Promise<void> {
	const server = new Server(
		{
			name: "godot-opencode-bridge",
			version: "0.1.0",
		},
		{
			capabilities: {
				tools: {},
			},
		},
	);

	const lifecycle = new SessionLifecycle({
		project_root: p_options.project_root,
		session_file: p_options.session_file,
		timeout_ms: p_options.timeout_ms,
	});

	server.setRequestHandler(ListToolsRequestSchema, async () => {
		return {
			tools: get_tool_list_for_mcp(),
		};
	});

	server.setRequestHandler(CallToolRequestSchema, async (p_request) => {
		const tool_name = p_request.params.name;
		const tool = get_tool_definition(tool_name);
		if (!tool) {
			return to_mcp_error({
				code: "METHOD_NOT_FOUND",
				message: `Unknown tool: ${tool_name}`,
				operation: tool_name,
				retryable: false,
			});
		}

		const args_variant = p_request.params.arguments ?? {};
		if (!args_variant || typeof args_variant !== "object" || Array.isArray(args_variant)) {
			return to_mcp_error({
				code: "INVALID_ARGUMENT",
				message: "Tool arguments must be an object.",
				operation: tool.method,
				retryable: false,
			});
		}

		const args = { ...(args_variant as Record<string, unknown>) };
		delete args.token;
		delete args._token;

		try {
			if (tool.method === "opencode.session.info") {
				try {
					await lifecycle.refresh();
					const editor = await lifecycle.call(tool.method, args, { retry_safe: true });
					return _to_mcp_success({
						ok: true,
						data: {
							bridge: lifecycle.get_info(),
							editor,
						},
						warnings: [],
					});
				} catch (error: unknown) {
					return _to_mcp_success({
						ok: true,
						data: {
							bridge: lifecycle.get_info(),
							editor: null,
						},
						warnings: [to_structured_error(error, tool.method)],
					});
				}
			}

			await lifecycle.refresh();
			const session = lifecycle.get_active_session();
			if (!session) {
				throw new Error("Session lifecycle reported connected without active session metadata.");
			}

			if (!session_has_capability(session, tool.capability)) {
				return to_mcp_error({
					code: "CAPABILITY_DENIED",
					message: `${tool.capability} is disabled by current Godot session.`,
					operation: tool.method,
					session_id: session.session_id,
					run_id: typeof args.run_id === "string" ? args.run_id : undefined,
					retryable: false,
				});
			}

			if (session.token) {
				args.token = session.token;
			}

			const result = tool.method === "filesystem.wait"
				? await wait_for_filesystem(lifecycle, args)
				: is_async_runtime_method(tool.method)
					? await wait_for_runtime_operation(lifecycle, tool.method, args)
					: await lifecycle.call(tool.method, args, { retry_safe: is_tool_retry_safe(tool) });
			return _to_mcp_success(result);
		} catch (error: unknown) {
			const session = lifecycle.get_active_session();
			return to_mcp_error(to_structured_error(error, tool.method, {
				session_id: session?.session_id,
				run_id: typeof args.run_id === "string" ? args.run_id : undefined,
			}));
		}
	});

	const transport = new StdioServerTransport();
	await server.connect(transport);

	const shutdown = async (): Promise<void> => {
		await lifecycle.close();
		process.exit(0);
	};

	process.on("SIGINT", () => {
		void shutdown();
	});
	process.on("SIGTERM", () => {
		void shutdown();
	});
}

async function _main(): Promise<void> {
	const options = _parse_cli_options(process.argv.slice(2));

	if (options.help) {
		_print_help();
		return;
	}

	if (options.install_opencode_config) {
		await _run_config_install(options);
		return;
	}

	await _run_mcp_server(options);
}

void _main().catch((error: unknown) => {
	process.stderr.write(`Fatal bridge error: ${as_error_text(error)}\n`);
	process.exit(1);
});
