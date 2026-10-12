/**************************************************************************/
/*  godot_mcp_runtime_debugger.cpp                                        */
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

#include "godot_mcp_runtime_debugger.h"

#include "core/config/engine.h"
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/os/time.h"
#include "core/variant/variant.h"
#include "scene/animation/animation_blend_tree.h"
#include "scene/animation/animation_node_state_machine.h"
#include "scene/animation/animation_player.h"
#include "scene/animation/animation_tree.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

#include <cfloat>
#include <limits>

namespace {
constexpr int MAX_SERIALIZED_ITEMS = 2000;
constexpr int MAX_SERIALIZED_CHARACTERS = 256 * 1024;
constexpr int MAX_PENDING_REQUESTS = 128;
constexpr int MAX_FRAME_WAIT = 600;
constexpr int MAX_CAPTURE_DIMENSION = 4096;
constexpr int MAX_ANIMATION_PARAMETER_NAMES = 256;
constexpr int MAX_ANIMATION_PATH_LENGTH = 4096;
constexpr int MAX_ANIMATION_PATH_DEPTH = 32;
constexpr double MAX_ANIMATION_TIME = 31540000.0;
constexpr double MAX_ANIMATION_SPEED = 1000000.0;

static bool _animation_string(const Variant &p_value) {
	return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
}

static bool _animation_number(const Variant &p_value, double p_min, double p_max, double &r_number) {
	if (!p_value.is_num()) {
		return false;
	}
	r_number = p_value;
	return Math::is_finite(r_number) && r_number >= p_min && r_number <= p_max;
}

static bool _animation_path(const String &p_path, bool p_parameter = true) {
	if (p_path.is_empty() || p_path.length() > MAX_ANIMATION_PATH_LENGTH || p_path.contains(":")) {
		return false;
	}
	const int count = p_path.get_slice_count("/");
	if (count > MAX_ANIMATION_PATH_DEPTH || (p_parameter && p_path.get_slicec('/', 0) != "parameters")) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		const String part = p_path.get_slicec('/', i);
		if (part.is_empty() || part == "." || part == "..") {
			return false;
		}
	}
	return true;
}

static Ref<AnimationNode> _animation_graph_node(AnimationTree *p_tree, const String &p_prefix) {
	if (!_animation_path(p_prefix)) {
		return Ref<AnimationNode>();
	}
	Ref<AnimationNode> graph = p_tree->get_root_animation_node();
	for (int i = 1; graph.is_valid() && i < p_prefix.get_slice_count("/"); i++) {
		const StringName child = p_prefix.get_slicec('/', i);
		const Ref<AnimationNodeStateMachine> state_machine = graph;
		const Ref<AnimationNodeBlendTree> blend_tree = graph;
		if ((state_machine.is_valid() && !state_machine->has_node(child)) || (blend_tree.is_valid() && !blend_tree->has_node(child))) {
			return Ref<AnimationNode>();
		}
		graph = graph->get_child_by_name(child);
	}
	return graph;
}

static Ref<AnimationNode> _animation_parameter_node(AnimationTree *p_tree, const String &p_parameter, StringName &r_parameter_name) {
	const String prefix = p_parameter.get_base_dir();
	r_parameter_name = p_parameter.get_file();
	if (prefix.get_file() == "conditions") {
		const Ref<AnimationNodeStateMachine> state_machine = _animation_graph_node(p_tree, prefix.get_base_dir());
		if (state_machine.is_valid()) {
			r_parameter_name = "conditions/" + p_parameter.get_file();
			return state_machine;
		}
	}
	return _animation_graph_node(p_tree, prefix);
}

static bool _animation_property(AnimationTree *p_tree, const String &p_name, PropertyInfo &r_info) {
	if (!_animation_path(p_name) || !p_name.begins_with("parameters/")) {
		return false;
	}
	List<PropertyInfo> properties;
	p_tree->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == p_name) {
			r_info = property;
			return true;
		}
	}
	return false;
}

// Convert already bounded, pointer-free values to useful JSON rather than the
// string representation JSON.stringify uses for native vector variants.
static Variant _animation_json_value(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::FLOAT:
			return Math::is_finite((double)p_value) ? p_value : Variant();
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_value);
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			return _animation_json_value(Array{ v.x, v.y });
		}
		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			return _animation_json_value(Array{ v.x, v.y });
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			return _animation_json_value(Array{ v.x, v.y, v.z });
		}
		case Variant::VECTOR3I: {
			const Vector3i v = p_value;
			return _animation_json_value(Array{ v.x, v.y, v.z });
		}
		case Variant::VECTOR4: {
			const Vector4 v = p_value;
			return _animation_json_value(Array{ v.x, v.y, v.z, v.w });
		}
		case Variant::VECTOR4I: {
			const Vector4i v = p_value;
			return _animation_json_value(Array{ v.x, v.y, v.z, v.w });
		}
		case Variant::QUATERNION: {
			const Quaternion v = p_value;
			return _animation_json_value(Array{ v.x, v.y, v.z, v.w });
		}
		case Variant::COLOR: {
			const Color v = p_value;
			return _animation_json_value(Array{ v.r, v.g, v.b, v.a });
		}
		case Variant::RECT2: {
			const Rect2 v = p_value;
			return _animation_json_value(Array{ v.position.x, v.position.y, v.size.x, v.size.y });
		}
		case Variant::RECT2I: {
			const Rect2i v = p_value;
			return _animation_json_value(Array{ v.position.x, v.position.y, v.size.x, v.size.y });
		}
		case Variant::PLANE: {
			const Plane v = p_value;
			return _animation_json_value(Array{ v.normal.x, v.normal.y, v.normal.z, v.d });
		}
		case Variant::AABB: {
			const AABB v = p_value;
			return Array{ _animation_json_value(v.position), _animation_json_value(v.size) };
		}
		case Variant::TRANSFORM2D: {
			const Transform2D v = p_value;
			return Array{ _animation_json_value(v[0]), _animation_json_value(v[1]), _animation_json_value(v[2]) };
		}
		case Variant::BASIS: {
			const Basis v = p_value;
			return Array{ _animation_json_value(v.get_column(0)), _animation_json_value(v.get_column(1)), _animation_json_value(v.get_column(2)) };
		}
		case Variant::TRANSFORM3D: {
			const Transform3D v = p_value;
			return Array{ _animation_json_value(v.basis.get_column(0)), _animation_json_value(v.basis.get_column(1)),
				_animation_json_value(v.basis.get_column(2)), _animation_json_value(v.origin) };
		}
		case Variant::PROJECTION: {
			const Projection v = p_value;
			return Array{ _animation_json_value(v[0]), _animation_json_value(v[1]), _animation_json_value(v[2]), _animation_json_value(v[3]) };
		}
		case Variant::ARRAY:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY: {
			Array result;
			for (const Variant &value : (Array)p_value) {
				result.push_back(_animation_json_value(value));
			}
			return result;
		}
		case Variant::DICTIONARY: {
			Dictionary result;
			for (const KeyValue<Variant, Variant> &entry : (Dictionary)p_value) {
				result[entry.key] = _animation_json_value(entry.value);
			}
			return result;
		}
		default:
			return p_value;
	}
}

