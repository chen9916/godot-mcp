/**************************************************************************/
/*  opencode_mcp_script_validation.cpp                                    */
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

#include "opencode_mcp_script_validation.h"

#include "opencode_mcp_error.h"

#include "core/io/file_access.h"
#include "core/object/editor_language.h"
#include "core/object/script_language.h"
#include "core/templates/hash_set.h"

namespace {
static String _first_quoted_value(const String &p_message) {
	for (const String &delimiter : { String("\""), String("'") }) {
		const int start = p_message.find(delimiter);
		if (start < 0) {
			continue;
		}
		const int end = p_message.find(delimiter, start + 1);
		if (end > start + 1) {
			return p_message.substr(start + 1, end - start - 1);
		}
	}
	return String();
}

static String _unresolved_dependency_path(const String &p_message) {
	const String lowered = p_message.to_lower();
	if (!lowered.contains("preload") && !lowered.contains("resource") && !lowered.contains("dependency")) {
		return String();
	}
	const int start = p_message.find("res://");
	if (start < 0) {
		return String();
	}
	int end = start;
	while (end < p_message.length() && !String("\"' \t\r\n)").contains(String::chr(p_message[end]))) {
		end++;
	}
	return p_message.substr(start, end - start);
}
} // namespace

Dictionary OpenCodeMCPScriptValidation::validate(const Dictionary &p_params, int &r_error_code, String &r_error_message) const {
	const String script_path = p_params.get("script_path", String());
	if (!script_path.begins_with("res://") || script_path.contains("::")) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: script_path must be an external res:// script path.";
		return Dictionary();
	}

	const bool has_unsaved_source = p_params.has("source");
	String source;
	if (has_unsaved_source) {
		if (p_params["source"].get_type() != Variant::STRING) {
			r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: source must be a string.";
			return Dictionary();
		}
		source = p_params["source"];
	} else {
		Ref<FileAccess> file = FileAccess::open(script_path, FileAccess::READ);
		if (file.is_null()) {
			r_error_code = OpenCodeMCPError::NOT_FOUND;
			r_error_message = "NOT_FOUND: script_path could not be read from disk.";
			return Dictionary();
		}
		source = file->get_as_text();
	}

	const String version = source.md5_text();
	const String expected_version = p_params.get("expected_version", String());
	if (!expected_version.is_empty() && expected_version != version) {
		r_error_code = OpenCodeMCPError::CONFLICT;
		r_error_message = "CONFLICT: script source version differs from expected_version (expected " + expected_version +
				", actual " + version + ").";
		return Dictionary();
	}

	ScriptLanguage *language = ScriptServer::get_language_for_extension(script_path.get_extension().to_lower());
	if (!language) {
		r_error_code = OpenCodeMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: no script language is registered for extension '" + script_path.get_extension() + "'.";
		return Dictionary();
	}

	EditorLanguage *editor_language = language->get_editor_language();
	if (!editor_language) {
		r_error_code = OpenCodeMCPError::INTERNAL;
		r_error_message = "INTERNAL: script language does not provide editor validation.";
		return Dictionary();
	}

	List<EditorLanguage::ScriptError> errors;
	List<EditorLanguage::Warning> warnings;
	const bool parse_success = editor_language->validate(source, script_path, &errors, &warnings, nullptr, nullptr);
	const bool validate_dependencies = bool(p_params.get("validate_dependencies", true));

	Array diagnostics;
	Array unresolved_dependencies;
	Array unresolved_global_classes;
	HashSet<String> unresolved_paths;
	HashSet<String> unresolved_class_names;
	for (const EditorLanguage::ScriptError &error : errors) {
		const String diagnostic_path = error.path.is_empty() ? script_path : error.path;
		const bool dependency_error = diagnostic_path != script_path;
		if (dependency_error && !unresolved_paths.has(diagnostic_path)) {
			Dictionary dependency;
			dependency["path"] = diagnostic_path;
			dependency["message"] = error.message;
			unresolved_dependencies.push_back(dependency);
			unresolved_paths.insert(diagnostic_path);
		}
		const String unresolved_dependency = _unresolved_dependency_path(error.message);
		if (!unresolved_dependency.is_empty() && !unresolved_paths.has(unresolved_dependency)) {
			Dictionary dependency;
			dependency["path"] = unresolved_dependency;
			dependency["message"] = error.message;
			unresolved_dependencies.push_back(dependency);
			unresolved_paths.insert(unresolved_dependency);
		}
		const String lowered_message = error.message.to_lower();
		if (lowered_message.contains("global class") || lowered_message.contains("could not find type")) {
			const String class_name = _first_quoted_value(error.message);
			if (!class_name.is_empty() && !unresolved_class_names.has(class_name)) {
				Dictionary global_class;
				global_class["name"] = class_name;
				global_class["message"] = error.message;
				global_class["file"] = diagnostic_path;
				global_class["line"] = error.start_line;
				global_class["column"] = error.start_column;
				unresolved_global_classes.push_back(global_class);
				unresolved_class_names.insert(class_name);
			}
		}
		if (dependency_error && !validate_dependencies) {
			continue;
		}

		Dictionary diagnostic;
		diagnostic["severity"] = "error";
		diagnostic["message"] = error.message;
		diagnostic["file"] = diagnostic_path;
		diagnostic["line"] = error.start_line;
		diagnostic["column"] = error.start_column;
		diagnostic["end_line"] = error.end_line;
		diagnostic["end_column"] = error.end_column;
		diagnostics.push_back(diagnostic);
	}

	for (const EditorLanguage::Warning &warning : warnings) {
		Dictionary diagnostic;
		diagnostic["severity"] = "warning";
		diagnostic["code"] = warning.string_code;
		diagnostic["message"] = warning.message;
		diagnostic["file"] = script_path;
		diagnostic["line"] = warning.start_line;
		diagnostic["column"] = warning.start_column;
		diagnostic["end_line"] = warning.end_line;
		diagnostic["end_column"] = warning.end_column;
		diagnostics.push_back(diagnostic);
	}

	Dictionary data;
	data["parse_success"] = parse_success;
	data["diagnostics"] = diagnostics;
	data["script_path"] = script_path;
	data["script_version"] = version;
	data["source_origin"] = has_unsaved_source ? "editor_buffer" : "disk";
	data["dependency_validation"] = validate_dependencies;
	data["unresolved_dependencies"] = unresolved_dependencies;
	data["unresolved_global_classes"] = unresolved_global_classes;
	data["language"] = language->get_name();
	return data;
}
