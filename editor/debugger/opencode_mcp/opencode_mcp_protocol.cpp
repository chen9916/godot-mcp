/**************************************************************************/
/*  opencode_mcp_protocol.cpp                                              */
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

#include "opencode_mcp_protocol.h"

#include "core/config/project_settings.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/keyboard.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/version.h"
#include "core/variant/callable.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/object/undo_redo.h"
#include "core/templates/vector.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/settings/project_settings_editor.h"
#include "editor/script/script_editor_base.h"
#include "editor/script/script_editor_plugin.h"
#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/language_server/gdscript_extend_parser.h"
#endif
#include "scene/3d/node_3d.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/shader.h"

namespace {
struct TextEditRange {
	int from = 0;
	int to = 0;
	String text;
};

constexpr const char *SCRIPT_WORKSPACE_AUTO = "auto";
constexpr const char *SCRIPT_WORKSPACE_SCENE_VIEW = "scene_view";
constexpr const char *SCRIPT_WORKSPACE_SCRIPT_EDITOR = "script_editor";

static bool _is_identifier_char(char32_t p_char) {
	return p_char == '_' || (p_char >= '0' && p_char <= '9') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= 'a' && p_char <= 'z');
}

static void _index_to_line_col(const String &p_text, int p_index, int &r_line, int &r_col) {
	r_line = 0;
	r_col = 0;
	if (p_index <= 0) {
		return;
	}

	const int text_length = p_text.length();
	const int end = p_index < text_length ? p_index : text_length;
	for (int i = 0; i < end; i++) {
		if (p_text[i] == '\n') {
			r_line++;
			r_col = 0;
		} else {
			r_col++;
		}
	}
}

static void _append_identifier_occurrences(const String &p_source, const String &p_identifier, const String &p_script_path, Array &r_locations) {
	if (p_identifier.is_empty()) {
		return;
	}

	int search_from = 0;
	while (true) {
		const int match_index = p_source.find(p_identifier, search_from);
		if (match_index < 0) {
			break;
		}

		const int before_index = match_index - 1;
		const int after_index = match_index + p_identifier.length();
		const bool has_valid_before = before_index < 0 || !_is_identifier_char(p_source[before_index]);
		const bool has_valid_after = after_index >= p_source.length() || !_is_identifier_char(p_source[after_index]);
		if (has_valid_before && has_valid_after) {
			int start_line = 0;
			int start_col = 0;
			int end_line = 0;
			int end_col = 0;
			_index_to_line_col(p_source, match_index, start_line, start_col);
			_index_to_line_col(p_source, after_index, end_line, end_col);

			Dictionary location;
			location["filePath"] = p_script_path;
			location["line"] = start_line;
			location["character"] = start_col;
			location["end_line"] = end_line;
			location["end_character"] = end_col;
			r_locations.push_back(location);
		}

		search_from = match_index + p_identifier.length();
	}
}

static String _lookup_result_type_to_string(ScriptLanguage::LookupResultType p_type) {
	switch (p_type) {
		case ScriptLanguage::LOOKUP_RESULT_SCRIPT_LOCATION:
			return "script_location";
		case ScriptLanguage::LOOKUP_RESULT_CLASS:
			return "class";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_CONSTANT:
			return "class_constant";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY:
			return "class_property";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD:
			return "class_method";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_SIGNAL:
			return "class_signal";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_ENUM:
			return "class_enum";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_TBD_GLOBALSCOPE:
			return "class_global";
		case ScriptLanguage::LOOKUP_RESULT_CLASS_ANNOTATION:
			return "class_annotation";
		case ScriptLanguage::LOOKUP_RESULT_LOCAL_CONSTANT:
			return "local_constant";
		case ScriptLanguage::LOOKUP_RESULT_LOCAL_VARIABLE:
			return "local_variable";
		case ScriptLanguage::LOOKUP_RESULT_MAX:
			return "unknown";
	}

	return "unknown";
}

static ScriptLanguage *_find_script_language(const String &p_language_name) {
	const String normalized = p_language_name.to_lower();
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		ScriptLanguage *language = ScriptServer::get_language(i);
		if (!language) {
			continue;
		}
		if (language->get_name().to_lower() == normalized) {
			return language;
		}
	}
	return nullptr;
}

static bool _variant_to_keycode(const Variant &p_value, Key &r_keycode) {
	if (p_value.get_type() == Variant::INT) {
		r_keycode = (Key)(int64_t)p_value;
		return true;
	}
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String key_text = String(p_value).strip_edges();
		if (key_text.is_empty()) {
			return false;
		}
		r_keycode = find_keycode(key_text);
		if (r_keycode == Key::NONE) {
			r_keycode = find_keycode(key_text.to_upper());
		}
		return r_keycode != Key::NONE;
	}
	return false;
}

static Ref<InputEvent> _coerce_input_event(const Variant &p_value, String &r_error_message) {
	if (p_value.get_type() == Variant::OBJECT) {
		Ref<InputEvent> event = p_value;
		if (event.is_null()) {
			r_error_message = "input event object is null";
		}
		return event;
	}

	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		Key keycode = Key::NONE;
		if (!_variant_to_keycode(p_value, keycode)) {
			r_error_message = "invalid key string in input event";
			return Ref<InputEvent>();
		}
		Ref<InputEventKey> key_event;
		key_event.instantiate();
		key_event->set_keycode(keycode);
		key_event->set_pressed(true);
		return key_event;
	}

	if (p_value.get_type() != Variant::DICTIONARY) {
		r_error_message = "input events must be InputEvent objects, dictionaries, or key strings";
		return Ref<InputEvent>();
	}

	Dictionary event_dict = p_value;
	String event_class = String(event_dict.get("class", event_dict.get("event_class", String())));
	if (event_class.is_empty()) {
		event_class = String(event_dict.get("class_name", event_dict.get("__class__", event_dict.get("_class", String()))));
	}
	if (event_class.is_empty()) {
		const String event_type = String(event_dict.get("type", String())).to_lower();
		if (event_type == "key" || event_type == "keyboard" || event_type == "inputeventkey") {
			event_class = "InputEventKey";
		} else if (event_type == "mouse_button" || event_type == "inputeventmousebutton") {
			event_class = "InputEventMouseButton";
		} else if (event_type == "joypad_button" || event_type == "inputeventjoypadbutton") {
			event_class = "InputEventJoypadButton";
		} else if (event_type == "joypad_motion" || event_type == "inputeventjoypadmotion") {
			event_class = "InputEventJoypadMotion";
		}
	}

	if (event_class.is_empty()) {
		if (event_dict.has("key") || event_dict.has("keycode") || event_dict.has("key_label") || event_dict.has("unicode") ||
				event_dict.has("physical_key") || event_dict.has("physical_keycode") || event_dict.has("scancode") ||
				event_dict.has("physical_scancode")) {
			event_class = "InputEventKey";
		} else if (event_dict.has("axis") || event_dict.has("axis_value")) {
			event_class = "InputEventJoypadMotion";
		} else if (event_dict.has("button_index")) {
			if (event_dict.has("position") || event_dict.has("global_position") || event_dict.has("double_click")) {
				event_class = "InputEventMouseButton";
			} else {
				event_class = "InputEventJoypadButton";
			}
		}
	}

	if (event_class.is_empty()) {
		r_error_message = "input event dictionary is missing class/type";
		return Ref<InputEvent>();
	}

	Object *obj = ClassDB::instantiate(event_class);
	InputEvent *event_ptr = Object::cast_to<InputEvent>(obj);
	if (!event_ptr) {
		if (obj) {
			memdelete(obj);
		}
		r_error_message = "unsupported input event class: " + event_class;
		return Ref<InputEvent>();
	}

	Ref<InputEvent> event(event_ptr);
	for (const KeyValue<Variant, Variant> &E : event_dict) {
		String property = String(E.key);
		if (property == "class" || property == "event_class" || property == "class_name" || property == "__class__" || property == "_class" || property == "type") {
			continue;
		}

		if (property == "key") {
			property = "keycode";
		} else if (property == "physical_key") {
			property = "physical_keycode";
		} else if (property == "scancode") {
			property = "keycode";
		} else if (property == "physical_scancode") {
			property = "physical_keycode";
		}

		Variant value = E.value;
		if (event_class == "InputEventKey" && (property == "keycode" || property == "physical_keycode" || property == "key_label")) {
			Key keycode = Key::NONE;
			if (!_variant_to_keycode(value, keycode)) {
				r_error_message = "invalid key value for property: " + property;
				return Ref<InputEvent>();
			}
			value = (int64_t)keycode;
		}

		bool valid = false;
		event->get(property, &valid);
		if (!valid) {
			r_error_message = "unknown input event property: " + property;
			return Ref<InputEvent>();
		}

		event->set(property, value);
	}

	if (event_class == "InputEventKey" && !event_dict.has("pressed")) {
		event->set("pressed", true);
	}
	if ((event_class == "InputEventMouseButton" || event_class == "InputEventJoypadButton") && !event_dict.has("pressed")) {
		event->set("pressed", true);
	}

	return event;
}

static bool _variant_to_real_number(const Variant &p_value, real_t &r_value) {
	if (p_value.get_type() == Variant::FLOAT) {
		r_value = (real_t)(double)p_value;
		return true;
	}
	if (p_value.get_type() == Variant::INT) {
		r_value = (real_t)(int64_t)p_value;
		return true;
	}
	return false;
}

static bool _coerce_variant_to_vector3(const Variant &p_value, Vector3 &r_vector) {
	if (p_value.get_type() == Variant::VECTOR3) {
		r_vector = p_value;
		return true;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() != 3) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		real_t z = 0;
		if (!_variant_to_real_number(arr[0], x) || !_variant_to_real_number(arr[1], y) || !_variant_to_real_number(arr[2], z)) {
			return false;
		}
		r_vector = Vector3(x, y, z);
		return true;
	}

	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = p_value;
		if (!dict.has("x") || !dict.has("y") || !dict.has("z")) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		real_t z = 0;
		if (!_variant_to_real_number(dict["x"], x) || !_variant_to_real_number(dict["y"], y) || !_variant_to_real_number(dict["z"], z)) {
			return false;
		}
		r_vector = Vector3(x, y, z);
		return true;
	}

	return false;
}

static bool _coerce_variant_to_vector2(const Variant &p_value, Vector2 &r_vector) {
	if (p_value.get_type() == Variant::VECTOR2) {
		r_vector = p_value;
		return true;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() != 2) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		if (!_variant_to_real_number(arr[0], x) || !_variant_to_real_number(arr[1], y)) {
			return false;
		}
		r_vector = Vector2(x, y);
		return true;
	}

	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = p_value;
		if (!dict.has("x") || !dict.has("y")) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		if (!_variant_to_real_number(dict["x"], x) || !_variant_to_real_number(dict["y"], y)) {
			return false;
		}
		r_vector = Vector2(x, y);
		return true;
	}

	return false;
}

static bool _coerce_variant_to_vector4(const Variant &p_value, Vector4 &r_vector) {
	if (p_value.get_type() == Variant::VECTOR4) {
		r_vector = p_value;
		return true;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() != 4) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		real_t z = 0;
		real_t w = 0;
		if (!_variant_to_real_number(arr[0], x) || !_variant_to_real_number(arr[1], y) || !_variant_to_real_number(arr[2], z) || !_variant_to_real_number(arr[3], w)) {
			return false;
		}
		r_vector = Vector4(x, y, z, w);
		return true;
	}

	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = p_value;
		if (!dict.has("x") || !dict.has("y") || !dict.has("z") || !dict.has("w")) {
			return false;
		}

		real_t x = 0;
		real_t y = 0;
		real_t z = 0;
		real_t w = 0;
		if (!_variant_to_real_number(dict["x"], x) || !_variant_to_real_number(dict["y"], y) || !_variant_to_real_number(dict["z"], z) || !_variant_to_real_number(dict["w"], w)) {
			return false;
		}
		r_vector = Vector4(x, y, z, w);
		return true;
	}

	return false;
}

static bool _coerce_variant_to_color(const Variant &p_value, Color &r_color) {
	const auto parse_color_literal = [](const String &p_text, Color &r_out_color) {
		String text = p_text.strip_edges();
		if (!text.begins_with("Color(") || !text.ends_with(")")) {
			return false;
		}

		const String args_text = text.substr(6, text.length() - 7).strip_edges();
		if (args_text.is_empty()) {
			return false;
		}

		PackedStringArray components = args_text.split(",", false);
		if (components.size() != 3 && components.size() != 4) {
			return false;
		}

		real_t channels[4] = { 0.0, 0.0, 0.0, 1.0 };
		for (int i = 0; i < components.size(); i++) {
			const String component = components[i].strip_edges();
			if (!component.is_valid_float()) {
				return false;
			}
			channels[i] = component.to_float();
		}

		r_out_color = Color(channels[0], channels[1], channels[2], channels[3]);
		return true;
	};

	if (p_value.get_type() == Variant::COLOR) {
		r_color = p_value;
		return true;
	}

	if (p_value.get_type() == Variant::STRING) {
		String text = String(p_value).strip_edges();
		if (parse_color_literal(text, r_color)) {
			return true;
		}
		if (!Color::html_is_valid(text)) {
			return false;
		}
		r_color = Color::html(text);
		return true;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() != 3 && arr.size() != 4) {
			return false;
		}

		real_t r = 0;
		real_t g = 0;
		real_t b = 0;
		real_t a = 1.0;
		if (!_variant_to_real_number(arr[0], r) || !_variant_to_real_number(arr[1], g) || !_variant_to_real_number(arr[2], b)) {
			return false;
		}
		if (arr.size() == 4 && !_variant_to_real_number(arr[3], a)) {
			return false;
		}
		r_color = Color(r, g, b, a);
		return true;
	}

	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = p_value;
		if (!dict.has("r") || !dict.has("g") || !dict.has("b")) {
			return false;
		}

		real_t r = 0;
		real_t g = 0;
		real_t b = 0;
		real_t a = 1.0;
		if (!_variant_to_real_number(dict["r"], r) || !_variant_to_real_number(dict["g"], g) || !_variant_to_real_number(dict["b"], b)) {
			return false;
		}
		if (dict.has("a") && !_variant_to_real_number(dict["a"], a)) {
			return false;
		}
		r_color = Color(r, g, b, a);
		return true;
	}

	return false;
}

static bool _is_resource_compatible(const Ref<Resource> &p_resource, const String &p_expected_resource_type) {
	if (p_resource.is_null() || p_expected_resource_type.is_empty()) {
		return true;
	}

	PackedStringArray accepted_types = p_expected_resource_type.split(",");
	for (int i = 0; i < accepted_types.size(); i++) {
		String type_name = accepted_types[i].strip_edges();
		if (!type_name.is_empty() && p_resource->is_class(type_name)) {
			return true;
		}
	}
	return false;
}

static bool _coerce_variant_to_resource_ref(const Variant &p_value, const String &p_expected_resource_type, Variant &r_coerced_value) {
	if (p_value.get_type() == Variant::NIL) {
		r_coerced_value = Variant();
		return true;
	}

	if (p_value.get_type() == Variant::OBJECT) {
		Ref<Resource> resource = p_value;
		if (resource.is_valid() && _is_resource_compatible(resource, p_expected_resource_type)) {
			r_coerced_value = resource;
			return true;
		}
		return false;
	}

	String resource_ref;
	if (p_value.get_type() == Variant::STRING) {
		resource_ref = String(p_value).strip_edges();
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = p_value;
		if (dict.has("path")) {
			resource_ref = String(dict["path"]).strip_edges();
		} else if (dict.has("uid")) {
			resource_ref = String(dict["uid"]).strip_edges();
		} else {
			return false;
		}
	}

	if (resource_ref.is_empty()) {
		return false;
	}

	if (resource_ref.begins_with("uid://")) {
		ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(resource_ref);
		if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
			resource_ref = ResourceUID::get_singleton()->get_id_path(uid);
		}
	}

	Ref<Resource> resource = ResourceLoader::load(resource_ref);
	if (resource.is_null() || !_is_resource_compatible(resource, p_expected_resource_type)) {
		return false;
	}

	r_coerced_value = resource;
	return true;
}

static String _accepted_shapes_hint(Variant::Type p_expected_type, const String &p_expected_resource_type) {
	if (p_expected_type == Variant::VECTOR2) {
		return "Vector2 | [x,y] | {x:num, y:num}";
	}
	if (p_expected_type == Variant::VECTOR3) {
		return "Vector3 | [x,y,z] | {x:num, y:num, z:num}";
	}
	if (p_expected_type == Variant::VECTOR4) {
		return "Vector4 | [x,y,z,w] | {x:num, y:num, z:num, w:num}";
	}
	if (p_expected_type == Variant::COLOR) {
		return "Color | Color(r,g,b[,a]) | '#RRGGBB'/'#RRGGBBAA' | [r,g,b,a?] | {r:num,g:num,b:num,a?:num}";
	}
	if (p_expected_type == Variant::OBJECT && !p_expected_resource_type.is_empty()) {
		return "ResourceRef path string (res:// or uid://) | {path:string} | {uid:string} | null";
	}
	return String();
}

static String _build_property_type_error(const String &p_property_name, Variant::Type p_expected_type, Variant::Type p_actual_type, const String &p_expected_resource_type) {
	String message = "INVALID_ARGUMENT: type mismatch for property write";
	if (!p_property_name.is_empty()) {
		message += " '" + p_property_name + "'";
	}
	message += " (expected " + Variant::get_type_name(p_expected_type);
	if (!p_expected_resource_type.is_empty()) {
		message += "<" + p_expected_resource_type + ">";
	}
	message += ", got " + Variant::get_type_name(p_actual_type) + ")";

	const String hint = _accepted_shapes_hint(p_expected_type, p_expected_resource_type);
	if (!hint.is_empty()) {
		message += ". Accepted shapes: " + hint + ".";
	}

	return message;
}

static String _resource_type_hint_for_property(const Object *p_object, const StringName &p_property_name) {
	if (!p_object) {
		return String();
	}

	List<PropertyInfo> plist;
	p_object->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.name != p_property_name) {
			continue;
		}
		if (pi.type == Variant::OBJECT && pi.hint == PROPERTY_HINT_RESOURCE_TYPE) {
			return pi.hint_string;
		}
		if (pi.type == Variant::OBJECT && p_property_name == SNAME("script")) {
			return "Script";
		}
		return String();
	}
	return String();
}

static bool _coerce_property_value(const Variant &p_existing_value, const Variant &p_requested_value,
		const String &p_expected_resource_type, const String &p_property_name, Variant &r_coerced_value, String &r_error_message) {
	Variant::Type expected_type = p_existing_value.get_type();
	if (expected_type == Variant::NIL && !p_expected_resource_type.is_empty()) {
		expected_type = Variant::OBJECT;
	}

	if (expected_type == p_requested_value.get_type()) {
		r_coerced_value = p_requested_value;
		if (expected_type == Variant::OBJECT && !p_expected_resource_type.is_empty()) {
			Ref<Resource> resource = r_coerced_value;
			if (!_is_resource_compatible(resource, p_expected_resource_type)) {
				r_error_message = _build_property_type_error(p_property_name, expected_type, p_requested_value.get_type(), p_expected_resource_type);
				return false;
			}
		}
		return true;
	}

	if (expected_type == Variant::VECTOR2) {
		Vector2 vector_value;
		if (_coerce_variant_to_vector2(p_requested_value, vector_value)) {
			r_coerced_value = vector_value;
			return true;
		}
	}

	if (expected_type == Variant::VECTOR3) {
		Vector3 vector_value;
		if (_coerce_variant_to_vector3(p_requested_value, vector_value)) {
			r_coerced_value = vector_value;
			return true;
		}
	}

	if (expected_type == Variant::VECTOR4) {
		Vector4 vector_value;
		if (_coerce_variant_to_vector4(p_requested_value, vector_value)) {
			r_coerced_value = vector_value;
			return true;
		}
	}

	if (expected_type == Variant::COLOR) {
		Color color_value;
		if (_coerce_variant_to_color(p_requested_value, color_value)) {
			r_coerced_value = color_value;
			return true;
		}
		r_error_message = _build_property_type_error(p_property_name, expected_type, p_requested_value.get_type(), p_expected_resource_type);
		return false;
	}

	if (expected_type == Variant::OBJECT && !p_expected_resource_type.is_empty()) {
		if (_coerce_variant_to_resource_ref(p_requested_value, p_expected_resource_type, r_coerced_value)) {
			return true;
		}
		r_error_message = _build_property_type_error(p_property_name, expected_type, p_requested_value.get_type(), p_expected_resource_type);
		return false;
	}

	if (Variant::can_convert(p_requested_value.get_type(), expected_type)) {
		Callable::CallError call_error;
		const Variant *args[1] = { &p_requested_value };
		Variant converted_value;
		Variant::construct(expected_type, converted_value, args, 1, call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			r_coerced_value = converted_value;
			return true;
		}
	}

	r_error_message = _build_property_type_error(p_property_name, expected_type, p_requested_value.get_type(), p_expected_resource_type);
	return false;
}

static bool _coerce_property_value(const Variant &p_existing_value, const Variant &p_requested_value, Variant &r_coerced_value, String &r_error_message) {
	return _coerce_property_value(p_existing_value, p_requested_value, String(), String(), r_coerced_value, r_error_message);
}

static bool _serialize_resource_reference(const Variant &p_value, Variant &r_serialized) {
	if (p_value.get_type() != Variant::OBJECT) {
		r_serialized = p_value;
		return true;
	}

	Object *object_value = p_value;
	if (!object_value) {
		r_serialized = Variant();
		return true;
	}

	Resource *resource_ptr = Object::cast_to<Resource>(object_value);
	if (!resource_ptr) {
		return false;
	}

	Ref<Resource> resource(resource_ptr);

	Dictionary ref_data;
	ref_data["class"] = resource->get_class();
	String path = resource->get_path();
	if (!path.is_empty()) {
		ref_data["path"] = path;
		String uid = ResourceUID::get_singleton()->path_to_uid(path);
		if (!uid.is_empty()) {
			ref_data["uid"] = uid;
		}
	}
	r_serialized = ref_data;
	return true;
}

static bool _collect_properties_from_payload(const Dictionary &p_payload, Dictionary &r_properties, String &r_error_message) {
	r_properties = p_payload.get("properties", Dictionary());

	Array property_entries = p_payload.get("property_entries", Array());
	for (int i = 0; i < property_entries.size(); i++) {
		if (property_entries[i].get_type() != Variant::DICTIONARY) {
			r_error_message = "INVALID_ARGUMENT: property_entries entries must be dictionaries.";
			return false;
		}

		Dictionary entry = property_entries[i];
		Variant property_name_variant = entry.get("name", entry.get("property", Variant()));
		if (property_name_variant.get_type() != Variant::STRING && property_name_variant.get_type() != Variant::STRING_NAME) {
			r_error_message = "INVALID_ARGUMENT: property_entries entries must include string name.";
			return false;
		}
		if (!entry.has("value")) {
			r_error_message = "INVALID_ARGUMENT: property_entries entries must include value.";
			return false;
		}

		r_properties[String(property_name_variant)] = entry["value"];
	}

	return true;
}

enum BatchMode {
	BATCH_MODE_ATOMIC,
	BATCH_MODE_BEST_EFFORT,
};

static String _batch_mode_to_string(BatchMode p_mode) {
	return p_mode == BATCH_MODE_BEST_EFFORT ? "best_effort" : "atomic";
}

