/**************************************************************************/
/*  opencode_mcp_server.h                                                  */
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

#include "opencode_mcp_protocol.h"
#include "editor/plugins/editor_plugin.h"

class OpenCodeMCPServer : public EditorPlugin {
	GDCLASS(OpenCodeMCPServer, EditorPlugin);

	static OpenCodeMCPServer *singleton;

	OpenCodeMCPProtocol protocol;

	bool started = false;
	uint64_t last_session_write_unix = 0;
	uint64_t last_session_write_warning_unix = 0;

	void _notification(int p_what);

	String _session_file_path() const;
	bool _write_session_file() const;
	bool _refresh_session_file(bool p_force);
	void _remove_session_file() const;
	void _refresh_server_state();

public:
	static OpenCodeMCPServer *get_singleton() { return singleton; }

	OpenCodeMCPServer();
	~OpenCodeMCPServer() override;

	void start();
	void stop();
	void refresh_session_metadata();
};
