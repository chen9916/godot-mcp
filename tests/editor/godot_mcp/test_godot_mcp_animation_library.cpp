/**************************************************************************/
/*  test_godot_mcp_animation_library.cpp                                  */
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

#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/undo_redo.h"
#include "editor/debugger/godot_mcp/godot_mcp_animation_library.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "scene/animation/animation_player.h"
#include "scene/animation/animation_tree.h"
#include "scene/resources/audio/audio_stream_wav.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include <limits>

TEST_FORCE_LINK(test_godot_mcp_animation_library)

namespace TestGodotMCPAnimationLibrary {

struct AnimationErrorCounter {
	int count = 0;
	ErrorHandlerList handler;

	static void _count(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_explanation, bool p_editor_notify, ErrorHandlerType p_type) {
		if (p_type != ERR_HANDLER_WARNING) {
			static_cast<AnimationErrorCounter *>(p_self)->count++;
		}
	}

	AnimationErrorCounter() {
		handler.errfunc = &_count;
		handler.userdata = this;
		add_error_handler(&handler);
	}
	~AnimationErrorCounter() {
		remove_error_handler(&handler);
	}
};

struct AnimationHarness {
	Node *root = memnew(Node);
	AnimationPlayer *player = memnew(AnimationPlayer);
	UndoRedo *undo_redo = memnew(UndoRedo);
	String scene_path = "res://mcp_animation_test.tscn";
	int history_id = 61;
	int error_code = 0;
	String error_message;
	Array warnings;
	GodotMCPAnimationLibrary tools;

	AnimationHarness() {
		root->set_name("Root");
		root->set_scene_file_path(scene_path);
		player->set_name("Player");
		root->add_child(player);
		player->set_owner(root);
		Ref<AnimationLibrary> library;
		library.instantiate();
		player->add_animation_library(StringName(), library);
	}

	~AnimationHarness() {
		memdelete(undo_redo);
		memdelete(root);
	}

	String version() const {
		return GodotMCPScenePatch::compute_scene_version(root, history_id, undo_redo->get_version());
	}

	GodotMCPScenePatch::Context context() const {
		GodotMCPScenePatch::Context context;
		context.root = root;
		context.scene_path = scene_path;
		context.scene_version = version();
		context.history_id = history_id;
		context.undo_redo = undo_redo;
		return context;
	}

	Dictionary params(const Array &p_operations, bool p_dry_run = false) const {
		Dictionary params;
		params["expected_scene_path"] = scene_path;
		params["expected_scene_version"] = version();
		params["node_path"] = "Player";
		params["operations"] = p_operations;
		params["dry_run"] = p_dry_run;
		return params;
	}

	Dictionary apply(const Array &p_operations, bool p_dry_run = false) {
		warnings.clear();
		return tools.apply(params(p_operations, p_dry_run), context(), warnings, error_code, error_message);
	}

	Dictionary apply_json(const String &p_operations, bool p_dry_run = false) {
		return apply(JSON::parse_string(p_operations), p_dry_run);
	}

	Dictionary get(const String &p_clip, bool p_keys = true, int p_limit = 1000) {
		Dictionary params;
		params["node_path"] = "Player";
		params["animation"] = p_clip;
		params["include_keys"] = p_keys;
		params["key_limit"] = p_limit;
		return tools.get(params, context(), error_code, error_message);
	}

	Ref<Animation> animation(const String &p_clip) const {
		return player->get_animation(p_clip);
	}
};

static Array _tracks(const Dictionary &p_data) {
	return p_data["tracks"];
}

static Array _keys(const Dictionary &p_data, int p_track) {
	return Dictionary(_tracks(p_data)[p_track])["keys"];
}

static Dictionary _stored_properties(Object *p_object) {
	Dictionary values;
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			// String keys do not retain transient property-name StringNames.
			// Compare every native authored field without depending on the
			// pointer ordering of regenerated virtual property descriptors.
			values[String(property.name)] = p_object->get(property.name);
		}
	}
	return values;
}

struct AnimationLibraryUpdateObserver : RefCounted {
	int updates = 0;
	AnimationPlayer *preview = memnew(AnimationPlayer);
	String autoplay;

	void updated(AnimationMixer *p_mixer) {
		updates++;
		LocalVector<StringName> names;
		preview->get_animation_library_list(&names);
		for (const StringName &name : names) {
			preview->remove_animation_library(name);
		}
		names.clear();
		p_mixer->get_animation_library_list(&names);
		for (const StringName &name : names) {
			preview->add_animation_library(name, p_mixer->get_animation_library(name));
		}
		if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_mixer)) {
			autoplay = player->get_autoplay();
		}
	}

	~AnimationLibraryUpdateObserver() {
		memdelete(preview);
	}
};

TEST_CASE("[GodotMCP][Animation] All track types use native insertion and typed key data") {
	AnimationHarness harness;
	const String before = harness.version();
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	const Dictionary result = harness.apply_json(R"([
		{"op":"create_animation","animation":"RESET","properties":{"length":2,"loop_mode":1,"step":0.1,"resource_name":"Reset Clip"}},
		{"op":"add_track","animation":"RESET","type":"value","path":".:visible","properties":{"value_update_mode":1}},
		{"op":"add_track","animation":"RESET","type":"position_3d","path":"."},
		{"op":"add_track","animation":"RESET","type":"rotation_3d","path":"."},
		{"op":"add_track","animation":"RESET","type":"scale_3d","path":"."},
		{"op":"add_track","animation":"RESET","type":"blend_shape","path":".:blend_shapes/smile"},
		{"op":"add_track","animation":"RESET","type":"method","path":"."},
		{"op":"add_track","animation":"RESET","type":"bezier","path":".:position:x"},
		{"op":"add_track","animation":"RESET","type":"audio","path":".","properties":{"audio_use_blend":false}},
		{"op":"add_track","animation":"RESET","type":"animation","path":"Player"},
		{"op":"insert_key","animation":"RESET","track":0,"time":0,"value":true,"transition":0.5},
		{"op":"insert_key","animation":"RESET","track":1,"time":0.5,"value":{"__type":"Vector3","value":[1,2,3]}},
		{"op":"insert_key","animation":"RESET","track":2,"time":0,"value":{"__type":"Quaternion","value":[0,0,0,1]}},
		{"op":"insert_key","animation":"RESET","track":3,"time":0,"value":{"__type":"Vector3i","value":[2,2,2]}},
		{"op":"insert_key","animation":"RESET","track":4,"time":0,"value":0.75},
		{"op":"insert_key","animation":"RESET","track":5,"time":0,"value":{"method":"example","args":[7,{"__type":"Vector2","value":[2,3]},{"__type":"NodePath","value":"Player"}]}},
		{"op":"insert_key","animation":"RESET","track":6,"time":0,"value":{"value":2,"in_handle":[-0.2,-1],"out_handle":[0.4,1]}},
		{"op":"insert_key","animation":"RESET","track":7,"time":0,"value":{"start_offset":0.1,"end_offset":0.2}},
		{"op":"insert_key","animation":"RESET","track":8,"time":0,"value":"RESET"}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE(harness.error_code == 0);
	CHECK(bool(result["applied"]));
	CHECK(int(result["operation_count"]) == 19);
	CHECK(harness.undo_redo->get_history_count() == 1);
	CHECK(harness.version() != before);
	CHECK(harness.player->get_animation_library(StringName()) != original);
	CHECK(original->get_animation_list_size() == 0);
	const Ref<Animation> animation = harness.animation("RESET");
	REQUIRE(animation.is_valid());
	CHECK(animation->get_track_count() == 9);
	CHECK(animation->get_length() == doctest::Approx(2));
	CHECK(animation->get_loop_mode() == Animation::LOOP_LINEAR);
	CHECK(animation->get_step() == doctest::Approx(0.1));
	CHECK(animation->get_name() == "Reset Clip");
	CHECK(animation->track_get_key_value(1, 0) == Variant(Vector3(1, 2, 3)));
	CHECK(animation->track_get_key_value(2, 0) == Variant(Quaternion()));
	CHECK(animation->track_get_key_value(3, 0) == Variant(Vector3(2, 2, 2)));
	CHECK(animation->method_track_get_name(5, 0) == StringName("example"));
	CHECK(animation->method_track_get_params(5, 0)[1] == Variant(Vector2(2, 3)));
	CHECK(animation->method_track_get_params(5, 0)[2] == Variant(NodePath("Player")));
	CHECK(animation->bezier_track_get_key_in_handle(6, 0) == Vector2(-0.2, -1));
	CHECK(animation->audio_track_get_key_stream(7, 0).is_null());
	CHECK_FALSE(animation->audio_track_is_use_blend(7));
	CHECK(animation->animation_track_get_key_animation(8, 0) == StringName("RESET"));

	const Dictionary data = harness.get("RESET");
	REQUIRE(harness.error_code == 0);
	CHECK(int(data["key_count"]) == 9);
	CHECK(int(data["returned_key_count"]) == 9);
	CHECK_FALSE(bool(data["truncated"]));
	CHECK(Dictionary(Dictionary(_keys(data, 1)[0])["value"])["__type"] == Variant("Vector3"));
	CHECK(Dictionary(Dictionary(_keys(data, 5)[0])["value"])["method"] == Variant("example"));
	CHECK(Dictionary(Dictionary(_keys(data, 6)[0])["value"])["value"] == Variant(2.0));
	CHECK(Dictionary(_keys(data, 8)[0])["value"] == Variant("RESET"));
	CHECK(JSON::parse_string(JSON::stringify(data)).get_type() == Variant::DICTIONARY);

	CHECK(harness.undo_redo->undo());
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.version() == before);
	CHECK(harness.undo_redo->redo());
	CHECK(harness.animation("RESET") == animation);
}