static bool _parse_batch_mode(const Dictionary &p_params, BatchMode &r_mode, String &r_error_message) {
	const Variant mode_variant = p_params.get("mode", String("atomic"));
	if (mode_variant.get_type() != Variant::STRING && mode_variant.get_type() != Variant::STRING_NAME) {
		r_error_message = "INVALID_ARGUMENT: mode must be one of: atomic, best_effort.";
		return false;
	}

	const String mode = String(mode_variant);
	if (mode == "atomic") {
		r_mode = BATCH_MODE_ATOMIC;
		return true;
	}
	if (mode == "best_effort") {
		r_mode = BATCH_MODE_BEST_EFFORT;
		return true;
	}

	r_error_message = "INVALID_ARGUMENT: mode must be one of: atomic, best_effort.";
	return false;
}

static Dictionary _make_batch_item_error(int p_index, const String &p_item_id, int p_error_code, const String &p_error_message, const String &p_status = "failed") {
	Dictionary result;
	result["index"] = p_index;
	if (!p_item_id.is_empty()) {
		result["item_id"] = p_item_id;
	}
	result["ok"] = false;
	result["status"] = p_status;

	Dictionary error;
	error["code"] = p_error_code;
	error["message"] = p_error_message;
	result["error"] = error;
	return result;
}

static Dictionary _make_batch_item_success(int p_index, const String &p_item_id, const Dictionary &p_data, const String &p_status = "applied") {
	Dictionary result;
	result["index"] = p_index;
	if (!p_item_id.is_empty()) {
		result["item_id"] = p_item_id;
	}
	result["ok"] = true;
	result["status"] = p_status;
	result["data"] = p_data;
	return result;
}

static bool _collect_properties_from_payload_with_alias(const Dictionary &p_payload, const String &p_properties_key, Dictionary &r_properties, String &r_error_message) {
	Dictionary normalized = p_payload;
	if (p_properties_key != "properties" && p_payload.has(p_properties_key) && !p_payload.has("properties")) {
		normalized["properties"] = p_payload[p_properties_key];
	}
	return _collect_properties_from_payload(normalized, r_properties, r_error_message);
}

static bool _docs_all_terms_in_name(const Vector<String> &p_terms, const String &p_name) {
	for (int i = 0; i < p_terms.size(); i++) {
		if (!p_name.containsn(p_terms[i])) {
			return false;
		}
	}
	return true;
}

static bool _docs_name_matches_search_term(const String &p_term, const Vector<String> &p_terms, const String &p_name) {
	const String lowered_name = p_name.to_lower();
	if (_docs_all_terms_in_name(p_terms, lowered_name)) {
		return true;
	}

	if (p_term.begins_with(".")) {
		const String method_term = p_term.substr(1);
		if (lowered_name.begins_with(method_term)) {
			return true;
		}
	}

	if (p_term.ends_with("(")) {
		const String call_term = p_term.left(p_term.length() - 1).strip_edges();
		if (lowered_name.ends_with(call_term)) {
			return true;
		}
	}

	if (p_term.begins_with(".") && p_term.ends_with("(")) {
		const String exact_term = p_term.substr(1, p_term.length() - 2).strip_edges();
		if (lowered_name == exact_term) {
			return true;
		}
	}

	return false;
}

static bool _docs_resolve_version(const Dictionary &p_params, String &r_resolved_version, int &r_error_code, String &r_error_message) {
	const String docs_branch = String(GODOT_VERSION_DOCS_BRANCH);
	const String docs_branch_l = docs_branch.to_lower();
	const String version_branch_l = String(GODOT_VERSION_BRANCH).to_lower();
	const String version_number_l = String(GODOT_VERSION_NUMBER).to_lower();
	const String version_full_l = String(GODOT_VERSION_FULL_CONFIG).to_lower();

	r_resolved_version = docs_branch;

	if (!p_params.has("version")) {
		return true;
	}

	const Variant version_variant = p_params["version"];
	if (version_variant.get_type() != Variant::STRING && version_variant.get_type() != Variant::STRING_NAME) {
		r_error_code = OpenCodeMCPProtocol::ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: version must be a string.";
		return false;
	}

	const String requested_version = String(version_variant).strip_edges().to_lower();
	if (requested_version.is_empty() || requested_version == "current" || requested_version == "latest" ||
			requested_version == docs_branch_l || requested_version == version_branch_l ||
			requested_version == version_number_l || requested_version == version_full_l) {
		return true;
	}

	r_error_code = OpenCodeMCPProtocol::ERROR_INVALID_ARGUMENT;
	r_error_message = "INVALID_ARGUMENT: unsupported version. Supported aliases: current, latest, " + docs_branch + ", " + String(GODOT_VERSION_BRANCH) + ", " + String(GODOT_VERSION_NUMBER) + ".";
	return false;
}

static String _docs_topic_for_kind(const String &p_kind) {
	if (p_kind == "constructor" || p_kind == "method" || p_kind == "operator") {
		return "class_method";
	}
	if (p_kind == "signal") {
		return "class_signal";
	}
	if (p_kind == "property") {
		return "class_property";
	}
	if (p_kind == "enum") {
		return "class_enum";
	}
	if (p_kind == "theme_item") {
		return "class_theme_item";
	}
	if (p_kind == "annotation") {
		return "class_annotation";
	}
	if (p_kind == "constant") {
		return "class_constant";
	}
	return "class_name";
}

static bool _docs_is_supported_member_kind(const String &p_kind) {
	return p_kind == "method" || p_kind == "constructor" || p_kind == "operator" ||
			p_kind == "signal" || p_kind == "property" || p_kind == "constant" ||
			p_kind == "annotation" || p_kind == "theme_item" || p_kind == "enum";
}

static String _docs_make_topic_id(const String &p_topic, const String &p_class_name, const String &p_member_name = String()) {
	if (p_member_name.is_empty()) {
		return p_topic + ":" + p_class_name;
	}
	return p_topic + ":" + p_class_name + ":" + p_member_name;
}

