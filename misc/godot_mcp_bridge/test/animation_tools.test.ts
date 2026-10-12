import assert from "node:assert/strict";
import test from "node:test";

import { AjvJsonSchemaValidator } from "@modelcontextprotocol/sdk/validation/ajv";
import type { JsonSchemaType } from "@modelcontextprotocol/sdk/validation";

import { ANIMATION_GRAPH_OPERATION_SCHEMA, ANIMATION_LIBRARY_OPERATION_SCHEMA } from "../src/animation_tools.js";
import { is_async_runtime_method } from "../src/operation_waiter.js";
import { get_animation_schema, get_tool_definition, is_tool_retry_safe, TOOL_DEFINITIONS } from "../src/tool_registry.js";

const capabilities: Record<string, string> = {
	"godot.animation.schema": "read_scene",
	"godot.animation.list": "read_scene",
	"godot.animation.get": "read_scene",
	"godot.animation.apply_patch": "write_scene",
	"godot.animation.graph_get": "read_scene",
	"godot.animation.graph_apply_patch": "write_scene",
	"godot.animation.graph_validate": "read_scene",
	"godot.runtime.animation_get": "runtime_inspection",
	"godot.runtime.animation_control": "runtime_inspection",
};

function operationNames(schema: Record<string, any>): string[] {
	assert.equal(schema.discriminator.propertyName, "op");
	return schema.oneOf.map((entry: Record<string, any>) => {
		assert.equal(entry.additionalProperties, false);
		assert.ok(entry.required.includes("op"));
		assert.deepEqual(entry.properties.op.enum, [entry.properties.op.const]);
		return entry.properties.op.const;
	});
}

test("animation suite is discoverable with explicit capabilities and replay safety", () => {
	assert.equal(TOOL_DEFINITIONS.length, 83);
	for (const [name, capability] of Object.entries(capabilities)) {
		const tool = get_tool_definition(name);
		assert.ok(tool, name);
		assert.equal(tool.capability, capability);
		assert.equal(tool.inputSchema.additionalProperties, false);
		assert.equal(is_tool_retry_safe(tool), !["godot.animation.apply_patch", "godot.animation.graph_apply_patch", "godot.runtime.animation_control"].includes(name));
	}
	assert.equal(new Set(TOOL_DEFINITIONS.map((tool) => tool.name)).size, TOOL_DEFINITIONS.length);
});

test("animation mutations require scene path/version and bound atomic operations", () => {
	for (const name of ["godot.animation.apply_patch", "godot.animation.graph_apply_patch"]) {
		const tool = get_tool_definition(name)!;
		assert.deepEqual(tool.inputSchema.required, ["expected_scene_path", "expected_scene_version", "node_path", "operations"]);
		const fields = tool.inputSchema.properties as Record<string, any>;
		assert.equal(fields.operations.minItems, 1);
		assert.equal(fields.operations.maxItems, 512);
		assert.equal(fields.dry_run.default, false);
		assert.equal(fields.save, undefined, "Scene persistence is a separate guarded resource.save call");
	}
});

test("graph schemas expose structural edits instead of opaque free-form operations", () => {
	assert.deepEqual(operationNames(ANIMATION_GRAPH_OPERATION_SCHEMA), [
		"set_root", "add_node", "remove_node", "rename_node", "move_node", "set_node_properties", "connect", "disconnect",
		"add_transition", "set_transition", "remove_transition", "add_blend_point", "set_blend_point", "remove_blend_point", "set_triangles", "set_filter",
	]);
});

test("clip schemas cover libraries, all track types, keys, and bezier handles", () => {
	const names = operationNames(ANIMATION_LIBRARY_OPERATION_SCHEMA);
	assert.equal(names.length, 17);
	for (const name of ["add_library", "rename_library", "create_animation", "duplicate_animation", "add_track", "move_track", "insert_key", "set_key", "remove_key", "set_bezier_handles"]) {
		assert.ok(names.includes(name));
	}
	const track = (ANIMATION_LIBRARY_OPERATION_SCHEMA.oneOf as any[]).find((operation) => operation.properties.op.const === "add_track");
	assert.equal(track.properties.type.enum.length, 9);
});

test("offline animation schema is complete and every local reference resolves", () => {
	const document = get_animation_schema() as Record<string, any>;
	assert.equal(document.schema_version, 1);
	assert.equal(document.operation_limit, 512);
	assert.equal(document.limits.decoded_keys_per_track, 10_000);
	assert.equal(document.limits.decoded_keys_per_unique_resource_graph, 25_000);
	assert.match(document.notes.join("\n"), /Ordinary non-@tool AnimationTree controller scripts and exported values are preserved/);
	assert.deepEqual(document.graph_operations, ANIMATION_GRAPH_OPERATION_SCHEMA);
	assert.deepEqual(document.library_operations, ANIMATION_LIBRARY_OPERATION_SCHEMA);
	assert.ok(document.runtime_schemas["runtime.animation_control"].allOf);
	assert.deepEqual(document.runtime_schemas["runtime.animation_get"].required, ["run_id", "node_path"]);
	assert.equal(Object.keys(document.key_formats).length, 9);
	assert.deepEqual(document.key_formats.method.required, ["method", "args"]);
	assert.equal(document.key_formats.bezier.oneOf[0].minItems, 5);
	function check(value: any): void {
		if (Array.isArray(value)) { value.forEach(check); return; }
		if (!value || typeof value !== "object") return;
		if (typeof value.$ref === "string") {
			assert.ok(value.$ref.startsWith("#/$defs/"));
			assert.ok(document.$defs[value.$ref.slice("#/$defs/".length)], value.$ref);
		}
		Object.values(value).forEach(check);
	}
	check(document);
});

