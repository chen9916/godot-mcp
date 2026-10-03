/**************************************************************************/
/*  godot_mcp_runtime_controller.cpp                                      */
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

#include "godot_mcp_runtime_controller.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/godot_mcp/godot_mcp_diagnostics_store.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/debugger/godot_mcp/godot_mcp_runtime_inspector.h"
#include "editor/editor_node.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/run_instances_dialog.h"

String GodotMCPRuntimeController::_generate_run_id() {
	uint8_t bytes[16];
	if (CryptoCore::generate_random(bytes, 16) == OK) {
		bytes[6] = (bytes[6] & 0x0f) | 0x40;
		bytes[8] = (bytes[8] & 0x3f) | 0x80;
		const String hex = String::hex_encode_buffer(bytes, 16);
		return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
	}
	return (itos(OS::get_singleton()->get_process_id()) + ":" + uitos(OS::get_singleton()->get_ticks_usec())).md5_text();
}

String GodotMCPRuntimeController::_state_name(State p_state) {
	switch (p_state) {
		case STATE_STARTING:
			return "starting";
		case STATE_RUNNING:
			return "running";
		case STATE_PAUSED:
			return "paused";
		case STATE_DISCONNECTED:
			return "disconnected";
		case STATE_CRASHED:
			return "crashed";
		case STATE_STOPPED:
		default:
			return "stopped";
	}
}

void GodotMCPRuntimeController::initialize(GodotMCPDiagnosticsStore *p_diagnostics_store) {
	if (inspector.is_valid()) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger) {
		return;
	}
	diagnostics_store = p_diagnostics_store;
	inspector.instantiate();
	inspector->set_diagnostics_store(diagnostics_store);
	debugger->add_debugger_plugin(inspector);
}

void GodotMCPRuntimeController::shutdown() {
	if (inspector.is_valid() && EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->remove_debugger_plugin(inspector);
	}
	inspector.unref();
	diagnostics_store = nullptr;
	state = STATE_STOPPED;
	run_id.clear();
	last_error_details.clear();
	process_id = 0;
}

void GodotMCPRuntimeController::poll() {
	if (inspector.is_null() || run_id.is_empty() || state == STATE_STOPPED || state == STATE_CRASHED) {
		return;
	}
	if ((state == STATE_STARTING || state == STATE_DISCONNECTED) && inspector->is_runtime_ready()) {
		state = STATE_RUNNING;
	}

	const bool process_gone = process_id != 0 && !OS::get_singleton()->is_process_running(process_id);
	if (!process_gone && inspector->has_runtime_stopped()) {
		if (state != STATE_DISCONNECTED && diagnostics_store) {
			diagnostics_store->add_runtime(run_id, "error", "Owned runtime debugger connection closed while the process was still running.");
		}
		state = STATE_DISCONNECTED;
		return;
	}
	if (!process_gone) {
		return;
	}

	exit_code = process_id == 0 ? 0 : OS::get_singleton()->get_process_exit_code(process_id);
	state = expected_stop ? STATE_STOPPED : STATE_CRASHED;
	if (!expected_stop && diagnostics_store) {
		diagnostics_store->add_runtime(run_id, "error", "Owned runtime process exited unexpectedly (exit code " + itos(exit_code) + ").");
	}
	process_id = 0;
	inspector->clear_run();
}

bool GodotMCPRuntimeController::_validate_run_id(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const String requested_run_id = p_params.get("run_id", String());
	if (requested_run_id.is_empty()) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: run_id is required.";
		return false;
	}
	if (run_id.is_empty() || requested_run_id != run_id) {
		r_error_code = GodotMCPError::STALE_RUN;
		r_error_message = "STALE_RUN: run_id does not identify the runtime owned by this editor session.";
		return false;
	}
	return true;
}

Dictionary GodotMCPRuntimeController::_status_data() const {
	Dictionary data;
	data["run_id"] = run_id;
	data["state"] = _state_name(state);
	data["process_id"] = (int64_t)process_id;
	data["scene_path"] = scene_path;
	data["headless"] = headless;
	data["runtime_connected"] = inspector.is_valid() && inspector->is_runtime_ready();
	data["started_at_msec"] = (int64_t)started_at_msec;
	data["diagnostics_sequence_start"] = (int64_t)diagnostics_sequence_start;
	data["process_frame"] = inspector.is_valid() ? (int64_t)inspector->get_process_frame() : 0;
	data["physics_frame"] = inspector.is_valid() ? (int64_t)inspector->get_physics_frame() : 0;
	if (diagnostics_store && !run_id.is_empty()) {
		Dictionary diagnostic_params;
		diagnostic_params["after_sequence"] = (int64_t)diagnostics_sequence_start;
		diagnostic_params["limit"] = 100;
		int diagnostic_error = 0;
		String diagnostic_error_message;
		const Dictionary diagnostics = diagnostics_store->query(diagnostic_params, diagnostic_error, diagnostic_error_message);
		data["diagnostics"] = Array();
		if (diagnostic_error == 0) {
			data["diagnostics"] = diagnostics.get("messages", Array());
		}
		data["diagnostics_truncated"] = diagnostic_error == 0 && bool(diagnostics.get("truncated", false));
	}
	if (state == STATE_STOPPED || state == STATE_CRASHED) {
		data["exit_code"] = exit_code;
	}
	return data;
}

