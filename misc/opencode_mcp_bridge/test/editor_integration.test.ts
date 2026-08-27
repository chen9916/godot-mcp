import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";

import { wait_for_filesystem, wait_for_runtime_operation } from "../src/operation_waiter.js";
import { SessionLifecycle } from "../src/session_lifecycle.js";

function unwrap_data(p_result: unknown): Record<string, unknown> {
	assert.ok(p_result && typeof p_result === "object" && !Array.isArray(p_result));
	const envelope = p_result as Record<string, unknown>;
	assert.equal(envelope.ok, true);
	assert.ok(envelope.data && typeof envelope.data === "object" && !Array.isArray(envelope.data));
	return envelope.data as Record<string, unknown>;
}

function md5_file(p_path: string): string {
	return createHash("md5").update(fs.readFileSync(p_path)).digest("hex");
}

async function wait_for_runtime_ready(p_lifecycle: SessionLifecycle, p_run_id: string): Promise<Record<string, unknown>> {
	const deadline = Date.now() + 20_000;
	while (Date.now() < deadline) {
		const status = unwrap_data(await p_lifecycle.call("runtime.status", { run_id: p_run_id }, { retry_safe: true }));
		if (status.runtime_connected === true && (status.state === "running" || status.state === "paused")) {
			return status;
		}
		if (status.state === "crashed" || status.state === "stopped") {
			assert.fail(`Runtime stopped before its debugger handshake: ${JSON.stringify(status)}`);
		}
		await new Promise((resolve) => setTimeout(resolve, 50));
	}
	assert.fail("Runtime debugger did not become ready within 20 seconds.");
}

const integration_enabled = process.env.GODOT_MCP_INTEGRATION === "1";

