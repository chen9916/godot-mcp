import assert from "node:assert/strict";
import test from "node:test";

import { get_tool_definition, TOOL_DEFINITIONS } from "../src/tool_registry.js";

const NEW_TOOLS: Record<string, string> = {
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
