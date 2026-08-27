/**************************************************************************/
/*  opencode_mcp_runtime_controller.h                                     */
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

#include "opencode_mcp_runtime_inspector.h"

#include "core/os/process_id.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class OpenCodeMCPDiagnosticsStore;
class OpenCodeMCPRuntimeController {
	enum State {
		STATE_STOPPED,
		STATE_STARTING,
		STATE_RUNNING,
		STATE_PAUSED,
		STATE_DISCONNECTED,
		STATE_CRASHED,
	};

	Ref<OpenCodeMCPRuntimeInspector> inspector;
	OpenCodeMCPDiagnosticsStore *diagnostics_store = nullptr;
	State state = STATE_STOPPED;
	String run_id;
	String scene_path;
	ProcessID process_id = 0;
	uint64_t started_at_msec = 0;
	uint64_t diagnostics_sequence_start = 0;
	Dictionary last_error_details;
	int exit_code = 0;
	bool headless = false;
	bool expected_stop = false;

	static String _generate_run_id();
	static String _state_name(State p_state);
	bool _validate_run_id(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	int _runtime_error_code(const Variant &p_payload) const;
	Dictionary _status_data() const;

public:
	void initialize(OpenCodeMCPDiagnosticsStore *p_diagnostics_store);
	void shutdown();
	void poll();

	Dictionary start(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary stop(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary status(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary pause(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary resume(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary advance_frames(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary request(const String &p_operation, const Dictionary &p_params, int &r_error_code, String &r_error_message);

	const String &get_run_id() const { return run_id; }
	const Dictionary &get_last_error_details() const { return last_error_details; }
};
