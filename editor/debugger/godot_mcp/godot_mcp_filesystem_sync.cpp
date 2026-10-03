/**************************************************************************/
/*  godot_mcp_filesystem_sync.cpp                                         */
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

#include "godot_mcp_filesystem_sync.h"

#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "editor/debugger/godot_mcp/godot_mcp_batch_result.h"
#include "editor/debugger/godot_mcp/godot_mcp_diagnostics_store.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/file_system/editor_file_system.h"

namespace {
constexpr int MAX_IMPORT_PATHS = 64;

static Dictionary _item_error(int p_code, const String &p_message, const String &p_session_id,
		const Dictionary &p_details = Dictionary()) {
	return GodotMCPError::make_data(p_code, p_message, "resource.import", p_session_id, String(), p_details);
}
} // namespace

Dictionary GodotMCPFilesystemSync::_status() const {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	Dictionary data;
	data["scan_id"] = active_scan_id;
	data["scanning"] = filesystem && filesystem->is_scanning();
	data["importing"] = filesystem && filesystem->is_importing();
	data["busy"] = filesystem && (filesystem->is_scanning() || filesystem->is_importing());
	data["complete"] = !bool(data["busy"]);
	data["progress"] = filesystem ? filesystem->get_scanning_progress() : 0.0;
	data["started_at_msec"] = (int64_t)scan_started_msec;
	data["timestamp_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	return data;
}

Dictionary GodotMCPFilesystemSync::scan(const Dictionary &p_params, int &r_error_code, String &r_error_message) {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: EditorFileSystem is not available.";
		return Dictionary();
	}

	if (filesystem->is_scanning() || filesystem->is_importing()) {
		Dictionary data = _status();
		data["already_running"] = true;
		return data;
	}

	scan_sequence++;
	active_scan_id = "scan-" + uitos(scan_sequence) + "-" + uitos(OS::get_singleton()->get_ticks_usec());
	scan_started_msec = OS::get_singleton()->get_ticks_msec();
	if (bool(p_params.get("full", false))) {
		filesystem->scan();
	} else {
		filesystem->scan_changes();
	}

	Dictionary data = _status();
	data["full"] = bool(p_params.get("full", false));
	data["already_running"] = false;
	return data;
}

Dictionary GodotMCPFilesystemSync::wait_status(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const String requested_scan_id = p_params.get("scan_id", String());
	if (!requested_scan_id.is_empty() && requested_scan_id != active_scan_id) {
		r_error_code = GodotMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: scan_id is not the current filesystem operation.";
		return Dictionary();
	}
	return _status();
}

Dictionary GodotMCPFilesystemSync::import_resources(const Dictionary &p_params,
		GodotMCPDiagnosticsStore &p_diagnostics, int &r_error_code, String &r_error_message,
		Dictionary *r_error_details) {
	if (r_error_details) {
		r_error_details->clear();
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	if (!filesystem || !format_importer) {
		r_error_code = GodotMCPError::INTERNAL;
		r_error_message = "INTERNAL: editor filesystem/importer is not available.";
		return Dictionary();
	}
	if (filesystem->is_scanning() || filesystem->is_importing()) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: editor filesystem is busy; call filesystem.wait before importing.";
		return Dictionary();
	}

	Array paths;
	if (p_params.has("paths")) {
		if (p_params["paths"].get_type() != Variant::ARRAY) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: paths must be an array.";
			return Dictionary();
		}
		paths = p_params["paths"];
	} else if (p_params.has("path")) {
		paths.push_back(p_params["path"]);
	}
	if (paths.is_empty() || paths.size() > MAX_IMPORT_PATHS) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: provide between 1 and 64 resource paths.";
		return Dictionary();
	}

	const bool atomic = bool(p_params.get("atomic", false));
	Vector<String> valid_paths;
	Vector<String> import_paths;
	GodotMCPBatchResult batch;
	for (int i = 0; i < paths.size(); i++) {
		const Variant path_variant = paths[i];
		if (path_variant.get_type() != Variant::STRING) {
			if (atomic) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: every resource path must be a string.";
				return Dictionary();
			}
			batch.add_error(i, _item_error(GodotMCPError::INVALID_ARGUMENT, "Resource path must be a string.", session_id), "index");
			continue;
		}
		const String path = path_variant;
		if (!path.begins_with("res://") || !FileAccess::exists(path)) {
			const int code = path.begins_with("res://") ? GodotMCPError::NOT_FOUND : GodotMCPError::INVALID_ARGUMENT;
			const String message = path.begins_with("res://") ? "Resource does not exist on disk." : "Resource path must start with res://.";
			if (atomic) {
				r_error_code = code;
				r_error_message = GodotMCPError::code_name(code) + ": " + message;
				return Dictionary();
			}
			batch.add_error(path, _item_error(code, message, session_id), "path");
			continue;
		}
		valid_paths.push_back(path);
		if (format_importer->get_importer_by_file(path).is_valid()) {
			import_paths.push_back(path);
		}
	}

	const uint64_t diagnostic_sequence = p_diagnostics.get_latest_sequence();
	if (!valid_paths.is_empty()) {
		filesystem->update_files(valid_paths);
	}
	if (!import_paths.is_empty()) {
		filesystem->reimport_files(import_paths);
	}

	for (const String &path : valid_paths) {
		const Ref<ResourceImporter> importer = format_importer->get_importer_by_file(path);
		Error load_error = OK;
		Ref<Resource> loaded = ResourceLoader::load(path, String(), ResourceLoader::CACHE_MODE_IGNORE_DEEP, &load_error);
		Dictionary value;
		value["path"] = path;
		value["importer"] = importer.is_valid() ? importer->get_importer_name() : String();
		value["imported"] = importer.is_null() || format_importer->is_imported(path);
		value["import_valid"] = importer.is_null() || format_importer->is_import_valid(path);
		value["loadable"] = loaded.is_valid();
		value["load_error"] = (int)load_error;
		value["resource_type"] = filesystem->get_file_type(path);
		const ResourceUID::ID uid = filesystem->get_file_uid(path);
		value["resource_uid"] = uid == ResourceUID::INVALID_ID ? String() : ResourceUID::get_singleton()->id_to_text(uid);
		if (loaded.is_null()) {
			Dictionary details;
			details["path"] = path;
			details["godot_error"] = (int)load_error;
			if (atomic) {
				if (r_error_details) {
					*r_error_details = details;
				}
				r_error_code = GodotMCPError::INTERNAL;
				r_error_message = "INTERNAL: resource is not loadable after filesystem synchronization.";
				return Dictionary();
			}
			batch.add_error(path, _item_error(GodotMCPError::INTERNAL, "Resource is not loadable after filesystem synchronization.", session_id, details), "path");
		} else {
			batch.add_success(path, value, "path");
		}
	}

	Dictionary diagnostic_params;
	diagnostic_params["after_sequence"] = (int64_t)diagnostic_sequence;
	diagnostic_params["source"] = "editor";
	diagnostic_params["limit"] = 100;
	int diagnostic_error = 0;
	String diagnostic_message;
	Dictionary diagnostics = p_diagnostics.query(diagnostic_params, diagnostic_error, diagnostic_message);

	Dictionary data = batch.to_data();
	data["atomic"] = atomic;
	data["diagnostics"] = diagnostics.get("messages", Array());
	data["filesystem"] = _status();
	return data;
}
