/**************************************************************************/
/*  opencode_mcp_resource_versioning.cpp                                  */
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

#include "opencode_mcp_resource_versioning.h"

#include "opencode_mcp_error.h"

#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/script_language.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/script/script_editor_base.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/node.h"

namespace {
constexpr int MAX_SCENE_REFERENCE_NODES = 4000;
constexpr int MAX_SCENE_REFERENCES = 128;

static String _dependency_path(const String &p_dependency) {
	const int separators = p_dependency.get_slice_count("::");
	return separators > 1 ? p_dependency.get_slice("::", separators - 1) : p_dependency;
}
} // namespace

String OpenCodeMCPResourceVersioning::_get_editor_buffer_version(const Ref<Resource> &p_resource,
		const String &p_path, bool &r_has_buffer, bool &r_unsaved) {
	r_has_buffer = false;
	r_unsaved = false;
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (!script_editor || p_resource.is_null()) {
		return String();
	}

	ScriptEditorBase *editor = nullptr;
	Ref<Script> script = p_resource;
	if (script.is_valid()) {
		editor = script_editor->find_open_script_editor(script, p_path);
	} else {
		editor = script_editor->get_resource_editor(p_resource);
	}
	if (!editor) {
		return String();
	}

	TextEditorBase *text_editor = Object::cast_to<TextEditorBase>(editor);
	if (!text_editor || !text_editor->get_code_editor() || !text_editor->get_code_editor()->get_text_editor()) {
		return String();
	}

	r_has_buffer = true;
	r_unsaved = editor->is_unsaved();
	return text_editor->get_code_editor()->get_text_editor()->get_text().md5_text();
}

void OpenCodeMCPResourceVersioning::_collect_scene_references(Node *p_node, Node *p_root, const String &p_path,
		int p_max_nodes, int &r_nodes_seen, Array &r_references) {
	if (!p_node || r_nodes_seen >= p_max_nodes || r_references.size() >= MAX_SCENE_REFERENCES) {
		return;
	}
	r_nodes_seen++;

	bool references_path = false;
	Ref<Script> script = p_node->get_script();
	if (script.is_valid() && script->get_path() == p_path) {
		references_path = true;
	}
	if (!references_path) {
		List<PropertyInfo> properties;
		p_node->get_property_list(&properties);
		for (const PropertyInfo &property : properties) {
			if (!(property.usage & PROPERTY_USAGE_STORAGE) || property.type != Variant::OBJECT) {
				continue;
			}
			Ref<Resource> resource = p_node->get(property.name);
			if (resource.is_valid() && resource->get_path() == p_path) {
				references_path = true;
				break;
			}
		}
	}

	if (references_path) {
		Dictionary reference;
		reference["scene_path"] = p_root->get_scene_file_path();
		reference["node_path"] = p_node == p_root ? String(".") : String(p_root->get_path_to(p_node));
		r_references.push_back(reference);
	}

	for (int i = 0; i < p_node->get_child_count(false); i++) {
		_collect_scene_references(p_node->get_child(i, false), p_root, p_path, p_max_nodes, r_nodes_seen, r_references);
	}
}

bool OpenCodeMCPResourceVersioning::has_editor_buffer_conflict(
		bool p_has_buffer, const String &p_disk_version, const String &p_buffer_version) {
	return p_has_buffer && p_buffer_version != p_disk_version;
}

