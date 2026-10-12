/**************************************************************************/
/*  test_godot_mcp_animation_graph.cpp                                    */
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

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/object/undo_redo.h"
#include "editor/debugger/godot_mcp/godot_mcp_animation_graph.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "scene/animation/animation_blend_space_1d.h"
#include "scene/animation/animation_blend_space_2d.h"
#include "scene/animation/animation_blend_tree.h"
#include "scene/animation/animation_node_state_machine.h"
#include "scene/animation/animation_player.h"
#include "tests/test_macros.h"
#include "tests/test_tools.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GDSCRIPT_ENABLED
#include "core/config/engine.h"
#include "core/object/script_instance.h"
#include "core/object/script_language.h"

#include "modules/gdscript/gdscript.h"
#endif

TEST_FORCE_LINK(test_godot_mcp_animation_graph)

namespace TestGodotMCPAnimationGraph {

static Array _v2(double p_x, double p_y) {
	Array result;
	result.push_back(p_x);
	result.push_back(p_y);
	return result;
}

static Dictionary _typed(const String &p_type, const Variant &p_value) {
	Dictionary result;
	result["__type"] = p_type;
	result["value"] = p_value;
	return result;
}

static Dictionary _properties(const String &p_name, const Variant &p_value) {
	Dictionary result;
	result[p_name] = p_value;
	return result;
}

static Dictionary _op(const String &p_op, const String &p_graph = String()) {
	Dictionary result;
	result["op"] = p_op;
	if (!p_graph.is_empty()) {
		result["graph_path"] = p_graph;
	}
	return result;
}

static Dictionary _root(const String &p_type, const Dictionary &p_properties = Dictionary()) {
	Dictionary result = _op("set_root");
	result["type"] = p_type;
	result["properties"] = p_properties;
	return result;
}

static Dictionary _add(const String &p_name, const String &p_type, const String &p_graph = String(), const Dictionary &p_properties = Dictionary()) {
	Dictionary result = _op("add_node", p_graph);
	result["name"] = p_name;
	result["type"] = p_type;
	result["position"] = _v2(100, 200);
	result["properties"] = p_properties;
	return result;
}

static Dictionary _connect(const String &p_from, const String &p_to, int p_input = 0, const String &p_graph = String()) {
	Dictionary result = _op("connect", p_graph);
	result["from"] = p_from;
	result["to"] = p_to;
	result["input"] = p_input;
	return result;
}

static Dictionary _transition(const String &p_op, const String &p_from, const String &p_to, const Dictionary &p_properties = Dictionary(), const String &p_graph = String()) {
	Dictionary result = _op(p_op, p_graph);
	result["from"] = p_from;
	result["to"] = p_to;
	result["properties"] = p_properties;
	return result;
}

static Dictionary _point(const String &p_name, const Variant &p_position, const String &p_graph = String()) {
	Dictionary result = _op("add_blend_point", p_graph);
	result["name"] = p_name;
	result["position"] = p_position;
	result["properties"] = _properties("animation", "idle");
	return result;
}

static Dictionary _find_child(const Dictionary &p_node, const String &p_name) {
	for (const Variant &value : Array(p_node.get("children", Array()))) {
		const Dictionary child = value;
		if (child.get("name", "") == Variant(p_name)) {
			return child;
		}
	}
	return Dictionary();
}

static bool _has_issue(const Dictionary &p_result, const String &p_key, const String &p_code) {
	for (const Variant &value : Array(p_result.get(p_key, Array()))) {
		if (Dictionary(value).get("code", "") == Variant(p_code)) {
			return true;
		}
	}
	return false;
}

static bool _visible_parameter(AnimationTree *p_tree, const String &p_name) {
	List<PropertyInfo> properties;
	p_tree->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (info.name == StringName(p_name)) {
			return true;
		}
	}
	return false;
}

struct Harness {
	Node *root = memnew(Node);
	AnimationTree *tree = memnew(AnimationTree);
	AnimationPlayer *player = memnew(AnimationPlayer);
	UndoRedo *history = memnew(UndoRedo);
	GodotMCPAnimationGraph graph;
	int code = 0;
	String message;
	Array warnings;

	Harness() {
		root->set_name("Root");
		root->set_scene_file_path("res://mcp_animation_graph_test.tscn");
		tree->set_name("Tree");
		tree->set_active(false);
		root->add_child(tree);
		tree->set_owner(root);
		player->set_name("Player");
		root->add_child(player);
		player->set_owner(root);
		Ref<AnimationLibrary> library;
		library.instantiate();
		for (const StringName &name : { SNAME("idle"), SNAME("fire") }) {
			Ref<Animation> animation;
			animation.instantiate();
			library->add_animation(name, animation);
		}
		player->add_animation_library(StringName(), library);
		tree->set_animation_player(NodePath("../Player"));
	}

	~Harness() {
		memdelete(history);
		memdelete(root);
	}

	String version() const {
		return GodotMCPScenePatch::compute_scene_version(root, 19, history->get_version());
	}

	GodotMCPScenePatch::Context context() const {
		GodotMCPScenePatch::Context result;
		result.root = root;
		result.scene_path = root->get_scene_file_path();
		result.scene_version = version();
		result.history_id = 19;
		result.undo_redo = history;
		return result;
	}

	Dictionary params() const {
		Dictionary result;
		result["node_path"] = "Tree";
		result["expected_scene_path"] = root->get_scene_file_path();
		result["expected_scene_version"] = version();
		return result;
	}

	Dictionary apply(const Array &p_operations, bool p_dry_run = false) {
		Dictionary input = params();
		input["operations"] = p_operations;
		input["dry_run"] = p_dry_run;
		warnings.clear();
		return graph.apply(input, context(), warnings, code, message);
	}

	Dictionary inspect(const Dictionary &p_options = Dictionary()) {
		Dictionary input = params();
		input.merge(p_options, true);
		return graph.inspect(input, context(), code, message);
	}

	Dictionary validate() {
		return graph.validate(params(), context(), code, message);
	}
};

TEST_CASE("[GodotMCP][AnimationGraph] Nested blends, one-shot properties and filters commit in one undo action") {
	Harness h;
	const String before = h.version();
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree", _properties("graph_offset", _typed("Vector2", _v2(25, 30)))));
	operations.push_back(_add("Locomotion", "AnimationNodeBlendTree"));
	operations.push_back(_add("Idle", "AnimationNodeAnimation", "Locomotion", _properties("animation", _typed("StringName", "idle"))));
	operations.push_back(_connect("Idle", "output", 0, "Locomotion"));
	operations.push_back(_add("Fire", "AnimationNodeAnimation", "", _properties("animation", "fire")));
	Dictionary shot_properties;
	shot_properties["fadein_time"] = 0.1;
	shot_properties["fadeout_time"] = 0.2;
	shot_properties["sync"] = true;
	operations.push_back(_add("Shot", "AnimationNodeOneShot", "", shot_properties));
	Dictionary filter = _op("set_filter");
	filter["node_path"] = "Shot";
	filter["enabled"] = true;
	Array paths;
	paths.push_back("Skeleton:Arm");
	paths.push_back("Body:position");
	filter["paths"] = paths;
	operations.push_back(filter);
	operations.push_back(_connect("Locomotion", "Shot", 0));
	operations.push_back(_connect("Fire", "Shot", 1));
	operations.push_back(_connect("Shot", "output"));
	const Dictionary result = h.apply(operations);
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	CHECK(bool(result["applied"]));
	CHECK_FALSE(bool(result["saved"]));
	CHECK(String(result["previous_scene_version"]) == before);
	CHECK(String(result["updated_scene_version"]) == h.version());
	CHECK(h.history->get_history_count() == 1);
	const Ref<AnimationRootNode> applied = h.tree->get_root_animation_node();
	const Ref<AnimationNodeBlendTree> blend = applied;
	REQUIRE(blend.is_valid());
	CHECK(blend->get_graph_offset() == Vector2(25, 30));
	const Ref<AnimationNodeOneShot> shot = blend->get_node("Shot");
	REQUIRE(shot.is_valid());
	CHECK(shot->get_fade_in_time() == doctest::Approx(0.1));
	CHECK(shot->get_fade_out_time() == doctest::Approx(0.2));
	CHECK(shot->is_path_filtered(NodePath("Skeleton:Arm")));
	CHECK(shot->is_filter_enabled());
	const Dictionary inspection = h.inspect();
	CHECK(h.code == 0);
	CHECK_FALSE(bool(inspection["truncated"]));
	const Dictionary inspected_root = inspection["root"];
	CHECK(Array(inspected_root["connections"]).size() == 3);
	const Dictionary nested = _find_child(inspected_root, "Locomotion");
	CHECK(String(_find_child(nested, "Idle")["path"]) == "Locomotion/Idle");
	CHECK(String(_find_child(nested, "Idle")["animation"]) == "idle");
	CHECK_FALSE(JSON::stringify(inspection).is_empty());
	CHECK(bool(h.validate()["valid"]));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node().is_null());
	CHECK(h.version() == before);
	CHECK(h.history->redo());
	CHECK(h.tree->get_root_animation_node() == applied);
}

