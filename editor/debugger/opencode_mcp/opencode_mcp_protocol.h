/**************************************************************************/
/*  opencode_mcp_protocol.h                                                */
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

#pragma once

#include "core/typedefs.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class Node;
class Control;
class Resource;
class Shader;
class Script;
class ScriptEditorBase;

class OpenCodeMCPProtocol {
public:
	enum ErrorCode {
		ERROR_CAPABILITY_DENIED = -32002,
		ERROR_INVALID_ARGUMENT = -32003,
		ERROR_CONFLICT = -32004,
		ERROR_NOT_FOUND = -32005,
		ERROR_INTERNAL = -32006,
	};

private:
	struct Client {
		Ref<StreamPeerTCP> peer;
		String read_buffer;
	};

	struct LogEntry {
		String type;
		String text;
		String file;
		String function;
		int line = 0;
		uint64_t timestamp = 0;
	};

	static constexpr int MAX_REQUEST_BYTES = 1024 * 1024;
	static constexpr int MAX_LOG_ENTRIES = 200;

	Ref<TCPServer> server;
	LocalVector<Client> clients;

	int remote_port = 0;
	bool started = false;
	Dictionary capabilities;

	LocalVector<LogEntry> log_ring_buffer;
	int log_ring_write_pos = 0;
	int log_ring_count = 0;
	ErrorHandlerList error_handler;

	static void _error_handler_callback(void *p_user_data, const char *p_function,
			const char *p_file, int p_line, const char *p_error,
			const char *p_message, bool p_editor_notify, ErrorHandlerType p_type);

	String _capability_for_method(const String &p_method) const;
	bool _is_method_allowed(const String &p_method, int &r_error_code, String &r_error_message) const;

	Dictionary _make_response(const Variant &p_result, const Variant &p_id) const;
	Dictionary _make_ok(const Variant &p_data = Variant(), const Array &p_warnings = Array()) const;
	Dictionary _make_error_response(int p_error_code, const String &p_error_message, const Variant &p_id) const;

	Dictionary _process_request_line(const String &p_line);
	Dictionary _dispatch_method(const String &p_method, const Dictionary &p_params, int &r_error_code, String &r_error_message);

	Dictionary _method_session_info() const;
	Dictionary _method_scene_get_active(int &r_error_code, String &r_error_message) const;
	Dictionary _method_scene_get_tree(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_get_property(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_list_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_get_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_create(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_delete(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_reparent(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_set_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_script_get_active(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_script_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_script_apply_text_edits(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_script_attach(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_lsp_query(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_resource_save(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	// Scene management tools.
	Dictionary _method_scene_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_scene_open(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_scene_create(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_scene_instantiate(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	// Node tools.
	Dictionary _method_node_find(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_get_groups(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_node_set_groups(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	// Resource tools.
	Dictionary _method_resource_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_resource_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_resource_create(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_resource_set_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	// Signal tools.
	Dictionary _method_signal_list(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_signal_get_connections(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_signal_connect(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_signal_disconnect(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	// Project/editor tools.
	Dictionary _method_project_get_setting(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_project_set_setting(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_editor_get_errors(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;

	// Shader/theme tools.
	Dictionary _method_shader_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_shader_edit(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_theme_get_overrides(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_theme_set_overrides(const Dictionary &p_params, int &r_error_code, String &r_error_message);

	String _resolve_script_workspace(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	bool _resolve_active_script_editor_context(ScriptEditorBase *&r_editor, Ref<Script> &r_script, String &r_script_path, String &r_display_name, bool &r_is_unsaved, String &r_source, bool &r_has_text_buffer, int &r_error_code, String &r_error_message) const;
	bool _resolve_open_script_editor_context(const Ref<Script> &p_script, const String &p_script_path, ScriptEditorBase *&r_editor, String &r_source, bool &r_has_text_buffer) const;
	bool _validate_script_editor_target(const String &p_requested_script_path, const String &p_active_script_path, int &r_error_code, String &r_error_message) const;
	bool _set_script_editor_buffer_source(ScriptEditorBase *p_editor, const String &p_source) const;
	Ref<Script> _resolve_active_script_editor_script(String &r_display_name, bool &r_is_unsaved, int &r_error_code, String &r_error_message) const;
	Node *_resolve_selected_scene_node(String &r_node_path, int &r_error_code, String &r_error_message) const;
	Ref<Script> _resolve_selected_scene_node_script(String &r_script_path, String &r_node_path, int &r_error_code, String &r_error_message) const;
	Ref<Script> _find_script_in_tree(Node *p_root, const String &p_script_path) const;
	Ref<Script> _resolve_script(const Dictionary &p_params, String &r_script_path, int &r_error_code, String &r_error_message) const;

	Node *_get_edited_scene_root() const;
	Node *_resolve_node_path(const String &p_node_path, Node *p_root) const;
	Dictionary _serialize_node(Node *p_node, Node *p_root, int p_depth, int p_max_depth, int p_max_nodes, int &r_nodes_seen, bool &r_truncated) const;
	bool _validate_property_value(Node *p_node, const StringName &p_property_name, const Variant &p_value, String &r_error_message) const;

	void _collect_files_recursive(const String &p_dir, const Vector<String> &p_extensions,
			bool p_recursive, int p_limit, Vector<String> &r_results, bool &r_truncated) const;

	static int _line_col_to_index(const String &p_text, int p_line, int p_col);
	static bool _apply_text_edits_to_source(const String &p_source, const Array &p_edits, String &r_updated_source, String &r_error_message);

public:
	Error start(int p_requested_port, const Dictionary &p_capabilities);
	void stop();
	void poll();

	bool is_started() const { return started; }
	int get_port() const { return remote_port; }
};
