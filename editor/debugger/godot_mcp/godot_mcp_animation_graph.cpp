/**************************************************************************/
/*  godot_mcp_animation_graph.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "godot_mcp_animation_graph.h"

#include "core/math/delaunay_2d.h"
#include "core/math/expression.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_instance.h"
#include "core/object/script_language.h"
#include "core/object/undo_redo.h"
#include "core/templates/hash_set.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/animation/animation_blend_space_1d.h"
#include "scene/animation/animation_blend_space_2d.h"
#include "scene/animation/animation_blend_tree.h"
#include "scene/animation/animation_node_state_machine.h"
#include "scene/animation/animation_player.h"

namespace {

constexpr const char *GRAPH_ACTION = "Godot MCP: Apply Animation Graph Patch";
constexpr int MAX_ITEMS = 512;
constexpr int MAX_PORTS = 64;
constexpr int MAX_STRING = 4096;

static bool _bounded_graph(const Ref<AnimationNode> &p_node, HashSet<ObjectID> &r_active, int &r_count, String &r_message, int p_depth = 0);

static bool _scripted(Object *p_object) {
	return p_object->get_script_instance() || p_object->get_script().get_validated_object();
}

static bool _editor_executing_script(Object *p_object) {
	ScriptInstance *instance = p_object->get_script_instance();
	if (instance && !instance->is_placeholder()) {
		return true;
	}
	const Ref<Script> script = p_object->get_script();
	return script.is_valid() && script->is_tool();
}

static void _native_children(const Ref<AnimationNode> &p_node, LocalVector<AnimationNode::ChildNode> &r_children) {
	if (AnimationNodeBlendTree *blend = Object::cast_to<AnimationNodeBlendTree>(p_node.ptr())) {
		blend->AnimationNodeBlendTree::get_child_nodes(&r_children);
	} else if (AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(p_node.ptr())) {
		machine->AnimationNodeStateMachine::get_child_nodes(&r_children);
	} else if (AnimationNodeBlendSpace1D *space1 = Object::cast_to<AnimationNodeBlendSpace1D>(p_node.ptr())) {
		space1->AnimationNodeBlendSpace1D::get_child_nodes(&r_children);
	} else if (AnimationNodeBlendSpace2D *space2 = Object::cast_to<AnimationNodeBlendSpace2D>(p_node.ptr())) {
		space2->AnimationNodeBlendSpace2D::get_child_nodes(&r_children);
	}
}

static bool _fail(String &r_message, const String &p_message) {
	r_message = "INVALID_ARGUMENT: " + p_message;
	return false;
}

static bool _is_string(const Variant &p_value) {
	return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
}

static bool _string(const Dictionary &p_input, const String &p_key, String &r_value, String &r_message, bool p_required = true) {
	if (!p_input.has(p_key) && !p_required) {
		r_value.clear();
		return true;
	}
	const Variant value = p_input.get(p_key, Variant());
	if (!_is_string(value) || String(value).length() > MAX_STRING || (p_required && String(value).is_empty())) {
		return _fail(r_message, p_key + " must be a bounded string" + (p_required ? String(" and must not be empty.") : String(".")));
	}
	r_value = value;
	return true;
}

static bool _number(const Variant &p_value, double &r_number) {
	if (p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) {
		return false;
	}
	r_number = p_value;
	// All graph numeric fields fit engine real_t/float storage without overflow.
	return Math::is_finite(r_number) && Math::abs(r_number) <= 1e12;
}

static bool _integer(const Variant &p_value, int p_min, int p_max, int &r_integer) {
	double number;
	if (!_number(p_value, number) || Math::floor(number) != number || number < p_min || number > p_max) {
		return false;
	}
	r_integer = int(number);
	return true;
}

static bool _name(const String &p_name, String &r_message) {
	if (p_name.is_empty() || p_name.length() > 128 || p_name.strip_edges() != p_name || p_name.contains_char('.') || p_name.contains_char('/') || p_name.contains_char(':') || p_name.contains_char('\\')) {
		return _fail(r_message, "Names must be 1..128 characters without dots, slashes, colons, or edge whitespace.");
	}
	for (int i = 0; i < p_name.length(); i++) {
		if (p_name[i] < 32) {
			return _fail(r_message, "Names must not contain control characters.");
		}
	}
	return true;
}

static bool _graph_path(const String &p_path, String &r_message) {
	if (p_path.is_empty()) {
		return true;
	}
	const PackedStringArray parts = p_path.split("/", true);
	if (parts.size() > GodotMCPAnimationGraph::MAX_DEPTH) {
		return _fail(r_message, "Graph path exceeds 32 child segments.");
	}
	for (const String &part : parts) {
		if (!_name(part, r_message)) {
			return false;
		}
	}
	return true;
}

static Ref<AnimationNode> _resolve(const Ref<AnimationNode> &p_root, const String &p_path) {
	Ref<AnimationNode> node = p_root;
	if (!p_path.is_empty()) {
		for (const String &part : p_path.split("/")) {
			if (node.is_null()) {
				return Ref<AnimationNode>();
			}
			LocalVector<AnimationNode::ChildNode> children;
			_native_children(node, children);
			Ref<AnimationNode> found;
			for (const AnimationNode::ChildNode &child : children) {
				if (child.name == StringName(part)) {
					found = child.node;
					break;
				}
			}
			node = found;
		}
	}
	return node;
}

static UndoRedo *_history(const GodotMCPScenePatch::Context &p_context) {
	return p_context.undo_redo ? p_context.undo_redo : (p_context.undo_redo_manager ? p_context.undo_redo_manager->get_history_undo_redo(p_context.history_id) : nullptr);
}

static AnimationTree *_target(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context, bool p_mutation, int &r_code, String &r_message) {
	r_code = 0;
	r_message.clear();
	if (!p_context.root) {
		r_code = GodotMCPError::NOT_FOUND;
		r_message = "NOT_FOUND: No active edited scene.";
		return nullptr;
	}
	String expected;
	if ((p_mutation || p_params.has("expected_scene_path")) && (!_string(p_params, "expected_scene_path", expected, r_message) || !expected.begins_with("res://"))) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_message = "INVALID_ARGUMENT: expected_scene_path must be a non-empty res:// scene path.";
		return nullptr;
	}
	if (!expected.is_empty() && expected != p_context.scene_path) {
		r_code = GodotMCPError::CONFLICT;
		r_message = "CONFLICT: Active scene does not match expected_scene_path.";
		return nullptr;
	}
	String path;
	if (!_string(p_params, "node_path", path, r_message)) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		return nullptr;
	}
	const NodePath node_path(path);
	bool contained_path = !node_path.is_absolute() && node_path.get_subname_count() == 0;
	for (int i = 0; i < node_path.get_name_count(); i++) {
		contained_path = contained_path && node_path.get_name(i) != SNAME("..");
	}
	if (!contained_path) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_message = "INVALID_ARGUMENT: node_path must be scene-relative without parent traversal or property subnames.";
		return nullptr;
	}
	Node *node = p_context.root->get_node_or_null(node_path);
	if (!node || (node != p_context.root && !p_context.root->is_ancestor_of(node))) {
		r_code = GodotMCPError::NOT_FOUND;
		r_message = "NOT_FOUND: node_path is not in the active scene.";
		return nullptr;
	}
	AnimationTree *tree = Object::cast_to<AnimationTree>(node);
	if (!tree) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_message = "INVALID_ARGUMENT: node_path must identify an AnimationTree.";
		return nullptr;
	}
	if (p_mutation) {
		if (_editor_executing_script(tree)) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			r_message = "INVALID_ARGUMENT: Graph authoring does not support editor-executing AnimationTree scripts (@tool or live script instances); ordinary controller scripts are preserved.";
			return nullptr;
		}
		// Check ALL scene AnimationTrees before any version/property enumeration,
		// not just the target: another tree may contain a cyclic hierarchy.
		if (!GodotMCPAnimationGraph::preflight_scene(p_context.root, r_message, true)) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			return nullptr;
		}
		String version;
		if (!_string(p_params, "expected_scene_version", version, r_message)) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			return nullptr;
		}
		UndoRedo *history = _history(p_context);
		const String current = history ? GodotMCPScenePatch::compute_scene_version(p_context.root, p_context.history_id, history->get_version()) : p_context.scene_version;
		if (version != current || version != p_context.scene_version) {
			r_code = GodotMCPError::CONFLICT;
			r_message = "CONFLICT: Scene version no longer matches expected_scene_version.";
			return nullptr;
		}
	}
	return tree;
}

static Array _vector(const Vector2 &p_value) {
	Array result;
	result.push_back(Math::is_finite(p_value.x) ? Variant(p_value.x) : Variant());
	result.push_back(Math::is_finite(p_value.y) ? Variant(p_value.y) : Variant());
	return result;
}

static bool _vector2(const Variant &p_input, Vector2 &r_value) {
	Variant value = p_input;
	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary wrapper = value;
		if (wrapper.get("__type", Variant()) != Variant("Vector2")) {
			return false;
		}
		value = wrapper.get("value", Variant());
	}
	if (value.get_type() == Variant::VECTOR2) {
		r_value = value;
		return r_value.is_finite() && Math::abs(r_value.x) <= 1e12 && Math::abs(r_value.y) <= 1e12;
	}
	if (value.get_type() != Variant::ARRAY) {
		return false;
	}
	const Array array = value;
	double x, y;
	if (array.size() != 2 || !_number(array[0], x) || !_number(array[1], y)) {
		return false;
	}
	r_value = Vector2(x, y);
	return true;
}

// No object handles escape through the protocol. Resource values are descriptors,
// not live references, and all containers/strings have explicit output bounds.
static Variant _encode(const Variant &p_value, bool &r_truncated, int p_depth = 0) {
	if (p_depth >= 8) {
		r_truncated = true;
		return Variant();
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
			return p_value;
		case Variant::FLOAT:
			return Math::is_finite(double(p_value)) ? p_value : Variant();
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			const String text = p_value;
			r_truncated = r_truncated || text.length() > MAX_STRING;
			return text.left(MAX_STRING);
		}
		case Variant::VECTOR2:
			return _vector(p_value);
		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			Array result;
			result.push_back(v.x);
			result.push_back(v.y);
			return result;
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			Array result;
			for (int i = 0; i < 3; i++) {
				result.push_back(Math::is_finite(v[i]) ? Variant(v[i]) : Variant());
			}
			return result;
		}
		case Variant::COLOR: {
			const Color v = p_value;
			Array result;
			for (int i = 0; i < 4; i++) {
				result.push_back(Math::is_finite(v[i]) ? Variant(v[i]) : Variant());
			}
			return result;
		}
		case Variant::ARRAY: {
			const Array source = p_value;
			Array result;
			r_truncated = r_truncated || source.size() > MAX_ITEMS;
			for (int i = 0; i < MIN(source.size(), MAX_ITEMS); i++) {
				result.push_back(_encode(source[i], r_truncated, p_depth + 1));
			}
			return result;
		}
		case Variant::DICTIONARY: {
			const Dictionary source = p_value;
			Dictionary result;
			int count = 0;
			for (const KeyValue<Variant, Variant> &E : source) {
				if (count++ >= MAX_ITEMS) {
					r_truncated = true;
					break;
				}
				if (_is_string(E.key)) {
					result[String(E.key).left(MAX_STRING)] = _encode(E.value, r_truncated, p_depth + 1);
				}
			}
			return result;
		}
		case Variant::OBJECT: {
			Object *object = p_value.get_validated_object();
			if (!object) {
				return Variant();
			}
			Dictionary descriptor;
			descriptor["class"] = object->get_class();
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				descriptor["resource_path"] = resource->get_path().left(MAX_STRING);
			}
			return descriptor;
		}
		default:
			// Do not construct an unbounded textual representation of packed data.
			Dictionary descriptor;
			descriptor["__type"] = Variant::get_type_name(p_value.get_type());
			return descriptor;
	}
}

static bool _structural_property(const String &p_name) {
	return p_name == "script" || p_name.begins_with("resource_") || p_name.begins_with("metadata/") || p_name.begins_with("nodes/") || p_name.begins_with("blend_point_") || p_name == "node_connections" || p_name == "transitions" || p_name == "triangles" || p_name == "filters" || p_name == "filter_enabled";
}

static Dictionary _properties(Object *p_object, bool &r_truncated) {
	Dictionary result;
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	int count = 0;
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE) || _structural_property(property.name)) {
			continue;
		}
		if (count++ >= MAX_ITEMS) {
			r_truncated = true;
			break;
		}
		result[property.name] = _encode(p_object->get(property.name), r_truncated);
	}
	return result;
}

static bool _decode(const Variant &p_input, const PropertyInfo &p_property, Variant &r_value, String &r_message) {
	Variant value = p_input;
	bool explicit_integer = false;
	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary wrapper = value;
		if (!_is_string(wrapper.get("__type", Variant())) || String(wrapper["__type"]) != Variant::get_type_name(p_property.type) || !wrapper.has("value")) {
			return _fail(r_message, "Property " + String(p_property.name) + " requires a matching {__type,value} wrapper.");
		}
		value = wrapper["value"];
		explicit_integer = p_property.type == Variant::INT;
	}
	switch (p_property.type) {
		case Variant::BOOL:
			if (value.get_type() != Variant::BOOL) {
				return _fail(r_message, "Property " + String(p_property.name) + " requires a boolean.");
			}
			r_value = value;
			break;
		case Variant::INT: {
			if (explicit_integer && value.get_type() == Variant::STRING) {
				const String decimal = value;
				const bool negative = decimal.begins_with("-");
				const String digits = negative ? decimal.substr(1) : decimal;
				const String bound = negative ? "2147483648" : "2147483647";
				if (digits.is_empty() || digits.length() > 10 || (digits.length() > 1 && digits[0] == '0') ||
						(digits.length() == 10 && digits > bound)) {
					return _fail(r_message, "Graph integer wrappers require a bounded 32-bit decimal value.");
				}
				for (int i = 0; i < digits.length(); i++) {
					if (digits[i] < '0' || digits[i] > '9') {
						return _fail(r_message, "Graph integer wrappers require a decimal value.");
					}
				}
				value = decimal.to_int();
			}
			int integer;
			if (!_integer(value, INT32_MIN, INT32_MAX, integer)) {
				return _fail(r_message, "Property " + String(p_property.name) + " requires a finite 32-bit integer.");
			}
			r_value = integer;
			if (p_property.hint == PROPERTY_HINT_ENUM) {
				bool found = false;
				int enum_value = 0;
				for (const String &item : p_property.hint_string.split(",")) {
					if (item.contains_char(':')) {
						enum_value = item.get_slicec(':', 1).to_int();
					}
					found = found || enum_value == integer;
					enum_value++;
				}
				if (!found) {
					return _fail(r_message, "Property " + String(p_property.name) + " has an invalid enum value.");
				}
			}
		} break;
		case Variant::FLOAT: {
			double number;
			if (!_number(value, number)) {
				return _fail(r_message, "Property " + String(p_property.name) + " requires a finite number with magnitude <= 1e12.");
			}
			r_value = number;
		} break;
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			if (!_is_string(value) || String(value).length() > MAX_STRING) {
				return _fail(r_message, "Property " + String(p_property.name) + " requires a bounded string.");
			}
			if (p_property.type == Variant::STRING_NAME) {
				r_value = StringName(String(value));
			} else if (p_property.type == Variant::NODE_PATH) {
				r_value = NodePath(String(value));
			} else {
				r_value = String(value);
			}
			break;
		case Variant::VECTOR2: {
			Vector2 vector;
			if (!_vector2(value, vector)) {
				return _fail(r_message, "Property " + String(p_property.name) + " requires two finite coordinates.");
			}
			r_value = vector;
		} break;
		case Variant::OBJECT:
			if (value.get_type() != Variant::NIL) {
				return _fail(r_message, "Resource/object retargeting is not supported in graph properties.");
			}
			r_value = Variant();
			break;
		default:
			return _fail(r_message, "Unsupported graph property Variant type: " + Variant::get_type_name(p_property.type) + ".");
	}
	if ((p_property.type == Variant::FLOAT || p_property.type == Variant::INT) && p_property.hint == PROPERTY_HINT_RANGE) {
		const PackedStringArray range = p_property.hint_string.split(",");
		if (range.size() >= 2) {
			const double number = r_value;
			if ((!range.has("or_less") && number < range[0].to_float()) || (!range.has("or_greater") && number > range[1].to_float())) {
				return _fail(r_message, "Property " + String(p_property.name) + " is outside its declared range.");
			}
		}
	}
	return true;
}

static bool _space_limits(Object *p_object, const Dictionary &p_properties, String &r_message) {
	if (!p_properties.has("min_space") && !p_properties.has("max_space")) {
		return true;
	}
	AnimationNodeBlendSpace1D *space1 = Object::cast_to<AnimationNodeBlendSpace1D>(p_object);
	AnimationNodeBlendSpace2D *space2 = Object::cast_to<AnimationNodeBlendSpace2D>(p_object);
	if (!space1 && !space2) {
		return _fail(r_message, "min_space/max_space require a blend space.");
	}
	Variant minimum = p_object->get(SNAME("min_space"));
	Variant maximum = p_object->get(SNAME("max_space"));
	for (const StringName &name : { SNAME("min_space"), SNAME("max_space") }) {
		if (p_properties.has(name)) {
			PropertyInfo info;
			Variant decoded;
			if (!ClassDB::get_property_info(p_object->get_class_name(), name, &info) || !_decode(p_properties[name], info, decoded, r_message)) {
				return false;
			}
			(name == SNAME("min_space") ? minimum : maximum) = decoded;
		}
	}
	if (space1) {
		const double min = minimum;
		const double max = maximum;
		if (min >= max) {
			return _fail(r_message, "min_space must be smaller than max_space.");
		}
		// Expand first to avoid the engine's setter clamping when the new range
		// lies entirely above/below the old range. Dictionary order is irrelevant.
		space1->set_min_space(MIN(space1->get_min_space(), min));
		space1->set_max_space(MAX(space1->get_max_space(), max));
		space1->set_min_space(min);
		space1->set_max_space(max);
	} else {
		const Vector2 min = minimum;
		const Vector2 max = maximum;
		if (min.x >= max.x || min.y >= max.y) {
			return _fail(r_message, "Each min_space axis must be smaller than max_space.");
		}
		space2->set_min_space(space2->get_min_space().min(min));
		space2->set_max_space(space2->get_max_space().max(max));
		space2->set_min_space(min);
		space2->set_max_space(max);
	}
	return true;
}

static bool _set_properties(Object *p_object, const Dictionary &p_operation, String &r_message) {
	if (!p_operation.has("properties")) {
		return true;
	}
	if (p_operation["properties"].get_type() != Variant::DICTIONARY) {
		return _fail(r_message, "properties must be a dictionary.");
	}
	const Dictionary properties = p_operation["properties"];
	if (properties.size() > MAX_ITEMS) {
		return _fail(r_message, "Too many properties.");
	}
	if (!_space_limits(p_object, properties, r_message)) {
		return false;
	}
	AnimationNodeTransition *transition = Object::cast_to<AnimationNodeTransition>(p_object);
	// This pseudo-property is deliberately handled before getting the dynamic
	// property list. Its public API resizes inputs and notifies enclosing graphs.
	if (properties.has("input_count")) {
		int count;
		PropertyInfo info(Variant::INT, "input_count");
		Variant decoded;
		if (!transition || !_decode(properties["input_count"], info, decoded, r_message) || !_integer(decoded, 0, MAX_PORTS, count)) {
			return _fail(r_message, "input_count is only supported on AnimationNodeTransition and must be 0..64.");
		}
		transition->set_input_count(count);
	}
	List<PropertyInfo> list;
	p_object->get_property_list(&list);
	Vector<String> names;
	for (const KeyValue<Variant, Variant> &E : properties) {
		if (!_is_string(E.key)) {
			return _fail(r_message, "Property names must be strings.");
		}
		names.push_back(E.key);
	}
	names.sort();
	for (const String &name : names) {
		if (name == "input_count" || name == "min_space" || name == "max_space") {
			continue;
		}
		if (_structural_property(name) || name == "tree_root" || name.contains_char(':') || (name.contains_char('/') && !(transition && name.begins_with("input_")))) {
			return _fail(r_message, "Identity, script, path, filter, and structural property writes are forbidden: " + name + ".");
		}
		PropertyInfo info;
		// Unvalidated native PropertyInfo also exposes settings hidden by sibling
		// properties (e.g. custom timelines). Their input type is still exact.
		bool found = ClassDB::get_property_info(p_object->get_class_name(), name, &info);
		if (!found) {
			for (const PropertyInfo &property : list) {
				if (property.name == StringName(name)) {
					info = property;
					found = true;
					break;
				}
			}
		}
		if (!found || !(info.usage & PROPERTY_USAGE_STORAGE) || (info.usage & PROPERTY_USAGE_READ_ONLY)) {
			return _fail(r_message, "Unknown or read-only graph property: " + name + ".");
		}
		Variant value;
		if (!_decode(properties[name], info, value, r_message)) {
			return false;
		}
		if ((name == "timeline_length" || name == "cyclic_length") && double(value) < 0) {
			return _fail(r_message, name + " must not be negative.");
		}
		if (name == "advance_condition" && (String(value).contains_char('/') || String(value).contains_char(':'))) {
			return _fail(r_message, "advance_condition must not contain slashes or colons.");
		}
		if (name == "advance_expression" && !String(value).strip_edges().is_empty()) {
			Ref<Expression> expression;
			expression.instantiate();
			if (expression->parse(value) != OK) {
				return _fail(r_message, "Invalid advance_expression: " + expression->get_error_text());
			}
		}
		if (transition && name.begins_with("input_")) {
			const int index = name.get_slicec('/', 0).get_slicec('_', 1).to_int();
			const String field = name.get_slicec('/', 1);
			if (field == "name") {
				if (!_name(value, r_message) || !transition->set_input_name(index, value)) {
					return _fail(r_message, "Invalid transition input name.");
				}
			} else if (field == "auto_advance") {
				transition->set_input_as_auto_advance(index, value);
			} else if (field == "break_loop_at_end") {
				transition->set_input_break_loop_at_end(index, value);
			} else if (field == "reset") {
				transition->set_input_reset(index, value);
			}
		} else {
			bool valid = false;
			p_object->set(name, value, &valid);
			if (!valid) {
				return _fail(r_message, "Engine rejected graph property: " + name + ".");
			}
		}
	}
	if (transition) {
		HashSet<String> input_names;
		for (int i = 0; i < transition->get_input_count(); i++) {
			const String name = transition->get_input_name(i);
			if (input_names.has(name)) {
				return _fail(r_message, "Transition input names must be unique.");
			}
			input_names.insert(name);
		}
	}
	return true;
}

static bool _supported_type(const String &p_type) {
	return p_type == "AnimationNodeAnimation" || p_type == "AnimationNodeBlendTree" || p_type == "AnimationNodeStateMachine" || p_type == "AnimationNodeBlendSpace1D" || p_type == "AnimationNodeBlendSpace2D" || p_type == "AnimationNodeOneShot" || p_type == "AnimationNodeAdd2" || p_type == "AnimationNodeAdd3" || p_type == "AnimationNodeSub2" || p_type == "AnimationNodeBlend2" || p_type == "AnimationNodeBlend3" || p_type == "AnimationNodeTimeScale" || p_type == "AnimationNodeTimeSeek" || p_type == "AnimationNodeTransition";
}

static Ref<AnimationNode> _create(const Dictionary &p_operation, bool p_root, String &r_message, const String &p_default = String()) {
	String type = p_default;
	if (p_operation.has("type") || type.is_empty()) {
		if (!_string(p_operation, "type", type, r_message)) {
			return Ref<AnimationNode>();
		}
	}
	if (!_supported_type(type)) {
		_fail(r_message, "Unsupported native AnimationNode type: " + type + ".");
		return Ref<AnimationNode>();
	}
	Ref<AnimationNode> node = Ref<AnimationNode>(Object::cast_to<AnimationNode>(ClassDB::instantiate(type)));
	if (node.is_null() || (p_root && !Object::cast_to<AnimationRootNode>(node.ptr()))) {
		_fail(r_message, "This graph requires an AnimationRootNode, not " + type + ".");
		return Ref<AnimationNode>();
	}
	if (!_set_properties(node.ptr(), p_operation, r_message)) {
		return Ref<AnimationNode>();
	}
	return node;
}

// Preflight every stored value before cloning: in particular, recursive container
// cycles and script setters must never be entered during resource duplication.
struct CloneBounds {
	HashSet<ObjectID> resources;
	HashSet<ObjectID> active_resources;
	HashSet<const void *> containers;
	int items = 0;

	bool check(const Variant &p_value, String &r_message, int p_depth = 0) {
		if (++items > 100000 || p_depth > 96) {
			return _fail(r_message, "Graph resource data exceeds the deep-clone safety limit.");
		}
		if (p_value.get_type() == Variant::OBJECT) {
			Object *object = p_value.get_validated_object();
			if (!object) {
				return true;
			}
			Resource *resource = Object::cast_to<Resource>(object);
			if (!resource || _scripted(resource) || ClassDB::is_parent_class(resource->get_class_name(), SNAME("Script"))) {
				return _fail(r_message, "Graph cloning only supports native, unscripted resources.");
			}
			if (active_resources.has(resource->get_instance_id())) {
				return _fail(r_message, "Cyclic graph subresources cannot be cloned safely.");
			}
			if (resources.has(resource->get_instance_id())) {
				return true;
			}
			if (resources.size() >= 10000) {
				return _fail(r_message, "Too many graph subresources.");
			}
			resources.insert(resource->get_instance_id());
			active_resources.insert(resource->get_instance_id());
			List<PropertyInfo> list;
			resource->get_property_list(&list);
			for (const PropertyInfo &info : list) {
				if ((info.usage & PROPERTY_USAGE_STORAGE) && !check(resource->get(info.name), r_message, p_depth + 1)) {
					return false;
				}
			}
			active_resources.erase(resource->get_instance_id());
		} else if (p_value.get_type() == Variant::ARRAY || p_value.get_type() == Variant::DICTIONARY) {
			const bool is_array = p_value.get_type() == Variant::ARRAY;
			const Array array = is_array ? Array(p_value) : Array();
			const Dictionary dictionary = is_array ? Dictionary() : Dictionary(p_value);
			const void *id = is_array ? array.id() : dictionary.id();
			if (containers.has(id)) {
				return _fail(r_message, "Cyclic graph property containers cannot be cloned safely.");
			}
			containers.insert(id);
			if (is_array) {
				for (const Variant &item : array) {
					if (!check(item, r_message, p_depth + 1)) {
						return false;
					}
				}
			} else {
				for (const KeyValue<Variant, Variant> &E : dictionary) {
					if (!check(E.key, r_message, p_depth + 1) || !check(E.value, r_message, p_depth + 1)) {
						return false;
					}
				}
			}
			containers.erase(id);
		} else if (p_value.get_type() == Variant::CALLABLE || p_value.get_type() == Variant::SIGNAL || p_value.get_type() == Variant::RID) {
			return _fail(r_message, "Graph resource data must not contain executable callbacks or server handles.");
		}
		return true;
	}
};

static bool _bounded_graph(const Ref<AnimationNode> &p_node, HashSet<ObjectID> &r_active, int &r_count, String &r_message, int p_depth) {
	if (p_node.is_null()) {
		return true;
	}
	if (++r_count > GodotMCPAnimationGraph::MAX_NODES || p_depth > GodotMCPAnimationGraph::MAX_DEPTH || r_active.has(p_node->get_instance_id())) {
		return _fail(r_message, "Graph hierarchy is cyclic or exceeds 2000 nodes / 32 levels.");
	}
	if (p_node->get_input_count() > MAX_PORTS) {
		return _fail(r_message, "Graph node exceeds 64 input ports.");
	}
	if (_scripted(p_node.ptr())) {
		return _fail(r_message, "Scripted AnimationNodes are not supported by graph transactions.");
	}
	const String type = p_node->get_class();
	if (!_supported_type(type) && type != "AnimationNodeStartState" && type != "AnimationNodeEndState" && type != "AnimationNodeOutput") {
		return _fail(r_message, "Graph preflight requires native supported AnimationNode classes.");
	}
	if (AnimationNodeBlendSpace1D *space = Object::cast_to<AnimationNodeBlendSpace1D>(p_node.ptr())) {
		if (!Math::is_finite(space->get_min_space()) || !Math::is_finite(space->get_max_space()) || space->get_min_space() >= space->get_max_space()) {
			return _fail(r_message, "BlendSpace1D has invalid limits.");
		}
	}
	if (AnimationNodeBlendSpace2D *space = Object::cast_to<AnimationNodeBlendSpace2D>(p_node.ptr())) {
		const Vector2 min = space->get_min_space();
		const Vector2 max = space->get_max_space();
		if (!min.is_finite() || !max.is_finite() || min.x >= max.x || min.y >= max.y || space->get_triangle_count() > MAX_ITEMS) {
			return _fail(r_message, "BlendSpace2D has invalid limits or more than 512 triangles.");
		}
	}
	if (AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(p_node.ptr())) {
		if (machine->get_transition_count() > MAX_ITEMS) {
			return _fail(r_message, "StateMachine exceeds 512 transitions.");
		}
	}
	r_active.insert(p_node->get_instance_id());
	LocalVector<AnimationNode::ChildNode> children;
	_native_children(p_node, children);
	for (const AnimationNode::ChildNode &child : children) {
		if (child.node.is_null()) {
			return _fail(r_message, "Graph hierarchy contains a null child.");
		}
		if (!_bounded_graph(child.node, r_active, r_count, r_message, p_depth + 1)) {
			return false;
		}
	}
	r_active.erase(p_node->get_instance_id());
	return true;
}

static Ref<AnimationNode> _clone_node(const Ref<AnimationNode> &p_source, HashMap<ObjectID, Ref<Resource>> &r_clones, String &r_message);

static Variant _clone_value(const Variant &p_source, HashMap<ObjectID, Ref<Resource>> &r_clones, String &r_message) {
	if (p_source.get_type() == Variant::OBJECT) {
		const Ref<Resource> source = Ref<Resource>(Object::cast_to<Resource>(p_source.get_validated_object()));
		if (source.is_null()) {
			return Variant();
		}
		if (const Ref<Resource> *existing = r_clones.getptr(source->get_instance_id())) {
			return *existing;
		}
		const Ref<AnimationNode> node = source;
		if (node.is_valid()) {
			return _clone_node(node, r_clones, r_message);
		}
		if (!ClassDB::can_instantiate(source->get_class_name())) {
			_fail(r_message, "Cannot instantiate graph subresource: " + source->get_class() + ".");
			return Variant();
		}
		Ref<Resource> copy = Ref<Resource>(Object::cast_to<Resource>(ClassDB::instantiate(source->get_class_name())));
		if (copy.is_null()) {
			_fail(r_message, "Cannot clone graph subresource.");
			return Variant();
		}
		r_clones.insert(source->get_instance_id(), copy);
		List<PropertyInfo> properties;
		source->get_property_list(&properties);
		for (const PropertyInfo &info : properties) {
			if (!(info.usage & PROPERTY_USAGE_STORAGE) || info.name == SNAME("script") || info.name == SNAME("resource_path") || info.name == SNAME("resource_scene_unique_id")) {
				continue;
			}
			const Variant value = _clone_value(source->get(info.name), r_clones, r_message);
			if (!r_message.is_empty()) {
				return Variant();
			}
			bool valid = false;
			copy->set(info.name, value, &valid);
			if (!valid) {
				_fail(r_message, "Cannot clone subresource property: " + String(info.name) + ".");
				return Variant();
			}
		}
		return copy;
	}
	if (p_source.get_type() == Variant::ARRAY) {
		const Array source = p_source;
		Array copy = source.duplicate();
		for (int i = 0; i < source.size(); i++) {
			copy[i] = _clone_value(source[i], r_clones, r_message);
		}
		return copy;
	}
	if (p_source.get_type() == Variant::DICTIONARY) {
		const Dictionary source = p_source;
		Dictionary copy = source.duplicate();
		copy.clear();
		for (const KeyValue<Variant, Variant> &E : source) {
			copy[_clone_value(E.key, r_clones, r_message)] = _clone_value(E.value, r_clones, r_message);
		}
		return copy;
	}
	return p_source.duplicate();
}

static bool _copy_node_properties(const Ref<AnimationNode> &p_source, const Ref<AnimationNode> &p_target, HashMap<ObjectID, Ref<Resource>> &r_clones, String &r_message) {
	List<PropertyInfo> properties;
	ClassDB::get_property_list(p_source->get_class_name(), &properties);
	List<PropertyInfo> dynamic_properties;
	p_source->get_property_list(&dynamic_properties);
	for (const PropertyInfo &info : dynamic_properties) {
		if (!ClassDB::has_property(p_source->get_class_name(), info.name)) {
			properties.push_back(info);
		}
	}
	for (const PropertyInfo &info : properties) {
		const String name = info.name;
		if (!(info.usage & PROPERTY_USAGE_STORAGE) || name == "script" || name == "resource_path" || name == "resource_scene_unique_id" || name.begins_with("nodes/") || name.begins_with("blend_point_") || name.begins_with("input_") || name == "node_connections" || name == "transitions" || name == "triangles" || name == "filters" || name == "filter_enabled" || name == "min_space" || name == "max_space") {
			continue;
		}
		const Variant value = _clone_value(p_source->get(info.name), r_clones, r_message);
		if (!r_message.is_empty()) {
			return false;
		}
		bool valid = false;
		p_target->set(info.name, value, &valid);
		if (!valid) {
			return _fail(r_message, "Cannot clone AnimationNode property: " + String(info.name) + ".");
		}
	}
	if (Object::cast_to<AnimationNodeBlendSpace1D>(p_source.ptr()) || Object::cast_to<AnimationNodeBlendSpace2D>(p_source.ptr())) {
		Dictionary limits;
		limits["min_space"] = p_source->get(SNAME("min_space"));
		limits["max_space"] = p_source->get(SNAME("max_space"));
		if (!_space_limits(p_target.ptr(), limits, r_message)) {
			return false;
		}
	}
	for (const Variant &path : p_source->_get_filters()) {
		p_target->set_filter_path(NodePath(path), true);
	}
	p_target->set_filter_enabled(p_source->is_filter_enabled());
	p_target->set_deletable(p_source->is_deletable());
	AnimationNodeTransition *source = Object::cast_to<AnimationNodeTransition>(p_source.ptr());
	AnimationNodeTransition *target = Object::cast_to<AnimationNodeTransition>(p_target.ptr());
	if (source && target) {
		// input_count is editor-only, so generic Resource::duplicate_deep() cannot
		// reconstruct it before its serialization-only input_N fields are set.
		target->set_input_count(source->get_input_count());
		for (int i = 0; i < source->get_input_count(); i++) {
			target->set_input_name(i, source->get_input_name(i));
			target->set_input_as_auto_advance(i, source->is_input_set_as_auto_advance(i));
			target->set_input_break_loop_at_end(i, source->is_input_loop_broken_at_end(i));
			target->set_input_reset(i, source->is_input_reset(i));
		}
	}
	return true;
}

static Ref<AnimationNode> _clone_node(const Ref<AnimationNode> &p_source, HashMap<ObjectID, Ref<Resource>> &r_clones, String &r_message) {
	if (p_source.is_null()) {
		_fail(r_message, "Cannot clone a null graph node.");
		return Ref<AnimationNode>();
	}
	if (const Ref<Resource> *existing = r_clones.getptr(p_source->get_instance_id())) {
		return *existing;
	}
	const String type = p_source->get_class();
	if (!_supported_type(type) && type != "AnimationNodeStartState" && type != "AnimationNodeEndState" && type != "AnimationNodeOutput") {
		_fail(r_message, "Cannot clone unsupported AnimationNode class: " + type + ".");
		return Ref<AnimationNode>();
	}
	Ref<AnimationNode> target = Ref<AnimationNode>(Object::cast_to<AnimationNode>(ClassDB::instantiate(type)));
	if (target.is_null()) {
		_fail(r_message, "Cannot instantiate AnimationNode class: " + type + ".");
		return target;
	}
	r_clones.insert(p_source->get_instance_id(), target);
	if (AnimationNodeBlendTree *source_blend = Object::cast_to<AnimationNodeBlendTree>(p_source.ptr())) {
		AnimationNodeBlendTree *target_blend = Object::cast_to<AnimationNodeBlendTree>(target.ptr());
		r_clones.insert(source_blend->get_node(SNAME("output"))->get_instance_id(), target_blend->get_node(SNAME("output")));
	} else if (AnimationNodeStateMachine *source_machine = Object::cast_to<AnimationNodeStateMachine>(p_source.ptr())) {
		AnimationNodeStateMachine *target_machine = Object::cast_to<AnimationNodeStateMachine>(target.ptr());
		// Start/End are virtual classes in ClassDB. Reuse the constructor-created
		// nodes and seed the map before properties/metadata can reference them.
		for (const StringName &name : { SNAME("Start"), SNAME("End") }) {
			r_clones.insert(source_machine->get_node(name)->get_instance_id(), target_machine->get_node(name));
		}
	}
	if (!_copy_node_properties(p_source, target, r_clones, r_message)) {
		return Ref<AnimationNode>();
	}
	if (AnimationNodeBlendTree *source_blend = Object::cast_to<AnimationNodeBlendTree>(p_source.ptr())) {
		AnimationNodeBlendTree *copy = Object::cast_to<AnimationNodeBlendTree>(target.ptr());
		for (const StringName &name : source_blend->get_node_list()) {
			Ref<AnimationNode> child;
			if (name == SNAME("output")) {
				child = copy->get_node(name);
				r_clones.insert(source_blend->get_node(name)->get_instance_id(), child);
				if (!_copy_node_properties(source_blend->get_node(name), child, r_clones, r_message)) {
					return Ref<AnimationNode>();
				}
			} else {
				child = _clone_node(source_blend->get_node(name), r_clones, r_message);
				if (child.is_null()) {
					return Ref<AnimationNode>();
				}
				copy->add_node(name, child, source_blend->get_node_position(name));
			}
			copy->set_node_position(name, source_blend->get_node_position(name));
		}
		LocalVector<AnimationNodeBlendTree::NodeConnection> connections;
		source_blend->get_node_connections(&connections);
		for (const AnimationNodeBlendTree::NodeConnection &connection : connections) {
			if (copy->can_connect_node(connection.input_node, connection.input_index, connection.output_node) != AnimationNodeBlendTree::CONNECTION_OK) {
				_fail(r_message, "Original graph contains invalid connection ports.");
				return Ref<AnimationNode>();
			}
			copy->connect_node(connection.input_node, connection.input_index, connection.output_node);
		}
	} else if (AnimationNodeStateMachine *source_machine = Object::cast_to<AnimationNodeStateMachine>(p_source.ptr())) {
		AnimationNodeStateMachine *copy = Object::cast_to<AnimationNodeStateMachine>(target.ptr());
		for (const StringName &name : source_machine->get_node_list()) {
			if (name == SNAME("Start") || name == SNAME("End")) {
				if (!_copy_node_properties(source_machine->get_node(name), copy->get_node(name), r_clones, r_message)) {
					return Ref<AnimationNode>();
				}
				copy->set_node_position(name, source_machine->get_node_position(name));
				continue;
			}
			Ref<AnimationNode> child = _clone_node(source_machine->get_node(name), r_clones, r_message);
			if (child.is_null()) {
				return Ref<AnimationNode>();
			}
			if (copy->has_node(name)) {
				copy->replace_node(name, child);
			} else {
				copy->add_node(name, child);
			}
			copy->set_node_position(name, source_machine->get_node_position(name));
		}
		for (int i = 0; i < source_machine->get_transition_count(); i++) {
			const Ref<AnimationNodeStateMachineTransition> transition = _clone_value(source_machine->get_transition(i), r_clones, r_message);
			if (transition.is_null()) {
				return Ref<AnimationNode>();
			}
			copy->add_transition(source_machine->get_transition_from(i), source_machine->get_transition_to(i), transition);
		}
	} else if (AnimationNodeBlendSpace1D *source_space1 = Object::cast_to<AnimationNodeBlendSpace1D>(p_source.ptr())) {
		AnimationNodeBlendSpace1D *copy = Object::cast_to<AnimationNodeBlendSpace1D>(target.ptr());
		for (int i = 0; i < source_space1->get_blend_point_count(); i++) {
			Ref<AnimationRootNode> child = _clone_node(source_space1->get_blend_point_node(i), r_clones, r_message);
			if (child.is_null()) {
				return Ref<AnimationNode>();
			}
			copy->add_blend_point(child, source_space1->get_blend_point_position(i), -1, source_space1->get_blend_point_name(i));
		}
	} else if (AnimationNodeBlendSpace2D *source_space2 = Object::cast_to<AnimationNodeBlendSpace2D>(p_source.ptr())) {
		AnimationNodeBlendSpace2D *copy = Object::cast_to<AnimationNodeBlendSpace2D>(target.ptr());
		copy->set_auto_triangles(false);
		for (int i = 0; i < source_space2->get_blend_point_count(); i++) {
			Ref<AnimationRootNode> child = _clone_node(source_space2->get_blend_point_node(i), r_clones, r_message);
			if (child.is_null()) {
				return Ref<AnimationNode>();
			}
			copy->add_blend_point(child, source_space2->get_blend_point_position(i), -1, source_space2->get_blend_point_name(i));
		}
		const PackedInt32Array triangles = source_space2->get(SNAME("triangles"));
		for (int i = 0; i + 2 < triangles.size(); i += 3) {
			copy->add_triangle(triangles[i], triangles[i + 1], triangles[i + 2]);
		}
		copy->set_auto_triangles(source_space2->get_auto_triangles());
	}
	return target;
}

static bool _depends_on(AnimationNodeBlendTree *p_graph, const StringName &p_node, const StringName &p_target, HashSet<StringName> &r_seen) {
	if (p_node == p_target) {
		return true;
	}
	if (r_seen.has(p_node) || !p_graph->has_node(p_node)) {
		return false;
	}
	if (r_seen.size() >= GodotMCPAnimationGraph::MAX_NODES) {
		return true;
	}
	r_seen.insert(p_node);
	const LocalVector<StringName> *connections = p_graph->get_node_connection_array(p_node);
	for (const StringName &connection : *connections) {
		if (connection != StringName() && _depends_on(p_graph, connection, p_target, r_seen)) {
			return true;
		}
	}
	return false;
}

struct ParameterSlot {
	String instance_path;
	StringName name;
	Variant default_value;
};

static bool _parameter_value_safe(const Variant &p_value, int p_depth = 0) {
	if (p_depth >= 8 || p_value.get_type() == Variant::OBJECT || p_value.get_type() == Variant::CALLABLE || p_value.get_type() == Variant::SIGNAL || p_value.get_type() == Variant::RID) {
		return false;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		const Array values = p_value;
		if (values.size() > MAX_ITEMS) {
			return false;
		}
		for (const Variant &value : values) {
			if (!_parameter_value_safe(value, p_depth + 1)) {
				return false;
			}
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary values = p_value;
		if (values.size() > MAX_ITEMS) {
			return false;
		}
		for (const KeyValue<Variant, Variant> &E : values) {
			if (!_parameter_value_safe(E.key, p_depth + 1) || !_parameter_value_safe(E.value, p_depth + 1)) {
				return false;
			}
		}
	}
	return true;
}

static void _parameter_slots(const Ref<AnimationNode> &p_node, const String &p_instance_path, Vector<ParameterSlot> &r_slots) {
	if (p_node.is_null()) {
		return;
	}
	LocalVector<PropertyInfo> properties;
	p_node->get_parameter_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (info.type == Variant::OBJECT) {
			continue; // Playback/observer handles must not be captured or replayed.
		}
		ParameterSlot slot;
		slot.instance_path = p_instance_path;
		slot.name = info.name;
		slot.default_value = p_node->get_parameter_default_value(info.name);
		if (_parameter_value_safe(slot.default_value)) {
			r_slots.push_back(slot);
		}
	}
	LocalVector<AnimationNode::ChildNode> children;
	_native_children(p_node, children);
	for (const AnimationNode::ChildNode &child : children) {
		_parameter_slots(child.node, p_instance_path + String(child.name) + "/", r_slots);
	}
}

static Dictionary _snapshot_parameters(AnimationTree *p_tree, const Ref<AnimationRootNode> &p_root) {
	Dictionary snapshot;
	// Controller scripts remain attached as non-executing editor placeholders.
	// Editor-executing scripts and unsafe graph hierarchies are checked before
	// cache initialization. Value reads use public native instance pointers.
	List<PropertyInfo> ignored;
	p_tree->get_property_list(&ignored);
	Vector<ParameterSlot> slots;
	_parameter_slots(p_root, "parameters/", slots);
	for (const ParameterSlot &slot : slots) {
		AnimationNodeInstance *instance = p_tree->get_node_instance_by_path_or_null(slot.instance_path);
		Variant **value = instance ? instance->property_ptrs.getptr(slot.name) : nullptr;
		if (value && _parameter_value_safe(**value)) {
			snapshot[slot.instance_path + String(slot.name)] = (**value).duplicate(true);
		}
	}
	return snapshot;
}

static String _parameter_prefix(const String &p_graph_path, const String &p_name) {
	return "parameters/" + (p_graph_path.is_empty() ? String() : p_graph_path + "/") + p_name + "/";
}

static void _remap_parameters(Dictionary &r_parameters, const Dictionary &p_operation) {
	const String op = p_operation.get("op", "");
	if (op == "set_root") {
		r_parameters.clear();
		return;
	}
	const bool rename = op == "rename_node" || (op == "set_blend_point" && p_operation.has("new_name"));
	const bool prune = op == "remove_node" || op == "remove_blend_point" || op == "add_node" || op == "add_blend_point";
	if (!rename && !prune) {
		return;
	}
	const String graph_path = p_operation.get("graph_path", "");
	const String name = p_operation.get("name", "");
	const String from = _parameter_prefix(graph_path, name);
	const String to = rename ? _parameter_prefix(graph_path, p_operation["new_name"]) : String();
	if (rename && from == to) {
		return;
	}
	const Array keys = r_parameters.keys();
	Dictionary renamed;
	for (const Variant &key : keys) {
		const String path = key;
		if (path.begins_with(from)) {
			if (rename) {
				renamed[to + path.substr(from.length())] = r_parameters[key];
			}
			r_parameters.erase(key);
		} else if (rename && path.begins_with(to)) {
			r_parameters.erase(key);
		}
	}
	r_parameters.merge(renamed, true);
}

// UndoRedo retains this RefCounted callable target and its bound root/snapshot.
// No Object::set or script-dispatched Callable("set_tree_root") is involved.
class GraphCommit : public RefCounted {
	ObjectID tree_id;

public:
	void set_tree(AnimationTree *p_tree) { tree_id = p_tree->get_instance_id(); }

	void commit(const Ref<AnimationRootNode> &p_root, const Dictionary &p_parameters) {
		AnimationTree *tree = Object::cast_to<AnimationTree>(ObjectDB::get_instance(tree_id));
		if (!tree) {
			return;
		}
		HashSet<ObjectID> active;
		int count = 0;
		String message;
		ERR_FAIL_COND_MSG(!_bounded_graph(p_root, active, count, message), message);
		tree->AnimationTree::set_root_animation_node(p_root);
		// An editor-executing script attached AFTER authoring must not be entered
		// on undo/redo. Ordinary controller placeholders retain parameter restore.
		// Root identity is still restored via the native method in either case.
		if (_editor_executing_script(tree)) {
			return;
		}
		List<PropertyInfo> ignored;
		tree->get_property_list(&ignored);
		Vector<ParameterSlot> slots;
		_parameter_slots(p_root, "parameters/", slots);
		for (const ParameterSlot &slot : slots) {
			AnimationNodeInstance *instance = tree->get_node_instance_by_path_or_null(slot.instance_path);
			Variant **target = instance ? instance->property_ptrs.getptr(slot.name) : nullptr;
			if (!target) {
				continue;
			}
			const Variant value = p_parameters.get(slot.instance_path + String(slot.name), slot.default_value);
			// Reset newly added/replaced paths to defaults even when AnimationTree's
			// private property map retained stale values at the same name.
			**target = (value.get_type() == slot.default_value.get_type() && _parameter_value_safe(value) ? value : slot.default_value).duplicate(true);
		}
	}
};

static bool _operation(Ref<AnimationRootNode> &r_root, const Dictionary &p_operation, String &r_message) {
	String op;
	if (!_string(p_operation, "op", op, r_message)) {
		return false;
	}
	if (op == "set_root") {
		Ref<AnimationNode> root = _create(p_operation, true, r_message);
		if (root.is_null()) {
			return false;
		}
		r_root = root;
		return true;
	}
	const bool node_operation = op == "set_node_properties" || op == "set_filter";
	String path;
	if (!_string(p_operation, node_operation ? "node_path" : "graph_path", path, r_message, false) || !_graph_path(path, r_message)) {
		return false;
	}
	Ref<AnimationNode> node = _resolve(r_root, path);
	if (node.is_null()) {
		return _fail(r_message, "Graph path does not exist: " + path + ".");
	}
	if (op == "set_node_properties") {
		if (!p_operation.has("properties")) {
			return _fail(r_message, "set_node_properties requires properties.");
		}
		return _set_properties(node.ptr(), p_operation, r_message);
	}
	if (op == "set_filter") {
		if (!node->has_filter() || p_operation.get("enabled", Variant()).get_type() != Variant::BOOL || p_operation.get("paths", Variant()).get_type() != Variant::ARRAY) {
			return _fail(r_message, "set_filter requires a filter-capable node, boolean enabled and paths array.");
		}
		const Array paths = p_operation["paths"];
		if (paths.size() > MAX_ITEMS) {
			return _fail(r_message, "Filter paths exceed 512 entries.");
		}
		for (const Variant &value : paths) {
			if (!_is_string(value) || String(value).is_empty() || String(value).length() > MAX_STRING || NodePath(value).is_absolute()) {
				return _fail(r_message, "Filter paths must be non-empty, relative track paths.");
			}
		}
		const Array previous = node->_get_filters();
		for (const Variant &value : previous) {
			node->set_filter_path(value, false);
		}
		for (const Variant &value : paths) {
			node->set_filter_path(NodePath(value), true);
		}
		node->set_filter_enabled(p_operation["enabled"]);
		return true;
	}
	AnimationNodeBlendTree *blend = Object::cast_to<AnimationNodeBlendTree>(node.ptr());
	AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(node.ptr());
	AnimationNodeBlendSpace1D *space1 = Object::cast_to<AnimationNodeBlendSpace1D>(node.ptr());
	AnimationNodeBlendSpace2D *space2 = Object::cast_to<AnimationNodeBlendSpace2D>(node.ptr());
	if (op == "add_node" || op == "remove_node" || op == "rename_node" || op == "move_node") {
		String name;
		if ((!blend && !machine) || !_string(p_operation, "name", name, r_message) || !_name(name, r_message)) {
			return _fail(r_message, "Node operations require a BlendTree/StateMachine and a valid name.");
		}
		const bool exists = blend ? blend->has_node(name) : machine->has_node(name);
		if ((op == "add_node") == exists) {
			return _fail(r_message, op == "add_node" ? String("Node name already exists.") : String("Node name does not exist."));
		}
		if ((blend && name == "output" && op != "move_node") || (machine && (name == "Start" || name == "End") && op != "move_node")) {
			return _fail(r_message, "Built-in output/Start/End nodes cannot be added, removed or renamed.");
		}
		Vector2 position;
		if ((op == "move_node" || p_operation.has("position")) && !_vector2(p_operation.get("position", Variant()), position)) {
			return _fail(r_message, "position must have two finite coordinates.");
		}
		if (op == "add_node") {
			Ref<AnimationNode> child = _create(p_operation, machine != nullptr, r_message);
			if (child.is_null()) {
				return false;
			}
			if (blend) {
				blend->add_node(name, child, position);
			} else {
				machine->add_node(name, child, position);
			}
		} else if (op == "remove_node") {
			if (blend) {
				blend->remove_node(name);
			} else {
				machine->remove_node(name);
			}
		} else if (op == "rename_node") {
			String new_name;
			if (!_string(p_operation, "new_name", new_name, r_message) || !_name(new_name, r_message) || (blend ? blend->has_node(new_name) : machine->has_node(new_name))) {
				return _fail(r_message, "new_name must be valid and unused.");
			}
			if (blend) {
				blend->rename_node(name, new_name);
			} else {
				machine->rename_node(name, new_name);
			}
		} else if (blend) {
			blend->set_node_position(name, position);
		} else {
			machine->set_node_position(name, position);
		}
		return true;
	}
	if (op == "connect" || op == "disconnect") {
		String to;
		int input;
		if (!blend || !_string(p_operation, "to", to, r_message) || !blend->has_node(to) || !_integer(p_operation.get("input", Variant()), 0, MAX_PORTS - 1, input) || input >= blend->get_node(to)->get_input_count()) {
			return _fail(r_message, "Connection requires a BlendTree, existing target and valid integer input port.");
		}
		if (op == "disconnect") {
			blend->disconnect_node(to, input);
			return true;
		}
		String from;
		if (!_string(p_operation, "from", from, r_message) || blend->can_connect_node(to, input, from) != AnimationNodeBlendTree::CONNECTION_OK) {
			return _fail(r_message, "Invalid connection: source/target missing, occupied port, reused output, or same node.");
		}
		HashSet<StringName> seen;
		if (_depends_on(blend, from, to, seen)) {
			return _fail(r_message, "Connection would create a blend graph cycle.");
		}
		blend->connect_node(to, input, from);
		return true;
	}
	if (op == "add_transition" || op == "set_transition" || op == "remove_transition") {
		String from, to;
		if (!machine || !_string(p_operation, "from", from, r_message) || !_string(p_operation, "to", to, r_message) || !machine->has_node(from) || !machine->has_node(to) || from == to || from == "End" || to == "Start") {
			return _fail(r_message, "Transitions require existing distinct StateMachine states (not from End or to Start).");
		}
		const int index = machine->find_transition(from, to);
		if ((op == "add_transition") == (index >= 0)) {
			return _fail(r_message, op == "add_transition" ? String("Transition already exists.") : String("Transition does not exist."));
		}
		if (op == "remove_transition") {
			machine->remove_transition(from, to);
			return true;
		}
		Ref<AnimationNodeStateMachineTransition> transition;
		if (op == "add_transition") {
			if (machine->get_transition_count() >= MAX_ITEMS) {
				return _fail(r_message, "StateMachine exceeds 512 transitions.");
			}
			transition.instantiate();
		} else {
			transition = machine->get_transition(index);
		}
		if (!_set_properties(transition.ptr(), p_operation, r_message)) {
			return false;
		}
		if (op == "add_transition") {
			machine->add_transition(from, to, transition);
		}
		return true;
	}
	if (op == "add_blend_point" || op == "set_blend_point" || op == "remove_blend_point") {
		String name;
		if ((!space1 && !space2) || !_string(p_operation, "name", name, r_message) || !_name(name, r_message)) {
			return _fail(r_message, "Blend point operations require a BlendSpace1D/2D and a valid name.");
		}
		const int count = space1 ? space1->get_blend_point_count() : space2->get_blend_point_count();
		const int index = space1 ? space1->find_blend_point_by_name(name) : space2->find_blend_point_by_name(name);
		if ((op == "add_blend_point") == (index >= 0)) {
			return _fail(r_message, op == "add_blend_point" ? String("Blend point name already exists.") : String("Blend point name does not exist."));
		}
		if (op == "remove_blend_point") {
			if (space1) {
				space1->remove_blend_point(index);
			} else {
				space2->remove_blend_point(index);
			}
			return true;
		}
		double position1 = 0;
		Vector2 position2;
		const bool has_position = p_operation.has("position");
		if ((op == "add_blend_point" || has_position) && !(space1 ? _number(p_operation.get("position", Variant()), position1) : _vector2(p_operation.get("position", Variant()), position2))) {
			return _fail(r_message, "Blend point position must be a finite number (1D) or two coordinates (2D).");
		}
		if (op == "add_blend_point") {
			int at = -1;
			if (count >= 64 || (p_operation.has("at_index") && !_integer(p_operation["at_index"], -1, count, at))) {
				return _fail(r_message, "Blend spaces allow 64 points; at_index must be -1 or 0..point_count.");
			}
			Ref<AnimationRootNode> child = _create(p_operation, true, r_message, "AnimationNodeAnimation");
			if (child.is_null()) {
				return false;
			}
			if (space1) {
				space1->add_blend_point(child, position1, at, name);
			} else {
				space2->add_blend_point(child, position2, at, name);
			}
		} else {
			if (p_operation.has("new_name")) {
				String new_name;
				if (!_string(p_operation, "new_name", new_name, r_message) || !_name(new_name, r_message)) {
					return false;
				}
				const int existing = space1 ? space1->find_blend_point_by_name(new_name) : space2->find_blend_point_by_name(new_name);
				if (existing >= 0 && existing != index) {
					return _fail(r_message, "Blend point new_name already exists.");
				}
				if (space1) {
					space1->set_blend_point_name(index, new_name);
				} else {
					space2->set_blend_point_name(index, new_name);
				}
			}
			if (has_position) {
				if (space1) {
					space1->set_blend_point_position(index, position1);
				} else {
					space2->set_blend_point_position(index, position2);
				}
			}
			Ref<AnimationRootNode> child = space1 ? space1->get_blend_point_node(index) : space2->get_blend_point_node(index);
			if (!_set_properties(child.ptr(), p_operation, r_message)) {
				return false;
			}
		}
		return true;
	}
	if (op == "set_triangles") {
		if (!space2 || p_operation.get("triangles", Variant()).get_type() != Variant::ARRAY) {
			return _fail(r_message, "set_triangles requires a BlendSpace2D and an array of index triples.");
		}
		const Array triangles = p_operation["triangles"];
		if (triangles.size() > MAX_ITEMS) {
			return _fail(r_message, "Triangles exceed 512 entries.");
		}
		HashSet<String> unique;
		for (const Variant &value : triangles) {
			if (value.get_type() != Variant::ARRAY || Array(value).size() != 3) {
				return _fail(r_message, "Each triangle must contain exactly three point indices.");
			}
			const Array triangle = value;
			int indices[3];
			for (int i = 0; i < 3; i++) {
				if (!_integer(triangle[i], 0, space2->get_blend_point_count() - 1, indices[i])) {
					return _fail(r_message, "Triangle point index is invalid.");
				}
			}
			if (indices[0] == indices[1] || indices[1] == indices[2] || indices[0] == indices[2]) {
				return _fail(r_message, "Triangle indices must be distinct.");
			}
			const Vector2 a = space2->get_blend_point_position(indices[0]);
			const Vector2 b = space2->get_blend_point_position(indices[1]);
			const Vector2 c = space2->get_blend_point_position(indices[2]);
			if (Math::is_zero_approx((b - a).cross(c - a))) {
				return _fail(r_message, "Triangle points must not be collinear.");
			}
			SortArray<int>().sort(indices, 3);
			const String key = itos(indices[0]) + ":" + itos(indices[1]) + ":" + itos(indices[2]);
			if (unique.has(key)) {
				return _fail(r_message, "Duplicate triangle.");
			}
			unique.insert(key);
		}
		// Manual triangles replace auto-generated topology, explicitly and safely.
		space2->set_auto_triangles(false);
		while (space2->get_triangle_count()) {
			space2->remove_triangle(space2->get_triangle_count() - 1);
		}
		for (const Variant &value : triangles) {
			const Array triangle = value;
			space2->add_triangle(int(triangle[0]), int(triangle[1]), int(triangle[2]));
		}
		return true;
	}
	return _fail(r_message, "Unknown graph operation: " + op + ".");
}

static PackedInt32Array _triangle_indices(AnimationNodeBlendSpace2D *p_space, bool &r_truncated) {
	if (!p_space->get_auto_triangles()) {
		return p_space->get(SNAME("triangles"));
	}
	// Match the engine's deferred triangulation on copied positions. Never call
	// get_triangle_point()/get_closest_point(), which would mutate the live graph.
	Vector<Vector2> positions;
	for (int i = 0; i < p_space->get_blend_point_count(); i++) {
		const Vector2 position = p_space->get_blend_point_position(i);
		if (!position.is_finite()) {
			r_truncated = true;
			return PackedInt32Array();
		}
		positions.push_back(position);
	}
	PackedInt32Array indices;
	for (const Delaunay2D::Triangle &triangle : Delaunay2D::triangulate(positions)) {
		int points[3] = { triangle.points[0], triangle.points[1], triangle.points[2] };
		SortArray<int>().sort(points, 3);
		for (int point : points) {
			indices.push_back(point);
		}
	}
	return indices;
}

struct GraphReader {
	int max_nodes = 256;
	int max_depth = 12;
	bool include_properties = true;
	bool truncated = false;
	int count = 0;
	HashSet<ObjectID> active;

	bool options(const Dictionary &p_params, String &r_message) {
		if (!_integer(p_params.get("max_nodes", 256), 1, GodotMCPAnimationGraph::MAX_NODES, max_nodes) || !_integer(p_params.get("max_depth", 12), 1, GodotMCPAnimationGraph::MAX_DEPTH, max_depth) || p_params.get("include_properties", true).get_type() != Variant::BOOL) {
			return _fail(r_message, "max_nodes must be 1..2000, max_depth 1..32, include_properties boolean.");
		}
		include_properties = p_params.get("include_properties", true);
		return true;
	}

	Dictionary node(const Ref<AnimationNode> &p_node, const String &p_path, int p_depth = 0) {
		Dictionary result;
		if (p_node.is_null()) {
			return result;
		}
		result["path"] = p_path.left(MAX_STRING);
		result["class"] = p_node->get_class();
		if (p_depth > max_depth || count >= max_nodes || active.has(p_node->get_instance_id())) {
			truncated = true;
			result["truncated"] = true;
			return result;
		}
		count++;
		truncated = truncated || p_path.length() > MAX_STRING;
		active.insert(p_node->get_instance_id());
		if (_scripted(p_node.ptr())) {
			result["unsupported_scripted_node"] = true;
			truncated = true;
			active.erase(p_node->get_instance_id());
			return result;
		}
		if (include_properties) {
			result["properties"] = _properties(p_node.ptr(), truncated);
		}
		Array ports;
		for (int i = 0; i < MIN(p_node->get_input_count(), MAX_PORTS); i++) {
			Dictionary port;
			port["index"] = i;
			port["name"] = p_node->get_input_name(i).left(128);
			ports.push_back(port);
		}
		result["inputs"] = ports;
		result["input_count"] = p_node->get_input_count();
		truncated = truncated || p_node->get_input_count() > MAX_PORTS;
		result["has_filter"] = p_node->has_filter();
		Dictionary filter;
		filter["enabled"] = p_node->is_filter_enabled();
		filter["paths"] = _encode(p_node->_get_filters(), truncated);
		result["filter"] = filter;
		if (AnimationNodeAnimation *clip = Object::cast_to<AnimationNodeAnimation>(p_node.ptr())) {
			result["animation"] = String(clip->get_animation()).left(MAX_STRING);
		}
		AnimationNodeBlendTree *blend = Object::cast_to<AnimationNodeBlendTree>(p_node.ptr());
		AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(p_node.ptr());
		AnimationNodeBlendSpace1D *space1 = Object::cast_to<AnimationNodeBlendSpace1D>(p_node.ptr());
		AnimationNodeBlendSpace2D *space2 = Object::cast_to<AnimationNodeBlendSpace2D>(p_node.ptr());
		if (blend) {
			Array connections;
			LocalVector<AnimationNodeBlendTree::NodeConnection> list;
			blend->get_node_connections(&list);
			for (const AnimationNodeBlendTree::NodeConnection &connection : list) {
				if (connections.size() >= MAX_ITEMS) {
					truncated = true;
					break;
				}
				Dictionary entry;
				entry["from"] = String(connection.output_node).left(128);
				entry["to"] = String(connection.input_node).left(128);
				entry["input"] = connection.input_index;
				connections.push_back(entry);
			}
			result["connections"] = connections;
		}
		if (machine) {
			Array transitions;
			for (int i = 0; i < MIN(machine->get_transition_count(), MAX_ITEMS); i++) {
				Dictionary entry;
				entry["from"] = String(machine->get_transition_from(i)).left(128);
				entry["to"] = String(machine->get_transition_to(i)).left(128);
				if (include_properties && !_scripted(machine->get_transition(i).ptr())) {
					entry["properties"] = _properties(machine->get_transition(i).ptr(), truncated);
				}
				transitions.push_back(entry);
			}
			truncated = truncated || machine->get_transition_count() > MAX_ITEMS;
			result["transitions"] = transitions;
		}
		if (space1 || space2) {
			Array points;
			const int point_count = space1 ? space1->get_blend_point_count() : space2->get_blend_point_count();
			for (int i = 0; i < point_count; i++) {
				Dictionary point;
				const String name = space1 ? space1->get_blend_point_name(i) : space2->get_blend_point_name(i);
				point["index"] = i;
				point["name"] = name.left(128);
				point["path"] = (p_path.is_empty() ? name : p_path + "/" + name).left(MAX_STRING);
				point["position"] = space1 ? _encode(space1->get_blend_point_position(i), truncated) : Variant(_vector(space2->get_blend_point_position(i)));
				points.push_back(point);
			}
			result["blend_points"] = points;
		}
		if (space2) {
			const PackedInt32Array indices = _triangle_indices(space2, truncated);
			Array triangles;
			for (int i = 0; i + 2 < indices.size() && triangles.size() < MAX_ITEMS; i += 3) {
				Array triangle;
				for (int j = 0; j < 3; j++) {
					triangle.push_back(indices[i + j]);
				}
				triangles.push_back(triangle);
			}
			truncated = truncated || indices.size() > MAX_ITEMS * 3;
			result["triangles"] = triangles;
			result["auto_triangles"] = space2->get_auto_triangles();
			result["triangles_computed"] = space2->get_auto_triangles();
		}
		LocalVector<AnimationNode::ChildNode> children;
		_native_children(p_node, children);
		Vector<String> names;
		for (const AnimationNode::ChildNode &child : children) {
			names.push_back(child.name);
		}
		names.sort();
		Array nodes;
		for (const String &name : names) {
			if (count >= max_nodes || p_depth >= max_depth) {
				truncated = true;
				break;
			}
			Dictionary child = node(p_node->get_child_by_name(name), p_path.is_empty() ? name : p_path + "/" + name, p_depth + 1);
			child["name"] = name.left(128);
			if (blend) {
				child["position"] = _vector(blend->get_node_position(name));
			} else if (machine) {
				child["position"] = _vector(machine->get_node_position(name));
			}
			nodes.push_back(child);
		}
		result["children"] = nodes;
		active.erase(p_node->get_instance_id());
		return result;
	}
};

static Dictionary _base(AnimationTree *p_tree, const GodotMCPScenePatch::Context &p_context) {
	Dictionary result;
	result["scene_path"] = p_context.scene_path;
	result["scene_version"] = p_context.scene_version;
	result["node_path"] = String(p_context.root->get_path_to(p_tree));
	result["animation_player"] = String(p_tree->get_animation_player());
	return result;
}

struct GraphValidator {
	Array errors;
	Array warnings;
	HashSet<ObjectID> active;
	HashSet<StringName> checked_clips;
	int count = 0;
	int tracks = 0;
	bool truncated = false;
	bool partial = false;
	AnimationTree *tree = nullptr;
	AnimationMixer *mixer = nullptr;
	Node *scene_root = nullptr;
	Node *track_root = nullptr;

	void issue(bool p_error, const String &p_code, const String &p_path, const String &p_message) {
		partial = partial || p_code.begins_with("partial_");
		if (errors.size() + warnings.size() >= MAX_ITEMS) {
			truncated = true;
			return;
		}
		Dictionary issue;
		issue["code"] = p_code;
		issue["path"] = p_path.left(MAX_STRING);
		issue["message"] = p_message.left(MAX_STRING);
		(p_error ? errors : warnings).push_back(issue);
	}

	void clip(AnimationNodeAnimation *p_clip, const String &p_path) {
		const StringName name = p_clip->get_animation();
		if (name == StringName() || !mixer || !mixer->has_animation(name)) {
			issue(true, "missing_clip", p_path, "Animation clip is unset or unavailable: " + String(name));
			return;
		}
		if (checked_clips.has(name) || !track_root) {
			return;
		}
		checked_clips.insert(name);
		const Ref<Animation> animation = mixer->get_animation(name);
		for (int i = 0; i < animation->get_track_count(); i++) {
			if (++tracks > 10000) {
				truncated = true;
				break;
			}
			const NodePath target = animation->track_get_path(i);
			Node *node = target.get_name_count() == 0 ? track_root : track_root->get_node_or_null(NodePath(target.get_concatenated_names()));
			if (!node) {
				issue(false, "missing_track_target", p_path, "Clip " + String(name) + " track target is missing: " + String(target));
			} else if (node != scene_root && !scene_root->is_ancestor_of(node)) {
				issue(false, "external_track_target", p_path, "Clip track targets a node outside the edited scene: " + String(target));
			} else if (animation->track_get_type(i) == Animation::TYPE_VALUE && target.get_subname_count() == 1) {
				PropertyInfo info;
				// Existence is metadata, not a value read. Never invoke _get, exported
				// getters, or a scripted/dynamic get_property_list during validation.
				if (!ClassDB::get_property_info(node->get_class_name(), target.get_subname(0), &info)) {
					issue(false, "partial_track_property_validation", p_path, "Custom/dynamic property existence was not inspected: " + String(target));
				}
			} else if (animation->track_get_type(i) == Animation::TYPE_VALUE && target.get_subname_count() > 1) {
				issue(false, "partial_track_property_validation", p_path, "Nested resource/property targets were not inspected: " + String(target));
			}
		}
	}

	void node(const Ref<AnimationNode> &p_node, const String &p_path, int p_depth = 0) {
		if (p_node.is_null()) {
			issue(true, "missing_node", p_path, "Animation graph node is null.");
			return;
		}
		if (++count > GodotMCPAnimationGraph::MAX_NODES || p_depth > GodotMCPAnimationGraph::MAX_DEPTH) {
			truncated = true;
			issue(true, "graph_limit", p_path, "Graph exceeds the validation node/depth limit.");
			return;
		}
		if (active.has(p_node->get_instance_id())) {
			issue(true, "hierarchy_cycle", p_path, "Graph resource hierarchy is cyclic.");
			return;
		}
		active.insert(p_node->get_instance_id());
		if (_scripted(p_node.ptr())) {
			issue(true, "unsupported_scripted_node", p_path, "Scripted graph nodes cannot be safely validated without executing user code.");
			active.erase(p_node->get_instance_id());
			return;
		}
		List<PropertyInfo> properties;
		p_node->get_property_list(&properties);
		for (const PropertyInfo &info : properties) {
			if (!(info.usage & PROPERTY_USAGE_STORAGE) || _structural_property(info.name)) {
				continue;
			}
			const Variant value = p_node->get(info.name);
			if ((value.get_type() == Variant::FLOAT && !Math::is_finite(double(value))) || (value.get_type() == Variant::VECTOR2 && !Vector2(value).is_finite())) {
				issue(true, "non_finite_property", p_path, "Non-finite graph setting: " + String(info.name));
			}
		}
		if (AnimationNodeAnimation *animation = Object::cast_to<AnimationNodeAnimation>(p_node.ptr())) {
			clip(animation, p_path);
		}
		if (AnimationNodeTransition *transition = Object::cast_to<AnimationNodeTransition>(p_node.ptr())) {
			HashSet<String> names;
			if (transition->get_input_count() == 0) {
				issue(true, "empty_transition", p_path, "AnimationNodeTransition has no input ports.");
			}
			for (int i = 0; i < MIN(transition->get_input_count(), MAX_PORTS); i++) {
				const String name = transition->get_input_name(i);
				if (names.has(name)) {
					issue(true, "duplicate_input_name", p_path, "Transition input names must be unique.");
				}
				names.insert(name);
			}
		}
		if (AnimationNodeBlendTree *blend = Object::cast_to<AnimationNodeBlendTree>(p_node.ptr())) {
			int checked_nodes = 0;
			for (const StringName &name : blend->get_node_list()) {
				if (++checked_nodes > GodotMCPAnimationGraph::MAX_NODES) {
					truncated = true;
					break;
				}
				const Ref<AnimationNode> child = blend->get_node(name);
				const LocalVector<StringName> *connections = blend->get_node_connection_array(name);
				if (!blend->get_node_position(name).is_finite()) {
					issue(true, "non_finite_position", p_path, "Non-finite graph node position: " + String(name));
				}
				if (child.is_null() || int(connections->size()) != child->get_input_count() || child->get_input_count() > MAX_PORTS) {
					issue(true, "invalid_ports", p_path, "Connection slots do not match input ports: " + String(name));
					continue;
				}
				for (uint32_t i = 0; i < connections->size(); i++) {
					const StringName source = (*connections)[i];
					if (source == StringName()) {
						issue(true, "missing_input", p_path, "Unconnected input " + itos(i) + " on " + String(name));
					} else if (!blend->has_node(source) || source == SNAME("output")) {
						issue(true, "invalid_connection", p_path, "Connection source does not exist or is output.");
					} else {
						HashSet<StringName> seen;
						if (_depends_on(blend, source, name, seen)) {
							issue(true, "blend_cycle", p_path, "Blend graph contains a cycle at " + String(name));
						}
					}
				}
			}
		}
		if (AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(p_node.ptr())) {
			const bool grouped = machine->get_state_machine_type() == AnimationNodeStateMachine::STATE_MACHINE_TYPE_GROUPED;
			if (grouped) {
				issue(false, "partial_grouped_reachability", p_path, "Grouped Start/End boundaries use parent transition settings; parent conditions were not modeled.");
			}
			HashSet<StringName> reachable;
			reachable.insert(SNAME("Start"));
			bool changed = true;
			while (changed) {
				changed = false;
				for (int i = 0; i < MIN(machine->get_transition_count(), MAX_ITEMS); i++) {
					const StringName from = machine->get_transition_from(i);
					const StringName to = machine->get_transition_to(i);
					const Ref<AnimationNodeStateMachineTransition> transition = machine->get_transition(i);
					const bool parent_boundary = grouped && (from == SNAME("Start") || to == SNAME("End"));
					if (transition.is_valid() && reachable.has(from) && !reachable.has(to) && (parent_boundary || transition->get_advance_mode() != AnimationNodeStateMachineTransition::ADVANCE_MODE_DISABLED)) {
						reachable.insert(to);
						changed = true;
					}
				}
			}
			for (int i = 0; i < MIN(machine->get_transition_count(), MAX_ITEMS); i++) {
				const Ref<AnimationNodeStateMachineTransition> transition = machine->get_transition(i);
				const StringName from = machine->get_transition_from(i);
				const StringName to = machine->get_transition_to(i);
				if (transition.is_null() || _scripted(transition.ptr())) {
					issue(true, "invalid_transition_resource", p_path, "Transition is null or scripted.");
					continue;
				}
				if (!machine->has_node(from) || !machine->has_node(to) || from == to || from == SNAME("End") || to == SNAME("Start")) {
					issue(true, "invalid_transition", p_path, "Invalid state transition endpoints.");
				}
				if (!Math::is_finite(transition->get_xfade_time()) || transition->get_xfade_time() < 0 || transition->get_switch_mode() < 0 || transition->get_switch_mode() > 2 || transition->get_advance_mode() < 0 || transition->get_advance_mode() > 2 || transition->get_priority() < 0) {
					issue(true, "invalid_transition_settings", p_path, "Invalid transition crossfade, priority, switch or advance mode.");
				}
				const String expression = transition->get_advance_expression().strip_edges();
				if (!expression.is_empty()) {
					Ref<Expression> parser;
					parser.instantiate();
					if (parser->parse(expression) != OK) {
						issue(true, "invalid_expression", p_path, parser->get_error_text());
					}
				}
			}
			truncated = truncated || machine->get_transition_count() > MAX_ITEMS;
			for (const StringName &name : machine->get_node_list()) {
				if (name != SNAME("Start") && name != SNAME("End") && !reachable.has(name)) {
					issue(false, "unreachable_state", p_path, "State is not reachable from Start via enabled transitions: " + String(name));
				}
			}
		}
		if (AnimationNodeBlendSpace1D *space = Object::cast_to<AnimationNodeBlendSpace1D>(p_node.ptr())) {
			if (space->get_blend_point_count() == 0) {
				issue(true, "empty_blend_space", p_path, "BlendSpace1D has no blend points.");
			}
			HashSet<StringName> names;
			for (int i = 0; i < space->get_blend_point_count(); i++) {
				const StringName name = space->get_blend_point_name(i);
				if (names.has(name)) {
					issue(true, "duplicate_blend_point", p_path, "Blend point names must be unique.");
				}
				names.insert(name);
				if (!Math::is_finite(space->get_blend_point_position(i))) {
					issue(true, "non_finite_position", p_path, "Non-finite 1D blend point position.");
				}
				if (space->get_sync_mode() >= AnimationNodeBlendSpace1D::SYNC_MODE_CYCLIC_MUTABLE && !Object::cast_to<AnimationNodeAnimation>(space->get_blend_point_node(i).ptr())) {
					issue(true, "invalid_cyclic_sync", p_path, "Cyclic sync requires non-nested AnimationNodeAnimation points.");
				}
			}
			if (space->get_sync_mode() == AnimationNodeBlendSpace1D::SYNC_MODE_CYCLIC_CONSTANT && space->get_cyclic_length() <= 0) {
				issue(true, "invalid_cyclic_length", p_path, "Constant cyclic sync requires positive cyclic_length.");
			}
		}
		if (AnimationNodeBlendSpace2D *space = Object::cast_to<AnimationNodeBlendSpace2D>(p_node.ptr())) {
			if (space->get_blend_point_count() == 0 || (!space->get_auto_triangles() && space->get_triangle_count() == 0 && space->get_blend_mode() == AnimationNodeBlendSpace2D::BLEND_MODE_INTERPOLATED)) {
				issue(true, "empty_blend_space", p_path, "BlendSpace2D has no points or manual interpolation triangles.");
			}
			HashSet<StringName> names;
			for (int i = 0; i < space->get_blend_point_count(); i++) {
				const StringName name = space->get_blend_point_name(i);
				if (names.has(name)) {
					issue(true, "duplicate_blend_point", p_path, "Blend point names must be unique.");
				}
				names.insert(name);
				if (!space->get_blend_point_position(i).is_finite()) {
					issue(true, "non_finite_position", p_path, "Non-finite 2D blend point position.");
				}
				if (space->get_sync_mode() >= AnimationNodeBlendSpace2D::SYNC_MODE_CYCLIC_MUTABLE && !Object::cast_to<AnimationNodeAnimation>(space->get_blend_point_node(i).ptr())) {
					issue(true, "invalid_cyclic_sync", p_path, "Cyclic sync requires non-nested AnimationNodeAnimation points.");
				}
			}
			if (space->get_sync_mode() == AnimationNodeBlendSpace2D::SYNC_MODE_CYCLIC_CONSTANT && space->get_cyclic_length() <= 0) {
				issue(true, "invalid_cyclic_length", p_path, "Constant cyclic sync requires positive cyclic_length.");
			}
			const PackedInt32Array indices = _triangle_indices(space, truncated);
			for (int i = 0; i + 2 < MIN(indices.size(), MAX_ITEMS * 3); i += 3) {
				const int a = indices[i];
				const int b = indices[i + 1];
				const int c = indices[i + 2];
				if (a < 0 || b < 0 || c < 0 || a >= space->get_blend_point_count() || b >= space->get_blend_point_count() || c >= space->get_blend_point_count() || a == b || b == c || a == c) {
					issue(true, "invalid_triangle", p_path, "Triangle point indices are invalid.");
				} else if (Math::is_zero_approx((space->get_blend_point_position(b) - space->get_blend_point_position(a)).cross(space->get_blend_point_position(c) - space->get_blend_point_position(a)))) {
					issue(true, "degenerate_triangle", p_path, "Triangle blend points are collinear.");
				}
			}
			truncated = truncated || indices.size() > MAX_ITEMS * 3;
			if (space->get_auto_triangles() && space->get_blend_point_count() < 3 && space->get_blend_mode() == AnimationNodeBlendSpace2D::BLEND_MODE_INTERPOLATED) {
				issue(true, "insufficient_blend_points", p_path, "Auto-triangulated interpolation requires at least three blend points.");
			} else if (space->get_auto_triangles() && indices.is_empty() && space->get_blend_mode() == AnimationNodeBlendSpace2D::BLEND_MODE_INTERPOLATED) {
				issue(true, "empty_auto_triangles", p_path, "Automatic triangulation produced no usable triangles.");
			}
		}
		LocalVector<AnimationNode::ChildNode> children;
		_native_children(p_node, children);
		for (const AnimationNode::ChildNode &child : children) {
			if (count >= GodotMCPAnimationGraph::MAX_NODES) {
				truncated = true;
				break;
			}
			node(child.node, p_path.is_empty() ? String(child.name) : p_path + "/" + String(child.name), p_depth + 1);
		}
		active.erase(p_node->get_instance_id());
	}

	Dictionary run(AnimationTree *p_tree, const GodotMCPScenePatch::Context &p_context) {
		tree = p_tree;
		scene_root = p_context.root;
		mixer = tree;
		const NodePath player_path = tree->get_animation_player();
		if (player_path.is_empty()) {
			issue(false, "missing_player_binding", "", "AnimationTree has no AnimationPlayer binding; its own animation libraries are used.");
		} else {
			AnimationPlayer *player = Object::cast_to<AnimationPlayer>(tree->get_node_or_null(player_path));
			if (!player || (player != scene_root && !scene_root->is_ancestor_of(player))) {
				issue(true, "invalid_player_binding", "", "AnimationPlayer binding is missing, not an AnimationPlayer, or outside the scene.");
			} else {
				mixer = player;
			}
		}
		track_root = mixer->get_node_or_null(mixer->get_root_node());
		if (!track_root) {
			issue(false, "missing_track_root", "", "Animation root_node cannot be resolved.");
		} else if (track_root != scene_root && !scene_root->is_ancestor_of(track_root)) {
			issue(false, "external_track_root", "", "Animation root_node points outside the edited scene; track targets were not inspected.");
			track_root = nullptr;
		}
		node(tree->get_root_animation_node(), "");
		Dictionary result = _base(tree, p_context);
		result["valid"] = errors.is_empty() && !truncated;
		result["errors"] = errors;
		result["warnings"] = warnings;
		result["truncated"] = truncated;
		result["partial"] = partial;
		result["node_count"] = count;
		result["checked_track_count"] = tracks;
		return result;
	}
};

} // namespace

bool GodotMCPAnimationGraph::preflight_scene(Node *p_root, String &r_message, bool p_reject_editor_scripts) {
	r_message.clear();
	if (!p_root) {
		return _fail(r_message, "No scene root is available for graph preflight.");
	}
	Vector<Node *> pending;
	pending.push_back(p_root);
	int nodes = 0;
	while (!pending.is_empty()) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (++nodes > 10000) {
			return _fail(r_message, "Graph preflight scene exceeds 10000 nodes.");
		}
		if (AnimationTree *tree = Object::cast_to<AnimationTree>(node)) {
			if (p_reject_editor_scripts && _editor_executing_script(tree)) {
				return _fail(r_message, "Graph authoring cannot enumerate an editor-executing AnimationTree script (@tool or a live script instance). AnimationTree: " + String(p_root->get_path_to(tree)).left(MAX_STRING));
			}
			HashSet<ObjectID> active;
			int count = 0;
			if (!_bounded_graph(tree->get_root_animation_node(), active, count, r_message)) {
				r_message += " AnimationTree: " + String(p_root->get_path_to(tree)).left(MAX_STRING);
				return false;
			}
		}
		const int children = node->get_child_count(true);
		if (nodes + pending.size() + children > 10000) {
			return _fail(r_message, "Graph preflight scene exceeds 10000 nodes.");
		}
		for (int i = 0; i < children; i++) {
			pending.push_back(node->get_child(i, true));
		}
	}
	return true;
}

Dictionary GodotMCPAnimationGraph::inspect(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context, int &r_error_code, String &r_error_message) const {
	AnimationTree *tree = _target(p_params, p_context, false, r_error_code, r_error_message);
	if (!tree) {
		return Dictionary();
	}
	GraphReader reader;
	if (!reader.options(p_params, r_error_message)) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		return Dictionary();
	}
	Dictionary result = _base(tree, p_context);
	result["root"] = tree->get_root_animation_node().is_valid() ? Variant(reader.node(tree->get_root_animation_node(), "")) : Variant();
	Dictionary parameters;
	List<PropertyInfo> properties;
	HashSet<ObjectID> active;
	int count = 0;
	String message;
	if (!_editor_executing_script(tree) && _bounded_graph(tree->get_root_animation_node(), active, count, message)) {
		tree->get_property_list(&properties);
	} else {
		reader.truncated = true;
	}
	for (const PropertyInfo &property : properties) {
		if (String(property.name).begins_with("parameters/")) {
			if (parameters.size() >= MAX_ITEMS) {
				reader.truncated = true;
				break;
			}
			const String key = property.name;
			reader.truncated = reader.truncated || key.length() > MAX_STRING;
			parameters[key.left(MAX_STRING)] = _encode(tree->get(property.name), reader.truncated);
		}
	}
	result["parameters"] = parameters;
	result["parameters_partial"] = _editor_executing_script(tree) || !message.is_empty();
	result["node_count"] = reader.count;
	result["truncated"] = reader.truncated;
	return result;
}

Dictionary GodotMCPAnimationGraph::validate(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context, int &r_error_code, String &r_error_message) const {
	AnimationTree *tree = _target(p_params, p_context, false, r_error_code, r_error_message);
	if (!tree) {
		return Dictionary();
	}
	GraphValidator validator;
	return validator.run(tree, p_context);
}

Dictionary GodotMCPAnimationGraph::apply(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context, Array &r_warnings, int &r_error_code, String &r_error_message) const {
	AnimationTree *tree = _target(p_params, p_context, true, r_error_code, r_error_message);
	if (!tree) {
		return Dictionary();
	}
	if (p_params.get("operations", Variant()).get_type() != Variant::ARRAY || p_params.get("dry_run", false).get_type() != Variant::BOOL) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: operations must be an array and dry_run a boolean.";
		return Dictionary();
	}
	const Array operations = p_params["operations"];
	if (operations.is_empty() || operations.size() > MAX_OPERATIONS) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: operations must contain 1..512 ordered operations.";
		return Dictionary();
	}
	const bool dry_run = p_params.get("dry_run", false);
	const Ref<AnimationRootNode> previous = tree->get_root_animation_node();
	const Dictionary previous_parameters = _snapshot_parameters(tree, previous);
	Dictionary planned_parameters = previous_parameters.duplicate(true);
	Ref<AnimationRootNode> planned;
	const bool replaces_root = operations[0].get_type() == Variant::DICTIONARY && Dictionary(operations[0]).get("op", "") == Variant("set_root");
	if (previous.is_valid() && !replaces_root) {
		HashSet<ObjectID> active;
		int count = 0;
		CloneBounds bounds;
		if (!_bounded_graph(previous, active, count, r_error_message) || !bounds.check(previous, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			return Dictionary();
		}
		HashMap<ObjectID, Ref<Resource>> clones;
		planned = _clone_node(previous, clones, r_error_message);
		if (planned.is_null() || planned == previous) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			if (r_error_message.is_empty()) {
				r_error_message = "INVALID_ARGUMENT: Could not deep-clone the graph.";
			}
			return Dictionary();
		}
	}
	Array summaries;
	for (int i = 0; i < operations.size(); i++) {
		HashSet<ObjectID> active;
		int count = 0;
		if (operations[i].get_type() != Variant::DICTIONARY || !_operation(planned, operations[i], r_error_message) || !_bounded_graph(planned, active, count, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: operations[" + itos(i) + "]: " + GodotMCPError::strip_code_prefix(r_error_message);
			return Dictionary();
		}
		_remap_parameters(planned_parameters, operations[i]);
		Dictionary summary;
		summary["index"] = i;
		summary["op"] = Dictionary(operations[i]).get("op", "");
		summary["status"] = dry_run ? "validated" : "applied";
		summaries.push_back(summary);
	}
	UndoRedo *history = _history(p_context);
	if (!dry_run && !history) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: Active scene has no undo history.";
		return Dictionary();
	}
	if (!dry_run) {
		if (p_context.undo_redo_manager) {
			p_context.undo_redo_manager->create_action_for_history(GRAPH_ACTION, p_context.history_id);
		} else {
			history->create_action(GRAPH_ACTION);
		}
		Ref<GraphCommit> committer;
		committer.instantiate();
		committer->set_tree(tree);
		history->add_do_method(callable_mp(committer.ptr(), &GraphCommit::commit).bind(planned, planned_parameters));
		history->add_undo_method(callable_mp(committer.ptr(), &GraphCommit::commit).bind(previous, previous_parameters));
		if (p_context.undo_redo_manager) {
			p_context.undo_redo_manager->commit_action();
		} else {
			history->commit_action();
		}
		r_warnings.push_back("Animation graph changed in editor state only; use resource.save to persist it.");
	}
	Dictionary result = _base(tree, p_context);
	result["previous_scene_version"] = p_context.scene_version;
	result["updated_scene_version"] = dry_run ? p_context.scene_version : GodotMCPScenePatch::compute_scene_version(p_context.root, p_context.history_id, history->get_version());
	result["scene_version"] = result["updated_scene_version"];
	result["applied"] = !dry_run;
	result["dry_run"] = dry_run;
	result["saved"] = false;
	result["undo_action_name"] = dry_run ? String() : String(GRAPH_ACTION);
	result["operation_count"] = operations.size();
	result["operations"] = summaries;
	GraphReader reader;
	result["root"] = reader.node(planned, "");
	result["truncated"] = reader.truncated;
	return result;
}
