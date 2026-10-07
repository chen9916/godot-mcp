/**************************************************************************/
/*  test_godot_mcp_scene_patch.cpp                                        */
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
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/undo_redo.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/debugger/godot_mcp/godot_mcp_scene_patch.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif
#include "scene/2d/node_2d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/gui/control.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

TEST_FORCE_LINK(test_godot_mcp_scene_patch)

namespace TestGodotMCPScenePatch {

struct ScopedProjectResourcePath {
	String previous_path;

	ScopedProjectResourcePath(const String &p_path) :
			previous_path(TestProjectSettingsInternalsAccessor::resource_path()) {
		TestProjectSettingsInternalsAccessor::resource_path() = p_path;
	}

	~ScopedProjectResourcePath() {
		TestProjectSettingsInternalsAccessor::resource_path() = previous_path;
	}
};

struct PatchHarness {
	Node3D *root = memnew(Node3D);
	UndoRedo *undo_redo = memnew(UndoRedo);
	String scene_path;
	int history_id = 17;

	PatchHarness(const String &p_scene_path = "res://scene_patch_test.tscn") :
			scene_path(p_scene_path) {
		root->set_name("Root");
		root->set_scene_file_path(scene_path);
	}

	~PatchHarness() {
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

	Dictionary apply(const Array &p_operations, int &r_error_code, String &r_error_message,
			const String &p_expected_version = String(), bool p_dry_run = false,
			const String &p_mode = "atomic", Dictionary *r_error_details = nullptr) {
		Dictionary params;
		params["expected_scene_path"] = scene_path;
		params["expected_scene_version"] = p_expected_version.is_empty() ? version() : p_expected_version;
		params["dry_run"] = p_dry_run;
		params["mode"] = p_mode;
		params["operations"] = p_operations;
		Array warnings;
		GodotMCPScenePatch patch;
		return patch.apply(params, context(), warnings, r_error_code, r_error_message, r_error_details);
	}
};

static Dictionary _resource_ref(const String &p_id) {
	Dictionary value;
	value["resource_ref"] = p_id;
	return value;
}

static Dictionary _node_ref(const String &p_id) {
	Dictionary value;
	value["node_ref"] = p_id;
	return value;
}

static Dictionary _typed(const String &p_type, const Array &p_value) {
	Dictionary value;
	value["__type"] = p_type;
	value["value"] = p_value;
	return value;
}

static Dictionary _create_resource(const String &p_id, const String &p_type, const Dictionary &p_properties = Dictionary()) {
	Dictionary operation;
	operation["op"] = "create_resource";
	operation["id"] = p_id;
	operation["type"] = p_type;
	if (!p_properties.is_empty()) {
		operation["properties"] = p_properties;
	}
	return operation;
}

static Dictionary _create_node(const String &p_id, const Variant &p_parent, const String &p_name,
		const String &p_type, const Dictionary &p_properties = Dictionary()) {
	Dictionary operation;
	operation["op"] = "create_node";
	operation["id"] = p_id;
	operation["parent"] = p_parent;
	operation["name"] = p_name;
	operation["type"] = p_type;
	if (!p_properties.is_empty()) {
		operation["properties"] = p_properties;
	}
	return operation;
}

static void _populate_complex_version_scene(Node3D *p_root, bool p_reverse_dictionary_order = false) {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.1, 0.8, 1.0, 1.0));
	Ref<SphereMesh> mesh;
	mesh.instantiate();
	mesh->set_radius(0.2);
	mesh->set_material(material);

	Dictionary nested;
	Array values;
	values.push_back(Vector3(1, 2, 3));
	values.push_back(NodePath("Branch_3/Mesh"));
	if (p_reverse_dictionary_order) {
		nested["zeta"] = values;
		nested["alpha"] = Color(0.2, 0.3, 0.4, 1.0);
	} else {
		nested["alpha"] = Color(0.2, 0.3, 0.4, 1.0);
		nested["zeta"] = values;
	}
	p_root->set_meta("nested", nested);
	p_root->set_meta("mesh", mesh);

	Node3D *signal_target = nullptr;
	for (int i = 0; i < 12; i++) {
		Node3D *branch = memnew(Node3D);
		branch->set_name("Branch_" + itos(i));
		branch->set_position(Vector3(i, i * 0.5, -i));
		p_root->add_child(branch);
		branch->set_owner(p_root);
		branch->add_to_group(i % 2 == 0 ? "even" : "odd", true);
		if (i == 3) {
			MeshInstance3D *mesh_node = memnew(MeshInstance3D);
			mesh_node->set_name("Mesh");
			mesh_node->set_mesh(mesh);
			branch->add_child(mesh_node);
			mesh_node->set_owner(p_root);
			signal_target = branch;
		}
	}
	REQUIRE(signal_target != nullptr);
	REQUIRE(p_root->connect(SNAME("ready"), Callable(signal_target, SNAME("queue_free")), Object::CONNECT_PERSIST) == OK);
}

TEST_CASE("[GodotMCP][ScenePatch] Complex snapshots are stable and independent of dictionary and object identity") {
	PatchHarness first;
	PatchHarness second;
	_populate_complex_version_scene(first.root, false);
	_populate_complex_version_scene(second.root, true);

	const String expected = first.version();
	CHECK(expected.begins_with("scene-v2:"));
	for (int i = 0; i < 20; i++) {
		CHECK(first.version() == expected);
	}
	CHECK(second.version() == expected);

	Dictionary changed = first.root->get_meta("nested");
	changed["alpha"] = Color(0.9, 0.3, 0.4, 1.0);
	first.root->set_meta("nested", changed);
	CHECK(first.version() != expected);
}

TEST_CASE("[GodotMCP][ScenePatch] Embedded resource cycles use stable scene-local anchors") {
	PatchHarness first;
	PatchHarness second;
	Ref<Resource> first_resource;
	first_resource.instantiate();
	Ref<Resource> second_resource;
	second_resource.instantiate();
	first_resource->set_meta("self", first_resource);
	second_resource->set_meta("self", second_resource);
	first.root->set_meta("cycle", first_resource);
	second.root->set_meta("cycle", second_resource);

	const String expected = first.version();
	CHECK(first.version() == expected);
	CHECK(second.version() == expected);

	first.root->remove_meta("cycle");
	second.root->remove_meta("cycle");
	first_resource->remove_meta("self");
	second_resource->remove_meta("self");
}

TEST_CASE("[GodotMCP][ScenePatch] Instanced and inherited scene snapshots remain stable") {
	const String temporary_project_path = TestUtils::get_temp_path("scene_patch_version_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project_path) == OK);
	ScopedProjectResourcePath resource_path_scope(temporary_project_path);
	const String base_path = "res://version_base.tscn";

	Node3D *base_root = memnew(Node3D);
	base_root->set_name("Base");
	Node3D *base_child = memnew(Node3D);
	base_child->set_name("BaseChild");
	base_root->add_child(base_child);
	base_child->set_owner(base_root);
	Ref<PackedScene> base_scene;
	base_scene.instantiate();
	REQUIRE(base_scene->pack(base_root) == OK);
	REQUIRE(ResourceSaver::save(base_scene, base_path) == OK);
	memdelete(base_root);

	Ref<PackedScene> loaded_base = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_base.is_valid());
	Node *inherited_root = loaded_base->instantiate(PackedScene::GEN_EDIT_STATE_MAIN_INHERITED);
	REQUIRE(inherited_root != nullptr);
	inherited_root->set_scene_file_path("res://version_inherited.tscn");
	const String inherited_version = GodotMCPScenePatch::compute_scene_version(inherited_root, 33, 1);
	for (int i = 0; i < 20; i++) {
		CHECK(GodotMCPScenePatch::compute_scene_version(inherited_root, 33, 1) == inherited_version);
	}

	Node3D *host = memnew(Node3D);
	host->set_name("Host");
	host->set_scene_file_path("res://version_host.tscn");
	Node *instance = loaded_base->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	REQUIRE(instance != nullptr);
	host->add_child(instance);
	instance->set_owner(host);
	const String instance_version = GodotMCPScenePatch::compute_scene_version(host, 34, 1);
	for (int i = 0; i < 20; i++) {
		CHECK(GodotMCPScenePatch::compute_scene_version(host, 34, 1) == instance_version);
	}

	memdelete(host);
	memdelete(inherited_root);
}