Dictionary OpenCodeMCPResourceVersioning::get_status(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const String path = p_params.get("path", String());
	if (!path.begins_with("res://") || path.contains("::")) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: path must be an external res:// resource path.";
		return Dictionary();
	}
	if (!FileAccess::exists(path)) {
		r_error_code = OpenCodeMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: resource does not exist on disk.";
		return Dictionary();
	}

	const String disk_version = FileAccess::get_md5(path);
	const uint64_t disk_modified_time = FileAccess::get_modified_time(path);
	Ref<Resource> loaded = ResourceCache::get_ref(path);
	if (loaded.is_null() && bool(p_params.get("load", false))) {
		loaded = ResourceLoader::load(path);
	}
	const bool is_loaded = loaded.is_valid();
	const bool loaded_resource_edited = loaded.is_valid() && loaded->is_edited();

	String loaded_version;
	Ref<Script> loaded_script = loaded;
	if (loaded_script.is_valid()) {
		loaded_version = loaded_script->get_source_code().md5_text();
	} else if (loaded.is_valid() && loaded->get_last_modified_time() > 0) {
		loaded_version = uitos(loaded->get_last_modified_time());
	}

	bool has_editor_buffer = false;
	bool editor_buffer_unsaved = false;
	const String editor_buffer_version = _get_editor_buffer_version(loaded, path, has_editor_buffer, editor_buffer_unsaved);

	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	int file_index = -1;
	EditorFileSystemDirectory *directory = filesystem ? filesystem->find_file(path, &file_index) : nullptr;
	ResourceUID::ID uid = filesystem ? filesystem->get_file_uid(path) : ResourceLoader::get_resource_uid(path);

	Dictionary importer_status;
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	Ref<ResourceImporter> importer = format_importer ? format_importer->get_importer_by_file(path) : Ref<ResourceImporter>();
	importer_status["required"] = importer.is_valid();
	importer_status["name"] = importer.is_valid() ? importer->get_importer_name() : String();
	importer_status["imported"] = importer.is_null() || (format_importer && format_importer->is_imported(path));
	importer_status["valid"] = importer.is_null() || (directory && file_index >= 0 && directory->get_file_import_is_valid(file_index));
	importer_status["modified_time"] = directory && file_index >= 0 ? (int64_t)directory->get_file_import_modified_time(file_index) : 0;

	Array dependencies;
	List<String> dependency_paths;
	ResourceLoader::get_dependencies(path, &dependency_paths, false);
	bool dependencies_ok = true;
	for (const String &dependency_value : dependency_paths) {
		const String dependency_path = _dependency_path(dependency_value);
		const bool exists = ResourceLoader::exists(dependency_path) || FileAccess::exists(dependency_path);
		Dictionary dependency;
		dependency["path"] = dependency_path;
		dependency["exists"] = exists;
		dependencies.push_back(dependency);
		dependencies_ok = dependencies_ok && exists;
	}

	Array scene_references;
	EditorData &editor_data = EditorNode::get_editor_data();
	int nodes_seen = 0;
	for (int i = 0; i < editor_data.get_edited_scene_count() && nodes_seen < MAX_SCENE_REFERENCE_NODES; i++) {
		Node *root = editor_data.get_edited_scene_root(i);
		_collect_scene_references(root, root, path, MAX_SCENE_REFERENCE_NODES, nodes_seen, scene_references);
	}

	String stale_state = "fresh";
	if (has_editor_buffer_conflict(has_editor_buffer, disk_version, editor_buffer_version)) {
		stale_state = "editor_buffer_conflict";
	} else if (loaded_resource_edited) {
		stale_state = "loaded_resource_unsaved";
	} else if (loaded_script.is_valid() && loaded_version != disk_version) {
		stale_state = "loaded_resource_stale";
	} else if (importer.is_valid() && (!bool(importer_status["imported"]) || !bool(importer_status["valid"]))) {
		stale_state = "import_stale";
	} else if (!dependencies_ok) {
		stale_state = "dependency_missing";
	}

	Dictionary data;
	data["path"] = path;
	data["resource_uid"] = uid == ResourceUID::INVALID_ID ? String() : ResourceUID::get_singleton()->id_to_text(uid);
	data["disk_modified_time"] = (int64_t)disk_modified_time;
	data["disk_version"] = disk_version;
	data["editor_buffer_version"] = editor_buffer_version;
	data["editor_buffer_open"] = has_editor_buffer;
	data["editor_buffer_unsaved"] = editor_buffer_unsaved;
	data["loaded_resource_version"] = loaded_version;
	data["loaded"] = is_loaded;
	data["loaded_resource_edited"] = loaded_resource_edited;
	data["stale"] = stale_state != "fresh";
	data["stale_state"] = stale_state;
	data["dependencies_ok"] = dependencies_ok;
	data["dependencies"] = dependencies;
	data["importer_status"] = importer_status;
	data["active_scene_references"] = scene_references;
	data["scene_reference_scan_truncated"] = nodes_seen >= MAX_SCENE_REFERENCE_NODES || scene_references.size() >= MAX_SCENE_REFERENCES;
	return data;
}

