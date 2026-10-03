/**************************************************************************/
/*  godot_mcp_diagnostics_store.h                                         */
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

#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class GodotMCPDiagnosticsStore {
	struct Entry {
		uint64_t sequence = 0;
		uint64_t timestamp_msec = 0;
		String severity;
		String source;
		String message;
		String session_id;
		String run_id;
		String file;
		String function;
		int line = 0;
		int column = 0;
		Array stack_trace;
		bool truncated = false;
	};

	static constexpr int MAX_ENTRIES = 1000;

	LocalVector<Entry> entries;
	int write_position = 0;
	int entry_count = 0;
	uint64_t next_sequence = 1;
	String session_id;

	void _append(Entry &&p_entry);
	static Dictionary _serialize(const Entry &p_entry);

public:
	void set_session_id(const String &p_session_id) { session_id = p_session_id; }
	const String &get_session_id() const { return session_id; }

	void add_editor(const String &p_severity, const String &p_message, const String &p_file = String(),
			int p_line = 0, int p_column = 0, const String &p_function = String(), const Array &p_stack_trace = Array());
	void add_runtime(const String &p_run_id, const String &p_severity, const String &p_message,
			const String &p_file = String(), int p_line = 0, int p_column = 0,
			const String &p_function = String(), const Array &p_stack_trace = Array());

	Dictionary query(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	void clear();
	uint64_t get_latest_sequence() const { return next_sequence > 0 ? next_sequence - 1 : 0; }
};
