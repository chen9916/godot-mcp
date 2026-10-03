/**************************************************************************/
/*  test_godot_mcp_primitives.cpp                                         */
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

#include "editor/debugger/godot_mcp/godot_mcp_batch_result.h"
#include "editor/debugger/godot_mcp/godot_mcp_diagnostics_store.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/debugger/godot_mcp/godot_mcp_filesystem_sync.h"
#include "editor/debugger/godot_mcp/godot_mcp_resource_versioning.h"
#include "editor/debugger/godot_mcp/godot_mcp_runtime_controller.h"
#include "editor/debugger/godot_mcp/godot_mcp_script_validation.h"
#include "editor/debugger/godot_mcp/godot_mcp_viewport_capture.h"
#include "tests/test_macros.h"

#include "modules/modules_enabled.gen.h"

TEST_FORCE_LINK(test_godot_mcp_primitives)

namespace TestGodotMCP {

TEST_CASE("[GodotMCP] Structured errors retain stable lifecycle fields") {
	Dictionary details;
	details["expected_version"] = "old";
	details["actual_version"] = "new";
	const Dictionary data = GodotMCPError::make_data(
			GodotMCPError::CONFLICT,
			"CONFLICT: resource changed on disk.",
			"resource.reload", "session-a", "run-a", details);

	CHECK(data["code"] == "CONFLICT");
	CHECK(data["message"] == "resource changed on disk.");
	CHECK(data["operation"] == "resource.reload");
	CHECK(data["session_id"] == "session-a");
	CHECK(data["run_id"] == "run-a");
	CHECK_FALSE(data["retryable"]);
	CHECK(Dictionary(data["conflict"])["actual_version"] == "new");
	CHECK(GodotMCPError::is_retryable(GodotMCPError::TIMEOUT));

	const Dictionary default_conflict = GodotMCPError::make_data(
			GodotMCPError::CONFLICT, "CONFLICT: unsaved editor state.", "resource.reload");
	CHECK(Dictionary(default_conflict["conflict"])["reason"] == "unsaved editor state.");

	Dictionary underlying_details;
	underlying_details["godot_error"] = 7;
	const Dictionary underlying = GodotMCPError::make_data(
			GodotMCPError::INTERNAL, "INTERNAL: importer failed.", "resource.import",
			String(), String(), underlying_details);
	CHECK(int(underlying["godot_error"]) == 7);
}

TEST_CASE("[GodotMCP] Batch results preserve successes beside item failures") {
	GodotMCPBatchResult batch;
	batch.add_success("existing/key", 123);
	batch.add_error("missing/key", GodotMCPError::make_data(GodotMCPError::NOT_FOUND, "Setting does not exist.", "project.get_setting"));

	const Dictionary data = batch.to_data();
	const Array items = data["items"];
	REQUIRE(items.size() == 2);
	CHECK(int(data["success_count"]) == 1);
	CHECK(int(data["failure_count"]) == 1);
	CHECK(int(Dictionary(items[0])["value"]) == 123);
	CHECK_FALSE(bool(Dictionary(items[1])["ok"]));
	CHECK(String(Dictionary(Dictionary(items[1])["error"])["code"]) == "NOT_FOUND");
}

TEST_CASE("[GodotMCP] Diagnostics are isolated by run and sequence") {
	GodotMCPDiagnosticsStore store;
	store.set_session_id("session-a");
	store.add_editor("warning", "editor warning", "res://tool.gd", 2);
	const uint64_t editor_sequence = store.get_latest_sequence();
	store.add_runtime("run-a", "error", "first run error", "res://main.gd", 5);
	store.add_runtime("run-b", "error", "second run error", "res://main.gd", 9);

	Dictionary query;
	query["run_id"] = "run-a";
	query["after_sequence"] = (int64_t)editor_sequence;
	query["limit"] = 10;
	int error_code = 0;
	String error_message;
	const Dictionary result = store.query(query, error_code, error_message);
	const Array messages = result["messages"];

	CHECK(error_code == 0);
	REQUIRE(messages.size() == 1);
	CHECK(Dictionary(messages[0])["run_id"] == "run-a");
	CHECK(Dictionary(messages[0])["message"] == "first run error");
	CHECK(Dictionary(messages[0])["session_id"] == "session-a");
}

TEST_CASE("[GodotMCP] Diagnostics reject ambiguous filters") {
	GodotMCPDiagnosticsStore store;
	Dictionary query;
	query["source"] = "game";
	int error_code = 0;
	String error_message;
	CHECK(store.query(query, error_code, error_message).is_empty());
	CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);