Dictionary OpenCodeMCPResourceVersioning::reload(const Dictionary &p_params, int &r_error_code, String &r_error_message,
		Dictionary *r_error_details) const {
	if (r_error_details) {
		r_error_details->clear();
	}
	Dictionary before = get_status(p_params, r_error_code, r_error_message);
	if (r_error_code != 0) {
		return Dictionary();
	}

	const String path = before["path"];
	const String expected_version = p_params.get("expected_version", String());
	if (!expected_version.is_empty() && expected_version != String(before["disk_version"])) {
		if (r_error_details) {
			(*r_error_details)["path"] = path;
			(*r_error_details)["resource_uid"] = before["resource_uid"];
			(*r_error_details)["expected_version"] = expected_version;
			(*r_error_details)["disk_version"] = before["disk_version"];
		}
		r_error_code = OpenCodeMCPError::CONFLICT;
		r_error_message = "CONFLICT: disk version differs from expected_version (expected " + expected_version +
				", actual " + String(before["disk_version"]) + ").";
		return Dictionary();
	}
	if (has_editor_buffer_conflict(
			bool(before["editor_buffer_open"]), String(before["disk_version"]), String(before["editor_buffer_version"]))) {
		if (r_error_details) {
			(*r_error_details)["path"] = path;
			(*r_error_details)["resource_uid"] = before["resource_uid"];
			(*r_error_details)["disk_version"] = before["disk_version"];
			(*r_error_details)["editor_buffer_version"] = before["editor_buffer_version"];
			(*r_error_details)["loaded_resource_version"] = before["loaded_resource_version"];
		}
		r_error_code = OpenCodeMCPError::CONFLICT;
		r_error_message = "CONFLICT: an open editor buffer differs from disk; reload refused to preserve editor changes. UID=" +
				String(before["resource_uid"]) +
				", disk=" + String(before["disk_version"]) + ", buffer=" + String(before["editor_buffer_version"]) +
				", loaded=" + String(before["loaded_resource_version"]);
		return Dictionary();
	}
	if (bool(before["loaded_resource_edited"])) {
		if (r_error_details) {
			(*r_error_details)["path"] = path;
			(*r_error_details)["resource_uid"] = before["resource_uid"];
			(*r_error_details)["disk_version"] = before["disk_version"];
			(*r_error_details)["editor_buffer_version"] = before["editor_buffer_version"];
			(*r_error_details)["loaded_resource_version"] = before["loaded_resource_version"];
			(*r_error_details)["loaded_resource_edited"] = true;
		}
		r_error_code = OpenCodeMCPError::CONFLICT;
		r_error_message = "CONFLICT: the loaded resource has unsaved editor changes; reload refused. UID=" + String(before["resource_uid"]) +
				", disk=" + String(before["disk_version"]) + ", buffer=" + String(before["editor_buffer_version"]) +
				", loaded=" + String(before["loaded_resource_version"]);
		return Dictionary();
	}

	Ref<Resource> resource = ResourceCache::get_ref(path);
	if (resource.is_null()) {
		resource = ResourceLoader::load(path);
	}
	if (resource.is_null()) {
		r_error_code = OpenCodeMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: resource could not be loaded for reload.";
		return Dictionary();
	}

	bool script_specific = false;
	bool existing_instances_updated = false;
	bool runtime_reload_requested = false;
	const int existing_scene_instance_count = Array(before["active_scene_references"]).size();
	Ref<Script> script = resource;
	if (script.is_valid()) {
		script_specific = true;
		script->reload_from_file();
		script->update_exports();
		existing_instances_updated = existing_scene_instance_count > 0;
		if (ScriptEditor::get_singleton()) {
			ScriptEditor::get_singleton()->trigger_live_script_reload(path);
			runtime_reload_requested = true;
		}
	} else if (resource->editor_can_reload_from_file()) {
		resource->reload_from_file();
		existing_instances_updated = existing_scene_instance_count > 0;
	} else {
		Error load_error = OK;
		resource = ResourceLoader::load(path, String(), ResourceLoader::CACHE_MODE_REPLACE_DEEP, &load_error);
		if (resource.is_null()) {
			if (r_error_details) {
				(*r_error_details)["path"] = path;
				(*r_error_details)["godot_error"] = (int)load_error;
			}
			r_error_code = OpenCodeMCPError::INTERNAL;
			r_error_message = "INTERNAL: replacement resource load failed with Godot error " + itos(load_error) + ".";
			return Dictionary();
		}
	}

	resource->set_edited(false);
	resource->set_last_modified_time(FileAccess::get_modified_time(path));
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}
	if (ScriptEditor::get_singleton()) {
		ScriptEditor::get_singleton()->update_script_times();
	}

	Dictionary after = get_status(p_params, r_error_code, r_error_message);
	if (r_error_code != 0) {
		return Dictionary();
	}
	const bool reload_verified = !script_specific || String(after["loaded_resource_version"]) == String(after["disk_version"]);
	if (!reload_verified) {
		if (r_error_details) {
			(*r_error_details)["path"] = path;
			(*r_error_details)["disk_version"] = after["disk_version"];
			(*r_error_details)["loaded_resource_version"] = after["loaded_resource_version"];
		}
		r_error_code = OpenCodeMCPError::INTERNAL;
		r_error_message = "INTERNAL: script reload completed without an error result, but the loaded source version "
				"still differs from disk (disk=" +
				String(after["disk_version"]) + ", loaded=" + String(after["loaded_resource_version"]) + ").";
		return Dictionary();
	}
	Dictionary data;
	data["path"] = path;
	data["before"] = before;
	data["after"] = after;
	data["script_specific_reload"] = script_specific;
	data["reload_verified"] = reload_verified;
	data["existing_scene_instances_updated"] = existing_instances_updated;
	data["existing_scene_instance_count"] = existing_scene_instance_count;
	data["runtime_reload_requested"] = runtime_reload_requested;
	data["stale_editor_buffer"] = false;
	return data;
}