static String _docs_make_url(const String &p_topic, const String &p_class_name, const String &p_member_name = String()) {
	const String lowered_class = p_class_name.to_lower();
	const String lowered_name = p_member_name.to_lower().replace_chars("/_", '-');

	String section;
	if (p_topic == "class_desc") {
		section = "#description";
	} else if (p_topic == "class_signal") {
		section = vformat("#class-%s-signal-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_method" || p_topic == "class_method_desc") {
		section = vformat("#class-%s-method-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_property") {
		section = vformat("#class-%s-property-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_enum") {
		section = vformat("#enum-%s-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_theme_item") {
		section = vformat("#class-%s-theme-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_constant") {
		section = vformat("#class-%s-constant-%s", lowered_class, lowered_name);
	} else if (p_topic == "class_annotation") {
		section = vformat("#%s", lowered_class);
	}

	if (lowered_class.is_empty()) {
		return String(GODOT_VERSION_DOCS_URL "/");
	}

	return vformat(GODOT_VERSION_DOCS_URL "/classes/class_%s.html%s", lowered_class, section);
}

static DocData::ClassDoc *_docs_find_class(DocTools *p_doc_tools, const String &p_class_name) {
	if (!p_doc_tools) {
		return nullptr;
	}

	const String normalized_class = p_class_name.strip_edges();
	if (normalized_class.is_empty()) {
		return nullptr;
	}

	if (DocData::ClassDoc *exact_match = p_doc_tools->class_list.getptr(normalized_class)) {
		return exact_match;
	}

	for (HashMap<String, DocData::ClassDoc>::Iterator E = p_doc_tools->class_list.begin(); E; ++E) {
		if (E->value.name.nocasecmp_to(normalized_class) == 0) {
			return &E->value;
		}
	}

	return nullptr;
}

static bool _docs_push_search_result(Array &r_results, Dictionary &r_seen, bool &r_truncated, int p_limit, const String &p_kind, const String &p_class_name, const String &p_member_name, const String &p_label, bool p_is_script_doc) {
	const String topic = p_kind == "class" ? "class_name" : _docs_topic_for_kind(p_kind);
	const String topic_id = _docs_make_topic_id(topic, p_class_name, p_member_name);
	if (r_seen.has(topic_id)) {
		return true;
	}

	if (r_results.size() >= p_limit) {
		r_truncated = true;
		return false;
	}

	r_seen[topic_id] = true;

	Dictionary result;
	result["topic_id"] = topic_id;
	result["label"] = p_label;
	result["kind"] = p_kind;
	result["class_name"] = p_class_name;
	if (!p_member_name.is_empty()) {
		result["member_name"] = p_member_name;
	}
	result["is_script_doc"] = p_is_script_doc;
	result["docs_url"] = _docs_make_url(topic, p_class_name, p_member_name);
	r_results.push_back(result);
	return true;
}

static void _docs_append_member_match(Array &r_matches, const String &p_kind, const String &p_declared_in, const String &p_member_name, const Variant &p_member_payload) {
	const String topic = _docs_topic_for_kind(p_kind);

	Dictionary match;
	match["kind"] = p_kind;
	match["declared_in"] = p_declared_in;
	match["member_name"] = p_member_name;
	match["topic_id"] = _docs_make_topic_id(topic, p_declared_in, p_member_name);
	match["docs_url"] = _docs_make_url(topic, p_declared_in, p_member_name);
	match["member"] = p_member_payload;
	r_matches.push_back(match);
}

static bool _docs_is_language_allowed(const String &p_filter, const String &p_language) {
	return p_filter == "any" || p_filter == p_language;
}

static void _docs_extract_tag_examples(const String &p_text, const String &p_tag, const String &p_language, const Dictionary &p_base_payload, int p_limit, Array &r_examples) {
	if (r_examples.size() >= p_limit) {
		return;
	}

	const String open_tag = "[" + p_tag + "]";
	const String close_tag = "[/" + p_tag + "]";

	int from = 0;
	while (r_examples.size() < p_limit) {
		const int open_idx = p_text.findn(open_tag, from);
		if (open_idx < 0) {
			break;
		}

		const int content_start = open_idx + open_tag.length();
		const int close_idx = p_text.findn(close_tag, content_start);
		if (close_idx < 0) {
			break;
		}

		const String code = p_text.substr(content_start, close_idx - content_start).strip_edges();
		if (!code.is_empty()) {
			Dictionary example = p_base_payload;
			example["language"] = p_language;
			example["code"] = code;
			r_examples.push_back(example);
		}

		from = close_idx + close_tag.length();
	}
}

static void _docs_collect_examples_from_text(const String &p_text, const Dictionary &p_base_payload, const String &p_language_filter, int p_limit, Array &r_examples) {
	if (p_text.is_empty() || r_examples.size() >= p_limit) {
		return;
	}

	if (_docs_is_language_allowed(p_language_filter, "gdscript")) {
		_docs_extract_tag_examples(p_text, "gdscript", "gdscript", p_base_payload, p_limit, r_examples);
	}
	if (_docs_is_language_allowed(p_language_filter, "csharp")) {
		_docs_extract_tag_examples(p_text, "csharp", "csharp", p_base_payload, p_limit, r_examples);
	}
	if (_docs_is_language_allowed(p_language_filter, "text")) {
		_docs_extract_tag_examples(p_text, "codeblock", "text", p_base_payload, p_limit, r_examples);
	}
}

static void _collect_subtree_nodes(Node *p_node, Vector<Node *> &r_nodes) {
	if (!p_node) {
		return;
	}

	r_nodes.push_back(p_node);
	for (int i = 0; i < p_node->get_child_count(false); i++) {
		_collect_subtree_nodes(p_node->get_child(i, false), r_nodes);
	}
}
} // namespace

void OpenCodeMCPProtocol::_error_handler_callback(void *p_user_data, const char *p_function,
		const char *p_file, int p_line, const char *p_error,
		const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	OpenCodeMCPProtocol *self = static_cast<OpenCodeMCPProtocol *>(p_user_data);
	if (!self) {
		return;
	}

	LogEntry entry;
	entry.type = (p_type == ERR_HANDLER_WARNING) ? "warning" : "error";
	entry.text = p_message && p_message[0] ? String::utf8(p_message) : String::utf8(p_error);
	entry.file = p_file ? String::utf8(p_file) : String();
	entry.function = p_function ? String::utf8(p_function) : String();
	entry.line = p_line;
	entry.timestamp = OS::get_singleton()->get_unix_time();

	if ((int)self->log_ring_buffer.size() < MAX_LOG_ENTRIES) {
		self->log_ring_buffer.push_back(entry);
	} else {
		self->log_ring_buffer[self->log_ring_write_pos] = entry;
	}
	self->log_ring_write_pos = (self->log_ring_write_pos + 1) % MAX_LOG_ENTRIES;
	if (self->log_ring_count < MAX_LOG_ENTRIES) {
		self->log_ring_count++;
	}
}

Error OpenCodeMCPProtocol::start(int p_requested_port, const Dictionary &p_capabilities) {
	if (started) {
		return OK;
	}

	Ref<TCPServer> new_server;
	new_server.instantiate();
	const int requested_port = p_requested_port < 0 ? 0 : p_requested_port;
	Error err = new_server->listen(requested_port, IPAddress("127.0.0.1"));
	ERR_FAIL_COND_V_MSG(err != OK, err, "Failed to start OpenCode MCP server.");

	server = new_server;
	remote_port = server->get_local_port();
	capabilities = p_capabilities;
	clients.clear();
	started = true;

	error_handler.errfunc = _error_handler_callback;
	error_handler.userdata = this;
	add_error_handler(&error_handler);

	return OK;
}

void OpenCodeMCPProtocol::stop() {
	if (!started) {
		return;
	}

	remove_error_handler(&error_handler);

	for (int i = 0; i < (int)clients.size(); i++) {
		if (clients[i].peer.is_valid()) {
			clients[i].peer->disconnect_from_host();
		}
	}
	clients.clear();

	if (server.is_valid()) {
		server->stop();
		server.unref();
	}

	remote_port = 0;
	capabilities.clear();
	started = false;
	log_ring_buffer.clear();
	log_ring_write_pos = 0;
	log_ring_count = 0;
}

void OpenCodeMCPProtocol::poll() {
	if (!started || server.is_null()) {
		return;
	}

	while (server->is_connection_available()) {
		Ref<StreamPeerTCP> peer = server->take_connection();
		if (peer.is_null()) {
			continue;
		}
		peer->set_no_delay(true);

		Client client;
		client.peer = peer;
		clients.push_back(client);
	}

	for (int i = clients.size() - 1; i >= 0; i--) {
		Client &client = clients[i];
		if (client.peer.is_null()) {
			clients.remove_at(i);
			continue;
		}

		client.peer->poll();
		if (client.peer->get_status() != StreamPeerSocket::STATUS_CONNECTED) {
			clients.remove_at(i);
			continue;
		}

		int available_bytes = client.peer->get_available_bytes();
		if (available_bytes > 0) {
			Vector<uint8_t> data;
			data.resize(available_bytes);
			int received = 0;
			Error err = client.peer->get_partial_data(data.ptrw(), available_bytes, received);
			if (err != OK && err != ERR_BUSY) {
				clients.remove_at(i);
				continue;
			}

			if (received > 0) {
				client.read_buffer += String::utf8((const char *)data.ptr(), received);
			}
		}

		if (client.read_buffer.length() > MAX_REQUEST_BYTES) {
			clients.remove_at(i);
			continue;
		}

		while (true) {
			int eol = client.read_buffer.find_char('\n');
			if (eol < 0) {
				break;
			}

			String line = client.read_buffer.substr(0, eol).strip_edges();
			client.read_buffer = client.read_buffer.substr(eol + 1, client.read_buffer.length() - eol - 1);
			if (line.is_empty()) {
				continue;
			}

			Dictionary response = _process_request_line(line);
			if (response.is_empty()) {
				continue;
			}

			String payload = Variant(response).to_json_string() + "\n";
			PackedByteArray bytes = payload.to_utf8_buffer();
			if (client.peer->put_data(bytes.ptr(), bytes.size()) != OK) {
				clients.remove_at(i);
				break;
			}
		}
	}
}

String OpenCodeMCPProtocol::_capability_for_method(const String &p_method) const {
	if (p_method == "scene.get_active" || p_method == "scene.get_tree" || p_method == "node.get_property" ||
			p_method == "node.list_properties" || p_method == "node.get_properties" || p_method == "node.get_properties_batch" ||
			p_method == "scene.list" || p_method == "node.find" || p_method == "node.get_groups" ||
			p_method == "signal.list" || p_method == "signal.get_connections" ||
			p_method == "theme.get_overrides") {
		return "read_scene";
	}
	if (p_method == "node.create" || p_method == "node.create_batch" || p_method == "node.create_from_template" ||
			p_method == "node.delete" || p_method == "node.delete_batch" || p_method == "node.duplicate" || p_method == "node.duplicate_batch" || p_method == "node.reparent" ||
			p_method == "node.set_properties" || p_method == "node.set_properties_batch" || p_method == "script.attach" ||
			p_method == "script.attach_external" || p_method == "script.attach_built_in" ||
			p_method == "scene.open" || p_method == "scene.create" || p_method == "scene.instantiate" ||
			p_method == "node.set_groups" ||
			p_method == "signal.connect" || p_method == "signal.disconnect" ||
			p_method == "theme.set_overrides") {
		return "write_scene";
	}
	if (p_method == "script.get" || p_method == "script.get_active" || p_method == "shader.get" || p_method == "lsp.query") {
		return "read_script";
	}
	if (p_method == "script.apply_text_edits" || p_method == "shader.edit") {
		return "write_script";
	}
	if (p_method == "resource.save" || p_method == "resource.reload") {
		return "save_resource";
	}
	if (p_method == "resource.get" || p_method == "resource.list") {
		return "read_resource";
	}
	if (p_method == "resource.create" || p_method == "resource.set_properties") {
		return "write_resource";
	}
	if (p_method == "project.get_setting" || p_method == "editor.get_errors") {
		return "read_project";
	}
	if (p_method == "project.set_setting") {
		return "write_project";
	}
	if (p_method == "docs.class_lookup" || p_method == "docs.member_lookup" || p_method == "docs.search" ||
			p_method == "docs.inheritance" || p_method == "docs.examples" || p_method == "docs.list_versions") {
		return "read_docs";
	}
	return String();
}

bool OpenCodeMCPProtocol::_is_method_allowed(const String &p_method, int &r_error_code, String &r_error_message) const {
	String required_capability = _capability_for_method(p_method);
	if (!required_capability.is_empty()) {
		if (!bool(capabilities.get(required_capability, false))) {
			r_error_code = ERROR_CAPABILITY_DENIED;
			r_error_message = "CAPABILITY_DENIED: " + required_capability;
			return false;
		}
	}

	return true;
}

Dictionary OpenCodeMCPProtocol::_make_ok(const Variant &p_data, const Array &p_warnings) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	result["warnings"] = p_warnings;
	return result;
}

Dictionary OpenCodeMCPProtocol::_make_response(const Variant &p_result, const Variant &p_id) const {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["result"] = p_result;
	return response;
}

Dictionary OpenCodeMCPProtocol::_make_error_response(int p_error_code, const String &p_error_message, const Variant &p_id) const {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;

	Dictionary error;
	error["code"] = p_error_code;
	error["message"] = p_error_message;
	response["error"] = error;
	return response;
}

Dictionary OpenCodeMCPProtocol::_process_request_line(const String &p_line) {
	JSON json;
	Error parse_error = json.parse(p_line);
	if (parse_error != OK) {
		return _make_error_response(-32700, "Parse error", Variant());
	}

	Variant data = json.get_data();
	if (data.get_type() != Variant::DICTIONARY) {
		return _make_error_response(-32600, "Invalid Request", Variant());
	}

	Dictionary request = data;
	Variant id = request.get("id", Variant());
	const bool is_notification = !request.has("id");

	String method = request.get("method", String());
	if (method.is_empty()) {
		if (is_notification) {
			return Dictionary();
		}
		return _make_error_response(-32600, "Invalid Request: missing method", id);
	}

	Variant params_variant = request.get("params", Dictionary());
	if (params_variant.get_type() != Variant::DICTIONARY) {
		if (is_notification) {
			return Dictionary();
		}
		return _make_error_response(-32602, "Invalid params: expected Dictionary", id);
	}

	Dictionary params = params_variant;
	int capability_error_code = 0;
	String capability_error_message;
	if (!_is_method_allowed(method, capability_error_code, capability_error_message)) {
		if (is_notification) {
			return Dictionary();
		}
		return _make_error_response(capability_error_code, capability_error_message, id);
	}

	int method_error_code = 0;
	String method_error_message;
	Dictionary result = _dispatch_method(method, params, method_error_code, method_error_message);
	if (method_error_code != 0) {
		if (is_notification) {
			return Dictionary();
		}
		return _make_error_response(method_error_code, method_error_message, id);
	}

	if (is_notification) {
		return Dictionary();
	}
	return _make_response(result, id);
}

Dictionary OpenCodeMCPProtocol::_dispatch_method(const String &p_method, const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	if (p_method == "opencode.session.info") {
		return _method_session_info();
	}
	if (p_method == "scene.get_active") {
		return _method_scene_get_active(r_error_code, r_error_message);
	}
	if (p_method == "scene.get_tree") {
		return _method_scene_get_tree(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.get_properties") {
		return _method_node_get_properties(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.get_properties_batch") {
		return _method_node_get_properties_batch(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.get_property") {
		return _method_node_get_property(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.list_properties") {
		return _method_node_list_properties(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.create") {
		return _method_node_create(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.create_batch") {
		return _method_node_create_batch(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.create_from_template") {
		return _method_node_create_from_template(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.delete") {
		return _method_node_delete(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.delete_batch") {
		return _method_node_delete_batch(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.duplicate") {
		return _method_node_duplicate(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.duplicate_batch") {
		return _method_node_duplicate_batch(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.reparent") {
		return _method_node_reparent(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.set_properties") {
		return _method_node_set_properties(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.set_properties_batch") {
		return _method_node_set_properties_batch(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.get") {
		return _method_script_get(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.get_active") {
		return _method_script_get_active(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.apply_text_edits") {
		return _method_script_apply_text_edits(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.attach") {
		return _method_script_attach(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.attach_external") {
		return _method_script_attach_external(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.attach_built_in") {
		return _method_script_attach_built_in(p_params, r_error_code, r_error_message);
	}
	if (p_method == "lsp.query") {
		return _method_lsp_query(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.save") {
		return _method_resource_save(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.reload") {
		return _method_resource_reload(p_params, r_error_code, r_error_message);
	}

	// Scene management tools.
	if (p_method == "scene.list") {
		return _method_scene_list(p_params, r_error_code, r_error_message);
	}
	if (p_method == "scene.open") {
		return _method_scene_open(p_params, r_error_code, r_error_message);
	}
	if (p_method == "scene.create") {
		return _method_scene_create(p_params, r_error_code, r_error_message);
	}
	if (p_method == "scene.instantiate") {
		return _method_scene_instantiate(p_params, r_error_code, r_error_message);
	}

	// Node tools.
	if (p_method == "node.find") {
		return _method_node_find(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.get_groups") {
		return _method_node_get_groups(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.set_groups") {
		return _method_node_set_groups(p_params, r_error_code, r_error_message);
	}

	// Resource tools.
	if (p_method == "resource.get") {
		return _method_resource_get(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.list") {
		return _method_resource_list(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.create") {
		return _method_resource_create(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.set_properties") {
		return _method_resource_set_properties(p_params, r_error_code, r_error_message);
	}

	// Signal tools.
	if (p_method == "signal.list") {
		return _method_signal_list(p_params, r_error_code, r_error_message);
	}
	if (p_method == "signal.get_connections") {
		return _method_signal_get_connections(p_params, r_error_code, r_error_message);
	}
	if (p_method == "signal.connect") {
		return _method_signal_connect(p_params, r_error_code, r_error_message);
	}
	if (p_method == "signal.disconnect") {
		return _method_signal_disconnect(p_params, r_error_code, r_error_message);
	}

	// Project/editor tools.
	if (p_method == "project.get_setting") {
		return _method_project_get_setting(p_params, r_error_code, r_error_message);
	}
	if (p_method == "project.set_setting") {
		return _method_project_set_setting(p_params, r_error_code, r_error_message);
	}
	if (p_method == "editor.get_errors") {
		return _method_editor_get_errors(p_params, r_error_code, r_error_message);
	}

	// Docs tools.
	if (p_method == "docs.class_lookup") {
		return _method_docs_class_lookup(p_params, r_error_code, r_error_message);
	}
	if (p_method == "docs.member_lookup") {
		return _method_docs_member_lookup(p_params, r_error_code, r_error_message);
	}
	if (p_method == "docs.search") {
		return _method_docs_search(p_params, r_error_code, r_error_message);
	}
	if (p_method == "docs.inheritance") {
		return _method_docs_inheritance(p_params, r_error_code, r_error_message);
	}
	if (p_method == "docs.examples") {
		return _method_docs_examples(p_params, r_error_code, r_error_message);
	}
	if (p_method == "docs.list_versions") {
		return _method_docs_list_versions(p_params, r_error_code, r_error_message);
	}

	// Shader/theme tools.
	if (p_method == "shader.get") {
		return _method_shader_get(p_params, r_error_code, r_error_message);
	}
	if (p_method == "shader.edit") {
		return _method_shader_edit(p_params, r_error_code, r_error_message);
	}
	if (p_method == "theme.get_overrides") {
		return _method_theme_get_overrides(p_params, r_error_code, r_error_message);
	}
	if (p_method == "theme.set_overrides") {
		return _method_theme_set_overrides(p_params, r_error_code, r_error_message);
	}

	r_error_code = -32601;
	r_error_message = "Method not found: " + p_method;
	return Dictionary();
}

Dictionary OpenCodeMCPProtocol::_method_session_info() const {
	Dictionary info;
	info["port"] = remote_port;
	info["capabilities"] = capabilities;
	info["transport"] = "jsonrpc-line";
	info["protocol"] = "opencode-godot-v1";
	return _make_ok(info);
}

Node *OpenCodeMCPProtocol::_get_edited_scene_root() const {
	if (!EditorNode::get_singleton()) {
		return nullptr;
	}
	return EditorNode::get_singleton()->get_edited_scene();
}

Node *OpenCodeMCPProtocol::_resolve_node_path(const String &p_node_path, Node *p_root) const {
	if (!p_root) {
		return nullptr;
	}
	if (p_node_path.is_empty() || p_node_path == ".") {
		return p_root;
	}

	return p_root->get_node_or_null(NodePath(p_node_path));
}

Ref<Script> OpenCodeMCPProtocol::_find_script_in_tree(Node *p_root, const String &p_script_path) const {
	if (!p_root) {
		return Ref<Script>();
	}

	Ref<Script> script = p_root->get_script();
	if (script.is_valid() && script->get_path() == p_script_path) {
		return script;
	}

	for (int i = 0; i < p_root->get_child_count(); i++) {
		Ref<Script> child_script = _find_script_in_tree(p_root->get_child(i), p_script_path);
		if (child_script.is_valid()) {
			return child_script;
		}
	}

	return Ref<Script>();
}

Ref<Script> OpenCodeMCPProtocol::_resolve_script(const Dictionary &p_params, String &r_script_path, int &r_error_code, String &r_error_message) const {
	String script_path = p_params.get("script_path", String());
	String node_path = p_params.get("node_path", String());
	Ref<Script> script;

	Node *root = nullptr;
	if (!node_path.is_empty()) {
		root = _get_edited_scene_root();
		if (!root) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: No edited scene is currently open.";
			return Ref<Script>();
		}

		Node *node = _resolve_node_path(node_path, root);
		if (!node) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: Node path does not exist.";
			return Ref<Script>();
		}

		script = node->get_script();
		if (script.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: Node has no attached script.";
			return Ref<Script>();
		}

		if (script_path.is_empty()) {
			script_path = script->get_path();
		}
	}

	if (script.is_null()) {
		if (script_path.is_empty()) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: provide script_path or a valid node_path.";
			return Ref<Script>();
		}

		if (!script_path.begins_with("res://")) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: script_path must start with res://";
			return Ref<Script>();
		}

		script = ResourceCache::get_ref(script_path);
		if (script.is_null()) {
			if (!root) {
				root = _get_edited_scene_root();
			}
			if (root) {
				script = _find_script_in_tree(root, script_path);
			}
		}
		if (script.is_null()) {
			script = ResourceLoader::load(script_path);
		}
		if (script.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: Script resource could not be loaded.";
			return Ref<Script>();
		}
	}

	r_script_path = script->get_path();
	if (r_script_path.is_empty()) {
		r_script_path = script_path;
	}

	return script;
}

String OpenCodeMCPProtocol::_resolve_script_workspace(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String workspace = String(p_params.get("workspace", SCRIPT_WORKSPACE_AUTO)).to_lower();
	if (workspace.is_empty()) {
		workspace = SCRIPT_WORKSPACE_AUTO;
	}

	if (workspace == SCRIPT_WORKSPACE_AUTO) {
		EditorNode *editor_node = EditorNode::get_singleton();
		EditorMainScreen *main_screen = editor_node ? editor_node->get_editor_main_screen() : nullptr;
		if (main_screen && main_screen->get_selected_index() == EditorMainScreen::EDITOR_SCRIPT) {
			return SCRIPT_WORKSPACE_SCRIPT_EDITOR;
		}
		return SCRIPT_WORKSPACE_SCENE_VIEW;
	}

	if (workspace != SCRIPT_WORKSPACE_SCENE_VIEW && workspace != SCRIPT_WORKSPACE_SCRIPT_EDITOR) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: workspace must be one of auto, scene_view, script_editor.";
		return String();
	}

	return workspace;
}

bool OpenCodeMCPProtocol::_resolve_active_script_editor_context(ScriptEditorBase *&r_editor, Ref<Script> &r_script, String &r_script_path, String &r_display_name, bool &r_is_unsaved, String &r_source, bool &r_has_text_buffer, int &r_error_code, String &r_error_message) const {
	r_editor = nullptr;
	r_script.unref();
	r_script_path = String();
	r_display_name = String();
	r_is_unsaved = false;
	r_source = String();
	r_has_text_buffer = false;

	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (!script_editor) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Script editor is not available.";
		return false;
	}

	ScriptEditorBase *current_editor = script_editor->get_current_editor();
	if (!current_editor) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No script editor tab is currently active.";
		return false;
	}

	Ref<Script> script = current_editor->get_edited_resource();
	if (script.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: The active editor tab is not a script.";
		return false;
	}

	r_editor = current_editor;
	r_script = script;
	r_script_path = script->get_path();
	r_display_name = current_editor->get_name();
	r_is_unsaved = current_editor->is_unsaved();
	r_source = script->get_source_code();

	TextEditorBase *text_editor = Object::cast_to<TextEditorBase>(current_editor);
	if (text_editor && text_editor->get_code_editor() && text_editor->get_code_editor()->get_text_editor()) {
		r_source = text_editor->get_code_editor()->get_text_editor()->get_text();
		r_has_text_buffer = true;
	}

	return true;
}

bool OpenCodeMCPProtocol::_resolve_open_script_editor_context(const Ref<Script> &p_script, const String &p_script_path, ScriptEditorBase *&r_editor, String &r_source, bool &r_has_text_buffer) const {
	r_editor = nullptr;
	r_source = String();
	r_has_text_buffer = false;

	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (!script_editor) {
		return false;
	}

	ScriptEditorBase *editor = script_editor->find_open_script_editor(p_script, p_script_path);
	if (!editor) {
		return false;
	}

	r_editor = editor;
	r_source = p_script.is_valid() ? p_script->get_source_code() : String();

	TextEditorBase *text_editor = Object::cast_to<TextEditorBase>(editor);
	if (text_editor && text_editor->get_code_editor() && text_editor->get_code_editor()->get_text_editor()) {
		r_source = text_editor->get_code_editor()->get_text_editor()->get_text();
		r_has_text_buffer = true;
	}

	return true;
}

bool OpenCodeMCPProtocol::_validate_script_editor_target(const String &p_requested_script_path, const String &p_active_script_path, int &r_error_code, String &r_error_message) const {
	if (p_requested_script_path.is_empty()) {
		return true;
	}
	if (!p_requested_script_path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script_path must start with res://";
		return false;
	}
	if (p_requested_script_path != p_active_script_path) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: script_path does not match active script editor tab.";
		return false;
	}
	return true;
}

bool OpenCodeMCPProtocol::_set_script_editor_buffer_source(ScriptEditorBase *p_editor, const String &p_source) const {
	TextEditorBase *text_editor = Object::cast_to<TextEditorBase>(p_editor);
	if (!text_editor || !text_editor->get_code_editor() || !text_editor->get_code_editor()->get_text_editor()) {
		return false;
	}

	text_editor->get_code_editor()->get_text_editor()->set_text(p_source);
	text_editor->validate_script();
	return true;
}

Ref<Script> OpenCodeMCPProtocol::_resolve_active_script_editor_script(String &r_display_name, bool &r_is_unsaved, int &r_error_code, String &r_error_message) const {
	ScriptEditorBase *editor = nullptr;
	Ref<Script> script;
	String script_path;
	String source;
	bool has_text_buffer = false;
	if (!_resolve_active_script_editor_context(editor, script, script_path, r_display_name, r_is_unsaved, source, has_text_buffer, r_error_code, r_error_message)) {
		return Ref<Script>();
	}
	(void)editor;
	(void)script_path;
	(void)source;
	(void)has_text_buffer;
	return script;
}

Node *OpenCodeMCPProtocol::_resolve_selected_scene_node(String &r_node_path, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return nullptr;
	}

	EditorNode *editor_node = EditorNode::get_singleton();
	EditorSelection *selection = editor_node ? editor_node->get_editor_selection() : nullptr;
	if (!selection) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Scene selection is not available.";
		return nullptr;
	}

	List<Node *> selected_nodes = selection->get_top_selected_node_list();
	if (selected_nodes.is_empty()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No node is selected in the scene view.";
		return nullptr;
	}

	Node *node = selected_nodes.front()->get();
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Selected node is no longer available.";
		return nullptr;
	}
	if (node != root && !root->is_ancestor_of(node)) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Selected node is outside the active edited scene.";
		return nullptr;
	}

	r_node_path = node == root ? String(".") : String(root->get_path_to(node));
	return node;
}

Ref<Script> OpenCodeMCPProtocol::_resolve_selected_scene_node_script(String &r_script_path, String &r_node_path, int &r_error_code, String &r_error_message) const {
	Node *node = _resolve_selected_scene_node(r_node_path, r_error_code, r_error_message);
	if (!node) {
		return Ref<Script>();
	}

	Ref<Script> script = node->get_script();
	if (script.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Selected node has no attached script.";
		return Ref<Script>();
	}

	r_script_path = script->get_path();
	return script;
}

Dictionary OpenCodeMCPProtocol::_method_scene_get_active(int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	Dictionary data;
	data["name"] = root->get_name();
	data["path"] = root->get_scene_file_path();
	data["node_path"] = ".";
	data["class"] = root->get_class();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_serialize_node(Node *p_node, Node *p_root, int p_depth, int p_max_depth, int p_max_nodes, int &r_nodes_seen, bool &r_truncated) const {
	Dictionary node_data;
	if (!p_node || !p_root) {
		return node_data;
	}

	if (r_nodes_seen >= p_max_nodes) {
		r_truncated = true;
		return node_data;
	}
	r_nodes_seen++;

	node_data["path"] = p_node == p_root ? String(".") : String(p_root->get_path_to(p_node));
	node_data["name"] = p_node->get_name();
	node_data["class"] = p_node->get_class();
	node_data["child_count"] = p_node->get_child_count();

	Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		node_data["script_path"] = script->get_path();
	}

	if (p_depth >= p_max_depth) {
		if (p_node->get_child_count() > 0) {
			r_truncated = true;
		}
		return node_data;
	}

	Array children;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (!child) {
			continue;
		}
		Dictionary child_data = _serialize_node(child, p_root, p_depth + 1, p_max_depth, p_max_nodes, r_nodes_seen, r_truncated);
		if (!child_data.is_empty()) {
			children.push_back(child_data);
		}
	}
	node_data["children"] = children;
	return node_data;
}

Dictionary OpenCodeMCPProtocol::_method_scene_get_tree(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String root_path = p_params.get("root_path", String("."));
	if (root_path.is_empty()) {
		root_path = ".";
	}

	Node *tree_root = _resolve_node_path(root_path, root);
	if (!tree_root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: root_path does not exist.";
		return Dictionary();
	}

	int max_depth = (int)p_params.get("max_depth", 6);
	if (max_depth < 0) {
		max_depth = 0;
	}
	int max_nodes = (int)p_params.get("max_nodes", 1500);
	if (max_nodes < 1) {
		max_nodes = 1;
	}

	int nodes_seen = 0;
	bool truncated = false;
	Dictionary tree = _serialize_node(tree_root, root, 0, max_depth, max_nodes, nodes_seen, truncated);

	Dictionary data;
	data["root"] = tree;
	data["root_path"] = tree_root == root ? String(".") : String(root->get_path_to(tree_root));
	data["node_count"] = nodes_seen;
	data["truncated"] = truncated;
	data["max_depth"] = max_depth;
	data["max_nodes"] = max_nodes;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_get_property(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	Variant property_name_variant = p_params.get("property_name", Variant());
	if (node_path.is_empty() || (property_name_variant.get_type() != Variant::STRING && property_name_variant.get_type() != Variant::STRING_NAME)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and property_name are required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	StringName property_name = String(property_name_variant);
	bool valid = false;
	Variant value = node->get(property_name, &valid);
	if (!valid) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
		return Dictionary();
	}

	if (value.get_type() == Variant::OBJECT) {
		Variant serialized;
		if (!_serialize_resource_reference(value, serialized)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property value is not serializable: " + String(property_name);
			return Dictionary();
		}
		value = serialized;
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["property_name"] = String(property_name);
	data["type"] = Variant::get_type_name(value.get_type());
	data["value"] = value;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_list_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	List<PropertyInfo> plist;
	node->get_property_list(&plist);

	Array properties;
	for (const PropertyInfo &pi : plist) {
		if (pi.usage & PROPERTY_USAGE_CATEGORY || pi.usage & PROPERTY_USAGE_GROUP || pi.usage & PROPERTY_USAGE_SUBGROUP) {
			continue;
		}
		if (!(pi.usage & PROPERTY_USAGE_EDITOR) && !(pi.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}

		Dictionary prop_info;
		prop_info["name"] = String(pi.name);
		prop_info["type"] = Variant::get_type_name(pi.type);
		properties.push_back(prop_info);
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["properties"] = properties;
	data["property_count"] = properties.size();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_get_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	Variant property_names_variant = p_params.get("property_names", Variant());
	if (node_path.is_empty() || property_names_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and non-empty property_names array are required.";
		return Dictionary();
	}

	Array property_names = property_names_variant;
	if (property_names.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: property_names must be non-empty.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Dictionary values;
	for (int i = 0; i < property_names.size(); i++) {
		Variant property_name_variant = property_names[i];
		if (property_name_variant.get_type() != Variant::STRING && property_name_variant.get_type() != Variant::STRING_NAME) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property_names entries must be strings.";
			return Dictionary();
		}

		StringName property_name = String(property_name_variant);
		bool valid = false;
		Variant value = node->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}

		if (value.get_type() == Variant::OBJECT) {
			Variant serialized;
			if (!_serialize_resource_reference(value, serialized)) {
				r_error_code = ERROR_INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: property value is not serializable: " + String(property_name);
				return Dictionary();
			}
			value = serialized;
		}

		values[String(property_name)] = value;
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["properties"] = values;
	data["property_count"] = values.size();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_get_properties_batch(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	const Variant items_variant = p_params.get("items", Variant());
	if (items_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty items array is required.";
		return Dictionary();
	}

	Array items = items_variant;
	if (items.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: items must be non-empty.";
		return Dictionary();
	}

	Array results;
	results.resize(items.size());
	int success_count = 0;
	int failure_count = 0;

	for (int i = 0; i < items.size(); i++) {
		String item_id;
		if (items[i].get_type() != Variant::DICTIONARY) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT: items entries must be dictionaries.");
			failure_count++;
			continue;
		}

		Dictionary item = items[i];
		const Variant item_id_variant = item.get("item_id", Variant());
		if (item_id_variant.get_type() == Variant::STRING || item_id_variant.get_type() == Variant::STRING_NAME) {
			item_id = String(item_id_variant);
		}

		String node_path = item.get("node_path", String());
		Variant property_names_variant = item.get("property_names", Variant());
		if (node_path.is_empty() || property_names_variant.get_type() != Variant::ARRAY) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT: node_path and non-empty property_names array are required.");
			failure_count++;
			continue;
		}

		Array property_names = property_names_variant;
		if (property_names.is_empty()) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT: property_names must be non-empty.");
			failure_count++;
			continue;
		}

		Node *node = _resolve_node_path(node_path, root);
		if (!node) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, "NOT_FOUND: Node path does not exist.");
			failure_count++;
			continue;
		}

		Dictionary values;
		bool item_failed = false;
		int item_error_code = 0;
		String item_error_message;
		for (int j = 0; j < property_names.size(); j++) {
			Variant property_name_variant = property_names[j];
			if (property_name_variant.get_type() != Variant::STRING && property_name_variant.get_type() != Variant::STRING_NAME) {
				item_error_code = ERROR_INVALID_ARGUMENT;
				item_error_message = "INVALID_ARGUMENT: property_names entries must be strings.";
				item_failed = true;
				break;
			}

			StringName property_name = String(property_name_variant);
			bool valid = false;
			Variant value = node->get(property_name, &valid);
			if (!valid) {
				item_error_code = ERROR_INVALID_ARGUMENT;
				item_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
				item_failed = true;
				break;
			}

			if (value.get_type() == Variant::OBJECT) {
				Variant serialized;
				if (!_serialize_resource_reference(value, serialized)) {
					item_error_code = ERROR_INVALID_ARGUMENT;
					item_error_message = "INVALID_ARGUMENT: property value is not serializable: " + String(property_name);
					item_failed = true;
					break;
				}
				value = serialized;
			}

			values[String(property_name)] = value;
		}

		if (item_failed) {
			results[i] = _make_batch_item_error(i, item_id, item_error_code, item_error_message);
			failure_count++;
			continue;
		}

		Dictionary item_data;
		item_data["node_path"] = node_path;
		item_data["properties"] = values;
		item_data["property_count"] = values.size();
		results[i] = _make_batch_item_success(i, item_id, item_data);
		success_count++;
	}

	Dictionary data;
	data["total_count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["results"] = results;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_create_batch(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	const Variant items_variant = p_params.get("items", Variant());
	if (items_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty items array is required.";
		return Dictionary();
	}

	Array items = items_variant;
	if (items.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: items must be non-empty.";
		return Dictionary();
	}

	BatchMode mode = BATCH_MODE_ATOMIC;
	if (!_parse_batch_mode(p_params, mode, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}

	bool preview_only = false;
	if (p_params.has("preview_only")) {
		const Variant preview_variant = p_params["preview_only"];
		if (preview_variant.get_type() != Variant::BOOL) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: preview_only must be a boolean.";
			return Dictionary();
		}
		preview_only = bool(preview_variant);
	}

	struct CreateBatchOp {
		int index = -1;
		String item_id;
		Node *parent = nullptr;
		Node *node = nullptr;
		int position = -1;
		Dictionary properties;
	};

	Array results;
	results.resize(items.size());
	Vector<CreateBatchOp> operations;
	HashMap<String, Node *> created_nodes_by_item_id;
	HashMap<String, bool> seen_item_ids;

	int failure_count = 0;
	int skipped_count = 0;
	bool atomic_failed = false;
	String atomic_failure_message = "CONFLICT: skipped due atomic batch failure.";

	for (int i = 0; i < items.size(); i++) {
		String item_id;
		if (atomic_failed) {
			if (items[i].get_type() == Variant::DICTIONARY) {
				Dictionary skipped_item = items[i];
				Variant skipped_item_id_variant = skipped_item.get("item_id", Variant());
				if (skipped_item_id_variant.get_type() == Variant::STRING || skipped_item_id_variant.get_type() == Variant::STRING_NAME) {
					item_id = String(skipped_item_id_variant);
				}
			}
			results[i] = _make_batch_item_error(i, item_id, ERROR_CONFLICT, atomic_failure_message, "skipped");
			skipped_count++;
			continue;
		}

		if (items[i].get_type() != Variant::DICTIONARY) {
			const String message = "INVALID_ARGUMENT: items entries must be dictionaries.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary item = items[i];

		Variant item_id_variant = item.get("item_id", Variant());
		if (item_id_variant.get_type() != Variant::STRING && item_id_variant.get_type() != Variant::STRING_NAME) {
			const String message = "INVALID_ARGUMENT: create_batch items must include non-empty item_id string.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}
		item_id = String(item_id_variant).strip_edges();
		if (item_id.is_empty()) {
			const String message = "INVALID_ARGUMENT: create_batch items must include non-empty item_id string.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}
		if (seen_item_ids.has(item_id)) {
			const String message = "INVALID_ARGUMENT: duplicate item_id in create_batch: " + item_id;
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}
		seen_item_ids.insert(item_id, true);

		String parent_path = item.get("parent_path", String());
		String parent_item_id = item.get("parent_item_id", String());
		if (parent_path.is_empty() == parent_item_id.is_empty()) {
			const String message = "INVALID_ARGUMENT: provide exactly one of parent_path or parent_item_id.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		Node *parent = nullptr;
		if (!parent_item_id.is_empty()) {
			if (!created_nodes_by_item_id.has(parent_item_id)) {
				const String message = "NOT_FOUND: parent_item_id not found in previous successful batch items: " + parent_item_id;
				results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
				failure_count++;
				if (mode == BATCH_MODE_ATOMIC) {
					atomic_failed = true;
					atomic_failure_message = "CONFLICT: atomic create_batch aborted after item failure.";
				}
				continue;
			}
			parent = created_nodes_by_item_id[parent_item_id];
		} else {
			parent = _resolve_node_path(parent_path, root);
			if (!parent) {
				const String message = "NOT_FOUND: Parent node path does not exist.";
				results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
				failure_count++;
				if (mode == BATCH_MODE_ATOMIC) {
					atomic_failed = true;
					atomic_failure_message = "CONFLICT: atomic create_batch aborted after item failure.";
				}
				continue;
			}
		}

		String type_name = item.get("type", String());
		if (type_name.is_empty()) {
			const String message = "INVALID_ARGUMENT: type is required for each create_batch item.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		int position = (int)item.get("position", -1);
		String node_name = item.get("name", String());

		Object *object = ClassDB::instantiate(type_name);
		Node *node = Object::cast_to<Node>(object);
		if (!node) {
			if (object) {
				memdelete(object);
			}
			const String message = "INVALID_ARGUMENT: type is not a concrete Node class.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		if (node_name.is_empty()) {
			node_name = type_name;
		}
		node->set_name(node_name);
		node->set_name(parent->validate_child_name(node));

		Dictionary initial_properties;
		String properties_error;
		if (!_collect_properties_from_payload(item, initial_properties, properties_error)) {
			memdelete(node);
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, properties_error);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary coerced_initial_properties;
		bool item_failed = false;
		String item_error_message;
		for (const KeyValue<Variant, Variant> &E : initial_properties) {
			StringName property_name = E.key;
			bool valid = false;
			Variant old_value = node->get(property_name, &valid);
			if (!valid) {
				item_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
				item_failed = true;
				break;
			}

			Variant coerced_value;
			String property_error;
			const String expected_resource_type = _resource_type_hint_for_property(node, property_name);
			if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, property_error)) {
				item_error_message = property_error;
				item_failed = true;
				break;
			}

			if (!_validate_property_value(node, property_name, coerced_value, property_error)) {
				item_error_message = property_error;
				item_failed = true;
				break;
			}

			coerced_initial_properties[property_name] = coerced_value;
		}

		if (item_failed) {
			memdelete(node);
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, item_error_message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic create_batch aborted after validation failure.";
			}
			continue;
		}

		CreateBatchOp op;
		op.index = i;
		op.item_id = item_id;
		op.parent = parent;
		op.node = node;
		op.position = position;
		op.properties = coerced_initial_properties;
		operations.push_back(op);
		created_nodes_by_item_id.insert(item_id, node);
	}

	if (atomic_failed) {
		for (const CreateBatchOp &op : operations) {
			if (op.node) {
				memdelete(op.node);
			}
			results[op.index] = _make_batch_item_error(op.index, op.item_id, ERROR_CONFLICT, "CONFLICT: atomic create_batch rolled back.", "skipped");
			skipped_count++;
		}

		Dictionary data;
		data["mode"] = _batch_mode_to_string(mode);
		data["preview_only"] = preview_only;
		data["applied"] = false;
		data["total_count"] = items.size();
		data["success_count"] = 0;
		data["failure_count"] = failure_count;
		data["skipped_count"] = skipped_count;
		data["results"] = results;
		return _make_ok(data);
	}

	int success_count = 0;
	const bool should_apply = !preview_only && !operations.is_empty();
	if (should_apply) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		EditorData &editor_data = EditorNode::get_editor_data();
		undo_redo->create_action_for_history("OpenCode: Create Nodes (Batch)", editor_data.get_current_edited_scene_history_id());

		for (const CreateBatchOp &op : operations) {
			undo_redo->add_do_method(op.parent, "add_child", op.node, true);
			if (op.position >= 0) {
				undo_redo->add_do_method(op.parent, "move_child", op.node, op.position);
			}
			undo_redo->add_do_method(op.node, "set_owner", root);
			for (const KeyValue<Variant, Variant> &E : op.properties) {
				StringName property = E.key;
				undo_redo->add_do_property(op.node, property, E.value);
			}
			undo_redo->add_do_reference(op.node);
			undo_redo->add_undo_method(op.parent, "remove_child", op.node);
		}

		undo_redo->commit_action();
	}

	for (const CreateBatchOp &op : operations) {
		Dictionary item_data;
		item_data["name"] = op.node->get_name();
		item_data["class"] = op.node->get_class();
		if (should_apply) {
			item_data["node_path"] = String(root->get_path_to(op.node));
			results[op.index] = _make_batch_item_success(op.index, op.item_id, item_data, "applied");
		} else {
			results[op.index] = _make_batch_item_success(op.index, op.item_id, item_data, "preview");
			memdelete(op.node);
		}
		success_count++;
	}

	Dictionary data;
	data["mode"] = _batch_mode_to_string(mode);
	data["preview_only"] = preview_only;
	data["applied"] = should_apply;
	data["total_count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["skipped_count"] = skipped_count;
	data["results"] = results;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_create_from_template(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String parent_path = p_params.get("parent_path", String());
	String root_name = p_params.get("root_name", String());
	String root_type = p_params.get("root_type", String("Node"));
	const Variant nodes_variant = p_params.get("nodes", Variant());

	if (parent_path.is_empty() || root_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: parent_path and root_name are required.";
		return Dictionary();
	}
	if (nodes_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty nodes array is required.";
		return Dictionary();
	}

	Array template_nodes = nodes_variant;
	if (template_nodes.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: nodes must be non-empty.";
		return Dictionary();
	}

	String root_item_id = "__template_root__";
	bool conflict = true;
	int suffix = 0;
	while (conflict) {
		conflict = false;
		for (int i = 0; i < template_nodes.size(); i++) {
			if (template_nodes[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary template_item = template_nodes[i];
			String item_id = template_item.get("item_id", String());
			if (item_id == root_item_id) {
				conflict = true;
				suffix++;
				root_item_id = "__template_root_" + itos(suffix) + "__";
				break;
			}
		}
	}

	Array batch_items;

	Dictionary root_item;
	root_item["item_id"] = root_item_id;
	root_item["parent_path"] = parent_path;
	root_item["type"] = root_type;
	root_item["name"] = root_name;
	if (p_params.has("root_properties")) {
		root_item["properties"] = p_params["root_properties"];
	}
	if (p_params.has("root_property_entries")) {
		root_item["property_entries"] = p_params["root_property_entries"];
	}
	batch_items.push_back(root_item);

	for (int i = 0; i < template_nodes.size(); i++) {
		if (template_nodes[i].get_type() != Variant::DICTIONARY) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: template nodes entries must be dictionaries.";
			return Dictionary();
		}

		Dictionary template_item = template_nodes[i];
		String type_name = template_item.get("type", String());
		if (type_name.is_empty()) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: each template node requires type.";
			return Dictionary();
		}

		Dictionary batch_item;
		String item_id = template_item.get("item_id", String());
		if (item_id.is_empty()) {
			item_id = "template_item_" + itos(i);
		}
		batch_item["item_id"] = item_id;
		batch_item["type"] = type_name;

		String parent_item_id = template_item.get("parent_item_id", String());
		if (parent_item_id.is_empty()) {
			parent_item_id = root_item_id;
		}
		batch_item["parent_item_id"] = parent_item_id;

		if (template_item.has("name")) {
			batch_item["name"] = template_item["name"];
		}
		if (template_item.has("position")) {
			batch_item["position"] = template_item["position"];
		}
		if (template_item.has("properties")) {
			batch_item["properties"] = template_item["properties"];
		}
		if (template_item.has("property_entries")) {
			batch_item["property_entries"] = template_item["property_entries"];
		}

		batch_items.push_back(batch_item);
	}

	Dictionary batch_params;
	batch_params["items"] = batch_items;
	batch_params["mode"] = p_params.get("mode", String("atomic"));
	if (p_params.has("preview_only")) {
		batch_params["preview_only"] = p_params["preview_only"];
	}

	return _method_node_create_batch(batch_params, r_error_code, r_error_message);
}

Dictionary OpenCodeMCPProtocol::_method_node_create(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String parent_path = p_params.get("parent_path", String());
	String type_name = p_params.get("type", String());
	String node_name = p_params.get("name", String());
	int position = (int)p_params.get("position", -1);
	Dictionary initial_properties = p_params.get("properties", Dictionary());

	if (parent_path.is_empty() || type_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: parent_path and type are required.";
		return Dictionary();
	}

	Node *parent = _resolve_node_path(parent_path, root);
	if (!parent) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Parent node path does not exist.";
		return Dictionary();
	}

	Object *object = ClassDB::instantiate(type_name);
	Node *node = Object::cast_to<Node>(object);
	if (!node) {
		if (object) {
			memdelete(object);
		}
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: type is not a concrete Node class.";
		return Dictionary();
	}

	if (node_name.is_empty()) {
		node_name = type_name;
	}
	node->set_name(node_name);
	node->set_name(parent->validate_child_name(node));

	Dictionary coerced_initial_properties;
	for (const KeyValue<Variant, Variant> &E : initial_properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = node->get(property_name, &valid);
		if (!valid) {
			memdelete(node);
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}

		Variant coerced_value;
		String property_error;
		const String expected_resource_type = _resource_type_hint_for_property(node, property_name);
		if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, property_error)) {
			memdelete(node);
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = property_error;
			return Dictionary();
		}

		if (!_validate_property_value(node, property_name, coerced_value, property_error)) {
			memdelete(node);
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = property_error;
			return Dictionary();
		}

		coerced_initial_properties[property_name] = coerced_value;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Create Node", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(parent, "add_child", node, true);
	if (position >= 0) {
		undo_redo->add_do_method(parent, "move_child", node, position);
	}
	undo_redo->add_do_method(node, "set_owner", root);
	for (const KeyValue<Variant, Variant> &E : coerced_initial_properties) {
		StringName property = E.key;
		undo_redo->add_do_property(node, property, E.value);
	}
	undo_redo->add_do_reference(node);
	undo_redo->add_undo_method(parent, "remove_child", node);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = String(root->get_path_to(node));
	data["name"] = node->get_name();
	data["class"] = node->get_class();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_duplicate_batch(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	const Variant items_variant = p_params.get("items", Variant());
	if (items_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty items array is required.";
		return Dictionary();
	}

	Array items = items_variant;
	if (items.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: items must be non-empty.";
		return Dictionary();
	}

	BatchMode mode = BATCH_MODE_ATOMIC;
	if (!_parse_batch_mode(p_params, mode, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}

	struct DuplicateBatchOp {
		int index = -1;
		String item_id;
		String source_path;
		Node *parent = nullptr;
		Node *node = nullptr;
		int position = -1;
		Dictionary overrides;
	};

	Array results;
	results.resize(items.size());
	Vector<DuplicateBatchOp> operations;
	HashMap<String, Node *> duplicated_nodes_by_item_id;
	HashMap<String, bool> seen_item_ids;

	int failure_count = 0;
	int skipped_count = 0;
	bool atomic_failed = false;
	String atomic_failure_message = "CONFLICT: skipped due atomic batch failure.";

	for (int i = 0; i < items.size(); i++) {
		String item_id;
		if (atomic_failed) {
			if (items[i].get_type() == Variant::DICTIONARY) {
				Dictionary skipped_item = items[i];
				Variant skipped_item_id_variant = skipped_item.get("item_id", Variant());
				if (skipped_item_id_variant.get_type() == Variant::STRING || skipped_item_id_variant.get_type() == Variant::STRING_NAME) {
					item_id = String(skipped_item_id_variant);
				}
			}
			results[i] = _make_batch_item_error(i, item_id, ERROR_CONFLICT, atomic_failure_message, "skipped");
			skipped_count++;
			continue;
		}

		if (items[i].get_type() != Variant::DICTIONARY) {
			const String message = "INVALID_ARGUMENT: items entries must be dictionaries.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary item = items[i];

		Variant item_id_variant = item.get("item_id", Variant());
		if (item_id_variant.get_type() != Variant::STRING && item_id_variant.get_type() != Variant::STRING_NAME) {
			const String message = "INVALID_ARGUMENT: duplicate_batch items must include non-empty item_id string.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}
		item_id = String(item_id_variant).strip_edges();
		if (item_id.is_empty()) {
			const String message = "INVALID_ARGUMENT: duplicate_batch items must include non-empty item_id string.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}
		if (seen_item_ids.has(item_id)) {
			const String message = "INVALID_ARGUMENT: duplicate item_id in duplicate_batch: " + item_id;
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}
		seen_item_ids.insert(item_id, true);

		String source_path = item.get("source_path", String());
		if (source_path.is_empty()) {
			const String message = "INVALID_ARGUMENT: source_path is required for each duplicate_batch item.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}

		Node *source = _resolve_node_path(source_path, root);
		if (!source) {
			const String message = "NOT_FOUND: source_path does not exist.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after item failure.";
			}
			continue;
		}

		String parent_path = item.get("parent_path", String());
		String parent_item_id = item.get("parent_item_id", String());
		if (!parent_path.is_empty() && !parent_item_id.is_empty()) {
			const String message = "INVALID_ARGUMENT: provide at most one of parent_path or parent_item_id.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}

		Node *parent = nullptr;
		if (!parent_item_id.is_empty()) {
			if (!duplicated_nodes_by_item_id.has(parent_item_id)) {
				const String message = "NOT_FOUND: parent_item_id not found in previous successful batch items: " + parent_item_id;
				results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
				failure_count++;
				if (mode == BATCH_MODE_ATOMIC) {
					atomic_failed = true;
					atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after item failure.";
				}
				continue;
			}
			parent = duplicated_nodes_by_item_id[parent_item_id];
		} else if (!parent_path.is_empty()) {
			parent = _resolve_node_path(parent_path, root);
			if (!parent) {
				const String message = "NOT_FOUND: parent_path does not exist.";
				results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
				failure_count++;
				if (mode == BATCH_MODE_ATOMIC) {
					atomic_failed = true;
					atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after item failure.";
				}
				continue;
			}
		} else {
			parent = source->get_parent();
			if (!parent) {
				const String message = "INVALID_ARGUMENT: source node has no parent; specify parent_path or parent_item_id.";
				results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
				failure_count++;
				if (mode == BATCH_MODE_ATOMIC) {
					atomic_failed = true;
					atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
				}
				continue;
			}
		}

		HashMap<const Node *, Node *> duplimap;
		Node *duplicate = source->duplicate_from_editor(duplimap);
		if (!duplicate) {
			const String message = "INTERNAL: Failed to duplicate source node.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INTERNAL, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after item failure.";
			}
			continue;
		}

		String new_name = item.get("new_name", String());
		if (!new_name.is_empty()) {
			duplicate->set_name(new_name);
		}
		duplicate->set_name(parent->validate_child_name(duplicate));

		Dictionary overrides;
		String overrides_error;
		if (!_collect_properties_from_payload_with_alias(item, "property_overrides", overrides, overrides_error)) {
			memdelete(duplicate);
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, overrides_error);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary coerced_overrides;
		bool item_failed = false;
		String item_error_message;
		for (const KeyValue<Variant, Variant> &E : overrides) {
			StringName property_name = E.key;
			bool valid = false;
			Variant old_value = duplicate->get(property_name, &valid);
			if (!valid) {
				item_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
				item_failed = true;
				break;
			}

			Variant coerced_value;
			const String expected_resource_type = _resource_type_hint_for_property(duplicate, property_name);
			if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, item_error_message)) {
				item_failed = true;
				break;
			}
			if (!_validate_property_value(duplicate, property_name, coerced_value, item_error_message)) {
				item_failed = true;
				break;
			}

			coerced_overrides[property_name] = coerced_value;
		}

		if (item_failed) {
			memdelete(duplicate);
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, item_error_message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic duplicate_batch aborted after validation failure.";
			}
			continue;
		}

		DuplicateBatchOp op;
		op.index = i;
		op.item_id = item_id;
		op.source_path = source_path;
		op.parent = parent;
		op.node = duplicate;
		op.position = (int)item.get("position", -1);
		op.overrides = coerced_overrides;
		operations.push_back(op);
		duplicated_nodes_by_item_id.insert(item_id, duplicate);
	}

	if (atomic_failed) {
		for (const DuplicateBatchOp &op : operations) {
			if (op.node) {
				memdelete(op.node);
			}
			results[op.index] = _make_batch_item_error(op.index, op.item_id, ERROR_CONFLICT, "CONFLICT: atomic duplicate_batch rolled back.", "skipped");
			skipped_count++;
		}

		Dictionary data;
		data["mode"] = _batch_mode_to_string(mode);
		data["applied"] = false;
		data["total_count"] = items.size();
		data["success_count"] = 0;
		data["failure_count"] = failure_count;
		data["skipped_count"] = skipped_count;
		data["results"] = results;
		return _make_ok(data);
	}

	const bool should_apply = !operations.is_empty();
	if (should_apply) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		EditorData &editor_data = EditorNode::get_editor_data();
		undo_redo->create_action_for_history("OpenCode: Duplicate Nodes (Batch)", editor_data.get_current_edited_scene_history_id());

		for (const DuplicateBatchOp &op : operations) {
			undo_redo->add_do_method(op.parent, "add_child", op.node, true);
			if (op.position >= 0) {
				undo_redo->add_do_method(op.parent, "move_child", op.node, op.position);
			}

			Vector<Node *> subtree_nodes;
			_collect_subtree_nodes(op.node, subtree_nodes);
			for (Node *subtree_node : subtree_nodes) {
				if (!subtree_node || subtree_node->is_internal()) {
					continue;
				}
				undo_redo->add_do_method(subtree_node, "set_owner", root);
			}

			for (const KeyValue<Variant, Variant> &E : op.overrides) {
				StringName property = E.key;
				undo_redo->add_do_property(op.node, property, E.value);
			}
			undo_redo->add_do_reference(op.node);
			undo_redo->add_undo_method(op.parent, "remove_child", op.node);
		}

		undo_redo->commit_action();
	}

	int success_count = 0;
	for (const DuplicateBatchOp &op : operations) {
		Dictionary item_data;
		item_data["node_path"] = String(root->get_path_to(op.node));
		item_data["name"] = op.node->get_name();
		item_data["class"] = op.node->get_class();
		item_data["source_path"] = op.source_path;
		results[op.index] = _make_batch_item_success(op.index, op.item_id, item_data);
		success_count++;
	}

	Dictionary data;
	data["mode"] = _batch_mode_to_string(mode);
	data["applied"] = should_apply;
	data["total_count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["skipped_count"] = skipped_count;
	data["results"] = results;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_duplicate(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Dictionary item;
	item["item_id"] = "duplicate_item";
	item["source_path"] = p_params.get("source_path", p_params.get("node_path", String()));
	if (p_params.has("parent_path")) {
		item["parent_path"] = p_params["parent_path"];
	}
	if (p_params.has("new_name")) {
		item["new_name"] = p_params["new_name"];
	}
	if (p_params.has("position")) {
		item["position"] = p_params["position"];
	}
	if (p_params.has("property_overrides")) {
		item["property_overrides"] = p_params["property_overrides"];
	}
	if (p_params.has("properties")) {
		item["properties"] = p_params["properties"];
	}
	if (p_params.has("property_entries")) {
		item["property_entries"] = p_params["property_entries"];
	}

	Dictionary batch_params;
	batch_params["mode"] = "atomic";
	Array items;
	items.push_back(item);
	batch_params["items"] = items;

	Dictionary batch_result = _method_node_duplicate_batch(batch_params, r_error_code, r_error_message);
	if (r_error_code != 0) {
		return Dictionary();
	}

	Dictionary batch_data = batch_result.get("data", Dictionary());
	Array results = batch_data.get("results", Array());
	if (results.is_empty() || results[0].get_type() != Variant::DICTIONARY) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: duplicate_batch returned an invalid response.";
		return Dictionary();
	}

	Dictionary first = results[0];
	if (!bool(first.get("ok", false))) {
		Dictionary error = first.get("error", Dictionary());
		r_error_code = int(error.get("code", ERROR_INTERNAL));
		r_error_message = String(error.get("message", String("INTERNAL: duplicate operation failed.")));
		return Dictionary();
	}

	Dictionary data = first.get("data", Dictionary());
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_delete_batch(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	const Variant items_variant = p_params.get("items", Variant());
	if (items_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty items array is required.";
		return Dictionary();
	}

	Array items = items_variant;
	if (items.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: items must be non-empty.";
		return Dictionary();
	}

	bool force = bool(p_params.get("force", false));
	if (!force) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: destructive operation requires force=true.";
		return Dictionary();
	}

	BatchMode mode = BATCH_MODE_ATOMIC;
	if (!_parse_batch_mode(p_params, mode, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}

	struct DeleteBatchOp {
		int index = -1;
		String item_id;
		String node_path;
		Node *node = nullptr;
		Node *parent = nullptr;
		int old_index = -1;
		int depth = 0;
	};

	Array results;
	results.resize(items.size());
	Vector<DeleteBatchOp> operations;
	HashMap<String, bool> seen_paths;

	int failure_count = 0;
	int skipped_count = 0;
	bool atomic_failed = false;
	String atomic_failure_message = "CONFLICT: skipped due atomic batch failure.";

	for (int i = 0; i < items.size(); i++) {
		String item_id;
		if (atomic_failed) {
			if (items[i].get_type() == Variant::DICTIONARY) {
				Dictionary skipped_item = items[i];
				Variant skipped_item_id_variant = skipped_item.get("item_id", Variant());
				if (skipped_item_id_variant.get_type() == Variant::STRING || skipped_item_id_variant.get_type() == Variant::STRING_NAME) {
					item_id = String(skipped_item_id_variant);
				}
			}
			results[i] = _make_batch_item_error(i, item_id, ERROR_CONFLICT, atomic_failure_message, "skipped");
			skipped_count++;
			continue;
		}

		if (items[i].get_type() != Variant::DICTIONARY) {
			const String message = "INVALID_ARGUMENT: items entries must be dictionaries.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary item = items[i];
		Variant item_id_variant = item.get("item_id", Variant());
		if (item_id_variant.get_type() == Variant::STRING || item_id_variant.get_type() == Variant::STRING_NAME) {
			item_id = String(item_id_variant);
		}

		String node_path = item.get("node_path", String());
		if (node_path.is_empty()) {
			const String message = "INVALID_ARGUMENT: node_path is required.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after validation failure.";
			}
			continue;
		}

		if (seen_paths.has(node_path)) {
			const String message = "INVALID_ARGUMENT: duplicate node_path in delete_batch: " + node_path;
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after validation failure.";
			}
			continue;
		}
		seen_paths.insert(node_path, true);

		Node *node = _resolve_node_path(node_path, root);
		if (!node) {
			const String message = "NOT_FOUND: Node path does not exist.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after item failure.";
			}
			continue;
		}
		if (node == root || node->is_internal()) {
			const String message = "INVALID_ARGUMENT: deleting root/internal nodes is not supported.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after validation failure.";
			}
			continue;
		}

		Node *parent = node->get_parent();
		if (!parent) {
			const String message = "INVALID_ARGUMENT: node has no parent.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic delete_batch aborted after validation failure.";
			}
			continue;
		}

		int depth = 0;
		for (Node *cursor = node; cursor && cursor != root; cursor = cursor->get_parent()) {
			depth++;
		}

		DeleteBatchOp op;
		op.index = i;
		op.item_id = item_id;
		op.node_path = node_path;
		op.node = node;
		op.parent = parent;
		op.old_index = node->get_index(false);
		op.depth = depth;
		operations.push_back(op);
	}

	if (atomic_failed) {
		for (const DeleteBatchOp &op : operations) {
			results[op.index] = _make_batch_item_error(op.index, op.item_id, ERROR_CONFLICT, "CONFLICT: atomic delete_batch rolled back.", "skipped");
			skipped_count++;
		}

		Dictionary data;
		data["mode"] = _batch_mode_to_string(mode);
		data["applied"] = false;
		data["total_count"] = items.size();
		data["success_count"] = 0;
		data["failure_count"] = failure_count;
		data["skipped_count"] = skipped_count;
		data["results"] = results;
		return _make_ok(data);
	}

	Vector<DeleteBatchOp> sorted_operations = operations;
	for (int i = 0; i < sorted_operations.size(); i++) {
		for (int j = i + 1; j < sorted_operations.size(); j++) {
			const bool deeper = sorted_operations[j].depth > sorted_operations[i].depth;
			const bool same_depth_reorder = sorted_operations[j].depth == sorted_operations[i].depth && sorted_operations[j].index < sorted_operations[i].index;
			if (!deeper && !same_depth_reorder) {
				continue;
			}
			DeleteBatchOp swap = sorted_operations[i];
			sorted_operations.write[i] = sorted_operations[j];
			sorted_operations.write[j] = swap;
		}
	}

	const bool should_apply = !sorted_operations.is_empty();
	if (should_apply) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		EditorData &editor_data = EditorNode::get_editor_data();
		undo_redo->create_action_for_history("OpenCode: Delete Nodes (Batch)", editor_data.get_current_edited_scene_history_id());

		for (const DeleteBatchOp &op : sorted_operations) {
			undo_redo->add_do_method(op.parent, "remove_child", op.node);
			undo_redo->add_undo_method(op.parent, "add_child", op.node, true);
			undo_redo->add_undo_method(op.parent, "move_child", op.node, op.old_index);
			undo_redo->add_undo_reference(op.node);
		}

		undo_redo->commit_action();
	}

	int success_count = 0;
	for (const DeleteBatchOp &op : operations) {
		Dictionary item_data;
		item_data["deleted_node_path"] = op.node_path;
		results[op.index] = _make_batch_item_success(op.index, op.item_id, item_data);
		success_count++;
	}

	Dictionary data;
	data["mode"] = _batch_mode_to_string(mode);
	data["applied"] = should_apply;
	data["total_count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["skipped_count"] = skipped_count;
	data["results"] = results;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_delete(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	bool force = bool(p_params.get("force", false));
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}
	if (!force) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: destructive operation requires force=true.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}
	if (node == root || node->is_internal()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: deleting root/internal nodes is not supported.";
		return Dictionary();
	}

	Node *parent = node->get_parent();
	if (!parent) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node has no parent.";
		return Dictionary();
	}

	const int old_index = node->get_index(false);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Delete Node", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node, true);
	undo_redo->add_undo_method(parent, "move_child", node, old_index);
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	Dictionary data;
	data["deleted_node_path"] = node_path;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_reparent(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	String new_parent_path = p_params.get("new_parent_path", String());
	String new_name = p_params.get("new_name", String());
	int position = (int)p_params.get("position", -1);
	if (node_path.is_empty() || new_parent_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and new_parent_path are required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	Node *new_parent = _resolve_node_path(new_parent_path, root);
	if (!node || !new_parent) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: node_path or new_parent_path does not exist.";
		return Dictionary();
	}
	if (node == root || node == new_parent || node->is_ancestor_of(new_parent) || node->is_internal()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: invalid reparent operation.";
		return Dictionary();
	}

	Node *old_parent = node->get_parent();
	if (!old_parent) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node has no parent.";
		return Dictionary();
	}

	const int old_index = node->get_index(false);
	const String old_name = node->get_name();
	Node *old_owner = node->get_owner();

	if (new_name.is_empty()) {
		new_name = old_name;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Reparent Node", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true);
	if (position >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, position);
	}
	if (new_name != old_name) {
		undo_redo->add_do_method(node, "set_name", new_name);
		undo_redo->add_undo_method(node, "set_name", old_name);
	}
	undo_redo->add_do_method(node, "set_owner", root);
	undo_redo->add_undo_method(node, "set_owner", old_owner);
	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node, true);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = String(root->get_path_to(node));
	return _make_ok(data);
}

bool OpenCodeMCPProtocol::_validate_property_value(Node *p_node, const StringName &p_property_name, const Variant &p_value, String &r_error_message) const {
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	if (!node_3d) {
		return true;
	}

	const String property_name = String(p_property_name);
	if (property_name == "scale") {
		if (p_value.get_type() != Variant::VECTOR3) {
			r_error_message = "INVALID_ARGUMENT: scale must be a Vector3.";
			return false;
		}

		const Vector3 scale = p_value;
		if (Math::is_zero_approx(scale.x) || Math::is_zero_approx(scale.y) || Math::is_zero_approx(scale.z)) {
			r_error_message = "INVALID_ARGUMENT: scale components must be non-zero.";
			return false;
		}
	}

	if (property_name == "basis" || property_name == "global_basis") {
		if (p_value.get_type() != Variant::BASIS) {
			r_error_message = "INVALID_ARGUMENT: basis must be a Basis.";
			return false;
		}

		const Basis basis = p_value;
		if (Math::is_zero_approx(basis.determinant())) {
			r_error_message = "INVALID_ARGUMENT: basis must be invertible (determinant != 0).";
			return false;
		}
	}

	if (property_name == "transform" || property_name == "global_transform") {
		if (p_value.get_type() != Variant::TRANSFORM3D) {
			r_error_message = "INVALID_ARGUMENT: transform must be a Transform3D.";
			return false;
		}

		const Transform3D transform = p_value;
		if (Math::is_zero_approx(transform.basis.determinant())) {
			r_error_message = "INVALID_ARGUMENT: transform basis must be invertible (determinant != 0).";
			return false;
		}
	}

	return true;
}

Dictionary OpenCodeMCPProtocol::_method_node_set_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	Dictionary properties;
	if (!_collect_properties_from_payload(p_params, properties, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}

	if (node_path.is_empty() || properties.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and non-empty properties (or property_entries) are required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Dictionary previous_values;
	Dictionary coerced_properties;
	for (const KeyValue<Variant, Variant> &E : properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = node->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}

		Variant coerced_value;
		const String expected_resource_type = _resource_type_hint_for_property(node, property_name);
		if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, r_error_message)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}
		if (!expected_resource_type.is_empty() && coerced_value.get_type() == Variant::NIL) {
			r_error_message = "INVALID_ARGUMENT: resource property '" + String(property_name) + "' cannot be null.";
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}

		if (!_validate_property_value(node, property_name, coerced_value, r_error_message)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}

		previous_values[property_name] = old_value;
		coerced_properties[property_name] = coerced_value;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Node Properties", editor_data.get_current_edited_scene_history_id());

	for (const KeyValue<Variant, Variant> &E : coerced_properties) {
		StringName property_name = E.key;
		undo_redo->add_do_property(node, property_name, E.value);
		undo_redo->add_undo_property(node, property_name, previous_values[property_name]);
	}

	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["property_count"] = properties.size();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_node_set_properties_batch(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	const Variant items_variant = p_params.get("items", Variant());
	if (items_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: non-empty items array is required.";
		return Dictionary();
	}

	Array items = items_variant;
	if (items.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: items must be non-empty.";
		return Dictionary();
	}

	BatchMode mode = BATCH_MODE_ATOMIC;
	if (!_parse_batch_mode(p_params, mode, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}

	struct SetPropertiesBatchOp {
		int index = -1;
		String item_id;
		String node_path;
		Node *node = nullptr;
		Dictionary previous_values;
		Dictionary coerced_properties;
	};

	Array results;
	results.resize(items.size());
	Vector<SetPropertiesBatchOp> operations;

	int failure_count = 0;
	int skipped_count = 0;
	bool atomic_failed = false;
	String atomic_failure_message = "CONFLICT: skipped due atomic batch failure.";

	for (int i = 0; i < items.size(); i++) {
		String item_id;
		if (atomic_failed) {
			if (items[i].get_type() == Variant::DICTIONARY) {
				Dictionary skipped_item = items[i];
				Variant skipped_item_id_variant = skipped_item.get("item_id", Variant());
				if (skipped_item_id_variant.get_type() == Variant::STRING || skipped_item_id_variant.get_type() == Variant::STRING_NAME) {
					item_id = String(skipped_item_id_variant);
				}
			}
			results[i] = _make_batch_item_error(i, item_id, ERROR_CONFLICT, atomic_failure_message, "skipped");
			skipped_count++;
			continue;
		}

		if (items[i].get_type() != Variant::DICTIONARY) {
			const String message = "INVALID_ARGUMENT: items entries must be dictionaries.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic set_properties_batch aborted after validation failure.";
			}
			continue;
		}

		Dictionary item = items[i];
		Variant item_id_variant = item.get("item_id", Variant());
		if (item_id_variant.get_type() == Variant::STRING || item_id_variant.get_type() == Variant::STRING_NAME) {
			item_id = String(item_id_variant);
		}

		String node_path = item.get("node_path", String());
		Dictionary properties;
		String properties_error;
		if (!_collect_properties_from_payload(item, properties, properties_error)) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, properties_error);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic set_properties_batch aborted after validation failure.";
			}
			continue;
		}

		if (node_path.is_empty() || properties.is_empty()) {
			const String message = "INVALID_ARGUMENT: node_path and non-empty properties (or property_entries) are required.";
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic set_properties_batch aborted after validation failure.";
			}
			continue;
		}

		Node *node = _resolve_node_path(node_path, root);
		if (!node) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_NOT_FOUND, "NOT_FOUND: Node path does not exist.");
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic set_properties_batch aborted after item failure.";
			}
			continue;
		}

		Dictionary previous_values;
		Dictionary coerced_properties;
		bool item_failed = false;
		String item_error_message;

		for (const KeyValue<Variant, Variant> &E : properties) {
			StringName property_name = E.key;
			bool valid = false;
			Variant old_value = node->get(property_name, &valid);
			if (!valid) {
				item_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
				item_failed = true;
				break;
			}

			Variant coerced_value;
			const String expected_resource_type = _resource_type_hint_for_property(node, property_name);
			if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, item_error_message)) {
				item_failed = true;
				break;
			}
			if (!expected_resource_type.is_empty() && coerced_value.get_type() == Variant::NIL) {
				item_error_message = "INVALID_ARGUMENT: resource property '" + String(property_name) + "' cannot be null.";
				item_failed = true;
				break;
			}

			if (!_validate_property_value(node, property_name, coerced_value, item_error_message)) {
				item_failed = true;
				break;
			}

			previous_values[property_name] = old_value;
			coerced_properties[property_name] = coerced_value;
		}

		if (item_failed) {
			results[i] = _make_batch_item_error(i, item_id, ERROR_INVALID_ARGUMENT, item_error_message);
			failure_count++;
			if (mode == BATCH_MODE_ATOMIC) {
				atomic_failed = true;
				atomic_failure_message = "CONFLICT: atomic set_properties_batch aborted after validation failure.";
			}
			continue;
		}

		SetPropertiesBatchOp op;
		op.index = i;
		op.item_id = item_id;
		op.node_path = node_path;
		op.node = node;
		op.previous_values = previous_values;
		op.coerced_properties = coerced_properties;
		operations.push_back(op);
	}

	if (atomic_failed) {
		for (const SetPropertiesBatchOp &op : operations) {
			results[op.index] = _make_batch_item_error(op.index, op.item_id, ERROR_CONFLICT, "CONFLICT: atomic set_properties_batch rolled back.", "skipped");
			skipped_count++;
		}

		Dictionary data;
		data["mode"] = _batch_mode_to_string(mode);
		data["applied"] = false;
		data["total_count"] = items.size();
		data["success_count"] = 0;
		data["failure_count"] = failure_count;
		data["skipped_count"] = skipped_count;
		data["results"] = results;
		return _make_ok(data);
	}

	if (!operations.is_empty()) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		EditorData &editor_data = EditorNode::get_editor_data();
		undo_redo->create_action_for_history("OpenCode: Set Node Properties (Batch)", editor_data.get_current_edited_scene_history_id());

		for (const SetPropertiesBatchOp &op : operations) {
			for (const KeyValue<Variant, Variant> &E : op.coerced_properties) {
				StringName property_name = E.key;
				undo_redo->add_do_property(op.node, property_name, E.value);
				undo_redo->add_undo_property(op.node, property_name, op.previous_values[property_name]);
			}
		}

		undo_redo->commit_action();
	}

	int success_count = 0;
	for (const SetPropertiesBatchOp &op : operations) {
		Dictionary item_data;
		item_data["node_path"] = op.node_path;
		item_data["property_count"] = op.coerced_properties.size();
		results[op.index] = _make_batch_item_success(op.index, op.item_id, item_data);
		success_count++;
	}

	Dictionary data;
	data["mode"] = _batch_mode_to_string(mode);
	data["applied"] = !operations.is_empty();
	data["total_count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["skipped_count"] = skipped_count;
	data["results"] = results;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_script_get_active(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String workspace = _resolve_script_workspace(p_params, r_error_code, r_error_message);
	if (workspace.is_empty()) {
		return Dictionary();
	}

	Dictionary data;
	if (workspace == SCRIPT_WORKSPACE_SCRIPT_EDITOR) {
		ScriptEditorBase *active_editor = nullptr;
		Ref<Script> script;
		String script_path;
		String display_name;
		bool is_unsaved = false;
		String source;
		bool has_text_buffer = false;
		if (!_resolve_active_script_editor_context(active_editor, script, script_path, display_name, is_unsaved, source, has_text_buffer, r_error_code, r_error_message)) {
			return Dictionary();
		}
		(void)active_editor;
		(void)has_text_buffer;

		data["workspace"] = workspace;
		data["script_path"] = script_path;
		data["display_name"] = display_name;
		data["is_built_in"] = script->is_built_in();
		data["is_unsaved"] = is_unsaved;
		data["source"] = source;
		data["version"] = source.md5_text();
		return _make_ok(data);
	}

	String script_path;
	String node_path;
	Ref<Script> script = _resolve_selected_scene_node_script(script_path, node_path, r_error_code, r_error_message);
	if (script.is_null()) {
		return Dictionary();
	}

	const String source = script->get_source_code();
	data["workspace"] = workspace;
	data["script_path"] = script_path;
	data["node_path"] = node_path;
	data["is_built_in"] = script->is_built_in();
	data["is_unsaved"] = false;
	data["source"] = source;
	data["version"] = source.md5_text();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_script_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String workspace = _resolve_script_workspace(p_params, r_error_code, r_error_message);
	if (workspace.is_empty()) {
		return Dictionary();
	}

	const String script_path_param = p_params.get("script_path", String());
	const String node_path_param = p_params.get("node_path", String());
	const bool workspace_requested = p_params.has("workspace");
	if (workspace == SCRIPT_WORKSPACE_SCRIPT_EDITOR && node_path_param.is_empty() && (workspace_requested || script_path_param.is_empty())) {
		ScriptEditorBase *active_editor = nullptr;
		Ref<Script> script;
		String script_path;
		String display_name;
		bool is_unsaved = false;
		String source;
		bool has_text_buffer = false;
		if (!_resolve_active_script_editor_context(active_editor, script, script_path, display_name, is_unsaved, source, has_text_buffer, r_error_code, r_error_message)) {
			return Dictionary();
		}
		(void)active_editor;
		(void)script;
		(void)display_name;
		(void)is_unsaved;
		(void)has_text_buffer;
		if (!_validate_script_editor_target(script_path_param, script_path, r_error_code, r_error_message)) {
			return Dictionary();
		}

		Dictionary data;
		data["script_path"] = script_path;
		data["source"] = source;
		data["version"] = source.md5_text();
		return _make_ok(data);
	}

	if (script_path_param.is_empty() && node_path_param.is_empty()) {
		return _method_script_get_active(p_params, r_error_code, r_error_message);
	}

	String script_path;
	Ref<Script> script = _resolve_script(p_params, script_path, r_error_code, r_error_message);
	if (script.is_null()) {
		return Dictionary();
	}

	const String source = script->get_source_code();
	Dictionary data;
	data["script_path"] = script_path;
	data["source"] = source;
	data["version"] = source.md5_text();
	return _make_ok(data);
}

int OpenCodeMCPProtocol::_line_col_to_index(const String &p_text, int p_line, int p_col) {
	if (p_line < 0 || p_col < 0) {
		return -1;
	}

	int line = 0;
	int index = 0;
	const int text_length = p_text.length();
	while (index < text_length && line < p_line) {
		if (p_text[index] == '\n') {
			line++;
		}
		index++;
	}
	if (line != p_line) {
		return -1;
	}

	int col = 0;
	while (index < text_length && p_text[index] != '\n' && col < p_col) {
		col++;
		index++;
	}
	if (col != p_col) {
		return -1;
	}

	return index;
}

bool OpenCodeMCPProtocol::_apply_text_edits_to_source(const String &p_source, const Array &p_edits, String &r_updated_source, String &r_error_message) {
	Vector<TextEditRange> edits;
	edits.resize(p_edits.size());

	for (int i = 0; i < p_edits.size(); i++) {
		if (p_edits[i].get_type() != Variant::DICTIONARY) {
			r_error_message = "INVALID_ARGUMENT: each edit entry must be a Dictionary.";
			return false;
		}
		Dictionary edit = p_edits[i];
		const int start_line = (int)edit.get("start_line", -1);
		const int start_col = (int)edit.get("start_col", -1);
		const int end_line = (int)edit.get("end_line", -1);
		const int end_col = (int)edit.get("end_col", -1);
		const String new_text = edit.get("new_text", String());

		if (start_line < 0 || start_col < 0 || end_line < 0 || end_col < 0) {
			r_error_message = "INVALID_ARGUMENT: edit coordinates must be zero-based non-negative integers at edits[" + itos(i) + "].";
			return false;
		}

		const int from = _line_col_to_index(p_source, start_line, start_col);
		const int to = _line_col_to_index(p_source, end_line, end_col);
		if (from < 0 || to < 0 || from > to) {
			r_error_message = "INVALID_ARGUMENT: invalid edit range at edits[" + itos(i) + "] "
						  "(start_line=" + itos(start_line) + ", start_col=" + itos(start_col) +
						  ", end_line=" + itos(end_line) + ", end_col=" + itos(end_col) +
						  "). Coordinates must be zero-based and within the current source.";
			return false;
		}

		edits.write[i].from = from;
		edits.write[i].to = to;
		edits.write[i].text = new_text;
	}

	for (int i = 1; i < edits.size(); i++) {
		int j = i;
		while (j > 0 && edits[j - 1].from < edits[j].from) {
			TextEditRange tmp = edits[j - 1];
			edits.write[j - 1] = edits[j];
			edits.write[j] = tmp;
			j--;
		}
	}

	int last_start = p_source.length() + 1;
	for (int i = 0; i < edits.size(); i++) {
		if (edits[i].to > last_start) {
			r_error_message = "INVALID_ARGUMENT: overlapping edits are not supported; conflict at edits[" + itos(i) + "].";
			return false;
		}
		last_start = edits[i].from;
	}

	String updated = p_source;
	for (int i = 0; i < edits.size(); i++) {
		const TextEditRange &edit = edits[i];
		updated = updated.substr(0, edit.from) + edit.text + updated.substr(edit.to, updated.length() - edit.to);
	}

	r_updated_source = updated;
	return true;
}

Dictionary OpenCodeMCPProtocol::_method_script_apply_text_edits(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Variant edits_variant = p_params.get("edits", Variant());
	if (edits_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: edits must be an Array.";
		return Dictionary();
	}
	Array edits = edits_variant;
	if (edits.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: edits must be non-empty.";
		return Dictionary();
	}

	String workspace = _resolve_script_workspace(p_params, r_error_code, r_error_message);
	if (workspace.is_empty()) {
		return Dictionary();
	}

	String script_path = p_params.get("script_path", String());
	String node_path = p_params.get("node_path", String());
	const bool workspace_requested = p_params.has("workspace");
	Ref<Script> script;
	String current_source;
	bool has_script_editor_source = false;
	ScriptEditorBase *target_editor = nullptr;
	if (workspace == SCRIPT_WORKSPACE_SCRIPT_EDITOR && node_path.is_empty() && (workspace_requested || script_path.is_empty())) {
		String active_script_path;
		String display_name;
		bool is_unsaved = false;
		bool has_text_buffer = false;
		if (!_resolve_active_script_editor_context(target_editor, script, active_script_path, display_name, is_unsaved, current_source, has_text_buffer, r_error_code, r_error_message)) {
			return Dictionary();
		}
		if (!_validate_script_editor_target(script_path, active_script_path, r_error_code, r_error_message)) {
			return Dictionary();
		}
		script_path = active_script_path;
		has_script_editor_source = has_text_buffer;
	} else if (script_path.is_empty() && node_path.is_empty()) {
		script = _resolve_selected_scene_node_script(script_path, node_path, r_error_code, r_error_message);
		if (script.is_null()) {
			return Dictionary();
		}
	} else {
		script = _resolve_script(p_params, script_path, r_error_code, r_error_message);
	}
	if (script.is_null()) {
		return Dictionary();
	}

	if (!has_script_editor_source) {
		String open_editor_source;
		bool open_has_text_buffer = false;
		if (_resolve_open_script_editor_context(script, script_path, target_editor, open_editor_source, open_has_text_buffer)) {
			has_script_editor_source = open_has_text_buffer;
			current_source = open_editor_source;
		}
	}

	if (!has_script_editor_source) {
		current_source = script->get_source_code();
	}
	String expected_version = p_params.get("expected_version", String());
	if (!expected_version.is_empty() && expected_version != current_source.md5_text()) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: script version mismatch.";
		return Dictionary();
	}

	String updated_source;
	String edit_error;
	if (!_apply_text_edits_to_source(current_source, edits, updated_source, edit_error)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = edit_error;
		return Dictionary();
	}
	if (updated_source == current_source) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: edits produced no source changes.";
		return Dictionary();
	}

	bool applied_via_script_editor_buffer = false;
	if (target_editor) {
		applied_via_script_editor_buffer = _set_script_editor_buffer_source(target_editor, updated_source);
		if (applied_via_script_editor_buffer) {
			target_editor->apply_code();
		}
	}

	if (!applied_via_script_editor_buffer) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		EditorData &editor_data = EditorNode::get_editor_data();
		undo_redo->create_action_for_history("OpenCode: Apply Script Edits", editor_data.get_current_edited_scene_history_id());
		undo_redo->add_do_method(script.ptr(), "set_source_code", updated_source);
		undo_redo->add_undo_method(script.ptr(), "set_source_code", current_source);

		undo_redo->commit_action();
		script->update_exports();
	}

	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor) {
		script_editor->notify_script_changed(script);
		if (!script_path.is_empty()) {
			script_editor->trigger_live_script_reload(script_path);
		}
	}

	Dictionary data;
	data["script_path"] = script_path;
	if (!node_path.is_empty()) {
		data["node_path"] = node_path;
	}
	data["version"] = updated_source.md5_text();
	Array warnings;
	warnings.push_back("Script text updated in editor state; call resource.save to persist to disk.");
	return _make_ok(data, warnings);
}

Dictionary OpenCodeMCPProtocol::_method_script_attach(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	const bool has_built_in = p_params.has("built_in");
	const bool has_script_path = p_params.has("script_path") && !String(p_params.get("script_path", String())).is_empty();
	if (has_built_in && has_script_path) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide only one of script_path or built_in for script.attach.";
		return Dictionary();
	}

	if (has_built_in) {
		return _method_script_attach_built_in(p_params, r_error_code, r_error_message);
	}

	Dictionary params = p_params;
	if (!has_script_path) {
		String workspace = _resolve_script_workspace(params, r_error_code, r_error_message);
		if (workspace.is_empty()) {
			return Dictionary();
		}

		if (workspace == SCRIPT_WORKSPACE_SCRIPT_EDITOR) {
			ScriptEditorBase *active_editor = nullptr;
			Ref<Script> script;
			String active_script_path;
			String display_name;
			bool is_unsaved = false;
			String source;
			bool has_text_buffer = false;
			if (!_resolve_active_script_editor_context(active_editor, script, active_script_path, display_name, is_unsaved, source, has_text_buffer, r_error_code, r_error_message)) {
				return Dictionary();
			}
			(void)active_editor;
			(void)display_name;
			(void)is_unsaved;
			(void)source;
			(void)has_text_buffer;

			if (!active_script_path.is_empty()) {
				params["script_path"] = active_script_path;
			}
		}
	}

	return _method_script_attach_external(params, r_error_code, r_error_message);
}

Dictionary OpenCodeMCPProtocol::_method_script_attach_external(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String workspace = _resolve_script_workspace(p_params, r_error_code, r_error_message);
	if (workspace.is_empty()) {
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		if (!_resolve_selected_scene_node(node_path, r_error_code, r_error_message)) {
			return Dictionary();
		}
	}

	String script_path = p_params.get("script_path", String());
	if (p_params.has("built_in")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: built_in is not allowed for script.attach_external; use script.attach_built_in.";
		return Dictionary();
	}

	if (script_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script_path is required for script.attach_external.";
		return Dictionary();
	}
	if (!script_path.begins_with("res://") || script_path.contains("::")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script_path must be a res:// file path for script.attach_external.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Ref<Script> script = ResourceLoader::load(script_path);
	if (script.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Script resource could not be loaded.";
		return Dictionary();
	}

	Ref<Script> old_script = node->get_script();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Attach External Script", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", old_script);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["script_path"] = script_path;
	data["is_built_in"] = false;
	data["created_built_in"] = false;
	data["workspace"] = workspace;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_script_attach_built_in(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String workspace = _resolve_script_workspace(p_params, r_error_code, r_error_message);
	if (workspace.is_empty()) {
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		if (!_resolve_selected_scene_node(node_path, r_error_code, r_error_message)) {
			return Dictionary();
		}
	}

	if (p_params.has("script_path")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script_path is not allowed for script.attach_built_in; use script.attach_external.";
		return Dictionary();
	}
	String script_path;

	Dictionary built_in = p_params.get("built_in", Dictionary());
	if (built_in.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: built_in is required for script.attach_built_in.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Ref<Script> old_script = node->get_script();
	if (old_script.is_valid() && !old_script->is_built_in()) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: refusing to replace external script with built-in script; use script.attach_external.";
		return Dictionary();
	}

	String language_name = String(built_in.get("language", "gdscript")).to_lower();
	ScriptLanguage *language = _find_script_language(language_name);
	if (!language) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: unknown script language: " + language_name;
		return Dictionary();
	}
	if (!language->supports_builtin_mode()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script language does not support built-in scripts.";
		return Dictionary();
	}

	String source = built_in.get("source", String());
	String script_name = built_in.get("name", String());
	if (source.is_empty()) {
		source = "extends " + node->get_class() + "\n";
	}
	Ref<Script> script = language->make_template(source, node->get_name(), node->get_class());
	if (script.is_null()) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: failed to create built-in script resource.";
		return Dictionary();
	}
	if (!script_name.is_empty()) {
		script->set_name(script_name);
	}

	const String scene_path = root->get_scene_file_path();
	if (scene_path.is_empty()) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: built-in script attach requires a saved scene path.";
		return Dictionary();
	}
	script->set_path(scene_path + "::" + script->generate_scene_unique_id());
	script->reload();
	script_path = script->get_path();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Attach Built-in Script", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", old_script);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["script_path"] = script_path;
	data["is_built_in"] = true;
	data["created_built_in"] = true;
	data["workspace"] = workspace;
	Array warnings;
	warnings.push_back("Attached built-in script is in editor state; call resource.save on the scene to persist.");
	return _make_ok(data, warnings);
}

