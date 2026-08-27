/**************************************************************************/
/*  opencode_mcp_diagnostics_store.cpp                                    */
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

#include "opencode_mcp_diagnostics_store.h"

#include "opencode_mcp_error.h"

#include "core/os/time.h"

namespace {
constexpr int MAX_MESSAGE_CHARS = 16384;
constexpr int MAX_LOCATION_CHARS = 2048;
constexpr int MAX_STACK_ENTRIES = 64;
constexpr int MAX_STACK_ENTRY_CHARS = 2048;

static String _bounded_string(const String &p_value, int p_limit, bool &r_truncated) {
	if (p_value.length() <= p_limit) {
		return p_value;
	}
	r_truncated = true;
	return p_value.substr(0, p_limit);
}

static Array _bounded_stack_trace(const Array &p_stack_trace, bool &r_truncated) {
	Array bounded;
	const int count = MIN(p_stack_trace.size(), MAX_STACK_ENTRIES);
	for (int i = 0; i < count; i++) {
		bounded.push_back(_bounded_string(String(p_stack_trace[i]), MAX_STACK_ENTRY_CHARS, r_truncated));
	}
	if (p_stack_trace.size() > MAX_STACK_ENTRIES) {
		r_truncated = true;
	}
	return bounded;
}
} // namespace

void OpenCodeMCPDiagnosticsStore::_append(Entry &&p_entry) {
	p_entry.message = _bounded_string(p_entry.message, MAX_MESSAGE_CHARS, p_entry.truncated);
	p_entry.file = _bounded_string(p_entry.file, MAX_LOCATION_CHARS, p_entry.truncated);
	p_entry.function = _bounded_string(p_entry.function, MAX_LOCATION_CHARS, p_entry.truncated);
	p_entry.stack_trace = _bounded_stack_trace(p_entry.stack_trace, p_entry.truncated);
	p_entry.sequence = next_sequence++;
	p_entry.timestamp_msec = (uint64_t)(Time::get_singleton()->get_unix_time_from_system() * 1000.0);
	p_entry.session_id = session_id;

	if ((int)entries.size() < MAX_ENTRIES) {
		entries.push_back(p_entry);
	} else {
		entries[write_position] = p_entry;
	}
	write_position = (write_position + 1) % MAX_ENTRIES;
	if (entry_count < MAX_ENTRIES) {
		entry_count++;
	}
}

Dictionary OpenCodeMCPDiagnosticsStore::_serialize(const Entry &p_entry) {
	Dictionary result;
	result["sequence"] = (int64_t)p_entry.sequence;
	result["timestamp_msec"] = (int64_t)p_entry.timestamp_msec;
	result["timestamp"] = (int64_t)(p_entry.timestamp_msec / 1000);
	result["severity"] = p_entry.severity;
	result["type"] = p_entry.severity; // editor.get_errors v1 compatibility.
	result["source"] = p_entry.source;
	result["message"] = p_entry.message;
	result["text"] = p_entry.message; // editor.get_errors v1 compatibility.
	result["session_id"] = p_entry.session_id;
	if (!p_entry.run_id.is_empty()) {
		result["run_id"] = p_entry.run_id;
	}
	result["file"] = p_entry.file;
	result["line"] = p_entry.line;
	result["column"] = p_entry.column;
	result["function"] = p_entry.function;
	result["stack_trace"] = p_entry.stack_trace;
	result["truncated"] = p_entry.truncated;
	return result;
}

void OpenCodeMCPDiagnosticsStore::add_editor(const String &p_severity, const String &p_message,
		const String &p_file, int p_line, int p_column, const String &p_function, const Array &p_stack_trace) {
	Entry entry;
	entry.severity = p_severity;
	entry.source = "editor";
	entry.message = p_message;
	entry.file = p_file;
	entry.line = p_line;
	entry.column = p_column;
	entry.function = p_function;
	entry.stack_trace = p_stack_trace;
	_append(static_cast<Entry &&>(entry));
}

void OpenCodeMCPDiagnosticsStore::add_runtime(const String &p_run_id, const String &p_severity,
		const String &p_message, const String &p_file, int p_line, int p_column,
		const String &p_function, const Array &p_stack_trace) {
	Entry entry;
	entry.severity = p_severity;
	entry.source = "runtime";
	entry.run_id = p_run_id;
	entry.message = p_message;
	entry.file = p_file;
	entry.line = p_line;
	entry.column = p_column;
	entry.function = p_function;
	entry.stack_trace = p_stack_trace;
	_append(static_cast<Entry &&>(entry));
}

Dictionary OpenCodeMCPDiagnosticsStore::query(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const int limit = (int)p_params.get("limit", 100);
	if (limit < 1 || limit > MAX_ENTRIES) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be in range 1..1000.";
		return Dictionary();
	}

	const int64_t after_sequence = (int64_t)p_params.get("after_sequence", 0);
	if (after_sequence < 0) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: after_sequence must be >= 0.";
		return Dictionary();
	}

	const String requested_session_id = p_params.get("session_id", String());
	const String requested_run_id = p_params.get("run_id", String());
	const String requested_source = String(p_params.get("source", String())).to_lower();
	if (!requested_source.is_empty() && requested_source != "editor" && requested_source != "runtime") {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: source must be editor or runtime.";
		return Dictionary();
	}

	Vector<String> severities;
	const Variant severities_variant = p_params.get("severities", Variant());
	if (severities_variant.get_type() == Variant::ARRAY) {
		for (const Variant &severity : (Array)severities_variant) {
			if (severity.get_type() != Variant::STRING && severity.get_type() != Variant::STRING_NAME) {
				r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: severities entries must be strings.";
				return Dictionary();
			}
			const String normalized = String(severity).to_lower();
			if (normalized != "error" && normalized != "warning" && normalized != "info") {
				r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: severity must be error, warning, or info.";
				return Dictionary();
			}
			severities.push_back(normalized);
		}
	}

	Array messages;
	bool truncated = false;
	const int start = entry_count == MAX_ENTRIES ? write_position : 0;
	for (int i = 0; i < entry_count; i++) {
		const Entry &entry = entries[(start + i) % entries.size()];
		if ((int64_t)entry.sequence <= after_sequence) {
			continue;
		}
		if (!requested_session_id.is_empty() && entry.session_id != requested_session_id) {
			continue;
		}
		if (!requested_run_id.is_empty() && entry.run_id != requested_run_id) {
			continue;
		}
		if (!requested_source.is_empty() && entry.source != requested_source) {
			continue;
		}
		if (!severities.is_empty() && !severities.has(entry.severity)) {
			continue;
		}
		if (messages.size() >= limit) {
			truncated = true;
			break;
		}
		messages.push_back(_serialize(entry));
	}

	Dictionary data;
	data["messages"] = messages;
	data["count"] = messages.size();
	data["truncated"] = truncated;
	data["latest_sequence"] = (int64_t)get_latest_sequence();
	return data;
}

void OpenCodeMCPDiagnosticsStore::clear() {
	entries.clear();
	write_position = 0;
	entry_count = 0;
}