TEST_CASE("[GodotMCP][AnimationGraph] Dry runs and late invalid operations preserve exact shared roots") {
	Harness h;
	Ref<AnimationNodeBlendTree> original;
	original.instantiate();
	Ref<AnimationNodeAnimation> idle;
	idle.instantiate();
	idle->set_animation("idle");
	original->add_node("Idle", idle);
	original->connect_node("output", 0, "Idle");
	h.tree->set_root_animation_node(original);
	AnimationTree *other = memnew(AnimationTree);
	h.root->add_child(other);
	other->set_root_animation_node(original);
	const String before = h.version();
	Array operations;
	Dictionary edit = _op("set_node_properties");
	edit["node_path"] = "Idle";
	edit["properties"] = _properties("animation", "fire");
	operations.push_back(edit);
	Dictionary rename = _op("rename_node");
	rename["name"] = "Idle";
	rename["new_name"] = "Renamed";
	operations.push_back(rename);
	const Dictionary preview = h.apply(operations, true);
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	CHECK_FALSE(bool(preview["applied"]));
	CHECK(String(preview["updated_scene_version"]) == before);
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(other->get_root_animation_node() == original);
	CHECK(idle->get_animation() == SNAME("idle"));
	CHECK(h.history->get_history_count() == 0);
	operations.push_back(_connect("Renamed", "output", 99));
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.message.contains("operations[2]"));
	CHECK(h.version() == before);
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(idle->get_animation() == SNAME("idle"));
	operations.resize(2);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(h.code == 0);
	CHECK(h.tree->get_root_animation_node() != original);
	CHECK(other->get_root_animation_node() == original);
	CHECK(original->has_node("Idle"));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->redo());
}

TEST_CASE("[GodotMCP][AnimationGraph] State transitions are typed, cloned, renamed and undoable") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeStateMachine"));
	operations.push_back(_add("Idle", "AnimationNodeAnimation", "", _properties("animation", "idle")));
	operations.push_back(_add("Fire", "AnimationNodeAnimation", "", _properties("animation", "fire")));
	operations.push_back(_transition("add_transition", "Start", "Idle"));
	Dictionary settings;
	settings["xfade_time"] = 0.25;
	settings["advance_mode"] = 2;
	settings["switch_mode"] = 1;
	settings["advance_condition"] = _typed("StringName", "ready");
	settings["advance_expression"] = "true";
	settings["priority"] = 3;
	operations.push_back(_transition("add_transition", "Idle", "Fire", settings));
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeStateMachine> original = h.tree->get_root_animation_node();
	const Ref<AnimationNodeStateMachineTransition> transition = original->get_transition(original->find_transition("Idle", "Fire"));
	CHECK(transition->get_xfade_time() == doctest::Approx(0.25));
	CHECK(transition->get_advance_condition() == SNAME("ready"));
	CHECK(bool(h.validate()["valid"]));
	operations.clear();
	operations.push_back(_transition("set_transition", "Idle", "Fire", _properties("xfade_time", 0.5)));
	Dictionary rename = _op("rename_node");
	rename["name"] = "Fire";
	rename["new_name"] = "Attack";
	operations.push_back(rename);
	Dictionary move = _op("move_node");
	move["name"] = "Attack";
	move["position"] = _v2(450, 125);
	operations.push_back(move);
	REQUIRE(!h.apply(operations).is_empty());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeStateMachine> changed = h.tree->get_root_animation_node();
	CHECK(changed != original);
	CHECK(changed->has_transition("Idle", "Attack"));
	CHECK(changed->get_node_position("Attack") == Vector2(450, 125));
	CHECK(transition->get_xfade_time() == doctest::Approx(0.25));
	CHECK(Array(Dictionary(h.inspect()["root"])["transitions"]).size() == 2);
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->redo());
	operations.clear();
	operations.push_back(_transition("remove_transition", "Idle", "Attack"));
	Dictionary remove = _op("remove_node");
	remove["name"] = "Attack";
	operations.push_back(remove);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK_FALSE(Ref<AnimationNodeStateMachine>(h.tree->get_root_animation_node())->has_node("Attack"));
	operations.clear();
	operations.push_back(_transition("add_transition", "Idle", "End", _properties("advance_condition", "bad/path")));
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
}

TEST_CASE("[GodotMCP][AnimationGraph] Named blend points, index insertion and manual triangles survive cloning") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	operations.push_back(_add("Walk", "AnimationNodeBlendSpace1D"));
	operations.push_back(_point("Left", -1.0, "Walk"));
	Dictionary right = _point("Right", 1.0, "Walk");
	right["at_index"] = 0;
	operations.push_back(right);
	operations.push_back(_add("Aim", "AnimationNodeBlendSpace2D"));
	operations.push_back(_point("A", _v2(0, 0), "Aim"));
	operations.push_back(_point("B", _v2(1, 0), "Aim"));
	operations.push_back(_point("C", _v2(0, 1), "Aim"));
	Dictionary topology = _op("set_triangles", "Aim");
	Array triangle;
	triangle.push_back(0);
	triangle.push_back(1);
	triangle.push_back(2);
	Array triangles;
	triangles.push_back(triangle);
	topology["triangles"] = triangles;
	operations.push_back(topology);
	operations.push_back(_connect("Aim", "output"));
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeBlendTree> original = h.tree->get_root_animation_node();
	const Ref<AnimationNodeBlendSpace1D> walk = original->get_node("Walk");
	const Ref<AnimationNodeBlendSpace2D> aim = original->get_node("Aim");
	CHECK(walk->get_blend_point_name(0) == SNAME("Right"));
	CHECK(aim->get_triangle_count() == 1);
	CHECK_FALSE(aim->get_auto_triangles());
	const Dictionary inspection = h.inspect();
	CHECK(Array(_find_child(inspection["root"], "Aim")["triangles"]) == triangles);
	operations.clear();
	Dictionary change = _op("set_blend_point", "Walk");
	change["name"] = "Right";
	change["new_name"] = "Forward";
	change["position"] = 0.75;
	change["properties"] = _properties("animation", "fire");
	operations.push_back(change);
	Dictionary offset = _op("set_node_properties");
	offset["node_path"] = "Aim";
	offset["properties"] = _properties("max_space", _typed("Vector2", _v2(2, 2)));
	operations.push_back(offset);
	REQUIRE(!h.apply(operations).is_empty());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeBlendTree> changed = h.tree->get_root_animation_node();
	const Ref<AnimationNodeBlendSpace1D> new_walk = changed->get_node("Walk");
	const Ref<AnimationNodeBlendSpace2D> new_aim = changed->get_node("Aim");
	CHECK(new_walk->get_blend_point_name(0) == SNAME("Forward"));
	CHECK(new_walk->get_blend_point_position(0) == doctest::Approx(0.75));
	CHECK(walk->get_blend_point_name(0) == SNAME("Right"));
	CHECK(new_aim->get_triangle_count() == 1);
	CHECK(new_aim->get_max_space() == Vector2(2, 2));
	CHECK(new_aim->get_blend_point_node(0) != aim->get_blend_point_node(0));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->redo());
	operations.clear();
	triangles.push_back(triangle);
	topology["triangles"] = triangles;
	operations.push_back(topology);
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	operations.clear();
	Dictionary remove = _op("remove_blend_point", "Aim");
	remove["name"] = "B";
	operations.push_back(remove);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(Ref<AnimationNodeBlendSpace2D>(Ref<AnimationNodeBlendTree>(h.tree->get_root_animation_node())->get_node("Aim"))->get_triangle_count() == 0);
}

