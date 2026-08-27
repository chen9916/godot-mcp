/**************************************************************************/
/*  opencode_mcp_resource_versioning.h                                    */
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

class Node;
class Resource;

class OpenCodeMCPResourceVersioning {
	static void _collect_scene_references(Node *p_node, Node *p_root, const String &p_path,
			int p_max_nodes, int &r_nodes_seen, Array &r_references);
	static String _get_editor_buffer_version(const Ref<Resource> &p_resource, const String &p_path,
			bool &r_has_buffer, bool &r_unsaved);

public:
	static bool has_editor_buffer_conflict(bool p_has_buffer, const String &p_disk_version, const String &p_buffer_version);
	Dictionary get_status(const Dictionary &p_params, int &r_error_code, String &r_error_message) const;
	Dictionary reload(const Dictionary &p_params, int &r_error_code, String &r_error_message,
			Dictionary *r_error_details = nullptr) const;
};