test("live editor workflow covers validation, SVG import, reload, runtime interaction, capture, and run diagnostics", {
	skip: integration_enabled ? false : "Set GODOT_MCP_INTEGRATION=1 and GODOT_MCP_INTEGRATION_PROJECT to run live editor tests.",
	timeout: 120_000,
}, async (t) => {
	const configured_project = process.env.GODOT_MCP_INTEGRATION_PROJECT;
	assert.ok(configured_project, "GODOT_MCP_INTEGRATION_PROJECT is required.");
	const project_root = path.resolve(configured_project);
	assert.ok(fs.existsSync(path.join(project_root, "project.godot")), "Integration project must contain project.godot.");

	const fixture_name = `mcp_integration_tmp_${process.pid}`;
	const fixture_dir = path.join(project_root, fixture_name);
	const relative_fixture = path.relative(project_root, fixture_dir);
	assert.ok(relative_fixture && !relative_fixture.startsWith("..") && !path.isAbsolute(relative_fixture));
	assert.equal(fs.existsSync(fixture_dir), false, `Refusing to overwrite existing fixture directory: ${fixture_dir}`);

	const resource_dir = `res://${fixture_name}`;
	const script_path = `${resource_dir}/runtime_probe.gd`;
	const scene_path = `${resource_dir}/runtime_probe.tscn`;
	const svg_path = `${resource_dir}/fresh.svg`;
	const script_absolute = path.join(fixture_dir, "runtime_probe.gd");
	let active_run_id: string | undefined;
	let captured_path: string | undefined;
	const lifecycle = new SessionLifecycle({ project_root, timeout_ms: 10_000 });

	fs.mkdirSync(fixture_dir);
	fs.writeFileSync(path.join(fixture_dir, "fresh.svg"), [
		'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">',
		'<rect width="8" height="8" fill="#55aaff"/>',
		"</svg>",
	].join("\n"));
	fs.writeFileSync(script_absolute, [
		"extends Node",
		"var physics_ticks: int = 0",
		"var input_seen: bool = false",
		"func _ready() -> void:",
		'\tpush_error("MCP integration run marker")',
		"func _physics_process(_delta: float) -> void:",
		"\tphysics_ticks += 1",
		'\tif Input.is_action_pressed("ui_accept"):',
		"\t\tinput_seen = true",
		"",
	].join("\n"));
	fs.writeFileSync(path.join(fixture_dir, "runtime_probe.tscn"), [
		"[gd_scene load_steps=2 format=3]",
		"",
		`[ext_resource type="Script" path="${script_path}" id="1_script"]`,
		"",
		'[node name="MCPIntegration" type="Node"]',
		'script = ExtResource("1_script")',
		"",
	].join("\n"));

	t.after(async () => {
		if (active_run_id) {
			try {
				await lifecycle.call("runtime.stop", { run_id: active_run_id, confirm: true });
			} catch {
				// The run may already be stopped or the editor may be closing.
			}
		}
		if (captured_path && fs.existsSync(captured_path)) {
			fs.unlinkSync(captured_path);
		}
		fs.rmSync(fixture_dir, { recursive: true, force: true });
		try {
			unwrap_data(await lifecycle.call("filesystem.scan", {}));
			await wait_for_filesystem(lifecycle, { timeout_ms: 10_000 });
		} catch {
			// Cleanup must still succeed when the editor exits during the test.
		}
		await lifecycle.close();
	});

	await lifecycle.refresh();
	const original_session_id = lifecycle.get_info().session_id;
	assert.ok(original_session_id);

	const valid = unwrap_data(await lifecycle.call("script.validate", { script_path }, { retry_safe: true }));
	assert.equal(valid.parse_success, true);
	assert.equal(valid.source_origin, "disk");
	const invalid = unwrap_data(await lifecycle.call("script.validate", {
		script_path,
		source: "extends Node\nfunc broken(\n",
	}, { retry_safe: true }));
	assert.equal(invalid.parse_success, false);
	assert.ok(Array.isArray(invalid.diagnostics) && invalid.diagnostics.length > 0);
	assert.equal(lifecycle.get_info().session_id, original_session_id, "In-process validation must not invalidate the editor session.");

	const scan = unwrap_data(await lifecycle.call("filesystem.scan", {}));
	await wait_for_filesystem(lifecycle, { scan_id: scan.scan_id, timeout_ms: 20_000 });
	const imported = unwrap_data(await lifecycle.call("resource.import", {
		paths: [svg_path, script_path, scene_path],
		atomic: true,
	}));
	const import_items = imported.items as Array<Record<string, unknown>>;
	assert.equal(import_items.length, 3);
	assert.ok(import_items.every((item) => item.ok === true), JSON.stringify(imported));
	const svg_import = import_items.find((item) => item.path === svg_path);
	assert.ok(svg_import && (svg_import.value as Record<string, unknown>).loadable === true, "Fresh SVG must load after synchronization.");

	const svg_status = unwrap_data(await lifecycle.call("resource.status", { path: svg_path, load: true }, { retry_safe: true }));
	assert.equal(svg_status.loaded, true);
	assert.equal((svg_status.importer_status as Record<string, unknown>).imported, true);

	unwrap_data(await lifecycle.call("resource.status", { path: script_path, load: true }, { retry_safe: true }));
	fs.appendFileSync(script_absolute, "# reload version two\n");
	const changed_version = md5_file(script_absolute);
	const reloaded = unwrap_data(await lifecycle.call("resource.reload", {
		path: script_path,
		expected_version: changed_version,
	}));
	assert.equal(reloaded.reload_verified, true);
	assert.equal((reloaded.after as Record<string, unknown>).disk_version, changed_version);

	const settings = unwrap_data(await lifecycle.call("project.get_setting", {
		keys: ["application/config/name", "mcp/integration/missing_setting"],
		atomic: false,
	}, { retry_safe: true }));
	assert.equal(settings.success_count, 1);
	assert.equal(settings.failure_count, 1);

	const first_start = unwrap_data(await lifecycle.call("runtime.start", { mode: "scene", scene_path }));
	active_run_id = String(first_start.run_id);
	await wait_for_runtime_ready(lifecycle, active_run_id);
	const first_sequence = Number(first_start.diagnostics_sequence_start);
	await wait_for_runtime_operation(lifecycle, "runtime.advance_frames", {
		run_id: active_run_id, frames: 3, frame_type: "physics", timeout_ms: 10_000,
	});
	const runtime_tree = unwrap_data(await wait_for_runtime_operation(lifecycle, "runtime.get_tree", {
		run_id: active_run_id, root_path: "/root/MCPIntegration", max_depth: 2, max_nodes: 20,
	}));
	assert.equal((runtime_tree.tree as Record<string, unknown>).path, "/root/MCPIntegration");
	const ticks = unwrap_data(await wait_for_runtime_operation(lifecycle, "runtime.get_property", {
		run_id: active_run_id, node_path: "/root/MCPIntegration", property_name: "physics_ticks",
	}));
	assert.ok(Number(ticks.value) >= 3);
	await wait_for_runtime_operation(lifecycle, "runtime.input_action", {
		run_id: active_run_id, action: "ui_accept", pressed: true, hold_frames: 2,
	});
	const input_seen = unwrap_data(await wait_for_runtime_operation(lifecycle, "runtime.get_property", {
		run_id: active_run_id, node_path: "/root/MCPIntegration", property_name: "input_seen",
	}));
	assert.equal(input_seen.value, true);
	const capture = unwrap_data(await wait_for_runtime_operation(lifecycle, "runtime.capture_viewport", {
		run_id: active_run_id, viewport: "/root", scale: 0.25, frame_wait: 1, timeout_ms: 20_000,
	}));
	assert.ok(Number(capture.width) > 0 && Number(capture.height) > 0);
	captured_path = String(capture.path);
	assert.ok(fs.existsSync(captured_path));

	const first_run_id = active_run_id;
	unwrap_data(await lifecycle.call("runtime.stop", { run_id: first_run_id, confirm: true }));
	active_run_id = undefined;
	const first_diagnostics = unwrap_data(await lifecycle.call("diagnostics.get", {
		run_id: first_run_id, after_sequence: first_sequence,
	}, { retry_safe: true }));
	assert.ok((first_diagnostics.messages as Array<Record<string, unknown>>).every((message) => message.run_id === first_run_id));

	const second_start = unwrap_data(await lifecycle.call("runtime.start", { mode: "scene", scene_path }));
	active_run_id = String(second_start.run_id);
	assert.notEqual(active_run_id, first_run_id);
	await wait_for_runtime_ready(lifecycle, active_run_id);
	await wait_for_runtime_operation(lifecycle, "runtime.advance_frames", { run_id: active_run_id, frames: 1 });
	const second_run_id = active_run_id;
	unwrap_data(await lifecycle.call("runtime.stop", { run_id: second_run_id, confirm: true }));
	active_run_id = undefined;
	const second_diagnostics = unwrap_data(await lifecycle.call("diagnostics.get", {
		run_id: second_run_id,
		after_sequence: Number(second_start.diagnostics_sequence_start),
	}, { retry_safe: true }));
	assert.ok((second_diagnostics.messages as Array<Record<string, unknown>>).every((message) => message.run_id === second_run_id));
});