TEST_CASE("[GodotMCP][AnimationGraph] Serialized manual triangles replace topology on reload without duplicate diagnostics") {
	Ref<AnimationNodeBlendSpace2D> space;
	space.instantiate();
	space->set_auto_triangles(false);
	for (int i = 0; i < 4; i++) {
		Ref<AnimationNodeAnimation> node;
		node.instantiate();
		space->add_blend_point(node, Vector2(i % 2, i / 2), -1, StringName("point" + itos(i)));
	}
	ErrorDetector detector;
	const PackedInt32Array first{ 0, 1, 2 };
	const PackedInt32Array second{ 0, 2, 3 };
	space->set("triangles", first);
	CHECK(space->get_triangle_count() == 1);
	space->set("triangles", first);
	CHECK_FALSE(detector.has_error);
	CHECK(space->get_triangle_count() == 1);
	space->set("triangles", second);
	CHECK(space->get_triangle_count() == 1);
	CHECK(PackedInt32Array(space->get("triangles")) == second);
	Ref<AnimationNodeBlendSpace2D> reloaded;
	reloaded.instantiate();
	REQUIRE(reloaded->copy_from(space) == OK);
	REQUIRE(reloaded->copy_from(space) == OK);
	CHECK(reloaded->get_triangle_count() == 1);
	CHECK(PackedInt32Array(reloaded->get("triangles")) == second);
	space->set("triangles", PackedInt32Array());
	CHECK(space->get_triangle_count() == 0);
	CHECK_FALSE(detector.has_error);
}

TEST_CASE("[GodotMCP][AnimationGraph] Transition input_count uses public APIs and survives a second transaction") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	Dictionary properties;
	properties["input_1/name"] = "Attack"; // Intentionally before input_count.
	properties["input_count"] = 2;
	properties["input_0/name"] = "Idle";
	properties["input_1/auto_advance"] = true;
	operations.push_back(_add("Switch", "AnimationNodeTransition", "", properties));
	operations.push_back(_add("IdleClip", "AnimationNodeAnimation", "", _properties("animation", "idle")));
	operations.push_back(_add("FireClip", "AnimationNodeAnimation", "", _properties("animation", "fire")));
	operations.push_back(_connect("IdleClip", "Switch", 0));
	operations.push_back(_connect("FireClip", "Switch", 1));
	operations.push_back(_connect("Switch", "output"));
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeBlendTree> original = h.tree->get_root_animation_node();
	operations.clear();
	Dictionary change = _op("set_node_properties");
	change["node_path"] = "Switch";
	change["properties"] = _properties("xfade_time", 0.2);
	operations.push_back(change);
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeBlendTree> changed = h.tree->get_root_animation_node();
	const Ref<AnimationNodeTransition> transition = changed->get_node("Switch");
	CHECK(transition->get_input_count() == 2);
	CHECK(transition->get_input_name(1) == "Attack");
	CHECK(transition->is_input_set_as_auto_advance(1));
	CHECK(changed->get_node_connection_array("Switch")->size() == 2);
	CHECK(bool(h.validate()["valid"]));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	operations.clear();
	change["properties"] = _properties("input_count", 65);
	operations.push_back(change);
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
}

TEST_CASE("[GodotMCP][AnimationGraph] Deserialized shared transitions select and process their first input per instance") {
	Harness h;
	Ref<AnimationNodeBlendTree> root;
	root.instantiate();
	Ref<AnimationNodeTransition> loaded;
	loaded.instantiate();
	// Match ResourceLoader's path, which does not assign editor-only input_count.
	loaded->set("input_0/name", "Idle");
	loaded->set("input_1/name", "Fire");
	REQUIRE(loaded->get_input_count() == 2);
	root->add_node("Switch", loaded);
	for (const StringName &clip : { SNAME("idle"), SNAME("fire") }) {
		Ref<AnimationNodeAnimation> node;
		node.instantiate();
		node->set_animation(clip);
		root->add_node(clip, node);
	}
	root->connect_node("Switch", 0, "idle");
	root->connect_node("Switch", 1, "fire");
	root->connect_node("output", 0, "Switch");
	const Ref<AnimationLibrary> library = h.player->get_animation_library(StringName());
	REQUIRE(h.tree->add_animation_library(StringName(), library) == OK);
	h.tree->set_root_animation_node(root);
	AnimationTree *peer = memnew(AnimationTree);
	peer->set_name("Peer");
	peer->set_active(false);
	h.root->add_child(peer);
	peer->set_owner(h.root);
	REQUIRE(peer->add_animation_library(StringName(), library) == OK);
	peer->set_root_animation_node(root);
	ErrorDetector detector;
	for (AnimationTree *tree : { h.tree, peer }) {
		tree->advance(1.0 / 60.0);
		CHECK(int64_t(tree->get("parameters/Switch/current_index")) == 0);
		CHECK(String(tree->get("parameters/Switch/current_state")) == "Idle");
		CHECK(double(tree->get("parameters/idle/current_position")) > 0);
	}
	CHECK_FALSE(detector.has_error);
}

