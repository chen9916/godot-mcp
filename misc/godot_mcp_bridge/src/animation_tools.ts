import type { ToolDefinition } from "./tool_registry.js";

type Schema = Record<string, unknown>;

const string: Schema = { type: "string" };
const name: Schema = { type: "string", minLength: 1, maxLength: 128 };
const index: Schema = { type: "integer", minimum: 0 };
const number: Schema = { type: "number" };
const boolean: Schema = { type: "boolean" };
const value: Schema = { $ref: "#/$defs/animationValue" };
const properties: Schema = { type: "object", additionalProperties: value };
const vector2: Schema = { type: "array", minItems: 2, maxItems: 2, items: number };
const graphPath: Schema = { type: "string", default: "", description: "Slash-separated graph resource path; empty means tree_root. These are not scene-node paths." };
const clip: Schema = { type: "string", minLength: 1, description: "Qualified library/clip name, or clip for the unnamed library." };
const library: Schema = { type: "string", description: "AnimationLibrary name; empty string selects the unnamed library." };
const resourcePath: Schema = { type: "string", pattern: "^res://", description: "Existing resource inside the project; copied before editing." };
const scenePath: Schema = { type: "string", minLength: 1, description: "Exact active res:// scene path returned by godot.scene.get_active." };
const sceneVersion: Schema = { type: "string", minLength: 1, description: "Opaque current scene_version returned by scene or animation inspection." };
const nodePath: Schema = { type: "string", minLength: 1, description: "Scene-relative node path; use . for the root." };
const timeout: Schema = { type: "integer", minimum: 100, maximum: 60_000 };

function operation(op: string, required: string[], fields: Record<string, Schema>): Schema {
	return {
		type: "object", additionalProperties: false,
		required: ["op", ...required],
		properties: { op: { type: "string", const: op, enum: [op] }, ...fields },
	};
}

export const ANIMATION_GRAPH_OPERATION_SCHEMA: Schema = {
	title: "AnimationGraphOperation", discriminator: { propertyName: "op" },
	oneOf: [
		operation("set_root", ["type"], { type: name, properties }),
		operation("add_node", ["name", "type"], { graph_path: graphPath, name, type: name, position: vector2, properties }),
		operation("remove_node", ["name"], { graph_path: graphPath, name }),
		operation("rename_node", ["name", "new_name"], { graph_path: graphPath, name, new_name: name }),
		operation("move_node", ["name", "position"], { graph_path: graphPath, name, position: vector2 }),
		operation("set_node_properties", ["properties"], { node_path: graphPath, properties }),
		operation("connect", ["from", "to", "input"], { graph_path: graphPath, from: name, to: name, input: index }),
		operation("disconnect", ["to", "input"], { graph_path: graphPath, to: name, input: index }),
		operation("add_transition", ["from", "to"], { graph_path: graphPath, from: name, to: name, properties }),
		operation("set_transition", ["from", "to", "properties"], { graph_path: graphPath, from: name, to: name, properties }),
		operation("remove_transition", ["from", "to"], { graph_path: graphPath, from: name, to: name }),
		operation("add_blend_point", ["name", "position"], {
			graph_path: graphPath, name, type: { ...name, default: "AnimationNodeAnimation" },
			position: { oneOf: [number, vector2] }, properties, at_index: { type: "integer", minimum: -1 },
		}),
		operation("set_blend_point", ["name"], { graph_path: graphPath, name, new_name: name, position: { oneOf: [number, vector2] }, properties }),
		operation("remove_blend_point", ["name"], { graph_path: graphPath, name }),
		operation("set_triangles", ["triangles"], { graph_path: graphPath, triangles: { type: "array", maxItems: 512, items: { type: "array", minItems: 3, maxItems: 3, items: { ...index, maximum: 63 } } } }),
		operation("set_filter", ["enabled", "paths"], { node_path: graphPath, enabled: boolean, paths: { type: "array", maxItems: 512, items: string } }),
	],
};

