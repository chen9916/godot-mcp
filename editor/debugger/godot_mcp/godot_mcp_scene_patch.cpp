/**************************************************************************/
/*  godot_mcp_scene_patch.cpp                                             */
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

#include "godot_mcp_scene_patch.h"

#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_instance.h"
#include "core/object/undo_redo.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/callable.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/2d/node_2d.h"
#include "scene/3d/node_3d.h"
#include "scene/animation/animation_mixer.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"

namespace {

constexpr int MAX_PATCH_SCENE_NODES = 10000;
constexpr int MAX_PATCH_VALUE_DEPTH = 16;
constexpr int MAX_PATCH_CONTAINER_ITEMS = 10000;
constexpr const char *PATCH_UNDO_ACTION = "Godot MCP: Apply Scene Patch";

enum PatchMode {
	PATCH_MODE_ATOMIC,
	PATCH_MODE_BEST_EFFORT,
};

enum PatchOperationKind {
	PATCH_CREATE_RESOURCE,
	PATCH_UPDATE_RESOURCE,
	PATCH_DELETE_RESOURCE,
	PATCH_CREATE_NODE,
	PATCH_UPDATE_NODE,
	PATCH_MOVE_NODE,
	PATCH_DELETE_NODE,
};

struct NodeState {
	Node *node = nullptr;
	Node *owner = nullptr;
	NodeState *parent = nullptr;
	Vector<NodeState *> children;
	String name;
	String local_id;
	bool created = false;
	bool deleted = false;
	bool internal = false;
	HashSet<StringName> groups;
};

struct PropertyWrite {
	Object *target = nullptr;
	Object *shadow = nullptr;
	StringName property;
	Variant before;
	Variant after;
	Variant encoded;
	bool contains_node_ref = false;
	bool top_level_keep_local = false;
	Vector<Pair<StringName, Variant>> before_script_state;
	Vector<Pair<StringName, Variant>> after_script_state;
	Vector<Pair<StringName, Variant>> before_node_2d_state;
};

struct OwnerWrite {
	Node *node = nullptr;
	Node *before = nullptr;
	Node *after = nullptr;
};

struct PatchOperation {
	int index = -1;
	PatchOperationKind kind = PATCH_UPDATE_NODE;
	String name;
	String local_id;
	Vector<PropertyWrite> property_writes;

	NodeState *node_state = nullptr;
	NodeState *old_parent_state = nullptr;
	NodeState *new_parent_state = nullptr;
	int old_position = -1;
	int new_position = -1;
	String old_name;
	String new_name;
	Vector<OwnerWrite> owner_writes;
	Vector<StringName> groups_added;
	Vector<StringName> groups_removed;

	Ref<Resource> resource;
	String resource_unique_id;
	bool resource_deleted = false;

	Dictionary summary;
};

static void _hash_bytes(uint64_t &r_hash, const uint8_t *p_bytes, int p_size) {
	for (int i = 0; i < p_size; i++) {
		r_hash ^= p_bytes[i];
		r_hash *= UINT64_C(1099511628211);
	}
}

static void _hash_string(uint64_t &r_hash, const String &p_string) {
	const CharString utf8 = p_string.utf8();
	_hash_bytes(r_hash, reinterpret_cast<const uint8_t *>(utf8.get_data()), utf8.length());
	const uint8_t separator = 0xff;
	_hash_bytes(r_hash, &separator, 1);
}

static void _hash_uint64(uint64_t &r_hash, uint64_t p_value) {
	uint8_t bytes[8];
	for (int i = 0; i < 8; i++) {
		bytes[i] = uint8_t(p_value >> (i * 8));
	}
	_hash_bytes(r_hash, bytes, 8);
}

struct SceneVersionHashContext {
	Node *root = nullptr;
	HashMap<ObjectID, String> resource_anchors;
	HashSet<ObjectID> active_resources;
	HashSet<uint64_t> active_containers;
	HashMap<String, String> file_digests;
};

struct SortedDictionaryEntry {
	Variant key;
	Variant value;
	String sort_key;
};

struct SortedLiveConnection {
	Object::Connection connection;
	String sort_key;
};

struct SortedPersistentChild {
	Node *node = nullptr;
	String path;
	int persistent_index = -1;
};

static String _stable_file_digest(SceneVersionHashContext &r_context, const String &p_path) {
	if (const String *cached = r_context.file_digests.getptr(p_path)) {
		return *cached;
	}
	const String digest = FileAccess::exists(p_path) ? FileAccess::get_md5(p_path) : String("missing");
	r_context.file_digests.insert(p_path, digest);
	return digest;
}

static String _variant_sort_key(const Variant &p_value, Node *p_root, int p_depth = 0) {
	String key = itos(p_value.get_type()) + ":";
	if (p_depth >= MAX_PATCH_VALUE_DEPTH) {
		return key + "depth_limit";
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
			return key + "null";
		case Variant::OBJECT: {
			Object *object = p_value;
			if (!object) {
				return key + "null";
			}
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				return key + resource->get_class() + ":" + resource->get_path() + ":" + resource->get_scene_unique_id();
			}
			if (Node *node = Object::cast_to<Node>(object)) {
				return key + (p_root && (node == p_root || p_root->is_ancestor_of(node)) ? String(p_root->get_path_to(node)) : String("outside_scene:" + node->get_class()));
			}
			return key + object->get_class();
		}
		case Variant::ARRAY: {
			const Array array = p_value;
			key += "[";
			for (int i = 0; i < array.size(); i++) {
				key += _variant_sort_key(array[i], p_root, p_depth + 1) + ";";
			}
			return key + "]";
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			Vector<String> entries;
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				entries.push_back(_variant_sort_key(E.key, p_root, p_depth + 1) + "=" + _variant_sort_key(E.value, p_root, p_depth + 1));
			}
			entries.sort();
			key += "{";
			for (const String &entry : entries) {
				key += entry + ";";
			}
			return key + "}";
		}
		default:
			return key + p_value.get_construct_string();
	}
}

static void _hash_variant(uint64_t &r_hash, const Variant &p_value, SceneVersionHashContext &r_context,
		const String &p_anchor, int p_depth);

static void _hash_resource(uint64_t &r_hash, const Ref<Resource> &p_resource, SceneVersionHashContext &r_context,
		const String &p_anchor, int p_depth) {
	if (p_resource.is_null()) {
		_hash_string(r_hash, "null_resource");
		return;
	}

	const String path = p_resource->get_path();
	const bool external = !path.is_empty() && !path.contains("::");
	const ObjectID resource_id = p_resource->get_instance_id();
	String stable_anchor;
	if (const String *existing_anchor = r_context.resource_anchors.getptr(resource_id)) {
		stable_anchor = *existing_anchor;
	} else {
		if (external) {
			stable_anchor = "external:" + path;
		} else if (!p_resource->get_scene_unique_id().is_empty()) {
			stable_anchor = "local-id:" + p_resource->get_scene_unique_id();
		} else if (!path.is_empty()) {
			stable_anchor = "local-path:" + path;
		} else {
			stable_anchor = "local-anchor:" + p_anchor;
		}
		r_context.resource_anchors.insert(resource_id, stable_anchor);
	}

	_hash_string(r_hash, p_resource->get_class());
	_hash_string(r_hash, stable_anchor);
	if (external) {
		_hash_string(r_hash, _stable_file_digest(r_context, path));
	}
	if (p_depth >= MAX_PATCH_VALUE_DEPTH) {
		_hash_string(r_hash, "resource_depth_limit");
		return;
	}

	if (r_context.active_resources.has(resource_id)) {
		_hash_string(r_hash, "resource_ref:" + stable_anchor);
		return;
	}
	r_context.active_resources.insert(resource_id);

	Vector<StringName> property_names;
	List<PropertyInfo> property_list;
	p_resource->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			property_names.push_back(property.name);
		}
	}
	property_names.sort();
	for (const StringName &property_name : property_names) {
		bool valid = false;
		const Variant value = p_resource->get(property_name, &valid);
		if (!valid) {
			continue;
		}
		_hash_string(r_hash, property_name);
		_hash_variant(r_hash, value, r_context, stable_anchor + "/" + String(property_name), p_depth + 1);
	}
	r_context.active_resources.erase(resource_id);
}

static void _hash_variant(uint64_t &r_hash, const Variant &p_value, SceneVersionHashContext &r_context,
		const String &p_anchor, int p_depth) {
	_hash_uint64(r_hash, p_value.get_type());
	if (p_depth >= MAX_PATCH_VALUE_DEPTH) {
		_hash_string(r_hash, "value_depth_limit");
		return;
	}

	switch (p_value.get_type()) {
		case Variant::NIL:
			return;
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			_hash_string(r_hash, String(p_value));
			return;
		case Variant::ARRAY: {
			const Array array = p_value;
			const uint64_t container_id = uint64_t(reinterpret_cast<uintptr_t>(array.id()));
			if (r_context.active_containers.has(container_id)) {
				_hash_string(r_hash, "array_cycle");
				return;
			}
			r_context.active_containers.insert(container_id);
			_hash_uint64(r_hash, array.size());
			for (int i = 0; i < array.size(); i++) {
				_hash_variant(r_hash, array[i], r_context, p_anchor + "/[" + itos(i) + "]", p_depth + 1);
			}
			r_context.active_containers.erase(container_id);
			return;
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			const uint64_t container_id = uint64_t(reinterpret_cast<uintptr_t>(dictionary.id()));
			if (r_context.active_containers.has(container_id)) {
				_hash_string(r_hash, "dictionary_cycle");
				return;
			}
			r_context.active_containers.insert(container_id);
			Vector<SortedDictionaryEntry> entries;
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				SortedDictionaryEntry entry;
				entry.key = E.key;
				entry.value = E.value;
				entry.sort_key = _variant_sort_key(E.key, r_context.root) + "=" + _variant_sort_key(E.value, r_context.root);
				entries.push_back(entry);
			}
			struct EntryComparator {
				_FORCE_INLINE_ bool operator()(const SortedDictionaryEntry &p_left, const SortedDictionaryEntry &p_right) const {
					return p_left.sort_key < p_right.sort_key;
				}
			};
			entries.sort_custom<EntryComparator>();
			_hash_uint64(r_hash, dictionary.size());
			for (const SortedDictionaryEntry &entry : entries) {
				_hash_variant(r_hash, entry.key, r_context, p_anchor + "/key:" + entry.sort_key, p_depth + 1);
				_hash_variant(r_hash, entry.value, r_context, p_anchor + "/value:" + entry.sort_key, p_depth + 1);
			}
			r_context.active_containers.erase(container_id);
			return;
		}
		case Variant::OBJECT: {
			Object *object = p_value;
			if (!object) {
				_hash_string(r_hash, "null_object");
				return;
			}
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				_hash_resource(r_hash, Ref<Resource>(resource), r_context, p_anchor, p_depth + 1);
				return;
			}
			if (Node *node = Object::cast_to<Node>(object)) {
				_hash_string(r_hash, r_context.root && (node == r_context.root || r_context.root->is_ancestor_of(node)) ? String(r_context.root->get_path_to(node)) : String("outside_scene:" + node->get_class()));
				return;
			}
			_hash_string(r_hash, object->get_class());
			return;
		}
		case Variant::CALLABLE: {
			const Callable callable = p_value;
			_hash_string(r_hash, callable.get_method());
			Object *target = callable.get_object();
			if (Node *node = Object::cast_to<Node>(target)) {
				_hash_string(r_hash, r_context.root && (node == r_context.root || r_context.root->is_ancestor_of(node)) ? String(r_context.root->get_path_to(node)) : String("outside_scene:" + node->get_class()));
			} else {
				_hash_string(r_hash, target ? target->get_class() : String("null"));
			}
			_hash_variant(r_hash, callable.get_bound_arguments(), r_context, p_anchor + "/binds", p_depth + 1);
			return;
		}
		case Variant::SIGNAL: {
			const Signal signal = p_value;
			_hash_string(r_hash, signal.get_name());
			Object *target = signal.get_object();
			if (Node *node = Object::cast_to<Node>(target)) {
				_hash_string(r_hash, r_context.root && (node == r_context.root || r_context.root->is_ancestor_of(node)) ? String(r_context.root->get_path_to(node)) : String("outside_scene:" + node->get_class()));
			} else {
				_hash_string(r_hash, target ? target->get_class() : String("null"));
			}
			return;
		}
		case Variant::RID:
			// RIDs are server-side handles and are never stable scene content.
			_hash_string(r_hash, "rid");
			return;
		default:
			_hash_uint64(r_hash, p_value.recursive_hash(0));
			return;
	}
}

static void _hash_node(uint64_t &r_hash, Node *p_node, SceneVersionHashContext &r_context, int &r_node_count) {
	if (!p_node) {
		return;
	}
	r_node_count++;
	const String node_path = p_node == r_context.root ? String(".") : String(r_context.root->get_path_to(p_node));

	_hash_string(r_hash, node_path);
	_hash_string(r_hash, p_node->get_class());
	_hash_string(r_hash, p_node->get_name());
	_hash_string(r_hash, p_node->get_scene_file_path());
	if (!p_node->get_scene_file_path().is_empty()) {
		_hash_string(r_hash, _stable_file_digest(r_context, p_node->get_scene_file_path()));
	}
	_hash_uint64(r_hash, p_node->is_internal());
	_hash_uint64(r_hash, p_node != r_context.root && r_context.root->is_editable_instance(p_node));
	if (p_node->get_owner()) {
		_hash_string(r_hash, p_node->get_owner() == r_context.root ? String(".") : String(r_context.root->get_path_to(p_node->get_owner())));
	}

	Vector<StringName> groups;
	List<Node::GroupInfo> group_list;
	p_node->get_groups(&group_list);
	for (const Node::GroupInfo &group : group_list) {
		if (group.persistent) {
			groups.push_back(group.name);
		}
	}
	groups.sort();
	_hash_uint64(r_hash, groups.size());
	for (const StringName &group : groups) {
		_hash_string(r_hash, group);
	}

	Vector<SortedLiveConnection> connections;
	List<Object::Connection> connection_list;
	p_node->get_all_signal_connections(&connection_list);
	for (const Object::Connection &connection : connection_list) {
		if (!(connection.flags & Object::CONNECT_PERSIST)) {
			continue;
		}
		SortedLiveConnection sorted;
		sorted.connection = connection;
		Object *target = connection.callable.get_object();
		String target_key = target ? target->get_class() : String("null");
		if (Node *target_node = Object::cast_to<Node>(target)) {
			target_key = target_node == r_context.root || r_context.root->is_ancestor_of(target_node) ? String(r_context.root->get_path_to(target_node)) : String("outside_scene:" + target_node->get_class());
		}
		sorted.sort_key = String(connection.signal.get_name()) + "->" + target_key + ":" + String(connection.callable.get_method()) + ":" +
				itos(connection.flags) + ":" + _variant_sort_key(connection.callable.get_bound_arguments(), r_context.root);
		connections.push_back(sorted);
	}
	struct ConnectionComparator {
		_FORCE_INLINE_ bool operator()(const SortedLiveConnection &p_left, const SortedLiveConnection &p_right) const {
			return p_left.sort_key < p_right.sort_key;
		}
	};
	connections.sort_custom<ConnectionComparator>();
	_hash_uint64(r_hash, connections.size());
	for (const SortedLiveConnection &sorted : connections) {
		_hash_string(r_hash, sorted.sort_key);
		_hash_variant(r_hash, sorted.connection.callable.get_bound_arguments(), r_context, node_path + "/connection:" + sorted.sort_key, 0);
	}

	Vector<StringName> property_names;
	List<PropertyInfo> property_list;
	p_node->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			property_names.push_back(property.name);
		}
	}
	property_names.sort();
	for (const StringName &property_name : property_names) {
		bool valid = false;
		const Variant value = p_node->get(property_name, &valid);
		if (!valid) {
			continue;
		}
		_hash_string(r_hash, property_name);
		_hash_variant(r_hash, value, r_context, node_path + "/property:" + String(property_name), 0);
	}

	Vector<SortedPersistentChild> persistent_children;
	int persistent_index = 0;
	for (int i = 0; i < p_node->get_child_count(true); i++) {
		Node *child = p_node->get_child(i, true);
		if (child && child->get_owner()) {
			SortedPersistentChild sorted;
			sorted.node = child;
			sorted.path = String(r_context.root->get_path_to(child));
			sorted.persistent_index = persistent_index++;
			persistent_children.push_back(sorted);
		}
	}
	struct ChildComparator {
		_FORCE_INLINE_ bool operator()(const SortedPersistentChild &p_left, const SortedPersistentChild &p_right) const {
			return p_left.path < p_right.path;
		}
	};
	persistent_children.sort_custom<ChildComparator>();
	_hash_uint64(r_hash, persistent_children.size());
	for (const SortedPersistentChild &child : persistent_children) {
		_hash_string(r_hash, child.path);
		_hash_uint64(r_hash, child.persistent_index);
		_hash_node(r_hash, child.node, r_context, r_node_count);
	}
}