TEST_CASE("[GodotMCP][AnimationGraph] Stale versions, containment, identity and numeric writes are rejected") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	Dictionary input = h.params();
	input["operations"] = operations;
	input["expected_scene_version"] = "stale";
	CHECK(h.graph.apply(input, h.context(), h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::CONFLICT);
	input = h.params();
	input["operations"] = operations;
	input["expected_scene_path"] = "res://other.tscn";
	CHECK(h.graph.apply(input, h.context(), h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::CONFLICT);
	for (const String &path : { String("../Tree"), String("/root/Tree"), String("Tree:tree_root") }) {
		input = h.params();
		input["node_path"] = path;
		CHECK(h.graph.inspect(input, h.context(), h.code, h.message).is_empty());
		CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	}
	REQUIRE(!h.apply(operations).is_empty());
	const Ref<AnimationRootNode> original = h.tree->get_root_animation_node();
	for (const String &property : { String("script"), String("nodes/output/node"), String("resource_path"), String("resource_name"), String("filters") }) {
		operations.clear();
		Dictionary change = _op("set_node_properties");
		change["properties"] = _properties(property, Variant());
		operations.push_back(change);
		CHECK(h.apply(operations).is_empty());
		CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(h.tree->get_root_animation_node() == original);
	}
	operations.clear();
	Dictionary bad = _add("Bad", "AnimationNodeOneShot", "", _properties("fadein_time", -1.0));
	operations.push_back(bad);
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	bad["properties"] = _properties("fadein_time", INFINITY);
	operations[0] = bad;
	CHECK(h.apply(operations).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	Dictionary options;
	options["max_nodes"] = 2001;
	CHECK(h.inspect(options).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
}

TEST_CASE("[GodotMCP][AnimationGraph] Validation diagnoses missing clips, ports, cycles, reachability and track targets") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	operations.push_back(_add("Missing", "AnimationNodeAnimation", "", _properties("animation", "does_not_exist")));
	operations.push_back(_connect("Missing", "output"));
	REQUIRE(!h.apply(operations).is_empty());
	Dictionary validation = h.validate();
	CHECK_FALSE(bool(validation["valid"]));
	CHECK(_has_issue(validation, "errors", "missing_clip"));
	operations.clear();
	operations.push_back(_add("A", "AnimationNodeTimeScale"));
	operations.push_back(_add("B", "AnimationNodeTimeScale"));
	operations.push_back(_connect("A", "B"));
	operations.push_back(_connect("B", "A"));
	const Ref<AnimationRootNode> before = h.tree->get_root_animation_node();
	CHECK(h.apply(operations).is_empty());
	CHECK(h.message.contains("cycle"));
	CHECK(h.tree->get_root_animation_node() == before);
	Ref<AnimationNodeBlendTree> cyclic;
	cyclic.instantiate();
	Ref<AnimationNodeTimeScale> a;
	a.instantiate();
	Ref<AnimationNodeTimeScale> b;
	b.instantiate();
	cyclic->add_node("A", a);
	cyclic->add_node("B", b);
	cyclic->connect_node("B", 0, "A");
	cyclic->connect_node("A", 0, "B");
	h.tree->set_root_animation_node(cyclic);
	validation = h.validate();
	CHECK(_has_issue(validation, "errors", "blend_cycle"));
	CHECK(_has_issue(validation, "errors", "missing_input"));
	operations.clear();
	operations.push_back(_root("AnimationNodeStateMachine"));
	operations.push_back(_add("Idle", "AnimationNodeAnimation", "", _properties("animation", "idle")));
	REQUIRE(!h.apply(operations).is_empty());
	validation = h.validate();
	CHECK(_has_issue(validation, "warnings", "unreachable_state"));
	const Ref<Animation> clip = h.player->get_animation("idle");
	const int track = clip->add_track(Animation::TYPE_VALUE);
	clip->track_set_path(track, NodePath("MissingNode:position"));
	validation = h.validate();
	CHECK(_has_issue(validation, "warnings", "missing_track_target"));
	h.tree->set_animation_player(NodePath("../MissingPlayer"));
	CHECK(_has_issue(h.validate(), "errors", "invalid_player_binding"));
}

TEST_CASE("[GodotMCP][AnimationGraph] Filter replacement, disconnect and inspection bounds are explicit") {
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	operations.push_back(_add("Blend", "AnimationNodeBlend2"));
	Dictionary filter = _op("set_filter");
	filter["node_path"] = "Blend";
	filter["enabled"] = true;
	Array paths;
	paths.push_back("Body:position");
	filter["paths"] = paths;
	operations.push_back(filter);
	operations.push_back(_connect("Blend", "output"));
	REQUIRE(!h.apply(operations).is_empty());
	operations.clear();
	paths.clear();
	paths.push_back("Body:rotation");
	filter["paths"] = paths;
	operations.push_back(filter);
	Dictionary disconnect = _op("disconnect");
	disconnect["to"] = "output";
	disconnect["input"] = 0;
	operations.push_back(disconnect);
	REQUIRE(!h.apply(operations).is_empty());
	const Ref<AnimationNodeBlendTree> root = h.tree->get_root_animation_node();
	const Ref<AnimationNodeBlend2> blend = root->get_node("Blend");
	CHECK_FALSE(blend->is_path_filtered(NodePath("Body:position")));
	CHECK(blend->is_path_filtered(NodePath("Body:rotation")));
	CHECK((*root->get_node_connection_array("output"))[0] == StringName());
	Dictionary options;
	options["max_nodes"] = 1;
	options["include_properties"] = false;
	const Dictionary inspection = h.inspect(options);
	CHECK(bool(inspection["truncated"]));
	CHECK(int(inspection["node_count"]) == 1);
	CHECK_FALSE(Dictionary(inspection["root"]).has("properties"));
}

TEST_CASE("[GodotMCP][AnimationGraph] Published decimal integer wrappers configure native enum properties") {
	Harness h;
	REQUIRE_FALSE(h.apply(Array{ _root("AnimationNodeBlendSpace1D", _properties("blend_mode", _typed("int", "1"))) }).is_empty());
	const Ref<AnimationNodeBlendSpace1D> original = h.tree->get_root_animation_node();
	REQUIRE(original.is_valid());
	CHECK(original->get_blend_mode() == AnimationNodeBlendSpace1D::BLEND_MODE_DISCRETE);
	const String before = h.version();
	for (const String &decimal : { String("2147483648"), String("-2147483649"), String("1.5"), String("1e2"), String(" 1"), String("01") }) {
		Dictionary update = _op("set_node_properties");
		update["properties"] = _properties("blend_mode", _typed("int", decimal));
		CHECK(h.apply(Array{ update }).is_empty());
		CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(h.tree->get_root_animation_node() == original);
		CHECK(h.version() == before);
	}
}

TEST_CASE("[GodotMCP][AnimationGraph] Deep clones retain curves, hidden settings and disjoint blend-space ranges") {
	Harness h;
	Ref<AnimationNodeBlendTree> original;
	original.instantiate();
	Ref<AnimationNodeOneShot> shot;
	shot.instantiate();
	Ref<Curve> curve;
	curve.instantiate();
	curve->add_point(Vector2(0, 0));
	curve->add_point(Vector2(1, 1));
	shot->set_fade_in_curve(curve);
	shot->set_fade_out_curve(curve);
	original->add_node("Shot", shot);
	Ref<AnimationNodeAnimation> clip;
	clip.instantiate();
	clip->set_animation("idle");
	clip->set_timeline_length(5.0);
	clip->set_use_custom_timeline(false); // Its fields are hidden, not lost.
	original->add_node("Clip", clip);
	h.tree->set_root_animation_node(original);
	Array operations;
	Dictionary change = _op("set_node_properties");
	change["node_path"] = "Clip";
	change["properties"] = _properties("use_custom_timeline", true);
	operations.push_back(change);
	Dictionary limits;
	limits["max_space"] = -2.0;
	limits["min_space"] = -10.0;
	limits["sync_mode"] = 1;
	operations.push_back(_add("Space", "AnimationNodeBlendSpace1D", "", limits));
	Dictionary custom;
	custom["use_custom_timeline"] = true;
	custom["timeline_length"] = 2.5;
	custom["loop_mode"] = 1;
	custom["animation"] = "fire";
	operations.push_back(_add("Custom", "AnimationNodeAnimation", "", custom));
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	const Ref<AnimationNodeBlendTree> changed = h.tree->get_root_animation_node();
	const Ref<AnimationNodeOneShot> changed_shot = changed->get_node("Shot");
	CHECK(changed_shot->get_fade_in_curve().is_valid());
	CHECK(changed_shot->get_fade_in_curve() != curve);
	CHECK(changed_shot->get_fade_in_curve() == changed_shot->get_fade_out_curve());
	CHECK(changed_shot->get_fade_in_curve()->get_point_count() == 2);
	const Ref<AnimationNodeAnimation> changed_clip = changed->get_node("Clip");
	CHECK(changed_clip->is_using_custom_timeline());
	CHECK(changed_clip->get_timeline_length() == 5.0);
	CHECK_FALSE(clip->is_using_custom_timeline());
	const Ref<AnimationNodeAnimation> custom_clip = changed->get_node("Custom");
	CHECK(custom_clip->get_timeline_length() == 2.5);
	const Ref<AnimationNodeBlendSpace1D> space = changed->get_node("Space");
	CHECK(space->get_min_space() == -10.0);
	CHECK(space->get_max_space() == -2.0);
	CHECK(space->get_sync_mode() == AnimationNodeBlendSpace1D::SYNC_MODE_INDEPENDENT);
	operations.clear();
	change["node_path"] = "Space";
	limits["min_space"] = 5.0;
	limits["max_space"] = 10.0;
	change["properties"] = limits;
	operations.push_back(change);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(Ref<AnimationNodeBlendSpace1D>(Ref<AnimationNodeBlendTree>(h.tree->get_root_animation_node())->get_node("Space"))->get_min_space() == 5.0);
	CHECK(space->get_min_space() == -10.0);
}

TEST_CASE("[GodotMCP][AnimationGraph] Automatic triangles are inspected without forcing live deferred updates") {
	Harness h;
	Ref<AnimationNodeBlendSpace2D> original;
	original.instantiate();
	for (int i = 0; i < 3; i++) {
		Ref<AnimationNodeAnimation> clip;
		clip.instantiate();
		clip->set_animation("idle");
		original->add_blend_point(clip, Vector2(i == 1 ? 1 : 0, i == 2 ? 1 : 0), -1, "Point" + itos(i));
	}
	h.tree->set_root_animation_node(original);
	const int before_triangles = original->get_triangle_count();
	const String before = h.version();
	const Dictionary inspection = h.inspect();
	REQUIRE(h.code == 0);
	CHECK(Array(Dictionary(inspection["root"])["triangles"]).size() == 1);
	CHECK(bool(Dictionary(inspection["root"])["triangles_computed"]));
	CHECK(original->get_triangle_count() == before_triangles);
	CHECK(h.version() == before);
	CHECK(bool(h.validate()["valid"]));
	Array operations;
	Dictionary change = _op("set_node_properties");
	change["properties"] = _properties("max_space", _v2(2, 2));
	operations.push_back(change);
	CHECK_FALSE(h.apply(operations, true).is_empty());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(original->get_triangle_count() == before_triangles);
	CHECK(h.version() == before);
}

TEST_CASE("[GodotMCP][AnimationGraph] Existing virtual Start and End nodes are cloned without ClassDB instantiation") {
	Harness h;
	CHECK_FALSE(ClassDB::can_instantiate("AnimationNodeStartState"));
	CHECK_FALSE(ClassDB::can_instantiate("AnimationNodeEndState"));
	Ref<AnimationNodeStateMachine> original;
	original.instantiate();
	original->get_node("Start")->set_name("StartLabel");
	original->get_node("Start")->set_meta("label", "entry");
	original->get_node("End")->set_meta("label", "exit");
	original->set_meta("start_ref", original->get_node("Start"));
	original->set_node_position("Start", Vector2(11, 22));
	original->set_node_position("End", Vector2(33, 44));
	h.tree->set_root_animation_node(original);
	Array operations;
	operations.push_back(_add("Idle", "AnimationNodeAnimation", "", _properties("animation", "idle")));
	operations.push_back(_transition("add_transition", "Start", "Idle"));
	ErrorDetector detector;
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	CHECK_FALSE(detector.has_error);
	const Ref<AnimationNodeStateMachine> cloned = h.tree->get_root_animation_node();
	CHECK(cloned->get_node("Start") != original->get_node("Start"));
	CHECK(cloned->get_node("End") != original->get_node("End"));
	CHECK(cloned->get_node("Start")->get_name() == "StartLabel");
	CHECK(cloned->get_node("Start")->get_meta("label") == Variant("entry"));
	CHECK(cloned->get_node("End")->get_meta("label") == Variant("exit"));
	CHECK(Ref<AnimationNode>(cloned->get_meta("start_ref")) == cloned->get_node("Start"));
	CHECK(cloned->get_node_position("Start") == Vector2(11, 22));
	CHECK(cloned->get_node_position("End") == Vector2(33, 44));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->redo());
	CHECK(h.tree->get_root_animation_node() == cloned);
	CHECK_FALSE(detector.has_error);
}

TEST_CASE("[GodotMCP][AnimationGraph] Ordered renames preserve scalar parameters and replacements reset stale paths") {
	Harness h;
	Ref<AnimationNodeBlendTree> original;
	original.instantiate();
	Ref<AnimationNodeTimeScale> speed;
	speed.instantiate();
	Ref<AnimationNodeTimeScale> other;
	other.instantiate();
	original->add_node("Speed", speed);
	original->add_node("Other", other);
	h.tree->set_root_animation_node(original);
	h.tree->set("parameters/Speed/scale", 2.0);
	h.tree->set("parameters/Other/scale", 7.0);
	Array operations;
	Dictionary rename = _op("rename_node");
	rename["name"] = "Speed";
	rename["new_name"] = "Intermediate";
	operations.push_back(rename);
	rename = rename.duplicate();
	rename["name"] = "Intermediate";
	rename["new_name"] = "Fast";
	operations.push_back(rename);
	REQUIRE(!h.apply(operations, true).is_empty());
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(2.0));
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->get_history_count() == 0);
	operations.push_back(_connect("Fast", "output", 99));
	CHECK(h.apply(operations).is_empty());
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(2.0));
	CHECK(h.tree->get_root_animation_node() == original);
	operations.resize(2);
	REQUIRE(!h.apply(operations).is_empty());
	REQUIRE(h.code == 0);
	const Ref<AnimationRootNode> renamed = h.tree->get_root_animation_node();
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(2.0));
	CHECK(h.tree->get("parameters/Other/scale") == Variant(7.0));
	CHECK_FALSE(_visible_parameter(h.tree, "parameters/Speed/scale"));
	CHECK(h.history->get_history_count() == 1);
	h.tree->set("parameters/Fast/scale", 4.0);
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(2.0));
	CHECK_FALSE(_visible_parameter(h.tree, "parameters/Fast/scale"));
	CHECK(h.history->redo());
	CHECK(h.tree->get_root_animation_node() == renamed);
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(2.0));
	operations.clear();
	Dictionary remove = _op("remove_node");
	remove["name"] = "Fast";
	operations.push_back(remove);
	operations.push_back(_add("Fast", "AnimationNodeTimeScale"));
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(1.0));
	CHECK(h.tree->get("parameters/Other/scale") == Variant(7.0));
	CHECK(h.history->undo());
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(2.0));
	CHECK(h.history->redo());
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(1.0));
	operations.clear();
	operations.push_back(_root("AnimationNodeBlendTree"));
	operations.push_back(_add("Other", "AnimationNodeTimeScale"));
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(h.tree->get("parameters/Other/scale") == Variant(1.0));
	CHECK(h.history->undo());
	CHECK(h.tree->get("parameters/Other/scale") == Variant(7.0));
}