export const ANIMATION_LIBRARY_OPERATION_SCHEMA: Schema = {
	title: "AnimationLibraryOperation", discriminator: { propertyName: "op" },
	oneOf: [
		operation("add_library", ["library"], { library, resource_path: resourcePath }),
		operation("remove_library", ["library"], { library }),
		operation("rename_library", ["library", "new_name"], { library, new_name: library }),
		operation("create_animation", ["animation"], { animation: clip, properties, resource_path: resourcePath }),
		operation("remove_animation", ["animation"], { animation: clip }),
		operation("rename_animation", ["animation", "new_name"], { animation: clip, new_name: name }),
		operation("duplicate_animation", ["animation", "new_name"], { animation: clip, new_name: name }),
		operation("set_animation_properties", ["animation", "properties"], { animation: clip, properties }),
		operation("add_track", ["animation", "type", "path"], {
			animation: clip, type: { type: "string", enum: ["value", "position_3d", "rotation_3d", "scale_3d", "blend_shape", "method", "bezier", "audio", "animation"] },
			path: string, at_index: { type: "integer", minimum: -1 }, properties,
		}),
		operation("remove_track", ["animation", "track"], { animation: clip, track: index }),
		operation("set_track", ["animation", "track", "properties"], { animation: clip, track: index, properties }),
		operation("move_track", ["animation", "track", "to_index"], { animation: clip, track: index, to_index: index }),
		operation("insert_key", ["animation", "track", "time", "value"], { animation: clip, track: index, time: { ...number, minimum: 0, maximum: 1_000_000 }, value, transition: number }),
		operation("set_key", ["animation", "track", "key"], { animation: clip, track: index, key: index, time: { ...number, minimum: 0, maximum: 1_000_000 }, value, transition: number }),
		operation("remove_key", ["animation", "track", "key"], { animation: clip, track: index, key: index }),
		operation("clear_animation", ["animation"], { animation: clip }),
		operation("set_bezier_handles", ["animation", "track", "key", "in_handle", "out_handle"], {
			animation: clip, track: index, key: index, in_handle: vector2, out_handle: vector2, handle_mode: { type: "integer", minimum: 0, maximum: 3 },
		}),
	],
};

const RUNTIME_ACTIONS = [
	"player_play", "player_stop", "player_pause", "player_seek", "player_queue", "player_advance",
	"state_start", "state_travel", "state_stop", "state_next", "set_parameter", "set_active",
	"oneshot_fire", "oneshot_abort", "oneshot_fadeout", "transition_request",
];