static bool _parse_mode(const Dictionary &p_params, PatchMode &r_mode, String &r_error_message) {
	const Variant mode_variant = p_params.get("mode", String("atomic"));
	if (mode_variant.get_type() != Variant::STRING && mode_variant.get_type() != Variant::STRING_NAME) {
		r_error_message = "INVALID_ARGUMENT: mode must be one of: atomic, best_effort.";
		return false;
	}
	const String mode = mode_variant;
	if (mode == "atomic") {
		r_mode = PATCH_MODE_ATOMIC;
		return true;
	}
	if (mode == "best_effort") {
		r_mode = PATCH_MODE_BEST_EFFORT;
		return true;
	}
	r_error_message = "INVALID_ARGUMENT: mode must be one of: atomic, best_effort.";
	return false;
}

static bool _collect_properties(const Dictionary &p_operation, Dictionary &r_properties, String &r_error_message) {
	if (p_operation.has("properties") && p_operation.has("property_entries")) {
		r_error_message = "INVALID_ARGUMENT: provide either properties or property_entries, not both.";
		return false;
	}

	const Variant properties_variant = p_operation.get("properties", Dictionary());
	if (properties_variant.get_type() != Variant::DICTIONARY) {
		r_error_message = "INVALID_ARGUMENT: properties must be a dictionary.";
		return false;
	}
	r_properties = properties_variant;

	const Variant entries_variant = p_operation.get("property_entries", Array());
	if (entries_variant.get_type() != Variant::ARRAY) {
		r_error_message = "INVALID_ARGUMENT: property_entries must be an array.";
		return false;
	}
	const Array entries = entries_variant;
	for (int i = 0; i < entries.size(); i++) {
		if (entries[i].get_type() != Variant::DICTIONARY) {
			r_error_message = "INVALID_ARGUMENT: property_entries entries must be dictionaries.";
			return false;
		}
		const Dictionary entry = entries[i];
		const Variant name_variant = entry.get("name", entry.get("property", Variant()));
		if ((name_variant.get_type() != Variant::STRING && name_variant.get_type() != Variant::STRING_NAME) || String(name_variant).is_empty() || !entry.has("value")) {
			r_error_message = "INVALID_ARGUMENT: each property_entries entry requires a non-empty string name and value.";
			return false;
		}
		r_properties[String(name_variant)] = entry["value"];
	}
	return true;
}

static bool _find_property_info(const Object *p_object, const StringName &p_name, PropertyInfo &r_info) {
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == p_name) {
			r_info = property;
			return true;
		}
	}
#ifndef DISABLE_DEPRECATED
	// AnimationMixer retains a dictionary setter for the legacy aggregate
	// property, but exposes only libraries/<name> entries in its dynamic list.
	// Treating the setter as a declared compatibility property lets a patch
	// author a complete AnimationLibrary graph without procedural method calls.
	if (p_name == SNAME("libraries") && Object::cast_to<AnimationMixer>(p_object)) {
		r_info = PropertyInfo(Variant::DICTIONARY, p_name, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE);
		return true;
	}
#endif
	return false;
}

static Variant _get_property_for_patch(Object *p_object, const StringName &p_name, bool &r_valid) {
	Variant value = p_object->get(p_name, &r_valid);
#ifndef DISABLE_DEPRECATED
	if (!r_valid && p_name == SNAME("libraries")) {
		if (AnimationMixer *mixer = Object::cast_to<AnimationMixer>(p_object)) {
			Dictionary libraries;
			LocalVector<StringName> names;
			mixer->get_animation_library_list(&names);
			for (const StringName &name : names) {
				libraries[name] = mixer->get_animation_library(name);
			}
			value = libraries;
			r_valid = true;
		}
	}
#endif
	return value;
}

static String _resource_type_hint(const PropertyInfo &p_info) {
	if (p_info.type == Variant::OBJECT && p_info.hint == PROPERTY_HINT_RESOURCE_TYPE) {
		return p_info.hint_string;
	}
	if (p_info.type == Variant::OBJECT && p_info.name == SNAME("script")) {
		return "Script";
	}
	return String();
}

static bool _resource_is_compatible(const Ref<Resource> &p_resource, const String &p_expected_types) {
	if (p_resource.is_null() || p_expected_types.is_empty()) {
		return true;
	}
	const PackedStringArray accepted_types = p_expected_types.split(",");
	for (int i = 0; i < accepted_types.size(); i++) {
		const String type_name = accepted_types[i].strip_edges();
		if (!type_name.is_empty() && p_resource->is_class(type_name)) {
			return true;
		}
	}
	return false;
}

static bool _variant_to_real(const Variant &p_value, real_t &r_value) {
	if (p_value.get_type() == Variant::FLOAT) {
		r_value = double(p_value);
		return true;
	}
	if (p_value.get_type() == Variant::INT) {
		r_value = int64_t(p_value);
		return true;
	}
	return false;
}

static bool _variant_to_integer(const Variant &p_value, int64_t p_min, int64_t p_max, int64_t &r_value) {
	int64_t value;
	if (p_value.get_type() == Variant::INT) {
		value = int64_t(p_value);
	} else if (p_value.get_type() == Variant::FLOAT) {
		// JSON numbers arrive as doubles, even without a fractional part.
		// double(INT64_MAX) rounds up to 2^63, so check that exclusive bound
		// before casting to avoid an out-of-range conversion.
		const double number = double(p_value);
		const double int64_limit = 9223372036854775808.0;
		if (!Math::is_finite(number) || Math::floor(number) != number || number < -int64_limit || number >= int64_limit) {
			return false;
		}
		value = int64_t(number);
	} else {
		return false;
	}
	if (value < p_min || value > p_max) {
		return false;
	}
	r_value = value;
	return true;
}

static bool _array_to_reals(const Variant &p_value, int p_count, real_t *r_values) {
	if (p_value.get_type() != Variant::ARRAY) {
		return false;
	}
	const Array values = p_value;
	if (values.size() != p_count) {
		return false;
	}
	for (int i = 0; i < p_count; i++) {
		if (!_variant_to_real(values[i], r_values[i])) {
			return false;
		}
	}
	return true;
}

static bool _validate_node_property(Node *p_node, const StringName &p_property_name, const Variant &p_value, String &r_error_message) {
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	if (!node_3d) {
		return true;
	}

	const String property_name = p_property_name;
	if (property_name == "scale") {
		if (p_value.get_type() != Variant::VECTOR3) {
			r_error_message = "INVALID_ARGUMENT: scale must be a Vector3.";
			return false;
		}
		const Vector3 scale = p_value;
		if (Math::is_zero_approx(scale.x) || Math::is_zero_approx(scale.y) || Math::is_zero_approx(scale.z)) {
			r_error_message = "INVALID_ARGUMENT: scale components must be non-zero.";
			return false;
		}
	}
	if (property_name == "basis" || property_name == "global_basis") {
		if (p_value.get_type() != Variant::BASIS || Math::is_zero_approx(Basis(p_value).determinant())) {
			r_error_message = "INVALID_ARGUMENT: basis must be an invertible Basis.";
			return false;
		}
	}
	if (property_name == "transform" || property_name == "global_transform") {
		if (p_value.get_type() != Variant::TRANSFORM3D || Math::is_zero_approx(Transform3D(p_value).basis.determinant())) {
			r_error_message = "INVALID_ARGUMENT: transform must have an invertible Transform3D basis.";
			return false;
		}
	}
	return true;
}

static Dictionary _operation_error(int p_index, const String &p_id, const String &p_name,
		int p_error_code, const String &p_message, const String &p_status = "failed") {
	Dictionary result;
	result["index"] = p_index;
	result["op"] = p_name;
	if (!p_id.is_empty()) {
		result["id"] = p_id;
	}
	result["ok"] = false;
	result["status"] = p_status;
	result["error"] = GodotMCPError::make_data(p_error_code, p_message, "scene.apply_patch");
	return result;
}

static Dictionary _operation_success(const PatchOperation &p_operation, const String &p_status) {
	Dictionary result;
	result["index"] = p_operation.index;
	result["op"] = p_operation.name;
	if (!p_operation.local_id.is_empty()) {
		result["id"] = p_operation.local_id;
	}
	result["ok"] = true;
	result["status"] = p_status;
	result["data"] = p_operation.summary;
	return result;
}

static Array _args(std::initializer_list<Variant> p_values) {
	Array args;
	for (const Variant &value : p_values) {
		args.push_back(value);
	}
	return args;
}

static Vector<Pair<StringName, Variant>> _script_property_state(Object *p_object) {
	Vector<Pair<StringName, Variant>> state;
	if (ScriptInstance *instance = p_object->get_script_instance()) {
		List<Pair<StringName, Variant>> properties;
		instance->get_property_state(properties);
		for (const Pair<StringName, Variant> &property : properties) {
			state.push_back(property);
		}
	}
	return state;
}

static void _restore_property_state(Object *p_object, const Vector<Pair<StringName, Variant>> &p_state) {
	for (const Pair<StringName, Variant> &property : p_state) {
		p_object->set(property.first, property.second);
	}
}

static Vector<Pair<StringName, Variant>> _node_2d_property_state(Node2D *p_node) {
	Vector<Pair<StringName, Variant>> state;
	state.push_back(Pair<StringName, Variant>(SNAME("position"), p_node->get_position()));
	state.push_back(Pair<StringName, Variant>(SNAME("rotation"), p_node->get_rotation()));
	state.push_back(Pair<StringName, Variant>(SNAME("scale"), p_node->get_scale()));
	state.push_back(Pair<StringName, Variant>(SNAME("skew"), p_node->get_skew()));
	return state;
}

static void _add_do_method(UndoRedo *p_undo_redo, Object *p_object, const StringName &p_method, const Array &p_arguments = Array()) {
	p_undo_redo->add_do_method(Callable(p_object, p_method).bindv(p_arguments));
}

static void _add_undo_method(UndoRedo *p_undo_redo, Object *p_object, const StringName &p_method, const Array &p_arguments = Array()) {
	p_undo_redo->add_undo_method(Callable(p_object, p_method).bindv(p_arguments));
}

class PatchPlanner {
	const GodotMCPScenePatch::Context &context;
	PatchMode mode = PATCH_MODE_ATOMIC;
	bool dry_run = false;
	bool committed = false;

	Vector<NodeState *> node_states;
	HashMap<ObjectID, NodeState *> node_state_by_object;
	HashMap<String, NodeState *> node_by_local_id;
	HashMap<String, Ref<Resource>> resource_by_local_id;
	HashMap<String, Object *> shadow_by_object;
	HashMap<String, bool> failed_local_ids;
	HashSet<String> declared_local_ids;
	HashSet<ObjectID> deleted_resources;
	HashSet<String> reserved_resource_ids;

	Vector<Node *> created_nodes;
	Vector<Ref<Resource>> created_resources;
	Vector<Node *> shadow_nodes;
	Vector<Ref<Resource>> shadow_resources;
	Vector<PatchOperation> operations;
	Array results;
	Array property_warnings;

	int failure_count = 0;
	int skipped_count = 0;

	static String _object_key(const Object *p_object) {
		return uitos(p_object->get_instance_id());
	}

	void _build_node_states(Node *p_node, NodeState *p_parent, int &r_count) {
		if (!p_node || r_count >= MAX_PATCH_SCENE_NODES) {
			return;
		}
		r_count++;

		NodeState *state = memnew(NodeState);
		state->node = p_node;
		state->owner = p_node->get_owner();
		state->parent = p_parent;
		state->name = p_node->get_name();
		state->internal = p_node->is_internal();
		List<Node::GroupInfo> groups;
		p_node->get_groups(&groups);
		for (const Node::GroupInfo &group : groups) {
			if (group.persistent) {
				state->groups.insert(group.name);
			}
		}
		node_states.push_back(state);
		node_state_by_object.insert(p_node->get_instance_id(), state);
		if (p_parent) {
			p_parent->children.push_back(state);
		}

		for (int i = 0; i < p_node->get_child_count(true); i++) {
			_build_node_states(p_node->get_child(i, true), state, r_count);
		}
	}

	bool _is_effectively_deleted(const NodeState *p_state) const {
		for (const NodeState *cursor = p_state; cursor; cursor = cursor->parent) {
			if (cursor->deleted) {
				return true;
			}
		}
		return false;
	}

	int _child_index(const NodeState *p_parent, const NodeState *p_child) const {
		if (!p_parent) {
			return -1;
		}
		for (int i = 0; i < p_parent->children.size(); i++) {
			if (p_parent->children[i] == p_child) {
				return i;
			}
		}
		return -1;
	}

	static void _remove_child_state(NodeState *p_parent, NodeState *p_child) {
		if (!p_parent) {
			return;
		}
		for (int i = 0; i < p_parent->children.size(); i++) {
			if (p_parent->children[i] == p_child) {
				p_parent->children.remove_at(i);
				return;
			}
		}
	}

	static void _insert_child_state(NodeState *p_parent, NodeState *p_child, int p_position) {
		if (p_position < 0 || p_position >= p_parent->children.size()) {
			p_parent->children.push_back(p_child);
		} else {
			p_parent->children.insert(p_position, p_child);
		}
		p_child->parent = p_parent;
	}

	void _capture_subtree_owners(NodeState *p_state, Vector<OwnerWrite> &r_writes) const {
		OwnerWrite write;
		write.node = p_state->node;
		write.before = p_state->owner;
		write.after = p_state->owner;
		r_writes.push_back(write);
		for (NodeState *child : p_state->children) {
			_capture_subtree_owners(child, r_writes);
		}
	}

	void _plan_moved_owners(PatchOperation &r_operation) {
		for (OwnerWrite &write : r_operation.owner_writes) {
			NodeState *state = node_state_by_object.get(write.node->get_instance_id());
			bool owner_is_ancestor = false;
			for (NodeState *ancestor = state->parent; ancestor; ancestor = ancestor->parent) {
				if (ancestor->node == state->owner) {
					owner_is_ancestor = true;
					break;
				}
			}
			if (!owner_is_ancestor && (state->owner || state == r_operation.node_state)) {
				state->owner = context.root;
			}
			write.after = state->owner;
		}
	}

	String _state_path(const NodeState *p_state) const {
		if (!p_state || _is_effectively_deleted(p_state)) {
			return String();
		}
		if (p_state->node == context.root) {
			return ".";
		}
		Vector<String> components;
		for (const NodeState *cursor = p_state; cursor && cursor->node != context.root; cursor = cursor->parent) {
			components.push_back(cursor->name);
		}
		String path;
		for (int i = components.size() - 1; i >= 0; i--) {
			if (!path.is_empty()) {
				path += "/";
			}
			path += components[i];
		}
		return path;
	}

	Vector<const NodeState *> _state_chain(const NodeState *p_state) const {
		Vector<const NodeState *> chain;
		for (const NodeState *cursor = p_state; cursor; cursor = cursor->parent) {
			chain.push_back(cursor);
		}
		chain.reverse();
		return chain;
	}

	String _relative_state_path(const NodeState *p_from, const NodeState *p_to) const {
		const Vector<const NodeState *> from_chain = _state_chain(p_from);
		const Vector<const NodeState *> to_chain = _state_chain(p_to);
		int common = 0;
		while (common < from_chain.size() && common < to_chain.size() && from_chain[common] == to_chain[common]) {
			common++;
		}
		String path;
		for (int i = common; i < from_chain.size(); i++) {
			if (!path.is_empty()) {
				path += "/";
			}
			path += "..";
		}
		for (int i = common; i < to_chain.size(); i++) {
			if (!path.is_empty()) {
				path += "/";
			}
			path += to_chain[i]->name;
		}
		return path.is_empty() ? String(".") : path;
	}

	NodeState *_find_child(NodeState *p_parent, const StringName &p_name) const {
		if (!p_parent) {
			return nullptr;
		}
		for (NodeState *child : p_parent->children) {
			if (!_is_effectively_deleted(child) && child->name == p_name) {
				return child;
			}
		}
		return nullptr;
	}

	NodeState *_resolve_logical_path(const NodePath &p_path, NodeState *p_base) const {
		if (p_path.is_empty()) {
			return nullptr;
		}
		NodeState *cursor = p_path.is_absolute() ? node_state_by_object.get(context.root->get_instance_id()) : p_base;
		if (!cursor) {
			return nullptr;
		}
		for (int i = 0; i < p_path.get_name_count(); i++) {
			const StringName component = p_path.get_name(i);
			if (component == SNAME(".")) {
				continue;
			}
			if (component == SNAME("..")) {
				cursor = cursor->parent;
			} else {
				cursor = _find_child(cursor, component);
			}
			if (!cursor || _is_effectively_deleted(cursor)) {
				return nullptr;
			}
		}
		return cursor;
	}