TEST_CASE("[GodotMCP][Animation] Key editing, replacement, deletion and final-position track reorder") {
	AnimationHarness harness;
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"create_animation","animation":"Walk"},
		{"op":"add_track","animation":"Walk","type":"value","path":".:a"},
		{"op":"add_track","animation":"Walk","type":"bezier","path":".:b"},
		{"op":"add_track","animation":"Walk","type":"position_3d","path":"."},
		{"op":"insert_key","animation":"Walk","track":0,"time":0,"value":1,"transition":0.1},
		{"op":"insert_key","animation":"Walk","track":0,"time":1,"value":2},
		{"op":"insert_key","animation":"Walk","track":0,"time":2,"value":3},
		{"op":"insert_key","animation":"Walk","track":1,"time":0,"value":[4,-0.2,0,0.2,0]}
	])")
					.is_empty());
	const Dictionary result = harness.apply_json(R"([
		{"op":"insert_key","animation":"Walk","track":0,"time":0,"value":10,"transition":0.7},
		{"op":"set_key","animation":"Walk","track":0,"key":0,"time":1.5,"value":20,"transition":0.4},
		{"op":"remove_key","animation":"Walk","track":0,"key":0},
		{"op":"set_bezier_handles","animation":"Walk","track":1,"key":0,"in_handle":[-0.5,-2],"out_handle":[0.5,2],"handle_mode":3},
		{"op":"set_track","animation":"Walk","track":0,"properties":{"path":".:changed","enabled":false,"imported":true,"interpolation_type":2,"interpolation_loop_wrap":false,"value_update_mode":2}},
		{"op":"move_track","animation":"Walk","track":0,"to_index":2},
		{"op":"add_track","animation":"Walk","type":"animation","path":"Player","at_index":1},
		{"op":"remove_track","animation":"Walk","track":2}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE(harness.error_code == 0);
	CHECK(int(Dictionary(Array(result["operations"])[1])["key"]) == 1);
	const Ref<Animation> animation = harness.animation("Walk");
	REQUIRE(animation->get_track_count() == 3);
	CHECK(animation->track_get_type(0) == Animation::TYPE_BEZIER);
	CHECK(animation->track_get_type(1) == Animation::TYPE_ANIMATION);
	CHECK(animation->track_get_type(2) == Animation::TYPE_VALUE);
	CHECK(animation->track_get_key_count(2) == 2);
	CHECK(animation->track_get_key_time(2, 0) == doctest::Approx(1.5));
	CHECK(animation->track_get_key_value(2, 0) == Variant(20.0));
	CHECK(animation->track_get_key_transition(2, 0) == doctest::Approx(0.4));
	CHECK(String(animation->track_get_path(2)) == ".:changed");
	CHECK_FALSE(animation->track_is_enabled(2));
	CHECK(animation->track_is_imported(2));
	CHECK(animation->track_get_interpolation_type(2) == Animation::INTERPOLATION_CUBIC);
	CHECK_FALSE(animation->track_get_interpolation_loop_wrap(2));
	CHECK(animation->value_track_get_update_mode(2) == Animation::UPDATE_CAPTURE);
	CHECK(animation->bezier_track_get_key_in_handle(0, 0) == Vector2(-0.5, -2));
	CHECK(animation->bezier_track_get_key_out_handle(0, 0) == Vector2(0.5, 2));
	CHECK(animation->bezier_track_get_key_handle_mode(0, 0) == Animation::HANDLE_MODE_MIRRORED);
	CHECK(harness.undo_redo->get_history_count() == 2);
	CHECK(harness.undo_redo->undo());
	CHECK(harness.animation("Walk")->track_get_type(0) == Animation::TYPE_VALUE);
	CHECK(harness.animation("Walk")->track_get_key_count(0) == 3);
	CHECK(harness.undo_redo->redo());
	CHECK(harness.animation("Walk") == animation);
}

TEST_CASE("[GodotMCP][Animation] Libraries, clip rename and duplicate, RESET, clear and remove") {
	AnimationHarness harness;
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"add_library","library":"movement"},
		{"op":"create_animation","animation":"movement/Walk","properties":{"length":3,"loop_mode":2}},
		{"op":"add_track","animation":"movement/Walk","type":"value","path":".:weight"},
		{"op":"insert_key","animation":"movement/Walk","track":0,"time":0,"value":0.5},
		{"op":"rename_animation","animation":"movement/Walk","new_name":"Run"},
		{"op":"duplicate_animation","animation":"movement/Run","new_name":"RESET"},
		{"op":"set_key","animation":"movement/RESET","track":0,"key":0,"value":0.8},
		{"op":"rename_library","library":"movement","new_name":"locomotion"},
		{"op":"set_animation_properties","animation":"locomotion/RESET","properties":{"length":4,"loop_mode":0}},
		{"op":"create_animation","animation":"Temporary"},
		{"op":"remove_animation","animation":"Temporary"}
	])")
					.is_empty());
	INFO(harness.error_message.utf8().get_data());
	REQUIRE(harness.error_code == 0);
	CHECK_FALSE(harness.player->has_animation_library("movement"));
	CHECK(harness.player->has_animation_library("locomotion"));
	CHECK_FALSE(harness.player->has_animation("locomotion/Walk"));
	CHECK_FALSE(harness.player->has_animation("Temporary"));
	CHECK(harness.animation("locomotion/Run")->track_get_key_value(0, 0) == Variant(0.5));
	CHECK(harness.animation("locomotion/RESET")->track_get_key_value(0, 0) == Variant(0.8));
	CHECK(harness.animation("locomotion/Run") != harness.animation("locomotion/RESET"));
	CHECK(harness.animation("locomotion/RESET")->get_length() == doctest::Approx(4));
	CHECK(harness.undo_redo->undo());
	CHECK_FALSE(harness.player->has_animation_library("locomotion"));
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.undo_redo->redo());
	const Ref<AnimationLibrary> locomotion = harness.player->get_animation_library("locomotion");
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"clear_animation","animation":"locomotion/RESET"},
		{"op":"remove_library","library":""}
	])")
					.is_empty());
	CHECK(harness.animation("locomotion/RESET")->get_track_count() == 0);
	CHECK(harness.animation("locomotion/RESET")->get_length() == doctest::Approx(1));
	CHECK_FALSE(harness.player->has_animation_library(StringName()));
	CHECK(harness.undo_redo->undo());
	CHECK(harness.player->get_animation_library("locomotion") == locomotion);
	CHECK(harness.player->has_animation_library(StringName()));
}