TEST_CASE("[GodotMCP][ScenePatch] External scene content participates in optimistic versions") {
	const String temporary_project_path = TestUtils::get_temp_path("scene_patch_disk_version_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project_path) == OK);
	ScopedProjectResourcePath resource_path_scope(temporary_project_path);
	PatchHarness harness("res://external_version.tscn");

	Ref<FileAccess> file = FileAccess::open(harness.scene_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("[gd_scene format=3]\n\n[node name=\"Root\" type=\"Node3D\"]\n");
	file.unref();
	const String before = harness.version();
	file = FileAccess::open(harness.scene_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("[gd_scene format=3]\n\n[node name=\"RootChangedOnDisk\" type=\"Node3D\"]\n");
	file.unref();
	CHECK(harness.version() != before);
}

TEST_CASE("[GodotMCP][ScenePatch] Property planning order and normalization warnings are deterministic") {
	PatchHarness first;
	PatchHarness second;
	Dictionary forward;
	forward["height"] = 1;
	forward["radius"] = 0.25;
	Dictionary reverse;
	reverse["radius"] = 0.25;
	reverse["height"] = 1;
	Array first_operations;
	first_operations.push_back(_create_resource("mesh", "SphereMesh", forward));
	Array second_operations;
	second_operations.push_back(_create_resource("mesh", "SphereMesh", reverse));

	int error_code = 0;
	String error_message;
	const Dictionary first_result = first.apply(first_operations, error_code, error_message, String(), true);
	REQUIRE(error_code == 0);
	const Dictionary second_result = second.apply(second_operations, error_code, error_message, String(), true);
	REQUIRE(error_code == 0);
	CHECK(Array(first_result["property_validation_warnings"]) == Array(second_result["property_validation_warnings"]));
	CHECK(Dictionary(first_result["change_summary"]) == Dictionary(second_result["change_summary"]));
	CHECK(first.version() == String(first_result["updated_scene_version"]));
	CHECK(second.version() == String(second_result["updated_scene_version"]));
}

TEST_CASE("[GodotMCP][ScenePatch] Local resource references commit as one undo action") {
	PatchHarness harness;
	const String before_version = harness.version();

	Array color_values;
	color_values.push_back(0.1);
	color_values.push_back(0.8);
	color_values.push_back(1.0);
	color_values.push_back(1.0);
	Dictionary material_properties;
	material_properties["emission_enabled"] = true;
	material_properties["emission"] = _typed("Color", color_values);

	Dictionary mesh_properties;
	mesh_properties["material"] = _resource_ref("core_material");
	mesh_properties["radius"] = 0.2;
	mesh_properties["height"] = 0.4;

	Dictionary node_properties;
	node_properties["mesh"] = _resource_ref("core_mesh");

	Array operations;
	operations.push_back(_create_resource("core_material", "StandardMaterial3D", material_properties));
	operations.push_back(_create_resource("core_mesh", "SphereMesh", mesh_properties));
	Dictionary node_operation = _create_node("core_node", ".", "Core", "MeshInstance3D", node_properties);
	Array groups;
	groups.push_back("vfx");
	node_operation["groups"] = groups;
	operations.push_back(node_operation);

	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	CHECK(String(result["previous_scene_version"]) == before_version);
	CHECK(String(result["updated_scene_version"]) != before_version);
	CHECK(String(result["undo_action_name"]) == "Godot MCP: Apply Scene Patch");
	CHECK(int(result["success_count"]) == 3);
	CHECK(harness.undo_redo->get_history_count() == 1);

	MeshInstance3D *core = Object::cast_to<MeshInstance3D>(harness.root->get_node_or_null(NodePath("Core")));
	REQUIRE(core != nullptr);
	CHECK(core->is_in_group("vfx"));
	Ref<Resource> mesh = core->get("mesh");
	REQUIRE(mesh.is_valid());
	Ref<Resource> material = mesh->get("material");
	REQUIRE(material.is_valid());
	CHECK(material->get("emission_enabled") == Variant(true));
	CHECK(mesh->get_path().begins_with(harness.scene_path + "::SphereMesh_core_mesh"));

	CHECK(harness.undo_redo->undo());
	CHECK(harness.root->get_node_or_null(NodePath("Core")) == nullptr);
	CHECK(harness.version() == before_version);

	CHECK(harness.undo_redo->redo());
	core = Object::cast_to<MeshInstance3D>(harness.root->get_node_or_null(NodePath("Core")));
	REQUIRE(core != nullptr);
	mesh = core->get("mesh");
	REQUIRE(mesh.is_valid());
	CHECK(Ref<Resource>(mesh->get("material")).is_valid());
}

TEST_CASE("[GodotMCP][ScenePatch] Atomic validation, dry run, and optimistic conflicts do not mutate") {
	PatchHarness harness;
	const String initial_version = harness.version();

	Array operations;
	operations.push_back(_create_node("valid", ".", "WouldBeValid", "Node3D"));
	Dictionary invalid_properties;
	invalid_properties["property_that_does_not_exist"] = 1;
	operations.push_back(_create_node("invalid", ".", "Invalid", "Node3D", invalid_properties));

	int error_code = 0;
	String error_message;
	Dictionary details;
	CHECK(harness.apply(operations, error_code, error_message, String(), false, "atomic", &details).is_empty());
	CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(int(details["operation_index"]) == 1);
	CHECK(harness.root->get_child_count() == 0);
	CHECK(harness.undo_redo->get_history_count() == 0);
	CHECK(harness.version() == initial_version);

	error_code = 0;
	error_message.clear();
	Array dry_operations;
	dry_operations.push_back(_create_node("preview", ".", "Preview", "Node3D"));
	const Dictionary dry_result = harness.apply(dry_operations, error_code, error_message, String(), true);
	REQUIRE(error_code == 0);
	CHECK(bool(dry_result["dry_run"]));
	CHECK_FALSE(bool(dry_result["applied"]));
	CHECK(Dictionary(dry_result["created_node_paths"])["preview"] == Variant("Preview"));
	CHECK(harness.root->get_child_count() == 0);
	CHECK(harness.undo_redo->get_history_count() == 0);

	error_code = 0;
	error_message.clear();
	details.clear();
	harness.root->set_position(Vector3(1.0, 2.0, 3.0));
	CHECK(harness.version() != initial_version);
	CHECK(harness.apply(dry_operations, error_code, error_message, initial_version, false, "atomic", &details).is_empty());
	CHECK(error_code == GodotMCPError::CONFLICT);
	CHECK(String(details["conflict_kind"]) == "content_version_conflict");
	CHECK(String(details["active_scene_version"]) == harness.version());
	CHECK(harness.root->get_child_count() == 0);
	CHECK(harness.root->get_position() == Vector3(1.0, 2.0, 3.0));
}

TEST_CASE("[GodotMCP][ScenePatch] Conflict diagnostics distinguish path and undo history") {
	PatchHarness harness;
	Array operations;
	operations.push_back(_create_node("first", ".", "First", "Node3D"));
	const String stale_version = harness.version();
	int error_code = 0;
	String error_message;
	Dictionary details;
	REQUIRE_FALSE(harness.apply(operations, error_code, error_message).is_empty());

	Array next_operations;
	next_operations.push_back(_create_node("second", ".", "Second", "Node3D"));
	CHECK(harness.apply(next_operations, error_code, error_message, stale_version, false, "atomic", &details).is_empty());
	CHECK(error_code == GodotMCPError::CONFLICT);
	CHECK(String(details["conflict_kind"]) == "history_conflict");

	Dictionary params;
	params["expected_scene_path"] = "res://another_scene.tscn";
	params["expected_scene_version"] = harness.version();
	params["operations"] = next_operations;
	Array warnings;
	GodotMCPScenePatch patch;
	details.clear();
	CHECK(patch.apply(params, harness.context(), warnings, error_code, error_message, &details).is_empty());
	CHECK(error_code == GodotMCPError::CONFLICT);
	CHECK(String(details["conflict_kind"]) == "path_conflict");
}

TEST_CASE("[GodotMCP][ScenePatch] Best effort reports failures and applies independent operations") {
	PatchHarness harness;
	Array operations;
	Dictionary invalid_properties;
	invalid_properties["missing"] = true;
	operations.push_back(_create_node("failed_node", ".", "Failed", "Node3D", invalid_properties));
	operations.push_back(_create_node("dependent", _node_ref("failed_node"), "Dependent", "Node3D"));
	operations.push_back(_create_node("successful", ".", "Successful", "Node3D"));

	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message, String(), false, "best_effort");
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	CHECK(int(result["success_count"]) == 1);
	CHECK(int(result["failure_count"]) == 1);
	CHECK(int(result["skipped_count"]) == 1);
	const Array operation_results = result["operations"];
	CHECK(String(Dictionary(operation_results[0])["status"]) == "failed");
	CHECK(String(Dictionary(operation_results[1])["status"]) == "skipped");
	CHECK(String(Dictionary(operation_results[2])["status"]) == "applied");
	CHECK(harness.root->get_node_or_null(NodePath("Successful")) != nullptr);
	CHECK(harness.root->get_node_or_null(NodePath("Failed")) == nullptr);
}

TEST_CASE("[GodotMCP][ScenePatch] Node move, update, delete, and resource delete undo together") {
	PatchHarness harness;
	Node3D *parent_a = memnew(Node3D);
	parent_a->set_name("ParentA");
	harness.root->add_child(parent_a);
	parent_a->set_owner(harness.root);
	Node3D *parent_b = memnew(Node3D);
	parent_b->set_name("ParentB");
	harness.root->add_child(parent_b);
	parent_b->set_owner(harness.root);
	Node3D *delete_me = memnew(Node3D);
	delete_me->set_name("DeleteMe");
	parent_a->add_child(delete_me);
	delete_me->set_owner(harness.root);
	const String before_version = harness.version();

	Dictionary mesh_properties;
	mesh_properties["material"] = _resource_ref("temporary_material");
	Array operations;
	operations.push_back(_create_resource("temporary_material", "StandardMaterial3D"));
	operations.push_back(_create_resource("temporary_mesh", "SphereMesh", mesh_properties));
	Dictionary created_properties;
	created_properties["mesh"] = _resource_ref("temporary_mesh");
	operations.push_back(_create_node("moving", "ParentA", "Moving", "MeshInstance3D", created_properties));

	Dictionary update;
	update["op"] = "update_node";
	update["node"] = _node_ref("moving");
	update["name"] = "Renamed";
	Array updated_groups;
	updated_groups.push_back("patched");
	update["add_groups"] = updated_groups;
	operations.push_back(update);

	Dictionary move;
	move["op"] = "move_node";
	move["node"] = _node_ref("moving");
	move["parent"] = "ParentB";
	operations.push_back(move);

	Dictionary delete_node;
	delete_node["op"] = "delete_node";
	delete_node["node"] = "ParentA/DeleteMe";
	operations.push_back(delete_node);

	Dictionary delete_resource;
	delete_resource["op"] = "delete_resource";
	delete_resource["resource"] = _resource_ref("temporary_material");
	operations.push_back(delete_resource);

	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	MeshInstance3D *moved = Object::cast_to<MeshInstance3D>(harness.root->get_node_or_null(NodePath("ParentB/Renamed")));
	REQUIRE(moved != nullptr);
	CHECK(moved->is_in_group("patched"));
	Ref<Resource> moved_mesh = moved->get("mesh");
	REQUIRE(moved_mesh.is_valid());
	CHECK(Ref<Resource>(moved_mesh->get("material")).is_null());
	CHECK(harness.root->get_node_or_null(NodePath("ParentA/DeleteMe")) == nullptr);

	CHECK(harness.undo_redo->undo());
	CHECK(harness.root->get_node_or_null(NodePath("ParentB/Renamed")) == nullptr);
	CHECK(harness.root->get_node_or_null(NodePath("ParentA/DeleteMe")) == delete_me);
	CHECK(harness.version() == before_version);

	CHECK(harness.undo_redo->redo());
	CHECK(harness.root->get_node_or_null(NodePath("ParentB/Renamed")) != nullptr);
	CHECK(harness.root->get_node_or_null(NodePath("ParentA/DeleteMe")) == nullptr);
}

TEST_CASE("[GodotMCP][ScenePatch] Root renames validate and undo through every name form") {
	PatchHarness harness;
	const String before_version = harness.version();
	Array operations;
	SUBCASE("operation name") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":".","name":"RenamedRoot"}])");
	}
	SUBCASE("property name") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":".","properties":{"name":"RenamedRoot"}}])");
	}
	SUBCASE("ordered property name with StringName encoding") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":".","property_entries":[{"name":"name","value":{"__type":"StringName","value":"RenamedRoot"}}]}])");
	}
	int error_code = 0;
	String error_message;
	const Dictionary preview = harness.apply(operations, error_code, error_message, String(), true);
	REQUIRE(error_code == 0);
	CHECK_FALSE(bool(preview["applied"]));
	CHECK(harness.root->get_name() == StringName("Root"));
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->get_history_count() == 0);

	const Dictionary result = harness.apply(operations, error_code, error_message);
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	CHECK(harness.root->get_name() == StringName("RenamedRoot"));
	CHECK(harness.undo_redo->undo());
	CHECK(harness.root->get_name() == StringName("Root"));
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->redo());
	CHECK(harness.root->get_name() == StringName("RenamedRoot"));
}