// JSON cannot preserve Variant types. Decode against the existing property,
// never by loading a resource or accepting an Object supplied by the caller.
static bool _animation_decode_value(const Variant &p_value, Variant::Type p_type, Variant &r_value,
		int p_depth = 0, const Variant &p_existing = Variant()) {
	if (p_depth > 8 || p_type == Variant::OBJECT || p_type == Variant::RID || p_type == Variant::CALLABLE || p_type == Variant::SIGNAL) {
		return false;
	}
	Variant value = p_value;
	bool explicit_integer = false;
	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary typed = value;
		const bool explicit_type = typed.has("__type");
		const bool native_type = typed.has("type") && (typed.has("value") || typed.has("args")) && p_type != Variant::DICTIONARY;
		if (explicit_type || native_type) {
			const Variant type_value = typed.get(explicit_type ? "__type" : "type", Variant());
			if (!_animation_string(type_value)) {
				return false;
			}
			const String type_name = type_value;
			explicit_integer = type_name == "int";
			if (p_type == Variant::NIL) {
				bool found = false;
				for (int i = 0; i < Variant::VARIANT_MAX; i++) {
					if (type_name == Variant::get_type_name((Variant::Type)i)) {
						p_type = (Variant::Type)i;
						found = true;
						break;
					}
				}
				if (!found || p_type == Variant::OBJECT || p_type == Variant::RID || p_type == Variant::CALLABLE || p_type == Variant::SIGNAL) {
					return false;
				}
			} else if (type_name != Variant::get_type_name(p_type)) {
				return false;
			}
			if (typed.has("value")) {
				value = typed["value"];
			} else if (typed.has("args")) {
				value = typed["args"];
			}
		}
	}
	if (p_type == Variant::NIL) {
		if (value.get_type() == Variant::NIL) {
			r_value = value;
			return true;
		}
		p_type = value.get_type();
	}
	if (p_type == Variant::BOOL || p_type == Variant::STRING) {
		if (value.get_type() != p_type) {
			return false;
		}
		r_value = value;
		return true;
	}
	if (p_type == Variant::STRING_NAME || p_type == Variant::NODE_PATH) {
		if (!_animation_string(value) && value.get_type() != p_type) {
			return false;
		}
		r_value = p_type == Variant::STRING_NAME ? Variant(StringName(String(value))) : Variant(NodePath(String(value)));
		return true;
	}
	if (p_type == Variant::INT || p_type == Variant::FLOAT) {
		if (p_type == Variant::INT && explicit_integer && value.get_type() == Variant::STRING) {
			const String decimal = value;
			const bool negative = decimal.begins_with("-");
			const String digits = negative ? decimal.substr(1) : decimal;
			const String bound = negative ? "9223372036854775808" : "9223372036854775807";
			if (digits.is_empty() || digits.length() > 19 || (digits.length() > 1 && digits[0] == '0') ||
					(digits.length() == 19 && digits > bound)) {
				return false;
			}
			for (int i = 0; i < digits.length(); i++) {
				if (digits[i] < '0' || digits[i] > '9') {
					return false;
				}
			}
			r_value = decimal.to_int();
			return true;
		}
		double number;
		if (!_animation_number(value, -DBL_MAX, DBL_MAX, number)) {
			return false;
		}
		if (p_type == Variant::INT) {
			if (value.get_type() == Variant::INT) {
				r_value = value;
				return true;
			}
			if (Math::floor(number) != number || number < -9223372036854775808.0 || number >= 9223372036854775808.0) {
				return false;
			}
			r_value = (int64_t)number;
		} else {
			r_value = number;
		}
		return true;
	}
	if (p_type == Variant::ARRAY || p_type == Variant::DICTIONARY) {
		if (value.get_type() != p_type) {
			return false;
		}
		if (p_type == Variant::ARRAY) {
			const Array existing = p_existing.get_type() == Variant::ARRAY ? (Array)p_existing : Array();
			Array result;
			if (existing.is_typed()) {
				if (existing.get_typed_builtin() == Variant::OBJECT) {
					return false;
				}
				result.set_typed(existing.get_typed_builtin(), existing.get_typed_class_name(), existing.get_typed_script());
			}
			for (const Variant &item : (Array)value) {
				Variant decoded;
				const Variant::Type element_type = existing.is_typed() ? (Variant::Type)existing.get_typed_builtin() : Variant::NIL;
				if (!_animation_decode_value(item, element_type, decoded, p_depth + 1)) {
					return false;
				}
				result.push_back(decoded);
			}
			r_value = result;
		} else {
			const Dictionary existing = p_existing.get_type() == Variant::DICTIONARY ? (Dictionary)p_existing : Dictionary();
			Dictionary result;
			if (existing.is_typed()) {
				if (existing.get_typed_key_builtin() == Variant::OBJECT || existing.get_typed_value_builtin() == Variant::OBJECT) {
					return false;
				}
				result.set_typed(existing.get_typed_key_builtin(), existing.get_typed_key_class_name(), existing.get_typed_key_script(),
						existing.get_typed_value_builtin(), existing.get_typed_value_class_name(), existing.get_typed_value_script());
			}
			for (const KeyValue<Variant, Variant> &entry : (Dictionary)value) {
				Variant decoded_key;
				Variant decoded;
				const Variant::Type key_type = existing.is_typed_key() ? (Variant::Type)existing.get_typed_key_builtin() : entry.key.get_type();
				const Variant::Type value_type = existing.is_typed_value() ? (Variant::Type)existing.get_typed_value_builtin() : Variant::NIL;
				if (!_animation_string(entry.key) || !_animation_decode_value(entry.key, key_type, decoded_key, p_depth + 1) ||
						!_animation_decode_value(entry.value, value_type, decoded, p_depth + 1)) {
					return false;
				}
				result[decoded_key] = decoded;
			}
			r_value = result;
		}
		return true;
	}
	if (p_type >= Variant::PACKED_BYTE_ARRAY && p_type <= Variant::PACKED_VECTOR4_ARRAY) {
		if (value.get_type() != Variant::ARRAY && value.get_type() != p_type) {
			return false;
		}
		Variant::Type element_type;
		switch (p_type) {
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
				element_type = Variant::INT;
				break;
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
				element_type = Variant::FLOAT;
				break;
			case Variant::PACKED_STRING_ARRAY:
				element_type = Variant::STRING;
				break;
			case Variant::PACKED_VECTOR2_ARRAY:
				element_type = Variant::VECTOR2;
				break;
			case Variant::PACKED_VECTOR3_ARRAY:
				element_type = Variant::VECTOR3;
				break;
			case Variant::PACKED_COLOR_ARRAY:
				element_type = Variant::COLOR;
				break;
			case Variant::PACKED_VECTOR4_ARRAY:
				element_type = Variant::VECTOR4;
				break;
			default:
				return false;
		}
		Array result;
		for (const Variant &item : (Array)value) {
			Variant decoded;
			if (!_animation_decode_value(item, element_type, decoded, p_depth + 1)) {
				return false;
			}
			if ((p_type == Variant::PACKED_BYTE_ARRAY && ((int64_t)decoded < 0 || (int64_t)decoded > 255)) ||
					(p_type == Variant::PACKED_INT32_ARRAY && ((int64_t)decoded < INT32_MIN || (int64_t)decoded > INT32_MAX)) ||
					(p_type == Variant::PACKED_FLOAT32_ARRAY && Math::abs((double)decoded) > FLT_MAX)) {
				return false;
			}
			result.push_back(decoded);
		}
		const Variant argument = result;
		const Variant *pointer = &argument;
		Callable::CallError error;
		Variant::construct(p_type, r_value, &pointer, 1, error);
		return error.error == Callable::CallError::CALL_OK;
	}

	if (p_type == Variant::COLOR && _animation_string(value)) {
		if (!Color::html_is_valid(String(value))) {
			return false;
		}
		r_value = Color::html(String(value));
		return true;
	}
	if (p_type == Variant::TRANSFORM3D && value.get_type() == Variant::ARRAY && Array(value).size() == 2) {
		Variant basis;
		Variant origin;
		if (!_animation_decode_value(Array(value)[0], Variant::BASIS, basis, p_depth + 1) ||
				!_animation_decode_value(Array(value)[1], Variant::VECTOR3, origin, p_depth + 1)) {
			return false;
		}
		r_value = Transform3D(basis, origin);
		return true;
	}
	Array components;
	LocalVector<Variant::Type> types;
	const char *names[4] = { "x", "y", "z", "w" };
	int count = 0;
	bool integer = false;
	switch (p_type) {
		case Variant::VECTOR2I:
			integer = true;
			[[fallthrough]];
		case Variant::VECTOR2:
			count = 2;
			break;
		case Variant::VECTOR3I:
			integer = true;
			[[fallthrough]];
		case Variant::VECTOR3:
			count = 3;
			break;
		case Variant::VECTOR4I:
			integer = true;
			[[fallthrough]];
		case Variant::VECTOR4:
		case Variant::QUATERNION:
			count = 4;
			break;
		case Variant::COLOR:
			count = 4;
			names[0] = "r";
			names[1] = "g";
			names[2] = "b";
			names[3] = "a";
			break;
		case Variant::RECT2I:
			integer = true;
			[[fallthrough]];
		case Variant::RECT2:
			count = 4;
			names[2] = "width";
			names[3] = "height";
			break;
		case Variant::PLANE:
			count = 4;
			names[3] = "d";
			break;
		case Variant::AABB:
			types.push_back(Variant::VECTOR3);
			types.push_back(Variant::VECTOR3);
			names[0] = "position";
			names[1] = "size";
			break;
		case Variant::TRANSFORM2D:
		case Variant::BASIS:
		case Variant::TRANSFORM3D:
		case Variant::PROJECTION: {
			const Variant::Type axis_type = p_type == Variant::TRANSFORM2D ? Variant::VECTOR2 : (p_type == Variant::PROJECTION ? Variant::VECTOR4 : Variant::VECTOR3);
			for (int i = 0; i < (p_type == Variant::TRANSFORM3D || p_type == Variant::PROJECTION ? 4 : 3); i++) {
				types.push_back(axis_type);
			}
			names[0] = "x_axis";
			names[1] = "y_axis";
			names[2] = p_type == Variant::TRANSFORM2D ? "origin" : "z_axis";
			names[3] = p_type == Variant::PROJECTION ? "w_axis" : "origin";
		} break;
		default:
			return false;
	}
	if (count > 0) {
		for (int i = 0; i < count; i++) {
			types.push_back(integer ? Variant::INT : Variant::FLOAT);
		}
	}
	if (value.get_type() == p_type) {
		// Native math values are normalized through the same finite component checks.
		value = _animation_json_value(value);
		if (value.get_type() == Variant::DICTIONARY) {
			value = ((Dictionary)value).get("args", Variant());
		}
	}
	if (value.get_type() == Variant::ARRAY) {
		components = value;
		// Also accept the flat constructor arguments used by JSON.from_native.
		if (count == 0 && !types.is_empty() && components.size() > (int)types.size()) {
			const int width = types[0] == Variant::VECTOR2 ? 2 : (types[0] == Variant::VECTOR4 ? 4 : 3);
			if (components.size() != (int)types.size() * width) {
				return false;
			}
			Array grouped;
			for (uint32_t i = 0; i < types.size(); i++) {
				Array axis;
				for (int j = 0; j < width; j++) {
					axis.push_back(components[i * width + j]);
				}
				grouped.push_back(axis);
			}
			components = grouped;
		}
		if (p_type == Variant::COLOR && components.size() == 3) {
			components = components.duplicate();
			components.push_back(1.0);
		}
	} else if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary fields = value;
		for (uint32_t i = 0; i < types.size(); i++) {
			if (!fields.has(names[i]) && !(p_type == Variant::COLOR && i == 3)) {
				return false;
			}
			components.push_back(fields.get(names[i], 1.0));
		}
	} else {
		return false;
	}
	if (components.size() != (int)types.size()) {
		return false;
	}
	LocalVector<Variant> arguments;
	LocalVector<const Variant *> pointers;
	arguments.resize(types.size());
	pointers.resize(types.size());
	for (uint32_t i = 0; i < types.size(); i++) {
		if (!_animation_decode_value(components[i], types[i], arguments[i], p_depth + 1)) {
			return false;
		}
		if (types[i] == Variant::INT && ((int64_t)arguments[i] < INT32_MIN || (int64_t)arguments[i] > INT32_MAX)) {
			return false;
		}
		if (types[i] == Variant::FLOAT && Math::abs((double)arguments[i]) > (p_type == Variant::COLOR ? (double)FLT_MAX : (double)std::numeric_limits<real_t>::max())) {
			return false;
		}
		pointers[i] = &arguments[i];
	}
	Callable::CallError error;
	Variant::construct(p_type, r_value, pointers.ptr(), pointers.size(), error);
	return error.error == Callable::CallError::CALL_OK;
}