TEST_CASE("[GodotMCP][Animation] Dry run, late failure and key-time collision preserve exact originals") {
	AnimationHarness harness;
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"create_animation","animation":"Idle"},
		{"op":"add_track","animation":"Idle","type":"value","path":".:value"},
		{"op":"insert_key","animation":"Idle","track":0,"time":0,"value":1},
		{"op":"insert_key","animation":"Idle","track":0,"time":1,"value":2}
	])")
					.is_empty());
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	const Ref<Animation> clip = harness.animation("Idle");
	const String before = harness.version();
	const int history_count = harness.undo_redo->get_history_count();
	const Array operations = JSON::parse_string(R"([
		{"op":"set_key","animation":"Idle","track":0,"key":0,"value":7},
		{"op":"rename_library","library":"","new_name":"preview"},
		{"op":"duplicate_animation","animation":"preview/Idle","new_name":"Copy"}
	])");
	const String input = JSON::stringify(operations);
	const Dictionary preview = harness.apply(operations, true);
	INFO(harness.error_message.utf8().get_data());
	REQUIRE(harness.error_code == 0);
	CHECK_FALSE(bool(preview["applied"]));
	CHECK(bool(preview["dry_run"]));
	CHECK(harness.version() == before);
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.animation("Idle") == clip);
	CHECK(clip->track_get_key_value(0, 0) == Variant(1.0));
	CHECK(harness.undo_redo->get_history_count() == history_count);
	CHECK(JSON::stringify(operations) == input);

	Array invalid = operations.duplicate(true);
	invalid.push_back(JSON::parse_string(R"({"op":"remove_track","animation":"preview/Copy","track":99})"));
	CHECK(harness.apply(invalid).is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(harness.error_message.contains("operation 3"));
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.version() == before);
	CHECK(harness.undo_redo->get_history_count() == history_count);
	CHECK(harness.apply_json(R"([
		{"op":"set_key","animation":"Idle","track":0,"key":0,"time":1,"value":100}
	])")
					.is_empty());
	CHECK(clip->track_get_key_value(0, 0) == Variant(1.0));
	CHECK(clip->track_get_key_count(0) == 2);
}

TEST_CASE("[GodotMCP][Animation] Approximate time moves cannot replace any other track key") {
	for (int type = Animation::TYPE_VALUE; type <= Animation::TYPE_ANIMATION; type++) {
		AnimationHarness harness;
		Ref<Animation> source;
		source.instantiate();
		const int track = source->add_track(Animation::TrackType(type));
		source->track_set_path(track, NodePath(".:data"));
		Variant value = 7;
		switch (type) {
			case Animation::TYPE_POSITION_3D:
			case Animation::TYPE_SCALE_3D:
				value = Vector3(1, 2, 3);
				break;
			case Animation::TYPE_ROTATION_3D:
				value = Quaternion();
				break;
			case Animation::TYPE_METHOD: {
				Dictionary method;
				method["method"] = "example";
				method["args"] = Array();
				value = method;
			} break;
			case Animation::TYPE_BEZIER:
				value = Array({ 1.0, -0.2, 0.0, 0.2, 0.0 });
				break;
			case Animation::TYPE_AUDIO: {
				Dictionary audio;
				audio["stream"] = Variant();
				audio["start_offset"] = 0.0;
				audio["end_offset"] = 0.0;
				value = audio;
			} break;
			case Animation::TYPE_ANIMATION:
				value = "Clip";
				break;
			default:
				break;
		}
		source->track_insert_key(track, 1000, value);
		source->track_insert_key(track, 1000.015, value);
		REQUIRE(source->track_get_key_count(track) == 2);
		const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
		original->add_animation("Clip", source);
		const String before = harness.version();
		CHECK(harness.apply_json(R"([{"op":"set_key","animation":"Clip","track":0,"key":0,"time":1000.0075}])").is_empty());
		CHECK(harness.error_message.contains("collides with another key"));
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(source->track_get_key_count(track) == 2);
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
		REQUIRE_FALSE(harness.apply_json(R"([{"op":"set_key","animation":"Clip","track":0,"key":0,"time":1000.03}])").is_empty());
		CHECK(harness.animation("Clip")->track_get_key_count(track) == 2);
		CHECK(harness.animation("Clip")->track_get_key_time(track, 1) == doctest::Approx(1000.03));
		CHECK(harness.undo_redo->undo());
		CHECK(harness.animation("Clip") == source);
	}
}

TEST_CASE("[GodotMCP][Animation] Rotation keys reject scaled shear and reflected bases without native conversion errors") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	source->add_track(Animation::TYPE_ROTATION_3D);
	source->track_set_path(0, NodePath("."));
	source->rotation_track_insert_key(0, 0, Quaternion());
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	original->add_animation("Rotation", source);
	String components;
	SUBCASE("nonsingular scaled rotation") {
		components = "[0,-2,0,2,0,0,0,0,2]";
	}
	SUBCASE("shear") {
		components = "[1,0,0,0.5,1,0,0,0,1]";
	}
	SUBCASE("reflection") {
		components = "[-1,0,0,0,1,0,0,0,1]";
	}
	const String before = harness.version();
	CHECK(harness.apply_json("[{\"op\":\"insert_key\",\"animation\":\"Rotation\",\"track\":0,\"time\":1,\"value\":{\"__type\":\"Basis\",\"value\":" + components + "}}]").is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(harness.error_message.contains("proper rotation"));
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.version() == before);
	CHECK_FALSE(harness.undo_redo->has_undo());
	REQUIRE_FALSE(harness.apply_json(R"([{"op":"insert_key","animation":"Rotation","track":0,"time":1,"value":{"__type":"Basis","value":[0,-1,0,1,0,0,0,0,1]}}])").is_empty());
	const Basis rotation(Vector3(0, -1, 0), Vector3(1, 0, 0), Vector3(0, 0, 1));
	CHECK(harness.animation("Rotation")->track_get_key_value(0, 1) == Variant(rotation.get_quaternion()));
}

TEST_CASE("[GodotMCP][Animation] Qualified renames remap autoplay blends and next links in one undo transaction") {
	AnimationHarness harness;
	AnimationErrorCounter errors;
	String library_name = "movement";
	String destination = "locomotion";
	SUBCASE("named to named library") {}
	SUBCASE("named to default library") {
		destination = "";
		harness.player->remove_animation_library(StringName());
	}
	SUBCASE("default to named library") {
		library_name = "";
	}
	Ref<AnimationLibrary> original;
	original.instantiate();
	Ref<Animation> idle;
	idle.instantiate();
	Ref<Animation> run;
	run.instantiate();
	original->add_animation("Idle", idle);
	original->add_animation("Run", run);
	if (harness.player->has_animation_library(library_name)) {
		harness.player->remove_animation_library(library_name);
	}
	harness.player->add_animation_library(library_name, original);
	Ref<AnimationLibrary> elsewhere;
	elsewhere.instantiate();
	Ref<Animation> other;
	other.instantiate();
	elsewhere->add_animation("Other", other);
	harness.player->add_animation_library("elsewhere", elsewhere);
	const String from = library_name.is_empty() ? String("Idle") : library_name + "/Idle";
	const String from_run = library_name.is_empty() ? String("Run") : library_name + "/Run";
	const String to = destination.is_empty() ? String("Walk") : destination + "/Walk";
	const String to_run = destination.is_empty() ? String("Run") : destination + "/Run";
	const double blend = 0.123456789012345;
	harness.player->set_autoplay(from);
	harness.player->set_blend_time(from, from_run, blend);
	harness.player->set_blend_time(from_run, from, 0.75);
	harness.player->set_blend_time("elsewhere/Other", from, 0.25);
	harness.player->animation_set_next(from, from_run);
	harness.player->animation_set_next(from_run, from);
	harness.player->animation_set_next("elsewhere/Other", from);
	Ref<AnimationLibraryUpdateObserver> observer;
	observer.instantiate();
	harness.player->connect(SNAME("animation_libraries_updated"), callable_mp(observer.ptr(), &AnimationLibraryUpdateObserver::updated).bind(harness.player));
	const Dictionary authored_before = _stored_properties(harness.player);
	const String before = harness.version();
	Dictionary rename_clip;
	rename_clip["op"] = "rename_animation";
	rename_clip["animation"] = from;
	rename_clip["new_name"] = "Walk";
	Dictionary rename_library;
	rename_library["op"] = "rename_library";
	rename_library["library"] = library_name;
	rename_library["new_name"] = destination;
	const Array operations({ rename_clip, rename_library });
	REQUIRE_FALSE(harness.apply(operations, true).is_empty());
	CHECK(observer->updates == 0);
	CHECK(harness.player->get_autoplay() == StringName(from));
	CHECK(harness.player->get_blend_time(from, from_run) == blend);
	Array invalid = operations.duplicate(true);
	invalid.push_back(JSON::parse_string(R"({"op":"remove_library","library":"missing"})"));
	CHECK(harness.apply(invalid).is_empty());
	CHECK(observer->updates == 0);
	CHECK(harness.version() == before);
	REQUIRE_FALSE(harness.apply(operations).is_empty());
	CHECK(errors.count == 0);
	CHECK(harness.undo_redo->get_history_count() == 1);
	CHECK(observer->updates == 1);
	CHECK(observer->autoplay == to);
	CHECK(observer->preview->has_animation(to));
	CHECK_FALSE(observer->preview->has_animation(from));
	CHECK(harness.player->get_autoplay() == StringName(to));
	CHECK(harness.player->get_blend_time(to, to_run) == blend);
	CHECK(harness.player->get_blend_time(to_run, to) == 0.75);
	CHECK(harness.player->get_blend_time("elsewhere/Other", to) == 0.25);
	CHECK(harness.player->get_blend_time(from, from_run) == 0);
	CHECK(harness.player->animation_get_next(to) == StringName(to_run));
	CHECK(harness.player->animation_get_next(to_run) == StringName(to));
	CHECK(harness.player->animation_get_next("elsewhere/Other") == StringName(to));
	CHECK(harness.player->animation_get_next(from).is_empty());
	CHECK(harness.player->get_animation_library("elsewhere") == elsewhere);
	CHECK(harness.undo_redo->undo());
	CHECK(errors.count == 0);
	CHECK(observer->updates == 2);
	CHECK(observer->autoplay == from);
	CHECK(observer->preview->get_animation_library(library_name) == original);
	CHECK(harness.player->get_animation_library(library_name) == original);
	CHECK(harness.player->get_autoplay() == StringName(from));
	CHECK(harness.player->get_blend_time(from, from_run) == blend);
	CHECK(harness.player->get_blend_time(to, to_run) == 0);
	CHECK(harness.player->animation_get_next(from) == StringName(from_run));
	CHECK(harness.player->animation_get_next("elsewhere/Other") == StringName(from));
	CHECK(_stored_properties(harness.player) == authored_before);
	CHECK(harness.version() == before);
	CHECK(harness.undo_redo->redo());
	CHECK(errors.count == 0);
	CHECK(observer->updates == 3);
	CHECK(harness.player->get_autoplay() == StringName(to));
	CHECK(harness.player->get_blend_time(to, to_run) == blend);
	CHECK(harness.player->animation_get_next(to_run) == StringName(to));
}