	NodeState *_resolve_initial_node_path(const String &p_path) const {
		if (p_path.is_empty()) {
			return nullptr;
		}
		Node *node = nullptr;
		if (p_path == ".") {
			node = context.root;
		} else {
			node = context.root->get_node_or_null(NodePath(p_path));
		}
		if (!node) {
			return nullptr;
		}
		NodeState *const *state = node_state_by_object.getptr(node->get_instance_id());
		return state ? *state : nullptr;
	}

	bool _selector_local_id(const Variant &p_selector, const String &p_key, String &r_id) const {
		if (p_selector.get_type() != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary selector = p_selector;
		if (!selector.has(p_key)) {
			return false;
		}
		const Variant id_variant = selector[p_key];
		if (id_variant.get_type() != Variant::STRING && id_variant.get_type() != Variant::STRING_NAME) {
			return false;
		}
		r_id = String(id_variant);
		return !r_id.is_empty();
	}

	NodeState *_resolve_node_selector(const Dictionary &p_operation, const String &p_field,
			String &r_dependency_id, int &r_error_code, String &r_error_message) const {
		Variant selector = p_operation.get(p_field, Variant());
		if (selector.get_type() == Variant::NIL && p_field == "node") {
			if (p_operation.has("node_ref")) {
				Dictionary wrapper;
				wrapper["node_ref"] = p_operation["node_ref"];
				selector = wrapper;
			} else if (p_operation.has("node_path")) {
				selector = p_operation["node_path"];
			}
		}
		if (selector.get_type() == Variant::NIL && p_field == "parent") {
			if (p_operation.has("parent_ref")) {
				Dictionary wrapper;
				wrapper["node_ref"] = p_operation["parent_ref"];
				selector = wrapper;
			} else if (p_operation.has("parent_path")) {
				selector = p_operation["parent_path"];
			}
		}

		String local_id;
		if (_selector_local_id(selector, "node_ref", local_id)) {
			r_dependency_id = local_id;
			NodeState *const *state = node_by_local_id.getptr(local_id);
			if (!state) {
				r_error_code = failed_local_ids.has(local_id) ? GodotMCPError::CONFLICT : GodotMCPError::NOT_FOUND;
				r_error_message = failed_local_ids.has(local_id) ? "CONFLICT: referenced node operation did not succeed: " + local_id : "NOT_FOUND: unknown request-local node ID: " + local_id;
				return nullptr;
			}
			if (_is_effectively_deleted(*state)) {
				r_error_code = GodotMCPError::CONFLICT;
				r_error_message = "CONFLICT: request-local node has been deleted: " + local_id;
				return nullptr;
			}
			return *state;
		}

		if (selector.get_type() != Variant::STRING && selector.get_type() != Variant::STRING_NAME) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: " + p_field + " must be a scene-relative path or {node_ref:string}.";
			return nullptr;
		}
		const String path = String(selector);
		NodeState *state = _resolve_initial_node_path(path);
		if (!state) {
			r_error_code = GodotMCPError::NOT_FOUND;
			r_error_message = "NOT_FOUND: node path does not exist in the initial scene: " + path;
			return nullptr;
		}
		if (_is_effectively_deleted(state)) {
			r_error_code = GodotMCPError::CONFLICT;
			r_error_message = "CONFLICT: node has already been deleted by this patch: " + path;
			return nullptr;
		}
		return state;
	}

