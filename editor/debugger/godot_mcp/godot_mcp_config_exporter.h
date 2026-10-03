/**************************************************************************/
/*  godot_mcp_config_exporter.h                                           */
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

// Runs the bundled bridge CLI without a shell or a blocking editor call.
class GodotMCPConfigExporter {
	Dictionary process;
	PackedByteArray stdout_bytes;
	PackedByteArray stderr_bytes;
	String error_message;
	uint64_t started_at_msec = 0;
	int exit_code = -1;

	void _read_output();

public:
	static String find_bridge_script(const String &p_editor_directory);
	static String find_node_executable(const String &p_editor_directory);

	Error start(const String &p_bridge_script, const String &p_node_executable);
	bool poll(); // Returns true when a running export has finished.
	bool is_running() const { return !process.is_empty(); }
	int get_exit_code() const { return exit_code; }
	String get_output() const;

	~GodotMCPConfigExporter();
};
