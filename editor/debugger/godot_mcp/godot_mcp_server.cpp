/**************************************************************************/
/*  godot_mcp_server.cpp                                                  */
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

#include "godot_mcp_server.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"

namespace {
constexpr uint64_t SESSION_REFRESH_INTERVAL_SEC = 300;
constexpr uint64_t SESSION_WRITE_WARNING_COOLDOWN_SEC = 60;

static Dictionary _make_capabilities() {
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
	capabilities["read_docs"] = true;
	capabilities["script_validation"] = true;
	capabilities["filesystem_sync"] = true;
	capabilities["resource_versioning"] = true;
	capabilities["runtime_control"] = true;
	capabilities["runtime_inspection"] = true;
	capabilities["viewport_capture"] = true;
	capabilities["diagnostics_v2"] = true;
	capabilities["partial_batch_results"] = true;
	return capabilities;
}

static String _generate_session_id() {
	uint8_t bytes[16];
	if (CryptoCore::generate_random(bytes, 16) == OK) {
		// RFC 4122 version 4 / variant 1 bits make logs and tooling recognize the
		// value as a UUID while uniqueness comes from the platform RNG.
		bytes[6] = (bytes[6] & 0x0f) | 0x40;
		bytes[8] = (bytes[8] & 0x3f) | 0x80;
		const String hex = String::hex_encode_buffer(bytes, 16);
		return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
	}

	return (itos(OS::get_singleton()->get_process_id()) + ":" +
			uitos(OS::get_singleton()->get_ticks_usec()))
			.md5_text();
}
} // namespace

GodotMCPServer *GodotMCPServer::singleton = nullptr;

GodotMCPServer::GodotMCPServer() {
	singleton = this;

	EDITOR_DEF_BASIC("network/godot_mcp/enabled", false);
	EDITOR_DEF("network/godot_mcp/bridge_script", "");
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::STRING, "network/godot_mcp/bridge_script", PROPERTY_HINT_GLOBAL_FILE, "*.js"));
	EDITOR_DEF("network/godot_mcp/node_executable", "");
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::STRING, "network/godot_mcp/node_executable", PROPERTY_HINT_GLOBAL_FILE));

	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings && editor_settings->has_setting("network/godot_mcp/remote_port")) {
		editor_settings->erase("network/godot_mcp/remote_port");
	}
}

GodotMCPServer::~GodotMCPServer() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void GodotMCPServer::_notification(int p_what) {
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
			if (EditorSettings::get_singleton()->check_changed_settings_in_group("network/godot_mcp")) {
				_refresh_server_state();
			}
		} break;
	}
}

String GodotMCPServer::_session_file_path() const {
	return "res://.godot/godot_mcp/session.json";
}

bool GodotMCPServer::_write_session_file() const {
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
	session["session_id"] = session_id;
	session["protocol_version"] = 2;
	session["state"] = "connected";
	session["port"] = protocol.get_port();
	session["pid"] = OS::get_singleton()->get_process_id();
	session["project_path"] = ProjectSettings::get_singleton()->get_resource_path();
	session["started_at"] = (int64_t)started_at_unix;
	session["expires_at"] = OS::get_singleton()->get_unix_time() + 3600;
	session["capabilities"] = _make_capabilities();

	file->store_string(Variant(session).to_json_string());
	return true;
}

void GodotMCPServer::_remove_session_file() const {
	const String session_file = _session_file_path();
	if (!FileAccess::exists(session_file)) {
		return;
	}

	// An older editor may finish shutting down after its replacement has
	// already registered. Never delete another process's session metadata.
	Ref<FileAccess> file = FileAccess::open(session_file, FileAccess::READ);
	if (file.is_null()) {
		return;
	}
	JSON json;
	if (json.parse(file->get_as_text()) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary current = json.get_data();
	if (String(current.get("session_id", String())) == session_id) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(session_file));
	}
}

void GodotMCPServer::_refresh_server_state() {
	bool enabled = EDITOR_GET("network/godot_mcp/enabled");

	if (!enabled) {
		stop();
		return;
	}

	if (!started) {
		start();
	}
}

bool GodotMCPServer::_refresh_session_file(bool p_force) {
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
		EditorNode::get_log()->add_message("--- Godot MCP server could not refresh session file ---", EditorLog::MSG_TYPE_WARNING);
		last_session_write_warning_unix = now;
	}

	return false;
}

void GodotMCPServer::start() {
	if (started) {
		return;
	}

	const Dictionary capabilities = _make_capabilities();

	session_id = _generate_session_id();
	started_at_unix = OS::get_singleton()->get_unix_time();
	if (protocol.start(0, capabilities, session_id) != OK) {
		session_id.clear();
		started_at_unix = 0;
		EditorNode::get_log()->add_message("--- Godot MCP server failed to start ---", EditorLog::MSG_TYPE_ERROR);
		return;
	}

	started = true;
	last_session_write_unix = 0;
	last_session_write_warning_unix = 0;
	set_process_internal(true);

	if (!_refresh_session_file(true)) {
		EditorNode::get_log()->add_message("--- Godot MCP server started, but session file could not be written ---", EditorLog::MSG_TYPE_WARNING);
	} else {
		EditorNode::get_log()->add_message("--- Godot MCP server started on port " + itos(protocol.get_port()) + " ---", EditorLog::MSG_TYPE_EDITOR);
	}
}

void GodotMCPServer::stop() {
	if (!started) {
		return;
	}

	_remove_session_file();
	protocol.stop();
	started = false;
	session_id.clear();
	started_at_unix = 0;
	last_session_write_unix = 0;
	last_session_write_warning_unix = 0;
	set_process_internal(false);
	EditorNode::get_log()->add_message("--- Godot MCP server stopped ---", EditorLog::MSG_TYPE_EDITOR);
}

void GodotMCPServer::refresh_session_metadata() {
	_refresh_server_state();

	if (!started) {
		EditorNode::get_log()->add_message("--- Godot MCP session refresh skipped (server is disabled) ---", EditorLog::MSG_TYPE_WARNING);
		return;
	}

	if (_refresh_session_file(true)) {
		EditorNode::get_log()->add_message("--- Godot MCP session metadata refreshed ---", EditorLog::MSG_TYPE_EDITOR);
	}
}