TEST_CASE("[GodotMCP][Animation] Mixer replacement notifies dummy preview only after complete do undo and redo") {
	AnimationHarness harness;
	AnimationTree *tree = memnew(AnimationTree);
	tree->set_name("Tree");
	harness.root->add_child(tree);
	tree->set_owner(harness.root);
	Ref<AnimationLibrary> original;
	original.instantiate();
	Ref<Animation> source;
	source.instantiate();
	original->add_animation("Idle", source);
	tree->add_animation_library(StringName(), original);
	Ref<AnimationLibraryUpdateObserver> observer;
	observer.instantiate();
	observer->updated(tree);
	observer->updates = 0;
	tree->connect(SNAME("animation_libraries_updated"), callable_mp(observer.ptr(), &AnimationLibraryUpdateObserver::updated).bind(tree));
	const Array operations = JSON::parse_string(R"([{"op":"create_animation","animation":"New"},{"op":"rename_library","library":"","new_name":"preview"}])");
	Dictionary params = harness.params(operations, true);
	params["node_path"] = "Tree";
	REQUIRE_FALSE(harness.tools.apply(params, harness.context(), harness.warnings, harness.error_code, harness.error_message).is_empty());
	CHECK(observer->updates == 0);
	params["dry_run"] = false;
	REQUIRE_FALSE(harness.tools.apply(params, harness.context(), harness.warnings, harness.error_code, harness.error_message).is_empty());
	CHECK(observer->updates == 1);
	CHECK_FALSE(observer->preview->has_animation_library(StringName()));
	CHECK(observer->preview->has_animation("preview/New"));
	CHECK(observer->preview->get_animation_library("preview") == tree->get_animation_library("preview"));
	CHECK(harness.undo_redo->undo());
	CHECK(observer->updates == 2);
	CHECK(observer->preview->get_animation_library(StringName()) == original);
	CHECK_FALSE(observer->preview->has_animation_library("preview"));
	CHECK(harness.undo_redo->redo());
	CHECK(observer->updates == 3);
	CHECK(observer->preview->has_animation("preview/New"));
}

TEST_CASE("[GodotMCP][Animation] Native binding history safely skips a freed target mixer") {
	AnimationHarness harness;
	AnimationErrorCounter errors;
	REQUIRE_FALSE(harness.apply_json(R"([{"op":"create_animation","animation":"Clip"}])").is_empty());
	CHECK(errors.count == 0);
	CHECK(harness.player->has_animation("Clip"));
	CHECK(harness.undo_redo->undo());
	CHECK(errors.count == 0);
	CHECK_FALSE(harness.player->has_animation("Clip"));
	CHECK(harness.undo_redo->redo());
	CHECK(errors.count == 0);
	CHECK(harness.player->has_animation("Clip"));
	harness.root->remove_child(harness.player);
	memdelete(harness.player);
	harness.player = nullptr;
	CHECK(harness.undo_redo->undo());
	CHECK(errors.count == 0);
	CHECK(harness.undo_redo->redo());
	CHECK(errors.count == 0);
}

TEST_CASE("[GodotMCP][Animation] Shared and external library, animation and nested resource isolation") {
	AnimationHarness harness;
	AnimationErrorCounter errors;
	Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	original->set_path("res://animation_shared_library.tres");
	Ref<Animation> clip;
	clip.instantiate();
	clip->set_path("res://animation_shared_clip.anim");
	Ref<Resource> nested;
	nested.instantiate();
	nested->set_meta("tag", "original");
	const int track = clip->add_track(Animation::TYPE_VALUE);
	clip->track_set_path(track, NodePath(".:asset"));
	Dictionary key;
	key["nested"] = Array({ nested });
	clip->track_insert_key(track, 0, key);
	original->add_animation("Idle", clip);
	original->add_animation("Alias", clip);
	AnimationPlayer *other = memnew(AnimationPlayer);
	other->set_name("Other");
	harness.root->add_child(other);
	other->set_owner(harness.root);
	REQUIRE(other->add_animation_library(StringName(), original) == OK);
	const Dictionary player_before = _stored_properties(harness.player);
	const Dictionary other_before = _stored_properties(other);
	const Dictionary library_before = _stored_properties(original.ptr());
	const Dictionary clip_before = _stored_properties(clip.ptr());
	const Dictionary nested_before = _stored_properties(nested.ptr());
	const String before = harness.version();
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"set_animation_properties","animation":"Idle","properties":{"length":5}},
		{"op":"set_track","animation":"Idle","track":0,"properties":{"enabled":false}}
	])")
					.is_empty());
	INFO(harness.error_message.utf8().get_data());
	REQUIRE(harness.error_code == 0);
	CHECK(errors.count == 0);
	const Ref<Animation> edited = harness.animation("Idle");
	CHECK(edited != clip);
	CHECK(edited->get_path().is_empty());
	CHECK(edited->is_local_to_scene());
	CHECK(harness.player->get_animation_library(StringName())->get_path().is_empty());
	CHECK(other->get_animation_library(StringName()) == original);
	CHECK(other->get_animation("Idle") == clip);
	CHECK(clip->get_length() == doctest::Approx(1));
	CHECK(clip->track_is_enabled(0));
	CHECK(harness.animation("Alias")->get_length() == doctest::Approx(1));
	CHECK(harness.animation("Alias") != edited);
	const Dictionary edited_key = edited->track_get_key_value(0, 0);
	const Ref<Resource> edited_nested = Array(edited_key["nested"])[0];
	REQUIRE(edited_nested.is_valid());
	CHECK(edited_nested != nested);
	CHECK(edited_nested->get_meta("tag") == Variant("original"));
	CHECK(harness.undo_redo->undo());
	CHECK(errors.count == 0);
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.animation("Idle") == clip);
	CHECK(_stored_properties(harness.player) == player_before);
	CHECK(_stored_properties(other) == other_before);
	CHECK(_stored_properties(original.ptr()) == library_before);
	CHECK(_stored_properties(clip.ptr()) == clip_before);
	CHECK(_stored_properties(nested.ptr()) == nested_before);
	CHECK(harness.version() == before);
	CHECK(harness.undo_redo->redo());
	CHECK(errors.count == 0);
	CHECK(harness.animation("Idle") == edited);
}