	Ref<Resource> _resolve_resource_selector(const Dictionary &p_operation, String &r_dependency_id,
			int &r_error_code, String &r_error_message) const {
		Variant selector = p_operation.get("resource", Variant());
		if (selector.get_type() == Variant::NIL && p_operation.has("resource_ref")) {
			Dictionary wrapper;
			wrapper["resource_ref"] = p_operation["resource_ref"];
			selector = wrapper;
		}
		if (selector.get_type() == Variant::NIL && p_operation.has("resource_path")) {
			Dictionary wrapper;
			wrapper["resource_path"] = p_operation["resource_path"];
			selector = wrapper;
		}

		String local_id;
		if (_selector_local_id(selector, "resource_ref", local_id)) {
			r_dependency_id = local_id;
			const Ref<Resource> *resource = resource_by_local_id.getptr(local_id);
			if (!resource) {
				r_error_code = failed_local_ids.has(local_id) ? GodotMCPError::CONFLICT : GodotMCPError::NOT_FOUND;
				r_error_message = failed_local_ids.has(local_id) ? "CONFLICT: referenced resource operation did not succeed: " + local_id : "NOT_FOUND: unknown request-local resource ID: " + local_id;
				return Ref<Resource>();
			}
			if (deleted_resources.has((*resource)->get_instance_id())) {
				r_error_code = GodotMCPError::CONFLICT;
				r_error_message = "CONFLICT: request-local resource has been deleted: " + local_id;
				return Ref<Resource>();
			}
			return *resource;
		}

		String resource_path;
		if (selector.get_type() == Variant::STRING || selector.get_type() == Variant::STRING_NAME) {
			resource_path = String(selector);
		} else if (selector.get_type() == Variant::DICTIONARY) {
			const Dictionary selector_dictionary = selector;
			if (selector_dictionary.has("resource_path")) {
				resource_path = String(selector_dictionary["resource_path"]);
			} else if (selector_dictionary.has("path")) {
				resource_path = String(selector_dictionary["path"]);
			}
		}
		if (resource_path.is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: resource selector must use resource_ref or resource_path.";
			return Ref<Resource>();
		}
		if (!resource_path.begins_with(context.scene_path + "::")) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: resource_path must identify an embedded resource in expected_scene_path.";
			return Ref<Resource>();
		}
		Ref<Resource> resource = ResourceCache::get_ref(resource_path);
		if (resource.is_null()) {
			resource = ResourceLoader::load(resource_path);
		}
		if (resource.is_null()) {
			r_error_code = GodotMCPError::NOT_FOUND;
			r_error_message = "NOT_FOUND: embedded resource could not be loaded: " + resource_path;
			return Ref<Resource>();
		}
		if (deleted_resources.has(resource->get_instance_id())) {
			r_error_code = GodotMCPError::CONFLICT;
			r_error_message = "CONFLICT: embedded resource has already been deleted by this patch.";
			return Ref<Resource>();
		}
		return resource;
	}

	Object *_make_shadow(Object *p_object, String &r_error_message) {
		const String key = _object_key(p_object);
		if (Object *const *existing = shadow_by_object.getptr(key)) {
			return *existing;
		}

		Object *shadow = nullptr;
		if (Resource *resource = Object::cast_to<Resource>(p_object)) {
			Ref<Resource> original(resource);
			Ref<Resource> duplicate = original->duplicate(false);
			if (duplicate.is_null()) {
				Object *object = ClassDB::instantiate(original->get_class());
				Resource *fallback = Object::cast_to<Resource>(object);
				if (!fallback) {
					if (object) {
						memdelete(object);
					}
					r_error_message = "INTERNAL: failed to create a validation copy of resource class " + original->get_class() + ".";
					return nullptr;
				}
				duplicate = Ref<Resource>(fallback);
				List<PropertyInfo> properties;
				original->get_property_list(&properties);
				for (const PropertyInfo &property : properties) {
					if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
						continue;
					}
					bool valid = false;
					const Variant value = original->get(property.name, &valid);
					if (valid) {
						duplicate->set(property.name, value);
					}
				}
			}
			shadow_resources.push_back(duplicate);
			shadow = duplicate.ptr();
		} else if (Node *node = Object::cast_to<Node>(p_object)) {
			Node *duplicate = node->duplicate(Node::DUPLICATE_GROUPS | Node::DUPLICATE_SCRIPTS);
			if (!duplicate) {
				r_error_message = "INTERNAL: failed to create a validation copy of node class " + node->get_class() + ".";
				return nullptr;
			}
			if (Node3D *node_3d = Object::cast_to<Node3D>(node)) {
				// This affects global transforms but is not a stored property copied
				// by Node::duplicate().
				Object::cast_to<Node3D>(duplicate)->set_disable_scale(node_3d->is_scale_disabled());
			}
			shadow_nodes.push_back(duplicate);
			shadow = duplicate;
		}
		if (!shadow) {
			r_error_message = "INTERNAL: unsupported patch target object.";
			return nullptr;
		}
		shadow_by_object.insert(key, shadow);
		return shadow;
	}

	void _warn_property(int p_operation_index, const String &p_operation_id, Object *p_target,
			const StringName &p_property, const String &p_code, const String &p_message) {
		Dictionary warning;
		warning["operation_index"] = p_operation_index;
		if (!p_operation_id.is_empty()) {
			warning["operation_id"] = p_operation_id;
		}
		warning["target_class"] = p_target->get_class();
		warning["property"] = String(p_property);
		warning["code"] = p_code;
		warning["message"] = p_message;
		property_warnings.push_back(warning);
	}

	bool _decode_node_ref(const Dictionary &p_wrapper, Object *p_property_target, Variant &r_value,
			bool &r_contains_node_ref, int &r_error_code, String &r_error_message) const {
		const Variant id_variant = p_wrapper.get("node_ref", Variant());
		if ((id_variant.get_type() != Variant::STRING && id_variant.get_type() != Variant::STRING_NAME) || String(id_variant).is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: node_ref must be a non-empty request-local node ID.";
			return false;
		}
		const String id = id_variant;
		NodeState *const *target_ptr = node_by_local_id.getptr(id);
		if (!target_ptr) {
			r_error_code = failed_local_ids.has(id) ? GodotMCPError::CONFLICT : GodotMCPError::NOT_FOUND;
			r_error_message = failed_local_ids.has(id) ? "CONFLICT: referenced node operation did not succeed: " + id : "NOT_FOUND: unknown request-local node ID: " + id;
			return false;
		}
		NodeState *target_state = *target_ptr;
		if (_is_effectively_deleted(target_state)) {
			r_error_code = GodotMCPError::CONFLICT;
			r_error_message = "CONFLICT: node_ref targets a deleted node: " + id;
			return false;
		}

		NodeState *base_state = node_state_by_object.get(context.root->get_instance_id());
		if (Node *property_node = Object::cast_to<Node>(p_property_target)) {
			if (NodeState *const *property_state = node_state_by_object.getptr(property_node->get_instance_id())) {
				base_state = *property_state;
			}
		}
		if (p_wrapper.has("relative_to")) {
			Dictionary selector_operation;
			selector_operation["node"] = p_wrapper["relative_to"];
			String dependency_id;
			base_state = _resolve_node_selector(selector_operation, "node", dependency_id, r_error_code, r_error_message);
			if (!base_state) {
				return false;
			}
		}

		const String relative_path = _relative_state_path(base_state, target_state);
		NodePath path(relative_path);
		if (p_wrapper.has("subnames")) {
			const Variant subnames_variant = p_wrapper["subnames"];
			Vector<StringName> subnames;
			if (subnames_variant.get_type() == Variant::STRING || subnames_variant.get_type() == Variant::STRING_NAME) {
				subnames.push_back(StringName(subnames_variant));
			} else if (subnames_variant.get_type() == Variant::ARRAY) {
				const Array subname_array = subnames_variant;
				for (int i = 0; i < subname_array.size(); i++) {
					if (subname_array[i].get_type() != Variant::STRING && subname_array[i].get_type() != Variant::STRING_NAME) {
						r_error_code = GodotMCPError::INVALID_ARGUMENT;
						r_error_message = "INVALID_ARGUMENT: node_ref subnames must contain only strings.";
						return false;
					}
					subnames.push_back(StringName(subname_array[i]));
				}
			} else {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: node_ref subnames must be a string or string array.";
				return false;
			}
			path = NodePath(path.get_names(), subnames, false);
		}
		r_value = path;
		r_contains_node_ref = true;
		return true;
	}

	bool _decode_typed_value(const String &p_type_name, const Variant &p_payload, Variant &r_value,
			int &r_error_code, String &r_error_message) const {
		real_t values[12] = {};
		if (p_type_name == "Color" && _array_to_reals(p_payload, 4, values)) {
			r_value = Color(values[0], values[1], values[2], values[3]);
			return true;
		}
		if (p_type_name == "Color" && p_payload.get_type() == Variant::ARRAY && Array(p_payload).size() == 3 && _array_to_reals(p_payload, 3, values)) {
			r_value = Color(values[0], values[1], values[2], 1.0);
			return true;
		}
		if (p_type_name == "Color" && p_payload.get_type() == Variant::STRING && Color::html_is_valid(String(p_payload))) {
			r_value = Color::html(String(p_payload));
			return true;
		}
		if (p_type_name == "Vector2" && _array_to_reals(p_payload, 2, values)) {
			r_value = Vector2(values[0], values[1]);
			return true;
		}
		if (p_type_name == "Vector2i" && _array_to_reals(p_payload, 2, values)) {
			r_value = Vector2i(int(values[0]), int(values[1]));
			return true;
		}
		if (p_type_name == "Vector3" && _array_to_reals(p_payload, 3, values)) {
			r_value = Vector3(values[0], values[1], values[2]);
			return true;
		}
		if (p_type_name == "Vector3i" && _array_to_reals(p_payload, 3, values)) {
			r_value = Vector3i(int(values[0]), int(values[1]), int(values[2]));
			return true;
		}
		if (p_type_name == "Vector4" && _array_to_reals(p_payload, 4, values)) {
			r_value = Vector4(values[0], values[1], values[2], values[3]);
			return true;
		}
		if (p_type_name == "Vector4i" && _array_to_reals(p_payload, 4, values)) {
			r_value = Vector4i(int(values[0]), int(values[1]), int(values[2]), int(values[3]));
			return true;
		}
		if (p_type_name == "Quaternion" && _array_to_reals(p_payload, 4, values)) {
			r_value = Quaternion(values[0], values[1], values[2], values[3]);
			return true;
		}
		if (p_type_name == "Rect2" && _array_to_reals(p_payload, 4, values)) {
			r_value = Rect2(values[0], values[1], values[2], values[3]);
			return true;
		}
		if (p_type_name == "Rect2i" && _array_to_reals(p_payload, 4, values)) {
			r_value = Rect2i(int(values[0]), int(values[1]), int(values[2]), int(values[3]));
			return true;
		}
		if (p_type_name == "Plane" && _array_to_reals(p_payload, 4, values)) {
			r_value = Plane(values[0], values[1], values[2], values[3]);
			return true;
		}
		if (p_type_name == "AABB" && _array_to_reals(p_payload, 6, values)) {
			r_value = AABB(Vector3(values[0], values[1], values[2]), Vector3(values[3], values[4], values[5]));
			return true;
		}
		if (p_type_name == "Basis" && _array_to_reals(p_payload, 9, values)) {
			r_value = Basis(Vector3(values[0], values[1], values[2]), Vector3(values[3], values[4], values[5]), Vector3(values[6], values[7], values[8]));
			return true;
		}
		if (p_type_name == "Transform2D" && _array_to_reals(p_payload, 6, values)) {
			r_value = Transform2D(values[0], values[1], values[2], values[3], values[4], values[5]);
			return true;
		}
		if (p_type_name == "Transform3D" && _array_to_reals(p_payload, 12, values)) {
			r_value = Transform3D(Basis(Vector3(values[0], values[1], values[2]), Vector3(values[3], values[4], values[5]), Vector3(values[6], values[7], values[8])), Vector3(values[9], values[10], values[11]));
			return true;
		}
		if (p_type_name == "NodePath" && (p_payload.get_type() == Variant::STRING || p_payload.get_type() == Variant::STRING_NAME)) {
			r_value = NodePath(String(p_payload));
			return true;
		}
		if (p_type_name == "StringName" && (p_payload.get_type() == Variant::STRING || p_payload.get_type() == Variant::STRING_NAME)) {
			r_value = StringName(String(p_payload));
			return true;
		}

		if (p_payload.get_type() == Variant::ARRAY) {
			const Array array = p_payload;
			if (p_type_name == "PackedByteArray") {
				PackedByteArray packed;
				packed.resize(array.size());
				if (array.is_empty()) {
					r_value = packed;
					return true;
				}
				for (int i = 0; i < array.size(); i++) {
					int64_t value = 0;
					if (!_variant_to_integer(array[i], 0, 255, value)) {
						break;
					}
					packed.set(i, value);
					if (i == array.size() - 1) {
						r_value = packed;
						return true;
					}
				}
			} else if (p_type_name == "PackedInt32Array" || p_type_name == "PackedInt64Array") {
				PackedInt64Array values64;
				values64.resize(array.size());
				if (array.is_empty()) {
					r_value = p_type_name == "PackedInt64Array" ? Variant(values64) : Variant(PackedInt32Array());
					return true;
				}
				for (int i = 0; i < array.size(); i++) {
					int64_t value = 0;
					const bool is_int32 = p_type_name == "PackedInt32Array";
					if (!_variant_to_integer(array[i], is_int32 ? INT32_MIN : INT64_MIN, is_int32 ? INT32_MAX : INT64_MAX, value)) {
						break;
					}
					values64.set(i, value);
					if (i == array.size() - 1) {
						if (p_type_name == "PackedInt64Array") {
							r_value = values64;
						} else {
							PackedInt32Array values32;
							values32.resize(array.size());
							for (int j = 0; j < array.size(); j++) {
								values32.set(j, int32_t(values64[j]));
							}
							r_value = values32;
						}
						return true;
					}
				}
			} else if (p_type_name == "PackedFloat32Array" || p_type_name == "PackedFloat64Array") {
				PackedFloat64Array values64;
				values64.resize(array.size());
				if (array.is_empty()) {
					r_value = p_type_name == "PackedFloat64Array" ? Variant(values64) : Variant(PackedFloat32Array());
					return true;
				}
				for (int i = 0; i < array.size(); i++) {
					real_t number = 0;
					if (!_variant_to_real(array[i], number)) {
						break;
					}
					values64.set(i, number);
					if (i == array.size() - 1) {
						if (p_type_name == "PackedFloat64Array") {
							r_value = values64;
						} else {
							PackedFloat32Array values32;
							values32.resize(array.size());
							for (int j = 0; j < array.size(); j++) {
								values32.set(j, float(values64[j]));
							}
							r_value = values32;
						}
						return true;
					}
				}
			} else if (p_type_name == "PackedStringArray") {
				PackedStringArray packed;
				if (array.is_empty()) {
					r_value = packed;
					return true;
				}
				for (int i = 0; i < array.size(); i++) {
					if (array[i].get_type() != Variant::STRING && array[i].get_type() != Variant::STRING_NAME) {
						break;
					}
					packed.push_back(String(array[i]));
					if (i == array.size() - 1) {
						r_value = packed;
						return true;
					}
				}
			}
		}

		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: malformed or unsupported typed Variant wrapper: " + p_type_name + ".";
		return false;
	}

	bool _decode_value(const Variant &p_encoded, Object *p_property_target, Variant &r_value,
			bool &r_contains_node_ref, int &r_item_count, int p_depth,
			int &r_error_code, String &r_error_message) const {
		if (p_depth > MAX_PATCH_VALUE_DEPTH || r_item_count > MAX_PATCH_CONTAINER_ITEMS) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: property value exceeds patch container bounds.";
			return false;
		}
		r_item_count++;

		if (p_encoded.get_type() == Variant::ARRAY) {
			const Array input = p_encoded;
			Array output;
			output.resize(input.size());
			for (int i = 0; i < input.size(); i++) {
				Variant decoded;
				if (!_decode_value(input[i], p_property_target, decoded, r_contains_node_ref, r_item_count, p_depth + 1, r_error_code, r_error_message)) {
					return false;
				}
				output[i] = decoded;
			}
			r_value = output;
			return true;
		}

		if (p_encoded.get_type() != Variant::DICTIONARY) {
			r_value = p_encoded;
			return true;
		}

		const Dictionary input = p_encoded;
		if (input.has("resource_ref")) {
			const Variant id_variant = input["resource_ref"];
			if ((id_variant.get_type() != Variant::STRING && id_variant.get_type() != Variant::STRING_NAME) || String(id_variant).is_empty()) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: resource_ref must be a non-empty request-local resource ID.";
				return false;
			}
			const String id = id_variant;
			const Ref<Resource> *resource = resource_by_local_id.getptr(id);
			if (!resource) {
				r_error_code = failed_local_ids.has(id) ? GodotMCPError::CONFLICT : GodotMCPError::NOT_FOUND;
				r_error_message = failed_local_ids.has(id) ? "CONFLICT: referenced resource operation did not succeed: " + id : "NOT_FOUND: unknown request-local resource ID: " + id;
				return false;
			}
			if (deleted_resources.has((*resource)->get_instance_id())) {
				r_error_code = GodotMCPError::CONFLICT;
				r_error_message = "CONFLICT: resource_ref targets a deleted resource: " + id;
				return false;
			}
			r_value = *resource;
			return true;
		}
		if (input.has("external_resource") || input.has("resource_path")) {
			const Variant path_variant = input.get("external_resource", input.get("resource_path", Variant()));
			if (path_variant.get_type() != Variant::STRING && path_variant.get_type() != Variant::STRING_NAME) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: external_resource/resource_path must be a string.";
				return false;
			}
			String path = String(path_variant).strip_edges();
			if (path.begins_with("uid://")) {
				const ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(path);
				if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
					path = ResourceUID::get_singleton()->get_id_path(uid);
				}
			}
			if (!path.begins_with("res://")) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: resource path must start with res:// or uid://.";
				return false;
			}
			Ref<Resource> resource = ResourceLoader::load(path);
			if (resource.is_null()) {
				r_error_code = GodotMCPError::NOT_FOUND;
				r_error_message = "NOT_FOUND: resource reference could not be loaded: " + path;
				return false;
			}
			r_value = resource;
			return true;
		}
		if (input.has("node_ref")) {
			return _decode_node_ref(input, p_property_target, r_value, r_contains_node_ref, r_error_code, r_error_message);
		}
		if (input.has("__type")) {
			const Variant type_variant = input["__type"];
			if ((type_variant.get_type() != Variant::STRING && type_variant.get_type() != Variant::STRING_NAME) || !input.has("value")) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: typed Variant wrappers require string __type and value.";
				return false;
			}
			Variant decoded_payload;
			if (!_decode_value(input["value"], p_property_target, decoded_payload, r_contains_node_ref, r_item_count, p_depth + 1, r_error_code, r_error_message)) {
				return false;
			}
			return _decode_typed_value(String(type_variant), decoded_payload, r_value, r_error_code, r_error_message);
		}

		Dictionary output;
		for (const KeyValue<Variant, Variant> &E : input) {
			Variant decoded_key;
			Variant decoded_value;
			if (!_decode_value(E.key, p_property_target, decoded_key, r_contains_node_ref, r_item_count, p_depth + 1, r_error_code, r_error_message) ||
					!_decode_value(E.value, p_property_target, decoded_value, r_contains_node_ref, r_item_count, p_depth + 1, r_error_code, r_error_message)) {
				return false;
			}
			output[decoded_key] = decoded_value;
		}
		r_value = output;
		return true;
	}

	bool _validate_typed_array_element(const Array &p_existing, const Variant &p_element, int p_index, String &r_error_message) const {
		if (!p_existing.is_typed()) {
			return true;
		}
		const Variant::Type expected = Variant::Type(p_existing.get_typed_builtin());
		if (p_element.get_type() == Variant::NIL && expected == Variant::OBJECT) {
			return true;
		}
		if (expected == Variant::OBJECT) {
			if (p_element.get_type() != Variant::OBJECT) {
				r_error_message = "INVALID_ARGUMENT: typed Array element " + itos(p_index) + " must be an Object.";
				return false;
			}
			Object *object = p_element;
			const StringName expected_class = p_existing.get_typed_class_name();
			if (object && !expected_class.is_empty() && !object->is_class(expected_class)) {
				r_error_message = "INVALID_ARGUMENT: typed Array element " + itos(p_index) + " must inherit " + String(expected_class) + ".";
				return false;
			}
			return true;
		}
		if (p_element.get_type() != expected && !Variant::can_convert_strict(p_element.get_type(), expected)) {
			r_error_message = "INVALID_ARGUMENT: typed Array element " + itos(p_index) + " cannot convert from " + Variant::get_type_name(p_element.get_type()) + " to " + Variant::get_type_name(expected) + ".";
			return false;
		}
		return true;
	}

	bool _coerce_value(const PropertyInfo &p_info, const Variant &p_existing, const Variant &p_requested,
			Variant &r_value, bool &r_coerced, String &r_error_message) const {
		const Variant::Type expected = p_info.type == Variant::VARIANT_MAX ? p_existing.get_type() : p_info.type;
		const String expected_resource_types = _resource_type_hint(p_info);
		r_coerced = expected != p_requested.get_type();

		if (p_requested.get_type() == Variant::NIL) {
			if (expected == Variant::OBJECT || expected == Variant::NIL) {
				r_value = Variant();
				return true;
			}
			r_error_message = "INVALID_ARGUMENT: property '" + String(p_info.name) + "' cannot be null.";
			return false;
		}

		if (expected == Variant::OBJECT) {
			Variant requested = p_requested;
			if (requested.get_type() == Variant::STRING || requested.get_type() == Variant::STRING_NAME || requested.get_type() == Variant::DICTIONARY) {
				String path;
				if (requested.get_type() == Variant::STRING || requested.get_type() == Variant::STRING_NAME) {
					path = String(requested);
				} else {
					const Dictionary dictionary = requested;
					if (dictionary.has("path")) {
						path = String(dictionary["path"]);
					} else if (dictionary.has("uid")) {
						path = String(dictionary["uid"]);
					}
				}
				if (path.begins_with("uid://")) {
					const ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(path);
					if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
						path = ResourceUID::get_singleton()->get_id_path(uid);
					}
				}
				if (!path.is_empty()) {
					Ref<Resource> loaded = ResourceLoader::load(path);
					if (loaded.is_valid()) {
						requested = loaded;
					}
				}
			}
			if (requested.get_type() != Variant::OBJECT) {
				r_error_message = "INVALID_ARGUMENT: property '" + String(p_info.name) + "' expects a Resource reference.";
				return false;
			}
			Ref<Resource> resource = requested;
			if (resource.is_null() || !_resource_is_compatible(resource, expected_resource_types)) {
				r_error_message = "INVALID_ARGUMENT: resource assigned to property '" + String(p_info.name) + "' is not compatible with " + expected_resource_types + ".";
				return false;
			}
			r_value = resource;
			return true;
		}

		if (expected == Variant::ARRAY && p_requested.get_type() == Variant::ARRAY) {
			const Array requested = p_requested;
			if (p_existing.get_type() == Variant::ARRAY && Array(p_existing).is_typed()) {
				const Array existing = p_existing;
				for (int i = 0; i < requested.size(); i++) {
					if (!_validate_typed_array_element(existing, requested[i], i, r_error_message)) {
						return false;
					}
				}
				Array typed;
				typed.set_typed(existing.get_typed_builtin(), existing.get_typed_class_name(), existing.get_typed_script());
				typed.assign(requested);
				r_value = typed;
			} else {
				r_value = requested;
			}
			return true;
		}
		if (expected == Variant::DICTIONARY && p_requested.get_type() == Variant::DICTIONARY) {
			r_value = p_requested;
			return true;
		}

		if (expected == p_requested.get_type()) {
			r_value = p_requested;
			return true;
		}
		if (expected == Variant::VECTOR2 && p_requested.get_type() == Variant::ARRAY) {
			real_t values[2];
			if (_array_to_reals(p_requested, 2, values)) {
				r_value = Vector2(values[0], values[1]);
				return true;
			}
		}
		if (expected == Variant::VECTOR3 && p_requested.get_type() == Variant::ARRAY) {
			real_t values[3];
			if (_array_to_reals(p_requested, 3, values)) {
				r_value = Vector3(values[0], values[1], values[2]);
				return true;
			}
		}
		if (expected == Variant::VECTOR4 && p_requested.get_type() == Variant::ARRAY) {
			real_t values[4];
			if (_array_to_reals(p_requested, 4, values)) {
				r_value = Vector4(values[0], values[1], values[2], values[3]);
				return true;
			}
		}
		if (expected == Variant::COLOR && p_requested.get_type() == Variant::ARRAY) {
			real_t values[4];
			const Array requested = p_requested;
			if (requested.size() == 3 && _array_to_reals(requested, 3, values)) {
				r_value = Color(values[0], values[1], values[2], 1.0);
				return true;
			}
			if (_array_to_reals(requested, 4, values)) {
				r_value = Color(values[0], values[1], values[2], values[3]);
				return true;
			}
		}
		if (expected == Variant::COLOR && p_requested.get_type() == Variant::STRING && Color::html_is_valid(String(p_requested))) {
			r_value = Color::html(String(p_requested));
			return true;
		}
		if (expected == Variant::NODE_PATH && (p_requested.get_type() == Variant::STRING || p_requested.get_type() == Variant::STRING_NAME)) {
			r_value = NodePath(String(p_requested));
			return true;
		}
		if (expected == Variant::STRING_NAME && p_requested.get_type() == Variant::STRING) {
			r_value = StringName(p_requested);
			return true;
		}

		if (Variant::can_convert(p_requested.get_type(), expected)) {
			Callable::CallError call_error;
			const Variant *arguments[1] = { &p_requested };
			Variant converted;
			Variant::construct(expected, converted, arguments, 1, call_error);
			if (call_error.error == Callable::CallError::CALL_OK) {
				r_value = converted;
				return true;
			}
		}

		r_error_message = "INVALID_ARGUMENT: type mismatch for property '" + String(p_info.name) + "' (expected " + Variant::get_type_name(expected) + ", got " + Variant::get_type_name(p_requested.get_type()) + ").";
		return false;
	}

	bool _validate_node_path_value(Object *p_target, const NodePath &p_path, String &r_error_message) const {
		if (p_path.is_empty()) {
			return true;
		}
		NodeState *base = node_state_by_object.get(context.root->get_instance_id());
		if (Node *node = Object::cast_to<Node>(p_target)) {
			if (NodeState *const *state = node_state_by_object.getptr(node->get_instance_id())) {
				base = *state;
			}
		}
		const NodePath names_only(String(p_path.get_concatenated_names()));
		if (!_resolve_logical_path(names_only, base)) {
			r_error_message = "INVALID_ARGUMENT: NodePath does not resolve in the proposed scene: " + String(p_path);
			return false;
		}
		return true;
	}

	Node3D *_planned_node_3d(const NodeState *p_state) const {
		if (Object *const *shadow = shadow_by_object.getptr(_object_key(p_state->node))) {
			return Object::cast_to<Node3D>(*shadow);
		}
		return Object::cast_to<Node3D>(p_state->node);
	}

	Transform3D _planned_parent_transform(const NodeState *p_state) const {
		if (p_state->parent && Object::cast_to<Node3D>(p_state->parent->node)) {
			return _planned_global_transform(p_state->parent);
		}
		// The edited root can have a parent outside the patch's scene boundary.
		if (!p_state->parent) {
			Node3D *parent = Object::cast_to<Node3D>(p_state->node->get_parent());
			if (parent && parent->is_inside_tree()) {
				return parent->get_global_transform();
			}
		}
		return Transform3D();
	}

	Transform3D _planned_global_transform(const NodeState *p_state) const {
		Node3D *node = _planned_node_3d(p_state);
		Transform3D transform = node->get_transform();
		if (!node->is_set_as_top_level()) {
			transform = _planned_parent_transform(p_state) * transform;
		}
		if (node->is_scale_disabled()) {
			transform.basis.orthonormalize();
		}
		return transform;
	}

	static bool _is_global_transform_property(const StringName &p_property) {
		return p_property == SNAME("global_transform") || p_property == SNAME("global_position") ||
				p_property == SNAME("global_basis") || p_property == SNAME("global_rotation") ||
				p_property == SNAME("global_rotation_degrees");
	}

	Variant _planned_global_property(const NodeState *p_state, const StringName &p_property) const {
		const Transform3D transform = _planned_global_transform(p_state);
		if (p_property == SNAME("global_transform")) {
			return transform;
		}
		if (p_property == SNAME("global_position")) {
			return transform.origin;
		}
		if (p_property == SNAME("global_basis")) {
			return transform.basis;
		}
		const Vector3 rotation = transform.basis.get_euler_normalized();
		if (p_property == SNAME("global_rotation_degrees")) {
			return Vector3(Math::rad_to_deg(rotation.x), Math::rad_to_deg(rotation.y), Math::rad_to_deg(rotation.z));
		}
		return rotation;
	}

	bool _set_planned_global_property(NodeState *p_state, Node3D *p_shadow, const StringName &p_property,
			const Variant &p_value, String &r_error_message) {
		Transform3D transform = _planned_global_transform(p_state);
		if (p_property == SNAME("global_transform")) {
			transform = p_value;
		} else if (p_property == SNAME("global_position")) {
			transform.origin = p_value;
		} else if (p_property == SNAME("global_basis")) {
			transform.basis = p_value;
		} else {
			Vector3 rotation = p_value;
			if (p_property == SNAME("global_rotation_degrees")) {
				rotation = Vector3(Math::deg_to_rad(rotation.x), Math::deg_to_rad(rotation.y), Math::deg_to_rad(rotation.z));
			}
			transform.basis = Basis::from_euler(rotation) * Basis::from_scale(transform.basis.get_scale());
		}
		if (!p_shadow->is_set_as_top_level()) {
			const Transform3D parent = _planned_parent_transform(p_state);
			if (Math::is_zero_approx(parent.basis.determinant())) {
				r_error_message = "INVALID_ARGUMENT: global transform requires an invertible planned parent transform.";
				return false;
			}
			transform = parent.affine_inverse() * transform;
		}
		p_shadow->set_transform(transform);
		return true;
	}

	Node2D *_planned_node_2d(const NodeState *p_state) const {
		if (Object *const *shadow = shadow_by_object.getptr(_object_key(p_state->node))) {
			return Object::cast_to<Node2D>(*shadow);
		}
		return Object::cast_to<Node2D>(p_state->node);
	}

	bool _planned_parent_transform_2d(const NodeState *p_state, Transform2D &r_transform, String &r_error_message) const {
		r_transform = Transform2D();
		if (_planned_node_2d(p_state)->is_set_as_top_level()) {
			return true;
		}
		if (p_state->parent) {
			if (Object::cast_to<Node2D>(p_state->parent->node)) {
				return _planned_global_transform_2d(p_state->parent, r_transform, r_error_message);
			}
			if (Object::cast_to<CanvasItem>(p_state->parent->node)) {
				r_error_message = "INVALID_ARGUMENT: Node2D global properties require Node2D ancestors; planned Control layout transforms are unsupported. Use local properties.";
				return false;
			}
		} else {
			CanvasItem *parent = Object::cast_to<CanvasItem>(p_state->node->get_parent());
			if (parent) {
				r_transform = parent->get_global_transform();
			}
		}
		return true;
	}

	bool _planned_global_transform_2d(const NodeState *p_state, Transform2D &r_transform, String &r_error_message) const {
		Transform2D parent;
		if (!_planned_parent_transform_2d(p_state, parent, r_error_message)) {
			return false;
		}
		r_transform = parent * _planned_node_2d(p_state)->get_transform();
		return true;
	}

	static Variant _global_property_2d(const Transform2D &p_transform, const StringName &p_property) {
		if (p_property == SNAME("global_transform")) {
			return p_transform;
		}
		if (p_property == SNAME("global_position")) {
			return p_transform.get_origin();
		}
		if (p_property == SNAME("global_scale")) {
			return p_transform.get_scale();
		}
		if (p_property == SNAME("global_skew")) {
			return p_transform.get_skew();
		}
		return p_property == SNAME("global_rotation_degrees") ? Math::rad_to_deg(p_transform.get_rotation()) : p_transform.get_rotation();
	}

	static StringName _local_property_2d(const StringName &p_property) {
		if (p_property == SNAME("global_position")) {
			return SNAME("position");
		}
		if (p_property == SNAME("global_scale")) {
			return SNAME("scale");
		}
		if (p_property == SNAME("global_skew")) {
			return SNAME("skew");
		}
		return p_property == SNAME("global_transform") ? SNAME("transform") : SNAME("rotation");
	}

	bool _set_planned_global_property_2d(NodeState *p_state, Node2D *p_shadow, const StringName &p_property,
			const Variant &p_value, String &r_error_message) {
		Transform2D parent;
		if (!_planned_parent_transform_2d(p_state, parent, r_error_message)) {
			return false;
		}
		if (Math::is_zero_approx(parent.determinant())) {
			r_error_message = "INVALID_ARGUMENT: global transform requires an invertible planned parent transform.";
			return false;
		}
		const Transform2D inverse = parent.affine_inverse();
		Transform2D transform = parent * p_shadow->get_transform();
		if (p_property == SNAME("global_position")) {
			p_shadow->set_position(inverse.xform(Vector2(p_value)));
		} else if (p_property == SNAME("global_transform")) {
			p_shadow->set_transform(inverse * Transform2D(p_value));
		} else if (p_property == SNAME("global_scale")) {
			transform.set_scale(p_value);
			p_shadow->set_scale((inverse * transform).get_scale());
		} else if (p_property == SNAME("global_skew")) {
			transform.set_skew(p_value);
			p_shadow->set_skew((inverse * transform).get_skew());
		} else {
			const real_t rotation = p_property == SNAME("global_rotation_degrees") ? Math::deg_to_rad(real_t(p_value)) : real_t(p_value);
			transform.set_rotation(rotation);
			p_shadow->set_rotation((inverse * transform).get_rotation());
		}
		return true;
	}

	bool _plan_property_writes(PatchOperation &r_operation, Object *p_target, const Dictionary &p_properties,
			int &r_error_code, String &r_error_message, const Array &p_entries = Array()) {
		if (p_properties.is_empty()) {
			return true;
		}
		Object *shadow = _make_shadow(p_target, r_error_message);
		if (!shadow) {
			r_error_code = GodotMCPError::INTERNAL;
			return false;
		}

		Vector<PropertyWrite> writes;
		const int warning_start = property_warnings.size();
		Vector<StringName> property_names;
		Vector<Variant> encoded_values;
		for (const KeyValue<Variant, Variant> &E : p_properties) {
			if (E.key.get_type() != Variant::STRING && E.key.get_type() != Variant::STRING_NAME) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: property names must be strings.";
				return false;
			}
			property_names.push_back(StringName(E.key));
		}
		property_names.sort();
		// Object-form properties have no requested order, but attaching a script
		// must precede properties supplied by that script.
		const int script_index = property_names.find(SNAME("script"));
		if (script_index > 0) {
			property_names.remove_at(script_index);
			property_names.insert(0, SNAME("script"));
		}
		if (!p_entries.is_empty()) {
			property_names.clear();
			for (int i = 0; i < p_entries.size(); i++) {
				const Dictionary entry = p_entries[i];
				const StringName name = String(entry.get("name", entry.get("property", Variant())));
				if (p_properties.has(name)) { // A node name is handled by the structural planner.
					property_names.push_back(name);
					encoded_values.push_back(entry["value"]);
				}
			}
		} else {
			for (const StringName &name : property_names) {
				encoded_values.push_back(p_properties[name]);
			}
		}
		for (int property_index = 0; property_index < property_names.size(); property_index++) {
			const StringName property_name = property_names[property_index];
			const Variant encoded_value = encoded_values[property_index];
			if (Object::cast_to<Control>(shadow) && property_name == SNAME("global_position")) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: Control.global_position depends on live layout and cannot be planned safely. Use local position or offsets.";
				break;
			}
			PropertyInfo property_info;
			if (!_find_property_info(shadow, property_name, property_info)) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: unknown property '" + String(property_name) + "' on " + p_target->get_class() + ".";
				break;
			}
			if (property_info.usage & PROPERTY_USAGE_READ_ONLY) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: property is read-only: " + String(property_name);
				break;
			}

			Node3D *node_3d = Object::cast_to<Node3D>(shadow);
			Node2D *node_2d = Object::cast_to<Node2D>(shadow);
			NodeState *state = (node_3d || node_2d) ? node_state_by_object.get(p_target->get_instance_id()) : nullptr;
			const bool global_property_3d = node_3d && _is_global_transform_property(property_name);
			const bool global_property_2d = node_2d && (_is_global_transform_property(property_name) || property_name == SNAME("global_scale") || property_name == SNAME("global_skew"));
			const bool top_level_property = node_3d && property_name == SNAME("top_level");
			const Transform3D local_before = node_3d ? node_3d->get_transform() : Transform3D();
			const StringName local_property_2d = global_property_2d ? _local_property_2d(property_name) : StringName();
			const Variant local_before_2d = global_property_2d ? shadow->get(local_property_2d) : Variant();
			const Vector<Pair<StringName, Variant>> before_node_2d_state = global_property_2d && local_property_2d == SNAME("transform") ? _node_2d_property_state(node_2d) : Vector<Pair<StringName, Variant>>();
			Transform2D global_2d;
			if (global_property_2d && !_planned_global_transform_2d(state, global_2d, r_error_message)) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				break;
			}
			bool valid = true;
			const Variant before = global_property_3d ? _planned_global_property(state, property_name) : (global_property_2d ? _global_property_2d(global_2d, property_name) : _get_property_for_patch(shadow, property_name, valid));
			if (!valid) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: property cannot be read for validation: " + String(property_name);
				break;
			}

			Variant decoded;
			bool contains_node_ref = false;
			int item_count = 0;
			if (!_decode_value(encoded_value, p_target, decoded, contains_node_ref, item_count, 0, r_error_code, r_error_message)) {
				break;
			}
			Variant coerced;
			bool was_coerced = false;
			if (!_coerce_value(property_info, before, decoded, coerced, was_coerced, r_error_message)) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				break;
			}
			if (coerced.get_type() == Variant::NODE_PATH && !_validate_node_path_value(p_target, NodePath(coerced), r_error_message)) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				break;
			}
			if (Node *node_shadow = Object::cast_to<Node>(shadow)) {
				if (!_validate_node_property(node_shadow, property_name, coerced, r_error_message)) {
					r_error_code = GodotMCPError::INVALID_ARGUMENT;
					break;
				}
			}

			bool set_valid = false;
			const Vector<Pair<StringName, Variant>> before_script_state = property_name == SNAME("script") ? _script_property_state(shadow) : Vector<Pair<StringName, Variant>>();
			if (global_property_3d) {
				set_valid = _set_planned_global_property(state, node_3d, property_name, coerced, r_error_message);
			} else if (global_property_2d) {
				set_valid = _set_planned_global_property_2d(state, node_2d, property_name, coerced, r_error_message);
			} else if (top_level_property) {
				if (node_3d->is_set_as_top_level() != bool(coerced)) {
					Transform3D transform = _planned_global_transform(state);
					if (!bool(coerced)) {
						const Transform3D parent = _planned_parent_transform(state);
						if (Math::is_zero_approx(parent.basis.determinant())) {
							r_error_code = GodotMCPError::INVALID_ARGUMENT;
							r_error_message = "INVALID_ARGUMENT: disabling top_level requires an invertible planned parent transform.";
							break;
						}
						transform = parent.affine_inverse() * transform;
					}
					node_3d->set_as_top_level_keep_local(coerced);
					node_3d->set_transform(transform);
				}
				set_valid = true;
			} else {
				shadow->set(property_name, coerced, &set_valid);
			}
			if (!set_valid) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: property setter rejected '" + String(property_name) + "' on " + p_target->get_class() + ".";
				break;
			}
			if (global_property_2d) {
				_planned_global_transform_2d(state, global_2d, r_error_message);
			}
			const Variant after = global_property_3d ? _planned_global_property(state, property_name) : (global_property_2d ? _global_property_2d(global_2d, property_name) : _get_property_for_patch(shadow, property_name, valid));
			if (!valid) {
				r_error_code = GodotMCPError::INTERNAL;
				r_error_message = "INTERNAL: property became unreadable after validation: " + String(property_name);
				break;
			}

			if (was_coerced) {
				_warn_property(r_operation.index, r_operation.local_id, p_target, property_name, "VALUE_COERCED",
						"Converted " + Variant::get_type_name(encoded_value.get_type()) + " to " + Variant::get_type_name(after.get_type()) + ".");
			}
			if (after != coerced) {
				_warn_property(r_operation.index, r_operation.local_id, p_target, property_name, "VALUE_NORMALIZED", "The Godot setter normalized or clamped the requested value.");
			}
			if (!(property_info.usage & PROPERTY_USAGE_STORAGE)) {
				_warn_property(r_operation.index, r_operation.local_id, p_target, property_name, "PROPERTY_NOT_STORED", "The property is not marked for direct scene serialization; its setter may persist an equivalent backing property.");
			}

			PropertyWrite write;
			write.target = p_target;
			write.shadow = shadow;
			// Store local backing properties so apply/undo do not depend on a
			// detached shadow's global getters or on later parent changes.
			write.property = global_property_3d ? SNAME("transform") : (global_property_2d ? local_property_2d : property_name);
			write.before = global_property_3d ? Variant(local_before) : (global_property_2d ? local_before_2d : before);
			write.after = global_property_3d ? Variant(node_3d->get_transform()) : (global_property_2d ? shadow->get(local_property_2d) : after);
			write.encoded = encoded_value;
			write.contains_node_ref = contains_node_ref;
			write.top_level_keep_local = top_level_property;
			write.before_script_state = before_script_state;
			write.before_node_2d_state = before_node_2d_state;
			if (property_name == SNAME("script")) {
				write.after_script_state = _script_property_state(shadow);
			}
			writes.push_back(write);
			if (top_level_property) {
				PropertyWrite transform_write;
				transform_write.target = p_target;
				transform_write.shadow = shadow;
				transform_write.property = SNAME("transform");
				transform_write.before = local_before;
				transform_write.after = node_3d->get_transform();
				writes.push_back(transform_write);
			}
		}

		if (r_error_code != 0) {
			for (int i = writes.size() - 1; i >= 0; i--) {
				if (writes[i].top_level_keep_local) {
					Object::cast_to<Node3D>(writes[i].shadow)->set_as_top_level_keep_local(writes[i].before);
				} else {
					writes[i].shadow->set(writes[i].property, writes[i].before);
					_restore_property_state(writes[i].shadow, writes[i].before_script_state);
					_restore_property_state(writes[i].shadow, writes[i].before_node_2d_state);
				}
			}
			property_warnings.resize(warning_start);
			return false;
		}
		for (const PropertyWrite &write : writes) {
			r_operation.property_writes.push_back(write);
		}
		return true;
	}

	bool _parse_position(const Dictionary &p_operation, int &r_position, int &r_error_code, String &r_error_message) const {
		if (!p_operation.has("position")) {
			r_position = -1;
			return true;
		}
		int64_t position = 0;
		if (!_variant_to_integer(p_operation["position"], -1, INT32_MAX, position)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: position must be an integer between -1 and " + itos(INT32_MAX) + ".";
			return false;
		}
		r_position = int(position);
		return true;
	}

	bool _validate_node_name(const String &p_name, String &r_error_message) const {
		if (p_name.is_empty() || p_name.validate_node_name() != p_name) {
			r_error_message = "INVALID_ARGUMENT: node name is empty or contains forbidden NodePath characters.";
			return false;
		}
		return true;
	}

	bool _sibling_name_available(NodeState *p_parent, const NodeState *p_except, const String &p_name) const {
		if (!p_parent) {
			return true; // The edited scene root has no scene-local siblings.
		}
		for (NodeState *child : p_parent->children) {
			if (child != p_except && !_is_effectively_deleted(child) && child->name == p_name) {
				return false;
			}
		}
		return true;
	}

	bool _extract_node_name(const Dictionary &p_input, Dictionary &r_properties, const String &p_default_name,
			String &r_name, bool &r_has_name, int &r_error_code, String &r_error_message) const {
		const bool property_name = r_properties.has("name");
		r_has_name = p_input.has("name") || property_name;
		r_name = p_default_name;
		if (!r_has_name) {
			return true;
		}
		if (p_input.has("name") && property_name) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: provide name as either an operation field or a property, not both.";
			return false;
		}
		Variant name = property_name ? r_properties["name"] : p_input["name"];
		if (property_name && name.get_type() == Variant::DICTIONARY) {
			const Dictionary wrapper = name;
			if (wrapper.get("__type", Variant()) == Variant("StringName")) {
				name = wrapper.get("value", Variant());
			}
		}
		if (name.get_type() != Variant::STRING && name.get_type() != Variant::STRING_NAME) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: node name must be a string.";
			return false;
		}
		r_name = String(name);
		if (!_validate_node_name(r_name, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			return false;
		}
		if (property_name) {
			// Preserve the caller's dictionary while normalizing the operation.
			r_properties = r_properties.duplicate();
			r_properties.erase("name");
		}
		return true;
	}

	bool _parse_group_array(const Variant &p_value, Vector<StringName> &r_groups, int &r_error_code, String &r_error_message) const {
		if (p_value.get_type() != Variant::ARRAY) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: groups must be arrays of non-empty strings.";
			return false;
		}
		HashSet<StringName> seen;
		const Array groups = p_value;
		for (int i = 0; i < groups.size(); i++) {
			if ((groups[i].get_type() != Variant::STRING && groups[i].get_type() != Variant::STRING_NAME) || String(groups[i]).is_empty()) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: groups must contain only non-empty strings.";
				return false;
			}
			const StringName group = groups[i];
			if (String(group).begins_with("_")) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: internal groups beginning with '_' cannot be authored by a scene patch.";
				return false;
			}
			if (!seen.has(group)) {
				seen.insert(group);
				r_groups.push_back(group);
			}
		}
		return true;
	}

	bool _plan_groups(const Dictionary &p_operation, NodeState *p_state,
			Vector<StringName> &r_added, Vector<StringName> &r_removed,
			int &r_error_code, String &r_error_message) const {
		HashSet<StringName> desired;
		for (const StringName &group : p_state->groups) {
			desired.insert(group);
		}
		if (p_operation.has("groups")) {
			Vector<StringName> exact_groups;
			if (!_parse_group_array(p_operation["groups"], exact_groups, r_error_code, r_error_message)) {
				return false;
			}
			desired.clear();
			for (const StringName &group : exact_groups) {
				desired.insert(group);
			}
		}
		if (p_operation.has("add_groups")) {
			Vector<StringName> groups;
			if (!_parse_group_array(p_operation["add_groups"], groups, r_error_code, r_error_message)) {
				return false;
			}
			for (const StringName &group : groups) {
				desired.insert(group);
			}
		}
		if (p_operation.has("remove_groups")) {
			Vector<StringName> groups;
			if (!_parse_group_array(p_operation["remove_groups"], groups, r_error_code, r_error_message)) {
				return false;
			}
			for (const StringName &group : groups) {
				desired.erase(group);
			}
		}

		for (const StringName &group : desired) {
			if (!p_state->groups.has(group)) {
				r_added.push_back(group);
			}
		}
		for (const StringName &group : p_state->groups) {
			if (!desired.has(group)) {
				r_removed.push_back(group);
			}
		}
		r_added.sort();
		r_removed.sort();
		return true;
	}

	bool _declare_local_id(const Dictionary &p_operation, String &r_id, int &r_error_code, String &r_error_message) {
		const Variant id_variant = p_operation.get("id", Variant());
		if ((id_variant.get_type() != Variant::STRING && id_variant.get_type() != Variant::STRING_NAME) || String(id_variant).is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: create operations require a non-empty string id.";
			return false;
		}
		r_id = String(id_variant);
		if (r_id.length() > 128 || declared_local_ids.has(r_id)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = declared_local_ids.has(r_id) ? "INVALID_ARGUMENT: duplicate request-local ID: " + r_id : "INVALID_ARGUMENT: request-local IDs are limited to 128 characters.";
			return false;
		}
		declared_local_ids.insert(r_id);
		return true;
	}

	String _make_resource_unique_id(const String &p_type, const String &p_local_id) {
		String suffix = p_local_id.validate_ascii_identifier();
		if (suffix.is_empty()) {
			suffix = p_local_id.md5_text().left(10);
		}
		String candidate = (p_type + "_" + suffix).left(120);
		if (candidate.is_empty()) {
			candidate = "Resource_" + p_local_id.md5_text().left(10);
		}
		const String base = candidate;
		int collision = 2;
		while (reserved_resource_ids.has(candidate) || ResourceCache::has(context.scene_path + "::" + candidate)) {
			candidate = (base.left(108) + "_" + itos(collision++)).left(120);
		}
		reserved_resource_ids.insert(candidate);
		return candidate;
	}

	bool _plan_create_resource(const Dictionary &p_input, int p_index, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String local_id;
		if (!_declare_local_id(p_input, local_id, r_error_code, r_error_message)) {
			return false;
		}
		r_operation.local_id = local_id;

		const Variant type_variant = p_input.get("type", Variant());
		if ((type_variant.get_type() != Variant::STRING && type_variant.get_type() != Variant::STRING_NAME) || String(type_variant).is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: create_resource requires a non-empty Resource class type.";
			return false;
		}
		const String type_name = type_variant;
		Object *object = ClassDB::instantiate(type_name);
		Resource *resource_ptr = Object::cast_to<Resource>(object);
		if (!resource_ptr) {
			if (object) {
				memdelete(object);
			}
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: type is not a concrete Resource class: " + type_name;
			return false;
		}
		Ref<Resource> resource(resource_ptr);
		resource_by_local_id.insert(local_id, resource);

		Dictionary properties;
		if (!_collect_properties(p_input, properties, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			resource_by_local_id.erase(local_id);
			return false;
		}
		r_operation.resource = resource;
		r_operation.resource_unique_id = _make_resource_unique_id(type_name, local_id);
		if (!_plan_property_writes(r_operation, resource.ptr(), properties, r_error_code, r_error_message, p_input.get("property_entries", Array()))) {
			resource_by_local_id.erase(local_id);
			return false;
		}

		created_resources.push_back(resource);
		Dictionary summary;
		summary["class"] = type_name;
		summary["resource_id"] = r_operation.resource_unique_id;
		summary["resource_path"] = context.scene_path + "::" + r_operation.resource_unique_id;
		summary["property_count"] = properties.size();
		r_operation.summary = summary;
		return true;
	}

	bool _plan_update_resource(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String dependency_id;
		Ref<Resource> resource = _resolve_resource_selector(p_input, dependency_id, r_error_code, r_error_message);
		if (resource.is_null()) {
			return false;
		}
		Dictionary properties;
		if (!_collect_properties(p_input, properties, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			return false;
		}
		if (properties.is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: update_resource requires non-empty properties or property_entries.";
			return false;
		}
		r_operation.resource = resource;
		if (!_plan_property_writes(r_operation, resource.ptr(), properties, r_error_code, r_error_message, p_input.get("property_entries", Array()))) {
			return false;
		}
		Dictionary summary;
		summary["class"] = resource->get_class();
		summary["resource_path"] = resource->get_path();
		summary["property_count"] = properties.size();
		r_operation.summary = summary;
		return true;
	}

	bool _plan_create_node(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String local_id;
		if (!_declare_local_id(p_input, local_id, r_error_code, r_error_message)) {
			return false;
		}
		r_operation.local_id = local_id;

		String parent_dependency;
		NodeState *parent = _resolve_node_selector(p_input, "parent", parent_dependency, r_error_code, r_error_message);
		if (!parent) {
			return false;
		}
		const Variant type_variant = p_input.get("type", Variant());
		if ((type_variant.get_type() != Variant::STRING && type_variant.get_type() != Variant::STRING_NAME) || String(type_variant).is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: create_node requires a non-empty Node class type.";
			return false;
		}
		const String type_name = type_variant;
		Object *object = ClassDB::instantiate(type_name);
		Node *node = Object::cast_to<Node>(object);
		if (!node) {
			if (object) {
				memdelete(object);
			}
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: type is not a concrete Node class: " + type_name;
			return false;
		}

		Dictionary properties;
		if (!_collect_properties(p_input, properties, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			memdelete(node);
			return false;
		}
		String node_name;
		bool has_name = false;
		if (!_extract_node_name(p_input, properties, type_name, node_name, has_name, r_error_code, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			memdelete(node);
			return false;
		}
		if (!_sibling_name_available(parent, nullptr, node_name)) {
			r_error_code = GodotMCPError::CONFLICT;
			r_error_message = "CONFLICT: parent already has a child named '" + node_name + "'.";
			memdelete(node);
			return false;
		}
		int requested_position = -1;
		if (!_parse_position(p_input, requested_position, r_error_code, r_error_message)) {
			memdelete(node);
			return false;
		}
		if (requested_position > parent->children.size()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: create_node position exceeds the proposed parent child count.";
			memdelete(node);
			return false;
		}
		const int final_position = requested_position < 0 ? parent->children.size() : requested_position;
		node->set_name(node_name);

		NodeState *state = memnew(NodeState);
		state->node = node;
		state->owner = context.root;
		state->name = node_name;
		state->local_id = local_id;
		state->created = true;
		node_states.push_back(state);
		node_state_by_object.insert(node->get_instance_id(), state);
		node_by_local_id.insert(local_id, state);
		_insert_child_state(parent, state, final_position);

		Vector<StringName> groups_added;
		Vector<StringName> groups_removed;
		if (!_plan_groups(p_input, state, groups_added, groups_removed, r_error_code, r_error_message)) {
			_remove_child_state(parent, state);
			node_by_local_id.erase(local_id);
			node_state_by_object.erase(node->get_instance_id());
			memdelete(node);
			state->node = nullptr;
			return false;
		}
		r_operation.node_state = state;
		r_operation.new_parent_state = parent;
		r_operation.new_position = final_position;
		r_operation.groups_added = groups_added;
		if (!_plan_property_writes(r_operation, node, properties, r_error_code, r_error_message, p_input.get("property_entries", Array()))) {
			_remove_child_state(parent, state);
			node_by_local_id.erase(local_id);
			node_state_by_object.erase(node->get_instance_id());
			memdelete(node);
			state->node = nullptr;
			return false;
		}
		for (const StringName &group : groups_added) {
			state->groups.insert(group);
		}
		created_nodes.push_back(node);

		Dictionary summary;
		summary["class"] = type_name;
		summary["node_path"] = _state_path(state);
		summary["property_count"] = properties.size();
		summary["group_count"] = groups_added.size();
		r_operation.summary = summary;
		return true;
	}

	bool _plan_update_node(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String dependency_id;
		NodeState *state = _resolve_node_selector(p_input, "node", dependency_id, r_error_code, r_error_message);
		if (!state) {
			return false;
		}
		Dictionary properties;
		if (!_collect_properties(p_input, properties, r_error_message)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			return false;
		}
		const bool has_group_change = p_input.has("groups") || p_input.has("add_groups") || p_input.has("remove_groups");
		String new_name;
		bool has_name_change = false;
		if (!_extract_node_name(p_input, properties, state->name, new_name, has_name_change, r_error_code, r_error_message)) {
			return false;
		}
		if (properties.is_empty() && !has_group_change && !has_name_change) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: update_node requires properties, groups, or name.";
			return false;
		}

		if (has_name_change) {
			if (!_sibling_name_available(state->parent, state, new_name)) {
				r_error_code = GodotMCPError::CONFLICT;
				r_error_message = "CONFLICT: parent already has a child named '" + new_name + "'.";
				return false;
			}
		}
		Vector<StringName> groups_added;
		Vector<StringName> groups_removed;
		if (!_plan_groups(p_input, state, groups_added, groups_removed, r_error_code, r_error_message)) {
			return false;
		}

		const String old_name = state->name;
		state->name = new_name;
		r_operation.node_state = state;
		r_operation.old_name = old_name;
		r_operation.new_name = new_name;
		r_operation.groups_added = groups_added;
		r_operation.groups_removed = groups_removed;
		if (!_plan_property_writes(r_operation, state->node, properties, r_error_code, r_error_message, p_input.get("property_entries", Array()))) {
			state->name = old_name;
			return false;
		}
		for (const StringName &group : groups_added) {
			state->groups.insert(group);
		}
		for (const StringName &group : groups_removed) {
			state->groups.erase(group);
		}

		Dictionary summary;
		summary["node_path"] = _state_path(state);
		summary["property_count"] = properties.size();
		summary["groups_added"] = groups_added.size();
		summary["groups_removed"] = groups_removed.size();
		summary["renamed"] = old_name != new_name;
		r_operation.summary = summary;
		return true;
	}

	bool _plan_move_node(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String node_dependency;
		String parent_dependency;
		NodeState *state = _resolve_node_selector(p_input, "node", node_dependency, r_error_code, r_error_message);
		if (!state) {
			return false;
		}
		NodeState *new_parent = _resolve_node_selector(p_input, "parent", parent_dependency, r_error_code, r_error_message);
		if (!new_parent) {
			return false;
		}
		if (state->node == context.root || state->internal || state == new_parent) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: root/internal nodes cannot be moved, and a node cannot parent itself.";
			return false;
		}
		for (NodeState *cursor = new_parent; cursor; cursor = cursor->parent) {
			if (cursor == state) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: move_node would create a parent cycle.";
				return false;
			}
		}

		String new_name = state->name;
		if (p_input.has("name")) {
			const Variant name_variant = p_input["name"];
			if ((name_variant.get_type() != Variant::STRING && name_variant.get_type() != Variant::STRING_NAME) || !_validate_node_name(String(name_variant), r_error_message)) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				return false;
			}
			new_name = String(name_variant);
		}
		if (!_sibling_name_available(new_parent, state->parent == new_parent ? state : nullptr, new_name)) {
			r_error_code = GodotMCPError::CONFLICT;
			r_error_message = "CONFLICT: destination parent already has a child named '" + new_name + "'.";
			return false;
		}

		int requested_position = -1;
		if (!_parse_position(p_input, requested_position, r_error_code, r_error_message)) {
			return false;
		}
		const int destination_count = new_parent->children.size() - (state->parent == new_parent ? 1 : 0);
		if (requested_position > destination_count) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: move_node position exceeds the proposed destination child count.";
			return false;
		}
		const int final_position = requested_position < 0 ? destination_count : requested_position;

		NodeState *old_parent = state->parent;
		const int old_position = _child_index(old_parent, state);
		const String old_name = state->name;
		_capture_subtree_owners(state, r_operation.owner_writes);
		_remove_child_state(old_parent, state);
		_insert_child_state(new_parent, state, final_position);
		state->name = new_name;

		r_operation.node_state = state;
		r_operation.old_parent_state = old_parent;
		r_operation.new_parent_state = new_parent;
		r_operation.old_position = old_position;
		r_operation.new_position = final_position;
		r_operation.old_name = old_name;
		r_operation.new_name = new_name;
		_plan_moved_owners(r_operation);

		Dictionary summary;
		summary["from_parent"] = _state_path(old_parent);
		summary["to_parent"] = _state_path(new_parent);
		summary["node_path"] = _state_path(state);
		summary["position"] = final_position;
		r_operation.summary = summary;
		return true;
	}

	bool _plan_delete_node(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String dependency_id;
		NodeState *state = _resolve_node_selector(p_input, "node", dependency_id, r_error_code, r_error_message);
		if (!state) {
			return false;
		}
		if (state->node == context.root || state->internal || !state->parent) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: deleting root/internal/unparented nodes is not supported.";
			return false;
		}
		NodeState *old_parent = state->parent;
		const int old_position = _child_index(old_parent, state);
		const String deleted_path = _state_path(state);
		_capture_subtree_owners(state, r_operation.owner_writes);
		_remove_child_state(old_parent, state);
		state->deleted = true;

		r_operation.node_state = state;
		r_operation.old_parent_state = old_parent;
		r_operation.old_position = old_position;
		Dictionary summary;
		summary["deleted_node_path"] = deleted_path;
		r_operation.summary = summary;
		return true;
	}

	Object *_effective_object(Object *p_object) const {
		if (Object *const *shadow = shadow_by_object.getptr(_object_key(p_object))) {
			return *shadow;
		}
		return p_object;
	}

	bool _replace_resource_in_value(const Variant &p_input, const Ref<Resource> &p_target,
			Variant &r_output, bool &r_changed, int p_depth, int &r_error_code, String &r_error_message) const {
		if (p_depth > MAX_PATCH_VALUE_DEPTH) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: resource reference graph exceeds patch nesting bounds.";
			return false;
		}
		if (p_input.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_input;
			if (resource.is_valid() && resource == p_target) {
				r_output = Variant();
				r_changed = true;
				return true;
			}
			r_output = p_input;
			return true;
		}
		if (p_input.get_type() == Variant::ARRAY) {
			const Array input = p_input;
			Array output = input.duplicate();
			for (int i = 0; i < input.size(); i++) {
				Variant replaced;
				bool changed = false;
				if (!_replace_resource_in_value(input[i], p_target, replaced, changed, p_depth + 1, r_error_code, r_error_message)) {
					return false;
				}
				if (changed) {
					output[i] = replaced;
					r_changed = true;
				}
			}
			r_output = output;
			return true;
		}
		if (p_input.get_type() == Variant::DICTIONARY) {
			const Dictionary input = p_input;
			Dictionary output = input.duplicate();
			const Array keys = input.keys();
			for (int i = 0; i < keys.size(); i++) {
				const Variant key = keys[i];
				if (key.get_type() == Variant::OBJECT) {
					Ref<Resource> key_resource = key;
					if (key_resource.is_valid() && key_resource == p_target) {
						output.erase(key);
						r_changed = true;
						continue;
					}
				}
				Variant replaced;
				bool changed = false;
				if (!_replace_resource_in_value(input[key], p_target, replaced, changed, p_depth + 1, r_error_code, r_error_message)) {
					return false;
				}
				if (changed) {
					output[key] = replaced;
					r_changed = true;
				}
			}
			r_output = output;
			return true;
		}
		r_output = p_input;
		return true;
	}

	bool _is_patch_embedded_resource(const Ref<Resource> &p_resource) const {
		if (p_resource.is_null()) {
			return false;
		}
		const String path = p_resource->get_path();
		if (path.begins_with(context.scene_path + "::")) {
			return true;
		}
		for (const Ref<Resource> &created : created_resources) {
			if (created == p_resource) {
				return true;
			}
		}
		return false;
	}

	bool _scan_nested_resources(const Variant &p_value, const Ref<Resource> &p_deleted_target,
			PatchOperation &r_operation, HashSet<ObjectID> &r_visited,
			int &r_detached_count, int p_depth, int &r_error_code, String &r_error_message) {
		if (p_depth > MAX_PATCH_VALUE_DEPTH) {
			return true;
		}
		if (p_value.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_value;
			if (resource.is_valid() && resource != p_deleted_target && _is_patch_embedded_resource(resource)) {
				return _scan_resource_object(resource, p_deleted_target, r_operation, r_visited, r_detached_count, p_depth + 1, r_error_code, r_error_message);
			}
			return true;
		}
		if (p_value.get_type() == Variant::ARRAY) {
			const Array array = p_value;
			for (int i = 0; i < array.size(); i++) {
				if (!_scan_nested_resources(array[i], p_deleted_target, r_operation, r_visited, r_detached_count, p_depth + 1, r_error_code, r_error_message)) {
					return false;
				}
			}
		} else if (p_value.get_type() == Variant::DICTIONARY) {
			const Dictionary dictionary = p_value;
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				if (!_scan_nested_resources(E.key, p_deleted_target, r_operation, r_visited, r_detached_count, p_depth + 1, r_error_code, r_error_message) ||
						!_scan_nested_resources(E.value, p_deleted_target, r_operation, r_visited, r_detached_count, p_depth + 1, r_error_code, r_error_message)) {
					return false;
				}
			}
		}
		return true;
	}

	bool _scan_resource_object(const Ref<Resource> &p_resource, const Ref<Resource> &p_deleted_target,
			PatchOperation &r_operation, HashSet<ObjectID> &r_visited,
			int &r_detached_count, int p_depth, int &r_error_code, String &r_error_message) {
		if (p_resource.is_null() || p_resource == p_deleted_target || r_visited.has(p_resource->get_instance_id())) {
			return true;
		}
		r_visited.insert(p_resource->get_instance_id());
		Object *effective = _effective_object(p_resource.ptr());
		List<PropertyInfo> properties;
		effective->get_property_list(&properties);
		for (const PropertyInfo &property : properties) {
			if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}
			bool valid = false;
			const Variant current = effective->get(property.name, &valid);
			if (!valid) {
				continue;
			}
			Variant replacement;
			bool changed = false;
			if (!_replace_resource_in_value(current, p_deleted_target, replacement, changed, 0, r_error_code, r_error_message)) {
				return false;
			}
			if (changed) {
				Dictionary write;
				write[property.name] = replacement;
				if (!_plan_property_writes(r_operation, p_resource.ptr(), write, r_error_code, r_error_message)) {
					return false;
				}
				r_detached_count++;
			}
			if (!_scan_nested_resources(replacement, p_deleted_target, r_operation, r_visited, r_detached_count, p_depth + 1, r_error_code, r_error_message)) {
				return false;
			}
		}
		return true;
	}

	bool _plan_delete_resource(const Dictionary &p_input, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		String dependency_id;
		Ref<Resource> resource = _resolve_resource_selector(p_input, dependency_id, r_error_code, r_error_message);
		if (resource.is_null()) {
			return false;
		}
		if (!_is_patch_embedded_resource(resource)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: delete_resource only accepts embedded resources owned by expected_scene_path.";
			return false;
		}

		const int write_start = r_operation.property_writes.size();
		const int warning_start = property_warnings.size();
		int detached_count = 0;
		HashSet<ObjectID> visited;
		for (NodeState *state : node_states) {
			if (!state->node || _is_effectively_deleted(state)) {
				continue;
			}
			Object *effective = _effective_object(state->node);
			List<PropertyInfo> properties;
			effective->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
					continue;
				}
				bool valid = false;
				const Variant current = effective->get(property.name, &valid);
				if (!valid) {
					continue;
				}
				Variant replacement;
				bool changed = false;
				if (!_replace_resource_in_value(current, resource, replacement, changed, 0, r_error_code, r_error_message)) {
					break;
				}
				if (changed) {
					Dictionary write;
					write[property.name] = replacement;
					if (!_plan_property_writes(r_operation, state->node, write, r_error_code, r_error_message)) {
						break;
					}
					detached_count++;
				}
				if (!_scan_nested_resources(replacement, resource, r_operation, visited, detached_count, 0, r_error_code, r_error_message)) {
					break;
				}
			}
			if (r_error_code != 0) {
				break;
			}
		}
		for (const Ref<Resource> &created : created_resources) {
			if (created != resource && !_scan_resource_object(created, resource, r_operation, visited, detached_count, 0, r_error_code, r_error_message)) {
				break;
			}
		}

		if (r_error_code != 0) {
			for (int i = r_operation.property_writes.size() - 1; i >= write_start; i--) {
				const PropertyWrite &write = r_operation.property_writes[i];
				write.shadow->set(write.property, write.before);
				_restore_property_state(write.shadow, write.before_script_state);
				_restore_property_state(write.shadow, write.before_node_2d_state);
			}
			r_operation.property_writes.resize(write_start);
			property_warnings.resize(warning_start);
			return false;
		}

		deleted_resources.insert(resource->get_instance_id());
		r_operation.resource = resource;
		r_operation.resource_deleted = true;
		Dictionary summary;
		summary["class"] = resource->get_class();
		summary["resource_path"] = resource->get_path();
		summary["detached_reference_count"] = detached_count;
		r_operation.summary = summary;
		return true;
	}

	bool _validate_node_paths_recursive(Object *p_target, const Variant &p_value, int p_depth, String &r_error_message) const {
		if (p_depth > MAX_PATCH_VALUE_DEPTH) {
			return true;
		}
		if (p_value.get_type() == Variant::NODE_PATH) {
			return _validate_node_path_value(p_target, NodePath(p_value), r_error_message);
		}
		if (p_value.get_type() == Variant::ARRAY) {
			const Array array = p_value;
			for (int i = 0; i < array.size(); i++) {
				if (!_validate_node_paths_recursive(p_target, array[i], p_depth + 1, r_error_message)) {
					return false;
				}
			}
		} else if (p_value.get_type() == Variant::DICTIONARY) {
			const Dictionary dictionary = p_value;
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				if (!_validate_node_paths_recursive(p_target, E.key, p_depth + 1, r_error_message) ||
						!_validate_node_paths_recursive(p_target, E.value, p_depth + 1, r_error_message)) {
					return false;
				}
			}
		}
		return true;
	}

	bool _refresh_and_validate_node_paths(int &r_error_code, String &r_error_message, int &r_failed_index) {
		for (PatchOperation &operation : operations) {
			r_failed_index = operation.index;
			for (PropertyWrite &write : operation.property_writes) {
				Variant final_value = write.after;
				if (write.contains_node_ref) {
					Variant decoded;
					bool contains_node_ref = false;
					int item_count = 0;
					if (!_decode_value(write.encoded, write.target, decoded, contains_node_ref, item_count, 0, r_error_code, r_error_message)) {
						return false;
					}
					PropertyInfo info;
					if (!_find_property_info(write.shadow, write.property, info)) {
						r_error_code = GodotMCPError::INTERNAL;
						r_error_message = "INTERNAL: property disappeared during final patch validation: " + String(write.property);
						return false;
					}
					bool coerced = false;
					if (!_coerce_value(info, write.before, decoded, final_value, coerced, r_error_message)) {
						r_error_code = GodotMCPError::INVALID_ARGUMENT;
						return false;
					}
				}
				if (!_validate_node_paths_recursive(write.target, final_value, 0, r_error_message)) {
					r_error_code = GodotMCPError::INVALID_ARGUMENT;
					return false;
				}
				write.after = final_value;
			}
		}
		return true;
	}

	void _clear_plan() {
		for (Node *shadow : shadow_nodes) {
			memdelete(shadow);
		}
		shadow_nodes.clear();
		shadow_resources.clear();
		shadow_by_object.clear();
		operations.clear();
		for (NodeState *state : node_states) {
			memdelete(state);
		}
		node_states.clear();
		node_state_by_object.clear();
		node_by_local_id.clear();
		resource_by_local_id.clear();
		created_resources.clear();
		if (!committed) {
			for (Node *node : created_nodes) {
				memdelete(node);
			}
		}
		created_nodes.clear();
		failed_local_ids.clear();
		declared_local_ids.clear();
		deleted_resources.clear();
		reserved_resource_ids.clear();
		// Result dictionaries retain these Array instances. Release our handles
		// without clearing the shared containers during destruction or replanning.
		results = Array();
		property_warnings = Array();
		failure_count = 0;
		skipped_count = 0;
	}

	bool _plan_one(const Dictionary &p_input, int p_index, PatchOperation &r_operation,
			int &r_error_code, String &r_error_message) {
		const Variant op_variant = p_input.get("op", Variant());
		if ((op_variant.get_type() != Variant::STRING && op_variant.get_type() != Variant::STRING_NAME) || String(op_variant).is_empty()) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: each patch operation requires a non-empty op string.";
			return false;
		}
		const String op_name = op_variant;
		r_operation.index = p_index;
		r_operation.name = op_name;

		if (op_name == "create_resource") {
			r_operation.kind = PATCH_CREATE_RESOURCE;
			return _plan_create_resource(p_input, p_index, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "update_resource") {
			r_operation.kind = PATCH_UPDATE_RESOURCE;
			return _plan_update_resource(p_input, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "delete_resource") {
			r_operation.kind = PATCH_DELETE_RESOURCE;
			return _plan_delete_resource(p_input, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "create_node") {
			r_operation.kind = PATCH_CREATE_NODE;
			return _plan_create_node(p_input, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "update_node") {
			r_operation.kind = PATCH_UPDATE_NODE;
			return _plan_update_node(p_input, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "move_node") {
			r_operation.kind = PATCH_MOVE_NODE;
			return _plan_move_node(p_input, r_operation, r_error_code, r_error_message);
		}
		if (op_name == "delete_node") {
			r_operation.kind = PATCH_DELETE_NODE;
			return _plan_delete_node(p_input, r_operation, r_error_code, r_error_message);
		}

		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: unsupported scene patch operation: " + op_name;
		return false;
	}

	void _add_property_do_ops(UndoRedo *p_undo_redo, const PatchOperation &p_operation) const {
		for (const PropertyWrite &write : p_operation.property_writes) {
			if (write.top_level_keep_local) {
				p_undo_redo->add_do_method(callable_mp(Object::cast_to<Node3D>(write.target), &Node3D::set_as_top_level_keep_local).bind(write.after));
			} else {
				p_undo_redo->add_do_property(write.target, write.property, write.after);
				for (const Pair<StringName, Variant> &property : write.after_script_state) {
					p_undo_redo->add_do_property(write.target, property.first, property.second);
				}
			}
		}
	}

	void _add_property_undo_ops(UndoRedo *p_undo_redo, const PatchOperation &p_operation) const {
		for (int i = p_operation.property_writes.size() - 1; i >= 0; i--) {
			const PropertyWrite &write = p_operation.property_writes[i];
			if (write.top_level_keep_local) {
				p_undo_redo->add_undo_method(callable_mp(Object::cast_to<Node3D>(write.target), &Node3D::set_as_top_level_keep_local).bind(write.before));
			} else {
				p_undo_redo->add_undo_property(write.target, write.property, write.before);
				for (const Pair<StringName, Variant> &property : write.before_script_state) {
					p_undo_redo->add_undo_property(write.target, property.first, property.second);
				}
				// Node2D stores components instead of its matrix. Restoring them
				// avoids changing serialized values through another decomposition.
				for (const Pair<StringName, Variant> &property : write.before_node_2d_state) {
					p_undo_redo->add_undo_property(write.target, property.first, property.second);
				}
			}
		}
	}

	void _add_do_ops(UndoRedo *p_undo_redo, const PatchOperation &p_operation) const {
		switch (p_operation.kind) {
			case PATCH_CREATE_RESOURCE:
			case PATCH_UPDATE_RESOURCE:
			case PATCH_DELETE_RESOURCE:
				_add_property_do_ops(p_undo_redo, p_operation);
				break;
			case PATCH_CREATE_NODE: {
				Node *node = p_operation.node_state->node;
				Node *parent = p_operation.new_parent_state->node;
				_add_do_method(p_undo_redo, parent, SNAME("add_child"), _args({ node, true }));
				_add_do_method(p_undo_redo, parent, SNAME("move_child"), _args({ node, p_operation.new_position }));
				_add_do_method(p_undo_redo, node, SNAME("set_owner"), _args({ context.root }));
				_add_property_do_ops(p_undo_redo, p_operation);
				for (const StringName &group : p_operation.groups_added) {
					_add_do_method(p_undo_redo, node, SNAME("add_to_group"), _args({ group, true }));
				}
				p_undo_redo->add_do_reference(node);
			} break;
			case PATCH_UPDATE_NODE: {
				Node *node = p_operation.node_state->node;
				if (p_operation.old_name != p_operation.new_name) {
					_add_do_method(p_undo_redo, node, SNAME("set_name"), _args({ p_operation.new_name }));
				}
				_add_property_do_ops(p_undo_redo, p_operation);
				for (const StringName &group : p_operation.groups_added) {
					_add_do_method(p_undo_redo, node, SNAME("add_to_group"), _args({ group, true }));
				}
				for (const StringName &group : p_operation.groups_removed) {
					_add_do_method(p_undo_redo, node, SNAME("remove_from_group"), _args({ group }));
				}
			} break;
			case PATCH_MOVE_NODE: {
				Node *node = p_operation.node_state->node;
				Node *old_parent = p_operation.old_parent_state->node;
				Node *new_parent = p_operation.new_parent_state->node;
				_add_do_method(p_undo_redo, node, SNAME("set_owner"), _args({ Variant() }));
				_add_do_method(p_undo_redo, old_parent, SNAME("remove_child"), _args({ node }));
				_add_do_method(p_undo_redo, node, SNAME("set_name"), _args({ p_operation.new_name }));
				_add_do_method(p_undo_redo, new_parent, SNAME("add_child"), _args({ node, true }));
				_add_do_method(p_undo_redo, new_parent, SNAME("move_child"), _args({ node, p_operation.new_position }));
				for (const OwnerWrite &write : p_operation.owner_writes) {
					_add_do_method(p_undo_redo, write.node, SNAME("set_owner"), _args({ write.after }));
				}
			} break;
			case PATCH_DELETE_NODE: {
				Node *node = p_operation.node_state->node;
				_add_do_method(p_undo_redo, node, SNAME("set_owner"), _args({ Variant() }));
				_add_do_method(p_undo_redo, p_operation.old_parent_state->node, SNAME("remove_child"), _args({ node }));
			} break;
		}
	}

	void _add_undo_ops(UndoRedo *p_undo_redo, const PatchOperation &p_operation) const {
		switch (p_operation.kind) {
			case PATCH_CREATE_RESOURCE:
			case PATCH_UPDATE_RESOURCE:
			case PATCH_DELETE_RESOURCE:
				_add_property_undo_ops(p_undo_redo, p_operation);
				break;
			case PATCH_CREATE_NODE: {
				Node *node = p_operation.node_state->node;
				for (int i = p_operation.groups_added.size() - 1; i >= 0; i--) {
					_add_undo_method(p_undo_redo, node, SNAME("remove_from_group"), _args({ p_operation.groups_added[i] }));
				}
				_add_property_undo_ops(p_undo_redo, p_operation);
				_add_undo_method(p_undo_redo, node, SNAME("set_owner"), _args({ Variant() }));
				_add_undo_method(p_undo_redo, p_operation.new_parent_state->node, SNAME("remove_child"), _args({ node }));
			} break;
			case PATCH_UPDATE_NODE: {
				Node *node = p_operation.node_state->node;
				for (int i = p_operation.groups_removed.size() - 1; i >= 0; i--) {
					_add_undo_method(p_undo_redo, node, SNAME("add_to_group"), _args({ p_operation.groups_removed[i], true }));
				}
				for (int i = p_operation.groups_added.size() - 1; i >= 0; i--) {
					_add_undo_method(p_undo_redo, node, SNAME("remove_from_group"), _args({ p_operation.groups_added[i] }));
				}
				_add_property_undo_ops(p_undo_redo, p_operation);
				if (p_operation.old_name != p_operation.new_name) {
					_add_undo_method(p_undo_redo, node, SNAME("set_name"), _args({ p_operation.old_name }));
				}
			} break;
			case PATCH_MOVE_NODE: {
				Node *node = p_operation.node_state->node;
				_add_undo_method(p_undo_redo, node, SNAME("set_owner"), _args({ Variant() }));
				_add_undo_method(p_undo_redo, p_operation.new_parent_state->node, SNAME("remove_child"), _args({ node }));
				_add_undo_method(p_undo_redo, node, SNAME("set_name"), _args({ p_operation.old_name }));
				_add_undo_method(p_undo_redo, p_operation.old_parent_state->node, SNAME("add_child"), _args({ node, true }));
				_add_undo_method(p_undo_redo, p_operation.old_parent_state->node, SNAME("move_child"), _args({ node, p_operation.old_position }));
				for (const OwnerWrite &write : p_operation.owner_writes) {
					_add_undo_method(p_undo_redo, write.node, SNAME("set_owner"), _args({ write.before }));
				}
			} break;
			case PATCH_DELETE_NODE: {
				Node *node = p_operation.node_state->node;
				_add_undo_method(p_undo_redo, p_operation.old_parent_state->node, SNAME("add_child"), _args({ node, true }));
				_add_undo_method(p_undo_redo, p_operation.old_parent_state->node, SNAME("move_child"), _args({ node, p_operation.old_position }));
				for (const OwnerWrite &write : p_operation.owner_writes) {
					_add_undo_method(p_undo_redo, write.node, SNAME("set_owner"), _args({ write.before }));
				}
				p_undo_redo->add_undo_reference(node);
			} break;
		}
	}

	void _refresh_summaries_and_results(const String &p_success_status) {
		for (PatchOperation &operation : operations) {
			if (operation.node_state && operation.kind != PATCH_DELETE_NODE) {
				operation.summary["node_path"] = _state_path(operation.node_state);
			}
			results[operation.index] = _operation_success(operation, p_success_status);
		}
	}

	Dictionary _build_data(const String &p_previous_version, const String &p_updated_version, bool p_applied,
			bool p_save_requested) const {
		Dictionary created_node_paths;
		Dictionary created_resource_ids;
		Dictionary created_resource_paths;
		int created_node_count = 0;
		int updated_node_count = 0;
		int moved_node_count = 0;
		int deleted_node_count = 0;
		int created_resource_count = 0;
		int updated_resource_count = 0;
		int deleted_resource_count = 0;
		int property_write_count = 0;

		for (const PatchOperation &operation : operations) {
			property_write_count += operation.property_writes.size();
			switch (operation.kind) {
				case PATCH_CREATE_NODE:
					created_node_count++;
					if (!_is_effectively_deleted(operation.node_state)) {
						created_node_paths[operation.local_id] = _state_path(operation.node_state);
					}
					break;
				case PATCH_UPDATE_NODE:
					updated_node_count++;
					break;
				case PATCH_MOVE_NODE:
					moved_node_count++;
					break;
				case PATCH_DELETE_NODE:
					deleted_node_count++;
					break;
				case PATCH_CREATE_RESOURCE:
					created_resource_count++;
					if (!deleted_resources.has(operation.resource->get_instance_id())) {
						created_resource_ids[operation.local_id] = operation.resource_unique_id;
						created_resource_paths[operation.local_id] = context.scene_path + "::" + operation.resource_unique_id;
					}
					break;
				case PATCH_UPDATE_RESOURCE:
					updated_resource_count++;
					break;
				case PATCH_DELETE_RESOURCE:
					deleted_resource_count++;
					break;
			}
		}

		Dictionary node_changes;
		node_changes["created"] = created_node_count;
		node_changes["updated"] = updated_node_count;
		node_changes["moved"] = moved_node_count;
		node_changes["deleted"] = deleted_node_count;
		Dictionary resource_changes;
		resource_changes["created"] = created_resource_count;
		resource_changes["updated"] = updated_resource_count;
		resource_changes["deleted"] = deleted_resource_count;
		Dictionary change_summary;
		change_summary["nodes"] = node_changes;
		change_summary["resources"] = resource_changes;
		change_summary["property_writes"] = property_write_count;

		Dictionary data;
		data["mode"] = mode == PATCH_MODE_BEST_EFFORT ? "best_effort" : "atomic";
		data["dry_run"] = dry_run;
		data["applied"] = p_applied;
		data["scene_path"] = context.scene_path;
		data["previous_scene_version"] = p_previous_version;
		data["scene_version"] = p_updated_version;
		data["updated_scene_version"] = p_updated_version;
		data["undo_action_name"] = PATCH_UNDO_ACTION;
		data["save_requested"] = p_save_requested;
		data["save_status"] = dry_run ? "dry_run" : (p_save_requested && p_applied ? "pending" : "not_requested");
		data["created_node_paths"] = created_node_paths;
		data["created_embedded_resource_ids"] = created_resource_ids;
		data["created_embedded_resource_paths"] = created_resource_paths;
		data["property_validation_warnings"] = property_warnings;
		data["change_summary"] = change_summary;
		data["operations"] = results;
		data["total_count"] = results.size();
		data["success_count"] = operations.size();
		data["failure_count"] = failure_count;
		data["skipped_count"] = skipped_count;
		return data;
	}

public:
	PatchPlanner(const GodotMCPScenePatch::Context &p_context, PatchMode p_mode, bool p_dry_run) :
			context(p_context), mode(p_mode), dry_run(p_dry_run) {
		int node_count = 0;
		_build_node_states(context.root, nullptr, node_count);
	}

	~PatchPlanner() {
		_clear_plan();
	}

	bool scene_is_bounded() const {
		return node_states.size() < MAX_PATCH_SCENE_NODES;
	}

	const Array &get_property_warnings() const { return property_warnings; }
	const Vector<PatchOperation> &get_operations() const { return operations; }
	const Array &get_results() const { return results; }
	int get_failure_count() const { return failure_count; }
	int get_skipped_count() const { return skipped_count; }

	bool plan(const Array &p_inputs, int &r_error_code, String &r_error_message, Dictionary *r_error_details) {
		HashMap<int, Dictionary> final_failures;
		while (true) {
			results.resize(p_inputs.size());
			for (int i = 0; i < p_inputs.size(); i++) {
				if (p_inputs[i].get_type() != Variant::DICTIONARY) {
					r_error_code = GodotMCPError::INVALID_ARGUMENT;
					r_error_message = "INVALID_ARGUMENT: scene patch operation " + itos(i) + " must be a dictionary.";
					results[i] = _operation_error(i, String(), String(), r_error_code, r_error_message);
					failure_count++;
					if (mode == PATCH_MODE_ATOMIC) {
						if (r_error_details) {
							(*r_error_details)["operation_index"] = i;
							(*r_error_details)["operations"] = results;
						}
						return false;
					}
					r_error_code = 0;
					r_error_message.clear();
					continue;
				}

				const Dictionary input = p_inputs[i];
				const Variant op_variant = input.get("op", Variant());
				const String op_name = (op_variant.get_type() == Variant::STRING || op_variant.get_type() == Variant::STRING_NAME) ? String(op_variant) : String();
				const Variant id_variant = input.get("id", Variant());
				const String operation_id = (id_variant.get_type() == Variant::STRING || id_variant.get_type() == Variant::STRING_NAME) ? String(id_variant) : String();
				if (const Dictionary *failure = final_failures.getptr(i)) {
					results[i] = *failure;
					failure_count++;
					if ((op_name == "create_node" || op_name == "create_resource") && !operation_id.is_empty()) {
						failed_local_ids.insert(operation_id, true);
						declared_local_ids.insert(operation_id);
					}
					continue;
				}

				PatchOperation operation;
				if (!_plan_one(input, i, operation, r_error_code, r_error_message)) {
					const bool dependency_failure = r_error_code == GodotMCPError::CONFLICT && r_error_message.contains("operation did not succeed");
					const String status = dependency_failure ? "skipped" : "failed";
					results[i] = _operation_error(i, operation.local_id.is_empty() ? operation_id : operation.local_id,
							op_name, r_error_code, r_error_message, status);
					if (dependency_failure) {
						skipped_count++;
					} else {
						failure_count++;
					}
					if ((op_name == "create_node" || op_name == "create_resource") && !operation_id.is_empty() &&
							!node_by_local_id.has(operation_id) && !resource_by_local_id.has(operation_id)) {
						failed_local_ids.insert(operation_id, true);
					}
					if (mode == PATCH_MODE_ATOMIC) {
						if (r_error_details) {
							(*r_error_details)["operation_index"] = i;
							(*r_error_details)["operation_id"] = operation.local_id.is_empty() ? operation_id : operation.local_id;
							(*r_error_details)["op"] = op_name;
							(*r_error_details)["operations"] = results;
						}
						return false;
					}
					r_error_code = 0;
					r_error_message.clear();
					continue;
				}

				operations.push_back(operation);
				results[i] = _operation_success(operation, "validated");
			}

			int failed_index = -1;
			if (!_refresh_and_validate_node_paths(r_error_code, r_error_message, failed_index)) {
				if (mode == PATCH_MODE_BEST_EFFORT) {
					const Dictionary input = p_inputs[failed_index];
					const String op_name = input["op"];
					const Variant id_variant = input.get("id", Variant());
					const String id = (id_variant.get_type() == Variant::STRING || id_variant.get_type() == Variant::STRING_NAME) ? String(id_variant) : String();
					final_failures.insert(failed_index, _operation_error(failed_index, id, op_name, r_error_code, r_error_message));
					// Rebuild from the untouched live scene. This rolls back all shadow,
					// hierarchy, group, and resource effects of the failed operation and
					// lets request-local dependents report skipped without losing valid
					// independent operations. Each pass excludes another operation.
					_clear_plan();
					int node_count = 0;
					_build_node_states(context.root, nullptr, node_count);
					r_error_code = 0;
					r_error_message.clear();
					continue;
				}
				if (r_error_details) {
					(*r_error_details)["stage"] = "final_node_path_validation";
					(*r_error_details)["operation_index"] = failed_index;
					(*r_error_details)["operations"] = results;
				}
				return false;
			}
			return true;
		}
	}

	Dictionary finish(const String &p_previous_version, bool p_save_requested,
			int &r_error_code, String &r_error_message) {
		if (dry_run || operations.is_empty()) {
			_refresh_summaries_and_results("validated");
			Dictionary data = _build_data(p_previous_version, p_previous_version, false, p_save_requested);
			if (operations.is_empty() && !dry_run) {
				data["save_status"] = p_save_requested ? "not_saved_no_changes" : "not_requested";
			}
			return data;
		}

		if (!context.undo_redo) {
			r_error_code = GodotMCPError::INTERNAL;
			r_error_message = "INTERNAL: the active scene has no undo history.";
			return Dictionary();
		}

		// These resources are not part of the live scene until the action assigns
		// them. Giving them stable scene-local paths here ensures PackedScene saves
		// preserve cross-references and return IDs that survive reload.
		for (PatchOperation &operation : operations) {
			if (operation.kind != PATCH_CREATE_RESOURCE || operation.resource.is_null()) {
				continue;
			}
			operation.resource->set_scene_unique_id(operation.resource_unique_id);
			operation.resource->set_path(context.scene_path + "::" + operation.resource_unique_id);
		}

		if (context.undo_redo_manager) {
			context.undo_redo_manager->create_action_for_history(PATCH_UNDO_ACTION, context.history_id);
		} else {
			context.undo_redo->create_action(PATCH_UNDO_ACTION);
		}
		for (const PatchOperation &operation : operations) {
			_add_do_ops(context.undo_redo, operation);
		}
		for (int i = operations.size() - 1; i >= 0; i--) {
			_add_undo_ops(context.undo_redo, operations[i]);
		}
		if (context.undo_redo_manager) {
			context.undo_redo_manager->commit_action();
		} else {
			context.undo_redo->commit_action();
		}
		committed = true;

		_refresh_summaries_and_results("applied");
		const String updated_version = GodotMCPScenePatch::compute_scene_version(
				context.root, context.history_id, context.undo_redo->get_version());
		return _build_data(p_previous_version, updated_version, true, p_save_requested);
	}
};

} // namespace

String GodotMCPScenePatch::compute_scene_version(Node *p_root, int p_history_id, uint64_t p_undo_version) {
	if (!p_root) {
		return String();
	}
	auto compute_fingerprint = [&]() {
		uint64_t value = UINT64_C(1469598103934665603);
		SceneVersionHashContext context;
		context.root = p_root;
		int node_count = 0;
		_hash_node(value, p_root, context, node_count);
		return value;
	};

	// Some engine resources populate non-serialized lazy caches the first time a
	// storage property is queried. Never expose that warm-up fingerprint: take a
	// second canonical pass, and one convergence pass when initialization changed
	// the first result. Meaningful serialized values are identical across passes.
	const uint64_t first_fingerprint = compute_fingerprint();
	uint64_t fingerprint = compute_fingerprint();
	if (fingerprint != first_fingerprint) {
		fingerprint = compute_fingerprint();
	}
	const String fingerprint_hex = String::num_uint64(fingerprint, 16);
	return "scene-v2:" + itos(p_history_id) + ":" + uitos(p_undo_version) + ":" + String("0").repeat(MAX(0, 16 - fingerprint_hex.length())) + fingerprint_hex;
}

Dictionary GodotMCPScenePatch::apply(const Dictionary &p_params, const Context &p_context, Array &r_warnings,
		int &r_error_code, String &r_error_message, Dictionary *r_error_details) const {
	r_error_code = 0;
	r_error_message.clear();
	if (r_error_details) {
		r_error_details->clear();
	}

	if (!p_context.root) {
		r_error_code = GodotMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: no active edited scene is available.";
		return Dictionary();
	}

	const Variant path_variant = p_params.get("expected_scene_path", Variant());
	if ((path_variant.get_type() != Variant::STRING && path_variant.get_type() != Variant::STRING_NAME) || String(path_variant).is_empty()) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: expected_scene_path is required and must be a non-empty string.";
		return Dictionary();
	}
	const String expected_path = String(path_variant);
	if (!expected_path.begins_with("res://")) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: expected_scene_path must start with res://.";
		return Dictionary();
	}
	if (p_context.scene_path != expected_path) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: active edited scene does not match expected_scene_path.";
		if (r_error_details) {
			(*r_error_details)["conflict_kind"] = "path_conflict";
			(*r_error_details)["expected_scene_path"] = expected_path;
			(*r_error_details)["active_scene_path"] = p_context.scene_path;
		}
		return Dictionary();
	}

	const Variant version_variant = p_params.get("expected_scene_version", Variant());
	if ((version_variant.get_type() != Variant::STRING && version_variant.get_type() != Variant::STRING_NAME) || String(version_variant).is_empty()) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: expected_scene_version is required and must be a non-empty string.";
		return Dictionary();
	}
	const String expected_version = String(version_variant);
	if (expected_version != p_context.scene_version) {
		r_error_code = GodotMCPError::CONFLICT;
		r_error_message = "CONFLICT: active edited scene version no longer matches expected_scene_version.";
		if (r_error_details) {
			const PackedStringArray expected_parts = expected_version.split(":");
			const PackedStringArray active_parts = p_context.scene_version.split(":");
			const bool history_changed = expected_parts.size() >= 4 && active_parts.size() >= 4 &&
					(expected_parts[1] != active_parts[1] || expected_parts[2] != active_parts[2]);
			(*r_error_details)["conflict_kind"] = history_changed ? "history_conflict" : "content_version_conflict";
			(*r_error_details)["expected_scene_version"] = expected_version;
			(*r_error_details)["active_scene_version"] = p_context.scene_version;
			(*r_error_details)["active_scene_path"] = p_context.scene_path;
		}
		return Dictionary();
	}

	const Variant operations_variant = p_params.get("operations", Variant());
	if (operations_variant.get_type() != Variant::ARRAY) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: operations is required and must be an array.";
		return Dictionary();
	}
	const Array operation_inputs = operations_variant;
	if (operation_inputs.is_empty() || operation_inputs.size() > MAX_OPERATIONS) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = operation_inputs.is_empty() ? "INVALID_ARGUMENT: operations must not be empty." : "INVALID_ARGUMENT: operations exceeds the maximum of " + itos(MAX_OPERATIONS) + ".";
		return Dictionary();
	}

	const Variant dry_run_variant = p_params.get("dry_run", false);
	const Variant save_variant = p_params.get("save", false);
	if (dry_run_variant.get_type() != Variant::BOOL || save_variant.get_type() != Variant::BOOL) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: dry_run and save must be booleans.";
		return Dictionary();
	}
	const bool dry_run = dry_run_variant;
	const bool save_requested = save_variant;
	if (dry_run && save_requested) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: dry_run and save cannot both be true.";
		return Dictionary();
	}

	PatchMode mode = PATCH_MODE_ATOMIC;
	if (!_parse_mode(p_params, mode, r_error_message)) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		return Dictionary();
	}
	PatchPlanner planner(p_context, mode, dry_run);
	if (!planner.scene_is_bounded()) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: active scene exceeds the patch validation limit of " + itos(MAX_PATCH_SCENE_NODES - 1) + " nodes.";
		return Dictionary();
	}
	if (!planner.plan(operation_inputs, r_error_code, r_error_message, r_error_details)) {
		return Dictionary();
	}

	Dictionary data = planner.finish(expected_version, save_requested, r_error_code, r_error_message);
	if (r_error_code != 0) {
		return Dictionary();
	}
	if (mode == PATCH_MODE_BEST_EFFORT && (planner.get_failure_count() > 0 || planner.get_skipped_count() > 0)) {
		r_warnings.push_back("Best-effort scene patch completed with failed or skipped operations; inspect operations for details.");
	}
	if (!dry_run && !save_requested && bool(data.get("applied", false))) {
		r_warnings.push_back("Scene patch applied to editor state but was not saved; pass save:true to persist it.");
	}
	return data;
}