Dictionary OpenCodeMCPProtocol::_method_lsp_query(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
#ifndef MODULE_GDSCRIPT_ENABLED
	r_error_code = ERROR_NOT_FOUND;
	r_error_message = "NOT_FOUND: GDScript module is not enabled in this build.";
	return Dictionary();
#else
	String operation = String(p_params.get("operation", String())).strip_edges();
	if (operation.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: operation is required.";
		return Dictionary();
	}

	String normalized_operation = operation.to_lower();
	if (normalized_operation == "gotodefinition") {
		normalized_operation = "definition";
	} else if (normalized_operation == "findreferences") {
		normalized_operation = "references";
	} else if (normalized_operation == "documentsymbol") {
		normalized_operation = "document_symbol";
	}

	if (normalized_operation != "definition" && normalized_operation != "references" && normalized_operation != "hover" && normalized_operation != "document_symbol") {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: unsupported operation. Supported operations are goToDefinition, findReferences, hover, and documentSymbol.";
		return Dictionary();
	}

	Dictionary script_params = p_params;
	if (!script_params.has("script_path") && script_params.has("filePath")) {
		script_params["script_path"] = script_params["filePath"];
	}

	int script_error_code = 0;
	String script_error_message;
	Dictionary script_result = _method_script_get(script_params, script_error_code, script_error_message);
	if (script_error_code != 0) {
		r_error_code = script_error_code;
		r_error_message = script_error_message;
		return Dictionary();
	}

	Dictionary script_data = script_result.get("data", Dictionary());
	String script_path = script_data.get("script_path", String());
	String source = script_data.get("source", String());
	if (script_path.is_empty() && script_params.has("script_path")) {
		script_path = script_params["script_path"];
	}

	if (script_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: resolved script has no file path; use script_path or a saved script editor tab.";
		return Dictionary();
	}

	const String script_path_lower = script_path.to_lower();
	if (!script_path_lower.ends_with(".gd") && !script_path.contains("::GDScript")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: lsp.query currently supports GDScript scripts only.";
		return Dictionary();
	}

	ExtendGDScriptParser parser;
	parser.parse(source, script_path);
	if (parser.parse_result != OK) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: unable to parse script source for LSP query.";
		return Dictionary();
	}

	if (normalized_operation == "document_symbol") {
		Array symbols;
		symbols.push_back(parser.get_symbols().to_json(true));

		Dictionary data;
		data["operation"] = operation;
		data["filePath"] = script_path;
		data["symbols"] = symbols;
		return _make_ok(data);
	}

	const int line = (int)p_params.get("line", -1);
	const int character = (int)p_params.get("character", -1);
	if (line < 0 || character < 0) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: line and character are required and must be >= 0.";
		return Dictionary();
	}

	LSP::Position position;
	position.line = line;
	position.character = character;

	LSP::Range identifier_range;
	const String identifier = parser.get_identifier_under_position(position, identifier_range);
	if (identifier.is_empty()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: no identifier found at the provided position.";
		return Dictionary();
	}

	if (normalized_operation == "references") {
		const bool include_declaration = bool(p_params.get("include_declaration", true));
		const bool workspace_search = bool(p_params.get("workspace_search", false));
		const String search_root = String(p_params.get("search_root", "res://"));
		int file_limit = (int)p_params.get("file_limit", 300);
		if (file_limit < 1) {
			file_limit = 1;
		}

		Array locations;
		_append_identifier_occurrences(source, identifier, script_path, locations);

		if (workspace_search && search_root.begins_with("res://")) {
			Vector<String> extensions;
			extensions.push_back("gd");

			Vector<String> script_files;
			bool truncated = false;
			_collect_files_recursive(search_root, extensions, true, file_limit, script_files, truncated);
			for (int i = 0; i < script_files.size(); i++) {
				const String &candidate_path = script_files[i];
				if (candidate_path == script_path) {
					continue;
				}
				Ref<FileAccess> file = FileAccess::open(candidate_path, FileAccess::READ);
				if (file.is_null()) {
					continue;
				}
				_append_identifier_occurrences(file->get_as_utf8_string(), identifier, candidate_path, locations);
			}
		}

		if (!include_declaration) {
			Array filtered_locations;
			const int declaration_line = identifier_range.start.line;
			const int declaration_character = identifier_range.start.character;
			for (int i = 0; i < locations.size(); i++) {
				Dictionary location = locations[i];
				if ((int)location.get("line", -1) == declaration_line && (int)location.get("character", -1) == declaration_character && String(location.get("filePath", String())) == script_path) {
					continue;
				}
				filtered_locations.push_back(location);
			}
			locations = filtered_locations;
		}

		Dictionary data;
		data["operation"] = operation;
		data["filePath"] = script_path;
		data["identifier"] = identifier;
		data["locations"] = locations;
		data["count"] = locations.size();
		return _make_ok(data);
	}

	GDScriptLanguage *gdscript_language = GDScriptLanguage::get_singleton();
	if (!gdscript_language) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: GDScript language singleton is not available.";
		return Dictionary();
	}

	String lookup_source = parser.get_text_for_lookup_symbol(position, identifier);
	if (lookup_source.is_empty()) {
		const int source_index = _line_col_to_index(source, position.line, position.character);
		if (source_index >= 0) {
			lookup_source = source.insert(source_index, String::chr(0xFFFF));
		}
	}

	if (lookup_source.is_empty()) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: unable to derive lookup source for symbol resolution.";
		return Dictionary();
	}

	ScriptLanguage::LookupResult lookup_result;
	Error lookup_error = gdscript_language->lookup_code(lookup_source, identifier, script_path, nullptr, lookup_result);
	if (lookup_error != OK) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: symbol lookup failed at the provided position.";
		return Dictionary();
	}

	String resolved_script_path = lookup_result.script_path;
	if (resolved_script_path.is_empty() && lookup_result.script.is_valid()) {
		resolved_script_path = lookup_result.script->get_path();
	}

	if (normalized_operation == "definition") {
		Array locations;
		if (lookup_result.location >= 0) {
			Dictionary location;
			location["filePath"] = resolved_script_path.is_empty() ? script_path : resolved_script_path;
			location["line"] = lookup_result.location;
			location["character"] = 0;
			locations.push_back(location);
		}

		Dictionary data;
		data["operation"] = operation;
		data["filePath"] = script_path;
		data["identifier"] = identifier;
		data["lookup_type"] = _lookup_result_type_to_string(lookup_result.type);
		if (!lookup_result.class_name.is_empty()) {
			data["class_name"] = lookup_result.class_name;
		}
		if (!lookup_result.class_member.is_empty()) {
			data["class_member"] = lookup_result.class_member;
		}
		data["locations"] = locations;
		return _make_ok(data);
	}

	String hover_contents = "`" + _lookup_result_type_to_string(lookup_result.type) + "`: `" + identifier + "`";
	if (!lookup_result.class_name.is_empty()) {
		hover_contents += "\nClass: `" + lookup_result.class_name + "`";
	}
	if (!lookup_result.class_member.is_empty()) {
		hover_contents += "\nMember: `" + lookup_result.class_member + "`";
	}
	if (!lookup_result.doc_type.is_empty()) {
		hover_contents += "\nType: `" + lookup_result.doc_type + "`";
	}
	if (!lookup_result.description.is_empty()) {
		hover_contents += "\n\n" + lookup_result.description;
	}
	if (!lookup_result.value.is_empty()) {
		hover_contents += "\nValue: `" + lookup_result.value + "`";
	}
	if (!resolved_script_path.is_empty() && lookup_result.location >= 0) {
		hover_contents += "\nDefined at: `" + resolved_script_path + ":" + itos(lookup_result.location) + "`";
	}

	Dictionary data;
	data["operation"] = operation;
	data["filePath"] = script_path;
	data["identifier"] = identifier;
	data["contents"] = hover_contents;
	data["lookup_type"] = _lookup_result_type_to_string(lookup_result.type);
	return _make_ok(data);