Dictionary GodotMCPRuntimeController::start(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	poll();
	if (inspector.is_null()) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: runtime inspector is not initialized.";
		return Dictionary();
	}
	if (state == STATE_STARTING || state == STATE_RUNNING || state == STATE_PAUSED || state == STATE_DISCONNECTED) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: this MCP editor session already owns an active runtime.";
		return Dictionary();
	}
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (!run_bar) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: editor run controller is unavailable.";
		return Dictionary();
	}
	if (run_bar->is_playing()) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: an unrelated editor runtime is already active and will not be stopped.";
		return Dictionary();
	}
	RunInstancesDialog *run_instances = RunInstancesDialog::get_singleton();
	if (!run_instances) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: editor run-instance configuration is unavailable.";
		return Dictionary();
	}
	if (run_instances->get_instance_count() != 1) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: MCP runtime control requires the editor run instance count to be one.";
		return Dictionary();
	}
	if (EditorNode::has_unsaved_changes() && !bool(p_params.get("allow_unsaved", false))) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: the editor has unsaved changes; pass allow_unsaved=true to permit "
						  "Godot's normal autosave-before-run behavior.";
		return Dictionary();
	}

	const String requested_scene = p_params.get("scene_path", String());
	if (!requested_scene.is_empty() && (!requested_scene.begins_with("res://") || !FileAccess::exists(requested_scene))) {
		r_error_code = GodotMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: scene_path must identify an existing res:// scene.";
		return Dictionary();
	}
	const String mode = String(p_params.get("mode", requested_scene.is_empty() ? String("main") : String("scene"))).to_lower();
	if (mode != "main" && mode != "current" && mode != "scene") {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: mode must be main, current, or scene.";
		return Dictionary();
	}
	if (mode == "scene" && requested_scene.is_empty()) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: scene_path is required when mode is scene.";
		return Dictionary();
	}

	run_id = _generate_run_id();
	scene_path = requested_scene;
	headless = bool(p_params.get("headless", false));
	expected_stop = false;
	exit_code = 0;
	started_at_msec = (uint64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
	diagnostics_sequence_start = diagnostics_store ? diagnostics_store->get_latest_sequence() : 0;
	inspector->prepare_run(run_id);

	Vector<String> arguments;
	if (headless) {
		arguments.push_back("--headless");
	}
	arguments.push_back("--");
	arguments.push_back("--godot-mcp-run-id=" + run_id);
	if (mode == "current") {
		run_bar->play_current_scene(false, arguments);
	} else if (mode == "scene") {
		run_bar->play_custom_scene(requested_scene, arguments);
	} else {
		run_bar->play_main_scene(false, arguments);
	}

	if (!run_bar->is_playing()) {
		inspector->clear_run();
		run_id.clear();
		state = STATE_STOPPED;
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: Godot did not start the requested runtime; inspect editor diagnostics.";
		return Dictionary();
	}
	process_id = run_bar->get_current_process();
	inspector->set_expected_pid(process_id);
	state = STATE_STARTING;
	return _status_data();
}

Dictionary GodotMCPRuntimeController::stop(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	poll();
	if (!_validate_run_id(p_params, r_error_code, r_error_message)) {
		return Dictionary();
	}
	if (!bool(p_params.get("confirm", false))) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: stopping a runtime requires confirm=true.";
		return Dictionary();
	}
	if (state == STATE_STOPPED || state == STATE_CRASHED) {
		return _status_data();
	}
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (!run_bar || process_id == 0 || !run_bar->has_child_process(process_id)) {
		r_error_code = GodotMCPError::STALE_RUN;
		r_error_message = "STALE_RUN: the owned runtime process is no longer registered with this editor.";
		return Dictionary();
	}
	expected_stop = true;
	run_bar->stop_child_process(process_id);
	state = STATE_STOPPED;
	process_id = 0;
	inspector->clear_run();
	return _status_data();
}

Dictionary GodotMCPRuntimeController::status(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	poll();
	if (p_params.has("run_id") && !_validate_run_id(p_params, r_error_code, r_error_message)) {
		return Dictionary();
	}
	return _status_data();
}

Dictionary GodotMCPRuntimeController::pause(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	poll();
	if (!_validate_run_id(p_params, r_error_code, r_error_message)) {
		return Dictionary();
	}
	if (state != STATE_RUNNING && state != STATE_PAUSED) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: runtime must be connected and running before it can be paused.";
		return Dictionary();
	}
	if (!inspector->send_scene_control("scene:suspend_changed", { true })) {
		r_error_code = GodotMCPError::DISCONNECTED;
		r_error_message = "DISCONNECTED: runtime debugger session is unavailable.";
		return Dictionary();
	}
	state = STATE_PAUSED;
	return _status_data();
}

