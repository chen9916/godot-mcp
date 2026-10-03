/**************************************************************************/
/*  godot_mcp_error.h                                                     */
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

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class GodotMCPError {
public:
	enum Code {
		CAPABILITY_DENIED = -32002,
		INVALID_ARGUMENT = -32003,
		CONFLICT = -32004,
		NOT_FOUND = -32005,
		INTERNAL = -32006,
		DISCONNECTED = -32007,
		TIMEOUT = -32008,
		STALE_SESSION = -32009,
		STALE_RUN = -32010,
	};

	static String code_name(int p_code);
	static bool is_retryable(int p_code);
	static String strip_code_prefix(const String &p_message);
	static Dictionary make_data(int p_code, const String &p_message, const String &p_operation,
			const String &p_session_id = String(), const String &p_run_id = String(),
			const Dictionary &p_details = Dictionary());
};
