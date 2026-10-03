/**************************************************************************/
/*  godot_mcp_filesystem_sync.h                                           */
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

#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class GodotMCPDiagnosticsStore;

class GodotMCPFilesystemSync {
	uint64_t scan_sequence = 0;
	String active_scan_id;
	String session_id;
	uint64_t scan_started_msec = 0;

	Dictionary _status() const;

public:
	void set_session_id(const String &p_session_id) { session_id = p_session_id; }
	Dictionary scan(const Dictionary &p_params, int &r_error_code, String &r_error_message);
	Dictionary wait_status(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary import_resources(const Dictionary &p_params, GodotMCPDiagnosticsStore &p_diagnostics,
			int &r_error_code, String &r_error_message, Dictionary *r_error_details = nullptr);
};