TEST_CASE("[GodotMCP][AnimationGraph] Named 1D and 2D blend-point subtree renames preserve parameter values") {
	Harness h;
	Ref<AnimationNodeBlendTree> nested;
	nested.instantiate();
	Ref<AnimationNodeTimeScale> speed;
	speed.instantiate();
	nested->add_node("Speed", speed);
	Ref<AnimationRootNode> original;
	Variant position;
	SUBCASE("1D") {
		Ref<AnimationNodeBlendSpace1D> space;
		space.instantiate();
		space->add_blend_point(nested, 0.0, -1, "Walk");
		original = space;
		position = 0.75;
	}
	SUBCASE("2D") {
		Ref<AnimationNodeBlendSpace2D> space;
		space.instantiate();
		space->add_blend_point(nested, Vector2(), -1, "Walk");
		original = space;
		position = Vector2(0.25, 0.5);
	}
	h.tree->set_root_animation_node(original);
	h.tree->set("parameters/Walk/Speed/scale", 3.5);
	h.tree->set("parameters/blend_position", position);
	Dictionary rename = _op("set_blend_point");
	rename["name"] = "Walk";
	rename["new_name"] = "Run";
	Array operations;
	operations.push_back(rename);
	REQUIRE(!h.apply(operations).is_empty());
	INFO(h.message.utf8().get_data());
	REQUIRE(h.code == 0);
	CHECK(h.tree->get("parameters/Run/Speed/scale") == Variant(3.5));
	CHECK(h.tree->get("parameters/blend_position") == position);
	CHECK_FALSE(_visible_parameter(h.tree, "parameters/Walk/Speed/scale"));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.tree->get("parameters/Walk/Speed/scale") == Variant(3.5));
	CHECK(h.tree->get("parameters/blend_position") == position);
	CHECK(h.history->redo());
	CHECK(h.tree->get("parameters/Run/Speed/scale") == Variant(3.5));
}