static String _runtime_argument(const String &p_prefix) {
	for (const String &argument : OS::get_singleton()->get_cmdline_user_args()) {
		if (argument.begins_with(p_prefix)) {
			return argument.substr(p_prefix.length());
		}
	}
	return String();
}
} // namespace

GodotMCPRuntimeDebugger *GodotMCPRuntimeDebugger::singleton = nullptr;

void GodotMCPRuntimeDebugger::_bind_methods() {
}

void GodotMCPRuntimeDebugger::initialize() {
	// Do not install an automation capture or error handler in ordinary game
	// runs. The editor controller adds this user argument only to runs it owns.
	if (!singleton && EngineDebugger::is_active() && !_runtime_argument("--godot-mcp-run-id=").is_empty()) {
		memnew(GodotMCPRuntimeDebugger);
	}
}

void GodotMCPRuntimeDebugger::deinitialize() {
	if (singleton) {
		memdelete(singleton);
	}
}

GodotMCPRuntimeDebugger::GodotMCPRuntimeDebugger() {
	singleton = this;
	run_id = _runtime_argument("--godot-mcp-run-id=");
	EngineDebugger::register_message_capture("godot_mcp", EngineDebugger::Capture(this, _capture_message));

	error_handler.errfunc = _error_handler_callback;
	error_handler.userdata = this;
	add_error_handler(&error_handler);
	error_handler_registered = true;
	callable_mp(this, &GodotMCPRuntimeDebugger::_send_ready).call_deferred();
}

GodotMCPRuntimeDebugger::~GodotMCPRuntimeDebugger() {
	if (error_handler_registered) {
		remove_error_handler(&error_handler);
		error_handler_registered = false;
	}
	if (EngineDebugger::has_capture("godot_mcp")) {
		EngineDebugger::unregister_message_capture("godot_mcp");
	}
	for (const PendingRequest &pending : pending_requests) {
		if (pending.kind == PENDING_ACTION_RELEASE && !pending.action.is_empty()) {
			Input::get_singleton()->action_release(pending.action);
		}
	}
	pending_requests.clear();
	singleton = nullptr;
}