TEST_CASE("[GodotMCP][ScenePatch] Property names participate in collision checks and returned paths") {
	PatchHarness harness;
	Node3D *original = memnew(Node3D);
	original->set_name("A");
	harness.root->add_child(original);
	original->set_owner(harness.root);
	int error_code = 0;
	String error_message;

	SUBCASE("a rename cannot hide a subsequent sibling collision") {
		const Array operations = JSON::parse_string(R"([
			{"op":"update_node","node":"A","properties":{"name":"RenamedA"}},
			{"op":"create_node","id":"collision","parent":".","name":"RenamedA","type":"Node3D"}
		])");
		CHECK(harness.apply(operations, error_code, error_message).is_empty());
		CHECK(error_code == GodotMCPError::CONFLICT);
		CHECK(original->get_name() == StringName("A"));
		CHECK(harness.root->get_child_count() == 1);
		CHECK(harness.undo_redo->get_history_count() == 0);
	}
	SUBCASE("create and update property names share the planned namespace") {
		const Array operations = JSON::parse_string(R"([
			{"op":"update_node","node":"A","properties":{"name":"RenamedA"}},
			{"op":"create_node","id":"replacement","parent":".","type":"Node3D","properties":{"name":"A"}}
		])");
		const String input_before = JSON::stringify(operations);
		const Dictionary result = harness.apply(operations, error_code, error_message);
		INFO(error_message.utf8().get_data());
		REQUIRE(error_code == 0);
		CHECK(JSON::stringify(operations) == input_before);
		CHECK(harness.root->get_node_or_null(NodePath("RenamedA")) == original);
		CHECK(harness.root->get_node_or_null(NodePath("A")) != original);
		CHECK(Dictionary(result["created_node_paths"])["replacement"] == Variant("A"));
		const Array results = result["operations"];
		CHECK(Dictionary(Dictionary(results[0])["data"])["node_path"] == Variant("RenamedA"));
		CHECK(harness.undo_redo->undo());
		CHECK(harness.root->get_node_or_null(NodePath("A")) == original);
		CHECK(harness.root->get_child_count() == 1);
		CHECK(harness.undo_redo->redo());
		CHECK(harness.root->get_node_or_null(NodePath("RenamedA")) == original);
	}
	SUBCASE("ambiguous field and property names are rejected") {
		const Array operations = JSON::parse_string(R"([{"op":"update_node","node":"A","name":"First","properties":{"name":"Second"}}])");
		CHECK(harness.apply(operations, error_code, error_message).is_empty());
		CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(original->get_name() == StringName("A"));
	}
}