TEST_CASE("[GodotMCP][Animation] Discovery and bounded inspection report counts and truncation") {
	AnimationHarness harness;
	AnimationTree *tree = memnew(AnimationTree);
	tree->set_name("Tree");
	harness.root->add_child(tree);
	tree->set_owner(harness.root);
	REQUIRE_FALSE(harness.apply_json(R"([
		{"op":"create_animation","animation":"One"},
		{"op":"create_animation","animation":"Two"},
		{"op":"add_track","animation":"One","type":"value","path":".:a"},
		{"op":"add_track","animation":"One","type":"value","path":".:b"},
		{"op":"insert_key","animation":"One","track":0,"time":0,"value":1},
		{"op":"insert_key","animation":"One","track":0,"time":1,"value":2},
		{"op":"insert_key","animation":"One","track":1,"time":0,"value":3}
	])")
					.is_empty());
	Dictionary params;
	Dictionary data = harness.tools.list(params, harness.context(), harness.error_code, harness.error_message);
	REQUIRE(harness.error_code == 0);
	CHECK(int(data["node_count"]) == 2);
	CHECK(int(data["animation_count"]) == 2);
	CHECK(int(data["track_count"]) == 2);
	CHECK_FALSE(bool(data["truncated"]));
	CHECK(bool(data["counts_complete"]));
	params["limit"] = 1;
	data = harness.tools.list(params, harness.context(), harness.error_code, harness.error_message);
	REQUIRE(harness.error_code == 0);
	CHECK(bool(data["truncated"]));
	CHECK_FALSE(bool(data["counts_complete"]));
	CHECK(int(data["returned_node_count"]) == 1);
	CHECK(int(data["returned_animation_count"]) == 1);
	CHECK(int(data["returned_track_count"]) == 1);

	data = harness.get("One", true, 1);
	REQUIRE(harness.error_code == 0);
	CHECK(int(data["key_count"]) == 3);
	CHECK(int(data["returned_key_count"]) == 1);
	CHECK(bool(data["keys_truncated"]));
	CHECK(bool(data["truncated"]));
	CHECK(int(Dictionary(_tracks(data)[0])["key_count"]) == 2);
	CHECK(int(Dictionary(_tracks(data)[1])["returned_key_count"]) == 0);
	data = harness.get("One", false);
	CHECK_FALSE(Dictionary(_tracks(data)[0]).has("keys"));
	CHECK(int(data["key_count"]) == 3);
	CHECK_FALSE(bool(data["truncated"]));
	CHECK(harness.get("Missing").is_empty());
	CHECK(harness.error_code == GodotMCPError::NOT_FOUND);
}

TEST_CASE("[GodotMCP][Animation] Compressed imported tracks become local editable keys") {
	AnimationHarness harness;
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	Ref<Animation> source;
	source.instantiate();
	source->set_length(2);
	source->set_loop_mode(Animation::LOOP_LINEAR);
	const int track = source->add_track(Animation::TYPE_POSITION_3D);
	source->track_set_path(track, NodePath("."));
	source->track_set_imported(track, true);
	for (int i = 0; i < 100; i++) {
		source->position_track_insert_key(track, i * 0.01, Vector3(i * 0.1, i * 0.2, i * 0.3));
	}
	source->compress();
	REQUIRE(source->track_is_compressed(0));
	source->track_set_enabled(track, false);
	source->track_set_interpolation_type(track, Animation::INTERPOLATION_NEAREST);
	source->track_set_interpolation_loop_wrap(track, false);
	REQUIRE(original->add_animation("Imported", source) == OK);
	SUBCASE("edit compressed clip directly") {}
	SUBCASE("unrelated clip creation preserves compressed source metadata") {
		REQUIRE_FALSE(harness.apply_json(R"([{"op":"create_animation","animation":"Unrelated"}])").is_empty());
		const Ref<Animation> shadow = harness.animation("Imported");
		CHECK(shadow != source);
		CHECK(shadow->track_is_compressed(track));
		CHECK_FALSE(shadow->track_is_enabled(track));
		CHECK(shadow->track_is_imported(track));
		CHECK(shadow->track_get_interpolation_type(track) == Animation::INTERPOLATION_NEAREST);
		CHECK_FALSE(shadow->track_get_interpolation_loop_wrap(track));
		CHECK(harness.undo_redo->undo());
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.animation("Imported") == source);
		CHECK(harness.undo_redo->redo());
		CHECK(harness.animation("Imported") == shadow);
		CHECK_FALSE(shadow->track_get_interpolation_loop_wrap(track));
		CHECK(harness.undo_redo->undo());
	}
	const Dictionary result = harness.apply_json(R"([
		{"op":"set_key","animation":"Imported","track":0,"key":0,"value":{"__type":"Vector3","value":[5,6,7]}},
		{"op":"set_track","animation":"Imported","track":0,"properties":{"imported":false}}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	REQUIRE(harness.error_code == 0);
	const Ref<Animation> edited = harness.animation("Imported");
	CHECK_FALSE(edited->track_is_compressed(0));
	CHECK_FALSE(edited->track_is_imported(0));
	CHECK_FALSE(edited->track_is_enabled(0));
	CHECK(edited->track_get_interpolation_type(0) == Animation::INTERPOLATION_NEAREST);
	CHECK_FALSE(edited->track_get_interpolation_loop_wrap(0));
	CHECK(edited->track_get_key_value(0, 0) == Variant(Vector3(5, 6, 7)));
	CHECK(edited->get_length() == doctest::Approx(2));
	CHECK(edited->get_loop_mode() == Animation::LOOP_LINEAR);
	CHECK(source->track_is_compressed(0));
	CHECK(source->track_is_imported(0));
	CHECK_FALSE(source->track_get_interpolation_loop_wrap(0));
	CHECK(harness.undo_redo->undo());
	CHECK(harness.animation("Imported") == source);
	CHECK(harness.player->get_animation_library(StringName()) == original);
}

TEST_CASE("[GodotMCP][Animation] Typed nested and packed values survive JSON inspection and reinsertion") {
	AnimationHarness harness;
	const Dictionary result = harness.apply_json(R"([
		{"op":"create_animation","animation":"Typed"},
		{"op":"add_track","animation":"Typed","type":"value","path":".:data"},
		{"op":"insert_key","animation":"Typed","track":0,"time":0,"value":{"vectors":{"__type":"PackedVector3Array","value":[{"__type":"Vector3","value":[1,2,3]}]},"bytes":{"__type":"PackedByteArray","value":[0,255]},"matrix":{"__type":"Basis","value":[1,2,3,4,5,6,7,8,9]},"color":{"__type":"Color","value":[0.1,0.2,0.3,1]},"literal":{"__type":"Dictionary","value":[["__type","ordinary"]]},"non_string_key":{"__type":"Dictionary","value":[[4,"four"]]}}}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	REQUIRE(harness.error_code == 0);
	const Ref<Animation> animation = harness.animation("Typed");
	const Variant original = animation->track_get_key_value(0, 0);
	const Dictionary dictionary = original;
	CHECK(dictionary["vectors"].get_type() == Variant::PACKED_VECTOR3_ARRAY);
	CHECK(dictionary["bytes"].get_type() == Variant::PACKED_BYTE_ARRAY);
	CHECK(dictionary["matrix"] == Variant(Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))));
	CHECK(Dictionary(dictionary["literal"])["__type"] == Variant("ordinary"));
	CHECK(Dictionary(dictionary["non_string_key"])[4.0] == Variant("four"));
	const Dictionary data = harness.get("Typed");
	REQUIRE_FALSE(bool(data["values_truncated"]));
	Dictionary insert;
	insert["op"] = "insert_key";
	insert["animation"] = "Typed";
	insert["track"] = 0;
	insert["time"] = 1;
	insert["value"] = JSON::parse_string(JSON::stringify(Dictionary(_keys(data, 0)[0])["value"]));
	REQUIRE_FALSE(harness.apply(Array({ insert })).is_empty());
	CHECK(harness.animation("Typed")->track_get_key_value(0, 1) == original);
}

