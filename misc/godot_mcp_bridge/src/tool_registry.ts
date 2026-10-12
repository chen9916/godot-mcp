import { create_animation_tool_definitions, get_animation_schema_document } from "./animation_tools.js";

export type GodotCapability =
	| "read_scene"
	| "write_scene"
	| "read_script"
	| "write_script"
	| "save_resource"
	| "read_resource"
	| "write_resource"
	| "read_project"
	| "write_project"
	| "read_docs"
	| "script_validation"
	| "filesystem_sync"
	| "resource_versioning"
	| "runtime_control"
	| "runtime_inspection"
	| "viewport_capture"
	| "diagnostics_v2"
	| "partial_batch_results";

export interface ToolDefinition {
	name: string;
	description: string;
	method: string;
	capability: GodotCapability;
	retrySafe?: boolean;
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

const PATCH_NODE_SELECTOR_SCHEMA = {
	oneOf: [
		{ type: "string", description: "Scene-relative node path (use . for the root)" },
		{
			type: "object",
			additionalProperties: false,
			required: ["node_ref"],
			properties: { node_ref: { type: "string", minLength: 1, description: "ID of a node created earlier in this request" } },
		},
	],
};

const PATCH_RESOURCE_SELECTOR_SCHEMA = {
	oneOf: [
		{ type: "string", description: "Embedded resource path in expected_scene_path" },
		{
			type: "object",
			additionalProperties: false,
			required: ["resource_ref"],
			properties: { resource_ref: { type: "string", minLength: 1, description: "ID of a resource created earlier in this request" } },
		},
		{
			type: "object",
			additionalProperties: false,
			required: ["resource_path"],
			properties: { resource_path: { type: "string", minLength: 1, description: "Embedded resource path in expected_scene_path" } },
		},
	],
};

const PATCH_VALUE_SCHEMA = { $ref: "#/$defs/patchValue" };

const PATCH_SCHEMA_DEFINITIONS = {
	patchValue: {
		description: "JSON scalar, recursively encoded Array/Dictionary, reference wrapper, or typed Godot Variant wrapper. Containers are limited to 16 levels and 10,000 total items per property value.",
		anyOf: [
			{ type: "null" },
			{ type: "boolean" },
			{ type: "integer" },
			{ type: "number" },
			{ type: "string" },
			{ type: "array", items: { $ref: "#/$defs/patchValue" } },
			{ $ref: "#/$defs/resourceReference" },
			{ $ref: "#/$defs/nodePathReference" },
			{ $ref: "#/$defs/typedVariant" },
			{
				type: "object",
				description: "Godot Dictionary; keys from JSON are strings and values are decoded recursively.",
				additionalProperties: { $ref: "#/$defs/patchValue" },
				not: { anyOf: [{ required: ["resource_ref"] }, { required: ["external_resource"] }, { required: ["resource_path"] }, { required: ["node_ref"] }, { required: ["__type"] }] },
			},
		],
	},
	resourceReference: {
		type: "object",
		additionalProperties: false,
		description: "A request-local embedded resource created by an earlier create_resource, or an existing external resource. resource_path is accepted as a backward-compatible alias of external_resource in property values.",
		properties: {
			resource_ref: { type: "string", minLength: 1, description: "Earlier create_resource id in this same patch request." },
			external_resource: { type: "string", pattern: "^(res|uid)://", description: "Existing external Godot resource path or UID." },
			resource_path: { type: "string", pattern: "^(res|uid)://", description: "Backward-compatible alias of external_resource in property values." },
		},
		oneOf: [
			{ required: ["resource_ref"] },
			{ required: ["external_resource"] },
			{ required: ["resource_path"] },
		],
	},
	nodePathReference: {
		type: "object",
		additionalProperties: false,
		required: ["node_ref"],
		description: "Encodes a NodePath to a node created earlier in this request.",
		properties: {
			node_ref: { type: "string", minLength: 1 },
			relative_to: PATCH_NODE_SELECTOR_SCHEMA,
			subnames: { oneOf: [{ type: "string" }, { type: "array", items: { type: "string" } }] },
		},
	},
	typedVariant: {
		description: "Complete explicit Godot Variant encodings. Numeric vectors and matrices use flat component arrays in constructor order.",
		discriminator: { propertyName: "__type" },
		oneOf: [
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: {
					__type: { type: "string", const: "Color", enum: ["Color"] },
					value: { oneOf: [{ type: "string", description: "Godot HTML color such as #RRGGBB or #RRGGBBAA." }, { type: "array", minItems: 3, maxItems: 4, items: { type: "number" }, description: "RGB or RGBA." }] },
				},
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["Vector2", "Vector2i"] }, value: { type: "array", minItems: 2, maxItems: 2, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["Vector3", "Vector3i"] }, value: { type: "array", minItems: 3, maxItems: 3, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["Vector4", "Vector4i", "Quaternion", "Rect2", "Rect2i", "Plane"] }, value: { type: "array", minItems: 4, maxItems: 4, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["AABB", "Transform2D"] }, value: { type: "array", minItems: 6, maxItems: 6, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", const: "Basis", enum: ["Basis"] }, value: { type: "array", minItems: 9, maxItems: 9, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", const: "Transform3D", enum: ["Transform3D"] }, value: { type: "array", minItems: 12, maxItems: 12, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["NodePath", "StringName"] }, value: { type: "string" } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", const: "PackedByteArray", enum: ["PackedByteArray"] }, value: { type: "array", maxItems: 10_000, items: { type: "integer", minimum: 0, maximum: 255 } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["PackedInt32Array", "PackedInt64Array"] }, value: { type: "array", maxItems: 10_000, items: { type: "integer" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", enum: ["PackedFloat32Array", "PackedFloat64Array"] }, value: { type: "array", maxItems: 10_000, items: { type: "number" } } },
			},
			{
				type: "object", additionalProperties: false, required: ["__type", "value"],
				properties: { __type: { type: "string", const: "PackedStringArray", enum: ["PackedStringArray"] }, value: { type: "array", maxItems: 10_000, items: { type: "string" } } },
			},
		],
	},
};