TEST_CASE("[GodotMCP][ScenePatch] JSON integer positions and packed values retain their types") {
	PatchHarness harness;
	harness.root->set_meta("bytes", PackedByteArray());
	harness.root->set_meta("int32", PackedInt32Array());
	harness.root->set_meta("int64", PackedInt64Array());
	const Array operations = JSON::parse_string(R"([
		{"op":"create_node","id":"last","parent":".","name":"Last","type":"Node3D","position":-1},
		{"op":"create_node","id":"first","parent":".","name":"First","type":"Node3D","position":0},
		{"op":"move_node","node":{"node_ref":"last"},"parent":".","position":0},
		{"op":"update_node","node":".","properties":{
			"metadata/bytes":{"__type":"PackedByteArray","value":[0,255]},
			"metadata/int32":{"__type":"PackedInt32Array","value":[-2147483648,2147483647]},
			"metadata/int64":{"__type":"PackedInt64Array","value":[-9223372036854775808,9007199254740992]}
		}}
	])");
	REQUIRE(Dictionary(operations[1])["position"].get_type() == Variant::FLOAT);
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	CHECK(harness.root->get_child(0)->get_name() == StringName("Last"));
	const PackedByteArray bytes = harness.root->get_meta("bytes");
	const PackedInt32Array int32 = harness.root->get_meta("int32");
	const PackedInt64Array int64 = harness.root->get_meta("int64");
	REQUIRE(bytes.size() == 2);
	REQUIRE(int32.size() == 2);
	REQUIRE(int64.size() == 2);
	CHECK(bytes[0] == 0);
	CHECK(bytes[1] == 255);
	CHECK(int32[0] == INT32_MIN);
	CHECK(int32[1] == INT32_MAX);
	CHECK(int64[0] == INT64_MIN);
	CHECK(int64[1] == INT64_C(9007199254740992));
	CHECK(harness.undo_redo->undo());
	CHECK(harness.root->get_child_count() == 0);
	CHECK(PackedByteArray(harness.root->get_meta("bytes")).is_empty());
	CHECK(harness.undo_redo->redo());
	CHECK(PackedInt64Array(harness.root->get_meta("int64")) == int64);
}

TEST_CASE("[GodotMCP][ScenePatch] Positions and undo exclude internal children") {
	PatchHarness harness;
	Node *parent = memnew(Node);
	parent->set_name("Parent");
	harness.root->add_child(parent);
	parent->set_owner(harness.root);
	Node *front = memnew(Node);
	front->set_name("InternalFront");
	parent->add_child(front, false, Node::INTERNAL_MODE_FRONT);
	Node *back = memnew(Node);
	back->set_name("InternalBack");
	parent->add_child(back, false, Node::INTERNAL_MODE_BACK);
	for (const String &name : { String("A"), String("B"), String("C") }) {
		Node *child = memnew(Node);
		child->set_name(name);
		parent->add_child(child);
		child->set_owner(harness.root);
	}
	const String before_version = harness.version();
	const auto check_order = [&](const String &p_names) {
		String names;
		for (int i = 0; i < parent->get_child_count(false); i++) {
			names += String(parent->get_child(i, false)->get_name());
		}
		CHECK(names == p_names);
		CHECK(parent->get_child(0, true) == front);
		CHECK(parent->get_child(parent->get_child_count(true) - 1, true) == back);
	};
	int error_code = 0;
	String error_message;
	Array operations;
	String expected;
	SUBCASE("moving away restores the original ordinary index") {
		operations = JSON::parse_string(R"([{"op":"move_node","node":"Parent/B","parent":"."}])");
		expected = "AC";
	}
	SUBCASE("deletion undo restores the original ordinary index") {
		operations = JSON::parse_string(R"([{"op":"delete_node","node":"Parent/B"}])");
		expected = "AC";
	}
	SUBCASE("ordered create move delete uses the planned ordinary range") {
		operations = JSON::parse_string(R"([
			{"op":"create_node","id":"d","parent":"Parent","name":"D","type":"Node","position":1},
			{"op":"move_node","node":"Parent/B","parent":"Parent","position":0},
			{"op":"delete_node","node":"Parent/A"}
		])");
		expected = "BDC";
	}
	SUBCASE("append stays before internal back children") {
		operations = JSON::parse_string(R"([{"op":"create_node","id":"d","parent":"Parent","name":"D","type":"Node"}])");
		expected = "ABCD";
	}
	SUBCASE("create cannot use internal children to extend the position bound") {
		operations = JSON::parse_string(R"([{"op":"create_node","id":"d","parent":"Parent","name":"D","type":"Node","position":4}])");
	}
	SUBCASE("move cannot use internal children to extend the position bound") {
		operations = JSON::parse_string(R"([{"op":"move_node","node":"Parent/B","parent":"Parent","position":3}])");
	}
	const Dictionary result = harness.apply(operations, error_code, error_message);
	if (expected.is_empty()) {
		CHECK(result.is_empty());
		CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.undo_redo->get_history_count() == 0);
		check_order("ABC");
		return;
	}
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	check_order(expected);
	CHECK(harness.undo_redo->undo());
	check_order("ABC");
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->redo());
	check_order(expected);
}