void GodotMCPRuntimeDebugger::_error_handler_callback(void *p_user_data, const char *p_function,
		const char *p_file, int p_line, const char *p_error, const char *p_message,
		bool p_editor_notify, ErrorHandlerType p_type) {
	GodotMCPRuntimeDebugger *self = static_cast<GodotMCPRuntimeDebugger *>(p_user_data);
	if (!self || self->run_id.is_empty() || !EngineDebugger::get_singleton()) {
		return;
	}
	Dictionary diagnostic;
	diagnostic["timestamp_msec"] = (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
	diagnostic["severity"] = p_type == ERR_HANDLER_WARNING ? "warning" : "error";
	diagnostic["source"] = "runtime";
	diagnostic["message"] = p_message && p_message[0] ? String::utf8(p_message) : String::utf8(p_error);
	diagnostic["file"] = p_file ? String::utf8(p_file) : String();
	diagnostic["line"] = p_line;
	diagnostic["column"] = 0;
	diagnostic["function"] = p_function ? String::utf8(p_function) : String();
	diagnostic["stack_trace"] = Array();
	EngineDebugger::get_singleton()->send_message("godot_mcp:diagnostic", { self->run_id, diagnostic });
}

Error GodotMCPRuntimeDebugger::_capture_message(void *p_user_data, const String &p_message,
		const Array &p_args, bool &r_captured) {
	r_captured = true;
	GodotMCPRuntimeDebugger *self = static_cast<GodotMCPRuntimeDebugger *>(p_user_data);
	ERR_FAIL_NULL_V(self, ERR_UNCONFIGURED);
	if (p_message != "request") {
		return ERR_SKIP;
	}
	ERR_FAIL_COND_V(p_args.size() != 4 || p_args[0].get_type() != Variant::STRING ||
					p_args[1].get_type() != Variant::STRING || p_args[2].get_type() != Variant::STRING ||
					p_args[3].get_type() != Variant::DICTIONARY,
			ERR_INVALID_DATA);
	self->_handle_request(p_args[0], p_args[1], p_args[2], p_args[3]);
	return OK;
}

bool GodotMCPRuntimeDebugger::_serialize_variant(const Variant &p_value, Variant &r_serialized,
		int p_depth, int *r_items, int *r_characters) {
	int local_items = 0;
	int local_characters = 0;
	if (!r_items) {
		r_items = &local_items;
	}
	if (!r_characters) {
		r_characters = &local_characters;
	}
	if (p_depth > 8 || *r_items >= MAX_SERIALIZED_ITEMS) {
		return false;
	}
	(*r_items)++;

	const Variant::Type type = p_value.get_type();
	if (type == Variant::STRING || type == Variant::STRING_NAME || type == Variant::NODE_PATH) {
		const String text = p_value;
		if (text.length() > MAX_SERIALIZED_CHARACTERS - *r_characters) {
			return false;
		}
		*r_characters += text.length();
		r_serialized = p_value;
		return true;
	}

	int packed_size = 0;
	bool packed_array = true;
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_INT32_ARRAY: {
			const PackedInt32Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_INT64_ARRAY: {
			const PackedInt64Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_FLOAT32_ARRAY: {
			const PackedFloat32Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_FLOAT64_ARRAY: {
			const PackedFloat64Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_VECTOR2_ARRAY: {
			const PackedVector2Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_VECTOR3_ARRAY: {
			const PackedVector3Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_COLOR_ARRAY: {
			const PackedColorArray values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_VECTOR4_ARRAY: {
			const PackedVector4Array values = p_value;
			packed_size = values.size();
		} break;
		case Variant::PACKED_STRING_ARRAY: {
			const PackedStringArray strings = p_value;
			packed_size = strings.size();
			for (const String &text : strings) {
				if (text.length() > MAX_SERIALIZED_CHARACTERS - *r_characters) {
					return false;
				}
				*r_characters += text.length();
			}
		} break;
		default:
			packed_array = false;
			break;
	}
	if (packed_array) {
		if (packed_size > MAX_SERIALIZED_ITEMS - *r_items) {
			return false;
		}
		*r_items += packed_size;
		r_serialized = p_value;
		return true;
	}

	if (type == Variant::OBJECT) {
		Object *object = p_value.get_validated_object();
		if (!object) {
			r_serialized = Variant();
			return true;
		}
		if (Node *node = Object::cast_to<Node>(object)) {
			Dictionary reference;
			const String class_name = node->get_class();
			const String node_path = String(node->get_path());
			if (class_name.length() + node_path.length() > MAX_SERIALIZED_CHARACTERS - *r_characters) {
				return false;
			}
			*r_characters += class_name.length() + node_path.length();
			reference["class"] = class_name;
			reference["node_path"] = node_path;
			r_serialized = reference;
			return true;
		}
		if (Resource *resource = Object::cast_to<Resource>(object)) {
			Dictionary reference;
			const String class_name = resource->get_class();
			const String resource_path = resource->get_path();
			const ResourceUID::ID uid = resource->get_path().is_empty() ? ResourceUID::INVALID_ID : ResourceLoader::get_resource_uid(resource->get_path());
			const String uid_text = uid == ResourceUID::INVALID_ID ? String() : ResourceUID::get_singleton()->id_to_text(uid);
			if (class_name.length() + resource_path.length() + uid_text.length() > MAX_SERIALIZED_CHARACTERS - *r_characters) {
				return false;
			}
			*r_characters += class_name.length() + resource_path.length() + uid_text.length();
			reference["class"] = class_name;
			reference["path"] = resource_path;
			reference["uid"] = uid_text;
			r_serialized = reference;
			return true;
		}
		return false;
	}
	if (type == Variant::ARRAY) {
		Array result;
		for (const Variant &item : (Array)p_value) {
			Variant serialized;
			if (!_serialize_variant(item, serialized, p_depth + 1, r_items, r_characters)) {
				return false;
			}
			result.push_back(serialized);
		}
		r_serialized = result;
		return true;
	}
	if (type == Variant::DICTIONARY) {
		Dictionary result;
		for (const KeyValue<Variant, Variant> &entry : (Dictionary)p_value) {
			if (entry.key.get_type() != Variant::STRING && entry.key.get_type() != Variant::STRING_NAME) {
				return false;
			}
			const String key = entry.key;
			if (*r_items >= MAX_SERIALIZED_ITEMS || key.length() > MAX_SERIALIZED_CHARACTERS - *r_characters) {
				return false;
			}
			(*r_items)++;
			*r_characters += key.length();
			Variant serialized;
			if (!_serialize_variant(entry.value, serialized, p_depth + 1, r_items, r_characters)) {
				return false;
			}
			result[key] = serialized;
		}
		r_serialized = result;
		return true;
	}
	if (type == Variant::CALLABLE || type == Variant::SIGNAL || type == Variant::RID) {
		return false;
	}
	r_serialized = p_value;
	return true;
}

bool GodotMCPRuntimeDebugger::_variant_to_vector2(const Variant &p_value, Vector2 &r_vector) {
	if (p_value.get_type() == Variant::VECTOR2) {
		r_vector = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array values = p_value;
		if (values.size() == 2 && values[0].is_num() && values[1].is_num()) {
			r_vector = Vector2(values[0], values[1]);
			return true;
		}
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary values = p_value;
		if (values.has("x") && values.has("y") && values["x"].is_num() && values["y"].is_num()) {
			r_vector = Vector2(values["x"], values["y"]);
			return true;
		}
	}
	return false;
}

Dictionary GodotMCPRuntimeDebugger::_serialize_tree(Node *p_node, int p_depth, int p_max_depth,
		int p_max_nodes, int &r_count, bool &r_truncated) {
	Dictionary result;
	if (!p_node || r_count >= p_max_nodes) {
		r_truncated = true;
		return result;
	}
	r_count++;
	result["name"] = p_node->get_name();
	result["class"] = p_node->get_class();
	result["path"] = String(p_node->get_path());
	result["process_mode"] = p_node->get_process_mode();
	result["child_count"] = p_node->get_child_count(false);
	Array children;
	if (p_depth < p_max_depth) {
		for (int i = 0; i < p_node->get_child_count(false); i++) {
			if (r_count >= p_max_nodes) {
				r_truncated = true;
				break;
			}
			children.push_back(_serialize_tree(p_node->get_child(i, false), p_depth + 1, p_max_depth, p_max_nodes, r_count, r_truncated));
		}
	} else if (p_node->get_child_count(false) > 0) {
		r_truncated = true;
	}
	result["children"] = children;
	return result;
}

bool GodotMCPRuntimeDebugger::_has_property(Object *p_object, const StringName &p_property) {
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == p_property) {
			return true;
		}
	}
	return false;
}

Node *GodotMCPRuntimeDebugger::_resolve_node(const String &p_path) const {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root() || !p_path.begins_with("/root")) {
		return nullptr;
	}
	if (p_path == "/root") {
		return tree->get_root();
	}
	return tree->get_root()->get_node_or_null(NodePath(p_path));
}

void GodotMCPRuntimeDebugger::_send_ready() {
	// Scene types are registered before the main SceneTree is constructed.
	// Hook frame callbacks in this deferred initialization so pending frame,
	// input, and capture operations can complete before announcing readiness.
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return;
	}
	tree->connect(SNAME("process_frame"), callable_mp(this, &GodotMCPRuntimeDebugger::_process_frame));
	tree->connect(SNAME("physics_frame"), callable_mp(this, &GodotMCPRuntimeDebugger::_physics_frame));
	if (!run_id.is_empty() && EngineDebugger::get_singleton()) {
		EngineDebugger::get_singleton()->send_message("godot_mcp:ready", {
																				 run_id,
																				 (int64_t)OS::get_singleton()->get_process_id(),
																				 (int64_t)Engine::get_singleton()->get_process_frames(),
																				 (int64_t)Engine::get_singleton()->get_physics_frames(),
																		 });
	}
}

void GodotMCPRuntimeDebugger::_send_response(const String &p_request_id, const String &p_operation,
		bool p_ok, const Variant &p_payload) const {
	if (EngineDebugger::get_singleton()) {
		EngineDebugger::get_singleton()->send_message("godot_mcp:response", { p_request_id, run_id, p_operation, p_ok, p_payload });
	}
}

void GodotMCPRuntimeDebugger::_send_error(const String &p_request_id, const String &p_operation,
		const String &p_code, const String &p_message, bool p_retryable, const Dictionary &p_details) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["operation"] = p_operation;
	error["run_id"] = run_id;
	error["retryable"] = p_retryable;
	if (!p_details.is_empty()) {
		error["details"] = p_details;
	}
	_send_response(p_request_id, p_operation, false, error);
}

void GodotMCPRuntimeDebugger::_handle_request(const String &p_request_id, const String &p_requested_run_id,
		const String &p_operation, const Dictionary &p_params) {
	if (run_id.is_empty() || p_requested_run_id != run_id) {
		_send_error(p_request_id, p_operation, "STALE_RUN", "Request run_id does not identify this runtime process.");
		return;
	}
	if (Engine::get_singleton()->is_editor_hint()) {
		_send_error(p_request_id, p_operation, "INVALID_RUNTIME_TARGET", "Runtime requests cannot execute in the editor SceneTree.");
		return;
	}
	if (p_operation == "get_tree") {
		_handle_get_tree(p_request_id, p_params);
	} else if (p_operation == "get_property") {
		_handle_get_property(p_request_id, p_params);
	} else if (p_operation == "set_property") {
		_handle_set_property(p_request_id, p_params);
	} else if (p_operation == "call") {
		_handle_call(p_request_id, p_params);
	} else if (p_operation == "animation_get") {
		_handle_animation_get(p_request_id, p_params);
	} else if (p_operation == "animation_control") {
		_handle_animation_control(p_request_id, p_params);
	} else if (p_operation == "input_action") {
		_handle_input_action(p_request_id, p_params);
	} else if (p_operation == "input_event") {
		_handle_input_event(p_request_id, p_params);
	} else if (p_operation == "wait_frames") {
		_handle_wait_frames(p_request_id, p_params);
	} else if (p_operation == "capture_viewport") {
		_handle_capture(p_request_id, p_params);
	} else {
		_send_error(p_request_id, p_operation, "METHOD_NOT_FOUND", "Unknown runtime operation.");
	}
}

void GodotMCPRuntimeDebugger::_handle_get_tree(const String &p_request_id, const Dictionary &p_params) {
	const String root_path = p_params.get("root_path", String("/root"));
	Node *root = _resolve_node(root_path);
	if (!root) {
		_send_error(p_request_id, "get_tree", "NOT_FOUND", "Runtime root_path was not found.");
		return;
	}
	const int max_depth = CLAMP((int)p_params.get("max_depth", 6), 0, 12);
	const int max_nodes = CLAMP((int)p_params.get("max_nodes", 500), 1, 2000);
	int count = 0;
	bool truncated = false;
	Dictionary data;
	data["tree"] = _serialize_tree(root, 0, max_depth, max_nodes, count, truncated);
	data["node_count"] = count;
	data["truncated"] = truncated;
	data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
	_send_response(p_request_id, "get_tree", true, data);
}

void GodotMCPRuntimeDebugger::_handle_get_property(const String &p_request_id, const Dictionary &p_params) {
	const String node_path = p_params.get("node_path", String());
	const StringName property_name = p_params.get("property_name", StringName());
	Node *node = _resolve_node(node_path);
	if (!node) {
		_send_error(p_request_id, "get_property", "NOT_FOUND", "Runtime node_path was not found.");
		return;
	}
	if (property_name.is_empty() || !_has_property(node, property_name)) {
		_send_error(p_request_id, "get_property", "NOT_FOUND", "Runtime property does not exist.");
		return;
	}
	Variant serialized;
	if (!_serialize_variant(node->get(property_name), serialized)) {
		_send_error(p_request_id, "get_property", "NON_SERIALIZABLE", "Runtime property value is not serializable.");
		return;
	}
	Dictionary data;
	data["node_path"] = node_path;
	data["property_name"] = property_name;
	data["value"] = serialized;
	data["type"] = Variant::get_type_name(node->get(property_name).get_type());
	_send_response(p_request_id, "get_property", true, data);
}

void GodotMCPRuntimeDebugger::_handle_set_property(const String &p_request_id, const Dictionary &p_params) {
	const String node_path = p_params.get("node_path", String());
	const StringName property_name = p_params.get("property_name", StringName());
	Node *node = _resolve_node(node_path);
	if (!node) {
		_send_error(p_request_id, "set_property", "NOT_FOUND", "Runtime node_path was not found.");
		return;
	}
	if (!p_params.has("value") || property_name.is_empty() || !_has_property(node, property_name)) {
		_send_error(p_request_id, "set_property", "NOT_FOUND", "Runtime property does not exist or value is missing.");
		return;
	}
	bool valid = false;
	node->set(property_name, p_params["value"], &valid);
	if (!valid) {
		_send_error(p_request_id, "set_property", "INVALID_ARGUMENT", "Godot rejected the runtime property value.");
		return;
	}
	Variant serialized;
	if (!_serialize_variant(node->get(property_name), serialized)) {
		serialized = Variant();
	}
	Dictionary data;
	data["node_path"] = node_path;
	data["property_name"] = property_name;
	data["value"] = serialized;
	_send_response(p_request_id, "set_property", true, data);
}

void GodotMCPRuntimeDebugger::_handle_call(const String &p_request_id, const Dictionary &p_params) {
	const String node_path = p_params.get("node_path", String());
	const StringName method = p_params.get("method", StringName());
	Node *node = _resolve_node(node_path);
	if (!node) {
		_send_error(p_request_id, "call", "NOT_FOUND", "Runtime node_path was not found.");
		return;
	}
	if (method.is_empty() || !node->has_method(method)) {
		_send_error(p_request_id, "call", "NOT_FOUND", "Runtime method does not exist.");
		return;
	}
	if ((method == SNAME("free") || method == SNAME("queue_free")) && !bool(p_params.get("confirm_destructive", false))) {
		_send_error(p_request_id, "call", "CONFIRMATION_REQUIRED", "Calling free/queue_free requires confirm_destructive=true.");
		return;
	}
	Array arguments = p_params.get("arguments", Array());
	if (arguments.size() > 16) {
		_send_error(p_request_id, "call", "INVALID_ARGUMENT", "Runtime calls accept at most 16 arguments.");
		return;
	}
	LocalVector<Variant> argument_values;
	argument_values.resize(arguments.size());
	LocalVector<const Variant *> argument_ptrs;
	argument_ptrs.resize(arguments.size());
	for (int i = 0; i < arguments.size(); i++) {
		argument_values[i] = arguments[i];
		argument_ptrs[i] = &argument_values[i];
	}
	Callable::CallError call_error;
	Variant result = node->callp(method, argument_ptrs.is_empty() ? nullptr : argument_ptrs.ptr(), argument_ptrs.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		_send_error(p_request_id, "call", "RUNTIME_CALL_ERROR", Variant::get_call_error_text(node, method, argument_ptrs.is_empty() ? nullptr : argument_ptrs.ptr(), argument_ptrs.size(), call_error));
		return;
	}
	Variant serialized;
	if (!_serialize_variant(result, serialized)) {
		_send_error(p_request_id, "call", "NON_SERIALIZABLE", "Runtime method result is not serializable.");
		return;
	}
	Dictionary data;
	data["node_path"] = node_path;
	data["method"] = method;
	data["result"] = serialized;
	_send_response(p_request_id, "call", true, data);
}

bool GodotMCPRuntimeDebugger::_validate_animation_request(const String &p_request_id, const String &p_operation,
		const Dictionary &p_params, Node *&r_node, String &r_playback_path, Array &r_parameter_names, int &r_limit) {
	const Variant path_value = p_params.get("node_path", Variant());
	if (!_animation_string(path_value)) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "node_path must be an absolute runtime /root node path.");
		return false;
	}
	const String path = path_value;
	const NodePath node_path(path);
	if (path.length() > MAX_ANIMATION_PATH_LENGTH || (path != "/root" && !path.begins_with("/root/")) ||
			!node_path.is_absolute() || node_path.get_subname_count() != 0) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "node_path must be an absolute runtime /root node path without property subnames (at most 4096 characters).");
		return false;
	}
	r_node = _resolve_node(path);
	if (!r_node) {
		_send_error(p_request_id, p_operation, "NOT_FOUND", "Runtime animation node_path was not found.");
		return false;
	}
	AnimationTree *tree = Object::cast_to<AnimationTree>(r_node);
	if (!tree && !Object::cast_to<AnimationPlayer>(r_node)) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "node_path must identify an AnimationPlayer or AnimationTree.");
		return false;
	}
	const Variant playback_value = p_params.get("playback_path", String("parameters/playback"));
	if (!_animation_string(playback_value)) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "playback_path must be a parameters/.../playback property path.");
		return false;
	}
	r_playback_path = playback_value;
	if (!_animation_path(r_playback_path) || !r_playback_path.ends_with("/playback")) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "playback_path must be a canonical parameters/.../playback property path (at most 32 segments and 4096 characters).");
		return false;
	}
	if (tree && p_params.has("playback_path")) {
		PropertyInfo info;
		const Ref<AnimationNodeStateMachine> graph = _animation_graph_node(tree, r_playback_path.get_base_dir());
		if (!_animation_property(tree, r_playback_path, info) || graph.is_null()) {
			_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "playback_path does not identify a state-machine playback property.");
			return false;
		}
		const Ref<AnimationNodeStateMachinePlayback> playback = tree->get(r_playback_path);
		if (playback.is_null()) {
			_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "playback_path does not contain AnimationNodeStateMachinePlayback.");
			return false;
		}
	}
	double limit;
	if (!_animation_number(p_params.get("limit", 256), 1, MAX_SERIALIZED_ITEMS, limit) || Math::floor(limit) != limit) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "limit must be an integer in range 1..2000.");
		return false;
	}
	r_limit = (int)limit;
	const Variant names_value = p_params.get("parameter_names", Array());
	if (names_value.get_type() != Variant::ARRAY || ((Array)names_value).size() > MAX_ANIMATION_PARAMETER_NAMES) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "parameter_names must be an array of at most 256 parameter paths.");
		return false;
	}
	r_parameter_names = names_value;
	Variant bounded_names;
	if (!_serialize_variant(r_parameter_names, bounded_names)) {
		_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "parameter_names exceeds the runtime serialization bounds.");
		return false;
	}
	for (const Variant &name_value : r_parameter_names) {
		PropertyInfo info;
		if (!_animation_string(name_value) || !tree || !_animation_property(tree, String(name_value), info)) {
			_send_error(p_request_id, p_operation, "INVALID_ARGUMENT", "parameter_names must identify existing parameters/* properties of an AnimationTree.");
			return false;
		}
	}
	return true;
}