TEST_CASE("[GodotMCP][Animation] Native int dictionary keys and full range PackedInt64Array survive JSON round trips") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	source->add_track(Animation::TYPE_VALUE);
	source->track_set_path(0, NodePath(".:data"));
	PackedInt64Array packed;
	packed.push_back(INT64_MIN);
	packed.push_back(INT64_MAX);
	packed.push_back(INT64_C(9007199254740993));
	packed.push_back(-INT64_C(9007199254740993));
	packed.push_back(4);
	Dictionary value;
	value[4] = "four";
	value["minimum"] = INT64_MIN;
	value["maximum"] = INT64_MAX;
	value["packed"] = packed;
	source->track_insert_key(0, 0, value);
	harness.player->get_animation_library(StringName())->add_animation("Integers", source);
	const Dictionary data = harness.get("Integers");
	REQUIRE_FALSE(bool(data["values_truncated"]));
	const Variant encoded = JSON::parse_string(JSON::stringify(Dictionary(_keys(data, 0)[0])["value"]));
	const Array entries = Dictionary(encoded)["value"];
	const Dictionary key = Array(entries[0])[0];
	CHECK(key["__type"] == Variant("int"));
	CHECK(key["value"] == Variant("4"));
	Dictionary insert;
	insert["op"] = "insert_key";
	insert["animation"] = "Integers";
	insert["track"] = 0;
	insert["time"] = 1;
	insert["value"] = encoded;
	const Dictionary result = harness.apply(Array({ insert }));
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	const Dictionary restored = harness.animation("Integers")->track_get_key_value(0, 1);
	CHECK(restored == value);
	CHECK(restored.keys()[0].get_type() == Variant::INT);
	CHECK(restored[4] == Variant("four"));
	CHECK(restored["minimum"].get_type() == Variant::INT);
	CHECK(int64_t(restored["minimum"]) == INT64_MIN);
	CHECK(int64_t(restored["maximum"]) == INT64_MAX);
	CHECK(restored["packed"].get_type() == Variant::PACKED_INT64_ARRAY);
	CHECK(PackedInt64Array(restored["packed"]) == packed);
	REQUIRE_FALSE(harness.apply_json(R"([{"op":"insert_key","animation":"Integers","track":0,"time":2,"value":{"__type":"PackedInt64Array","value":[4,{"__type":"int","value":"9223372036854775807"},{"__type":"int","value":"-9223372036854775808"}]}}])").is_empty());
	const PackedInt64Array mixed = harness.animation("Integers")->track_get_key_value(0, 2);
	CHECK(mixed[0] == 4);
	CHECK(mixed[1] == INT64_MAX);
	CHECK(mixed[2] == INT64_MIN);
}

TEST_CASE("[GodotMCP][Animation] Invalid decimal int wrappers and unsafe raw packed integers reject atomically") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	source->add_track(Animation::TYPE_VALUE);
	source->track_set_path(0, NodePath(".:data"));
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	original->add_animation("Integers", source);
	const Array invalid({ "", "+1", "01", "-0", "1.0", " 1", "1e3", "9223372036854775808", "-9223372036854775809", "999999999999999999999", 1 });
	const String before = harness.version();
	for (const Variant &text : invalid) {
		Dictionary wrapper;
		wrapper["__type"] = "int";
		wrapper["value"] = text;
		Dictionary insert;
		insert["op"] = "insert_key";
		insert["animation"] = "Integers";
		insert["track"] = 0;
		insert["time"] = 0;
		insert["value"] = wrapper;
		CHECK(harness.apply(Array({ insert })).is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
	}
	CHECK(harness.apply_json(R"([{"op":"insert_key","animation":"Integers","track":0,"time":0,"value":{"__type":"PackedInt64Array","value":[9007199254740992]}}])").is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(source->track_get_key_count(0) == 0);
}

TEST_CASE("[GodotMCP][Animation] Dictionary and Array wrapper transport does not inflate semantic value bounds") {
	AnimationHarness harness;
	Variant value;
	SUBCASE("six nested plain dictionaries") {
		value = 4;
		for (int i = 0; i < 6; i++) {
			Dictionary nested;
			nested["nested"] = value;
			value = nested;
		}
	}
	SUBCASE("typed arrays at eight semantic levels") {
		value = 4;
		for (int i = 0; i < 8; i++) {
			Dictionary wrapper;
			wrapper["__type"] = "Array";
			wrapper["value"] = Array({ value });
			value = wrapper;
		}
	}
	SUBCASE("entry pairs do not consume the 10000 item budget") {
		Array values;
		for (int i = 0; i < 2400; i++) {
			Dictionary dictionary;
			dictionary["k"] = i;
			values.push_back(dictionary);
		}
		value = values;
	}
	Array operations = JSON::parse_string(R"([{"op":"create_animation","animation":"Nested"},{"op":"add_track","animation":"Nested","type":"value","path":".:data"}])");
	Dictionary insert;
	insert["op"] = "insert_key";
	insert["animation"] = "Nested";
	insert["track"] = 0;
	insert["time"] = 0;
	insert["value"] = value;
	operations.push_back(insert);
	const Dictionary result = harness.apply(operations);
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	const Variant original = harness.animation("Nested")->track_get_key_value(0, 0);
	const Dictionary data = harness.get("Nested");
	REQUIRE_FALSE(bool(data["values_truncated"]));
	insert["time"] = 1;
	insert["value"] = JSON::parse_string(JSON::stringify(Dictionary(_keys(data, 0)[0])["value"]));
	const Dictionary reinsert = harness.apply(Array({ insert }));
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(reinsert.is_empty());
	CHECK(harness.animation("Nested")->track_get_key_value(0, 1) == original);
}

TEST_CASE("[GodotMCP][Animation] Math wrappers share scene-patch conventions and round trip") {
	AnimationHarness harness;
	const Dictionary result = harness.apply_json(R"([
		{"op":"create_animation","animation":"Math"},
		{"op":"add_track","animation":"Math","type":"value","path":".:data"},
		{"op":"insert_key","animation":"Math","track":0,"time":0,"value":{
			"rgb":{"__type":"Color","value":[0.25,0.5,0.75]},
			"html":{"__type":"Color","value":"#ff0000"},
			"transform":{"__type":"Transform3D","value":[1,2,3,4,5,6,7,8,9,10,11,12]},
			"projection":{"__type":"Projection","value":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}
		}}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	const Variant original = harness.animation("Math")->track_get_key_value(0, 0);
	const Dictionary values = original;
	CHECK(values["rgb"] == Variant(Color(0.25, 0.5, 0.75)));
	CHECK(values["html"] == Variant(Color(1, 0, 0)));
	CHECK(values["transform"] == Variant(Transform3D(Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9)), Vector3(10, 11, 12))));
	CHECK(values["projection"] == Variant(Projection()));
	const Dictionary data = harness.get("Math");
	REQUIRE_FALSE(bool(data["values_truncated"]));
	Dictionary insert;
	insert["op"] = "insert_key";
	insert["animation"] = "Math";
	insert["track"] = 0;
	insert["time"] = 1;
	insert["value"] = JSON::parse_string(JSON::stringify(Dictionary(_keys(data, 0)[0])["value"]));
	REQUIRE_FALSE(harness.apply(Array({ insert })).is_empty());
	CHECK(harness.animation("Math")->track_get_key_value(0, 1) == original);
}

TEST_CASE("[GodotMCP][Animation] Freed Object keys are safe to inspect and cannot be deep duplicated") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	const int track = source->add_track(Animation::TYPE_VALUE);
	source->track_set_path(track, NodePath(".:data"));
	Node *temporary = memnew(Node);
	const Variant reference = temporary;
	memdelete(temporary);
	source->track_insert_key(track, 0, reference);
	const Variant stored = source->track_get_key_value(track, 0);
	REQUIRE(stored.get_type() == Variant::OBJECT);
	bool previously_freed = false;
	CHECK(stored.get_validated_object_with_check(previously_freed) == nullptr);
	REQUIRE(previously_freed);
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	REQUIRE(original->add_animation("Freed", source) == OK);
	const String before = harness.version();
	const Dictionary data = harness.get("Freed");
	REQUIRE_FALSE(data.is_empty());
	CHECK(Dictionary(_keys(data, 0)[0])["value"].get_type() == Variant::NIL);
	CHECK(harness.apply_json(R"([{"op":"set_animation_properties","animation":"Freed","properties":{"length":2}}])").is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(harness.error_message.contains("freed Object references"));
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(source->get_length() == doctest::Approx(1));
	CHECK(harness.version() == before);
	CHECK_FALSE(harness.undo_redo->has_undo());
}

TEST_CASE("[GodotMCP][Animation] Cyclic values have bounded inspection and safe mutation rejection") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	const int track = source->add_track(Animation::TYPE_VALUE);
	source->track_set_path(track, NodePath(".:value"));
	Array cycle;
	cycle.push_back(cycle);
	source->track_insert_key(track, 0, cycle);
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	original->add_animation("Cycle", source);
	const Dictionary data = harness.get("Cycle");
	CHECK(harness.error_code == 0);
	CHECK(bool(data["values_truncated"]));
	CHECK(bool(data["truncated"]));
	CHECK(harness.apply_json(R"([{"op":"set_animation_properties","animation":"Cycle","properties":{"length":2}}])").is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(harness.error_message.contains("Cyclic Variant containers"));
	CHECK(harness.undo_redo->get_history_count() == 0);
	CHECK(harness.player->get_animation_library(StringName()) == original);
	CHECK(harness.animation("Cycle") == source);
	cycle.clear(); // Release the intentional self-reference before fixture teardown.
}

