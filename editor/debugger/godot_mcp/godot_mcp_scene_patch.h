/**************************************************************************/
/*  godot_mcp_scene_patch.h                                               */
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class EditorUndoRedoManager;
class Node;
class UndoRedo;

// Plans and applies a declarative scene patch without touching scene text.
// The Context split keeps the transaction core usable by focused native tests
// with a plain UndoRedo while production routes it through the editor manager.
class GodotMCPScenePatch {
public:
	struct Context {
		Node *root = nullptr;
		String scene_path;
		String scene_version;
		int history_id = 0;
		UndoRedo *undo_redo = nullptr;
		EditorUndoRedoManager *undo_redo_manager = nullptr;
	};

	static constexpr int MAX_OPERATIONS = 512;

	static String compute_scene_version(Node *p_root, int p_history_id, uint64_t p_undo_version);

	Dictionary apply(const Dictionary &p_params, const Context &p_context, Array &r_warnings,
			int &r_error_code, String &r_error_message, Dictionary *r_error_details = nullptr) const;
};