Dictionary GodotMCPRuntimeDebugger::_animation_snapshot(Node *p_node, const String &p_playback_path,
		const Array &p_parameter_names, int p_limit) const {
	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_node);
	AnimationTree *tree = Object::cast_to<AnimationTree>(p_node);
	AnimationMixer *mixer = Object::cast_to<AnimationMixer>(p_node);
	Dictionary data;
	data["run_id"] = run_id;
	data["node_path"] = String(p_node->get_path());
	data["class"] = p_node->get_class();
	data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
	data["paused"] = SceneTree::get_singleton()->is_paused();
	data["suspended"] = SceneTree::get_singleton()->is_suspended();
	data["active"] = mixer->is_active();
	data["limit"] = p_limit;
	data["truncated"] = false;
	data["animations"] = Array();
	data["parameters"] = Dictionary();
	data["playback"] = Variant();
	Dictionary root_motion;
	root_motion["track"] = String(mixer->get_root_motion_track());
	root_motion["local"] = mixer->is_root_motion_local();
	root_motion["position"] = mixer->get_root_motion_position();
	root_motion["rotation"] = mixer->get_root_motion_rotation();
	root_motion["scale"] = mixer->get_root_motion_scale();
	root_motion["position_accumulator"] = mixer->get_root_motion_position_accumulator();
	root_motion["rotation_accumulator"] = mixer->get_root_motion_rotation_accumulator();
	root_motion["scale_accumulator"] = mixer->get_root_motion_scale_accumulator();
	data["root_motion"] = root_motion;
	if (player) {
		Dictionary state;
		state["current_animation"] = String(player->get_current_animation());
		state["assigned_animation"] = String(player->get_assigned_animation());
		state["playing"] = player->is_playing();
		state["valid"] = player->is_valid();
		state["length"] = player->is_valid() ? player->get_current_animation_length() : 0.0;
		state["position"] = player->is_valid() ? player->get_current_animation_position() : 0.0;
		state["speed_scale"] = player->get_speed_scale();
		state["playing_speed"] = player->get_playing_speed();
		state["queue"] = Array();
		data["player"] = state;
	}
	Ref<AnimationNodeStateMachinePlayback> playback;
	if (tree) {
		Dictionary state;
		state["animation_player"] = String(tree->get_animation_player());
		Node *bound_player = tree->get_animation_player().is_empty() ? nullptr : tree->get_node_or_null(tree->get_animation_player());
		state["bound_player"] = bound_player ? Variant(String(bound_player->get_path())) : Variant();
		state["tree_root"] = tree->get_root_animation_node();
		data["tree"] = state;
		PropertyInfo info;
		if (_animation_property(tree, p_playback_path, info)) {
			playback = tree->get(p_playback_path);
		}
		if (playback.is_valid()) {
			Dictionary state_machine;
			state_machine["path"] = p_playback_path;
			state_machine["class"] = playback->get_class();
			state_machine["playing"] = playback->is_playing();
			state_machine["current_node"] = String(playback->get_current_node());
			state_machine["position"] = playback->get_current_play_pos();
			state_machine["length"] = playback->get_current_length();
			state_machine["travel_path"] = Array();
			state_machine["fading_from_node"] = String(playback->get_fading_from_node());
			state_machine["fading_from_position"] = playback->get_fading_from_play_pos();
			state_machine["fading_from_length"] = playback->get_fading_from_length();
			state_machine["fading_length"] = playback->get_fading_time();
			state_machine["fading_position"] = playback->get_fading_pos();
			state_machine["at_start"] = playback->get_current_node() == SNAME("Start");
			state_machine["at_end"] = playback->is_end();
			data["playback"] = state_machine;
		}
	}

	// Reserve all fixed fields and collection containers first. Share the existing
	// serialization budget across every returned list and parameter, not per item.
	Variant safe;
	if (!_serialize_variant(data, safe)) {
		return Dictionary();
	}
	data = _animation_json_value(safe);
	// Leave room for the control acknowledgment fields added after a mutation.
	int items = 16;
	int characters = 128;
	if (!_serialize_variant(data, safe, 0, &items, &characters)) {
		return Dictionary();
	}
	bool truncated = false;
	auto append_item = [&](Array &r_array, const Variant &p_item, int p_depth) {
		if (r_array.size() >= p_limit) {
			truncated = true;
			return false;
		}
		int next_items = items;
		int next_characters = characters;
		Variant serialized;
		if (!_serialize_variant(p_item, serialized, p_depth, &next_items, &next_characters)) {
			truncated = true;
			return false;
		}
		items = next_items;
		characters = next_characters;
		r_array.push_back(serialized);
		return true;
	};
	if (tree) {
		List<PropertyInfo> properties;
		if (p_parameter_names.is_empty()) {
			tree->get_property_list(&properties);
		} else {
			for (const Variant &name : p_parameter_names) {
				PropertyInfo info;
				if (_animation_property(tree, name, info)) {
					properties.push_back(info);
				}
			}
		}
		Dictionary parameters = data["parameters"];
		for (const PropertyInfo &info : properties) {
			const String name = info.name;
			if (!name.begins_with("parameters/") || parameters.has(name)) {
				continue;
			}
			if (parameters.size() >= p_limit) {
				truncated = true;
				break;
			}
			const Variant value = tree->get(info.name);
			Variant serialized_value;
			if (!_serialize_variant(value, serialized_value)) {
				truncated = true;
				continue;
			}
			StringName parameter_name;
			const Ref<AnimationNode> graph = _animation_parameter_node(tree, name, parameter_name);
			Dictionary entry;
			entry["type"] = Variant::get_type_name(info.type == Variant::NIL ? value.get_type() : info.type);
			entry["value"] = _animation_json_value(serialized_value);
			entry["read_only"] = bool(info.usage & PROPERTY_USAGE_READ_ONLY) || info.type == Variant::OBJECT ||
					(graph.is_valid() && graph->is_parameter_read_only(parameter_name));
			int next_items = items + 1;
			int next_characters = characters + name.length();
			Variant serialized_entry;
			if (next_characters > MAX_SERIALIZED_CHARACTERS || !_serialize_variant(entry, serialized_entry, 2, &next_items, &next_characters)) {
				truncated = true;
				break;
			}
			items = next_items;
			characters = next_characters;
			parameters[name] = serialized_entry;
		}
	}
	if (playback.is_valid()) {
		Dictionary state = data["playback"];
		Array path = state["travel_path"];
		for (const StringName &name : playback->get_travel_path()) {
			if (!append_item(path, String(name), 3)) {
				break;
			}
		}
	}
	if (player) {
		Dictionary state = data["player"];
		Array queue = state["queue"];
		for (const StringName &name : player->get_queue()) {
			if (!append_item(queue, String(name), 3)) {
				break;
			}
		}
	}
	Array animations = data["animations"];
	for (const StringName &name : mixer->get_sorted_animation_list()) {
		if (!append_item(animations, String(name), 2)) {
			break;
		}
	}
	data["truncated"] = truncated;
	return data;
}

