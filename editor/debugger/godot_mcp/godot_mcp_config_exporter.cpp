/**************************************************************************/
/*  godot_mcp_config_exporter.cpp                                         */
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

#include "godot_mcp_config_exporter.h"

#include "core/io/file_access.h"
#include "core/os/os.h"

String GodotMCPConfigExporter::find_bridge_script(const String &p_editor_directory) {
	const String candidates[] = {
		p_editor_directory.path_join("bridge/dist/main.js"),
		p_editor_directory.path_join("../misc/godot_mcp_bridge/dist/main.js").simplify_path(),
	};
	for (const String &candidate : candidates) {
		if (FileAccess::exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

String GodotMCPConfigExporter::find_node_executable(const String &p_editor_directory) {
#ifdef WINDOWS_ENABLED
	const String bundled_node = p_editor_directory.path_join("runtime/node.exe");
#else
	const String bundled_node = p_editor_directory.path_join("runtime/bin/node");
#endif
	return FileAccess::exists(bundled_node) ? bundled_node : String("node");
}

Error GodotMCPConfigExporter::start(const String &p_bridge_script, const String &p_node_executable) {
	if (is_running()) {
		return ERR_BUSY;
	}
	stdout_bytes.clear();
	stderr_bytes.clear();
	error_message = String();
	exit_code = -1;
	if (!p_bridge_script.is_absolute_path() || !FileAccess::exists(p_bridge_script)) {
		error_message = TTR("The Godot MCP bridge was not found. Keep the bridge folder beside the Godot executable, or select its dist/main.js file in Advanced Settings > Bridge Script. For a source checkout, build the Godot MCP bridge first.");
		return ERR_FILE_NOT_FOUND;
	}
	List<String> arguments;
	arguments.push_back(p_bridge_script);
	arguments.push_back("--print-config");
	arguments.push_back("mcp");
	process = OS::get_singleton()->execute_with_pipe(p_node_executable, arguments, false);
	if (int64_t(process.get("pid", 0)) <= 0) {
		process.clear();
		error_message = TTR("Could not start Node.js. Keep the runtime folder beside the Godot executable, or select Node Executable in Advanced Settings.");
		return ERR_CANT_FORK;
	}
	started_at_msec = OS::get_singleton()->get_ticks_msec();
	return OK;
}

void GodotMCPConfigExporter::_read_output() {
	const char *keys[] = { "stdio", "stderr" };
	PackedByteArray *outputs[] = { &stdout_bytes, &stderr_bytes };
	for (int i = 0; i < 2; i++) {
		Ref<FileAccess> pipe = process.get(keys[i], Variant());
		if (pipe.is_null() || !pipe->is_open()) {
			continue;
		}
		// Drain both nonblocking pipes, with bounded work and retained output.
		const PackedByteArray bytes = pipe->get_buffer(65536);
		const int remaining = 65536 - outputs[i]->size();
		if (remaining > 0 && !bytes.is_empty()) {
			outputs[i]->append_array(bytes.slice(0, MIN(remaining, bytes.size())));
		}
	}
}

bool GodotMCPConfigExporter::poll() {
	if (!is_running()) {
		return false;
	}
	_read_output();
	const ProcessID pid = process["pid"];
	if (OS::get_singleton()->is_process_running(pid)) {
		if (OS::get_singleton()->get_ticks_msec() - started_at_msec < 30000) {
			return false;
		}
		OS::get_singleton()->kill(pid);
		error_message = TTR("Bridge setup timed out. Check the selected Node executable and bridge script, then try again.");
	} else {
		exit_code = OS::get_singleton()->get_process_exit_code(pid);
	}
	_read_output();
	process.clear();
	return true;
}

String GodotMCPConfigExporter::get_output() const {
	String output;
	if (!stdout_bytes.is_empty()) {
		output.append_utf8((const char *)stdout_bytes.ptr(), stdout_bytes.size());
	}
	if (!stderr_bytes.is_empty()) {
		output.append_utf8((const char *)stderr_bytes.ptr(), stderr_bytes.size());
	}
	if (!error_message.is_empty()) {
		output += "\n" + error_message;
	}
	return output.strip_edges();
}

GodotMCPConfigExporter::~GodotMCPConfigExporter() {
	if (is_running()) {
		const ProcessID pid = process["pid"];
		if (OS::get_singleton()->is_process_running(pid)) {
			OS::get_singleton()->kill(pid);
		}
	}
}
