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

#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/undo_redo.h"
#include "core/templates/vector.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/3d/node_3d.h"
#include "scene/main/node.h"
#include "core/object/script_language.h"

namespace {
struct TextEditRange {
	int from = 0;
	int to = 0;
	String text;
};
} // namespace

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
	return OK;
}

void OpenCodeMCPProtocol::stop() {
	if (!started) {
		return;
	}

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
	if (p_method == "scene.get_active" || p_method == "scene.get_tree") {
		return "read_scene";
	}
	if (p_method == "node.create" || p_method == "node.delete" || p_method == "node.reparent" || p_method == "node.set_properties") {
		return "write_scene";
	}
	if (p_method == "script.get" || p_method == "script.get_active") {
		return "read_script";
	}
	if (p_method == "script.apply_text_edits" || p_method == "script.attach") {
		return "write_script";
	}
	if (p_method == "resource.save") {
		return "save_resource";
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
		return _method_script_get_active(r_error_code, r_error_message);
	}
	if (p_method == "script.apply_text_edits") {
		return _method_script_apply_text_edits(p_params, r_error_code, r_error_message);
	}
	if (p_method == "script.attach") {
		return _method_script_attach(p_params, r_error_code, r_error_message);
	}
	if (p_method == "resource.save") {
		return _method_resource_save(p_params, r_error_code, r_error_message);
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

	for (const KeyValue<Variant, Variant> &E : initial_properties) {
		StringName property_name = E.key;
		String property_error;
		if (!_validate_property_value(node, property_name, E.value, property_error)) {
			memdelete(node);
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = property_error;
			return Dictionary();
		}
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Create Node", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(parent, "add_child", node, true);
	if (position >= 0) {
		undo_redo->add_do_method(parent, "move_child", node, position);
	}
	undo_redo->add_do_method(node, "set_owner", root);
	for (const KeyValue<Variant, Variant> &E : initial_properties) {
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
	if (node_path.is_empty() || properties.is_empty()) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and non-empty properties are required.";
		return Dictionary();
	}

	Node *node = _resolve_node_path(node_path, root);
	if (!node) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Node path does not exist.";
		return Dictionary();
	}

	Dictionary previous_values;
	for (const KeyValue<Variant, Variant> &E : properties) {
		StringName property_name = E.key;
		bool valid = false;
		Variant old_value = node->get(property_name, &valid);
		if (!valid) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: unknown property: " + String(property_name);
			return Dictionary();
		}
		if (!_validate_property_value(node, property_name, E.value, r_error_message)) {
			r_error_code = ERROR_INVALID_ARGUMENT;
			return Dictionary();
		}
		previous_values[property_name] = old_value;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Set Node Properties", editor_data.get_current_edited_scene_history_id());

	for (const KeyValue<Variant, Variant> &E : properties) {
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

Dictionary OpenCodeMCPProtocol::_method_script_get_active(int &r_error_code, String &r_error_message) const {
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (!script_editor) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: Script editor is not available.";
		return Dictionary();
	}

	ScriptEditorBase *current_editor = script_editor->get_current_editor();
	if (!current_editor) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: No script editor tab is currently active.";
		return Dictionary();
	}

	Ref<Script> script = current_editor->get_edited_resource();
	if (script.is_null()) {
		r_error_code = ERROR_NOT_FOUND;
		r_error_message = "NOT_FOUND: The active editor tab is not a script.";
		return Dictionary();
	}

	const String source = script->get_source_code();
	Dictionary data;
	data["script_path"] = script->get_path();
	data["display_name"] = current_editor->get_name();
	data["is_built_in"] = script->is_built_in();
	data["is_unsaved"] = current_editor->is_unsaved();
	data["source"] = source;
	data["version"] = source.md5_text();
	return _make_ok(data);
}

Dictionary OpenCodeMCPProtocol::_method_script_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const String script_path_param = p_params.get("script_path", String());
	const String node_path_param = p_params.get("node_path", String());
	if (script_path_param.is_empty() && node_path_param.is_empty()) {
		return _method_script_get_active(r_error_code, r_error_message);
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

		const int from = _line_col_to_index(p_source, start_line, start_col);
		const int to = _line_col_to_index(p_source, end_line, end_col);
		if (from < 0 || to < 0 || from > to) {
			r_error_message = "INVALID_ARGUMENT: invalid edit range.";
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
			r_error_message = "INVALID_ARGUMENT: overlapping edits are not supported.";
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

	String script_path;
	Ref<Script> script = _resolve_script(p_params, script_path, r_error_code, r_error_message);
	if (script.is_null()) {
		return Dictionary();
	}

	String current_source = script->get_source_code();
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

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorData &editor_data = EditorNode::get_editor_data();
	undo_redo->create_action_for_history("OpenCode: Apply Script Edits", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(script.ptr(), "set_source_code", updated_source);
	undo_redo->add_undo_method(script.ptr(), "set_source_code", current_source);
	undo_redo->add_do_method(script.ptr(), "update_exports");
	undo_redo->add_undo_method(script.ptr(), "update_exports");

	undo_redo->commit_action();

	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor) {
		script_editor->notify_script_changed(script);
		if (!script_path.is_empty()) {
			script_editor->trigger_live_script_reload(script_path);
		}
	}

	Dictionary data;
	data["script_path"] = script_path;
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

	String node_path = p_params.get("node_path", String());
	String script_path = p_params.get("script_path", String());
	if (node_path.is_empty() || !script_path.begins_with("res://")) {
		r_error_code = ERROR_INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: node_path and res:// script_path are required.";
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
	undo_redo->create_action_for_history("OpenCode: Attach Script", editor_data.get_current_edited_scene_history_id());
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", old_script);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = node_path;
	data["script_path"] = script_path;
	return _make_ok(data);
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

	if (path.ends_with(".tscn") || path.ends_with(".scn")) {
		if (!root) {
			r_error_code = ERROR_NOT_FOUND;
			r_error_message = "NOT_FOUND: no active edited scene.";
			return Dictionary();
		}
		EditorNode::get_singleton()->save_scene_to_path(path, false);
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
	}

	Dictionary data;
	data["path"] = path;
	return _make_ok(data);
}