Dictionary GodotMCPRuntimeController::resume(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	poll();
	if (!_validate_run_id(p_params, r_error_code, r_error_message)) {
		return Dictionary();
	}
	if (state != STATE_PAUSED && state != STATE_RUNNING) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: runtime is not paused.";
		return Dictionary();
	}
	if (!inspector->send_scene_control("scene:suspend_changed", { false })) {
		r_error_code = GodotMCPError::DISCONNECTED;
		r_error_message = "DISCONNECTED: runtime debugger session is unavailable.";
		return Dictionary();
	}
	state = STATE_RUNNING;
	return _status_data();
}

int GodotMCPRuntimeController::_runtime_error_code(const Variant &p_payload) const {
	if (p_payload.get_type() != Variant::DICTIONARY) {
		return GodotMCPError::INTERNAL;
	}
	const String code = String(Dictionary(p_payload).get("code", String())).to_upper();
	if (code == "INVALID_ARGUMENT" || code == "NON_SERIALIZABLE" || code == "RUNTIME_CALL_ERROR") {
		return GodotMCPError::INVALID_ARGUMENT;
	}
	if (code == "CONFIRMATION_REQUIRED") {
		return GodotMCPError::INVALID_ARGUMENT;
	}
	if (code == "NOT_FOUND" || code == "INVALID_RUNTIME_TARGET") {
		return GodotMCPError::NOT_FOUND;
	}
	if (code == "STALE_RUN") {
		return GodotMCPError::STALE_RUN;
	}
	if (code == "BUSY") {
		return GodotMCPError::CONFLICT;
	}
	return GodotMCPError::INTERNAL;
}

Dictionary GodotMCPRuntimeController::request(const String &p_operation, const Dictionary &p_params,
		int &r_error_code, String &r_error_message) {
	last_error_details.clear();
	poll();
	if (!_validate_run_id(p_params, r_error_code, r_error_message)) {
		return Dictionary();
	}
	if (state != STATE_RUNNING && state != STATE_PAUSED) {
		const bool temporarily_disconnected = state == STATE_STARTING || state == STATE_DISCONNECTED;
		r_error_code = temporarily_disconnected ? GodotMCPError::DISCONNECTED : GodotMCPError::STALE_RUN;
		if (state == STATE_STARTING) {
			r_error_message = "DISCONNECTED: runtime is starting but has not completed its debugger handshake.";
		} else if (state == STATE_DISCONNECTED) {
			r_error_message = "DISCONNECTED: runtime process is active but its debugger connection is unavailable.";
		} else {
			r_error_message = "STALE_RUN: runtime is no longer active.";
		}
		return Dictionary();
	}

	const String request_id = p_params.get("_request_id", String());
	if (bool(p_params.get("_request_ack", false))) {
		Dictionary acknowledged;
		acknowledged["pending"] = false;
		acknowledged["acknowledged"] = inspector->acknowledge(request_id, p_operation);
		acknowledged["request_id"] = request_id;
		acknowledged["run_id"] = run_id;
		return acknowledged;
	}
	bool completed = false;
	bool ok = false;
	bool sent = false;
	Variant payload;
	if (!inspector->request(request_id, p_operation, p_params, completed, ok, payload, sent, r_error_code, r_error_message)) {
		return Dictionary();
	}
	if (!completed) {
		Dictionary pending;
		pending["pending"] = true;
		pending["request_sent"] = sent;
		pending["request_id"] = request_id;
		pending["operation"] = p_operation;
		return pending;
	}
	if (!ok) {
		r_error_code = _runtime_error_code(payload);
		if (payload.get_type() == Variant::DICTIONARY) {
			const Dictionary runtime_error = payload;
			last_error_details["runtime_error"] = runtime_error;
			last_error_details["retryable"] = bool(runtime_error.get("retryable", false));
			const Dictionary runtime_details = runtime_error.get("details", Dictionary());
			if (runtime_details.has("godot_error")) {
				last_error_details["godot_error"] = runtime_details["godot_error"];
			}
			r_error_message = String(runtime_error.get("code", String("RUNTIME_ERROR"))) + ": " +
					String(runtime_error.get("message", String("Runtime operation failed.")));
		} else {
			r_error_message = "INTERNAL: runtime operation failed without structured details.";
		}
		return Dictionary();
	}
	Dictionary result = payload.get_type() == Variant::DICTIONARY ? Dictionary(payload) : Dictionary();
	result["pending"] = false;
	result["request_sent"] = false;
	result["request_id"] = request_id;
	result["run_id"] = run_id;
	return result;
}

Dictionary GodotMCPRuntimeController::advance_frames(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	last_error_details.clear();
	const int frames = (int)p_params.get("frames", 1);
	if (frames < 1 || frames > 600) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: frames must be in range 1..600.";
		return Dictionary();
	}
	Dictionary runtime_params = p_params;
	runtime_params["frame_type"] = String(p_params.get("frame_type", String("physics"))).to_lower();
	Dictionary result = request("wait_frames", runtime_params, r_error_code, r_error_message);
	if (r_error_code != 0) {
		return Dictionary();
	}
	// The runtime counts real node callbacks and restores suspension itself.
	// Batched scene:next_frame messages collapse into one rendered step.
	return result;
}
