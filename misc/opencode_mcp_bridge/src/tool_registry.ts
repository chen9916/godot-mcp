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
				root_path: COMMON_PATH_SCHEMA,
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
		name: "godot.node.get_properties_batch",
		description: "Get properties for multiple nodes in one call.",
		method: "node.get_properties_batch",
		capability: "read_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
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
		name: "godot.node.create_batch",
		description: "Create multiple nodes with ordered dependencies.",
		method: "node.create_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
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
		name: "godot.node.set_properties_batch",
		description: "Set properties on multiple nodes in one call.",
		method: "node.set_properties_batch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["items"],
			properties: {
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
	{
		name: "godot.resource.reload",
		description: "Reload active scene or resource from disk.",
		method: "resource.reload",
		capability: "save_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				path: { type: "string", description: "res:// resource path" },
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