TEST_CASE("[GodotMCP][ScenePatch] Integer inputs reject fractions non-numbers and overflow") {
	PatchHarness harness;
	int error_code = 0;
	String error_message;
	const Array invalid_positions = JSON::parse_string(R"([0.5,-2,2147483648,4294967296,9223372036854775808,true,"0"])");
	for (int i = 0; i < invalid_positions.size(); i++) {
		Dictionary create = _create_node("invalid", ".", "Invalid", "Node3D");
		create["position"] = invalid_positions[i];
		Array operations;
		operations.push_back(create);
		CHECK(harness.apply(operations, error_code, error_message).is_empty());
		CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
	}
	const Array invalid_packed_values = JSON::parse_string(R"([
		{"__type":"PackedByteArray","value":[256]},
		{"__type":"PackedByteArray","value":[-1]},
		{"__type":"PackedByteArray","value":[1.5]},
		{"__type":"PackedInt32Array","value":[2147483648]},
		{"__type":"PackedInt32Array","value":[-2147483649]},
		{"__type":"PackedInt64Array","value":[9223372036854775808]},
		{"__type":"PackedInt64Array","value":[-9223372036854777856]},
		{"__type":"PackedInt64Array","value":[true]}
	])");
	harness.root->set_meta("value", PackedByteArray());
	for (int i = 0; i < invalid_packed_values.size(); i++) {
		Dictionary properties;
		properties["metadata/value"] = invalid_packed_values[i];
		Dictionary update;
		update["op"] = "update_node";
		update["node"] = ".";
		update["properties"] = properties;
		Array operations;
		operations.push_back(update);
		CHECK(harness.apply(operations, error_code, error_message).is_empty());
		CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
	}
	CHECK(harness.root->get_child_count() == 0);
	CHECK(harness.undo_redo->get_history_count() == 0);
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Subtree ownership survives moves deletion undo and packing") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Node3D *parent_a = memnew(Node3D);
	parent_a->set_name("A");
	harness.root->add_child(parent_a);
	parent_a->set_owner(harness.root);
	Node3D *parent_b = memnew(Node3D);
	parent_b->set_name("B");
	harness.root->add_child(parent_b);
	parent_b->set_owner(harness.root);
	Node3D *branch = memnew(Node3D);
	branch->set_name("Branch");
	parent_a->add_child(branch);
	branch->set_owner(harness.root);
	Node3D *leaf = memnew(Node3D);
	leaf->set_name("Leaf");
	branch->add_child(leaf);
	leaf->set_owner(harness.root);
	Node3D *nested = memnew(Node3D);
	nested->set_name("Nested");
	leaf->add_child(nested);
	nested->set_owner(branch);
	Node3D *transient = memnew(Node3D);
	transient->set_name("Transient");
	branch->add_child(transient);
	const String before_version = harness.version();

	Array operations;
	bool deleted = false;
	SUBCASE("move") {
		operations = JSON::parse_string(R"([{"op":"move_node","node":"A/Branch","parent":"B"}])");
	}
	SUBCASE("delete") {
		deleted = true;
		operations = JSON::parse_string(R"([{"op":"delete_node","node":"A/Branch"}])");
	}
	SUBCASE("move then delete uses ownership from the preceding planned operation") {
		deleted = true;
		operations = JSON::parse_string(R"([
			{"op":"move_node","node":"A/Branch","parent":"B"},
			{"op":"delete_node","node":"A/Branch"}
		])");
	}
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	CHECK(bool(result["applied"]));
	if (!deleted) {
		CHECK(branch->get_parent() == parent_b);
		CHECK(leaf->get_owner() == harness.root);
		CHECK(nested->get_owner() == branch);
		CHECK(transient->get_owner() == nullptr);
		Ref<PackedScene> packed;
		packed.instantiate();
		REQUIRE(packed->pack(harness.root) == OK);
		Node *reloaded = packed->instantiate();
		REQUIRE(reloaded != nullptr);
		CHECK(reloaded->get_node_or_null(NodePath("B/Branch/Leaf")) != nullptr);
		memdelete(reloaded);
	}
	CHECK(harness.undo_redo->undo());
	CHECK(branch->get_parent() == parent_a);
	CHECK(branch->get_owner() == harness.root);
	CHECK(leaf->get_owner() == harness.root);
	CHECK(nested->get_owner() == branch);
	CHECK(transient->get_owner() == nullptr);
	CHECK(harness.version() == before_version);
	Ref<PackedScene> restored;
	restored.instantiate();
	REQUIRE(restored->pack(harness.root) == OK);
	Node *reloaded = restored->instantiate();
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded->get_node_or_null(NodePath("A/Branch/Leaf")) != nullptr);
	memdelete(reloaded);
	CHECK(harness.undo_redo->redo());
	if (!deleted) {
		CHECK(branch->get_parent() == parent_b);
		CHECK(leaf->get_owner() == harness.root);
		CHECK(nested->get_owner() == branch);
	}
}

