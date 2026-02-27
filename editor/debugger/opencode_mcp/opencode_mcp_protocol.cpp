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
#include "core/variant/callable.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/object/undo_redo.h"
#include "core/templates/vector.h"
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

static bool _coerce_property_value(const Variant &p_existing_value, const Variant &p_requested_value, Variant &r_coerced_value, String &r_error_message) {
	const Variant::Type expected_type = p_existing_value.get_type();
	if (expected_type == p_requested_value.get_type()) {
		r_coerced_value = p_requested_value;
		return true;
	}

	if (expected_type == Variant::VECTOR3) {
		Vector3 vector_value;
		if (_coerce_variant_to_vector3(p_requested_value, vector_value)) {
			r_coerced_value = vector_value;
			return true;
		}
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

	r_error_message = "INVALID_ARGUMENT: type mismatch for property write (expected " + Variant::get_type_name(expected_type) + ", got " + Variant::get_type_name(p_requested_value.get_type()) + ").";
	return false;
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
			p_method == "node.list_properties" || p_method == "node.get_properties" ||
			p_method == "scene.list" || p_method == "node.find" || p_method == "node.get_groups" ||
			p_method == "signal.list" || p_method == "signal.get_connections" ||
			p_method == "theme.get_overrides") {
		return "read_scene";
	}
	if (p_method == "node.create" || p_method == "node.delete" || p_method == "node.reparent" ||
			p_method == "node.set_properties" || p_method == "script.attach" ||
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
	if (p_method == "resource.save") {
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
	if (p_method == "node.get_property") {
		return _method_node_get_property(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.list_properties") {
		return _method_node_list_properties(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.create") {
		return _method_node_create(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.delete") {
		return _method_node_delete(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.reparent") {
		return _method_node_reparent(p_params, r_error_code, r_error_message);
	}
	if (p_method == "node.set_properties") {
		return _method_node_set_properties(p_params, r_error_code, r_error_message);
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
	if (p_method == "lsp.query") {
		return _method_lsp_query(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.save") {
		return _method_resource_save(p_params, r_error_code, r_error_message);
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
	Dictionary tree = _serialize_node(root, root, 0, max_depth, max_nodes, nodes_seen, truncated);

	Dictionary data;
	data["root"] = tree;
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
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: property value is not serializable: " + String(property_name);
		return Dictionary();
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
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property value is not serializable: " + String(property_name);
			return Dictionary();
		}

		values[String(property_name)] = value;
	}

	Dictionary data;
	data["node_path"] = node_path;
	data["properties"] = values;
	data["property_count"] = values.size();
	return _make_ok(data);
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
		if (!_coerce_property_value(old_value, E.value, coerced_value, property_error)) {
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
	Dictionary properties = p_params.get("properties", Dictionary());

	Array property_entries = p_params.get("property_entries", Array());
	for (int i = 0; i < property_entries.size(); i++) {
		if (property_entries[i].get_type() != Variant::DICTIONARY) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property_entries entries must be dictionaries.";
			return Dictionary();
		}

		Dictionary entry = property_entries[i];
		Variant property_name_variant = entry.get("name", entry.get("property", Variant()));
		if (property_name_variant.get_type() != Variant::STRING && property_name_variant.get_type() != Variant::STRING_NAME) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property_entries entries must include string name.";
			return Dictionary();
		}
		if (!entry.has("value")) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property_entries entries must include value.";
			return Dictionary();
		}

		properties[String(property_name_variant)] = entry["value"];
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
		if (!_coerce_property_value(old_value, E.value, coerced_value, r_error_message)) {
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
	Dictionary built_in = p_params.get("built_in", Dictionary());
	const bool has_built_in = !built_in.is_empty();
	if (has_built_in && !script_path.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide either script_path or built_in, not both.";
		return Dictionary();
	}

	if (script_path.is_empty() && workspace == SCRIPT_WORKSPACE_SCRIPT_EDITOR) {
		String display_name;
		bool is_unsaved = false;
		Ref<Script> active_script = _resolve_active_script_editor_script(display_name, is_unsaved, r_error_code, r_error_message);
		if (active_script.is_null()) {
			return Dictionary();
		}
		if (active_script->is_built_in() || active_script->get_path().begins_with("res://")) {
			script_path = active_script->get_path();
		}
	}

	if (script_path.is_empty() && !has_built_in) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide script_path or built_in when attaching a script.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Ref<Script> script;
	bool created_built_in = false;
	if (has_built_in) {
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
		script = language->make_template(source, node->get_name(), node->get_class());
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
		created_built_in = true;
	} else {
		if (script_path.is_empty() || (!script_path.begins_with("res://") && !script_path.contains("::"))) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: script_path must be a res:// file path or built-in scene resource path.";
			return Dictionary();
		}
		script = ResourceLoader::load(script_path);
		if (script.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: Script resource could not be loaded.";
			return Dictionary();
		}
	}

	Ref<Script> old_script = node->get_script();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Attach Script", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", old_script);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["script_path"] = script_path;
	data["is_built_in"] = script->is_built_in();
	data["created_built_in"] = created_built_in;
	data["workspace"] = workspace;
	Array warnings;
	if (script->is_built_in()) {
		warnings.push_back("Attached built-in script is in editor state; call resource.save on the scene to persist.");
	}
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

		// Save through the full editor path to match manual scene saves.
		EditorNode::get_singleton()->save_scene_to_path(path, true);

		// Keep external-change detection in sync for the just-saved scene.
		EditorData &editor_data = EditorNode::get_editor_data();
		int scene_idx = editor_data.get_edited_scene_from_path(path);
		if (scene_idx < 0) {
			scene_idx = editor_data.get_edited_scene();
		}
		if (scene_idx >= 0) {
			editor_data.set_scene_modified_time(scene_idx, FileAccess::get_modified_time(path));
		}
	} else {
		Ref<Resource> resource = ResourceLoader::load(path);
		if (resource.is_null()) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: resource path could not be loaded.";
			return Dictionary();
		}
		Error save_err = ResourceSaver::save(resource, path);
		if (save_err != OK) {
			r_error_code = ERROR_INTERNAL;
			r_error_message = "INTERNAL: failed to save resource.";
			return Dictionary();
		}

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
				continue; // Skip non-serializable object properties.
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
			if (!valid || value.get_type() == Variant::OBJECT) {
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
	Dictionary initial_properties = p_params.get("properties", Dictionary());

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
		String prop_name = E.key;
		bool valid = false;
		ref_resource->get(prop_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + prop_name;
			return Dictionary();
		}
		ref_resource->set(prop_name, E.value);
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
	Dictionary properties = p_params.get("properties", Dictionary());

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

	// Validate all properties first.
	Dictionary previous_values;
	for (const KeyValue<Variant, Variant> &E : properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = resource->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}
		previous_values[property_name] = old_value;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Resource Properties", editor_data.get_current_edited_scene_history_id());

	for (const KeyValue<Variant, Variant> &E : properties) {
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
