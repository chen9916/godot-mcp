export type GodotCapability = "read_scene" | "write_scene" | "read_script" | "write_script" | "save_resource" | "read_resource" | "write_resource" | "read_project" | "write_project" | "read_docs";

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

const EXPECTED_SCENE_PATH_SCHEMA = {
	type: "string",
	description: "Optional expected active scene path; returns conflict if the editor switched to another scene",
};

export const TOOL_DEFINITIONS: ToolDefinition[] = [
	{
		name: "godot.session.info",
		description: "Get active Godot MCP session metadata and capabilities.",
		method: "opencode.session.info",
		capability: "read_project",
		inputSchema: { type: "object", additionalProperties: false, properties: {} },
	},
	{
		name: "godot.scene.get_active",
		description: "Get the currently edited scene root.",
		method: "scene.get_active",
		capability: "read_scene",
		inputSchema: { type: "object", additionalProperties: false, properties: {} },
	},
	{
		name: "godot.scene.list_open",
		description: "List currently open scene tabs and the active scene.",
		method: "scene.list_open",
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				root_path: COMMON_PATH_SCHEMA,
				max_depth: { type: "number", minimum: 0 },
				max_nodes: { type: "number", minimum: 1 },
			},
		},
	},
	{
		name: "godot.scene.list",
		description: "List scene files under a project directory.",
		method: "scene.list",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				directory: { type: "string", description: "res:// directory to scan" },
				recursive: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.scene.open",
		description: "Open a scene and make it the active edited scene.",
		method: "scene.open",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// scene path" },
			},
		},
	},
	{
		name: "godot.scene.create",
		description: "Create a new in-memory scene with a root node type.",
		method: "scene.create",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["root_type"],
			properties: {
				root_type: { type: "string", description: "Root node class name" },
				root_name: { type: "string" },
			},
		},
	},
	{
		name: "godot.scene.instantiate",
		description: "Instantiate a PackedScene under an existing node.",
		method: "scene.instantiate",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["scene_path", "parent_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				scene_path: { type: "string", description: "res:// packed scene path" },
				parent_path: COMMON_PATH_SCHEMA,
				name: { type: "string" },
				position: { type: "number" },
			},
		},
	},
	{
		name: "godot.node.get_property",
		description: "Get one node property value with type information.",
		method: "node.get_property",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "property_name"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				property_name: { type: "string" },
			},
		},
	},
	{
		name: "godot.node.list_properties",
		description: "List discoverable properties on a node.",
		method: "node.list_properties",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
		name: "godot.node.get_properties_batch",
		description: "Get properties for multiple nodes in one call.",
		method: "node.get_properties_batch",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				items: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["node_path", "property_names"],
						properties: {
							item_id: { type: "string" },
							node_path: COMMON_PATH_SCHEMA,
							property_names: {
								type: "array",
								minItems: 1,
								items: { type: "string" },
							},
						},
					},
				},
			},
		},
	},
	{
		name: "godot.node.find",
		description: "Search scene nodes by pattern, type, or group.",
		method: "node.find",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				pattern: { type: "string" },
				type: { type: "string", description: "Node class filter" },
				group: { type: "string" },
				limit: { type: "number", minimum: 1 },
				owned: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.node.get_groups",
		description: "List non-internal groups for a node.",
		method: "node.get_groups",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
			},
		},
	},
	{
		name: "godot.node.set_groups",
		description: "Add or remove groups on a node.",
		method: "node.set_groups",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				add: { type: "array", items: { type: "string" } },
				remove: { type: "array", items: { type: "string" } },
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				parent_path: COMMON_PATH_SCHEMA,
				type: { type: "string", description: "Concrete Godot Node class name" },
				name: { type: "string" },
				position: { type: "number" },
				properties: { type: "object", additionalProperties: true },
			},
		},
	},
	{
		name: "godot.node.create_batch",
		description: "Create multiple nodes with ordered dependencies.",
		method: "node.create_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				mode: { type: "string", enum: ["atomic", "best_effort"] },
				preview_only: { type: "boolean" },
				items: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["item_id", "type"],
						properties: {
							item_id: { type: "string" },
							parent_path: COMMON_PATH_SCHEMA,
							parent_item_id: { type: "string" },
							type: { type: "string", description: "Concrete Godot Node class name" },
							name: { type: "string" },
							position: { type: "number" },
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
						anyOf: [{ required: ["parent_path"] }, { required: ["parent_item_id"] }],
					},
				},
			},
		},
	},
	{
		name: "godot.node.create_from_template",
		description: "Create a composite subtree from a template payload.",
		method: "node.create_from_template",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["parent_path", "root_name", "nodes"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				parent_path: COMMON_PATH_SCHEMA,
				root_name: { type: "string" },
				root_type: { type: "string" },
				mode: { type: "string", enum: ["atomic", "best_effort"] },
				preview_only: { type: "boolean" },
				root_properties: { type: "object", additionalProperties: true },
				root_property_entries: {
					type: "array",
					items: {
						type: "object",
						additionalProperties: false,
						required: ["name", "value"],
						properties: {
							name: { type: "string" },
							value: {},
						},
					},
				},
				nodes: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["type"],
						properties: {
							item_id: { type: "string" },
							parent_item_id: { type: "string" },
							type: { type: "string" },
							name: { type: "string" },
							position: { type: "number" },
							properties: { type: "object", additionalProperties: true },
							property_entries: {
								type: "array",
								items: {
									type: "object",
									additionalProperties: false,
									required: ["name", "value"],
									properties: {
										name: { type: "string" },
										value: {},
									},
								},
							},
						},
					},
				},
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				force: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.node.delete_batch",
		description: "Delete multiple nodes with deepest-first ordering.",
		method: "node.delete_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items", "force"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				force: { type: "boolean" },
				mode: { type: "string", enum: ["atomic", "best_effort"] },
				items: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["node_path"],
						properties: {
							item_id: { type: "string" },
							node_path: COMMON_PATH_SCHEMA,
						},
					},
				},
			},
		},
	},
	{
		name: "godot.node.duplicate",
		description: "Duplicate a node and apply optional overrides.",
		method: "node.duplicate",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["source_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				source_path: COMMON_PATH_SCHEMA,
				parent_path: COMMON_PATH_SCHEMA,
				new_name: { type: "string" },
				position: { type: "number" },
				property_overrides: { type: "object", additionalProperties: true },
				property_entries: {
					type: "array",
					description: "Alternative to property_overrides for deterministic payloads",
					items: {
						type: "object",
						additionalProperties: false,
						required: ["name", "value"],
						properties: {
							name: { type: "string" },
							value: {},
						},
					},
				},
			},
		},
	},
	{
		name: "godot.node.duplicate_batch",
		description: "Duplicate multiple nodes with ordered dependencies.",
		method: "node.duplicate_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				mode: { type: "string", enum: ["atomic", "best_effort"] },
				items: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["item_id", "source_path"],
						properties: {
							item_id: { type: "string" },
							source_path: COMMON_PATH_SCHEMA,
							parent_path: COMMON_PATH_SCHEMA,
							parent_item_id: { type: "string" },
							new_name: { type: "string" },
							position: { type: "number" },
							property_overrides: { type: "object", additionalProperties: true },
							property_entries: {
								type: "array",
								items: {
									type: "object",
									additionalProperties: false,
									required: ["name", "value"],
									properties: {
										name: { type: "string" },
										value: {},
									},
								},
							},
						},
					},
				},
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
		name: "godot.node.set_properties_batch",
		description: "Set properties on multiple nodes in one call.",
		method: "node.set_properties_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				mode: { type: "string", enum: ["atomic", "best_effort"] },
				items: {
					type: "array",
					minItems: 1,
					items: {
						type: "object",
						additionalProperties: false,
						required: ["node_path"],
						properties: {
							item_id: { type: "string" },
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
						anyOf: [{ required: ["properties"] }, { required: ["property_entries"] }],
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
		name: "godot.shader.get",
		description: "Read shader source code and metadata.",
		method: "shader.get",
		capability: "read_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// shader path or resource path" },
			},
		},
	},
	{
		name: "godot.shader.edit",
		description: "Replace shader source code with optimistic version checks.",
		method: "shader.edit",
		capability: "write_script",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path", "source"],
			properties: {
				path: { type: "string", description: "res:// shader path or resource path" },
				source: { type: "string" },
				expected_version: { type: "string" },
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				path: { type: "string", description: "res:// resource path" },
			},
		},
	},
	{
		name: "godot.resource.reload",
		description: "Reload active scene or resource from disk.",
		method: "resource.reload",
		capability: "save_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				path: { type: "string", description: "res:// resource path" },
			},
		},
	},
	{
		name: "godot.resource.get",
		description: "Inspect serializable properties on a loaded resource.",
		method: "resource.get",
		capability: "read_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// resource path" },
				property_names: {
					type: "array",
					items: { type: "string" },
					description: "Optional list of resource properties to fetch",
				},
			},
		},
	},
	{
		name: "godot.resource.list",
		description: "List resource files in a directory with filters.",
		method: "resource.list",
		capability: "read_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				directory: { type: "string", description: "res:// directory to scan" },
				extensions: { type: "array", items: { type: "string" } },
				recursive: { type: "boolean" },
				limit: { type: "number", minimum: 1 },
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
				properties: { type: "object", additionalProperties: true },
				property_entries: {
					type: "array",
					description: "Alternative to properties for clients that cannot send free-form maps",
					items: {
						type: "object",
						additionalProperties: false,
						required: ["name", "value"],
						properties: {
							name: { type: "string", description: "Resource property name" },
							value: { description: "Resource property value" },
						},
					},
				},
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
			required: ["path"],
			properties: {
				path: { type: "string", description: "res:// path of the resource" },
				properties: { type: "object", additionalProperties: true },
				property_entries: {
					type: "array",
					description: "Alternative to properties for clients that cannot send free-form maps",
					items: {
						type: "object",
						additionalProperties: false,
						required: ["name", "value"],
						properties: {
							name: { type: "string", description: "Resource property name" },
							value: { description: "Resource property value" },
						},
					},
				},
			},
		},
	},
	{
		name: "godot.signal.list",
		description: "List built-in and script-defined signals on a node.",
		method: "signal.list",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				signal_name: { type: "string" },
			},
		},
	},
	{
		name: "godot.signal.connect",
		description: "Connect a node signal to a target method.",
		method: "signal.connect",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "signal_name", "target_path", "method"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				signal_name: { type: "string" },
				target_path: COMMON_PATH_SCHEMA,
				method: { type: "string" },
				flags: { type: "number", minimum: 0 },
			},
		},
	},
	{
		name: "godot.signal.disconnect",
		description: "Disconnect a node signal from a target method.",
		method: "signal.disconnect",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "signal_name", "target_path", "method"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				signal_name: { type: "string" },
				target_path: COMMON_PATH_SCHEMA,
				method: { type: "string" },
			},
		},
	},
	{
		name: "godot.project.get_setting",
		description: "Read one or more project settings keys.",
		method: "project.get_setting",
		capability: "read_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["keys"],
			properties: {
				keys: { type: "array", minItems: 1, items: { type: "string" } },
			},
		},
	},
	{
		name: "godot.project.set_setting",
		description: "Update project settings and save project.godot.",
		method: "project.set_setting",
		capability: "write_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["settings"],
			properties: {
				settings: { type: "object", additionalProperties: true },
			},
		},
	},
	{
		name: "godot.editor.get_errors",
		description: "Read recent Godot editor errors and warnings.",
		method: "editor.get_errors",
		capability: "read_project",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				types: { type: "array", items: { type: "string", enum: ["error", "warning"] } },
				limit: { type: "number", minimum: 1 },
				clear: { type: "boolean" },
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
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				override_type: {
					type: "string",
					enum: ["colors", "constants", "font_sizes", "fonts", "icons", "styleboxes"],
				},
			},
		},
	},
	{
		name: "godot.theme.set_overrides",
		description: "Set or remove theme overrides on a Control node.",
		method: "theme.set_overrides",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["node_path", "overrides"],
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				node_path: COMMON_PATH_SCHEMA,
				overrides: { type: "object", additionalProperties: true },
			},
		},
	},
	{
		name: "godot.docs.class_lookup",
		description: "Look up class documentation from Godot's runtime docs database.",
		method: "docs.class_lookup",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["class_name"],
			properties: {
				class_name: { type: "string" },
				version: { type: "string" },
			},
		},
	},
	{
		name: "godot.docs.member_lookup",
		description: "Look up method/property/signal/constant docs for a class member.",
		method: "docs.member_lookup",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["class_name", "member_name"],
			properties: {
				class_name: { type: "string" },
				member_name: { type: "string" },
				kind: {
					type: "string",
					enum: ["method", "constructor", "operator", "signal", "property", "constant", "annotation", "theme_item", "enum"],
				},
				include_inherited: { type: "boolean" },
				version: { type: "string" },
			},
		},
	},
	{
		name: "godot.docs.search",
		description: "Search classes and members using Godot's help-style matching.",
		method: "docs.search",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["query"],
			properties: {
				query: { type: "string" },
				version: { type: "string" },
				limit: { type: "number", minimum: 1 },
				include_members: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.docs.inheritance",
		description: "Get inheritance chain, descendants, and inherited member summaries.",
		method: "docs.inheritance",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["class_name"],
			properties: {
				class_name: { type: "string" },
				include_descendants: { type: "boolean" },
				descendants_limit: { type: "number", minimum: 1 },
				include_inherited_members: { type: "boolean" },
				version: { type: "string" },
			},
		},
	},
	{
		name: "godot.docs.examples",
		description: "Get short usage snippets extracted from class/member docs.",
		method: "docs.examples",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				class_name: { type: "string", description: "Class name. Provide either class_name or topic." },
				topic: { type: "string", description: "Topic text. Provide either topic or class_name." },
				language: { type: "string", enum: ["any", "gdscript", "csharp", "text"] },
				limit: { type: "number", minimum: 1 },
				version: { type: "string" },
			},
		},
	},
	{
		name: "godot.docs.list_versions",
		description: "List docs versions supported by the running Godot editor.",
		method: "docs.list_versions",
		capability: "read_docs",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {},
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