TEST_CASE("[GodotMCP][ScenePatch][Editor] Complex particle fixture saves and reloads embedded references") {
	const String temporary_project_path = TestUtils::get_temp_path("scene_patch_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project_path) == OK);
	ScopedProjectResourcePath resource_path_scope(temporary_project_path);
	const String scene_path = "res://capacitor_primer_burst.tscn";

	{
		PatchHarness harness(scene_path);
		Ref<FileAccess> fixture_file = FileAccess::open(TestUtils::get_data_path("godot_mcp/capacitor_primer_burst_patch.json"), FileAccess::READ);
		REQUIRE(fixture_file.is_valid());
		const Variant fixture_variant = JSON::parse_string(fixture_file->get_as_text());
		REQUIRE(fixture_variant.get_type() == Variant::DICTIONARY);
		const Dictionary fixture = fixture_variant;
		const Array operations = fixture["operations"];
		REQUIRE(operations.size() == 44);

		int error_code = 0;
		String error_message;
		const Dictionary result = harness.apply(operations, error_code, error_message);
		INFO(error_message.utf8().get_data());
		REQUIRE(error_code == 0);
		CHECK(bool(result["applied"]));
		CHECK(int(Dictionary(result["change_summary"])["property_writes"]) > 40);
		CHECK(Dictionary(result["created_node_paths"]).size() == 23);
		CHECK(Dictionary(result["created_embedded_resource_ids"]).size() == 20);
		const Dictionary created_resource_ids = result["created_embedded_resource_ids"];
		CHECK(harness.undo_redo->get_history_count() == 1);

		Ref<PackedScene> packed_scene;
		packed_scene.instantiate();
		REQUIRE(packed_scene->pack(harness.root) == OK);
		REQUIRE(ResourceSaver::save(packed_scene, scene_path) == OK);

		Error load_error = OK;
		Ref<PackedScene> reloaded = ResourceLoader::load(scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &load_error);
		REQUIRE(load_error == OK);
		REQUIRE(reloaded.is_valid());
		Node *instance = reloaded->instantiate();
		REQUIRE(instance != nullptr);

		MeshInstance3D *core = Object::cast_to<MeshInstance3D>(instance->get_node_or_null(NodePath("CapacitorPrimerBurst/Core")));
		REQUIRE(core != nullptr);
		Ref<Resource> core_mesh = core->get("mesh");
		REQUIRE(core_mesh.is_valid());
		Ref<Resource> core_material = core_mesh->get("material");
		REQUIRE(core_material.is_valid());
		CHECK(core_mesh->get_path().begins_with(scene_path + "::"));
		CHECK(core_material->get_path().begins_with(scene_path + "::"));
		CHECK(core_mesh->get_scene_unique_id() == String(created_resource_ids["core_mesh"]));
		CHECK(core_material->get_scene_unique_id() == String(created_resource_ids["core_material"]));

		Node *core_particles = instance->get_node_or_null(NodePath("CapacitorPrimerBurst/Particles/CoreParticles"));
		REQUIRE(core_particles != nullptr);
		Node *spark_particles = instance->get_node_or_null(NodePath("CapacitorPrimerBurst/Particles/SparkParticles"));
		REQUIRE(spark_particles != nullptr);
		CHECK(core_particles->get_node_or_null(NodePath(core_particles->get("sub_emitter"))) == spark_particles);
		Ref<Resource> process_material = core_particles->get("process_material");
		REQUIRE(process_material.is_valid());
		Ref<Resource> curve_texture = process_material->get("scale_curve");
		REQUIRE(curve_texture.is_valid());
		Ref<Resource> curve = curve_texture->get("curve");
		REQUIRE(curve.is_valid());
		CHECK(int(curve->get("point_count")) == 2);

		Node *animation_player = instance->get_node_or_null(NodePath("CapacitorPrimerBurst/AnimationPlayer"));
		REQUIRE(animation_player != nullptr);
		Ref<Resource> animation_library = animation_player->get("libraries/");
		REQUIRE(animation_library.is_valid());
		const Dictionary animations = animation_library->get("_data");
		CHECK(animations.has("burst"));
		CHECK(Ref<Resource>(animations["burst"]).is_valid());

		memdelete(instance);
	}
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Global properties use planned transforms and preserve undo and serialization") {
	const String temporary_project_path = TestUtils::get_temp_path("scene_patch_global_project");
	REQUIRE(DirAccess::make_dir_recursive_absolute(temporary_project_path) == OK);
	ScopedProjectResourcePath resource_path_scope(temporary_project_path);
	PatchHarness harness("res://global_patch.tscn");
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	harness.root->set_position(Vector3(3, 5, 7));
	Node3D *parent = memnew(Node3D);
	parent->set_name("Parent");
	harness.root->add_child(parent);
	parent->set_owner(harness.root);
	parent->set_transform(Transform3D(Basis::from_euler(Vector3(0.2, 0.5, 0.1)) * Basis::from_scale(Vector3(2, 3, 4)), Vector3(10, 1, 2)));
	Node3D *child = memnew(Node3D);
	child->set_name("Child");
	parent->add_child(child);
	child->set_owner(harness.root);
	child->set_position(Vector3(1, 2, 3));
	const Transform3D before = child->get_transform();
	const String before_version = harness.version();
	String property;
	Variant requested;
	SUBCASE("position") {
		property = "global_position";
		requested = Vector3(20, 30, 40);
	}
	SUBCASE("basis") {
		property = "global_basis";
		requested = Basis::from_euler(Vector3(0.3, 0.1, 0.4)) * Basis::from_scale(Vector3(3, 2, 1));
	}
	SUBCASE("rotation") {
		property = "global_rotation";
		requested = Vector3(0.3, 0.1, 0.4);
	}
	SUBCASE("rotation degrees") {
		property = "global_rotation_degrees";
		requested = Vector3(15, 25, 35);
	}
	SUBCASE("transform") {
		property = "global_transform";
		requested = Transform3D(Basis::from_euler(Vector3(0.3, 0.1, 0.4)), Vector3(20, 30, 40));
	}
	Dictionary properties;
	properties[property] = requested;
	Dictionary update;
	update["op"] = "update_node";
	update["node"] = "Parent/Child";
	update["properties"] = properties;
	Array operations;
	operations.push_back(update);
	int error_code = 0;
	String error_message;
	REQUIRE_FALSE(harness.apply(operations, error_code, error_message, String(), true).is_empty());
	REQUIRE(error_code == 0);
	CHECK(child->get_transform().is_equal_approx(before));
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->get_history_count() == 0);
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	if (requested.get_type() == Variant::VECTOR3) {
		CHECK(Vector3(child->get(property)).is_equal_approx(Vector3(requested)));
	} else if (requested.get_type() == Variant::BASIS) {
		CHECK(Basis(child->get(property)).is_equal_approx(Basis(requested)));
	} else {
		CHECK(Transform3D(child->get(property)).is_equal_approx(Transform3D(requested)));
	}
	const Transform3D applied = child->get_transform();
	REQUIRE(harness.undo_redo->undo());
	CHECK(child->get_transform().is_equal_approx(before));
	CHECK(harness.version() == before_version);
	REQUIRE(harness.undo_redo->redo());
	CHECK(child->get_transform().is_equal_approx(applied));
	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(harness.root) == OK);
	REQUIRE(ResourceSaver::save(packed, harness.scene_path) == OK);
	Ref<PackedScene> reloaded = ResourceLoader::load(harness.scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(reloaded.is_valid());
	Node *instance = reloaded->instantiate();
	REQUIRE(instance != nullptr);
	Node3D *reloaded_child = Object::cast_to<Node3D>(instance->get_node_or_null(NodePath("Parent/Child")));
	REQUIRE(reloaded_child != nullptr);
	CHECK(reloaded_child->get_transform().is_equal_approx(applied));
	memdelete(instance);
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Ordered transforms follow ancestor updates creates moves and top level transitions") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Node3D *parent = memnew(Node3D);
	parent->set_name("Parent");
	harness.root->add_child(parent);
	parent->set_owner(harness.root);
	parent->set_position(Vector3(10, 0, 0));
	Node3D *child = memnew(Node3D);
	child->set_name("Child");
	harness.root->add_child(child);
	child->set_owner(harness.root);
	child->set_position(Vector3(1, 2, 3));
	const String before_version = harness.version();
	Array operations;
	SUBCASE("new child uses the updated parent's transform") {
		operations = JSON::parse_string(R"([
			{"op":"update_node","node":"Parent","properties":{"position":[15,0,0]}},
			{"op":"create_node","id":"new","parent":"Parent","name":"New","type":"Node3D","property_entries":[
				{"name":"position","value":[1,2,3]}, {"name":"global_position","value":[20,30,40]}
			]}
		])");
	}
	SUBCASE("moved child and repeated property entries retain their explicit order") {
		operations = JSON::parse_string(R"([
			{"op":"move_node","node":"Child","parent":"Parent"},
			{"op":"update_node","node":"Parent","properties":{"position":[15,0,0]}},
			{"op":"update_node","node":"Child","property_entries":[
				{"name":"position","value":[99,99,99]}, {"name":"global_position","value":[1,2,3]},
				{"name":"position","value":[5,30,40]}
			]}
		])");
	}
	SUBCASE("top level transitions preserve world transforms") {
		operations = JSON::parse_string(R"([
			{"op":"move_node","node":"Child","parent":"Parent"},
			{"op":"update_node","node":"Child","property_entries":[
				{"name":"top_level","value":true}, {"name":"global_position","value":[20,30,40]},
				{"name":"top_level","value":false}, {"name":"global_rotation_degrees","value":[0,45,0]}
			]}
		])");
	}
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	Node3D *patched = Object::cast_to<Node3D>(parent->get_child(0));
	REQUIRE(patched != nullptr);
	CHECK(patched->get_global_position().is_equal_approx(Vector3(20, 30, 40)));
	CHECK_FALSE(patched->is_set_as_top_level());
	CHECK(harness.undo_redo->get_history_count() == 1);
	REQUIRE(harness.undo_redo->undo());
	CHECK(harness.version() == before_version);
	CHECK(child->get_parent() == harness.root);
	CHECK(child->get_position().is_equal_approx(Vector3(1, 2, 3)));
	REQUIRE(harness.undo_redo->redo());
	patched = Object::cast_to<Node3D>(parent->get_child(0));
	CHECK(patched->get_global_position().is_equal_approx(Vector3(20, 30, 40)));
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Singular planned parents reject global writes without mutation") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	harness.root->set_scale(Vector3(0, 1, 1));
	Node3D *child = memnew(Node3D);
	child->set_name("Child");
	harness.root->add_child(child);
	child->set_owner(harness.root);
	const String before_version = harness.version();
	Array operations;
	bool rejected = true;
	SUBCASE("global conversion is rejected") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":"Child","properties":{"global_position":[20,30,40]}}])");
	}
	SUBCASE("unchanged top_level flag needs no inverse and allows a local update") {
		rejected = false;
		operations = JSON::parse_string(R"([{"op":"update_node","node":"Child","properties":{"top_level":false,"position":[1,2,3]}}])");
	}
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	if (rejected) {
		CHECK(result.is_empty());
		CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
		CHECK(harness.version() == before_version);
		CHECK(harness.undo_redo->get_history_count() == 0);
	} else {
		REQUIRE(error_code == 0);
		REQUIRE_FALSE(result.is_empty());
		CHECK(child->get_position().is_equal_approx(Vector3(1, 2, 3)));
		REQUIRE(harness.undo_redo->undo());
		CHECK(harness.version() == before_version);
	}
}