	error_code = 0;
	query.clear();
	Array severities;
	severities.push_back("fatal");
	query["severities"] = severities;
	CHECK(store.query(query, error_code, error_message).is_empty());
	CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
}

TEST_CASE("[GodotMCP] Diagnostics bound individual messages and stack traces") {
	GodotMCPDiagnosticsStore store;
	Array stack_trace;
	for (int i = 0; i < 70; i++) {
		stack_trace.push_back(String("frame-") + String::num_int64(i));
	}
	store.add_editor("error", String("x").repeat(17000), String("res://") + String("f").repeat(2100),
			1, 2, String("g").repeat(2100), stack_trace);

	int error_code = 0;
	String error_message;
	const Dictionary result = store.query(Dictionary(), error_code, error_message);
	const Array messages = result["messages"];
	REQUIRE(messages.size() == 1);
	const Dictionary message = messages[0];
	CHECK(error_code == 0);
	CHECK(String(message["message"]).length() == 16384);
	CHECK(String(message["file"]).length() == 2048);
	CHECK(String(message["function"]).length() == 2048);
	CHECK(Array(message["stack_trace"]).size() == 64);
	CHECK(bool(message["truncated"]));
}

#ifdef MODULE_GDSCRIPT_ENABLED
TEST_CASE("[GodotMCP] Script validation accepts unsaved source without loading a scene") {
	GodotMCPScriptValidation validator;
	Dictionary params;
	params["script_path"] = "res://mcp_validation_test.gd";
	params["source"] = "extends Node\nvar answer: int = 42\n";
	int error_code = 0;
	String error_message;
	const Dictionary valid = validator.validate(params, error_code, error_message);
	CHECK(error_code == 0);
	CHECK(bool(valid["parse_success"]));
	CHECK(String(valid["source_origin"]) == "editor_buffer");

	params["source"] = "extends Node\nfunc broken(\n";
	error_code = 0;
	const Dictionary invalid = validator.validate(params, error_code, error_message);
	CHECK(error_code == 0);
	CHECK_FALSE(bool(invalid["parse_success"]));
	CHECK(Array(invalid["diagnostics"]).size() > 0);
}
#endif

TEST_CASE("[GodotMCP] Focused modules fail safely before editor state is available") {
	int error_code = 0;
	String error_message;
	GodotMCPResourceVersioning versioning;
	Dictionary status_params;
	status_params["path"] = "outside-project.svg";
	CHECK(versioning.get_status(status_params, error_code, error_message).is_empty());
	CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);

	GodotMCPViewportCapture capture;
	error_code = 0;
	CHECK(capture.capture(Dictionary(), error_code, error_message).is_empty());
	CHECK(error_code == GodotMCPError::INTERNAL);

	GodotMCPRuntimeController runtime;
	error_code = 0;
	const Dictionary runtime_status = runtime.status(Dictionary(), error_code, error_message);
	CHECK(error_code == 0);
	CHECK(String(runtime_status["state"]) == "stopped");

	GodotMCPFilesystemSync filesystem;
	error_code = 0;
	const Dictionary filesystem_status = filesystem.wait_status(Dictionary(), error_code, error_message);
	CHECK(error_code == 0);
	CHECK(filesystem_status.has("complete"));
}

TEST_CASE("[GodotMCP] Resource reload conflict preserves divergent editor buffers") {
	CHECK(GodotMCPResourceVersioning::has_editor_buffer_conflict(true, "disk-v2", "buffer-v1"));
	CHECK_FALSE(GodotMCPResourceVersioning::has_editor_buffer_conflict(true, "same", "same"));
	CHECK_FALSE(GodotMCPResourceVersioning::has_editor_buffer_conflict(false, "disk-v2", "buffer-v1"));
}

} // namespace TestGodotMCP

#endif // TOOLS_ENABLED
