/**************************************************************************/
/*  opencode_mcp_server.cpp                                                */
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

#include "opencode_mcp_server.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"

namespace {
constexpr uint64_t SESSION_REFRESH_INTERVAL_SEC = 300;
constexpr uint64_t SESSION_WRITE_WARNING_COOLDOWN_SEC = 60;
}

OpenCodeMCPServer *OpenCodeMCPServer::singleton = nullptr;

OpenCodeMCPServer::OpenCodeMCPServer() {
	singleton = this;

	_EDITOR_DEF("network/opencode_mcp/enabled", false);

	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings && editor_settings->has_setting("network/opencode_mcp/remote_port")) {
		editor_settings->erase("network/opencode_mcp/remote_port");
	}
}

OpenCodeMCPServer::~OpenCodeMCPServer() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void OpenCodeMCPServer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_refresh_server_state();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (started) {
				protocol.poll();
				_refresh_session_file(false);
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (EditorSettings::get_singleton()->check_changed_settings_in_group("network/opencode_mcp")) {
				_refresh_server_state();
			}
		} break;
	}
}

String OpenCodeMCPServer::_session_file_path() const {
	return "res://.godot/opencode_mcp/session.json";
}

bool OpenCodeMCPServer::_write_session_file() const {
	String session_file = _session_file_path();
	String session_dir_abs = ProjectSettings::get_singleton()->globalize_path(session_file.get_base_dir());
	if (DirAccess::make_dir_recursive_absolute(session_dir_abs) != OK) {
		return false;
	}

	Ref<FileAccess> file = FileAccess::open(session_file, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}

	Dictionary session;
	session["port"] = protocol.get_port();
	session["pid"] = OS::get_singleton()->get_process_id();
	session["expires_at"] = OS::get_singleton()->get_unix_time() + 3600;
	Dictionary session_capabilities;
	session_capabilities["read_scene"] = true;
	session_capabilities["write_scene"] = true;
	session_capabilities["read_script"] = true;
	session_capabilities["write_script"] = true;
	session_capabilities["save_resource"] = true;
	session_capabilities["read_resource"] = true;
	session_capabilities["write_resource"] = true;
	session_capabilities["read_project"] = true;
	session_capabilities["write_project"] = true;
	session["capabilities"] = session_capabilities;

	file->store_string(Variant(session).to_json_string());
	return true;
}

void OpenCodeMCPServer::_remove_session_file() const {
	const String session_file = _session_file_path();
	if (FileAccess::exists(session_file)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(session_file));
	}
}

void OpenCodeMCPServer::_refresh_server_state() {
	bool enabled = EDITOR_GET("network/opencode_mcp/enabled");

	if (!enabled) {
		stop();
		return;
	}

	if (!started) {
		start();
	}
}

bool OpenCodeMCPServer::_refresh_session_file(bool p_force) {
	if (!started) {
		return false;
	}

	const uint64_t now = OS::get_singleton()->get_unix_time();
	if (!p_force && now < last_session_write_unix + SESSION_REFRESH_INTERVAL_SEC) {
		return true;
	}

	if (_write_session_file()) {
		last_session_write_unix = now;
		return true;
	}

	if (now >= last_session_write_warning_unix + SESSION_WRITE_WARNING_COOLDOWN_SEC) {
		EditorNode::get_log()->add_message("--- OpenCode MCP server could not refresh session file ---", EditorLog::MSG_TYPE_WARNING);
		last_session_write_warning_unix = now;
	}

	return false;
}

void OpenCodeMCPServer::start() {
	if (started) {
		return;
	}

	Dictionary capabilities;
	capabilities["read_scene"] = true;
	capabilities["write_scene"] = true;
	capabilities["read_script"] = true;
	capabilities["write_script"] = true;
	capabilities["save_resource"] = true;
	capabilities["read_resource"] = true;
	capabilities["write_resource"] = true;
	capabilities["read_project"] = true;
	capabilities["write_project"] = true;

	if (protocol.start(0, capabilities) != OK) {
		EditorNode::get_log()->add_message("--- OpenCode MCP server failed to start ---", EditorLog::MSG_TYPE_ERROR);
		return;
	}

	started = true;
	last_session_write_unix = 0;
	last_session_write_warning_unix = 0;
	set_process_internal(true);

	if (!_refresh_session_file(true)) {
		EditorNode::get_log()->add_message("--- OpenCode MCP server started, but session file could not be written ---", EditorLog::MSG_TYPE_WARNING);
	} else {
		EditorNode::get_log()->add_message("--- OpenCode MCP server started on port " + itos(protocol.get_port()) + " ---", EditorLog::MSG_TYPE_EDITOR);
	}
}

void OpenCodeMCPServer::stop() {
	if (!started) {
		return;
	}

	protocol.stop();
	started = false;
	last_session_write_unix = 0;
	last_session_write_warning_unix = 0;
	set_process_internal(false);
	_remove_session_file();

	EditorNode::get_log()->add_message("--- OpenCode MCP server stopped ---", EditorLog::MSG_TYPE_EDITOR);
}

void OpenCodeMCPServer::refresh_session_metadata() {
	_refresh_server_state();

	if (!started) {
		EditorNode::get_log()->add_message("--- OpenCode MCP session refresh skipped (server is disabled) ---", EditorLog::MSG_TYPE_WARNING);
		return;
	}

	if (_refresh_session_file(true)) {
		EditorNode::get_log()->add_message("--- OpenCode MCP session metadata refreshed ---", EditorLog::MSG_TYPE_EDITOR);
	}
}