#endif
}

Dictionary OpenCodeMCPProtocol::_method_resource_save(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String path = p_params.get("path", String());
	Node *root = _get_edited_scene_root();

	if (path.is_empty()) {
		if (!root || root->get_scene_file_path().is_empty()) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: no path provided and no active scene to save.";
			return Dictionary();
		}
		path = root->get_scene_file_path();
	}

	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}

	const String extension = path.get_extension().to_lower();
	if (extension == "tscn" || extension == "scn") {
		if (!root) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: no active edited scene.";
			return Dictionary();
		}

		EditorData &editor_data = EditorNode::get_editor_data();
		int scene_idx = editor_data.get_edited_scene_from_path(path);
		if (scene_idx < 0 && root->get_scene_file_path() == path) {
			scene_idx = editor_data.get_edited_scene();
		}

		const uint64_t disk_modified_time = FileAccess::get_modified_time(path);
		if (scene_idx >= 0) {
			const uint64_t tracked_modified_time = editor_data.get_scene_modified_time(scene_idx);
			if (tracked_modified_time > 0 && disk_modified_time > tracked_modified_time) {
				r_error_code = ERROR_CONFLICT;
				r_error_message = "CONFLICT: path changed on disk since it was loaded. Call resource.reload before resource.save.";
				return Dictionary();
			}
		}

		// Save through the full editor path to match manual scene saves.
		EditorNode::get_singleton()->save_scene_to_path(path, true);

		// Keep external-change detection in sync for the just-saved scene.
		scene_idx = editor_data.get_edited_scene_from_path(path);
		if (scene_idx < 0) {
			if (root->get_scene_file_path() == path) {
				scene_idx = editor_data.get_edited_scene();
			}
		}
		if (scene_idx >= 0) {
			editor_data.set_scene_modified_time(scene_idx, FileAccess::get_modified_time(path));
		}
	} else {
		Ref<Resource> resource = ResourceCache::get_ref(path);
		if (resource.is_null()) {
			resource = ResourceLoader::load(path);
		}
		if (resource.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: resource path could not be loaded.";
			return Dictionary();
		}

		const uint64_t disk_modified_time = FileAccess::get_modified_time(path);
		const uint64_t tracked_modified_time = resource->get_last_modified_time();
		if (tracked_modified_time > 0 && disk_modified_time > tracked_modified_time) {
			r_error_code = ERROR_CONFLICT;
			r_error_message = "CONFLICT: path changed on disk since it was loaded. Call resource.reload before resource.save.";
			return Dictionary();
		}

		Error save_err = ResourceSaver::save(resource, path);
		if (save_err != OK) {
			r_error_code = ERROR_INTERNAL;
			r_error_message = "INTERNAL: failed to save resource.";
			return Dictionary();
		}
		resource->set_last_modified_time(FileAccess::get_modified_time(path));

		// Mirror editor-side notifications so open resource tabs stay in sync.
		EditorNode *editor_node = EditorNode::get_singleton();
		if (editor_node) {
			editor_node->emit_signal(SNAME("resource_saved"), resource);
			EditorNode::get_editor_data().notify_resource_saved(resource);
		}

		if (ScriptEditor::get_singleton()) {
			ScriptEditor::get_singleton()->update_script_times();
		}
	}

	Dictionary data;
	data["path"] = path;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_resource_reload(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String path = p_params.get("path", String());
	Node *root = _get_edited_scene_root();

	if (path.is_empty()) {
		if (!root || root->get_scene_file_path().is_empty()) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: no path provided and no active scene to reload.";
			return Dictionary();
		}
		path = root->get_scene_file_path();
	}

	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}

	if (!FileAccess::exists(path)) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: resource path does not exist on disk.";
		return Dictionary();
	}

	const String extension = path.get_extension().to_lower();
	if (extension == "tscn" || extension == "scn") {
		EditorNode *editor_node = EditorNode::get_singleton();
		if (!editor_node) {
			r_error_code = ERROR_INTERNAL;
			r_error_message = "INTERNAL: EditorNode not available.";
			return Dictionary();
		}

		editor_node->reload_scene(path);

		EditorData &editor_data = EditorNode::get_editor_data();
		const int scene_idx = editor_data.get_edited_scene_from_path(path);
		if (scene_idx >= 0) {
			editor_data.set_scene_modified_time(scene_idx, FileAccess::get_modified_time(path));
		}
	} else {
		Ref<Resource> resource = ResourceCache::get_ref(path);
		if (resource.is_null()) {
			resource = ResourceLoader::load(path);
		}
		if (resource.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: resource path could not be loaded.";
			return Dictionary();
		}
		if (!resource->editor_can_reload_from_file()) {
			r_error_code = ERROR_CONFLICT;
			r_error_message = "CONFLICT: resource type does not support reload_from_file.";
			return Dictionary();
		}

		resource->reload_from_file();
		resource->set_last_modified_time(FileAccess::get_modified_time(path));

		EditorNode::get_editor_data().notify_resource_saved(resource);
		if (ScriptEditor::get_singleton()) {
			ScriptEditor::get_singleton()->update_script_times();
		}
	}

	Dictionary data;
	data["path"] = path;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// Helper: collect files recursively from a directory.
