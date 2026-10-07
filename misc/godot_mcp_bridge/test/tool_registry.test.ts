import assert from "node:assert/strict";
import test from "node:test";

import { get_scene_patch_schema_document, get_tool_definition, TOOL_DEFINITIONS } from "../src/tool_registry.js";

const NEW_TOOLS: Record<string, string> = {
	"godot.scene.apply_patch": "write_scene",
	"godot.scene.patch_schema": "read_scene",
	"godot.script.validate": "script_validation",
	"godot.filesystem.scan": "filesystem_sync",
	"godot.filesystem.wait": "filesystem_sync",
	"godot.resource.import": "filesystem_sync",
	"godot.resource.status": "resource_versioning",
	"godot.runtime.start": "runtime_control",
	"godot.runtime.stop": "runtime_control",
	"godot.runtime.status": "runtime_control",
	"godot.runtime.pause": "runtime_control",
	"godot.runtime.resume": "runtime_control",
	"godot.runtime.advance_frames": "runtime_control",
	"godot.runtime.get_tree": "runtime_inspection",
	"godot.runtime.get_property": "runtime_inspection",
	"godot.runtime.set_property": "runtime_inspection",
	"godot.runtime.call": "runtime_inspection",
	"godot.runtime.input_action": "runtime_inspection",
	"godot.runtime.input_event": "runtime_inspection",
	"godot.runtime.capture_viewport": "viewport_capture",
	"godot.diagnostics.get": "diagnostics_v2",
};

test("registers the atomic scene patch contract with required concurrency guards", () => {
	const patch = get_tool_definition("godot.scene.apply_patch");
	assert.ok(patch);
	assert.equal(patch.method, "scene.apply_patch");
	assert.equal(patch.retrySafe, undefined);
	assert.deepEqual(patch.inputSchema.required, ["expected_scene_path", "expected_scene_version", "operations"]);
	const properties = patch.inputSchema.properties as Record<string, Record<string, unknown>>;
	assert.deepEqual(properties.mode.enum, ["atomic", "best_effort"]);
	assert.equal(properties.operations.maxItems, 512);
	assert.ok(properties.operations.items);
	const operationSchema = properties.operations.items as Record<string, unknown>;
	const variants = operationSchema.oneOf as Array<Record<string, unknown>>;
	assert.equal(variants.length, 7);
	assert.deepEqual(variants.map((variant) => {
		const operationProperties = variant.properties as Record<string, Record<string, unknown>>;
		return operationProperties.op.enum;
	}), [["create_resource"], ["update_resource"], ["delete_resource"], ["create_node"], ["update_node"], ["move_node"], ["delete_node"]]);
	assert.equal((operationSchema.discriminator as Record<string, unknown>).propertyName, "op");
	const definitions = patch.inputSchema.$defs as Record<string, Record<string, unknown>>;
	assert.ok(definitions.patchValue);
	const typedVariant = definitions.typedVariant;
	assert.equal((typedVariant.discriminator as Record<string, unknown>).propertyName, "__type");
	const typedNames = (typedVariant.oneOf as Array<Record<string, unknown>>).flatMap((variant) => {
		const typedProperties = variant.properties as Record<string, Record<string, unknown>>;
		return typedProperties.__type.enum as string[];
	});
	assert.ok(typedNames.includes("Transform3D"));
	assert.ok(typedNames.includes("PackedStringArray"));
	assert.equal(new Set(typedNames).size, 23);
});

test("publishes the complete scene patch schema as a dedicated offline tool", () => {
	const tool = get_tool_definition("godot.scene.patch_schema");
	assert.ok(tool);
	assert.equal(tool.method, "bridge.scene.patch_schema");
	assert.equal(tool.retrySafe, true);
	const document = get_scene_patch_schema_document();
	assert.equal(document.schema_version, 1);
	assert.equal((document.operations as Record<string, unknown>).discriminator instanceof Object, true);
	assert.ok((document.$defs as Record<string, unknown>).patchValue);
});

test("registers each negotiated automation tool once with the expected capability", () => {
	assert.equal(new Set(TOOL_DEFINITIONS.map((tool) => tool.name)).size, TOOL_DEFINITIONS.length);
	for (const [name, capability] of Object.entries(NEW_TOOLS)) {
		const tool = get_tool_definition(name);
		assert.ok(tool, `${name} must be registered`);
		assert.equal(tool.capability, capability);
		assert.equal(tool.inputSchema.type, "object");
	}
});

test("requires ownership and confirmation before stopping a runtime", () => {
	const stop = get_tool_definition("godot.runtime.stop");
	assert.ok(stop);
	assert.deepEqual(stop.inputSchema.required, ["run_id", "confirm"]);
});

test("explicitly advertises arbitrary serializable runtime arguments", () => {
	const call = get_tool_definition("godot.runtime.call");
	assert.ok(call);
	const properties = call.inputSchema.properties as Record<string, Record<string, unknown>>;
	assert.equal(properties.arguments.type, "array");
	assert.equal(properties.arguments.maxItems, 16);
	assert.deepEqual(properties.arguments.items, {});
});

test("exposes partial project setting reads and non-destructive diagnostics", () => {
	const settings = get_tool_definition("godot.project.get_setting");
	const nodeProperties = get_tool_definition("godot.node.get_properties");
	const nodePropertiesBatch = get_tool_definition("godot.node.get_properties_batch");
	const resourceProperties = get_tool_definition("godot.resource.get");
	const diagnostics = get_tool_definition("godot.diagnostics.get");
	assert.ok(settings);
	assert.ok(nodeProperties);
	assert.ok(nodePropertiesBatch);
	assert.ok(resourceProperties);
	assert.ok(diagnostics);
	for (const definition of [settings, nodeProperties, nodePropertiesBatch, resourceProperties]) {
		assert.ok((definition.inputSchema.properties as Record<string, unknown>).atomic);
	}
	assert.equal((diagnostics.inputSchema.properties as Record<string, unknown>).clear, undefined);
});