TEST_CASE("[GodotMCP][AnimationGraph] Condition names containing slashes are remapped without copying playback handles") {
	Harness h;
	Ref<AnimationNodeBlendTree> original;
	original.instantiate();
	Ref<AnimationNodeStateMachine> machine;
	machine.instantiate();
	for (const StringName &name : { SNAME("Idle"), SNAME("Fire") }) {
		Ref<AnimationNodeAnimation> clip;
		clip.instantiate();
		clip->set_animation(name == SNAME("Idle") ? SNAME("idle") : SNAME("fire"));
		machine->add_node(name, clip);
	}
	Ref<AnimationNodeStateMachineTransition> transition;
	transition.instantiate();
	transition->set_advance_condition("run");
	machine->add_transition("Idle", "Fire", transition);
	original->add_node("States", machine);
	h.tree->set_root_animation_node(original);
	h.tree->set("parameters/States/conditions/run", true);
	const Ref<AnimationNodeStateMachinePlayback> playback = h.tree->get("parameters/States/playback");
	REQUIRE(playback.is_valid());
	Dictionary rename = _op("rename_node");
	rename["name"] = "States";
	rename["new_name"] = "Renamed";
	Array operations;
	operations.push_back(rename);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(h.tree->get("parameters/Renamed/conditions/run") == Variant(true));
	const Ref<AnimationNodeStateMachinePlayback> renamed_playback = h.tree->get("parameters/Renamed/playback");
	CHECK(renamed_playback.is_valid());
	CHECK(renamed_playback != playback);
	CHECK(h.history->undo());
	CHECK(h.tree->get("parameters/States/conditions/run") == Variant(true));
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->redo());
	CHECK(h.tree->get("parameters/Renamed/conditions/run") == Variant(true));
}

