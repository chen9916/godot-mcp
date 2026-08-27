/**************************************************************************/
/*  opencode_mcp_runtime_inspector.h                                      */
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

#include "core/os/process_id.h"
#include "core/templates/hash_map.h"
#include "editor/debugger/editor_debugger_plugin.h"

class OpenCodeMCPDiagnosticsStore;

class OpenCodeMCPRuntimeInspector : public EditorDebuggerPlugin {
	GDCLASS(OpenCodeMCPRuntimeInspector, EditorDebuggerPlugin);

	struct PendingRequest {
		String operation;
		uint64_t sent_at_usec = 0;
	};

	struct CompletedResponse {
		String operation;
		bool ok = false;
		Variant payload;
		uint64_t completed_at_usec = 0;
	};

	OpenCodeMCPDiagnosticsStore *diagnostics_store = nullptr;
	HashMap<String, PendingRequest> pending_requests;
	HashMap<String, CompletedResponse> completed_responses;
	String active_run_id;
	ProcessID expected_pid = 0;
	ProcessID runtime_pid = 0;
	int active_session = -1;
	uint64_t process_frame = 0;
	uint64_t physics_frame = 0;
	bool runtime_ready = false;
	bool runtime_stopped = false;

	void _session_stopped(int p_session);
	void _add_runtime_diagnostic(const Dictionary &p_diagnostic);
	void _prune_completed_responses();

protected:
	static void _bind_methods();

public:
	virtual void setup_session(int p_session) override;
	virtual bool capture(const String &p_message, const Array &p_data, int p_session) override;
	virtual bool has_capture(const String &p_capture) const override;

	void set_diagnostics_store(OpenCodeMCPDiagnosticsStore *p_store) { diagnostics_store = p_store; }
	void prepare_run(const String &p_run_id);
	void set_expected_pid(ProcessID p_pid) { expected_pid = p_pid; }
	void clear_run();

	bool request(const String &p_request_id, const String &p_operation, const Dictionary &p_params,
			bool &r_completed, bool &r_ok, Variant &r_payload, bool &r_sent,
			int &r_error_code, String &r_error_message);
	bool acknowledge(const String &p_request_id, const String &p_operation);
	bool send_scene_control(const String &p_message, const Array &p_data = Array());

	bool is_runtime_ready() const { return runtime_ready; }
	bool has_runtime_stopped() const { return runtime_stopped; }
	ProcessID get_runtime_pid() const { return runtime_pid; }
	uint64_t get_process_frame() const { return process_frame; }
	uint64_t get_physics_frame() const { return physics_frame; }
};