TEST_CASE("[GodotMCP][Animation] Graph preflight visits nested arrays and permits non-cyclic shared containers") {
	AnimationHarness harness;
	Ref<Animation> source;
	source.instantiate();
	const int track = source->add_track(Animation::TYPE_VALUE);
	source->track_set_path(track, NodePath(".:data"));
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	original->add_animation("Nested", source);
	const String operations = R"([{"op":"set_animation_properties","animation":"Nested","properties":{"length":2}}])";

	SUBCASE("shared arrays are not cycles") {
		const Array shared({ 1, 2, 3 });
		source->track_insert_key(track, 0, Array({ shared, shared }));
		const Dictionary result = harness.apply_json(operations);
		INFO(harness.error_message.utf8().get_data());
		REQUIRE_FALSE(result.is_empty());
		CHECK(harness.error_code == 0);
		CHECK(harness.animation("Nested")->track_get_key_value(track, 0) == source->track_get_key_value(track, 0));
		CHECK(source->get_length() == doctest::Approx(1));
		CHECK(harness.undo_redo->undo());
		CHECK(harness.player->get_animation_library(StringName()) == original);
	}
	SUBCASE("dictionary cycle reached through an array") {
		Dictionary cycle;
		const Array nested({ cycle });
		cycle["nested"] = nested;
		source->track_insert_key(track, 0, Array({ cycle }));
		const String before = harness.version();
		const Dictionary data = harness.get("Nested");
		CHECK(harness.error_code == 0);
		CHECK(bool(data["values_truncated"]));
		CHECK(harness.apply_json(operations).is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("Cyclic Variant containers"));
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
		cycle.clear();
	}
	SUBCASE("freed Object reached through nested arrays") {
		Node *temporary = memnew(Node);
		const Variant reference = temporary;
		memdelete(temporary);
		source->track_insert_key(track, 0, Array({ Array({ reference }) }));
		const String before = harness.version();
		CHECK(harness.apply_json(operations).is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("freed Object references"));
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
	}
}

TEST_CASE("[GodotMCP][Animation] Strict invalid input and stale guards never create history") {
	AnimationHarness harness;
	const String before = harness.version();
	const Array create = JSON::parse_string(R"([{"op":"create_animation","animation":"Idle"}])");
	Dictionary params = harness.params(create);
	SUBCASE("missing version") {
		params.erase("expected_scene_version");
	}
	SUBCASE("stale version") {
		params["expected_scene_version"] = "stale";
	}
	SUBCASE("stale path") {
		params["expected_scene_path"] = "res://other_scene.tscn";
	}
	SUBCASE("missing path") {
		params.erase("expected_scene_path");
	}
	SUBCASE("absolute node path") {
		params["node_path"] = "/root/Player";
	}
	SUBCASE("escaping node path") {
		params["node_path"] = "../Player";
	}
	SUBCASE("property node path") {
		params["node_path"] = "Player:root_node";
	}
	SUBCASE("non-mixer node") {
		params["node_path"] = ".";
	}
	SUBCASE("invalid dry_run") {
		params["dry_run"] = "true";
	}
	SUBCASE("invalid clip name") {
		params["operations"] = JSON::parse_string(R"([{"op":"create_animation","animation":"/Idle"}])");
	}
	SUBCASE("unknown property") {
		params["operations"] = JSON::parse_string(R"([{"op":"create_animation","animation":"Idle","properties":{"unknown":true}}])");
	}
	SUBCASE("invalid enum") {
		params["operations"] = JSON::parse_string(R"([{"op":"create_animation","animation":"Idle","properties":{"loop_mode":3}}])");
	}
	SUBCASE("unsafe resource path") {
		params["operations"] = JSON::parse_string(R"([{"op":"add_library","library":"external","resource_path":"res://../escape.tres"}])");
	}
	SUBCASE("operation limit") {
		Array operations;
		operations.resize(513);
		params["operations"] = operations;
	}
	SUBCASE("empty operations") {
		params["operations"] = Array();
	}
	CHECK(harness.tools.apply(params, harness.context(), harness.warnings, harness.error_code, harness.error_message).is_empty());
	CHECK(harness.error_code != 0);
	CHECK(harness.undo_redo->get_history_count() == 0);
	CHECK(harness.version() == before);
	CHECK(harness.player->get_animation_library(StringName())->get_animation_list_size() == 0);
}

TEST_CASE("[GodotMCP][Animation] Invalid key types, finite times, indexes and paths reject atomically") {
	AnimationHarness harness;
	Array operations = JSON::parse_string(R"([
		{"op":"create_animation","animation":"Clip"},
		{"op":"add_track","animation":"Clip","type":"position_3d","path":"."},
		{"op":"add_track","animation":"Clip","type":"bezier","path":".:value"}
	])");
	Dictionary invalid;
	SUBCASE("wrong position type") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":0,"time":0,"value":[1,2,3]})");
	}
	SUBCASE("fractional index") {
		invalid = JSON::parse_string(R"({"op":"remove_track","animation":"Clip","track":0.5})");
	}
	SUBCASE("negative time") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":0,"time":-1,"value":{"__type":"Vector3","value":[1,2,3]}})");
	}
	SUBCASE("non-finite time") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":0,"time":0,"value":{"__type":"Vector3","value":[1,2,3]}})");
		invalid["time"] = std::numeric_limits<double>::infinity();
	}
	SUBCASE("non-finite component") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":0,"time":0,"value":{"__type":"Vector3","value":[1,2,3]}})");
		Dictionary value = invalid["value"];
		Array components = value["value"];
		components[0] = std::numeric_limits<double>::quiet_NaN();
	}
	SUBCASE("bezier invalid handle sign") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":1,"time":0,"value":{"value":2,"in_handle":[1,0]}})");
	}
	SUBCASE("unsupported transition") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":1,"time":0,"value":{"value":2},"transition":0.5})");
	}
	SUBCASE("wrong-track update mode") {
		invalid = JSON::parse_string(R"({"op":"set_track","animation":"Clip","track":0,"properties":{"value_update_mode":1}})");
	}
	SUBCASE("out-of-scene track path") {
		invalid = JSON::parse_string(R"({"op":"set_track","animation":"Clip","track":0,"properties":{"path":"../Outside:position"}})");
	}
	SUBCASE("unknown operation") {
		invalid = JSON::parse_string(R"({"op":"execute","animation":"Clip","track":0})");
	}
	SUBCASE("excessive nested value") {
		invalid = JSON::parse_string(R"({"op":"insert_key","animation":"Clip","track":1,"time":0,"value":{"value":2}})");
		Variant nested = 1;
		for (int i = 0; i < 20; i++) {
			nested = Array({ nested });
		}
		invalid["value"] = nested;
	}
	operations.push_back(invalid);
	CHECK(harness.apply(operations).is_empty());
	CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(harness.undo_redo->get_history_count() == 0);
	CHECK_FALSE(harness.player->has_animation("Clip"));
}

struct ScopedAnimationProjectPath {
	String previous_path;
	ScopedAnimationProjectPath(const String &p_path) :
			previous_path(TestProjectSettingsInternalsAccessor::resource_path()) {
		TestProjectSettingsInternalsAccessor::resource_path() = p_path;
	}
	~ScopedAnimationProjectPath() {
		TestProjectSettingsInternalsAccessor::resource_path() = previous_path;
	}
};

static Ref<Animation> _compressed_animation(int p_keys) {
	Ref<Animation> animation;
	animation.instantiate();
	animation->set_length(p_keys * 0.025);
	const int track = animation->add_track(Animation::TYPE_POSITION_3D);
	animation->track_set_path(track, NodePath("."));
	for (int i = 0; i < p_keys; i++) {
		animation->position_track_insert_key(track, i * 0.025, Vector3(i % 17, i % 23, i % 31));
	}
	animation->compress();
	return animation;
}

