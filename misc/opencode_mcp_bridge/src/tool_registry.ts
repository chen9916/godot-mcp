export type GodotCapability = "read_scene" | "write_scene" | "read_script" | "write_script" | "save_resource" | "read_resource" | "write_resource" | "read_project" | "write_project";

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
		name: "godot.node.get_properties",
		description: "Get selected node property values by property_names.",
		method: "node.get_properties",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "property_names"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				property_names: {
					type: "array",
					minItems: 1,
					items: { type: "string" },
				},
			},
		},
	},
	{
		name: "godot.node.get_property",
		description: "Get one node property value by name.",
		method: "node.get_property",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "property_name"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				property_name: { type: "string", description: "Node property name" },
			},
		},
	},
	{
		name: "godot.node.list_properties",
		description: "List node property names and types.",
		method: "node.list_properties",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
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
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				properties: { type: "object", additionalProperties: true },
				property_entries: {
					type: "array",
					description: "Alternative to properties for clients that cannot send free-form maps",
					items: {
						type: "object",
						additionalProperties: false,
						required: ["name", "value"],
						properties: {
							name: { type: "string", description: "Node property name" },
							value: { description: "Node property value" },
						},
					},
				},
			},
		},
	},
	{
		name: "godot.script.get_active",
		description: "Get script source from the active editor buffer or selected scene node workspace.",
		method: "script.get_active",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				workspace: {
					type: "string",
					enum: ["auto", "scene_view", "script_editor"],
					description: "Selection mode: auto uses active main screen, scene_view uses selected node, script_editor uses active script tab",
				},
			},
		},
	},
	{
		name: "godot.script.get",
		description: "Get script source by target, or from workspace with buffer-aware script editor behavior.",
		method: "script.get",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				script_path: { type: "string", description: "res:// script path" },
				node_path: COMMON_PATH_SCHEMA,
				workspace: {
					type: "string",
					enum: ["auto", "scene_view", "script_editor"],
					description: "When script_editor is used, source comes from the active tab buffer and script_path (if provided) must match the active tab",
				},
			},
		},
	},
	{
		name: "godot.script.apply_text_edits",
		description: "Apply LSP-style text edits to a script or active script editor buffer.",
		method: "script.apply_text_edits",
		capability: "write_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["edits"],
			properties: {
				script_path: { type: "string", description: "res:// script path" },
				node_path: COMMON_PATH_SCHEMA,
				workspace: {
					type: "string",
					enum: ["auto", "scene_view", "script_editor"],
					description: "When script_editor is used, edits apply to active tab buffer first and script_path (if provided) must match active tab",
				},
				expected_version: { type: "string" },
				edits: {
					type: "array",
					items: {
						type: "object",
						required: ["start_line", "start_col", "end_line", "end_col", "new_text"],
						properties: {
							start_line: { type: "number", minimum: 0, description: "Zero-based start line" },
							start_col: { type: "number", minimum: 0, description: "Zero-based start column" },
							end_line: { type: "number", minimum: 0, description: "Zero-based end line" },
							end_col: { type: "number", minimum: 0, description: "Zero-based end column" },
							new_text: { type: "string" },
						},
						additionalProperties: false,
					},
				},
			},
		},
	},
	{
		name: "godot.lsp",
		description: "Run GDScript LSP-style queries (definition, references, hover, symbols).",
		method: "lsp.query",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["operation"],
			properties: {
				operation: {
					type: "string",
					enum: ["goToDefinition", "findReferences", "hover", "documentSymbol"],
					description: "LSP operation name",
				},
				filePath: { type: "string", description: "res:// script path (alias of script_path)" },
				script_path: { type: "string", description: "res:// script path" },
				node_path: COMMON_PATH_SCHEMA,
				workspace: {
					type: "string",
					enum: ["auto", "scene_view", "script_editor"],
					description: "Script target resolution workspace when file path is omitted",
				},
				line: { type: "number", minimum: 0, description: "Zero-based line number" },
				character: { type: "number", minimum: 0, description: "Zero-based character offset" },
				include_declaration: { type: "boolean", description: "Include declaration in findReferences (default: true)" },
				workspace_search: { type: "boolean", description: "Search all .gd files in search_root for references (default: false)" },
				search_root: { type: "string", description: "res:// search root for workspace_search (default: res://)" },
				file_limit: { type: "number", minimum: 1, description: "Max .gd files scanned in workspace_search (default: 300)" },
			},
		},
	},
	{
		name: "godot.script.attach",
		description: "Attach an existing script or create and attach a built-in script.",
		method: "script.attach",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				script_path: { type: "string", description: "res:// script path or built-in scene script path (res://scene.tscn::GDScript_*)" },
				built_in: {
					type: "object",
					additionalProperties: false,
					properties: {
						language: { type: "string", description: "Script language name (default: gdscript)" },
						source: { type: "string", description: "Built-in script source text" },
						name: { type: "string", description: "Optional built-in script display name" },
						replace_existing: { type: "boolean", description: "Reserved for future behavior" },
					},
				},
				workspace: {
					type: "string",
					enum: ["auto", "scene_view", "script_editor"],
					description: "If node_path is omitted, use selected scene node. If script_path is omitted in script_editor, use active script tab",
				},
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

	// -----------------------------------------------------------------------
	// Scene management tools
	// -----------------------------------------------------------------------
	{
		name: "godot.scene.list",
		description: "List all .tscn/.scn scene files in the project or a subdirectory.",
		method: "scene.list",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				directory: { type: "string", description: "res:// directory to search (default: res://)" },
				recursive: { type: "boolean", description: "Search subdirectories (default: true)" },
			},
		},
	},
	{
		name: "godot.scene.open",
		description: "Open a scene by res:// path in the editor (makes it the active edited scene).",
		method: "scene.open",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// path to a .tscn or .scn file" },
			},
		},
	},
	{
		name: "godot.scene.create",
		description: "Create a new in-memory scene with a specified root node type. Requires resource.save to persist.",
		method: "scene.create",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["root_type"],
			properties: {
				root_type: { type: "string", description: "Class name for the root node (e.g. Node2D, Node3D, Control)" },
				root_name: { type: "string", description: "Name for the root node (defaults to root_type)" },
			},
		},
	},
	{
		name: "godot.scene.instantiate",
		description: "Instance a packed scene as a child of a node (like adding a prefab).",
		method: "scene.instantiate",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["scene_path", "parent_path"],
			properties: {
				scene_path: { type: "string", description: "res:// path to the .tscn/.scn to instantiate" },
				parent_path: COMMON_PATH_SCHEMA,
				name: { type: "string", description: "Override instance name" },
				position: { type: "number", description: "Child index position (-1 to append)" },
			},
		},
	},

	// -----------------------------------------------------------------------
	// Node tools
	// -----------------------------------------------------------------------
	{
		name: "godot.node.find",
		description: "Search nodes by name pattern, type, group, or metadata across the tree.",
		method: "node.find",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				pattern: { type: "string", description: "Name pattern with * and ? wildcards (default: *)" },
				type: { type: "string", description: "Filter by class name" },
				group: { type: "string", description: "Filter by group membership" },
				limit: { type: "number", minimum: 1, description: "Max results (default: 100)" },
				owned: { type: "boolean", description: "Only include nodes owned by the scene root (default: true)" },
			},
		},
	},
	{
		name: "godot.node.get_groups",
		description: "List groups a node belongs to.",
		method: "node.get_groups",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
			},
		},
	},
	{
		name: "godot.node.set_groups",
		description: "Add/remove a node from groups in one undoable operation.",
		method: "node.set_groups",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				add: { type: "array", items: { type: "string" }, description: "Group names to add" },
				remove: { type: "array", items: { type: "string" }, description: "Group names to remove" },
			},
		},
	},

	// -----------------------------------------------------------------------
	// Resource & asset tools
	// -----------------------------------------------------------------------
	{
		name: "godot.resource.get",
		description: "Inspect any resource's properties (materials, textures, audio, etc.).",
		method: "resource.get",
		capability: "read_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// path to the resource" },
				property_names: { type: "array", items: { type: "string" }, description: "Specific properties to read (omit for all)" },
			},
		},
	},
	{
		name: "godot.resource.list",
		description: "List resources by type or directory.",
		method: "resource.list",
		capability: "read_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				directory: { type: "string", description: "res:// directory to list (default: res://)" },
				extensions: { type: "array", items: { type: "string" }, description: "File extension filters (e.g. [\"tres\", \"res\", \"png\"])" },
				recursive: { type: "boolean", description: "Recurse into subdirectories (default: true)" },
				limit: { type: "number", minimum: 1, description: "Max results (default: 500)" },
			},
		},
	},
	{
		name: "godot.resource.create",
		description: "Create new resources (StyleBox, ShaderMaterial, AudioStream, etc.) in memory.",
		method: "resource.create",
		capability: "write_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["type"],
			properties: {
				type: { type: "string", description: "Resource class name (e.g. StyleBoxFlat, ShaderMaterial)" },
				path: { type: "string", description: "Optional res:// path to assign (does not save to disk)" },
				properties: { type: "object", additionalProperties: true, description: "Initial flat property values" },
			},
		},
	},
	{
		name: "godot.resource.set_properties",
		description: "Modify resource properties (e.g. change a material's albedo color).",
		method: "resource.set_properties",
		capability: "write_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path", "properties"],
			properties: {
				path: { type: "string", description: "res:// path of the resource" },
				properties: { type: "object", additionalProperties: true, description: "Key-value pairs of property names to new values" },
			},
		},
	},

	// -----------------------------------------------------------------------
	// Signal & connection tools
	// -----------------------------------------------------------------------
	{
		name: "godot.signal.list",
		description: "List all signals on a node (built-in and custom from scripts).",
		method: "signal.list",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
			},
		},
	},
	{
		name: "godot.signal.get_connections",
		description: "Inspect existing signal connections on a node.",
		method: "signal.get_connections",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				signal_name: { type: "string", description: "Filter to connections for this signal only" },
			},
		},
	},
	{
		name: "godot.signal.connect",
		description: "Connect a signal to a method on a target node.",
		method: "signal.connect",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "signal_name", "target_path", "method"],
			properties: {
				node_path: { type: "string", description: "Source node (signal emitter)" },
				signal_name: { type: "string", description: "Signal name on the source node" },
				target_path: { type: "string", description: "Target node (receiver)" },
				method: { type: "string", description: "Method name on the target node" },
				flags: { type: "number", description: "Connection flags bitmask (default: 0)" },
			},
		},
	},
	{
		name: "godot.signal.disconnect",
		description: "Remove a signal connection.",
		method: "signal.disconnect",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "signal_name", "target_path", "method"],
			properties: {
				node_path: { type: "string", description: "Source node (signal emitter)" },
				signal_name: { type: "string", description: "Signal name" },
				target_path: { type: "string", description: "Target node (receiver)" },
				method: { type: "string", description: "Method name on the target" },
			},
		},
	},

	// -----------------------------------------------------------------------
	// Project & editor tools
	// -----------------------------------------------------------------------
	{
		name: "godot.project.get_setting",
		description: "Read project.godot settings (e.g. window size, main scene).",
		method: "project.get_setting",
		capability: "read_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["keys"],
			properties: {
				keys: { type: "array", minItems: 1, items: { type: "string" }, description: "Setting keys (e.g. application/config/name)" },
			},
		},
	},
	{
		name: "godot.project.set_setting",
		description: "Modify project settings and save to project.godot.",
		method: "project.set_setting",
		capability: "write_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["settings"],
			properties: {
				settings: { type: "object", additionalProperties: true, description: "Key-value pairs of setting paths to new values" },
			},
		},
	},
	{
		name: "godot.editor.get_errors",
		description: "Fetch current errors/warnings from the editor.",
		method: "editor.get_errors",
		capability: "read_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				types: { type: "array", items: { type: "string" }, description: "Filter by message type: error, warning (default: both)" },
				limit: { type: "number", minimum: 1, description: "Max messages to return (default: 50)" },
				clear: { type: "boolean", description: "Clear the error buffer after reading (default: false)" },
			},
		},
	},

	// -----------------------------------------------------------------------
	// Shader & visual tools
	// -----------------------------------------------------------------------
	{
		name: "godot.shader.get",
		description: "Read shader source code from a .gdshader or shader resource.",
		method: "shader.get",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// path to a .gdshader file or shader resource" },
			},
		},
	},
	{
		name: "godot.shader.edit",
		description: "Replace shader source code.",
		method: "shader.edit",
		capability: "write_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path", "source"],
			properties: {
				path: { type: "string", description: "res:// path to the shader resource" },
				source: { type: "string", description: "New shader source code" },
				expected_version: { type: "string", description: "MD5 of current source for conflict detection" },
			},
		},
	},
	{
		name: "godot.theme.get_overrides",
		description: "Inspect theme overrides on a Control node.",
		method: "theme.get_overrides",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				override_type: { type: "string", description: "Filter by type: color, constant, font, font_size, icon, stylebox (omit for all)" },
			},
		},
	},
	{
		name: "godot.theme.set_overrides",
		description: "Apply theme overrides on a Control node. Set value to null to remove an override.",
		method: "theme.set_overrides",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "overrides"],
			properties: {
				node_path: COMMON_PATH_SCHEMA,
				overrides: {
					type: "object",
					description: "Nested object with categories: colors, constants, font_sizes, fonts, icons, styleboxes",
					additionalProperties: true,
				},
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