TEST_CASE("[GodotMCP][AnimationGraph] Preflight detects cyclic hierarchies before scene version enumeration") {
	Harness h;
	String message;
	CHECK(GodotMCPAnimationGraph::preflight_scene(h.root, message)); // Null roots can be authored.
	const GodotMCPScenePatch::Context captured = h.context();
	Dictionary input = h.params();
	Array operations;
	operations.push_back(_root("AnimationNodeBlendTree"));
	input["operations"] = operations;
	Ref<AnimationNodeBlendTree> cyclic;
	cyclic.instantiate();
	cyclic->set_block_signals(true);
	cyclic->add_node("Self", cyclic);
	cyclic->set_block_signals(false);
	SUBCASE("target tree") {
		h.tree->set_root_animation_node(cyclic);
	}
	SUBCASE("another contained tree") {
		AnimationTree *other = memnew(AnimationTree);
		other->set_name("OtherTree");
		h.root->add_child(other);
		other->set_root_animation_node(cyclic);
	}
	ErrorDetector detector;
	CHECK_FALSE(GodotMCPAnimationGraph::preflight_scene(h.root, message));
	CHECK(message.contains("cyclic"));
	CHECK(h.graph.apply(input, captured, h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.message.contains("cyclic"));
	CHECK(h.history->get_history_count() == 0);
	if (h.tree->get_root_animation_node() == cyclic) {
		Dictionary read;
		read["node_path"] = "Tree";
		const Dictionary validation = h.graph.validate(read, captured, h.code, h.message);
		CHECK(h.code == 0);
		CHECK(_has_issue(validation, "errors", "hierarchy_cycle"));
		const Dictionary inspection = h.graph.inspect(read, captured, h.code, h.message);
		CHECK(bool(inspection["truncated"]));
		CHECK(bool(inspection["parameters_partial"]));
	}
	CHECK_FALSE(detector.has_error);
	// Break only the disposable fixture cycle before releasing its resources.
	cyclic->set_block_signals(true);
	cyclic->remove_node("Self");
	cyclic->set_block_signals(false);
	CHECK(GodotMCPAnimationGraph::preflight_scene(h.root, message));
}

TEST_CASE("[GodotMCP][AnimationGraph] Grouped boundary modes do not incorrectly mark entry states unreachable") {
	Harness h;
	Ref<AnimationNodeStateMachine> parent;
	parent.instantiate();
	Ref<AnimationNodeStateMachine> group;
	group.instantiate();
	group->set_state_machine_type(AnimationNodeStateMachine::STATE_MACHINE_TYPE_GROUPED);
	for (const StringName &name : { SNAME("Idle"), SNAME("Fire") }) {
		Ref<AnimationNodeAnimation> clip;
		clip.instantiate();
		clip->set_animation(name == SNAME("Idle") ? SNAME("idle") : SNAME("fire"));
		group->add_node(name, clip);
	}
	for (const Pair<StringName, StringName> &edge : { Pair<StringName, StringName>("Start", "Idle"), Pair<StringName, StringName>("Idle", "Fire"), Pair<StringName, StringName>("Idle", "End") }) {
		Ref<AnimationNodeStateMachineTransition> transition;
		transition.instantiate();
		transition->set_advance_mode(AnimationNodeStateMachineTransition::ADVANCE_MODE_DISABLED);
		group->add_transition(edge.first, edge.second, transition);
	}
	parent->add_node("Group", group);
	Ref<AnimationNodeStateMachineTransition> entry;
	entry.instantiate();
	parent->add_transition("Start", "Group", entry);
	h.tree->set_root_animation_node(parent);
	const Dictionary result = h.validate();
	CHECK(bool(result["partial"]));
	CHECK(_has_issue(result, "warnings", "partial_grouped_reachability"));
	bool idle_unreachable = false;
	bool fire_unreachable = false;
	for (const Variant &value : Array(result["warnings"])) {
		const Dictionary warning = value;
		if (warning["code"] == Variant("unreachable_state")) {
			idle_unreachable = idle_unreachable || String(warning["message"]).ends_with("Idle");
			fire_unreachable = fire_unreachable || String(warning["message"]).ends_with("Fire");
		}
	}
	CHECK_FALSE(idle_unreachable);
	CHECK(fire_unreachable); // Non-boundary disabled edges are still disabled.
}

TEST_CASE("[GodotMCP][AnimationGraph] Missing graph children return structured errors without native error logs") {
	Harness h;
	for (const String &type : { String("AnimationNodeBlendTree"), String("AnimationNodeStateMachine"), String("AnimationNodeBlendSpace1D"), String("AnimationNodeBlendSpace2D") }) {
		Array setup;
		setup.push_back(_root(type));
		REQUIRE(!h.apply(setup).is_empty());
		Array operations;
		Dictionary change = _op("set_node_properties");
		change["node_path"] = "Missing/Child";
		change["properties"] = Dictionary();
		operations.push_back(change);
		ErrorDetector detector;
		CHECK(h.apply(operations).is_empty());
		CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(h.message.contains("does not exist"));
		CHECK_FALSE(detector.has_error);
	}
}

TEST_CASE("[GodotMCP][AnimationGraph] Inspection serializes freed object parameter handles as null") {
	Harness h;
	Ref<AnimationNodeStateMachine> machine;
	machine.instantiate();
	h.tree->set_root_animation_node(machine);
	const GodotMCPScenePatch::Context captured = h.context();
	AnimationNodeInstance *instance = h.tree->get_node_instance_by_path_or_null("parameters/");
	REQUIRE(instance != nullptr);
	Variant **playback = instance->property_ptrs.getptr(SNAME("playback"));
	REQUIRE(playback != nullptr);
	const Variant original = **playback;
	Node *temporary = memnew(Node);
	**playback = temporary;
	memdelete(temporary);
	Dictionary input;
	input["node_path"] = "Tree";
	const Dictionary result = h.graph.inspect(input, captured, h.code, h.message);
	REQUIRE(h.code == 0);
	CHECK(Dictionary(result["parameters"])["parameters/playback"].get_type() == Variant::NIL);
	**playback = original;
}

#ifdef MODULE_GDSCRIPT_ENABLED
struct ControllerEditorScope {
	bool previous_editor_hint = Engine::get_singleton()->is_editor_hint();
	bool previous_scripting_enabled = ScriptServer::is_scripting_enabled();

	ControllerEditorScope() {
		Engine::get_singleton()->set_editor_hint(true);
		ScriptServer::set_scripting_enabled(false);
	}

	~ControllerEditorScope() {
		ScriptServer::set_scripting_enabled(previous_scripting_enabled);
		Engine::get_singleton()->set_editor_hint(previous_editor_hint);
	}
};

TEST_CASE("[GodotMCP][AnimationGraph] Ordinary controller scripts survive graph creation inspection dry runs and undo") {
	GDScriptLanguage::get_singleton()->init();
	ControllerEditorScope editor_scope;
	Harness h;
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("extends AnimationTree\n@export var controller_speed: float = 3.5\n@export var guarded_value: int = 4:\n\tget:\n\t\tset_meta(\"_getter_calls\", get_meta(\"_getter_calls\", 0) + 1)\n\t\treturn 4\nfunc _get(_property):\n\tset_meta(\"_dynamic_get_calls\", get_meta(\"_dynamic_get_calls\", 0) + 1)\n\treturn null\nfunc _set(_property, _value):\n\tset_meta(\"_setter_calls\", get_meta(\"_setter_calls\", 0) + 1)\n\treturn false\nfunc _get_property_list():\n\tset_meta(\"_property_list_calls\", get_meta(\"_property_list_calls\", 0) + 1)\n\treturn []\n");
	REQUIRE(script->reload() == OK);
	CHECK_FALSE(script->is_tool());
	h.tree->set_script(script);
	REQUIRE(h.tree->get_script_instance() != nullptr);
	REQUIRE(h.tree->get_script_instance()->is_placeholder());
	h.tree->set("controller_speed", 7.25);
	for (const StringName &name : { SNAME("_getter_calls"), SNAME("_dynamic_get_calls"), SNAME("_setter_calls"), SNAME("_property_list_calls") }) {
		h.tree->set_meta(name, 0);
	}
	Array create;
	create.push_back(_root("AnimationNodeBlendTree"));
	create.push_back(_add("Idle", "AnimationNodeAnimation", "", _properties("animation", "idle")));
	create.push_back(_add("Speed", "AnimationNodeTimeScale"));
	create.push_back(_connect("Idle", "Speed"));
	create.push_back(_connect("Speed", "output"));
	REQUIRE(!h.apply(create).is_empty());
	CHECK(h.code == 0);
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_speed") == Variant(7.25));
	h.tree->set("parameters/Speed/scale", 2.75);
	const Ref<AnimationRootNode> original = h.tree->get_root_animation_node();
	const String original_version = h.version();
	Dictionary input;
	input["node_path"] = "Tree";
	Dictionary inspected = h.graph.inspect(input, h.context(), h.code, h.message);
	REQUIRE(h.code == 0);
	CHECK_FALSE(bool(inspected["parameters_partial"]));
	CHECK_FALSE(bool(inspected["truncated"]));
	CHECK(Dictionary(inspected["parameters"])["parameters/Speed/scale"] == Variant(2.75));
	CHECK(h.version() == original_version);
	Dictionary rename = _op("rename_node");
	rename["name"] = "Speed";
	rename["new_name"] = "Fast";
	Array operations;
	operations.push_back(rename);
	const Dictionary dry = h.apply(operations, true);
	REQUIRE(h.code == 0);
	CHECK(bool(dry["dry_run"]));
	CHECK_FALSE(bool(dry["applied"]));
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->get_history_count() == 1);
	CHECK(h.version() == original_version);
	Array invalid = operations.duplicate(true);
	invalid.push_back(_connect("missing", "output"));
	CHECK(h.apply(invalid).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.history->get_history_count() == 1);
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(2.75));
	CHECK(h.version() == original_version);
	REQUIRE(!h.apply(operations).is_empty());
	CHECK(h.history->get_history_count() == 2);
	const Ref<AnimationRootNode> renamed = h.tree->get_root_animation_node();
	CHECK(renamed != original);
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(2.75));
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_speed") == Variant(7.25));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(2.75));
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_speed") == Variant(7.25));
	CHECK(h.history->redo());
	CHECK(h.tree->get_root_animation_node() == renamed);
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(2.75));
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_speed") == Variant(7.25));
	for (const StringName &name : { SNAME("_getter_calls"), SNAME("_dynamic_get_calls"), SNAME("_setter_calls"), SNAME("_property_list_calls") }) {
		CHECK(h.tree->get_meta(name) == Variant(0));
	}
}

TEST_CASE("[GodotMCP][AnimationGraph] A controller attached after authoring preserves parameters during undo and redo") {
	GDScriptLanguage::get_singleton()->init();
	ControllerEditorScope editor_scope;
	Harness h;
	Array create;
	create.push_back(_root("AnimationNodeBlendTree"));
	create.push_back(_add("Speed", "AnimationNodeTimeScale"));
	REQUIRE(!h.apply(create).is_empty());
	h.tree->set("parameters/Speed/scale", 4.0);
	Dictionary rename = _op("rename_node");
	rename["name"] = "Speed";
	rename["new_name"] = "Fast";
	Array operations;
	operations.push_back(rename);
	REQUIRE(!h.apply(operations).is_empty());
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("extends AnimationTree\n@export var controller_tag: String = \"kept\"\n");
	REQUIRE(script->reload() == OK);
	h.tree->set_script(script);
	REQUIRE(h.tree->get_script_instance() != nullptr);
	REQUIRE(h.tree->get_script_instance()->is_placeholder());
	h.tree->set("controller_tag", "attached-later");
	CHECK(h.history->undo());
	CHECK(h.tree->get("parameters/Speed/scale") == Variant(4.0));
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_tag") == Variant("attached-later"));
	CHECK(h.history->redo());
	CHECK(h.tree->get("parameters/Fast/scale") == Variant(4.0));
	CHECK(h.tree->get_script() == Variant(script));
	CHECK(h.tree->get("controller_tag") == Variant("attached-later"));
}