void GodotMCPRuntimeDebugger::_handle_animation_get(const String &p_request_id, const Dictionary &p_params) {
	Node *node = nullptr;
	String playback_path;
	Array parameter_names;
	int limit;
	if (!_validate_animation_request(p_request_id, "animation_get", p_params, node, playback_path, parameter_names, limit)) {
		return;
	}
	const Dictionary data = _animation_snapshot(node, playback_path, parameter_names, limit);
	if (data.is_empty()) {
		_send_error(p_request_id, "animation_get", "NON_SERIALIZABLE", "Animation snapshot exceeds the runtime serialization bounds.");
		return;
	}
	_send_response(p_request_id, "animation_get", true, data);
}

void GodotMCPRuntimeDebugger::_handle_animation_control(const String &p_request_id, const Dictionary &p_params) {
	Node *node = nullptr;
	String playback_path;
	Array parameter_names;
	int limit;
	if (!_validate_animation_request(p_request_id, "animation_control", p_params, node, playback_path, parameter_names, limit)) {
		return;
	}
	auto invalid = [&](const String &p_message) {
		_send_error(p_request_id, "animation_control", "INVALID_ARGUMENT", p_message);
	};
	const Variant action_value = p_params.get("action", Variant());
	if (!_animation_string(action_value)) {
		invalid("action must name a supported animation control action.");
		return;
	}
	const String action = action_value;
	const bool player_action = action == "player_play" || action == "player_stop" || action == "player_pause" ||
			action == "player_seek" || action == "player_queue" || action == "player_advance";
	const bool state_action = action == "state_start" || action == "state_travel" || action == "state_stop" || action == "state_next";
	const bool oneshot_action = action == "oneshot_fire" || action == "oneshot_abort" || action == "oneshot_fadeout";
	if (!player_action && !state_action && !oneshot_action && action != "set_parameter" && action != "set_active" && action != "transition_request") {
		invalid("Unknown animation control action.");
		return;
	}
	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(node);
	AnimationTree *tree = Object::cast_to<AnimationTree>(node);
	if ((player_action && !player) || (!player_action && !tree)) {
		invalid("This action is not supported by the target animation node class.");
		return;
	}
	for (const char *name : { "animation", "state", "parameter", "transition" }) {
		if (p_params.has(name) && (!_animation_string(p_params[name]) || String(p_params[name]).length() > MAX_ANIMATION_PATH_LENGTH)) {
			invalid(String(name) + " must be a string of at most 4096 characters.");
			return;
		}
	}
	for (const char *name : { "from_end", "update", "reset", "keep_state", "active" }) {
		if (p_params.has(name) && p_params[name].get_type() != Variant::BOOL) {
			invalid(String(name) + " must be a boolean.");
			return;
		}
	}
	double time;
	double delta;
	double custom_blend;
	double custom_speed;
	if (!_animation_number(p_params.get("time", 0.0), 0.0, MAX_ANIMATION_TIME, time) ||
			!_animation_number(p_params.get("delta", 0.0), -MAX_ANIMATION_TIME, MAX_ANIMATION_TIME, delta) ||
			!_animation_number(p_params.get("custom_blend", -1.0), -1.0, MAX_ANIMATION_TIME, custom_blend) ||
			(custom_blend < 0.0 && custom_blend != -1.0) ||
			!_animation_number(p_params.get("custom_speed", 1.0), -MAX_ANIMATION_SPEED, MAX_ANIMATION_SPEED, custom_speed)) {
		invalid("time must be finite in 0..31540000; delta in -31540000..31540000; custom_blend must be -1 or 0..31540000; custom_speed in -1000000..1000000.");
		return;
	}
	const ObjectID target_id = node->get_instance_id();
	Ref<AnimationNodeStateMachinePlayback> playback;
	StringName animation;
	String state;
	String parameter;
	Variant parameter_value;
	if (player_action) {
		if (action == "player_play" || action == "player_queue") {
			animation = p_params.get("animation", StringName());
			if (animation.is_empty() && action == "player_play") {
				animation = player->get_assigned_animation();
			}
			if (animation.is_empty() || !player->has_animation(animation)) {
				invalid("animation must identify an existing clip (player_play may omit it to resume the assigned animation).");
				return;
			}
		}
		if (action == "player_seek" && (!p_params.has("time") || (!player->is_valid() && (player->get_assigned_animation().is_empty() || !player->has_animation(player->get_assigned_animation()))))) {
			invalid("player_seek requires time and a current or assigned animation.");
			return;
		}
		if (action == "player_advance" && !p_params.has("delta")) {
			invalid("player_advance requires delta.");
			return;
		}
		if (action == "player_queue" && player->get_queue().size() >= MAX_SERIALIZED_ITEMS) {
			invalid("The animation queue is limited to 2000 entries.");
			return;
		}
	} else if (state_action) {
		PropertyInfo info;
		const Ref<AnimationNodeStateMachine> graph = _animation_graph_node(tree, playback_path.get_base_dir());
		if (graph.is_null() || !_animation_property(tree, playback_path, info)) {
			invalid("playback_path must identify an AnimationNodeStateMachinePlayback property and its graph.");
			return;
		}
		playback = tree->get(playback_path);
		if (playback.is_null() || graph->get_state_machine_type() == AnimationNodeStateMachine::STATE_MACHINE_TYPE_GROUPED) {
			invalid("Grouped state machines must be controlled through their parent playback.");
			return;
		}
		if (action == "state_start" || action == "state_travel") {
			state = p_params.get("state", String());
			if (!_animation_path(state, false) || state.contains("/Start") || state.contains("/End")) {
				invalid("state must be a canonical existing state path; grouped Start/End paths cannot be requested directly.");
				return;
			}
			Ref<AnimationNodeStateMachine> current_graph = graph;
			for (int i = 0; i < state.get_slice_count("/"); i++) {
				const StringName name = state.get_slicec('/', i);
				if (current_graph.is_null() || !current_graph->has_node(name)) {
					invalid("state does not exist in the graph identified by playback_path.");
					return;
				}
				if (i + 1 < state.get_slice_count("/")) {
					current_graph = current_graph->get_child_by_name(name);
					if (current_graph.is_null() || current_graph->get_state_machine_type() != AnimationNodeStateMachine::STATE_MACHINE_TYPE_GROUPED) {
						invalid("Only grouped states can be traversed through a parent state path; use the nested machine's playback_path instead.");
						return;
					}
				}
			}
		}
	} else if (action == "set_active") {
		if (!p_params.has("active")) {
			invalid("set_active requires active.");
			return;
		}
	} else {
		parameter = p_params.get("parameter", String());
		if (!_animation_path(parameter)) {
			invalid("parameter must be a canonical parameters/* property or graph prefix.");
			return;
		}
		if (oneshot_action) {
			const Ref<AnimationNodeOneShot> graph = _animation_graph_node(tree, parameter);
			if (graph.is_null()) {
				invalid("OneShot parameter must identify an AnimationNodeOneShot graph prefix, for example parameters/Attack.");
				return;
			}
			parameter += "/request";
			parameter_value = action == "oneshot_fire" ? AnimationNodeOneShot::ONE_SHOT_REQUEST_FIRE : (action == "oneshot_abort" ? AnimationNodeOneShot::ONE_SHOT_REQUEST_ABORT : AnimationNodeOneShot::ONE_SHOT_REQUEST_FADE_OUT);
		} else if (action == "transition_request") {
			const Ref<AnimationNodeTransition> graph = _animation_graph_node(tree, parameter);
			const String transition = p_params.get("transition", String());
			if (graph.is_null() || transition.is_empty() || graph->find_input(transition) < 0) {
				invalid("transition must identify an existing input of the AnimationNodeTransition graph prefix.");
				return;
			}
			parameter += "/transition_request";
			parameter_value = transition;
		}
		PropertyInfo info;
		StringName parameter_name;
		const Ref<AnimationNode> graph = _animation_parameter_node(tree, parameter, parameter_name);
		if (!_animation_property(tree, parameter, info) || graph.is_null() || info.type == Variant::OBJECT ||
				(info.usage & PROPERTY_USAGE_READ_ONLY) || graph->is_parameter_read_only(parameter_name)) {
			invalid("parameter must identify a known writable parameters/* value; playback, observer and read-only values cannot be replaced.");
			return;
		}
		if (action == "set_parameter") {
			Variant bounded;
			if (!p_params.has("value") || !_serialize_variant(p_params["value"], bounded)) {
				invalid("set_parameter requires a bounded serializable value (depth 8, 2000 items, 256 Ki characters).");
				return;
			}
			const Variant existing = tree->get(parameter);
			const Variant::Type type = info.type == Variant::NIL ? existing.get_type() : info.type;
			if (!_animation_decode_value(p_params["value"], type, parameter_value, 0, existing)) {
				invalid("value does not match the existing parameter type " + Variant::get_type_name(type) + ". Numeric components must be finite and integer components integral.");
				return;
			}
			// Do not bypass the graph-specific bounds on request parameters via set_parameter.
			if (parameter.get_file() == "request" && Object::cast_to<AnimationNodeOneShot>(graph.ptr()) &&
					((int64_t)parameter_value < AnimationNodeOneShot::ONE_SHOT_REQUEST_NONE || (int64_t)parameter_value > AnimationNodeOneShot::ONE_SHOT_REQUEST_FADE_OUT)) {
				invalid("OneShot request must be in range 0..3.");
				return;
			}
			const Ref<AnimationNodeTransition> transition_graph = graph;
			if (parameter.get_file() == "transition_request" && transition_graph.is_valid() &&
					!String(parameter_value).is_empty() && transition_graph->find_input(String(parameter_value)) < 0) {
				invalid("value must identify an existing transition input, or be empty to clear the request.");
				return;
			}
			if (info.hint == PROPERTY_HINT_ENUM && parameter_value.get_type() == Variant::INT && !info.hint_string.is_empty()) {
				const PackedStringArray choices = info.hint_string.split(",", true);
				int64_t next_value = 0;
				bool found = false;
				for (const String &choice : choices) {
					const int separator = choice.rfind(":");
					if (separator >= 0 && choice.substr(separator + 1).is_valid_int()) {
						next_value = choice.substr(separator + 1).to_int();
					}
					if ((int64_t)parameter_value == next_value) {
						found = true;
						break;
					}
					if (next_value < INT64_MAX) {
						next_value++;
					}
				}
				if (!found) {
					invalid("value is not one of the parameter's declared enum values.");
					return;
				}
			}
			if (info.hint == PROPERTY_HINT_RANGE && parameter_value.is_num()) {
				const PackedStringArray range = info.hint_string.split(",");
				if (range.size() >= 2 && range[0].is_valid_float() && range[1].is_valid_float()) {
					const double number = parameter_value;
					if ((number < range[0].to_float() && !range.has("or_less")) || (number > range[1].to_float() && !range.has("or_greater"))) {
						invalid("value is outside the parameter's declared range.");
						return;
					}
				}
			}
		}
	}

	// All validation precedes the first call into gameplay animation code.
	if (action == "player_play") {
		player->play(animation, custom_blend, (float)custom_speed, p_params.get("from_end", false));
	} else if (action == "player_stop") {
		player->stop(p_params.get("keep_state", false));
	} else if (action == "player_pause") {
		player->pause();
	} else if (action == "player_seek") {
		player->seek(time, p_params.get("update", true));
	} else if (action == "player_queue") {
		player->queue(animation);
	} else if (action == "player_advance") {
		player->advance(delta);
	} else if (action == "state_start") {
		playback->start(state, p_params.get("reset", true));
	} else if (action == "state_travel") {
		playback->travel(state, p_params.get("reset", true));
	} else if (action == "state_stop") {
		playback->stop();
	} else if (action == "state_next") {
		playback->next();
	} else if (action == "set_active") {
		tree->set_active(p_params["active"]);
	} else {
		bool valid = false;
		tree->set(parameter, parameter_value, &valid);
		if (!valid) {
			invalid("Godot rejected the runtime animation parameter value.");
			return;
		}
	}

	// Animation method tracks/signals may have freed the target during a seek,
	// advance or stop. Never dereference the old pointer to take the snapshot.
	node = Object::cast_to<Node>(ObjectDB::get_instance(target_id));
	Dictionary data;
	if (node && node->is_inside_tree()) {
		data = _animation_snapshot(node, playback_path, parameter_names, limit);
	}
	if (data.is_empty()) {
		data["run_id"] = run_id;
		data["node_path"] = p_params["node_path"];
		data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
		data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
		data["snapshot_available"] = false;
		data["target_freed"] = !node;
	}
	data["action"] = action;
	data["applied"] = true;
	data["deferred"] = state_action || oneshot_action || action == "transition_request" ||
			(action == "set_parameter" && (parameter.get_file() == "request" || parameter.get_file() == "transition_request"));
	_send_response(p_request_id, "animation_control", true, data);
}