test("runtime animation actions inherit bounded async deduplication and require owned runs", () => {
	for (const name of ["godot.runtime.animation_get", "godot.runtime.animation_control"]) {
		const tool = get_tool_definition(name)!;
		assert.equal(is_async_runtime_method(tool.method), true);
		assert.ok((tool.inputSchema.required as string[]).includes("run_id"));
		assert.ok((tool.inputSchema.required as string[]).includes("node_path"));
		assert.equal((tool.inputSchema.properties as Record<string, any>).timeout_ms.maximum, 60_000);
	}
	const tool = get_tool_definition("godot.runtime.animation_control")!;
	assert.equal((tool.inputSchema.properties as Record<string, any>).action.enum.length, 16);
});

test("the standard MCP schema validator accepts authoring examples and rejects malformed commands", () => {
	const validator = new AjvJsonSchemaValidator();
	for (const name of Object.keys(capabilities)) {
		assert.doesNotThrow(() => validator.getValidator(get_tool_definition(name)!.inputSchema as JsonSchemaType), name);
	}
	const graph = validator.getValidator(get_tool_definition("godot.animation.graph_apply_patch")!.inputSchema as JsonSchemaType);
	const guards = { expected_scene_path: "res://character.tscn", expected_scene_version: "scene-v2:example", node_path: "AnimationTree" };
	assert.equal(graph({ ...guards, operations: [
		{ op: "set_root", type: "AnimationNodeBlendTree" },
		{ op: "add_node", name: "Walk", type: "AnimationNodeAnimation", position: [0, 0], properties: { animation: "walk" } },
		{ op: "connect", from: "Walk", to: "output", input: 0 },
	] }).valid, true);
	assert.equal(graph({ ...guards, operations: [{ op: "connect", from: "Walk", to: "output", input: -1 }] }).valid, false);
	assert.equal(graph({ ...guards, operations: [{ op: "execute", code: "unsafe" }] }).valid, false);
	assert.equal(graph({ ...guards, operations: [] }).valid, false);
	const clips = validator.getValidator(get_tool_definition("godot.animation.apply_patch")!.inputSchema as JsonSchemaType);
	assert.equal(clips({ ...guards, node_path: "AnimationPlayer", operations: [
		{ op: "create_animation", animation: "idle", properties: { length: 1 } },
		{ op: "add_track", animation: "idle", type: "position_3d", path: "Visual" },
		{ op: "insert_key", animation: "idle", track: 0, time: 0, value: { __type: "Vector3", value: [0, 0, 0] } },
	] }).valid, true);
	assert.equal(clips({ ...guards, node_path: "AnimationPlayer", operations: [{
		op: "insert_key", animation: "data", track: 0, time: 0,
		value: { __type: "Dictionary", value: [
			["__type", "literal"],
			[4, { __type: "PackedVector3Array", value: [{ __type: "Vector3", value: [1, 2, 3] }] }],
			["transform", { __type: "Transform3D", value: [{ __type: "Basis", value: [1, 0, 0, 0, 1, 0, 0, 0, 1] }, { __type: "Vector3", value: [0, 1, 0] }] }],
			["stream", { resource_path: "res://sound.wav", resource_type: "AudioStreamWAV", embedded: false }],
		] },
	}] }).valid, true, "Typed dictionary and resource inspection values must remain reinsertable through the real MCP schema.");
	const integerKey = (input: unknown) => clips({ ...guards, node_path: "AnimationPlayer", operations: [{ op: "insert_key", animation: "data", track: 0, time: 0, value: input }] }).valid;
	assert.equal(integerKey({ __type: "int", value: "9223372036854775807" }), true);
	assert.equal(integerKey({ __type: "int", value: "-9223372036854775808" }), true);
	assert.equal(integerKey({ __type: "int", value: "1.5" }), false);
	assert.equal(integerKey({ __type: "int", value: "1e12" }), false);
	assert.equal(integerKey({ __type: "PackedInt64Array", value: [{ __type: "int", value: "9223372036854775807" }, 4] }), true);
	assert.equal(integerKey({ __type: "PackedInt64Array", value: [Number.MAX_SAFE_INTEGER + 1] }), false);
	assert.equal(clips({ ...guards, operations: [{ op: "insert_key", animation: "idle", track: 0, time: -1, value: 1 }] }).valid, false);
	const runtime = validator.getValidator(get_tool_definition("godot.runtime.animation_control")!.inputSchema as JsonSchemaType);
	const target = { run_id: "owned-run", node_path: "/root/Character/AnimationTree" };
	assert.equal(runtime({ ...target, action: "state_travel", state: "Run" }).valid, true);
	assert.equal(runtime({ ...target, node_path: "/root/Character/AnimationPlayer", action: "player_play" }).valid, true);
	assert.equal(runtime({ ...target, node_path: "/root/Character/AnimationPlayer", action: "player_queue" }).valid, false);
	assert.equal(runtime({ ...target, action: "state_travel" }).valid, false);
	assert.equal(runtime({ ...target, action: "set_parameter", parameter: "parameters/Move/blend_position", value: { __type: "Vector2", value: [1, 0] } }).valid, true);
});