// ---------------------------------------------------------------------------

void OpenCodeMCPProtocol::_collect_files_recursive(const String &p_dir, const Vector<String> &p_extensions,
		bool p_recursive, int p_limit, Vector<String> &r_results, bool &r_truncated) const {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	String item = dir->get_next();
	while (!item.is_empty()) {
		if (r_results.size() >= p_limit) {
			r_truncated = true;
			break;
		}

		String full_path = p_dir.path_join(item);
		if (dir->current_is_dir()) {
			if (p_recursive && !item.begins_with(".")) {
				_collect_files_recursive(full_path, p_extensions, p_recursive, p_limit, r_results, r_truncated);
			}
		} else {
			if (p_extensions.is_empty()) {
				r_results.push_back(full_path);
			} else {
				String ext = item.get_extension().to_lower();
				for (int i = 0; i < p_extensions.size(); i++) {
					if (ext == p_extensions[i]) {
						r_results.push_back(full_path);
						break;
					}
				}
			}
		}
		item = dir->get_next();
	}
	dir->list_dir_end();
}

// ---------------------------------------------------------------------------
// scene.list
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_scene_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String directory = p_params.get("directory", "res://");
	bool recursive = bool(p_params.get("recursive", true));

	if (!directory.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: directory must start with res://";
		return Dictionary();
	}

	Ref<DirAccess> dir_check = DirAccess::open(directory);
	if (dir_check.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: directory does not exist.";
		return Dictionary();
	}

	Vector<String> extensions;
	extensions.push_back("tscn");
	extensions.push_back("scn");

	Vector<String> results;
	bool truncated = false;
	_collect_files_recursive(directory, extensions, recursive, 1000, results, truncated);

	Array scenes;
	for (int i = 0; i < results.size(); i++) {
		scenes.push_back(results[i]);
	}

	Dictionary data;
	data["directory"] = directory;
	data["scenes"] = scenes;
	data["count"] = scenes.size();
	data["truncated"] = truncated;
	data["recursive"] = recursive;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// scene.open
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_scene_open(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String path = p_params.get("path", String());
	if (path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path is required.";
		return Dictionary();
	}
	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}
	if (!path.ends_with(".tscn") && !path.ends_with(".scn")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must end with .tscn or .scn";
		return Dictionary();
	}
	if (!FileAccess::exists(path)) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: scene file does not exist.";
		return Dictionary();
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!editor_interface) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: EditorInterface not available.";
		return Dictionary();
	}

	editor_interface->open_scene_from_path(path);

	Node *root = _get_edited_scene_root();
	Dictionary data;
	data["path"] = path;
	if (root) {
		data["name"] = root->get_name();
		data["class"] = root->get_class();
	}
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// scene.create
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_scene_create(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String root_type = p_params.get("root_type", String());
	String root_name = p_params.get("root_name", String());

	if (root_type.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: root_type is required.";
		return Dictionary();
	}

	Object *object = ClassDB::instantiate(root_type);
	Node *new_root = Object::cast_to<Node>(object);
	if (!new_root) {
		if (object) {
			memdelete(object);
		}
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: root_type is not a concrete Node class.";
		return Dictionary();
	}

	if (!root_name.is_empty()) {
		new_root->set_name(root_name);
	} else {
		new_root->set_name(root_type);
	}

	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node) {
		memdelete(new_root);
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: EditorNode not available.";
		return Dictionary();
	}

	EditorData &ed = EditorNode::get_editor_data();
	ed.add_edited_scene(-1);
	int new_idx = ed.get_edited_scene_count() - 1;
	ed.set_edited_scene(new_idx);
	ed.set_edited_scene_root(new_root);
	editor_node->set_edited_scene(new_root);

	Dictionary data;
	data["name"] = new_root->get_name();
	data["class"] = new_root->get_class();
	data["path"] = String();
	Array warnings;
	warnings.push_back("Scene is in-memory only. Call resource.save with a .tscn path to persist.");
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// scene.instantiate
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_scene_instantiate(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String scene_path = p_params.get("scene_path", String());
	String parent_path = p_params.get("parent_path", String());
	String node_name = p_params.get("name", String());
	int position = (int)p_params.get("position", -1);

	if (scene_path.is_empty() || parent_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: scene_path and parent_path are required.";
		return Dictionary();
	}

	if (!scene_path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: scene_path must start with res://";
		return Dictionary();
	}

	Node *parent = _resolve_node_path(parent_path, root);
	if (!parent) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Parent node path does not exist.";
		return Dictionary();
	}

	Ref<PackedScene> packed_scene = ResourceLoader::load(scene_path);
	if (packed_scene.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Scene resource could not be loaded.";
		return Dictionary();
	}

	Node *instance = packed_scene->instantiate();
	if (!instance) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: Failed to instantiate scene.";
		return Dictionary();
	}

	if (!node_name.is_empty()) {
		instance->set_name(node_name);
	}
	instance->set_name(parent->validate_child_name(instance));

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Instantiate Scene", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(parent, "add_child", instance, true);
	if (position >= 0) {
		undo_redo->add_do_method(parent, "move_child", instance, position);
	}
	undo_redo->add_do_method(instance, "set_owner", root);
	undo_redo->add_do_reference(instance);
	undo_redo->add_undo_method(parent, "remove_child", instance);
	undo_redo->commit_action();

	// Set the scene file path on the instance so the editor recognizes it.
	instance->set_scene_file_path(scene_path);

	Dictionary data;
	data["node_path"] = String(root->get_path_to(instance));
	data["name"] = instance->get_name();
	data["class"] = instance->get_class();
	data["scene_path"] = scene_path;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// node.find
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_node_find(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String pattern = p_params.get("pattern", "*");
	String type_filter = p_params.get("type", String());
	String group_filter = p_params.get("group", String());
	int limit = (int)p_params.get("limit", 100);
	bool owned = bool(p_params.get("owned", true));

	if (limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be >= 1.";
		return Dictionary();
	}

	TypedArray<Node> found = root->find_children(pattern, type_filter, true, owned);

	Array matches;
	bool truncated = false;
	for (int i = 0; i < found.size(); i++) {
		Node *node = Object::cast_to<Node>(found[i]);
		if (!node) {
			continue;
		}

		if (!group_filter.is_empty() && !node->is_in_group(group_filter)) {
			continue;
		}

		if (matches.size() >= limit) {
			truncated = true;
			break;
		}

		Dictionary match;
		match["path"] = node == root ? String(".") : String(root->get_path_to(node));
		match["name"] = node->get_name();
		match["class"] = node->get_class();
		matches.push_back(match);
	}

	Dictionary data;
	data["matches"] = matches;
	data["count"] = matches.size();
	data["truncated"] = truncated;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// node.get_groups
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_node_get_groups(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Array groups;
	List<Node::GroupInfo> group_list;
	node->get_groups(&group_list);
	for (const Node::GroupInfo &gi : group_list) {
		String group_name = gi.name;
		// Filter internal groups (starting with underscore).
		if (!group_name.begins_with("_")) {
			groups.push_back(group_name);
		}
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["groups"] = groups;
	data["count"] = groups.size();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// node.set_groups
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_node_set_groups(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Array add_groups = p_params.get("add", Array());
	Array remove_groups = p_params.get("remove", Array());
	if (add_groups.is_empty() && remove_groups.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: at least one of add or remove must be non-empty.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Node Groups", editor_data.get_current_edited_scene_history_id());

	Array added;
	for (int i = 0; i < add_groups.size(); i++) {
		String group_name = add_groups[i];
		if (!node->is_in_group(group_name)) {
			undo_redo->add_do_method(node, "add_to_group", group_name, true);
			undo_redo->add_undo_method(node, "remove_from_group", group_name);
			added.push_back(group_name);
		}
	}

	Array removed;
	for (int i = 0; i < remove_groups.size(); i++) {
		String group_name = remove_groups[i];
		if (node->is_in_group(group_name)) {
			undo_redo->add_do_method(node, "remove_from_group", group_name);
			undo_redo->add_undo_method(node, "add_to_group", group_name, true);
			removed.push_back(group_name);
		}
	}

	undo_redo->commit_action();

	// Collect current groups after the action.
	Array current_groups;
	List<Node::GroupInfo> group_list_after;
	node->get_groups(&group_list_after);
	for (const Node::GroupInfo &gi : group_list_after) {
		String group_name = gi.name;
		if (!group_name.begins_with("_")) {
			current_groups.push_back(group_name);
		}
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["added"] = added;
	data["removed"] = removed;
	data["current_groups"] = current_groups;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// resource.get
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_resource_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String path = p_params.get("path", String());
	if (path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path is required.";
		return Dictionary();
	}
	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}

	Ref<Resource> resource = ResourceLoader::load(path);
	if (resource.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Resource could not be loaded.";
		return Dictionary();
	}

	Variant property_names_variant = p_params.get("property_names", Variant());
	Dictionary properties;

	if (property_names_variant.get_type() == Variant::ARRAY) {
		Array property_names = property_names_variant;
		for (int i = 0; i < property_names.size(); i++) {
			String prop_name = property_names[i];
			bool valid = false;
			Variant value = resource->get(prop_name, &valid);
			if (!valid) {
				r_error_code = ERROR_INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: unknown property: " + prop_name;
				return Dictionary();
			}
			if (value.get_type() == Variant::OBJECT) {
				Variant serialized;
				if (!_serialize_resource_reference(value, serialized)) {
					continue;
				}
				properties[prop_name] = serialized;
				continue;
			}
			properties[prop_name] = value;
		}
	} else {
		// Return all serializable properties.
		List<PropertyInfo> plist;
		resource->get_property_list(&plist);
		for (const PropertyInfo &pi : plist) {
			if (pi.usage & PROPERTY_USAGE_CATEGORY || pi.usage & PROPERTY_USAGE_GROUP || pi.usage & PROPERTY_USAGE_SUBGROUP) {
				continue;
			}
			if (!(pi.usage & PROPERTY_USAGE_EDITOR) && !(pi.usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}
			bool valid = false;
			Variant value = resource->get(pi.name, &valid);
			if (!valid) {
				continue;
			}
			if (value.get_type() == Variant::OBJECT) {
				Variant serialized;
				if (!_serialize_resource_reference(value, serialized)) {
					continue;
				}
				properties[String(pi.name)] = serialized;
				continue;
			}
			properties[String(pi.name)] = value;
		}
	}

	Dictionary data;
	data["path"] = path;
	data["class"] = resource->get_class();
	data["properties"] = properties;
	data["property_count"] = properties.size();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// resource.list
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_resource_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String directory = p_params.get("directory", "res://");
	bool recursive = bool(p_params.get("recursive", true));
	int limit = (int)p_params.get("limit", 500);

	if (!directory.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: directory must start with res://";
		return Dictionary();
	}
	if (limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be >= 1.";
		return Dictionary();
	}

	Ref<DirAccess> dir_check = DirAccess::open(directory);
	if (dir_check.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: directory does not exist.";
		return Dictionary();
	}

	Vector<String> extensions;
	Variant ext_variant = p_params.get("extensions", Variant());
	if (ext_variant.get_type() == Variant::ARRAY) {
		Array ext_array = ext_variant;
		for (int i = 0; i < ext_array.size(); i++) {
			extensions.push_back(String(ext_array[i]).to_lower());
		}
	}

	Vector<String> results;
	bool truncated = false;
	_collect_files_recursive(directory, extensions, recursive, limit, results, truncated);

	Array resources;
	for (int i = 0; i < results.size(); i++) {
		resources.push_back(results[i]);
	}

	Dictionary data;
	data["directory"] = directory;
	data["resources"] = resources;
	data["count"] = resources.size();
	data["truncated"] = truncated;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// resource.create
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_resource_create(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String type_name = p_params.get("type", String());
	String path = p_params.get("path", String());
	Dictionary initial_properties;
	if (!_collect_properties_from_payload(p_params, initial_properties, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}
	if (p_params.has("properties") && p_params.has("property_entries")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide either properties or property_entries, not both.";
		return Dictionary();
	}

	if (type_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: type is required.";
		return Dictionary();
	}

	Object *object = ClassDB::instantiate(type_name);
	Resource *resource = Object::cast_to<Resource>(object);
	if (!resource) {
		if (object) {
			memdelete(object);
		}
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: type is not a concrete Resource class.";
		return Dictionary();
	}

	Ref<Resource> ref_resource(resource);

	if (!path.is_empty()) {
		if (!path.begins_with("res://")) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: path must start with res://";
			return Dictionary();
		}
		ref_resource->set_path(path);
	}

	for (const KeyValue<Variant, Variant> &E : initial_properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = ref_resource->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}

		Variant coerced_value;
		const String expected_resource_type = _resource_type_hint_for_property(ref_resource.ptr(), property_name);
		if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, r_error_message)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}
		if (!expected_resource_type.is_empty() && coerced_value.get_type() == Variant::NIL) {
			r_error_message = "INVALID_ARGUMENT: resource property '" + String(property_name) + "' cannot be null.";
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}
		ref_resource->set(property_name, coerced_value);
	}

	Dictionary data;
	data["class"] = ref_resource->get_class();
	data["path"] = ref_resource->get_path();
	data["property_count"] = initial_properties.size();
	Array warnings;
	warnings.push_back("Resource created in memory. Call resource.save to persist to disk.");
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// resource.set_properties
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_resource_set_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String path = p_params.get("path", String());
	Dictionary properties;
	if (!_collect_properties_from_payload(p_params, properties, r_error_message)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		return Dictionary();
	}
	if (p_params.has("properties") && p_params.has("property_entries")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide either properties or property_entries, not both.";
		return Dictionary();
	}

	if (path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path is required.";
		return Dictionary();
	}
	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}
	if (properties.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: properties must be non-empty.";
		return Dictionary();
	}

	Ref<Resource> resource = ResourceCache::get_ref(path);
	if (resource.is_null()) {
		resource = ResourceLoader::load(path);
	}
	if (resource.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Resource could not be loaded.";
		return Dictionary();
	}

	// Validate and coerce all properties first.
	Dictionary previous_values;
	Dictionary coerced_properties;
	for (const KeyValue<Variant, Variant> &E : properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = resource->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}

		Variant coerced_value;
		const String expected_resource_type = _resource_type_hint_for_property(resource.ptr(), property_name);
		if (!_coerce_property_value(old_value, E.value, expected_resource_type, String(property_name), coerced_value, r_error_message)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}
		if (!expected_resource_type.is_empty() && coerced_value.get_type() == Variant::NIL) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: resource property '" + String(property_name) + "' cannot be null.";
			return Dictionary();
		}

		previous_values[property_name] = old_value;
		coerced_properties[property_name] = coerced_value;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Resource Properties", editor_data.get_current_edited_scene_history_id());

	for (const KeyValue<Variant, Variant> &E : coerced_properties) {
		StringName property_name = E.key;
		undo_redo->add_do_property(resource.ptr(), property_name, E.value);
		undo_redo->add_undo_property(resource.ptr(), property_name, previous_values[property_name]);
	}

	undo_redo->commit_action();

	Dictionary data;
	data["path"] = path;
	data["property_count"] = properties.size();
	Array warnings;
	warnings.push_back("Resource modified in editor state; call resource.save to persist.");
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// signal.list
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_signal_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	List<MethodInfo> signal_list;
	node->get_signal_list(&signal_list);

	Array signals;
	for (const MethodInfo &mi : signal_list) {
		Dictionary sig;
		sig["name"] = mi.name;

		Array args;
		for (const PropertyInfo &pi : mi.arguments) {
			Dictionary arg;
			arg["name"] = pi.name;
			arg["type"] = Variant::get_type_name(pi.type);
			args.push_back(arg);
		}
		sig["args"] = args;
		signals.push_back(sig);
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["signals"] = signals;
	data["count"] = signals.size();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// signal.get_connections
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_signal_get_connections(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	String signal_filter = p_params.get("signal_name", String());

	Array connections;
	List<Object::Connection> connection_list;

	if (!signal_filter.is_empty()) {
		node->get_signal_connection_list(signal_filter, &connection_list);
	} else {
		List<MethodInfo> signal_list;
		node->get_signal_list(&signal_list);
		for (const MethodInfo &mi : signal_list) {
			node->get_signal_connection_list(mi.name, &connection_list);
		}
	}

	for (const Object::Connection &conn : connection_list) {
		Dictionary conn_data;
		conn_data["signal"] = conn.signal.get_name();

		Object *target_obj = conn.callable.get_object();
		Node *target_node = Object::cast_to<Node>(target_obj);
		if (target_node) {
			conn_data["target_path"] = target_node == root ? String(".") : String(root->get_path_to(target_node));
		} else {
			conn_data["target_path"] = String();
		}

		conn_data["method"] = conn.callable.get_method();
		conn_data["flags"] = conn.flags;
		connections.push_back(conn_data);
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["connections"] = connections;
	data["count"] = connections.size();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// signal.connect
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_signal_connect(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	String signal_name = p_params.get("signal_name", String());
	String target_path = p_params.get("target_path", String());
	String method = p_params.get("method", String());
	int flags = (int)p_params.get("flags", 0);

	if (node_path.is_empty() || signal_name.is_empty() || target_path.is_empty() || method.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path, signal_name, target_path, and method are required.";
		return Dictionary();
	}

	Node *source = _resolve_node_path(node_path, root);
	if (!source) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Source node path does not exist.";
		return Dictionary();
	}

	Node *target = _resolve_node_path(target_path, root);
	if (!target) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Target node path does not exist.";
		return Dictionary();
	}

	// Validate signal exists on the source node.
	bool signal_found = false;
	List<MethodInfo> signal_list;
	source->get_signal_list(&signal_list);
	for (const MethodInfo &mi : signal_list) {
		if (mi.name == signal_name) {
			signal_found = true;
			break;
		}
	}
	if (!signal_found) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: signal '" + signal_name + "' does not exist on source node.";
		return Dictionary();
	}

	Callable callable = Callable(target, method);
	if (source->is_connected(signal_name, callable)) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: connection already exists.";
		return Dictionary();
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Connect Signal", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(source, "connect", signal_name, callable, flags);
	undo_redo->add_undo_method(source, "disconnect", signal_name, callable);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["signal_name"] = signal_name;
	data["target_path"] = target_path;
	data["method"] = method;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// signal.disconnect
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_signal_disconnect(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	String signal_name = p_params.get("signal_name", String());
	String target_path = p_params.get("target_path", String());
	String method = p_params.get("method", String());

	if (node_path.is_empty() || signal_name.is_empty() || target_path.is_empty() || method.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path, signal_name, target_path, and method are required.";
		return Dictionary();
	}

	Node *source = _resolve_node_path(node_path, root);
	if (!source) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Source node path does not exist.";
		return Dictionary();
	}

	Node *target = _resolve_node_path(target_path, root);
	if (!target) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Target node path does not exist.";
		return Dictionary();
	}

	Callable callable = Callable(target, method);
	if (!source->is_connected(signal_name, callable)) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: connection does not exist.";
		return Dictionary();
	}

	// Capture flags for undo.
	int old_flags = 0;
	List<Object::Connection> existing_connections;
	source->get_signal_connection_list(signal_name, &existing_connections);
	for (const Object::Connection &conn : existing_connections) {
		if (conn.callable == callable) {
			old_flags = conn.flags;
			break;
		}
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Disconnect Signal", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(source, "disconnect", signal_name, callable);
	undo_redo->add_undo_method(source, "connect", signal_name, callable, old_flags);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["signal_name"] = signal_name;
	data["target_path"] = target_path;
	data["method"] = method;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// project.get_setting
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_project_get_setting(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Variant keys_variant = p_params.get("keys", Variant());
	if (keys_variant.get_type() != Variant::ARRAY) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: keys must be an array of strings.";
		return Dictionary();
	}
	Array keys = keys_variant;
	if (keys.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: keys must be non-empty.";
		return Dictionary();
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: ProjectSettings not available.";
		return Dictionary();
	}

	Dictionary settings;
	for (int i = 0; i < keys.size(); i++) {
		String key = keys[i];
		if (!ps->has_setting(key)) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: setting key does not exist: " + key;
			return Dictionary();
		}
		Variant value = ps->get_setting(key);
		if (value.get_type() == Variant::OBJECT) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: setting value is not serializable: " + key;
			return Dictionary();
		}
		settings[key] = value;
	}

	Dictionary data;
	data["settings"] = settings;
	data["count"] = settings.size();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// project.set_setting
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_project_set_setting(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Dictionary settings = p_params.get("settings", Dictionary());
	if (settings.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: settings must be non-empty.";
		return Dictionary();
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: ProjectSettings not available.";
		return Dictionary();
	}

	for (const KeyValue<Variant, Variant> &E : settings) {
		String key = E.key;
		Variant setting_value = E.value;

		if (key.begins_with("input/") && setting_value.get_type() == Variant::DICTIONARY) {
			Dictionary input_action = setting_value;
			if (input_action.has("events")) {
				Variant events_variant = input_action["events"];
				if (events_variant.get_type() != Variant::ARRAY) {
					r_error_code = ERROR_INVALID_ARGUMENT;
					r_error_message = "INVALID_ARGUMENT: input action events must be an array for setting: " + key;
					return Dictionary();
				}

				Array events = events_variant;
				Array normalized_events;
				normalized_events.resize(events.size());
				for (int i = 0; i < events.size(); i++) {
					String event_error;
					Ref<InputEvent> event = _coerce_input_event(events[i], event_error);
					if (event.is_null()) {
						r_error_code = ERROR_INVALID_ARGUMENT;
						r_error_message = "INVALID_ARGUMENT: input action event at index " + itos(i) + " for setting '" + key + "': " + event_error;
						return Dictionary();
					}
					normalized_events[i] = event;
				}
				input_action["events"] = normalized_events;
				setting_value = input_action;
			}
		}

		if (setting_value.get_type() == Variant::OBJECT) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: setting value is not serializable: " + key;
			return Dictionary();
		}
		ps->set_setting(key, setting_value);
	}

	Error save_err = ps->save();
	if (save_err != OK) {
		r_error_code = ERROR_INTERNAL;
		r_error_message = "INTERNAL: failed to save project settings.";
		return Dictionary();
	}

	// Reload InputMap if any input/* settings were changed.
	bool has_input_changes = false;
	for (const KeyValue<Variant, Variant> &E : settings) {
		if (String(E.key).begins_with("input/")) {
			has_input_changes = true;
			break;
		}
	}
	if (has_input_changes) {
		InputMap::get_singleton()->load_from_project_settings();

		// Keep the Project Settings > Input Map tab in sync when actions are changed through MCP.
		ProjectSettingsEditor *project_settings_editor = ProjectSettingsEditor::get_singleton();
		if (project_settings_editor) {
			project_settings_editor->call_deferred("_update_action_map_editor");
		}
	}

	Dictionary data;
	data["count"] = settings.size();
	Array warnings;
	warnings.push_back("Project settings saved and applied. Some display/window settings may still require editor restart.");
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// editor.get_errors
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_editor_get_errors(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	int limit = (int)p_params.get("limit", 50);
	bool clear = bool(p_params.get("clear", false));

	if (limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be >= 1.";
		return Dictionary();
	}

	// Filter by types.
	Vector<String> type_filters;
	Variant types_variant = p_params.get("types", Variant());
	if (types_variant.get_type() == Variant::ARRAY) {
		Array types = types_variant;
		for (int i = 0; i < types.size(); i++) {
			type_filters.push_back(String(types[i]).to_lower());
		}
	} else {
		type_filters.push_back("error");
		type_filters.push_back("warning");
	}

	Array messages;
	bool truncated = false;

	// Read from the ring buffer (oldest first).
	int total = log_ring_count;
	int start_idx = 0;
	if (total >= MAX_LOG_ENTRIES) {
		start_idx = log_ring_write_pos; // Oldest entry.
	}

	for (int i = 0; i < total; i++) {
		int idx = (start_idx + i) % (int)log_ring_buffer.size();
		const LogEntry &entry = log_ring_buffer[idx];

		bool type_match = false;
		for (int j = 0; j < type_filters.size(); j++) {
			if (entry.type == type_filters[j]) {
				type_match = true;
				break;
			}
		}
		if (!type_match) {
			continue;
		}

		if (messages.size() >= limit) {
			truncated = true;
			break;
		}

		Dictionary msg;
		msg["type"] = entry.type;
		msg["text"] = entry.text;
		msg["file"] = entry.file;
		msg["function"] = entry.function;
		msg["line"] = entry.line;
		msg["timestamp"] = (int64_t)entry.timestamp;
		messages.push_back(msg);
	}

	if (clear) {
		// const_cast is needed because poll() runs on the main thread too.
		OpenCodeMCPProtocol *mutable_self = const_cast<OpenCodeMCPProtocol *>(this);
		mutable_self->log_ring_buffer.clear();
		mutable_self->log_ring_write_pos = 0;
		mutable_self->log_ring_count = 0;
	}

	Dictionary data;
	data["messages"] = messages;
	data["count"] = messages.size();
	data["truncated"] = truncated;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// docs.*
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_docs_list_versions(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	Array aliases;
	aliases.push_back("current");
	aliases.push_back("latest");
	aliases.push_back(String(GODOT_VERSION_DOCS_BRANCH));
	aliases.push_back(String(GODOT_VERSION_BRANCH));
	aliases.push_back(String(GODOT_VERSION_NUMBER));

	Dictionary version_entry;
	version_entry["id"] = String(GODOT_VERSION_DOCS_BRANCH);
	version_entry["docs_url"] = String(GODOT_VERSION_DOCS_URL);
	version_entry["aliases"] = aliases;
	version_entry["is_default"] = true;

	Array versions;
	versions.push_back(version_entry);

	Dictionary data;
	data["default_version"] = resolved_version;
	data["versions"] = versions;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_docs_class_lookup(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	const Variant class_name_variant = p_params.get("class_name", Variant());
	if (class_name_variant.get_type() != Variant::STRING && class_name_variant.get_type() != Variant::STRING_NAME) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name is required.";
		return Dictionary();
	}

	const String class_name = String(class_name_variant).strip_edges();
	if (class_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name is required.";
		return Dictionary();
	}

	DocTools *doc_tools = EditorHelp::get_doc_data();
	if (!doc_tools) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: documentation data is not available.";
		return Dictionary();
	}

	DocData::ClassDoc *class_doc = _docs_find_class(doc_tools, class_name);
	if (!class_doc) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: class documentation not found: " + class_name;
		return Dictionary();
	}

	Array inherited_by;
	if (doc_tools->inheriting.has(class_doc->name)) {
		for (RBSet<String, NaturalNoCaseComparator>::Element *E = doc_tools->inheriting[class_doc->name].front(); E; E = E->next()) {
			inherited_by.push_back(E->get());
		}
	}

	Dictionary member_counts;
	member_counts["constructors"] = class_doc->constructors.size();
	member_counts["methods"] = class_doc->methods.size();
	member_counts["operators"] = class_doc->operators.size();
	member_counts["signals"] = class_doc->signals.size();
	member_counts["constants"] = class_doc->constants.size();
	member_counts["properties"] = class_doc->properties.size();
	member_counts["theme_properties"] = class_doc->theme_properties.size();
	member_counts["annotations"] = class_doc->annotations.size();
	member_counts["enums"] = class_doc->enums.size();

	Dictionary data;
	data["class_name"] = class_doc->name;
	data["version"] = resolved_version;
	data["docs_url"] = _docs_make_url("class_name", class_doc->name);
	data["class_doc"] = DocData::ClassDoc::to_dict(*class_doc);
	data["inherited_by"] = inherited_by;
	data["member_counts"] = member_counts;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_docs_member_lookup(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	const Variant class_name_variant = p_params.get("class_name", Variant());
	const Variant member_name_variant = p_params.get("member_name", Variant());
	if ((class_name_variant.get_type() != Variant::STRING && class_name_variant.get_type() != Variant::STRING_NAME) ||
			(member_name_variant.get_type() != Variant::STRING && member_name_variant.get_type() != Variant::STRING_NAME)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name and member_name are required.";
		return Dictionary();
	}

	const String class_name = String(class_name_variant).strip_edges();
	const String member_name = String(member_name_variant).strip_edges();
	if (class_name.is_empty() || member_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name and member_name are required.";
		return Dictionary();
	}

	String kind_filter = String(p_params.get("kind", String())).strip_edges().to_lower();
	if (!kind_filter.is_empty() && !_docs_is_supported_member_kind(kind_filter)) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: kind must be one of method, constructor, operator, signal, property, constant, annotation, theme_item, enum.";
		return Dictionary();
	}

	const bool include_inherited = bool(p_params.get("include_inherited", true));

	DocTools *doc_tools = EditorHelp::get_doc_data();
	if (!doc_tools) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: documentation data is not available.";
		return Dictionary();
	}

	DocData::ClassDoc *base_class_doc = _docs_find_class(doc_tools, class_name);
	if (!base_class_doc) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: class documentation not found: " + class_name;
		return Dictionary();
	}

	Array matches;
	Dictionary visited_classes;

	DocData::ClassDoc *current_class = base_class_doc;
	while (current_class) {
		if (visited_classes.has(current_class->name)) {
			break;
		}
		visited_classes[current_class->name] = true;

		auto append_method_matches = [&](const Vector<DocData::MethodDoc> &p_methods, const String &p_kind) {
			if (!kind_filter.is_empty() && kind_filter != p_kind) {
				return;
			}
			for (const DocData::MethodDoc &method : p_methods) {
				if (method.name.nocasecmp_to(member_name) == 0) {
					_docs_append_member_match(matches, p_kind, current_class->name, method.name, DocData::MethodDoc::to_dict(method));
				}
			}
		};

		append_method_matches(current_class->constructors, "constructor");
		append_method_matches(current_class->methods, "method");
		append_method_matches(current_class->operators, "operator");
		append_method_matches(current_class->signals, "signal");
		append_method_matches(current_class->annotations, "annotation");

		if (kind_filter.is_empty() || kind_filter == "property") {
			for (const DocData::PropertyDoc &property : current_class->properties) {
				if (property.name.nocasecmp_to(member_name) == 0) {
					_docs_append_member_match(matches, "property", current_class->name, property.name, DocData::PropertyDoc::to_dict(property));
				}
			}
		}

		if (kind_filter.is_empty() || kind_filter == "constant") {
			for (const DocData::ConstantDoc &constant : current_class->constants) {
				if (constant.name.nocasecmp_to(member_name) == 0) {
					_docs_append_member_match(matches, "constant", current_class->name, constant.name, DocData::ConstantDoc::to_dict(constant));
				}
			}
		}

		if (kind_filter.is_empty() || kind_filter == "theme_item") {
			for (const DocData::ThemeItemDoc &theme_item : current_class->theme_properties) {
				if (theme_item.name.nocasecmp_to(member_name) == 0) {
					_docs_append_member_match(matches, "theme_item", current_class->name, theme_item.name, DocData::ThemeItemDoc::to_dict(theme_item));
				}
			}
		}

		if (kind_filter.is_empty() || kind_filter == "enum") {
			for (const KeyValue<String, DocData::EnumDoc> &E : current_class->enums) {
				if (E.key.nocasecmp_to(member_name) == 0) {
					Dictionary enum_payload = DocData::EnumDoc::to_dict(E.value);
					enum_payload["name"] = E.key;
					_docs_append_member_match(matches, "enum", current_class->name, E.key, enum_payload);
				}
			}
		}

		if (!include_inherited || current_class->inherits.is_empty()) {
			break;
		}

		current_class = _docs_find_class(doc_tools, current_class->inherits);
	}

	if (matches.is_empty()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: member not found: " + member_name;
		return Dictionary();
	}

	Dictionary data;
	data["class_name"] = base_class_doc->name;
	data["member_name"] = member_name;
	data["kind"] = kind_filter;
	data["version"] = resolved_version;
	data["matches"] = matches;
	data["count"] = matches.size();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_docs_search(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	const Variant query_variant = p_params.get("query", Variant());
	if (query_variant.get_type() != Variant::STRING && query_variant.get_type() != Variant::STRING_NAME) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: query is required.";
		return Dictionary();
	}

	const String query = String(query_variant).strip_edges();
	if (query.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: query is required.";
		return Dictionary();
	}

	const int limit = (int)p_params.get("limit", 50);
	if (limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be >= 1.";
		return Dictionary();
	}

	const bool include_members = bool(p_params.get("include_members", true));

	DocTools *doc_tools = EditorHelp::get_doc_data();
	if (!doc_tools) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: documentation data is not available.";
		return Dictionary();
	}

	const String term = query.to_lower();
	Vector<String> terms = term.split_spaces();
	if (terms.is_empty()) {
		terms.push_back(term);
	}

	Array results;
	Dictionary seen;
	bool truncated = false;

	for (HashMap<String, DocData::ClassDoc>::Iterator class_it = doc_tools->class_list.begin(); class_it; ++class_it) {
		DocData::ClassDoc &class_doc = class_it->value;
		if (class_doc.name.is_empty()) {
			continue;
		}

		if (class_doc.name.containsn(term)) {
			if (!_docs_push_search_result(results, seen, truncated, limit, "class", class_doc.name, String(), class_doc.name, class_doc.is_script_doc)) {
				break;
			}
		}

		if (truncated) {
			break;
		}

		if (!include_members || (term.length() <= 1 && term != "@")) {
			continue;
		}

		auto append_method_search = [&](const Vector<DocData::MethodDoc> &p_methods, const String &p_kind_label, const String &p_kind) -> bool {
			for (const DocData::MethodDoc &method : p_methods) {
				if (_docs_name_matches_search_term(term, terms, method.name)) {
					const String label = vformat("%s > %s: %s", class_doc.name, p_kind_label, method.name);
					if (!_docs_push_search_result(results, seen, truncated, limit, p_kind, class_doc.name, method.name, label, class_doc.is_script_doc)) {
						return false;
					}
				}
			}
			return true;
		};

		auto append_property_search = [&](const Vector<DocData::PropertyDoc> &p_properties) -> bool {
			for (const DocData::PropertyDoc &property : p_properties) {
				if (_docs_name_matches_search_term(term, terms, property.name)) {
					const String label = vformat("%s > Property: %s", class_doc.name, property.name);
					if (!_docs_push_search_result(results, seen, truncated, limit, "property", class_doc.name, property.name, label, class_doc.is_script_doc)) {
						return false;
					}
				}
			}
			return true;
		};

		auto append_constant_search = [&](const Vector<DocData::ConstantDoc> &p_constants) -> bool {
			for (const DocData::ConstantDoc &constant : p_constants) {
				if (_docs_name_matches_search_term(term, terms, constant.name)) {
					const String label = vformat("%s > Constant: %s", class_doc.name, constant.name);
					if (!_docs_push_search_result(results, seen, truncated, limit, "constant", class_doc.name, constant.name, label, class_doc.is_script_doc)) {
						return false;
					}
				}
			}
			return true;
		};

		auto append_theme_item_search = [&](const Vector<DocData::ThemeItemDoc> &p_theme_items) -> bool {
			for (const DocData::ThemeItemDoc &theme_item : p_theme_items) {
				if (_docs_name_matches_search_term(term, terms, theme_item.name)) {
					const String label = vformat("%s > Theme Property: %s", class_doc.name, theme_item.name);
					if (!_docs_push_search_result(results, seen, truncated, limit, "theme_item", class_doc.name, theme_item.name, label, class_doc.is_script_doc)) {
						return false;
					}
				}
			}
			return true;
		};

		if (!append_method_search(class_doc.constructors, "Constructor", "constructor") ||
				!append_method_search(class_doc.methods, "Method", "method") ||
				!append_method_search(class_doc.operators, "Operator", "operator") ||
				!append_method_search(class_doc.signals, "Signal", "signal") ||
				!append_constant_search(class_doc.constants) ||
				!append_property_search(class_doc.properties) ||
				!append_theme_item_search(class_doc.theme_properties) ||
				!append_method_search(class_doc.annotations, "Annotation", "annotation")) {
			break;
		}
	}

	Dictionary data;
	data["query"] = query;
	data["version"] = resolved_version;
	data["limit"] = limit;
	data["results"] = results;
	data["count"] = results.size();
	data["truncated"] = truncated;
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_docs_inheritance(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	const Variant class_name_variant = p_params.get("class_name", Variant());
	if (class_name_variant.get_type() != Variant::STRING && class_name_variant.get_type() != Variant::STRING_NAME) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name is required.";
		return Dictionary();
	}

	const String class_name = String(class_name_variant).strip_edges();
	if (class_name.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: class_name is required.";
		return Dictionary();
	}

	const bool include_descendants = bool(p_params.get("include_descendants", true));
	const bool include_inherited_members = bool(p_params.get("include_inherited_members", true));
	const int descendants_limit = (int)p_params.get("descendants_limit", 500);
	if (descendants_limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: descendants_limit must be >= 1.";
		return Dictionary();
	}

	DocTools *doc_tools = EditorHelp::get_doc_data();
	if (!doc_tools) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: documentation data is not available.";
		return Dictionary();
	}

	DocData::ClassDoc *class_doc = _docs_find_class(doc_tools, class_name);
	if (!class_doc) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: class documentation not found: " + class_name;
		return Dictionary();
	}

	Array chain;
	Dictionary visited_chain;
	DocData::ClassDoc *current_class = class_doc;
	while (current_class) {
		if (visited_chain.has(current_class->name)) {
			break;
		}
		visited_chain[current_class->name] = true;
		chain.push_back(current_class->name);

		if (current_class->inherits.is_empty()) {
			break;
		}

		current_class = _docs_find_class(doc_tools, current_class->inherits);
	}

	Array inherited_by;
	if (doc_tools->inheriting.has(class_doc->name)) {
		for (RBSet<String, NaturalNoCaseComparator>::Element *E = doc_tools->inheriting[class_doc->name].front(); E; E = E->next()) {
			inherited_by.push_back(E->get());
		}
	}

	Array descendants;
	bool descendants_truncated = false;
	if (include_descendants) {
		Vector<String> queue;
		Dictionary seen_descendants;

		for (int i = 0; i < inherited_by.size(); i++) {
			String child = inherited_by[i];
			if (!seen_descendants.has(child)) {
				seen_descendants[child] = true;
				queue.push_back(child);
			}
		}

		int queue_index = 0;
		while (queue_index < queue.size()) {
			const String descendant_name = queue[queue_index++];
			if (descendants.size() >= descendants_limit) {
				descendants_truncated = true;
				break;
			}

			descendants.push_back(descendant_name);

			if (doc_tools->inheriting.has(descendant_name)) {
				for (RBSet<String, NaturalNoCaseComparator>::Element *E = doc_tools->inheriting[descendant_name].front(); E; E = E->next()) {
					const String child_name = E->get();
					if (!seen_descendants.has(child_name)) {
						seen_descendants[child_name] = true;
						queue.push_back(child_name);
					}
				}
			}
		}
	}

	Dictionary inherited_members;
	if (include_inherited_members) {
		Array inherited_methods;
		Array inherited_properties;
		Array inherited_signals;
		Array inherited_constants;

		Dictionary seen_methods;
		Dictionary seen_properties;
		Dictionary seen_signals;
		Dictionary seen_constants;

		DocData::ClassDoc *ancestor_class = class_doc;
		if (!ancestor_class->inherits.is_empty()) {
			ancestor_class = _docs_find_class(doc_tools, ancestor_class->inherits);
		} else {
			ancestor_class = nullptr;
		}

		Dictionary visited_ancestors;
		while (ancestor_class) {
			if (visited_ancestors.has(ancestor_class->name)) {
				break;
			}
			visited_ancestors[ancestor_class->name] = true;

			for (const DocData::MethodDoc &method : ancestor_class->methods) {
				const String key = method.name.to_lower();
				if (seen_methods.has(key)) {
					continue;
				}
				seen_methods[key] = true;

				Dictionary entry;
				entry["name"] = method.name;
				entry["declared_in"] = ancestor_class->name;
				entry["kind"] = "method";
				inherited_methods.push_back(entry);
			}

			for (const DocData::PropertyDoc &property : ancestor_class->properties) {
				const String key = property.name.to_lower();
				if (seen_properties.has(key)) {
					continue;
				}
				seen_properties[key] = true;

				Dictionary entry;
				entry["name"] = property.name;
				entry["declared_in"] = ancestor_class->name;
				entry["kind"] = "property";
				inherited_properties.push_back(entry);
			}

			for (const DocData::MethodDoc &signal : ancestor_class->signals) {
				const String key = signal.name.to_lower();
				if (seen_signals.has(key)) {
					continue;
				}
				seen_signals[key] = true;

				Dictionary entry;
				entry["name"] = signal.name;
				entry["declared_in"] = ancestor_class->name;
				entry["kind"] = "signal";
				inherited_signals.push_back(entry);
			}

			for (const DocData::ConstantDoc &constant : ancestor_class->constants) {
				const String key = constant.name.to_lower();
				if (seen_constants.has(key)) {
					continue;
				}
				seen_constants[key] = true;

				Dictionary entry;
				entry["name"] = constant.name;
				entry["declared_in"] = ancestor_class->name;
				entry["kind"] = "constant";
				inherited_constants.push_back(entry);
			}

			if (ancestor_class->inherits.is_empty()) {
				break;
			}

			ancestor_class = _docs_find_class(doc_tools, ancestor_class->inherits);
		}

		inherited_members["methods"] = inherited_methods;
		inherited_members["properties"] = inherited_properties;
		inherited_members["signals"] = inherited_signals;
		inherited_members["constants"] = inherited_constants;
	}

	Dictionary data;
	data["class_name"] = class_doc->name;
	data["version"] = resolved_version;
	data["chain"] = chain;
	data["inherits"] = class_doc->inherits;
	data["inherited_by"] = inherited_by;
	data["descendants"] = descendants;
	data["descendants_truncated"] = descendants_truncated;
	if (include_inherited_members) {
		data["inherited_members"] = inherited_members;
	}
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_docs_examples(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String resolved_version;
	if (!_docs_resolve_version(p_params, resolved_version, r_error_code, r_error_message)) {
		return Dictionary();
	}

	const String class_name = String(p_params.get("class_name", String())).strip_edges();
	const String topic = String(p_params.get("topic", String())).strip_edges();
	if (class_name.is_empty() && topic.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide class_name or topic.";
		return Dictionary();
	}

	const String language_filter = String(p_params.get("language", String("any"))).strip_edges().to_lower();
	if (language_filter != "any" && language_filter != "gdscript" && language_filter != "csharp" && language_filter != "text") {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: language must be one of any, gdscript, csharp, text.";
		return Dictionary();
	}

	const int limit = (int)p_params.get("limit", 8);
	if (limit < 1) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be >= 1.";
		return Dictionary();
	}

	DocTools *doc_tools = EditorHelp::get_doc_data();
	if (!doc_tools) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: documentation data is not available.";
		return Dictionary();
	}

	Array examples;
	Array warnings;
	bool truncated = false;
	const String topic_filter = topic.to_lower();

	auto append_examples_from_text = [&](const String &p_text, const String &p_topic_kind, const String &p_class, const String &p_member = String()) {
		if (examples.size() >= limit) {
			truncated = true;
			return;
		}

		Dictionary base_payload;
		base_payload["kind"] = p_topic_kind;
		base_payload["class_name"] = p_class;
		if (!p_member.is_empty()) {
			base_payload["member_name"] = p_member;
		}
		base_payload["topic_id"] = _docs_make_topic_id(p_topic_kind, p_class, p_member);
		base_payload["docs_url"] = _docs_make_url(p_topic_kind, p_class, p_member);

		_docs_collect_examples_from_text(p_text, base_payload, language_filter, limit, examples);
		if (examples.size() >= limit) {
			truncated = true;
		}
	};

	auto collect_class_examples = [&](DocData::ClassDoc *p_class_doc) {
		if (!p_class_doc || examples.size() >= limit) {
			return;
		}

		const bool include_class_description = topic_filter.is_empty() || p_class_doc->name.containsn(topic_filter) ||
				p_class_doc->brief_description.containsn(topic_filter) || p_class_doc->description.containsn(topic_filter);
		if (include_class_description) {
			append_examples_from_text(p_class_doc->brief_description, "class_desc", p_class_doc->name);
			append_examples_from_text(p_class_doc->description, "class_desc", p_class_doc->name);
		}

		auto collect_method_examples = [&](const Vector<DocData::MethodDoc> &p_methods, const String &p_topic_kind) {
			for (const DocData::MethodDoc &method : p_methods) {
				if (examples.size() >= limit) {
					break;
				}

				if (!topic_filter.is_empty() && !method.name.containsn(topic_filter) && !method.description.containsn(topic_filter)) {
					continue;
				}

				append_examples_from_text(method.description, p_topic_kind, p_class_doc->name, method.name);
			}
		};

		collect_method_examples(p_class_doc->constructors, "class_method");
		collect_method_examples(p_class_doc->methods, "class_method");
		collect_method_examples(p_class_doc->operators, "class_method");
		collect_method_examples(p_class_doc->signals, "class_signal");
		collect_method_examples(p_class_doc->annotations, "class_annotation");

		for (const DocData::PropertyDoc &property : p_class_doc->properties) {
			if (examples.size() >= limit) {
				break;
			}
			if (!topic_filter.is_empty() && !property.name.containsn(topic_filter) && !property.description.containsn(topic_filter)) {
				continue;
			}
			append_examples_from_text(property.description, "class_property", p_class_doc->name, property.name);
		}

		for (const DocData::ConstantDoc &constant : p_class_doc->constants) {
			if (examples.size() >= limit) {
				break;
			}
			if (!topic_filter.is_empty() && !constant.name.containsn(topic_filter) && !constant.description.containsn(topic_filter)) {
				continue;
			}
			append_examples_from_text(constant.description, "class_constant", p_class_doc->name, constant.name);
		}

		for (const DocData::ThemeItemDoc &theme_item : p_class_doc->theme_properties) {
			if (examples.size() >= limit) {
				break;
			}
			if (!topic_filter.is_empty() && !theme_item.name.containsn(topic_filter) && !theme_item.description.containsn(topic_filter)) {
				continue;
			}
			append_examples_from_text(theme_item.description, "class_theme_item", p_class_doc->name, theme_item.name);
		}
	};

	if (!class_name.is_empty()) {
		DocData::ClassDoc *class_doc = _docs_find_class(doc_tools, class_name);
		if (!class_doc) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: class documentation not found: " + class_name;
			return Dictionary();
		}

		collect_class_examples(class_doc);
	} else {
		for (HashMap<String, DocData::ClassDoc>::Iterator class_it = doc_tools->class_list.begin(); class_it; ++class_it) {
			DocData::ClassDoc &candidate = class_it->value;
			if (candidate.name.is_empty()) {
				continue;
			}

			collect_class_examples(&candidate);
			if (examples.size() >= limit) {
				truncated = true;
				break;
			}
		}
	}

	if (examples.is_empty()) {
		warnings.push_back("No code examples found for the requested input.");
	}

	Dictionary data;
	data["source"] = class_name.is_empty() ? "topic" : "class";
	data["query"] = class_name.is_empty() ? topic : class_name;
	data["version"] = resolved_version;
	data["language"] = language_filter;
	data["examples"] = examples;
	data["count"] = examples.size();
	data["truncated"] = truncated;
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// shader.get
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_shader_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	String path = p_params.get("path", String());
	if (path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path is required.";
		return Dictionary();
	}
	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}

	Ref<Shader> shader = ResourceLoader::load(path);
	if (shader.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Shader resource could not be loaded.";
		return Dictionary();
	}

	String source = shader->get_code();
	String shader_mode;
	switch (shader->get_mode()) {
		case Shader::MODE_SPATIAL:
			shader_mode = "spatial";
			break;
		case Shader::MODE_CANVAS_ITEM:
			shader_mode = "canvas_item";
			break;
		case Shader::MODE_PARTICLES:
			shader_mode = "particles";
			break;
		case Shader::MODE_SKY:
			shader_mode = "sky";
			break;
		case Shader::MODE_FOG:
			shader_mode = "fog";
			break;
		default:
			shader_mode = "unknown";
			break;
	}

	Dictionary data;
	data["path"] = path;
	data["class"] = shader->get_class();
	data["shader_type"] = shader_mode;
	data["source"] = source;
	data["version"] = source.md5_text();
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// shader.edit
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_shader_edit(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	String path = p_params.get("path", String());
	String new_source = p_params.get("source", String());
	String expected_version = p_params.get("expected_version", String());

	if (path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path is required.";
		return Dictionary();
	}
	if (!path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must start with res://";
		return Dictionary();
	}
	if (new_source.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: source is required.";
		return Dictionary();
	}

	Ref<Shader> shader = ResourceLoader::load(path);
	if (shader.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Shader resource could not be loaded.";
		return Dictionary();
	}

	String old_source = shader->get_code();
	if (!expected_version.is_empty() && expected_version != old_source.md5_text()) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: shader version mismatch.";
		return Dictionary();
	}
	if (new_source == old_source) {
		r_error_code = ERROR_CONFLICT;
		r_error_message = "CONFLICT: new source is identical to current source.";
		return Dictionary();
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Edit Shader", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(shader.ptr(), "set_code", new_source);
	undo_redo->add_undo_method(shader.ptr(), "set_code", old_source);
	undo_redo->commit_action();

	Dictionary data;
	data["path"] = path;
	data["version"] = new_source.md5_text();
	Array warnings;
	warnings.push_back("Shader modified in editor state; call resource.save to persist to disk.");
	return _make_ok(data, warnings);
}