void GodotMCPRuntimeDebugger::_handle_input_action(const String &p_request_id, const Dictionary &p_params) {
	const StringName action = p_params.get("action", StringName());
	if (action.is_empty() || !InputMap::get_singleton()->has_action(action)) {
		_send_error(p_request_id, "input_action", "NOT_FOUND", "Input action is not defined in InputMap.");
		return;
	}
	const bool pressed = bool(p_params.get("pressed", true));
	const float strength = CLAMP((float)p_params.get("strength", 1.0), 0.0f, 1.0f);
	const int hold_frames = (int)p_params.get("hold_frames", 0);
	if (hold_frames < 0 || hold_frames > MAX_FRAME_WAIT) {
		_send_error(p_request_id, "input_action", "INVALID_ARGUMENT", "hold_frames must be in range 0..600.");
		return;
	}
	if (pressed && hold_frames > 0) {
		for (const PendingRequest &pending : pending_requests) {
			if (pending.step_paused) {
				_send_error(p_request_id, "input_action", "BUSY", "A paused runtime step is already pending.", true);
				return;
			}
		}
	}
	if (pressed) {
		Input::get_singleton()->action_press(action, strength);
	} else {
		Input::get_singleton()->action_release(action);
	}
	if (pressed && hold_frames > 0) {
		if ((int)pending_requests.size() >= MAX_PENDING_REQUESTS) {
			Input::get_singleton()->action_release(action);
			_send_error(p_request_id, "input_action", "BUSY", "Too many pending runtime operations.", true);
			return;
		}
		PendingRequest pending;
		pending.kind = PENDING_ACTION_RELEASE;
		pending.request_id = p_request_id;
		pending.operation = "input_action";
		pending.physics = true;
		pending.target_frame = Engine::get_singleton()->get_physics_frames() + hold_frames;
		pending.action = action;
		SceneTree *tree = SceneTree::get_singleton();
		if (tree && tree->is_suspended()) {
			pending.step_paused = true;
			pending.frames_remaining = hold_frames;
			// Runtime selection disables Input polling while suspended. Enable
			// the explicit automation action during its steps and restore the
			// previous debugger input state when the hold completes.
			pending.input_disabled_before_step = Input::get_singleton()->is_input_disabled();
			Input::get_singleton()->set_disable_input(false);
			tree->set_suspend(false);
		}
		pending_requests.push_back(pending);
		return;
	}
	Dictionary data;
	data["action"] = action;
	data["pressed"] = pressed;
	data["strength"] = strength;
	data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
	_send_response(p_request_id, "input_action", true, data);
}

void GodotMCPRuntimeDebugger::_handle_input_event(const String &p_request_id, const Dictionary &p_params) {
	const String type = String(p_params.get("type", String())).to_lower();
	// Account for earlier queued presses/releases before composing the mask.
	Input::get_singleton()->flush_buffered_events();
	Ref<InputEvent> event;
	if (type == "mouse_motion") {
		Ref<InputEventMouseMotion> motion;
		motion.instantiate();
		Vector2 position;
		Vector2 relative;
		if (p_params.has("position") && !_variant_to_vector2(p_params["position"], position)) {
			_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "position must be a Vector2, [x,y], or {x,y}.");
			return;
		}
		if (p_params.has("relative") && !_variant_to_vector2(p_params["relative"], relative)) {
			_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "relative must be a Vector2, [x,y], or {x,y}.");
			return;
		}
		motion->set_position(position);
		motion->set_global_position(position);
		motion->set_relative(relative);
		motion->set_button_mask(Input::get_singleton()->get_mouse_button_mask());
		event = motion;
	} else if (type == "mouse_button") {
		const int button = (int)p_params.get("button_index", 0);
		if (button < (int)MouseButton::LEFT || button > (int)MouseButton::MB_XBUTTON2) {
			_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "button_index is outside the supported mouse button range.");
			return;
		}
		Ref<InputEventMouseButton> mouse_button;
		mouse_button.instantiate();
		Vector2 position;
		if (p_params.has("position") && !_variant_to_vector2(p_params["position"], position)) {
			_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "position must be a Vector2, [x,y], or {x,y}.");
			return;
		}
		mouse_button->set_position(position);
		mouse_button->set_global_position(position);
		mouse_button->set_button_index((MouseButton)button);
		const bool pressed = bool(p_params.get("pressed", true));
		mouse_button->set_pressed(pressed);
		BitField<MouseButtonMask> button_mask = Input::get_singleton()->get_mouse_button_mask();
		if (pressed) {
			button_mask.set_flag(mouse_button_to_mask((MouseButton)button));
		} else {
			button_mask.clear_flag(mouse_button_to_mask((MouseButton)button));
		}
		mouse_button->set_button_mask(button_mask);
		mouse_button->set_double_click(bool(p_params.get("double_click", false)));
		event = mouse_button;
	} else {
		_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "type must be mouse_motion or mouse_button.");
		return;
	}
	Input::get_singleton()->parse_input_event(event);
	Input::get_singleton()->flush_buffered_events();
	Dictionary data;
	data["type"] = type;
	data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	_send_response(p_request_id, "input_event", true, data);
}