const PATCH_PROPERTY_ENTRIES_SCHEMA = {
	type: "array",
	maxItems: 10_000,
	items: {
		type: "object",
		additionalProperties: false,
		required: ["name", "value"],
		properties: {
			name: { type: "string", minLength: 1 },
			value: PATCH_VALUE_SCHEMA,
		},
	},
};

const PATCH_GROUPS_SCHEMA = {
	type: "array",
	maxItems: 10_000,
	items: { type: "string", minLength: 1 },
};

const PATCH_OPERATION_SCHEMA = {
	title: "GodotScenePatchOperation",
	description: "Discriminated union keyed by op. Request-local ids are visible only to later operations in the same ordered request.",
	discriminator: { propertyName: "op" },
	oneOf: [
		{
			title: "Create embedded resource",
			type: "object",
			additionalProperties: false,
			required: ["op", "id", "type"],
			properties: {
				op: { type: "string", const: "create_resource", enum: ["create_resource"], description: "Create an embedded Resource." },
				id: { type: "string", minLength: 1, maxLength: 128, description: "Request-local ID usable by later resource_ref values." },
				type: { type: "string", minLength: 1, description: "Concrete Godot Resource class" },
				properties: { type: "object", description: "Initial properties; application order is deterministic by property name.", additionalProperties: PATCH_VALUE_SCHEMA },
				property_entries: PATCH_PROPERTY_ENTRIES_SCHEMA,
			},
			not: { required: ["properties", "property_entries"] },
		},
		{
			title: "Update embedded resource",
			type: "object",
			additionalProperties: false,
			required: ["op", "resource"],
			properties: {
				op: { type: "string", const: "update_resource", enum: ["update_resource"] },
				resource: PATCH_RESOURCE_SELECTOR_SCHEMA,
				properties: { type: "object", minProperties: 1, additionalProperties: PATCH_VALUE_SCHEMA },
				property_entries: PATCH_PROPERTY_ENTRIES_SCHEMA,
			},
			oneOf: [{ required: ["properties"] }, { required: ["property_entries"] }],
		},
		{
			title: "Delete embedded resource",
			type: "object",
			additionalProperties: false,
			required: ["op", "resource"],
			properties: {
				op: { type: "string", const: "delete_resource", enum: ["delete_resource"] },
				resource: PATCH_RESOURCE_SELECTOR_SCHEMA,
			},
		},
		{
			title: "Create node",
			type: "object",
			additionalProperties: false,
			required: ["op", "id", "parent", "type"],
			properties: {
				op: { type: "string", const: "create_node", enum: ["create_node"] },
				id: { type: "string", minLength: 1, maxLength: 128, description: "Request-local ID usable by later node_ref selectors and NodePath property wrappers." },
				parent: PATCH_NODE_SELECTOR_SCHEMA,
				name: { type: "string", minLength: 1 },
				type: { type: "string", minLength: 1, description: "Concrete Godot Node class" },
				position: { type: "integer", minimum: -1 },
				properties: { type: "object", description: "Initial properties, including script via {external_resource:'res://...gd'}.", additionalProperties: PATCH_VALUE_SCHEMA },
				property_entries: PATCH_PROPERTY_ENTRIES_SCHEMA,
				groups: PATCH_GROUPS_SCHEMA,
				add_groups: PATCH_GROUPS_SCHEMA,
				remove_groups: PATCH_GROUPS_SCHEMA,
			},
			not: { required: ["properties", "property_entries"] },
		},
		{
			title: "Update or rename node",
			type: "object",
			additionalProperties: false,
			required: ["op", "node"],
			properties: {
				op: { type: "string", const: "update_node", enum: ["update_node"] },
				node: PATCH_NODE_SELECTOR_SCHEMA,
				name: { type: "string", minLength: 1, description: "Rename the node." },
				properties: { type: "object", description: "Property assignments; use script:null to clear or an external resource wrapper to assign a script.", additionalProperties: PATCH_VALUE_SCHEMA },
				property_entries: PATCH_PROPERTY_ENTRIES_SCHEMA,
				groups: PATCH_GROUPS_SCHEMA,
				add_groups: PATCH_GROUPS_SCHEMA,
				remove_groups: PATCH_GROUPS_SCHEMA,
			},
			not: { required: ["properties", "property_entries"] },
			anyOf: [
				{ required: ["name"] },
				{ required: ["properties"] },
				{ required: ["property_entries"] },
				{ required: ["groups"] },
				{ required: ["add_groups"] },
				{ required: ["remove_groups"] },
			],
		},
		{
			title: "Move or reparent node",
			type: "object",
			additionalProperties: false,
			required: ["op", "node", "parent"],
			properties: {
				op: { type: "string", const: "move_node", enum: ["move_node"] },
				node: PATCH_NODE_SELECTOR_SCHEMA,
				parent: PATCH_NODE_SELECTOR_SCHEMA,
				name: { type: "string", minLength: 1 },
				position: { type: "integer", minimum: -1 },
			},
		},
		{
			title: "Delete node subtree",
			type: "object",
			additionalProperties: false,
			required: ["op", "node"],
			properties: {
				op: { type: "string", const: "delete_node", enum: ["delete_node"] },
				node: PATCH_NODE_SELECTOR_SCHEMA,
			},
		},
	],
};

