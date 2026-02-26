export type GodotCapability = "read_scene" | "write_scene" | "read_script" | "write_script" | "save_resource";

export interface ToolDefinition {
	name: string;
	description: string;
	method: string;
	capability: GodotCapability;
	inputSchema: Record<string, unknown>;
}

const COMMON_PATH_SCHEMA = {
	type: "string",
	description: "Godot resource/node path",
};

export const TOOL_DEFINITIONS: ToolDefinition[] = [
	{
		name: "godot.scene.get_active",
		description: "Get the currently edited scene root.",
		method: "scene.get_active",
		capability: "read_scene",
		inputSchema: { type: "object", additionalProperties: false, properties: {} },
	},
	{
		name: "godot.scene.get_tree",
		description: "Get a bounded tree snapshot for the edited scene.",
		method: "scene.get_tree",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				max_depth: { type: "number", minimum: 0 },
				max_nodes: { type: "number", minimum: 1 },
			},
		},
	},
	{
		name: "godot.node.create",
		description: "Create a node under a parent node path.",
		method: "node.create",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["parent_path", "type"],
			properties: {
				parent_path: COMMON_PATH_SCHEMA,
				type: { type: "string", description: "Concrete Godot Node class name" },
				name: { type: "string" },
				position: { type: "number" },
				properties: { type: "object", additionalProperties: true },
			},
		},
	},
	{
		name: "godot.node.delete",
		description: "Delete one node. Requires force=true.",
		method: "node.delete",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "force"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				force: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.node.reparent",
		description: "Move an existing node under a new parent.",
		method: "node.reparent",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "new_parent_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				new_parent_path: COMMON_PATH_SCHEMA,
				position: { type: "number" },
				new_name: { type: "string" },
			},
		},
	},
	{
		name: "godot.node.set_properties",
		description: "Set multiple node properties in one undoable operation.",
		method: "node.set_properties",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "properties"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				properties: { type: "object", additionalProperties: true },
			},
		},
	},
	{
		name: "godot.script.get_active",
		description: "Get source for the currently active script editor tab.",
		method: "script.get_active",
		capability: "read_script",
		inputSchema: { type: "object", additionalProperties: false, properties: {} },
	},
	{
		name: "godot.script.get",
		description: "Get script source by script_path/node_path, or active script if omitted.",
		method: "script.get",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				script_path: { type: "string", description: "res:// script path" },
				node_path: COMMON_PATH_SCHEMA,
			},
		},
	},
	{
		name: "godot.script.apply_text_edits",
		description: "Apply LSP-style text edits to a script.",
		method: "script.apply_text_edits",
		capability: "write_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["edits"],
			properties: {
				script_path: { type: "string", description: "res:// script path" },
				node_path: COMMON_PATH_SCHEMA,
				expected_version: { type: "string" },
				edits: {
					type: "array",
					items: {
						type: "object",
						required: ["start_line", "start_col", "end_line", "end_col", "new_text"],
						properties: {
							start_line: { type: "number" },
							start_col: { type: "number" },
							end_line: { type: "number" },
							end_col: { type: "number" },
							new_text: { type: "string" },
						},
						additionalProperties: false,
					},
				},
			},
		},
	},
	{
		name: "godot.script.attach",
		description: "Attach a script resource to a node.",
		method: "script.attach",
		capability: "write_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "script_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				script_path: { type: "string", description: "res:// script path" },
			},
		},
	},
	{
		name: "godot.resource.save",
		description: "Save active scene or any loaded resource path.",
		method: "resource.save",
		capability: "save_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				path: { type: "string", description: "res:// resource path" },
			},
		},
	},
];

const TOOL_BY_NAME = new Map(TOOL_DEFINITIONS.map((tool) => [tool.name, tool]));

export function get_tool_definition(p_name: string): ToolDefinition | undefined {
	return TOOL_BY_NAME.get(p_name);
}

export function get_tool_list_for_mcp(): Array<Record<string, unknown>> {
	return TOOL_DEFINITIONS.map((tool) => ({
		name: tool.name,
		description: tool.description,
		inputSchema: tool.inputSchema,
	}));
}
