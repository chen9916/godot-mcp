/**************************************************************************/
/*  test_godot_mcp_scene_save.cpp                                         */
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

#ifdef TOOLS_ENABLED

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "editor/editor_data.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

TEST_FORCE_LINK(test_godot_mcp_scene_save)

namespace TestGodotMCPSceneSave {

TEST_CASE("[GodotMCP][SceneSave] Disk guards detect rewritten, removed and restored content") {
	const String directory = TestUtils::get_temp_path("scene_save_guard");
	REQUIRE(DirAccess::make_dir_recursive_absolute(directory) == OK);
	const String path = directory.path_join("guard.tscn");
	DirAccess::remove_absolute(path);
	CHECK(EditorData::get_scene_disk_version(path) == "missing");
	CHECK_FALSE(EditorData::is_scene_disk_version_current(String(), path));
	CHECK(EditorData::is_scene_disk_version_current("missing", path));

	const String original = "[gd_scene format=3]\n\n[node name=\"Root\" type=\"Node\"]\n";
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(original);
	file.unref();
	const String loaded_version = EditorData::get_scene_disk_version(path);
	REQUIRE_FALSE(loaded_version.is_empty());
	CHECK(EditorData::is_scene_disk_version_current(loaded_version, path));
	CHECK_FALSE(EditorData::is_scene_disk_version_current("missing", path));
	CHECK_FALSE(EditorData::is_scene_disk_version_current(String(), path));

	// No timestamp comparison is involved: the original content baseline remains authoritative.
	file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(original + "\n[node name=\"External\" type=\"Node\" parent=\".\"]\n");
	file.unref();
	CHECK_FALSE(EditorData::is_scene_disk_version_current(loaded_version, path));
	const String reloaded_version = EditorData::get_scene_disk_version(path);
	CHECK(EditorData::is_scene_disk_version_current(reloaded_version, path));

	REQUIRE(DirAccess::remove_absolute(path) == OK);
	CHECK_FALSE(EditorData::is_scene_disk_version_current(loaded_version, path));
	CHECK_FALSE(EditorData::is_scene_disk_version_current(reloaded_version, path));

	file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(original);
	file.unref();
	CHECK(EditorData::is_scene_disk_version_current(loaded_version, path));
	CHECK_FALSE(EditorData::is_scene_disk_version_current(reloaded_version, path));
	CHECK_FALSE(EditorData::is_scene_disk_version_current(loaded_version, directory.path_join("other.tscn")));
	CHECK(DirAccess::remove_absolute(path) == OK);
}

TEST_CASE("[GodotMCP][SceneSave] Accepted disk baselines follow scene tabs and survive memory replacement") {
	const String directory = TestUtils::get_temp_path("scene_save_tab_baselines");
	REQUIRE(DirAccess::make_dir_recursive_absolute(directory) == OK);
	const String first_path = directory.path_join("first.tscn");
	const String second_path = directory.path_join("second.tscn");
	Ref<FileAccess> file = FileAccess::open(first_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("original scene content");
	file.unref();
	const String first_version = EditorData::get_scene_disk_version(first_path);

	EditorUndoRedoManager *previous_manager = EditorUndoRedoManager::get_singleton();
	{
		EditorData data;
		const int first = data.add_edited_scene(-1);
		const int second = data.add_edited_scene(-1);
		Node *first_root = memnew(Node);
		first_root->set_scene_file_path(first_path);
		Node *second_root = memnew(Node);
		second_root->set_scene_file_path(second_path);
		data.set_scene_root(first, first_root);
		data.set_scene_root(second, second_root);
		CHECK(data.get_scene_file_version(first, first_path).is_empty());
		data.set_scene_file_version(first, first_path, first_version);
		data.set_scene_file_version(second, second_path, "missing");
		CHECK(data.get_scene_file_version(first, second_path).is_empty());
		CHECK(data.get_scene_file_version(second, first_path).is_empty());

		// Editor reinstances replace roots from memory, not by accepting new disk content.
		file = FileAccess::open(first_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("external scene content");
		file.unref();
		Node *replacement = memnew(Node);
		replacement->set_scene_file_path(first_path);
		data.set_scene_root(first, replacement);
		memdelete(first_root);
		CHECK(data.get_scene_file_version(first, first_path) == first_version);
		CHECK_FALSE(EditorData::is_scene_disk_version_current(data.get_scene_file_version(first, first_path), first_path));

		data.move_scene_to_index(first, second);
		CHECK(data.get_scene_file_version(1, first_path) == first_version);
		CHECK(data.get_scene_file_version(0, second_path) == "missing");
		data.set_edited_scene(1);
		const String accepted_reload_version = EditorData::get_scene_disk_version(first_path);
		data.set_scene_file_version(-1, first_path, accepted_reload_version);
		CHECK(EditorData::is_scene_disk_version_current(data.get_scene_file_version(1, first_path), first_path));
		CHECK(data.get_scene_file_version(0, second_path) == "missing");

		const String save_as_path = directory.path_join("save_as.tscn");
		data.set_scene_path(1, save_as_path);
		CHECK(data.get_scene_file_version(1, save_as_path).is_empty());
		const int inherited = data.add_edited_scene(-1);
		Node *inherited_root = memnew(Node);
		data.set_scene_root(inherited, inherited_root);
		CHECK(data.get_scene_path(inherited).is_empty());
		CHECK(data.get_scene_file_version(inherited, first_path).is_empty());

		// Avoid EditorNode signals: this native fixture owns its detached roots explicitly.
		data.set_scene_root(0, nullptr);
		data.set_scene_root(1, nullptr);
		data.set_scene_root(inherited, nullptr);
		memdelete(second_root);
		memdelete(replacement);
		memdelete(inherited_root);
	}
	CHECK(EditorUndoRedoManager::get_singleton() == previous_manager);
	CHECK(DirAccess::remove_absolute(first_path) == OK);
}

} // namespace TestGodotMCPSceneSave

#endif // TOOLS_ENABLED
