import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";

import { SessionLifecycle } from "../src/session_lifecycle.js";

function data(p_result: unknown): Record<string, unknown> {
	assert.ok(p_result && typeof p_result === "object");
	const envelope = p_result as Record<string, unknown>;
	assert.equal(envelope.ok, true);
	assert.ok(envelope.data && typeof envelope.data === "object");
	return envelope.data as Record<string, unknown>;
}

test("live scene patches preserve saved subtrees, root renames, JSON integers, and node paths", {
	skip: process.env.GODOT_MCP_SCENE_PATCH_INTEGRATION === "1" ? false :
		"Set GODOT_MCP_SCENE_PATCH_INTEGRATION=1 and GODOT_MCP_INTEGRATION_PROJECT to run scene patch regressions.",
	timeout: 60_000,
}, async (t) => {
	const configured_project = process.env.GODOT_MCP_INTEGRATION_PROJECT;
	assert.ok(configured_project, "Use a disposable project with MCP enabled.");
	const project_root = path.resolve(configured_project);
	assert.ok(fs.existsSync(path.join(project_root, "project.godot")));
	const fixture_dir = fs.mkdtempSync(path.join(project_root, "mcp_scene_patch_"));
	assert.equal(path.dirname(fixture_dir), project_root);
	const scene_path = "res://" + path.basename(fixture_dir) + "/main.tscn";
	const scene_file = path.join(fixture_dir, "main.tscn");
	const lifecycle = new SessionLifecycle({ project_root, timeout_ms: 10_000 });
	t.after(async () => {
		await lifecycle.close();
		fs.rmSync(fixture_dir, { recursive: true, force: true });
	});
	fs.writeFileSync(scene_file, [
		"[gd_scene format=3]",
		'[node name="Root" type="Node3D"]',
		"metadata/bytes = PackedByteArray()",
		"metadata/int32 = PackedInt32Array()",
		"metadata/int64 = PackedInt64Array()",
		'[node name="A" type="Node3D" parent="."]',
		'[node name="Branch" type="Node3D" parent="A"]',
		'[node name="Leaf" type="Node3D" parent="A/Branch"]',
		'[node name="B" type="Node3D" parent="."]',
		"",
	].join("\n"));
	await lifecycle.call("scene.open", { path: scene_path });
	// Opening a scene is deferred by the editor.
	const deadline = Date.now() + 10_000;
	let active: Record<string, unknown> | undefined;
	while (Date.now() < deadline) {
		try {
			active = data(await lifecycle.call("scene.get_active", {}, { retry_safe: true }));
			if (active.path === scene_path) {
				break;
			}
		} catch {
			// No scene is active until the deferred open completes.
		}
		await new Promise((resolve) => setTimeout(resolve, 50));
	}
	assert.equal(active?.path, scene_path);
	const patch = async (operations: unknown[], options: Record<string, unknown> = {}) => {
		const current = data(await lifecycle.call("scene.get_active", {}, { retry_safe: true }));
		return data(await lifecycle.call("scene.apply_patch", {
			expected_scene_path: scene_path,
			expected_scene_version: current.scene_version,
			operations,
			...options,
		}));
	};

	const preview = await patch([{ op: "update_node", node: ".", name: "RenamedRoot" }], { dry_run: true });
	assert.equal(preview.applied, false);
	assert.equal(data(await lifecycle.call("scene.get_active", {}, { retry_safe: true })).name, "Root");
	assert.equal((await patch([{ op: "update_node", node: ".", name: "RenamedRoot" }], { save: true })).save_status, "saved");
	assert.equal(data(await lifecycle.call("scene.get_active", {}, { retry_safe: true })).name, "RenamedRoot");

	assert.equal((await patch([{ op: "move_node", node: "A/Branch", parent: "B" }], { save: true })).save_status, "saved");
	assert.match(fs.readFileSync(scene_file, "utf8"), /\[node name="Leaf" [^\n]*parent="B\/Branch"/);

	const integer_result = await patch([
		{ op: "create_node", id: "positioned", parent: ".", name: "Positioned", type: "Node3D", position: 0 },
		{ op: "update_node", node: ".", properties: {
			"metadata/bytes": { __type: "PackedByteArray", value: [0, 255] },
			"metadata/int32": { __type: "PackedInt32Array", value: [-2147483648, 2147483647] },
			"metadata/int64": { __type: "PackedInt64Array", value: [-4294967296, 4294967296] },
		} },
	], { save: true });
	assert.equal(integer_result.success_count, 2);
	assert.equal(integer_result.save_status, "saved");
	await assert.rejects(patch([
		{ op: "create_node", id: "invalid", parent: ".", name: "Invalid", type: "Node3D", position: 0.5 },
	]), /position must be an integer/);

	const before_collision = data(await lifecycle.call("scene.get_active", {}, { retry_safe: true }));
	await assert.rejects(patch([
		{ op: "update_node", node: "A", properties: { name: "RenamedA" } },
		{ op: "create_node", id: "collision", parent: ".", name: "RenamedA", type: "Node3D" },
	]), /already has a child named/);
	assert.equal(data(await lifecycle.call("scene.get_active", {}, { retry_safe: true })).scene_version, before_collision.scene_version);
	const renamed = await patch([
		{ op: "update_node", node: "A", properties: { name: "RenamedA" } },
		{ op: "create_node", id: "replacement", parent: ".", type: "Node3D", properties: { name: "A" } },
	], { save: true });
	assert.equal((renamed.created_node_paths as Record<string, unknown>).replacement, "A");
	assert.equal(data(await lifecycle.call("node.get_property", { node_path: "RenamedA", property_name: "name" }, { retry_safe: true })).value, "RenamedA");
	assert.equal(data(await lifecycle.call("node.get_property", { node_path: "A", property_name: "name" }, { retry_safe: true })).value, "A");
});