void GodotMCPRuntimeDebugger::_handle_wait_frames(const String &p_request_id, const Dictionary &p_params) {
	const int frames = (int)p_params.get("frames", 1);
	const String frame_type = String(p_params.get("frame_type", String("physics"))).to_lower();
	if (frames < 1 || frames > MAX_FRAME_WAIT || (frame_type != "physics" && frame_type != "process")) {
		_send_error(p_request_id, "wait_frames", "INVALID_ARGUMENT", "frames must be 1..600 and frame_type must be physics or process.");
		return;
	}
	if ((int)pending_requests.size() >= MAX_PENDING_REQUESTS) {
		_send_error(p_request_id, "wait_frames", "BUSY", "Too many pending runtime operations.", true);
		return;
	}
	for (const PendingRequest &pending : pending_requests) {
		if (pending.step_paused) {
			_send_error(p_request_id, "wait_frames", "BUSY", "A paused runtime step is already pending.", true);
			return;
		}
	}
	PendingRequest pending;
	pending.kind = PENDING_WAIT_FRAMES;
	pending.request_id = p_request_id;
	pending.operation = "wait_frames";
	pending.physics = frame_type == "physics";
	uint64_t current_frame;
	if (pending.physics) {
		current_frame = Engine::get_singleton()->get_physics_frames();
	} else {
		current_frame = Engine::get_singleton()->get_process_frames();
	}
	pending.target_frame = current_frame + frames;
	pending.params = p_params;
	SceneTree *tree = SceneTree::get_singleton();
	if (tree && tree->is_suspended()) {
		pending.step_paused = true;
		pending.frames_remaining = frames;
		tree->set_suspend(false);
	}
	pending_requests.push_back(pending);
}

void GodotMCPRuntimeDebugger::_handle_capture(const String &p_request_id, const Dictionary &p_params) {
	const int frame_wait = (int)p_params.get("frame_wait", 0);
	if (frame_wait < 0 || frame_wait > MAX_FRAME_WAIT) {
		_send_error(p_request_id, "capture_viewport", "INVALID_ARGUMENT", "frame_wait must be in range 0..600.");
		return;
	}
	if (frame_wait == 0) {
		_capture_now(p_request_id, p_params);
		return;
	}
	if ((int)pending_requests.size() >= MAX_PENDING_REQUESTS) {
		_send_error(p_request_id, "capture_viewport", "BUSY", "Too many pending runtime operations.", true);
		return;
	}
	PendingRequest pending;
	pending.kind = PENDING_CAPTURE;
	pending.request_id = p_request_id;
	pending.operation = "capture_viewport";
	pending.physics = false;
	pending.target_frame = Engine::get_singleton()->get_process_frames() + frame_wait;
	pending.params = p_params;
	pending_requests.push_back(pending);
}

void GodotMCPRuntimeDebugger::_capture_now(const String &p_request_id, const Dictionary &p_params) {
	const String selector = p_params.get("viewport", String("/root"));
	Node *node = _resolve_node(selector);
	Viewport *viewport = Object::cast_to<Viewport>(node);
	if (!viewport) {
		_send_error(p_request_id, "capture_viewport", "NOT_FOUND", "viewport must identify a runtime Viewport node path.");
		return;
	}
	Ref<ViewportTexture> texture = viewport->get_texture();
	Ref<Image> image = texture.is_valid() ? texture->get_image() : Ref<Image>();
	if (image.is_null() || image->is_empty()) {
		_send_error(p_request_id, "capture_viewport", "CAPTURE_FAILED", "Viewport texture could not be read.", true);
		return;
	}
	image->clear_mipmaps();
	if (p_params.has("crop")) {
		Dictionary crop = p_params["crop"];
		Rect2i region(
				(int)crop.get("x", 0), (int)crop.get("y", 0),
				(int)crop.get("width", image->get_width()), (int)crop.get("height", image->get_height()));
		region = region.intersection(Rect2i(0, 0, image->get_width(), image->get_height()));
		if (region.size.x <= 0 || region.size.y <= 0) {
			_send_error(p_request_id, "capture_viewport", "INVALID_ARGUMENT", "crop does not intersect the viewport image.");
			return;
		}
		image = image->get_region(region);
	}
	const float scale = CLAMP((float)p_params.get("scale", 1.0), 0.05f, 4.0f);
	const int target_width = MIN(MAX_CAPTURE_DIMENSION, MAX(1, (int)Math::round(image->get_width() * scale)));
	const int target_height = MIN(MAX_CAPTURE_DIMENSION, MAX(1, (int)Math::round(image->get_height() * scale)));
	if (target_width != image->get_width() || target_height != image->get_height()) {
		image->resize(target_width, target_height, Image::INTERPOLATE_LANCZOS);
	}
	image->convert(Image::FORMAT_RGBA8);
	const String path = OS::get_singleton()->get_temp_path().path_join("godot-mcp-" + run_id.validate_filename() + "-" +
			uitos(Time::get_singleton()->get_ticks_usec()) + ".png");
	const Error save_error = image->save_png(path);
	if (save_error != OK) {
		Dictionary details;
		details["godot_error"] = (int)save_error;
		_send_error(p_request_id, "capture_viewport", "CAPTURE_FAILED",
				"Failed to save viewport image (Godot error " + itos(save_error) + ").", false, details);
		return;
	}
	Dictionary data;
	data["path"] = path;
	data["width"] = image->get_width();
	data["height"] = image->get_height();
	data["capture_timestamp_msec"] = (int64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
	data["frame_number"] = (int64_t)Engine::get_singleton()->get_process_frames();
	data["viewport"] = selector;
	_send_response(p_request_id, "capture_viewport", true, data);
}

void GodotMCPRuntimeDebugger::_process_frame() {
	_complete_pending(false);
}

void GodotMCPRuntimeDebugger::_physics_frame() {
	_complete_pending(true);
}

void GodotMCPRuntimeDebugger::_complete_pending(bool p_physics) {
	const uint64_t current_frame = p_physics ? Engine::get_singleton()->get_physics_frames() : Engine::get_singleton()->get_process_frames();
	for (int i = (int)pending_requests.size() - 1; i >= 0; i--) {
		PendingRequest &entry = pending_requests[i];
		if (entry.physics != p_physics) {
			continue;
		}
		if (entry.step_paused) {
			if (!entry.completion_scheduled && --entry.frames_remaining == 0) {
				entry.completion_scheduled = true;
				const Callable finish = callable_mp(this, &GodotMCPRuntimeDebugger::_finish_frame_request).bind(entry.request_id);
				// Physics signals precede node callbacks; deferred work runs after
				// them. Process signals flush deferred work before node callbacks,
				// so process steps finish only after the rendered frame instead.
				if (p_physics) {
					finish.call_deferred();
				} else {
					RenderingServer::get_singleton()->connect(SNAME("frame_post_draw"), finish, Object::CONNECT_ONE_SHOT);
				}
			}
			continue;
		}
		const PendingRequest pending = entry;
		if (pending.physics != p_physics || current_frame < pending.target_frame) {
			continue;
		}
		if (pending.kind == PENDING_ACTION_RELEASE) {
			// Release only after the final requested physics callback has read
			// the pressed action, including ordinary running games.
			if (!entry.completion_scheduled) {
				entry.completion_scheduled = true;
				callable_mp(this, &GodotMCPRuntimeDebugger::_finish_frame_request).bind(entry.request_id).call_deferred();
			}
			continue;
		}
		pending_requests.remove_at(i);
		if (pending.kind == PENDING_CAPTURE) {
			_capture_now(pending.request_id, pending.params);
		} else {
			Dictionary data;
			data["frame_type"] = pending.physics ? "physics" : "process";
			data["frame_number"] = (int64_t)current_frame;
			data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
			data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
			_send_response(pending.request_id, pending.operation, true, data);
		}
	}
}

void GodotMCPRuntimeDebugger::_finish_frame_request(const String &p_request_id) {
	for (int i = (int)pending_requests.size() - 1; i >= 0; i--) {
		const PendingRequest pending = pending_requests[i];
		if (pending.request_id != p_request_id || !pending.completion_scheduled) {
			continue;
		}
		pending_requests.remove_at(i);
		SceneTree *tree = SceneTree::get_singleton();
		if (tree && pending.step_paused) {
			tree->set_suspend(true);
		}
		Dictionary data;
		if (pending.kind == PENDING_ACTION_RELEASE) {
			Input::get_singleton()->action_release(pending.action);
			if (pending.step_paused) {
				Input::get_singleton()->set_disable_input(pending.input_disabled_before_step);
			}
			data["action"] = pending.action;
			data["pressed"] = false;
			data["held_until_physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
		} else {
			data["frame_type"] = pending.physics ? "physics" : "process";
			data["frame_number"] = (int64_t)(pending.physics ? Engine::get_singleton()->get_physics_frames() : Engine::get_singleton()->get_process_frames());
		}
		data["process_frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
		data["physics_frame"] = (int64_t)Engine::get_singleton()->get_physics_frames();
		_send_response(pending.request_id, pending.operation, true, data);
		return;
	}
}
