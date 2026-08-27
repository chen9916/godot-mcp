/**************************************************************************/
/*  opencode_mcp_runtime_inspector.cpp                                    */
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

#include "opencode_mcp_runtime_inspector.h"

#include "opencode_mcp_diagnostics_store.h"
#include "opencode_mcp_error.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"

namespace {
constexpr uint64_t REQUEST_RETENTION_USEC = 60 * 1000 * 1000;
constexpr int MAX_PENDING_REQUESTS = 128;
}

void OpenCodeMCPRuntimeInspector::_bind_methods() {
}

void OpenCodeMCPRuntimeInspector::setup_session(int p_session) {
	Ref<EditorDebuggerSession> session = get_session(p_session);
	ERR_FAIL_COND(session.is_null());
	session->connect(SNAME("stopped"), callable_mp(this, &OpenCodeMCPRuntimeInspector::_session_stopped).bind(p_session));
}

bool OpenCodeMCPRuntimeInspector::has_capture(const String &p_capture) const {
	return p_capture == "opencode_mcp";
}

bool OpenCodeMCPRuntimeInspector::capture(const String &p_message, const Array &p_data, int p_session) {
	if (p_message == "opencode_mcp:ready") {
		if (p_data.size() != 4 || String(p_data[0]) != active_run_id) {
			return true;
		}
		const ProcessID reported_pid = (ProcessID)(int64_t)p_data[1];
		if (expected_pid != 0 && reported_pid != expected_pid) {
			return true;
		}
		runtime_pid = reported_pid;
		active_session = p_session;
		process_frame = (uint64_t)(int64_t)p_data[2];
		physics_frame = (uint64_t)(int64_t)p_data[3];
		runtime_ready = true;
		runtime_stopped = false;
		return true;
	}

	if (p_message == "opencode_mcp:diagnostic") {
		if (p_data.size() == 2 && String(p_data[0]) == active_run_id && p_data[1].get_type() == Variant::DICTIONARY) {
			_add_runtime_diagnostic(p_data[1]);
		}
		return true;
	}

	if (p_message == "opencode_mcp:response") {
		if (p_data.size() != 5 || String(p_data[1]) != active_run_id) {
			return true;
		}
		const String request_id = p_data[0];
		PendingRequest *pending = pending_requests.getptr(request_id);
		if (!pending || pending->operation != String(p_data[2])) {
			return true;
		}
		CompletedResponse response;
		response.operation = pending->operation;
		response.ok = p_data[3];
		response.payload = p_data[4];
		response.completed_at_usec = OS::get_singleton()->get_ticks_usec();
		if (response.payload.get_type() == Variant::DICTIONARY) {
			const Dictionary payload = response.payload;
			process_frame = (uint64_t)(int64_t)payload.get("process_frame", (int64_t)process_frame);
			physics_frame = (uint64_t)(int64_t)payload.get("physics_frame", (int64_t)physics_frame);
		}
		pending_requests.erase(request_id);
		_prune_completed_responses();
		completed_responses.insert(request_id, response);
		return true;
	}

	return false;
}

void OpenCodeMCPRuntimeInspector::_session_stopped(int p_session) {
	if (p_session != active_session) {
		return;
	}
	runtime_ready = false;
	runtime_stopped = true;
	active_session = -1;
}

void OpenCodeMCPRuntimeInspector::_add_runtime_diagnostic(const Dictionary &p_diagnostic) {
	if (!diagnostics_store) {
		return;
	}
	diagnostics_store->add_runtime(active_run_id,
			String(p_diagnostic.get("severity", String("error"))).to_lower(),
			p_diagnostic.get("message", String()), p_diagnostic.get("file", String()),
			(int)p_diagnostic.get("line", 0), (int)p_diagnostic.get("column", 0),
			p_diagnostic.get("function", String()), p_diagnostic.get("stack_trace", Array()));
}

void OpenCodeMCPRuntimeInspector::_prune_completed_responses() {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	Vector<String> expired;
	for (const KeyValue<String, CompletedResponse> &entry : completed_responses) {
		if (now - entry.value.completed_at_usec > REQUEST_RETENTION_USEC) {
			expired.push_back(entry.key);
		}
	}
	for (const String &request_id : expired) {
		completed_responses.erase(request_id);
	}
}