TEST_CASE("[GodotMCP][AnimationGraph] Editor-executing authoring is rejected and native undo bypasses a later tool setter") {
	GDScriptLanguage::get_singleton()->init();
	Harness h;
	Array operations;
	operations.push_back(_root("AnimationNodeAnimation", _properties("animation", "idle")));
	REQUIRE(!h.apply(operations).is_empty());
	const Ref<AnimationRootNode> applied = h.tree->get_root_animation_node();
	const GodotMCPScenePatch::Context captured = h.context();
	Dictionary input = h.params();
	input["operations"] = operations;
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("@tool\nextends AnimationTree\nfunc _set(property, _value):\n\tif property == &\"tree_root\" or String(property).begins_with(\"parameters/\"):\n\t\tset_meta(\"setter_calls\", get_meta(\"setter_calls\", 0) + 1)\n\t\treturn true\n\treturn false\n");
	REQUIRE(script->reload() == OK);
	h.tree->set_script(script);
	REQUIRE(h.tree->get_script_instance() != nullptr);
	h.tree->set_meta("setter_calls", 0);
	CHECK(h.graph.apply(input, captured, h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.message.contains("editor-executing AnimationTree"));
	CHECK(h.tree->get_root_animation_node() == applied);
	CHECK(h.history->get_history_count() == 1);
	CHECK(h.tree->get_meta("setter_calls") == Variant(0));
	CHECK(h.history->undo());
	CHECK(h.tree->get_root_animation_node().is_null());
	CHECK(h.history->redo());
	CHECK(h.tree->get_root_animation_node() == applied);
	CHECK(h.tree->get_meta("setter_calls") == Variant(0));
}

TEST_CASE("[GodotMCP][AnimationGraph] Scene preflight rejects another executing tree before its property hooks") {
	GDScriptLanguage::get_singleton()->init();
	Harness h;
	AnimationTree *other = memnew(AnimationTree);
	other->set_name("OtherTree");
	other->set_active(false);
	h.root->add_child(other);
	other->set_owner(h.root);
	const GodotMCPScenePatch::Context captured = h.context();
	Dictionary input = h.params();
	Array operations;
	operations.push_back(_root("AnimationNodeAnimation", _properties("animation", "idle")));
	input["operations"] = operations;
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("@tool\nextends AnimationTree\nfunc _get_property_list():\n\tset_meta(\"_property_list_calls\", get_meta(\"_property_list_calls\", 0) + 1)\n\treturn []\n");
	REQUIRE(script->reload() == OK);
	other->set_script(script);
	REQUIRE(other->get_script_instance() != nullptr);
	REQUIRE_FALSE(other->get_script_instance()->is_placeholder());
	other->set_meta("_property_list_calls", 0);
	String preflight_message;
	CHECK(GodotMCPAnimationGraph::preflight_scene(h.root, preflight_message));
	CHECK(other->get_meta("_property_list_calls") == Variant(0));
	CHECK(h.graph.apply(input, captured, h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.message.contains("OtherTree"));
	CHECK(other->get_meta("_property_list_calls") == Variant(0));
	CHECK(h.tree->get_root_animation_node().is_null());
	CHECK(h.history->get_history_count() == 0);
}

TEST_CASE("[GodotMCP][AnimationGraph] Live non-tool script instances are not mistaken for editor placeholders") {
	GDScriptLanguage::get_singleton()->init();
	ControllerEditorScope editor_scope;
	ScriptServer::set_scripting_enabled(true);
	Harness h;
	const GodotMCPScenePatch::Context captured = h.context();
	Dictionary input = h.params();
	Array operations;
	operations.push_back(_root("AnimationNodeAnimation", _properties("animation", "idle")));
	input["operations"] = operations;
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("extends AnimationTree\nfunc _get_property_list():\n\tset_meta(\"_property_list_calls\", get_meta(\"_property_list_calls\", 0) + 1)\n\treturn []\n");
	REQUIRE(script->reload() == OK);
	CHECK_FALSE(script->is_tool());
	h.tree->set_script(script);
	REQUIRE(h.tree->get_script_instance() != nullptr);
	REQUIRE_FALSE(h.tree->get_script_instance()->is_placeholder());
	h.tree->set_meta("_property_list_calls", 0);
	CHECK(h.graph.apply(input, captured, h.warnings, h.code, h.message).is_empty());
	CHECK(h.code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(h.message.contains("editor-executing AnimationTree"));
	CHECK(h.tree->get_meta("_property_list_calls") == Variant(0));
	CHECK(h.history->get_history_count() == 0);
	Dictionary inspect;
	inspect["node_path"] = "Tree";
	const Dictionary result = h.graph.inspect(inspect, captured, h.code, h.message);
	REQUIRE(h.code == 0);
	CHECK(bool(result["parameters_partial"]));
	CHECK(h.tree->get_meta("_property_list_calls") == Variant(0));
}

TEST_CASE("[GodotMCP][AnimationGraph] Track existence checks do not execute script getters or dynamic property lists") {
	GDScriptLanguage::get_singleton()->init();
	Harness h;
	Ref<AnimationNodeAnimation> original;
	original.instantiate();
	original->set_animation("idle");
	h.tree->set_root_animation_node(original);
	const GodotMCPScenePatch::Context captured = h.context();
	Node *target = memnew(Node);
	target->set_name("Danger");
	h.root->add_child(target);
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("@tool\nextends Node\n@export var watched: int = 1:\n\tget:\n\t\tset_meta(\"getter_calls\", get_meta(\"getter_calls\", 0) + 1)\n\t\tget_parent().get_node(\"Tree\").tree_root = null\n\t\treturn 1\nfunc _get(_property):\n\tset_meta(\"dynamic_get_calls\", get_meta(\"dynamic_get_calls\", 0) + 1)\n\tget_parent().get_node(\"Tree\").tree_root = null\n\treturn 1\nfunc _get_property_list():\n\tset_meta(\"property_list_calls\", get_meta(\"property_list_calls\", 0) + 1)\n\treturn []\n");
	REQUIRE(script->reload() == OK);
	target->set_script(script);
	REQUIRE(target->get_script_instance() != nullptr);
	h.tree->set_root_animation_node(original);
	target->set_meta("getter_calls", 0);
	target->set_meta("dynamic_get_calls", 0);
	target->set_meta("property_list_calls", 0);
	const Ref<Animation> clip = h.player->get_animation("idle");
	for (const String &path : { String("Danger:watched"), String("Danger:custom"), String("Danger:name") }) {
		const int track = clip->add_track(Animation::TYPE_VALUE);
		clip->track_set_path(track, NodePath(path));
	}
	Dictionary input;
	input["node_path"] = "Tree";
	const Dictionary result = h.graph.validate(input, captured, h.code, h.message);
	REQUIRE(h.code == 0);
	CHECK(_has_issue(result, "warnings", "partial_track_property_validation"));
	CHECK(bool(result["partial"]));
	CHECK(h.tree->get_root_animation_node() == original);
	CHECK(target->get_meta("getter_calls") == Variant(0));
	CHECK(target->get_meta("dynamic_get_calls") == Variant(0));
	CHECK(target->get_meta("property_list_calls") == Variant(0));
}
#endif // MODULE_GDSCRIPT_ENABLED

} // namespace TestGodotMCPAnimationGraph

#endif // TOOLS_ENABLED