// ---------------------------------------------------------------------------
// theme.get_overrides
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_theme_get_overrides(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Control *control = Object::cast_to<Control>(node);
	if (!control) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node is not a Control subclass.";
		return Dictionary();
	}

	String override_type = String(p_params.get("override_type", String())).to_lower();

	Dictionary colors;
	Dictionary constants;
	Dictionary font_sizes;
	Dictionary fonts;
	Dictionary icons;
	Dictionary styleboxes;

	List<PropertyInfo> plist;
	control->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		String prop_name = pi.name;
		if (!prop_name.begins_with("theme_override_")) {
			continue;
		}

		bool valid = false;
		Variant value = control->get(pi.name, &valid);
		if (!valid || value.get_type() == Variant::NIL) {
			continue;
		}

		// Parse category and name from "theme_override_<category>/<name>".
		String remainder = prop_name.substr(String("theme_override_").length());
		int slash = remainder.find("/");
		if (slash < 0) {
			continue;
		}
		String category = remainder.substr(0, slash);
		String entry_name = remainder.substr(slash + 1);

		if (!override_type.is_empty() && category != override_type && category != override_type + "s") {
			continue;
		}

		// Skip Object values (fonts, icons, styleboxes are Resources but serialize as paths).
		if (value.get_type() == Variant::OBJECT) {
			// Store resource path if available.
			Ref<Resource> res = value;
			if (res.is_valid() && !res->get_path().is_empty()) {
				if (category == "colors") {
					colors[entry_name] = res->get_path();
				} else if (category == "constants") {
					constants[entry_name] = res->get_path();
				} else if (category == "font_sizes") {
					font_sizes[entry_name] = res->get_path();
				} else if (category == "fonts") {
					fonts[entry_name] = res->get_path();
				} else if (category == "icons") {
					icons[entry_name] = res->get_path();
				} else if (category == "styles") {
					styleboxes[entry_name] = res->get_path();
				}
			}
			continue;
		}

		if (category == "colors") {
			colors[entry_name] = value;
		} else if (category == "constants") {
			constants[entry_name] = value;
		} else if (category == "font_sizes") {
			font_sizes[entry_name] = value;
		} else if (category == "fonts") {
			fonts[entry_name] = value;
		} else if (category == "icons") {
			icons[entry_name] = value;
		} else if (category == "styles") {
			styleboxes[entry_name] = value;
		}
	}

	Dictionary overrides;
	overrides["colors"] = colors;
	overrides["constants"] = constants;
	overrides["font_sizes"] = font_sizes;
	overrides["fonts"] = fonts;
	overrides["icons"] = icons;
	overrides["styleboxes"] = styleboxes;

	Dictionary data;
	data["node_path"] = node_path;
	data["overrides"] = overrides;
	return _make_ok(data);
}

