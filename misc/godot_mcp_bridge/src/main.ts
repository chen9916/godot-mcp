#!/usr/bin/env node
import path from "node:path";

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";
import { AjvJsonSchemaValidator } from "@modelcontextprotocol/sdk/validation/ajv";
import type { JsonSchemaType, JsonSchemaValidator } from "@modelcontextprotocol/sdk/validation";

import { as_error_text } from "./error_map.js";
import { bridge_command } from "./connection_config.js";
import { is_async_runtime_method, operation_timeout_ms, wait_for_filesystem, wait_for_runtime_operation } from "./operation_waiter.js";
import { to_mcp_error, to_structured_error } from "./protocol_error.js";
import { session_has_capability } from "./session.js";
import { SessionLifecycle } from "./session_lifecycle.js";
import { get_animation_schema, get_scene_patch_schema_document, get_tool_definition, get_tool_list_for_mcp, is_tool_retry_safe } from "./tool_registry.js";

interface CliOptions {
	project_root?: string;
	session_file?: string;
	timeout_ms: number;
	print_config?: "mcp" | "connection";
	help: boolean;
}

function _parse_cli_options(p_argv: string[]): CliOptions {
	const options: CliOptions = {
		timeout_ms: 10_000,
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
			case "--print-config": {
				const format = p_argv[++i];
				if (format !== "mcp" && format !== "connection") {
					throw new Error("--print-config requires mcp or connection.");
				}
				if (options.print_config) {
					throw new Error("Choose one print action per command.");
				}
				options.print_config = format;
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
		"Godot MCP bridge (standard stdio; works with any compatible MCP client)",
		"",
		"Usage:",
		"  node dist/main.js [options]",
		"  godot-mcp setup|config|doctor|remove|clients --help",
		"",
		"Options:",
		"  --project-root <path>          Optional project root for session discovery (auto-detected by default).",
		"  --session-file <path>          Explicit session file path override.",
		"  --timeout-ms <number>          RPC timeout in milliseconds (default: 10000).",
		"  --print-config <format>       Print mcp (mcpServers JSON) or connection (command/args JSON), then exit.",
		"  --help                         Show this help text.",
	].join("\n");

	process.stderr.write(`${help_text}\n`);
}

function _to_mcp_success(p_payload: unknown): Record<string, unknown> {
	const text = typeof p_payload === "string" ? p_payload : JSON.stringify(p_payload, null, 2);
	return {
		content: [{ type: "text", text }],
		...(p_payload && typeof p_payload === "object" && !Array.isArray(p_payload) ? { structuredContent: p_payload } : {}),
		isError: false,
	};
}

async function _run_mcp_server(p_options: CliOptions): Promise<void> {
	const server = new Server(
		{
			name: "godot-mcp",
			version: "0.3.0",
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
	const animationSchemaValidator = new AjvJsonSchemaValidator();
	const animationValidators = new Map<string, JsonSchemaValidator<unknown>>();

	server.setRequestHandler(ListToolsRequestSchema, async () => {
		return {
			tools: get_tool_list_for_mcp(),
		};
	});

	server.setRequestHandler(CallToolRequestSchema, async (p_request, p_extra) => {
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
			if (tool.name.startsWith("godot.animation.") || tool.name === "godot.runtime.animation_get" || tool.name === "godot.runtime.animation_control") {
				let validate = animationValidators.get(tool.name);
				if (!validate) {
					validate = animationSchemaValidator.getValidator(tool.inputSchema as JsonSchemaType);
					animationValidators.set(tool.name, validate);
				}
				const validation = validate(args);
				if (!validation.valid) {
					return to_mcp_error({
						code: "INVALID_ARGUMENT", message: validation.errorMessage, operation: tool.method, retryable: false,
					});
				}
			}
			const limits = {
				signal: p_extra.signal,
				...(tool.method === "filesystem.wait" || is_async_runtime_method(tool.method)
					? { deadline_ms: Date.now() + operation_timeout_ms(args) } : {}),
			};
			if (tool.method === "bridge.scene.patch_schema") {
				return _to_mcp_success({ ok: true, data: get_scene_patch_schema_document(), warnings: [] });
			}
			if (tool.method === "bridge.animation.schema") {
				return _to_mcp_success({ ok: true, data: get_animation_schema(), warnings: [] });
			}
			if (tool.method === "godot.session.info") {
				try {
					await lifecycle.refresh(limits);
					const editor = await lifecycle.call(tool.method, args, { ...limits, retry_safe: true });
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

			await lifecycle.refresh(limits);
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
				? await wait_for_filesystem(lifecycle, args, limits)
				: is_async_runtime_method(tool.method)
					? await wait_for_runtime_operation(lifecycle, tool.method, args, undefined, limits)
					: await lifecycle.call(tool.method, args, { ...limits, retry_safe: is_tool_retry_safe(tool) });
			return _to_mcp_success(result);
		} catch (error: unknown) {
			const session = lifecycle.get_active_session();
			return to_mcp_error(to_structured_error(error, tool.method, {
				session_id: session?.session_id,
				run_id: typeof args.run_id === "string" ? args.run_id : undefined,
			}));
		}
	});

	server.onclose = () => { void lifecycle.close(); };
	const transport = new StdioServerTransport();
	await server.connect(transport);

	const shutdown = async (): Promise<void> => {
		await lifecycle.close();
		process.exit(0);
	};

	// A stdio client owns this process. Release the editor connection on EOF.
	process.stdin.once("end", () => { void shutdown(); });
	process.on("SIGINT", () => {
		void shutdown();
	});
	process.on("SIGTERM", () => {
		void shutdown();
	});
}

async function _main(): Promise<void> {
	const args = process.argv.slice(2);
	if (["setup", "config", "doctor", "remove", "clients"].includes(args[0])) {
		const { run_setup_cli } = await import("./setup.js");
		await run_setup_cli(args, path.resolve(process.argv[1]));
		return;
	}
	const options = _parse_cli_options(args[0] === "serve" ? args.slice(1) : args);

	if (options.help) {
		_print_help();
		return;
	}

	if (options.print_config) {
		const launch = bridge_command({ ...options, bridge_script_path: path.resolve(process.argv[1]) });
		const config = options.print_config === "connection" ? launch : { mcpServers: { godot: launch } };
		process.stdout.write(JSON.stringify(config, null, 2) + "\n");
		return;
	}

	await _run_mcp_server(options);
}

void _main().catch((error: unknown) => {
	process.stderr.write(`Fatal bridge error: ${as_error_text(error)}\n`);
	process.exit(1);
});