#ifdef MODULE_GDSCRIPT_ENABLED
TEST_CASE("[GodotMCP][ScenePatch] Script attachment precedes exported values and rolls back together") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("extends Node3D\n@export var health: int = 1\n");
	REQUIRE(script->reload() == OK);
	PatchHarness harness;
	Dictionary properties;
	properties["health"] = 7;
	properties["script"] = script;
	Dictionary operation = _create_node("scripted", ".", "Scripted", "Node3D", properties);
	SUBCASE("object form resolves script dependencies") {}
	SUBCASE("ordered entries retain script-first and repeated assignments") {
		operation.erase("properties");
		Array entries;
		Dictionary script_entry;
		script_entry["name"] = "script";
		script_entry["value"] = script;
		entries.push_back(script_entry);
		Dictionary health_entry;
		health_entry["name"] = "health";
		health_entry["value"] = 3;
		entries.push_back(health_entry);
		health_entry = health_entry.duplicate();
		health_entry["value"] = 7;
		entries.push_back(health_entry);
		operation["property_entries"] = entries;
	}
	SUBCASE("update attaches a script and sets exports in one operation") {
		operation["op"] = "update_node";
		operation["node"] = ".";
	}
	Array operations;
	operations.push_back(operation);
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	Node *scripted = String(operation["op"]) == "update_node" ? harness.root : harness.root->get_node_or_null(NodePath("Scripted"));
	REQUIRE(scripted != nullptr);
	CHECK(int(scripted->get("health")) == 7);
	REQUIRE(harness.undo_redo->undo());
	CHECK(harness.root->get_script().is_null());
	CHECK(harness.root->get_child_count() == 0);
	REQUIRE(harness.undo_redo->redo());
	scripted = String(operation["op"]) == "update_node" ? harness.root : harness.root->get_node_or_null(NodePath("Scripted"));
	CHECK(int(scripted->get("health")) == 7);
}

TEST_CASE("[GodotMCP][ScenePatch] Script replacement restores instance values on undo and failed planning") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> old_script;
	old_script.instantiate();
	old_script->set_source_code("extends Node3D\n@export var health: int = 1\n");
	REQUIRE(old_script->reload() == OK);
	Ref<GDScript> new_script;
	new_script.instantiate();
	new_script->set_source_code("extends Node3D\n@export var health: int = 2\n");
	REQUIRE(new_script->reload() == OK);
	PatchHarness harness;
	harness.root->set_script(old_script);
	harness.root->set("health", 7);
	const String before_version = harness.version();
	Dictionary operation;
	operation["op"] = "update_node";
	operation["node"] = ".";
	Dictionary script_entry;
	script_entry["name"] = "script";
	script_entry["value"] = new_script;
	Dictionary health_entry;
	health_entry["name"] = "health";
	health_entry["value"] = 9;
	Array entries;
	entries.push_back(script_entry);
	entries.push_back(health_entry);
	Array operations;
	bool best_effort = false;
	int expected_health = 9;
	Ref<GDScript> expected_script = new_script;
	SUBCASE("replacement restores existing exported values on undo") {}
	SUBCASE("repeated replacements retain state at each explicit assignment") {
		script_entry = script_entry.duplicate();
		script_entry["value"] = old_script;
		entries.push_back(script_entry);
		health_entry = health_entry.duplicate();
		health_entry["value"] = 11;
		entries.push_back(health_entry);
		expected_health = 11;
		expected_script = old_script;
	}
	SUBCASE("failed replacement rolls back the shadow before an independent write") {
		best_effort = true;
		Dictionary invalid_entry;
		invalid_entry["name"] = "unknown_property";
		invalid_entry["value"] = true;
		entries.push_back(invalid_entry);
		expected_health = 11;
		expected_script = old_script;
	}
	operation["property_entries"] = entries;
	operations.push_back(operation);
	if (best_effort) {
		operations.push_back(JSON::parse_string(R"({"op":"update_node","node":".","properties":{"health":11}})"));
	}
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message, String(), false, best_effort ? "best_effort" : "atomic");
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	CHECK(Ref<GDScript>(harness.root->get_script()) == expected_script);
	CHECK(int(harness.root->get("health")) == expected_health);
	CHECK(int(result["success_count"]) == 1);
	CHECK(int(result["failure_count"]) == (best_effort ? 1 : 0));
	REQUIRE(harness.undo_redo->undo());
	CHECK(Ref<GDScript>(harness.root->get_script()) == old_script);
	CHECK(int(harness.root->get("health")) == 7);
	CHECK(harness.version() == before_version);
	REQUIRE(harness.undo_redo->redo());
	CHECK(Ref<GDScript>(harness.root->get_script()) == expected_script);
	CHECK(int(harness.root->get("health")) == expected_health);
}

TEST_CASE("[GodotMCP][ScenePatch] Failed embedded script deletion restores exports before independent writes") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code("extends Node3D\n@export var health: int = 1\n");
	REQUIRE(script->reload() == OK);
	PatchHarness harness;
	const String script_path = harness.scene_path + "::RollbackScript";
	script->set_path(script_path);
	harness.root->set_script(script);
	harness.root->set("health", 7);
	Variant nested = true;
	for (int i = 0; i < 20; i++) {
		Array level;
		level.push_back(nested);
		nested = level;
	}
	harness.root->set_meta("nested", nested);
	const String before_version = harness.version();
	Dictionary deletion;
	deletion["op"] = "delete_resource";
	deletion["resource_path"] = script_path;
	Array operations;
	operations.push_back(deletion);
	operations.push_back(JSON::parse_string(R"({"op":"update_node","node":".","properties":{"health":11}})"));
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message, String(), false, "best_effort");
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	CHECK(int(result["failure_count"]) == 1);
	CHECK(int(result["success_count"]) == 1);
	CHECK(Ref<GDScript>(harness.root->get_script()) == script);
	CHECK(int(harness.root->get("health")) == 11);
	REQUIRE(harness.undo_redo->undo());
	CHECK(int(harness.root->get("health")) == 7);
	CHECK(harness.version() == before_version);
}
#endif