export function get_animation_schema_definitions(variantDefinitions: Record<string, unknown>): Schema {
	const typedVariants = variantDefinitions.typedVariant as { oneOf: Schema[] };
	const wrapper = (type: string, payload: Schema): Schema => ({
		type: "object", additionalProperties: false, required: ["__type", "value"],
		properties: { __type: { type: "string", const: type, enum: [type] }, value: payload },
	});
	const integer = wrapper("int", { type: "string", pattern: "^-?(0|[1-9][0-9]{0,18})$", maxLength: 20, description: "Lossless signed 64-bit Godot integer in decimal. Native validation enforces the int64 range." });
	const typed = typedVariants.oneOf.map((entry) => {
		const fields = entry.properties as Record<string, Schema>;
		if (fields.__type.const === "PackedByteArray") {
			return wrapper("PackedByteArray", { type: "array", maxItems: 10_000, items: { anyOf: [{ type: "integer", minimum: 0, maximum: 255 }, integer] } });
		}
		if (Array.isArray(fields.__type.enum) && fields.__type.enum.includes("PackedInt64Array")) {
			return wrapper("PackedInt32Array", { type: "array", maxItems: 10_000, items: { anyOf: [{ type: "integer", minimum: -2_147_483_648, maximum: 2_147_483_647 }, integer] } });
		}
		if (fields.__type.const !== "Transform3D") return entry;
		return wrapper("Transform3D", {
			oneOf: [fields.value, { type: "array", minItems: 2, maxItems: 2, items: value, description: "Typed Basis followed by typed Vector3 origin, as exported by animation.get." }],
		});
	});
	typed.push(
		integer,
		wrapper("PackedInt64Array", { type: "array", maxItems: 10_000, items: { anyOf: [{ type: "integer", minimum: Number.MIN_SAFE_INTEGER, maximum: Number.MAX_SAFE_INTEGER }, integer] } }),
		wrapper("Array", { type: "array", maxItems: 10_000, items: value }),
		wrapper("Dictionary", { type: "array", maxItems: 10_000, items: { type: "array", minItems: 2, maxItems: 2, items: value }, description: "[key,value] entries preserve non-string keys and literal reserved names." }),
		wrapper("Projection", { oneOf: [
			{ type: "array", minItems: 16, maxItems: 16, items: number },
			{ type: "array", minItems: 4, maxItems: 4, items: value, description: "Four typed Vector4 columns." },
		] }),
		...["PackedVector2Array", "PackedVector3Array", "PackedVector4Array", "PackedColorArray"].map((type) => wrapper(type, {
			type: "array", maxItems: 10_000, items: value, description: "Each element must be a corresponding typed vector/color wrapper, as exported by animation.get.",
		})),
	);
	return {
		...variantDefinitions,
		typedVariant: { description: "Explicit bounded Godot Variant encodings; accepts animation.gets reinsertable typed values.", discriminator: { propertyName: "__type" }, oneOf: typed },
		animationValue: {
			description: "Bounded recursively encoded Godot Variant; vectors may use typed wrappers. Resources use external_resource/resource_path, never request-local scene-patch IDs.",
			anyOf: [
				{ type: "null" }, boolean, { type: "integer" }, number, string,
				{ type: "array", items: value },
				{ $ref: "#/$defs/typedVariant" },
				{
					type: "object", additionalProperties: false,
					properties: { external_resource: resourcePath, resource_path: resourcePath, resource_type: string, embedded: boolean },
					oneOf: [{ required: ["external_resource"] }, { required: ["resource_path"] }],
				},
				{ type: "object", additionalProperties: value, not: { anyOf: [{ required: ["__type"] }, { required: ["external_resource"] }, { required: ["resource_path"] }] } },
			],
		},
	};
}

export function get_animation_schema_document(variantDefinitions: Record<string, unknown>): Schema {
	const runtimeTools = create_animation_tool_definitions(variantDefinitions).filter((tool) => tool.method.startsWith("runtime."));
	return {
		schema_version: 1, operation_limit: 512,
		limits: {
			scene_nodes: 10_000, graph_nodes: 2000, graph_depth: 32, graph_ports_or_points: 64,
			graph_transitions_triangles_or_filters: 512, tracks_per_clip: 2000,
			decoded_keys_per_track: 10_000, decoded_keys_per_unique_resource_graph: 25_000,
			variant_depth: 16, variant_weighted_items: 10_000, clone_packed_buffer_bytes: 64 * 1024 * 1024,
		},
		graph_operations: ANIMATION_GRAPH_OPERATION_SCHEMA,
		library_operations: ANIMATION_LIBRARY_OPERATION_SCHEMA,
		runtime_actions: RUNTIME_ACTIONS,
		runtime_schemas: Object.fromEntries(runtimeTools.map((tool) => [tool.method, tool.inputSchema])),
		key_formats: {
			value,
			position_3d: { description: "Vector3 (or Vector3i), preferably encoded as {__type:'Vector3',value:[x,y,z]}.", $ref: "#/$defs/animationValue" },
			rotation_3d: { description: "Quaternion, encoded as {__type:'Quaternion',value:[x,y,z,w]}; Basis can also represent rotation.", $ref: "#/$defs/animationValue" },
			scale_3d: { description: "Vector3 (or Vector3i), preferably encoded as {__type:'Vector3',value:[x,y,z]}.", $ref: "#/$defs/animationValue" },
			blend_shape: number,
			method: { type: "object", required: ["method", "args"], additionalProperties: false, properties: { method: name, args: { type: "array", maxItems: 10_000, items: value } } },
			bezier: { oneOf: [
				{ description: "Native five-component key representation [value,in_x,in_y,out_x,out_y].", type: "array", minItems: 5, maxItems: 5, items: number },
				{ type: "object", required: ["value"], additionalProperties: false, properties: { value: number, in_handle: vector2, out_handle: vector2 } },
			] },
			audio: {
				type: "object", additionalProperties: false,
				properties: { stream: { description: "AudioStream external_resource/resource_path reference, or null for a silent key.", $ref: "#/$defs/animationValue" }, start_offset: { ...number, minimum: 0, maximum: 1_000_000, default: 0 }, end_offset: { ...number, minimum: 0, maximum: 1_000_000, default: 0 } },
			},
			animation: string,
		},
		$defs: get_animation_schema_definitions(variantDefinitions),
		notes: [
			"Create AnimationPlayer/AnimationTree scene nodes using scene.apply_patch, then use animation graph/library patches for animation resources.",
			"Editor mutations require the current active scene path and scene_version and execute atomically in array order. dry_run creates no undo action.",
			"Graph paths are relative to tree_root; the empty path means the root. Scene node paths and graph resource paths are distinct.",
			"A blend connection flows from the source output to the target input index. StateMachine connections use transition operations.",
			"Patches create local copies before editing shared or imported resources. Use resource.save to persist the scene after successful edits.",
			"Ordinary non-@tool AnimationTree controller scripts and exported values are preserved during graph edits and undo/redo. Editor-executing AnimationTree scripts and custom scripted AnimationNode resources are not supported for graph authoring.",
			"Runtime operations require an MCP-owned run_id. State playback uses playback_path, defaulting to parameters/playback.",
			"Integer key values and Dictionary keys inspect as {__type:'int',value:'decimal'} to preserve identity and full int64 precision through JSON. Use typed integer elements for PackedInt64Array values outside JavaScript's safe range.",
		],
	};
}

