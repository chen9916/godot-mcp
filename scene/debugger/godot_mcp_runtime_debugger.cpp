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
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

namespace {
constexpr int MAX_SERIALIZED_ITEMS = 2000;
constexpr int MAX_SERIALIZED_CHARACTERS = 256 * 1024;
constexpr int MAX_PENDING_REQUESTS = 128;
constexpr int MAX_FRAME_WAIT = 600;
constexpr int MAX_CAPTURE_DIMENSION = 4096;

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
		Object *object = p_value;
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
		motion->set_relative(relative);
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
		mouse_button->set_button_index((MouseButton)button);
		mouse_button->set_pressed(bool(p_params.get("pressed", true)));
		mouse_button->set_double_click(bool(p_params.get("double_click", false)));
		event = mouse_button;
	} else {
		_send_error(p_request_id, "input_event", "INVALID_ARGUMENT", "type must be mouse_motion or mouse_button.");
		return;
	}
	Input::get_singleton()->parse_input_event(event);
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
