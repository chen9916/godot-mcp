/**************************************************************************/
/*  opencode_mcp_error.cpp                                                */
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

#include "opencode_mcp_error.h"

String OpenCodeMCPError::code_name(int p_code) {
	switch (p_code) {
		case CAPABILITY_DENIED:
			return "CAPABILITY_DENIED";
		case INVALID_ARGUMENT:
		case -32602:
			return "INVALID_ARGUMENT";
		case CONFLICT:
			return "CONFLICT";
		case NOT_FOUND:
			return "NOT_FOUND";
		case INTERNAL:
			return "INTERNAL";
		case DISCONNECTED:
			return "DISCONNECTED";
		case TIMEOUT:
			return "TIMEOUT";
		case STALE_SESSION:
			return "STALE_SESSION";
		case STALE_RUN:
			return "STALE_RUN";
		case -32700:
		case -32600:
			return "PROTOCOL_ERROR";
		case -32601:
			return "METHOD_NOT_FOUND";
		default:
			return "GODOT_RPC_ERROR";
	}
}

bool OpenCodeMCPError::is_retryable(int p_code) {
	return p_code == DISCONNECTED || p_code == TIMEOUT || p_code == STALE_SESSION;
}

String OpenCodeMCPError::strip_code_prefix(const String &p_message) {
	const int separator = p_message.find(": ");
	if (separator <= 0) {
		return p_message;
	}
	const String candidate = p_message.substr(0, separator);
	for (int i = 0; i < candidate.length(); i++) {
		const char32_t c = candidate[i];
		if (c != '_' && (c < 'A' || c > 'Z')) {
			return p_message;
		}
	}
	return p_message.substr(separator + 2);
}

Dictionary OpenCodeMCPError::make_data(int p_code, const String &p_message, const String &p_operation,
		const String &p_session_id, const String &p_run_id, const Dictionary &p_details) {
	Dictionary data;
	data["code"] = code_name(p_code);
	data["message"] = strip_code_prefix(p_message);
	data["operation"] = p_operation;
	data["session_id"] = p_session_id;
	if (!p_run_id.is_empty()) {
		data["run_id"] = p_run_id;
	}
	data["retryable"] = p_details.has("retryable") ? bool(p_details["retryable"]) : is_retryable(p_code);
	if (p_code == CONFLICT) {
		Dictionary conflict = p_details;
		if (conflict.is_empty()) {
			conflict["reason"] = strip_code_prefix(p_message);
		}
		data["conflict"] = conflict;
	} else if (!p_details.is_empty()) {
		data["details"] = p_details;
	}
	if (p_details.has("godot_error")) {
		data["godot_error"] = p_details["godot_error"];
	}
	return data;
}