TEST_CASE("[GodotMCP][Animation] Decoded key caps reject before cloning any clip in a compressed library") {
	AnimationHarness harness;
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	int total = 0;
	SUBCASE("per track cap blocks editing and unrelated clip creation") {
		const Ref<Animation> source = _compressed_animation(GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK + 1);
		REQUIRE(source->track_is_compressed(0));
		REQUIRE(source->track_get_key_count(0) > GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK);
		original->add_animation("Compressed", source);
	}
	SUBCASE("aggregate cap spans all unique compressed clips") {
		for (int i = 0; i < 3; i++) {
			const Ref<Animation> source = _compressed_animation(9000);
			REQUIRE(source->track_is_compressed(0));
			REQUIRE(source->track_get_key_count(0) <= GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK);
			total += source->track_get_key_count(0);
			original->add_animation(i == 0 ? String("Compressed") : "Clip" + itos(i), source);
		}
		REQUIRE(total > GodotMCPAnimationLibrary::MAX_DECODED_KEYS);
	}
	const String before = harness.version();
	for (const String &operation : { String(R"([{"op":"create_animation","animation":"Unrelated"}])"), String(R"([{"op":"set_animation_properties","animation":"Compressed","properties":{"length":2}}])") }) {
		CHECK(harness.apply_json(operation, true).is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("Decoded animation keys"));
		CHECK(harness.apply_json(operation).is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("Decoded animation keys"));
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK_FALSE(harness.player->has_animation("Unrelated"));
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
	}
}

TEST_CASE("[GodotMCP][Animation] Imported compressed graphs preserve metadata and enforce decoded key caps before copying") {
	const String temporary_project = TestUtils::get_temp_path("animation_compressed_library_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project) == OK);
	ScopedAnimationProjectPath project_path(temporary_project);
	int count = 100;
	bool excessive = false;
	SUBCASE("small library import preserves source-only compressed metadata") {}
	SUBCASE("oversized resource library import") {
		count = GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK + 1;
		excessive = true;
	}
	const Ref<Animation> source = _compressed_animation(count);
	REQUIRE(source->track_is_compressed(0));
	source->track_set_interpolation_type(0, Animation::INTERPOLATION_NEAREST);
	source->track_set_interpolation_loop_wrap(0, false);
	source->track_set_enabled(0, false);
	source->track_set_imported(0, true);
	Ref<AnimationLibrary> library;
	library.instantiate();
	library->add_animation("Compressed", source);
	REQUIRE(ResourceSaver::save(source, "res://compressed.anim") == OK);
	REQUIRE(ResourceSaver::save(library, "res://compressed_library.tres") == OK);
	const String source_hash = FileAccess::get_md5("res://compressed.anim");
	const String library_hash = FileAccess::get_md5("res://compressed_library.tres");
	if (!excessive) {
		// Cached editor resources can carry metadata omitted from compressed
		// track storage. A clone must preserve the loaded SOURCE, not defaults.
		source->set_path("res://compressed.anim");
		library->set_path("res://compressed_library.tres");
	}
	AnimationHarness harness;
	const Ref<AnimationLibrary> original = harness.player->get_animation_library(StringName());
	const String before = harness.version();
	if (excessive) {
		REQUIRE(source->track_get_key_count(0) > GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK);
		CHECK(harness.apply_json(R"([{"op":"add_library","library":"assets","resource_path":"res://compressed_library.tres"}])").is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("Decoded animation keys"));
		CHECK(harness.apply_json(R"([{"op":"create_animation","animation":"Copy","resource_path":"res://compressed.anim"}])").is_empty());
		CHECK(harness.error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.error_message.contains("Decoded animation keys"));
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK_FALSE(harness.player->has_animation_library("assets"));
		CHECK_FALSE(harness.player->has_animation("Copy"));
		CHECK(harness.version() == before);
		CHECK_FALSE(harness.undo_redo->has_undo());
	} else {
		REQUIRE_FALSE(harness.apply_json(R"([{"op":"add_library","library":"assets","resource_path":"res://compressed_library.tres"},{"op":"create_animation","animation":"Copy","resource_path":"res://compressed.anim"}])").is_empty());
		const Ref<Animation> imported = harness.animation("assets/Compressed");
		CHECK(imported != source);
		CHECK(imported->track_is_compressed(0));
		CHECK_FALSE(imported->track_is_enabled(0));
		CHECK(imported->track_is_imported(0));
		CHECK(imported->track_get_interpolation_type(0) == Animation::INTERPOLATION_NEAREST);
		CHECK_FALSE(imported->track_get_interpolation_loop_wrap(0));
		const Ref<Animation> copy = harness.animation("Copy");
		CHECK_FALSE(copy->track_is_compressed(0));
		CHECK_FALSE(copy->track_is_enabled(0));
		CHECK(copy->track_is_imported(0));
		CHECK(copy->track_get_interpolation_type(0) == Animation::INTERPOLATION_NEAREST);
		CHECK_FALSE(copy->track_get_interpolation_loop_wrap(0));
		CHECK(harness.undo_redo->undo());
		CHECK(harness.player->get_animation_library(StringName()) == original);
		CHECK(harness.version() == before);
	}
	CHECK(FileAccess::get_md5("res://compressed.anim") == source_hash);
	CHECK(FileAccess::get_md5("res://compressed_library.tres") == library_hash);
}

TEST_CASE("[GodotMCP][Animation] Existing library, animation and AudioStream resource references stay on disk unchanged") {
	const String temporary_project = TestUtils::get_temp_path("animation_library_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project) == OK);
	ScopedAnimationProjectPath project_path(temporary_project);
	Ref<Animation> source;
	source.instantiate();
	source->set_length(2);
	Ref<AnimationLibrary> library;
	library.instantiate();
	library->add_animation("Original", source);
	Ref<Animation> embedded;
	embedded.instantiate();
	embedded->set_length(3);
	embedded->set_scene_unique_id("Animation_Embedded");
	embedded->set_path("res://library.tres::Animation_Embedded");
	library->add_animation("Embedded", embedded);
	Ref<AudioStreamWAV> audio;
	audio.instantiate();
	REQUIRE(ResourceSaver::save(source, "res://source.anim") == OK);
	REQUIRE(ResourceSaver::save(library, "res://library.tres") == OK);
	REQUIRE(ResourceSaver::save(audio, "res://stream.tres") == OK);
	const String animation_hash = FileAccess::get_md5("res://source.anim");
	const String library_hash = FileAccess::get_md5("res://library.tres");
	const String audio_hash = FileAccess::get_md5("res://stream.tres");
	AnimationHarness harness;
	const Dictionary result = harness.apply_json(R"([
		{"op":"add_library","library":"assets","resource_path":"res://library.tres"},
		{"op":"create_animation","animation":"Loaded","resource_path":"res://source.anim","properties":{"length":5}},
		{"op":"create_animation","animation":"EmbeddedCopy","resource_path":"res://library.tres::Animation_Embedded","properties":{"length":6}},
		{"op":"set_animation_properties","animation":"assets/Original","properties":{"length":7}},
		{"op":"add_track","animation":"Loaded","type":"audio","path":"."},
		{"op":"insert_key","animation":"Loaded","track":0,"time":0,"value":{"stream":"res://stream.tres","start_offset":0.2,"end_offset":0.4}},
		{"op":"set_key","animation":"Loaded","track":0,"key":0,"value":{"stream":{"resource_path":"res://stream.tres"},"start_offset":0.1}},
		{"op":"add_track","animation":"Loaded","type":"value","path":".:resource"},
		{"op":"insert_key","animation":"Loaded","track":1,"time":0,"value":{"resource_path":"res://stream.tres"}}
	])");
	INFO(harness.error_message.utf8().get_data());
	REQUIRE_FALSE(result.is_empty());
	REQUIRE(harness.error_code == 0);
	CHECK(harness.animation("Loaded")->get_length() == doctest::Approx(5));
	CHECK(harness.animation("EmbeddedCopy")->get_length() == doctest::Approx(6));
	CHECK(embedded->get_length() == doctest::Approx(3));
	CHECK(harness.animation("assets/Original")->get_length() == doctest::Approx(7));
	CHECK(source->get_length() == doctest::Approx(2));
	CHECK(library->get_animation("Original")->get_length() == doctest::Approx(2));
	CHECK(harness.animation("Loaded")->audio_track_get_key_stream(0, 0).is_valid());
	CHECK(harness.animation("Loaded")->audio_track_get_key_end_offset(0, 0) == doctest::Approx(0));
	const Dictionary data = harness.get("Loaded");
	CHECK(Dictionary(Dictionary(Dictionary(_keys(data, 0)[0])["value"])["stream"])["resource_path"] == Variant("res://stream.tres"));
	CHECK(FileAccess::get_md5("res://source.anim") == animation_hash);
	CHECK(FileAccess::get_md5("res://library.tres") == library_hash);
	CHECK(FileAccess::get_md5("res://stream.tres") == audio_hash);
}

} // namespace TestGodotMCPAnimationLibrary

#endif // TOOLS_ENABLED