TEST_CASE("[GodotMCP][ScenePatch][Editor] Best effort isolates final NodePath failures and their dependents") {
	PatchHarness harness;
	harness.root->set_meta("independent", 0);
	const String before_version = harness.version();
	const Array operations = JSON::parse_string(R"([
		{"op":"create_node","id":"target","parent":".","name":"Target","type":"Node3D"},
		{"op":"create_node","id":"source","parent":".","name":"Source","type":"GPUParticles3D","properties":{"sub_emitter":{"node_ref":"target"}}},
		{"op":"create_node","id":"dependent","parent":{"node_ref":"source"},"name":"Dependent","type":"Node3D"},
		{"op":"delete_node","node":{"node_ref":"target"}},
		{"op":"update_node","node":".","properties":{"metadata/independent":42}}
	])");
	int error_code = 0;
	String error_message;
	bool dry_run = false;
	SUBCASE("apply independent operations with one undo") {}
	SUBCASE("dry run retains the same ordered classifications without mutation") {
		dry_run = true;
	}
	const Dictionary result = harness.apply(operations, error_code, error_message, String(), dry_run, "best_effort");
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	CHECK(int(result["success_count"]) == 3);
	CHECK(int(result["failure_count"]) == 1);
	CHECK(int(result["skipped_count"]) == 1);
	const Array operation_results = result["operations"];
	REQUIRE(operation_results.size() == 5);
	for (int i = 0; i < operation_results.size(); i++) {
		CHECK(int(Dictionary(operation_results[i])["index"]) == i);
	}
	CHECK(String(Dictionary(operation_results[1])["status"]) == "failed");
	CHECK(String(Dictionary(Dictionary(operation_results[1])["error"])["message"]).contains("deleted"));
	CHECK(String(Dictionary(operation_results[2])["status"]) == "skipped");
	CHECK(harness.root->get_child_count() == 0);
	CHECK(int(harness.root->get_meta("independent")) == (dry_run ? 0 : 42));
	CHECK(harness.undo_redo->get_history_count() == (dry_run ? 0 : 1));
	if (!dry_run) {
		REQUIRE(harness.undo_redo->undo());
		CHECK(harness.version() == before_version);
		REQUIRE(harness.undo_redo->redo());
		CHECK(int(harness.root->get_meta("independent")) == 42);
		CHECK(harness.root->get_child_count() == 0);
	}
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Node2D global setters match live transformed parents and preserve undo and packing") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Node2D *parent = memnew(Node2D);
	parent->set_name("Parent");
	harness.root->add_child(parent);
	parent->set_owner(harness.root);
	parent->set_transform(Transform2D(0.35, Vector2(2, 3), 0.2, Vector2(10, 15)));
	Node2D *child = memnew(Node2D);
	child->set_name("Child");
	parent->add_child(child);
	child->set_owner(harness.root);
	child->set_transform(Transform2D(0.1, Vector2(1.2, 0.8), 0.15, Vector2(1, 2)));
	Node2D *reference = memnew(Node2D);
	parent->add_child(reference);
	reference->set_transform(child->get_transform());
	String property;
	Variant requested;
	SUBCASE("position") {
		property = "global_position";
		requested = Vector2(20, 30);
	}
	SUBCASE("rotation") {
		property = "global_rotation";
		requested = 0.8;
	}
	SUBCASE("rotation degrees") {
		property = "global_rotation_degrees";
		requested = 45.0;
	}
	SUBCASE("scale") {
		property = "global_scale";
		requested = Vector2(4, 5);
	}
	SUBCASE("skew") {
		property = "global_skew";
		requested = 0.3;
	}
	SUBCASE("transform") {
		property = "global_transform";
		requested = Transform2D(0.7, Vector2(4, 2), 0.1, Vector2(20, 30));
	}
	reference->set(property, requested);
	const Transform2D expected = reference->get_transform();
	const Transform2D before = child->get_transform();
	const String before_version = harness.version();
	Dictionary properties;
	properties[property] = requested;
	Dictionary update;
	update["op"] = "update_node";
	update["node"] = "Parent/Child";
	update["properties"] = properties;
	Array operations;
	operations.push_back(update);
	int error_code = 0;
	String error_message;
	REQUIRE_FALSE(harness.apply(operations, error_code, error_message, String(), true).is_empty());
	REQUIRE(error_code == 0);
	CHECK(child->get_transform().is_equal_approx(before));
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->get_history_count() == 0);
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	CHECK(child->get_transform().is_equal_approx(expected));
	CHECK(child->get_global_transform().is_equal_approx(reference->get_global_transform()));
	REQUIRE(harness.undo_redo->undo());
	CHECK(child->get_transform().is_equal_approx(before));
	CHECK(harness.version() == before_version);
	REQUIRE(harness.undo_redo->redo());
	CHECK(child->get_transform().is_equal_approx(expected));
	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(harness.root) == OK);
	Node *instance = packed->instantiate();
	REQUIRE(instance != nullptr);
	Node2D *reloaded_child = Object::cast_to<Node2D>(instance->get_node_or_null(NodePath("Parent/Child")));
	REQUIRE(reloaded_child != nullptr);
	CHECK(reloaded_child->get_transform().is_equal_approx(expected));
	memdelete(instance);
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Node2D global writes follow planned ancestors and top level changes") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Node2D *parent = memnew(Node2D);
	parent->set_name("Parent");
	harness.root->add_child(parent);
	parent->set_owner(harness.root);
	parent->set_position(Vector2(10, 15));
	const String before_version = harness.version();
	const Array operations = JSON::parse_string(R"([
		{"op":"update_node","node":"Parent","properties":{"position":[15,20]}},
		{"op":"create_node","id":"child","parent":"Parent","name":"Child","type":"Node2D","property_entries":[
			{"name":"position","value":[1,2]}, {"name":"global_position","value":[20,30]},
			{"name":"top_level","value":true}, {"name":"global_position","value":[99,99]},
			{"name":"top_level","value":false}, {"name":"global_position","value":[20,30]}
		]}
	])");
	int error_code = 0;
	String error_message;
	const Dictionary result = harness.apply(operations, error_code, error_message);
	INFO(error_message.utf8().get_data());
	REQUIRE(error_code == 0);
	REQUIRE_FALSE(result.is_empty());
	Node2D *child = Object::cast_to<Node2D>(parent->get_child(0));
	REQUIRE(child != nullptr);
	CHECK(child->get_position().is_equal_approx(Vector2(5, 10)));
	CHECK(child->get_global_position().is_equal_approx(Vector2(20, 30)));
	REQUIRE(harness.undo_redo->undo());
	CHECK(harness.version() == before_version);
	REQUIRE(harness.undo_redo->redo());
	child = Object::cast_to<Node2D>(parent->get_child(0));
	CHECK(child->get_global_position().is_equal_approx(Vector2(20, 30)));
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Node2D transform undo restores exact cached component values") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Node2D *child = memnew(Node2D);
	child->set_name("Child");
	harness.root->add_child(child);
	child->set_owner(harness.root);
	child->set_rotation(0.1);
	child->set_skew(0.15);
	child->set_scale(Vector2(1.2, 0.8));
	child->set_position(Vector2(1, 2));
	const String before_version = harness.version();
	const real_t rotation = child->get_rotation();
	const real_t skew = child->get_skew();
	const Vector2 scale = child->get_scale();
	Dictionary properties;
	properties["global_transform"] = Transform2D(0.7, Vector2(4, 2), 0.1, Vector2(20, 30));
	Dictionary update;
	update["op"] = "update_node";
	update["node"] = "Child";
	update["properties"] = properties;
	Array operations;
	operations.push_back(update);
	int error_code = 0;
	String error_message;
	REQUIRE_FALSE(harness.apply(operations, error_code, error_message).is_empty());
	REQUIRE(error_code == 0);
	REQUIRE(harness.undo_redo->undo());
	CHECK(child->get_rotation() == rotation);
	CHECK(child->get_skew() == skew);
	CHECK(child->get_scale() == scale);
	CHECK(harness.version() == before_version);
}

TEST_CASE("[SceneTree][GodotMCP][ScenePatch] Global writes reject unsupported Control layout dependencies") {
	PatchHarness harness;
	SceneTree::get_singleton()->get_root()->add_child(harness.root);
	Control *control = memnew(Control);
	control->set_name("Control");
	harness.root->add_child(control);
	control->set_owner(harness.root);
	Node2D *child = memnew(Node2D);
	child->set_name("Child");
	control->add_child(child);
	child->set_owner(harness.root);
	const String before_version = harness.version();
	Array operations;
	SUBCASE("Control global position") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":"Control","properties":{"global_position":[20,30]}}])");
	}
	SUBCASE("Node2D under a planned Control layout") {
		operations = JSON::parse_string(R"([{"op":"update_node","node":"Control/Child","properties":{"global_position":[20,30]}}])");
	}
	int error_code = 0;
	String error_message;
	CHECK(harness.apply(operations, error_code, error_message).is_empty());
	CHECK(error_code == GodotMCPError::INVALID_ARGUMENT);
	CHECK(error_message.contains("layout"));
	CHECK(harness.version() == before_version);
	CHECK(harness.undo_redo->get_history_count() == 0);
}

} // namespace TestGodotMCPScenePatch

#endif // TOOLS_ENABLED
