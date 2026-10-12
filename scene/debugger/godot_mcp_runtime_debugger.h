/**************************************************************************/
/*  godot_mcp_runtime_debugger.h                                          */
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

#include "core/object/object.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"

class Node;
class Viewport;

class GodotMCPRuntimeDebugger : public Object {
	GDCLASS(GodotMCPRuntimeDebugger, Object);

	enum PendingKind {
		PENDING_WAIT_FRAMES,
		PENDING_ACTION_RELEASE,
		PENDING_CAPTURE,
	};

	struct PendingRequest {
		PendingKind kind = PENDING_WAIT_FRAMES;
		String request_id;
		String operation;
		Dictionary params;
		uint64_t target_frame = 0;
		bool physics = true;
		bool step_paused = false;
		int frames_remaining = 0;
		bool completion_scheduled = false;
		bool input_disabled_before_step = false;
		StringName action;
	};

	static GodotMCPRuntimeDebugger *singleton;
	String run_id;
	LocalVector<PendingRequest> pending_requests;
	ErrorHandlerList error_handler;
	bool error_handler_registered = false;

	static void _error_handler_callback(void *p_user_data, const char *p_function,
			const char *p_file, int p_line, const char *p_error,
			const char *p_message, bool p_editor_notify, ErrorHandlerType p_type);
	static Error _capture_message(void *p_user_data, const String &p_message, const Array &p_args, bool &r_captured);

	static bool _serialize_variant(const Variant &p_value, Variant &r_serialized, int p_depth = 0,
			int *r_items = nullptr, int *r_characters = nullptr);
	static bool _variant_to_vector2(const Variant &p_value, Vector2 &r_vector);
	static Dictionary _serialize_tree(Node *p_node, int p_depth, int p_max_depth, int p_max_nodes, int &r_count, bool &r_truncated);
	static bool _has_property(Object *p_object, const StringName &p_property);
	Dictionary _animation_snapshot(Node *p_node, const String &p_playback_path, const Array &p_parameter_names, int p_limit) const;
	bool _validate_animation_request(const String &p_request_id, const String &p_operation, const Dictionary &p_params,
			Node *&r_node, String &r_playback_path, Array &r_parameter_names, int &r_limit);

	Node *_resolve_node(const String &p_path) const;
	void _send_ready();
	void _send_response(const String &p_request_id, const String &p_operation, bool p_ok, const Variant &p_payload) const;
	void _send_error(const String &p_request_id, const String &p_operation, const String &p_code,
			const String &p_message, bool p_retryable = false, const Dictionary &p_details = Dictionary()) const;
	void _handle_request(const String &p_request_id, const String &p_requested_run_id,
			const String &p_operation, const Dictionary &p_params);

	void _handle_get_tree(const String &p_request_id, const Dictionary &p_params);
	void _handle_get_property(const String &p_request_id, const Dictionary &p_params);
	void _handle_set_property(const String &p_request_id, const Dictionary &p_params);
	void _handle_call(const String &p_request_id, const Dictionary &p_params);
	void _handle_animation_get(const String &p_request_id, const Dictionary &p_params);
	void _handle_animation_control(const String &p_request_id, const Dictionary &p_params);
	void _handle_input_action(const String &p_request_id, const Dictionary &p_params);
	void _handle_input_event(const String &p_request_id, const Dictionary &p_params);
	void _handle_wait_frames(const String &p_request_id, const Dictionary &p_params);
	void _handle_capture(const String &p_request_id, const Dictionary &p_params);
	void _capture_now(const String &p_request_id, const Dictionary &p_params);

	void _process_frame();
	void _physics_frame();
	void _complete_pending(bool p_physics);
	void _finish_frame_request(const String &p_request_id);

protected:
	static void _bind_methods();

public:
	static void initialize();
	static void deinitialize();
	static GodotMCPRuntimeDebugger *get_singleton() { return singleton; }

	GodotMCPRuntimeDebugger();
	~GodotMCPRuntimeDebugger();
};