void OpenCodeMCPRuntimeInspector::prepare_run(const String &p_run_id) {
	active_run_id = p_run_id;
	expected_pid = 0;
	runtime_pid = 0;
	active_session = -1;
	process_frame = 0;
	physics_frame = 0;
	runtime_ready = false;
	runtime_stopped = false;
	pending_requests.clear();
	completed_responses.clear();
}

void OpenCodeMCPRuntimeInspector::clear_run() {
	active_run_id.clear();
	expected_pid = 0;
	runtime_pid = 0;
	active_session = -1;
	runtime_ready = false;
	runtime_stopped = false;
	pending_requests.clear();
	completed_responses.clear();
}

bool OpenCodeMCPRuntimeInspector::request(const String &p_request_id, const String &p_operation,
		const Dictionary &p_params, bool &r_completed, bool &r_ok, Variant &r_payload,
		bool &r_sent, int &r_error_code, String &r_error_message) {
	r_completed = false;
	r_ok = false;
	r_sent = false;
	r_payload = Variant();
	_prune_completed_responses();

	if (p_request_id.is_empty()) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: _request_id is required for asynchronous runtime operations.";
		return false;
	}
	if (!runtime_ready || active_session < 0) {
		r_error_code = OpenCodeMCPError::DISCONNECTED;
		r_error_message = "DISCONNECTED: the requested runtime has not connected to the editor debugger.";
		return false;
	}

	if (CompletedResponse *completed = completed_responses.getptr(p_request_id)) {
		if (completed->operation != p_operation) {
			r_error_code = OpenCodeMCPError::CONFLICT;
			r_error_message = "CONFLICT: _request_id was already used for another runtime operation.";
			return false;
		}
		r_completed = true;
		r_ok = completed->ok;
		r_payload = completed->payload;
		return true;
	}

	if (PendingRequest *pending = pending_requests.getptr(p_request_id)) {
		if (pending->operation != p_operation) {
			r_error_code = OpenCodeMCPError::CONFLICT;
			r_error_message = "CONFLICT: _request_id is pending for another runtime operation.";
			return false;
		}
		if (OS::get_singleton()->get_ticks_usec() - pending->sent_at_usec > REQUEST_RETENTION_USEC) {
			pending_requests.erase(p_request_id);
			r_error_code = OpenCodeMCPError::TIMEOUT;
			r_error_message = "TIMEOUT: runtime operation did not respond within 60 seconds.";
			return false;
		}
		return true;
	}

	if (pending_requests.size() + completed_responses.size() >= MAX_PENDING_REQUESTS) {
		r_error_code = OpenCodeMCPError::CONFLICT;
		r_error_message = "CONFLICT: too many runtime operations are pending.";
		return false;
	}

	Ref<EditorDebuggerSession> session = get_session(active_session);
	if (session.is_null() || !session->is_active()) {
		r_error_code = OpenCodeMCPError::DISCONNECTED;
		r_error_message = "DISCONNECTED: the runtime debugger session is no longer active.";
		return false;
	}

	PendingRequest pending;
	pending.operation = p_operation;
	pending.sent_at_usec = OS::get_singleton()->get_ticks_usec();
	pending_requests.insert(p_request_id, pending);
	session->send_message("opencode_mcp:request", { p_request_id, active_run_id, p_operation, p_params });
	r_sent = true;
	return true;
}

bool OpenCodeMCPRuntimeInspector::acknowledge(const String &p_request_id, const String &p_operation) {
	_prune_completed_responses();
	CompletedResponse *completed = completed_responses.getptr(p_request_id);
	if (!completed || completed->operation != p_operation) {
		return false;
	}
	completed_responses.erase(p_request_id);
	return true;
}

bool OpenCodeMCPRuntimeInspector::send_scene_control(const String &p_message, const Array &p_data) {
	if (!runtime_ready || active_session < 0) {
		return false;
	}
	Ref<EditorDebuggerSession> session = get_session(active_session);
	if (session.is_null() || !session->is_active()) {
		return false;
	}
	session->send_message(p_message, p_data);
	return true;
}
