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
#include "core/templates/local_vector.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class Node;
class Script;

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

	static constexpr int MAX_REQUEST_BYTES = 1024 * 1024;

	Ref<TCPServer> server;
	LocalVector<Client> clients;

	int remote_port = 0;
	bool started = false;
	Dictionary capabilities;

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
	Dictionary _method_node_create(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_delete(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_reparent(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_node_set_properties(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_script_get_active(int &r_error_code, String &r_error_message) const;
	Dictionary _method_script_get(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary _method_script_apply_text_edits(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_script_attach(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary _method_resource_save(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Ref<Script> _find_script_in_tree(Node *p_root, const String &p_script_path) const;
	Ref<Script> _resolve_script(const Dictionary &p_params, String &r_script_path, int &r_error_code, String &r_error_message) const;

	Node *_get_edited_scene_root() const;
	Node *_resolve_node_path(const String &p_node_path, Node *p_root) const;
	Dictionary _serialize_node(Node *p_node, Node *p_root, int p_depth, int p_max_depth, int p_max_nodes, int &r_nodes_seen, bool &r_truncated) const;
	bool _validate_property_value(Node *p_node, const StringName &p_property_name, const Variant &p_value, String &r_error_message) const;

	static int _line_col_to_index(const String &p_text, int p_line, int p_col);
	static bool _apply_text_edits_to_source(const String &p_source, const Array &p_edits, String &r_updated_source, String &r_error_message);

public:
	Error start(int p_requested_port, const Dictionary &p_capabilities);
	void stop();
	void poll();

	bool is_started() const { return started; }
	int get_port() const { return remote_port; }
};
