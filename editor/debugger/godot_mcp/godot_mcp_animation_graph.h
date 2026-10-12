/**************************************************************************/
/*  godot_mcp_animation_graph.h                                           */
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

#include "editor/debugger/godot_mcp/godot_mcp_scene_patch.h"

// Editor-only graph transactions. Results are data, not protocol envelopes.
// Graph paths are slash-separated child names; the empty path is tree_root.
class GodotMCPAnimationGraph {
public:
	static constexpr int MAX_OPERATIONS = 512;
	static constexpr int MAX_NODES = 2000;
	static constexpr int MAX_DEPTH = 32;

	// Call before computing a scene version: AnimationTree's dynamic parameter
	// enumeration assumes an acyclic, bounded native resource hierarchy.
	// Graph authoring additionally rejects editor-executing tree scripts, but
	// preserves ordinary controller placeholders and other scene-tool behavior.
	static bool preflight_scene(Node *p_root, String &r_message, bool p_reject_editor_scripts = false);

	Dictionary inspect(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
			int &r_error_code, String &r_error_message) const;
	Dictionary validate(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
			int &r_error_code, String &r_error_message) const;
	Dictionary apply(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
			Array &r_warnings, int &r_error_code, String &r_error_message) const;
};