// ---------------------------------------------------------------------------
// theme.set_overrides
// ---------------------------------------------------------------------------

Dictionary OpenCodeMCPProtocol::_method_theme_set_overrides(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	Node *root = _get_edited_scene_root();
	if (!root) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No edited scene is currently open.";
		return Dictionary();
	}

	String node_path = p_params.get("node_path", String());
	if (node_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path is required.";
		return Dictionary();
	}

	Dictionary overrides = p_params.get("overrides", Dictionary());
	if (overrides.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: overrides must be non-empty.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Control *control = Object::cast_to<Control>(node);
	if (!control) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node is not a Control subclass.";
		return Dictionary();
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Theme Overrides", editor_data.get_current_edited_scene_history_id());

	int override_count = 0;

	// Map category names to their property prefix.
	// Categories: colors, constants, font_sizes, fonts, icons, styleboxes (→ "styles" in the property).
	const char *categories[] = { "colors", "constants", "font_sizes", "fonts", "icons", "styleboxes", nullptr };
	const char *prop_categories[] = { "colors", "constants", "font_sizes", "fonts", "icons", "styles", nullptr };

	for (int c = 0; categories[c] != nullptr; c++) {
		String cat_key = categories[c];
		String prop_cat = prop_categories[c];
		if (!overrides.has(cat_key)) {
			continue;
		}
		Variant cat_variant = overrides[cat_key];
		if (cat_variant.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary cat_entries = cat_variant;
		for (const KeyValue<Variant, Variant> &E : cat_entries) {
			String entry_name = E.key;
			String property_path = "theme_override_" + prop_cat + "/" + entry_name;

			bool valid = false;
			Variant old_value = control->get(property_path, &valid);
			if (!valid) {
				r_error_code = ERROR_INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: unknown theme override: " + property_path;
				undo_redo->commit_action(); // Commit whatever we have so far.
				return Dictionary();
			}

			if (E.value.get_type() == Variant::NIL) {
				// Remove override: set to default (nil).
				undo_redo->add_do_property(control, property_path, Variant());
				undo_redo->add_undo_property(control, property_path, old_value);
			} else {
				undo_redo->add_do_property(control, property_path, E.value);
				undo_redo->add_undo_property(control, property_path, old_value);
			}
			override_count++;
		}
	}

	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["override_count"] = override_count;
	return _make_ok(data);
}