export function get_scene_patch_schema_document(): Record<string, unknown> {
	return {
		schema_version: 1,
		operation_limit: 512,
		value_depth_limit: 16,
		value_item_limit: 10_000,
		operations: PATCH_OPERATION_SCHEMA,
		$defs: PATCH_SCHEMA_DEFINITIONS,
		notes: [
			"Operations execute in array order; node_ref and resource_ref can only target earlier successful create operations in the same request.",
			"Node selectors are scene-relative paths or {node_ref}; resource selectors identify embedded resources in expected_scene_path or use {resource_ref}.",
			"Use {external_resource:'res://...'} or {external_resource:'uid://...'} for existing external resources in property values.",
			"properties and property_entries are backward-compatible alternatives and cannot both be supplied.",
		],
	};
}

export function get_animation_schema(): Record<string, unknown> {
	return get_animation_schema_document(PATCH_SCHEMA_DEFINITIONS);
}

export const TOOL_DEFINITIONS: ToolDefinition[] = [
	...create_animation_tool_definitions(PATCH_SCHEMA_DEFINITIONS),
	{
		name: "godot.session.info",
		description: "Get active Godot MCP session metadata and capabilities.",
		method: "godot.session.info",
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
				max_depth: { type: "integer", minimum: 0 },
				max_nodes: { type: "integer", minimum: 1 },
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
		name: "godot.scene.patch_schema",
		description: "Return the complete declarative scene patch operation, selector, reference, and Godot Variant encoding schema. Available without inspecting server source.",
		method: "bridge.scene.patch_schema",
		capability: "read_scene",
		retrySafe: true,
		inputSchema: { type: "object", additionalProperties: false, properties: {} },
	},
	{
		name: "godot.scene.apply_patch",
		description: "Atomically validate and apply a declarative multi-node and embedded-resource patch to the active scene in one undo action.",
		method: "scene.apply_patch",
		capability: "write_scene",
		inputSchema: {
			type: "object",
			$defs: PATCH_SCHEMA_DEFINITIONS,
			additionalProperties: false,
			required: ["expected_scene_path", "expected_scene_version", "operations"],
			properties: {
				expected_scene_path: {
					type: "string",
					minLength: 1,
					description: "Exact res:// path returned by godot.scene.get_active",
				},
				expected_scene_version: {
					type: "string",
					minLength: 1,
					description: "Opaque scene_version returned by godot.scene.get_active or godot.scene.get_tree",
				},
				mode: { type: "string", enum: ["atomic", "best_effort"], default: "atomic" },
				dry_run: { type: "boolean", default: false },
				save: {
					type: "boolean",
					default: false,
					description: "Persist after applying; also requires the negotiated save_resource capability",
				},
				operations: {
					type: "array",
					minItems: 1,
					maxItems: 512,
					items: PATCH_OPERATION_SCHEMA,
				},
			},
			allOf: [{ not: { required: ["dry_run", "save"], properties: { dry_run: { const: true }, save: { const: true } } } }],
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
				atomic: { type: "boolean", description: "Fail the whole read when any property cannot be returned" },
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
				atomic: { type: "boolean", description: "Fail the whole read when any requested property cannot be returned" },
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
		description: "Reload a scene, script, or resource from disk without overwriting an unsaved editor buffer.",
		method: "resource.reload",
		capability: "save_resource",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				expected_scene_path: EXPECTED_SCENE_PATH_SCHEMA,
				path: { type: "string", description: "res:// resource path" },
				expected_version: { type: "string", description: "Expected MD5 of the file on disk" },
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
				atomic: { type: "boolean", description: "Fail the whole read when any property cannot be returned" },
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
				atomic: { type: "boolean", description: "Fail the whole read if any key is missing (default: false)" },
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
				after_sequence: { type: "number", minimum: 0 },
				clear: { type: "boolean" },
				confirm_clear: { type: "boolean", description: "Required with clear=true" },
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
	{
		name: "godot.script.validate",
		description: "Validate disk or unsaved script source without opening a scene, changing tabs, or running the project.",
		method: "script.validate",
		capability: "script_validation",
		retrySafe: true,
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["script_path"],
			properties: {
				script_path: { type: "string", description: "External res:// script path" },
				source: { type: "string", description: "Optional unsaved editor-buffer source" },
				expected_version: { type: "string", description: "Expected MD5 of the selected source" },
				validate_dependencies: { type: "boolean" },
			},
		},
	},
	{
		name: "godot.filesystem.scan",
		description: "Start an asynchronous Godot editor filesystem scan and return its scan ID.",
		method: "filesystem.scan",
		capability: "filesystem_sync",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: { full: { type: "boolean", description: "Run a full scan instead of scanning changes" } },
		},
	},
	{
		name: "godot.filesystem.wait",
		description: "Wait for the editor filesystem scanner and importer to become idle, with a bounded timeout.",
		method: "filesystem.wait",
		capability: "filesystem_sync",
		retrySafe: true,
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				scan_id: { type: "string" },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.resource.import",
		description: "Synchronize and import one or up to 64 resources, returning a result for every path and loadability diagnostics.",
		method: "resource.import",
		capability: "filesystem_sync",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				path: { type: "string" },
				paths: { type: "array", minItems: 1, maxItems: 64, items: { type: "string" } },
				atomic: { type: "boolean" },
			},
			oneOf: [{ required: ["path"] }, { required: ["paths"] }],
		},
	},
	{
		name: "godot.resource.status",
		description: "Compare resource UID, disk, editor-buffer, loaded-cache, importer, dependency, and active-scene state.",
		method: "resource.status",
		capability: "resource_versioning",
		retrySafe: true,
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["path"],
			properties: {
				path: { type: "string", description: "External res:// resource path" },
				load: { type: "boolean", description: "Load the resource when it is not already cached" },
			},
		},
	},
	{
		name: "godot.runtime.start",
		description: "Start one MCP-owned project or scene runtime and return a unique run ID.",
		method: "runtime.start",
		capability: "runtime_control",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: {
				mode: { type: "string", enum: ["main", "current", "scene"] },
				scene_path: { type: "string", description: "Required for mode=scene" },
				headless: { type: "boolean" },
				allow_unsaved: { type: "boolean", description: "Permit Godot's normal autosave-before-run behavior" },
			},
		},
	},
	{
		name: "godot.runtime.stop",
		description: "Stop only the MCP-owned runtime identified by run_id.",
		method: "runtime.stop",
		capability: "runtime_control",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["run_id", "confirm"],
			properties: {
				run_id: { type: "string" },
				confirm: { type: "boolean", description: "Must be true" },
			},
		},
	},
	{
		name: "godot.runtime.status",
		description: "Get the state, debugger connection, process ID, frame counters, and exit code for an MCP-owned runtime.",
		method: "runtime.status",
		capability: "runtime_control",
		retrySafe: true,
		inputSchema: {
			type: "object",
			additionalProperties: false,
			properties: { run_id: { type: "string" } },
		},
	},
	{
		name: "godot.runtime.pause",
		description: "Pause an MCP-owned connected runtime.",
		method: "runtime.pause",
		capability: "runtime_control",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id"], properties: { run_id: { type: "string" } },
		},
	},
	{
		name: "godot.runtime.resume",
		description: "Resume an MCP-owned paused runtime.",
		method: "runtime.resume",
		capability: "runtime_control",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id"], properties: { run_id: { type: "string" } },
		},
	},
	{
		name: "godot.runtime.advance_frames",
		description: "Wait for a bounded number of process or physics frames; paused runtimes are stepped explicitly.",
		method: "runtime.advance_frames",
		capability: "runtime_control",
		inputSchema: {
			type: "object",
			additionalProperties: false,
			required: ["run_id", "frames"],
			properties: {
				run_id: { type: "string" },
				frames: { type: "integer", minimum: 1, maximum: 600 },
				frame_type: { type: "string", enum: ["physics", "process"] },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.runtime.get_tree",
		description: "Get a bounded runtime-only node tree snapshot using runtime node paths.",
		method: "runtime.get_tree",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id"],
			properties: {
				run_id: { type: "string" }, root_path: { type: "string" },
				max_depth: { type: "integer", minimum: 0, maximum: 12 },
				max_nodes: { type: "integer", minimum: 1, maximum: 2000 },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.runtime.get_property",
		description: "Read a serializable property from a runtime node.",
		method: "runtime.get_property",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id", "node_path", "property_name"],
			properties: { run_id: { type: "string" }, node_path: { type: "string" }, property_name: { type: "string" }, timeout_ms: { type: "integer", minimum: 100, maximum: 60000 } },
		},
	},
	{
		name: "godot.runtime.set_property",
		description: "Set a property on a runtime node and return its resulting serializable value.",
		method: "runtime.set_property",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id", "node_path", "property_name", "value"],
			properties: { run_id: { type: "string" }, node_path: { type: "string" }, property_name: { type: "string" }, value: {}, timeout_ms: { type: "integer", minimum: 100, maximum: 60000 } },
		},
	},
	{
		name: "godot.runtime.call",
		description: "Call a runtime-node method with up to 16 serializable arguments.",
		method: "runtime.call",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id", "node_path", "method"],
			properties: {
				run_id: { type: "string" }, node_path: { type: "string" }, method: { type: "string" },
				arguments: { type: "array", maxItems: 16, items: {} },
				confirm_destructive: { type: "boolean", description: "Required for free or queue_free" },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.runtime.input_action",
		description: "Press, release, or hold a project InputMap action for a bounded number of physics frames.",
		method: "runtime.input_action",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id", "action"],
			properties: {
				run_id: { type: "string" }, action: { type: "string" }, pressed: { type: "boolean" },
				strength: { type: "number", minimum: 0, maximum: 1 }, hold_frames: { type: "integer", minimum: 0, maximum: 600 },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.runtime.input_event",
		description: "Send bounded mouse motion or mouse-button input to an MCP-owned runtime.",
		method: "runtime.input_event",
		capability: "runtime_inspection",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id", "type"],
			properties: {
				run_id: { type: "string" }, type: { type: "string", enum: ["mouse_motion", "mouse_button"] },
				position: { type: "object", required: ["x", "y"], properties: { x: { type: "number" }, y: { type: "number" } }, additionalProperties: false },
				relative: { type: "object", required: ["x", "y"], properties: { x: { type: "number" }, y: { type: "number" } }, additionalProperties: false },
				button_index: { type: "integer", minimum: 1, maximum: 9 }, pressed: { type: "boolean" }, double_click: { type: "boolean" },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.runtime.capture_viewport",
		description: "Capture a bounded PNG from a runtime viewport after an optional frame wait.",
		method: "runtime.capture_viewport",
		capability: "viewport_capture",
		inputSchema: {
			type: "object", additionalProperties: false, required: ["run_id"],
			properties: {
				run_id: { type: "string" }, viewport: { type: "string", description: "Runtime Viewport node path (default: /root)" },
				crop: { type: "object", required: ["x", "y", "width", "height"], additionalProperties: false, properties: { x: { type: "integer" }, y: { type: "integer" }, width: { type: "integer", minimum: 1 }, height: { type: "integer", minimum: 1 } } },
				scale: { type: "number", minimum: 0.05, maximum: 4 }, frame_wait: { type: "integer", minimum: 0, maximum: 600 },
				timeout_ms: { type: "integer", minimum: 100, maximum: 60000 },
			},
		},
	},
	{
		name: "godot.diagnostics.get",
		description: "Read non-destructive, sequence-filtered editor or run-scoped diagnostics.",
		method: "diagnostics.get",
		capability: "diagnostics_v2",
		retrySafe: true,
		inputSchema: {
			type: "object", additionalProperties: false,
			properties: {
				after_sequence: { type: "integer", minimum: 0 }, limit: { type: "integer", minimum: 1, maximum: 1000 },
				session_id: { type: "string" }, run_id: { type: "string" }, source: { type: "string", enum: ["editor", "runtime"] },
				severities: { type: "array", items: { type: "string", enum: ["error", "warning", "info"] } },
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

export function is_tool_retry_safe(p_tool: ToolDefinition): boolean {
	if (p_tool.retrySafe !== undefined) {
		return p_tool.retrySafe;
	}
	return p_tool.capability.startsWith("read_") || p_tool.method.endsWith(".status") || p_tool.method.endsWith(".info");
}