export function create_animation_tool_definitions(variantDefinitions: Record<string, unknown>): ToolDefinition[] {
	const definitions = get_animation_schema_definitions(variantDefinitions);
	const readFields = { expected_scene_path: scenePath, node_path: nodePath };
	const patchSchema = (operations: Schema): Schema => ({
		type: "object", additionalProperties: false, $defs: definitions,
		required: ["expected_scene_path", "expected_scene_version", "node_path", "operations"],
		properties: {
			...readFields, expected_scene_version: sceneVersion, dry_run: { ...boolean, default: false },
			operations: { type: "array", minItems: 1, maxItems: 512, items: operations },
		},
	});
	const runtimeFields = {
		run_id: { type: "string", minLength: 1 },
		node_path: { type: "string", pattern: "^/root(?:/|$)", description: "Runtime path to an AnimationPlayer or AnimationTree in the owned game process." },
		playback_path: { type: "string", default: "parameters/playback", description: "AnimationTree playback parameter, including nested state-machine paths." },
		parameter_names: { type: "array", maxItems: 256, items: string },
		limit: { type: "integer", minimum: 1, maximum: 2000, default: 256 },
		timeout_ms: timeout,
	};
	return [
		{
			name: "godot.animation.schema", method: "bridge.animation.schema", capability: "read_scene", retrySafe: true,
			description: "Return the complete animation graph, library, track/key, Variant, and runtime action schemas, without requiring an editor connection.",
			inputSchema: { type: "object", additionalProperties: false, properties: {} },
		},
		{
			name: "godot.animation.list", method: "animation.list", capability: "read_scene", retrySafe: true,
			description: "Discover animation mixers, libraries, and available clips with duration, loop mode, track counts, and resource provenance in the edited scene.",
			inputSchema: { type: "object", additionalProperties: false, properties: { ...readFields, limit: { type: "integer", minimum: 1, maximum: 2000, default: 256 } } },
		},
		{
			name: "godot.animation.get", method: "animation.get", capability: "read_scene", retrySafe: true,
			description: "Inspect one animation clip and its typed tracks, paths, interpolation settings, and optionally bounded keyframes.",
			inputSchema: {
				type: "object", additionalProperties: false, required: ["node_path", "animation"],
				properties: { ...readFields, animation: clip, include_keys: { ...boolean, default: false }, key_limit: { type: "integer", minimum: 1, maximum: 10_000, default: 1000 } },
			},
		},
		{
			name: "godot.animation.apply_patch", method: "animation.apply_patch", capability: "write_scene", retrySafe: false,
			description: "Atomically edit animation libraries, clips, all track types, and keyframes on local copies in one undo action. Requires current scene guards; supports dry_run; save the scene explicitly afterward.",
			inputSchema: patchSchema(ANIMATION_LIBRARY_OPERATION_SCHEMA),
		},
		{
			name: "godot.animation.graph_get", method: "animation.graph_get", capability: "read_scene", retrySafe: true,
			description: "Inspect a nested AnimationTree graph with resource paths, nodes, input ports, connections, state transitions, blend points, triangles, filters, and parameters.",
			inputSchema: {
				type: "object", additionalProperties: false, required: ["node_path"], properties: {
					...readFields, max_nodes: { type: "integer", minimum: 1, maximum: 2000, default: 256 },
					max_depth: { type: "integer", minimum: 0, maximum: 32, default: 12 }, include_properties: { ...boolean, default: true },
				},
			},
		},
		{
			name: "godot.animation.graph_apply_patch", method: "animation.graph_apply_patch", capability: "write_scene", retrySafe: false,
			description: "Build or edit nested BlendTrees, StateMachines, 1D/2D blend spaces, transitions, one-shots and filters using ordered structural operations, atomic validation, local copies, and one undo action. Preserves ordinary non-@tool AnimationTree controller scripts.",
			inputSchema: patchSchema(ANIMATION_GRAPH_OPERATION_SCHEMA),
		},
		{
			name: "godot.animation.graph_validate", method: "animation.graph_validate", capability: "read_scene", retrySafe: true,
			description: "Validate an AnimationTree against its player and clips; report missing bindings, invalid connections, cycles, state reachability, blend-space problems, and animation track targets.",
			inputSchema: { type: "object", additionalProperties: false, required: ["node_path"], properties: readFields },
		},
		{
			name: "godot.runtime.animation_get", method: "runtime.animation_get", capability: "runtime_inspection", retrySafe: true,
			description: "Read owned-runtime AnimationPlayer/AnimationTree playback, selected parameters, nested state-machine travel state, and root motion without changing playback.",
			inputSchema: {
				type: "object", additionalProperties: false, required: ["run_id", "node_path"], properties: runtimeFields,
			},
		},
		{
			name: "godot.runtime.animation_control", method: "runtime.animation_control", capability: "runtime_inspection", retrySafe: false,
			description: "Control owned-runtime clip playback, seek/queue/advance, nested state-machine start/travel/stop/next, typed parameters, one-shot requests and transitions; return the resulting playback snapshot.",
			inputSchema: {
				type: "object", additionalProperties: false, $defs: definitions, required: ["run_id", "node_path", "action"],
				properties: {
					...runtimeFields, action: { type: "string", enum: RUNTIME_ACTIONS },
					animation: clip, state: name, parameter: string, value,
					time: { ...number, minimum: 0, maximum: 31_540_000 }, delta: { ...number, minimum: -31_540_000, maximum: 31_540_000 },
					custom_blend: { oneOf: [{ type: "number", const: -1 }, { ...number, minimum: 0, maximum: 31_540_000 }], default: -1 },
					custom_speed: { ...number, minimum: -1_000_000, maximum: 1_000_000, default: 1 },
					from_end: { ...boolean, default: false }, update: { ...boolean, default: true },
					reset: { ...boolean, default: true }, keep_state: { ...boolean, default: false }, active: boolean, transition: string,
				},
				allOf: [
					{ if: { properties: { action: { const: "player_queue" } } }, then: { required: ["animation"] } },
					{ if: { properties: { action: { const: "player_seek" } } }, then: { required: ["time"] } },
					{ if: { properties: { action: { const: "player_advance" } } }, then: { required: ["delta"] } },
					{ if: { properties: { action: { enum: ["state_start", "state_travel"] } } }, then: { required: ["state"] } },
					{ if: { properties: { action: { const: "set_parameter" } } }, then: { required: ["parameter", "value"] } },
					{ if: { properties: { action: { const: "set_active" } } }, then: { required: ["active"] } },
					{ if: { properties: { action: { enum: ["oneshot_fire", "oneshot_abort", "oneshot_fadeout"] } } }, then: { required: ["parameter"] } },
					{ if: { properties: { action: { const: "transition_request" } } }, then: { required: ["parameter", "transition"] } },
				],
			},
		},
	];
}
