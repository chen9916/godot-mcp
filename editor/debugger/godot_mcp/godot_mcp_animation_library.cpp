/**************************************************************************/
/*  godot_mcp_animation_library.cpp                                       */
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

#include "godot_mcp_animation_library.h"

#include "core/io/resource_loader.h"
#include "core/object/undo_redo.h"
#include "core/templates/hash_set.h"
#include "core/variant/callable.h"
#include "editor/debugger/godot_mcp/godot_mcp_error.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/animation/animation_mixer.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/audio/audio_stream.h"

namespace {

constexpr int MAX_SCENE_NODES = 10000;
constexpr int MAX_TRACKS = 2000;
constexpr int MAX_GRAPH_ITEMS = 100000;
constexpr int MAX_VALUE_ITEMS = 10000;
constexpr int MAX_VALUE_DEPTH = 16;
constexpr int MAX_STRING_LENGTH = 16384;
constexpr double MAX_TIME = 1000000.0;
constexpr double MAX_COMPONENT = 1.0e30;
constexpr const char *UNDO_ACTION = "Godot MCP: Apply Animation Patch";

static bool _fail(String &r_error, const String &p_error) {
	r_error = p_error;
	return false;
}

static bool _string(const Variant &p_value, String &r_value, bool p_allow_empty = false) {
	if (p_value.get_type() != Variant::STRING || String(p_value).length() > MAX_STRING_LENGTH) {
		return false;
	}
	r_value = p_value;
	return p_allow_empty || !r_value.is_empty();
}

static bool _number(const Variant &p_value, double &r_value, double p_min = -MAX_COMPONENT, double p_max = MAX_COMPONENT) {
	if (p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) {
		return false;
	}
	r_value = p_value;
	return Math::is_finite(r_value) && r_value >= p_min && r_value <= p_max;
}

// JSON parsers produce FLOAT even for integral numbers. Reject fractional,
// non-finite and out-of-range values before narrowing to an engine index.
static bool _integer(const Variant &p_value, int &r_value, int p_min, int p_max) {
	double value;
	if (!_number(p_value, value, p_min, p_max) || Math::floor(value) != value) {
		return false;
	}
	r_value = int(value);
	return true;
}

static bool _name(const Variant &p_value, String &r_name, bool p_library) {
	if (!_string(p_value, r_name, p_library) || r_name.length() > 256 || r_name.strip_edges() != r_name) {
		return false;
	}
	for (int i = 0; i < r_name.length(); i++) {
		if (r_name[i] < 32 || r_name[i] == 127) {
			return false;
		}
	}
	return p_library ? AnimationLibrary::is_valid_library_name(r_name) : AnimationLibrary::is_valid_animation_name(r_name);
}

static bool _clip_name(const Variant &p_value, String &r_library, String &r_clip) {
	String qualified;
	if (!_string(p_value, qualified)) {
		return false;
	}
	const int slash = qualified.find_char('/');
	if (slash < 0) {
		r_library = String();
		r_clip = qualified;
	} else {
		r_library = qualified.substr(0, slash);
		r_clip = qualified.substr(slash + 1);
		// The default library is addressed as "clip", not "/clip".
		if (r_library.is_empty()) {
			return false;
		}
	}
	return _name(r_library, r_library, true) && _name(r_clip, r_clip, false);
}

static String _qualified(const String &p_library, const String &p_clip) {
	return p_library.is_empty() ? p_clip : p_library + "/" + p_clip;
}

static bool _resource_path(const Variant &p_value, String &r_path) {
	if (!_string(p_value, r_path) || !r_path.begins_with("res://") || r_path.contains_char('\\')) {
		return false;
	}
	const int separator = r_path.find("::");
	const String file = separator < 0 ? r_path : r_path.substr(0, separator);
	if (file != file.simplify_path() || file.contains("/../")) {
		return false;
	}
	if (separator >= 0) {
		const String id = r_path.substr(separator + 2);
		if (id.is_empty() || id.contains_char('/') || id.contains_char(':')) {
			return false;
		}
	}
	for (int i = 0; i < r_path.length(); i++) {
		if (r_path[i] < 32 || r_path[i] == 127) {
			return false;
		}
	}
	return true;
}

static bool _load_resource(const Variant &p_path, Ref<Resource> &r_resource, String &r_error) {
	String path;
	if (!_resource_path(p_path, path)) {
		return _fail(r_error, "resource_path must be a normalized project-local res:// path, optionally followed by ::subresource_id.");
	}
	// Imported and embedded resources are cached under file::id paths. Keep
	// their containing resource alive while resolving a previously uncached ID.
	Ref<Resource> container;
	if (path.contains("::") && !ResourceLoader::exists(path)) {
		const String file = path.get_slice("::", 0);
		if (ResourceLoader::exists(file)) {
			container = ResourceLoader::load(file);
		}
	}
	if (!ResourceLoader::exists(path)) {
		return _fail(r_error, "Resource does not exist: " + path);
	}
	r_resource = ResourceLoader::load(path);
	return r_resource.is_valid() || _fail(r_error, "Resource could not be loaded: " + path);
}

static bool _guard(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context, bool p_write,
		int &r_code, String &r_error) {
	r_code = 0;
	r_error.clear();
	if (!p_context.root) {
		r_code = GodotMCPError::NOT_FOUND;
		return _fail(r_error, "NOT_FOUND: no edited scene.");
	}
	if (p_write || p_params.has("expected_scene_path")) {
		String path;
		if (!_string(p_params.get("expected_scene_path", Variant()), path)) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			return _fail(r_error, "INVALID_ARGUMENT: expected_scene_path is required and must be a non-empty string.");
		}
		if (path != p_context.scene_path) {
			r_code = GodotMCPError::CONFLICT;
			return _fail(r_error, "CONFLICT: expected_scene_path does not match the edited scene.");
		}
	}
	if (p_write) {
		String version;
		if (!_string(p_params.get("expected_scene_version", Variant()), version)) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			return _fail(r_error, "INVALID_ARGUMENT: expected_scene_version is required.");
		}
		if (version != p_context.scene_version) {
			r_code = GodotMCPError::CONFLICT;
			return _fail(r_error, "CONFLICT: expected_scene_version is stale.");
		}
	}
	return true;
}

static AnimationMixer *_target(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
		int &r_code, String &r_error) {
	String path;
	if (!_string(p_params.get("node_path", Variant()), path) || path.length() > 4096) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_error = "INVALID_ARGUMENT: node_path is required and must be a non-empty scene-relative string.";
		return nullptr;
	}
	const NodePath node_path(path);
	if (node_path.is_absolute() || node_path.get_subname_count() != 0) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_error = "INVALID_ARGUMENT: node_path must be relative and must not contain property subnames.";
		return nullptr;
	}
	for (int i = 0; i < node_path.get_name_count(); i++) {
		if (node_path.get_name(i) == SNAME("..")) {
			r_code = GodotMCPError::INVALID_ARGUMENT;
			r_error = "INVALID_ARGUMENT: node_path must not traverse outside the scene root.";
			return nullptr;
		}
	}
	Node *node = p_context.root->get_node_or_null(node_path);
	if (!node || (node != p_context.root && !p_context.root->is_ancestor_of(node))) {
		r_code = GodotMCPError::NOT_FOUND;
		r_error = "NOT_FOUND: node_path is not in the edited scene.";
		return nullptr;
	}
	AnimationMixer *mixer = Object::cast_to<AnimationMixer>(node);
	if (!mixer) {
		r_code = GodotMCPError::INVALID_ARGUMENT;
		r_error = "INVALID_ARGUMENT: node_path must target an AnimationMixer (including AnimationPlayer or AnimationTree).";
	}
	return mixer;
}

static Dictionary _resource_info(const Ref<Resource> &p_resource) {
	Dictionary data;
	const String path = p_resource->get_path();
	const String name = p_resource->get_name();
	data["resource_path"] = path.substr(0, 4096);
	data["resource_name"] = name.substr(0, 1024);
	data["resource_type"] = p_resource->get_class();
	data["external"] = !path.is_empty() && !path.contains("::");
	data["local_to_scene"] = p_resource->is_local_to_scene();
	data["metadata_truncated"] = path.length() > 4096 || name.length() > 1024;
	return data;
}

static const char *const TRACK_NAMES[] = { "value", "position_3d", "rotation_3d", "scale_3d", "blend_shape", "method", "bezier", "audio", "animation" };

static Dictionary _track_info(const Ref<Animation> &p_animation, int p_track) {
	Dictionary data;
	const Animation::TrackType type = p_animation->track_get_type(p_track);
	const String path = String(p_animation->track_get_path(p_track));
	data["track"] = p_track;
	data["type"] = TRACK_NAMES[type];
	data["path"] = path.substr(0, 4096);
	data["metadata_truncated"] = path.length() > 4096;
	data["enabled"] = p_animation->track_is_enabled(p_track);
	data["imported"] = p_animation->track_is_imported(p_track);
	data["compressed"] = p_animation->track_is_compressed(p_track);
	data["interpolation_type"] = int(p_animation->track_get_interpolation_type(p_track));
	data["interpolation_loop_wrap"] = p_animation->track_get_interpolation_loop_wrap(p_track);
	data["key_count"] = p_animation->track_get_key_count(p_track);
	if (type == Animation::TYPE_VALUE) {
		data["value_update_mode"] = int(p_animation->value_track_get_update_mode(p_track));
	}
	if (type == Animation::TYPE_AUDIO) {
		data["audio_use_blend"] = p_animation->audio_track_is_use_blend(p_track);
	}
	return data;
}

static Dictionary _animation_info(const Ref<Animation> &p_animation, const String &p_library, const String &p_clip) {
	Dictionary data = _resource_info(p_animation);
	data["animation"] = _qualified(p_library.substr(0, 256), p_clip.substr(0, 256));
	data["name"] = p_clip.substr(0, 256);
	data["library"] = p_library.substr(0, 256);
	data["metadata_truncated"] = bool(data["metadata_truncated"]) || p_library.length() > 256 || p_clip.length() > 256;
	data["length"] = p_animation->get_length();
	data["duration"] = p_animation->get_length();
	data["loop_mode"] = int(p_animation->get_loop_mode());
	data["step"] = p_animation->get_step();
	data["track_count"] = p_animation->get_track_count();
	return data;
}

static Dictionary _typed(const String &p_type, const Variant &p_value) {
	Dictionary data;
	data["__type"] = p_type;
	data["value"] = p_value;
	return data;
}

static Array _components(std::initializer_list<double> p_values) {
	Array data;
	for (double value : p_values) {
		data.push_back(value);
	}
	return data;
}

// All exported values remain JSON-compatible. A global item budget and depth
// guard also terminate cyclic Variant containers; no arbitrary Object is sent.
static Variant _encode(const Variant &p_value, int &r_items, bool &r_truncated, int p_depth = 0) {
	if (p_depth > MAX_VALUE_DEPTH || r_items <= 0) {
		r_truncated = true;
		return _typed("Truncated", Variant());
	}
	r_items--;
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
			return p_value;
		case Variant::INT:
			return _typed("int", itos(int64_t(p_value)));
		case Variant::FLOAT: {
			const double value = p_value;
			return Math::is_finite(value) ? p_value : Variant(_typed("NonFinite", String(p_value)));
		}
		case Variant::STRING: {
			const String value = p_value;
			const int length = MIN(MAX_STRING_LENGTH, r_items * 64);
			r_items -= (MIN(value.length(), length) + 63) / 64;
			if (value.length() > length) {
				r_truncated = true;
				return _typed("TruncatedString", value.substr(0, length));
			}
			return value;
		}
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return _typed(Variant::get_type_name(p_value.get_type()), _encode(String(p_value), r_items, r_truncated, p_depth + 1));
		case Variant::OBJECT: {
			Object *object = p_value.get_validated_object();
			if (!object) {
				return Variant();
			}
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				Dictionary data;
				data["resource_path"] = _encode(resource->get_path(), r_items, r_truncated, p_depth + 1);
				data["resource_type"] = resource->get_class();
				data["embedded"] = resource->get_path().is_empty() || resource->get_path().contains("::");
				return data;
			}
			r_truncated = true;
			return _typed("Unsupported", object->get_class());
		}
		case Variant::ARRAY: {
			const Array input = p_value;
			Array output;
			for (int i = 0; i < input.size() && r_items > 0; i++) {
				output.push_back(_encode(input[i], r_items, r_truncated, p_depth + 1));
			}
			r_truncated |= output.size() != input.size();
			return output;
		}
		case Variant::DICTIONARY: {
			const Dictionary input = p_value;
			// Entry arrays preserve non-string keys and literal reserved names.
			Array entries;
			for (const KeyValue<Variant, Variant> &E : input) {
				if (r_items <= 0) {
					r_truncated = true;
					break;
				}
				Array entry;
				entry.push_back(_encode(E.key, r_items, r_truncated, p_depth + 1));
				entry.push_back(_encode(E.value, r_items, r_truncated, p_depth + 1));
				entries.push_back(entry);
			}
			return _typed("Dictionary", entries);
		}
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			return _typed("Vector2", _encode(_components({ v.x, v.y }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			return _typed("Vector2i", _encode(_components({ double(v.x), double(v.y) }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			return _typed("Vector3", _encode(_components({ v.x, v.y, v.z }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::VECTOR3I: {
			const Vector3i v = p_value;
			return _typed("Vector3i", _encode(_components({ double(v.x), double(v.y), double(v.z) }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::VECTOR4: {
			const Vector4 v = p_value;
			return _typed("Vector4", _encode(_components({ v.x, v.y, v.z, v.w }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::VECTOR4I: {
			const Vector4i v = p_value;
			return _typed("Vector4i", _encode(_components({ double(v.x), double(v.y), double(v.z), double(v.w) }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::QUATERNION: {
			const Quaternion v = p_value;
			return _typed("Quaternion", _encode(_components({ v.x, v.y, v.z, v.w }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::COLOR: {
			const Color v = p_value;
			return _typed("Color", _encode(_components({ v.r, v.g, v.b, v.a }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::RECT2: {
			const Rect2 v = p_value;
			return _typed("Rect2", _encode(_components({ v.position.x, v.position.y, v.size.x, v.size.y }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::RECT2I: {
			const Rect2i v = p_value;
			return _typed("Rect2i", _encode(_components({ double(v.position.x), double(v.position.y), double(v.size.x), double(v.size.y) }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::PLANE: {
			const Plane v = p_value;
			return _typed("Plane", _encode(_components({ v.normal.x, v.normal.y, v.normal.z, v.d }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::AABB: {
			const AABB v = p_value;
			return _typed("AABB", _encode(_components({ v.position.x, v.position.y, v.position.z, v.size.x, v.size.y, v.size.z }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::BASIS: {
			const Basis v = p_value;
			const Vector3 x = v.get_column(0);
			const Vector3 y = v.get_column(1);
			const Vector3 z = v.get_column(2);
			return _typed("Basis", _encode(_components({ x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::TRANSFORM2D: {
			const Transform2D v = p_value;
			return _typed("Transform2D", _encode(_components({ v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y }), r_items, r_truncated, p_depth + 1));
		}
		case Variant::TRANSFORM3D: {
			const Transform3D v = p_value;
			Array values;
			values.push_back(_encode(v.basis, r_items, r_truncated, p_depth + 1));
			values.push_back(_encode(v.origin, r_items, r_truncated, p_depth + 1));
			return _typed("Transform3D", values);
		}
		case Variant::PROJECTION: {
			const Projection v = p_value;
			Array values;
			for (int i = 0; i < 4; i++) {
				values.push_back(_encode(v[i], r_items, r_truncated, p_depth + 1));
			}
			return _typed("Projection", values);
		}
		default: {
			if (p_value.is_array()) {
				const int64_t size = p_value.get_indexed_size();
				Array values;
				for (int64_t i = 0; i < size && r_items > 0; i++) {
					bool valid = false;
					bool out_of_bounds = false;
					const Variant value = p_value.get_indexed(i, valid, out_of_bounds);
					if (valid) {
						// Byte/int32 buffers already convey the native integer type;
						// their numeric elements are exactly representable in JSON.
						const Variant encoded_value = p_value.get_type() == Variant::PACKED_BYTE_ARRAY || p_value.get_type() == Variant::PACKED_INT32_ARRAY ? Variant(double(value)) : value;
						values.push_back(_encode(encoded_value, r_items, r_truncated, p_depth + 1));
					}
				}
				r_truncated |= values.size() != size;
				return _typed(Variant::get_type_name(p_value.get_type()), values);
			}
			r_truncated = true;
			return _typed("Unsupported", Variant::get_type_name(p_value.get_type()));
		}
	}
}

static bool _reals(const Variant &p_value, int p_count, double *r_values, bool p_integer = false) {
	if (p_value.get_type() != Variant::ARRAY || Array(p_value).size() != p_count) {
		return false;
	}
	const Array values = p_value;
	for (int i = 0; i < p_count; i++) {
		if (!_number(values[i], r_values[i]) || (p_integer && (Math::floor(r_values[i]) != r_values[i] || r_values[i] < INT32_MIN || r_values[i] > INT32_MAX))) {
			return false;
		}
	}
	return true;
}

static bool _decode_typed(const String &p_type, const Variant &p_value, Variant &r_value, String &r_error) {
	double v[16];
	if (p_type == "int") {
		if (p_value.get_type() != Variant::STRING) {
			return _fail(r_error, "int wrappers require a signed decimal string.");
		}
		const String text = p_value;
		const bool negative = text.begins_with("-");
		const int start = negative ? 1 : 0;
		if (text.length() <= start || text.length() > 20 || (text.length() - start > 1 && text[start] == '0') || text == "-0") {
			return _fail(r_error, "int wrappers require a canonical signed decimal int64 string.");
		}
		const uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
		uint64_t magnitude = 0;
		for (int i = start; i < text.length(); i++) {
			if (text[i] < '0' || text[i] > '9' || magnitude > (limit - uint64_t(text[i] - '0')) / 10) {
				return _fail(r_error, "int wrapper is malformed or outside the signed int64 range.");
			}
			magnitude = magnitude * 10 + uint64_t(text[i] - '0');
		}
		r_value = negative ? (magnitude == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -int64_t(magnitude)) : int64_t(magnitude);
		return true;
	}
	if ((p_type == "StringName" || p_type == "NodePath") && p_value.get_type() == Variant::STRING) {
		r_value = p_type == "StringName" ? Variant(StringName(String(p_value))) : Variant(NodePath(String(p_value)));
		return true;
	}
	if (p_type == "Vector2" && _reals(p_value, 2, v)) {
		r_value = Vector2(v[0], v[1]);
	} else if (p_type == "Vector2i" && _reals(p_value, 2, v, true)) {
		r_value = Vector2i(v[0], v[1]);
	} else if (p_type == "Vector3" && _reals(p_value, 3, v)) {
		r_value = Vector3(v[0], v[1], v[2]);
	} else if (p_type == "Vector3i" && _reals(p_value, 3, v, true)) {
		r_value = Vector3i(v[0], v[1], v[2]);
	} else if (p_type == "Vector4" && _reals(p_value, 4, v)) {
		r_value = Vector4(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Vector4i" && _reals(p_value, 4, v, true)) {
		r_value = Vector4i(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Quaternion" && _reals(p_value, 4, v)) {
		r_value = Quaternion(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Color" && _reals(p_value, 4, v)) {
		r_value = Color(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Color" && _reals(p_value, 3, v)) {
		r_value = Color(v[0], v[1], v[2]);
	} else if (p_type == "Color" && p_value.get_type() == Variant::STRING && Color::html_is_valid(String(p_value))) {
		r_value = Color::html(String(p_value));
	} else if (p_type == "Rect2" && _reals(p_value, 4, v)) {
		r_value = Rect2(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Rect2i" && _reals(p_value, 4, v, true)) {
		r_value = Rect2i(v[0], v[1], v[2], v[3]);
	} else if (p_type == "Plane" && _reals(p_value, 4, v)) {
		r_value = Plane(Vector3(v[0], v[1], v[2]), v[3]);
	} else if (p_type == "AABB" && _reals(p_value, 6, v)) {
		r_value = AABB(Vector3(v[0], v[1], v[2]), Vector3(v[3], v[4], v[5]));
	} else if (p_type == "Basis" && _reals(p_value, 9, v)) {
		r_value = Basis(Vector3(v[0], v[1], v[2]), Vector3(v[3], v[4], v[5]), Vector3(v[6], v[7], v[8]));
	} else if (p_type == "Transform2D" && _reals(p_value, 6, v)) {
		r_value = Transform2D(Vector2(v[0], v[1]), Vector2(v[2], v[3]), Vector2(v[4], v[5]));
	} else if (p_type == "Transform3D" && p_value.get_type() == Variant::ARRAY && Array(p_value).size() == 2 &&
			Array(p_value)[0].get_type() == Variant::BASIS && Array(p_value)[1].get_type() == Variant::VECTOR3) {
		r_value = Transform3D(Array(p_value)[0], Array(p_value)[1]);
	} else if (p_type == "Transform3D" && _reals(p_value, 12, v)) {
		r_value = Transform3D(Basis(Vector3(v[0], v[1], v[2]), Vector3(v[3], v[4], v[5]), Vector3(v[6], v[7], v[8])), Vector3(v[9], v[10], v[11]));
	} else if (p_type == "Projection" && p_value.get_type() == Variant::ARRAY && Array(p_value).size() == 4 &&
			Array(p_value)[0].get_type() == Variant::VECTOR4 && Array(p_value)[1].get_type() == Variant::VECTOR4 &&
			Array(p_value)[2].get_type() == Variant::VECTOR4 && Array(p_value)[3].get_type() == Variant::VECTOR4) {
		r_value = Projection(Array(p_value)[0], Array(p_value)[1], Array(p_value)[2], Array(p_value)[3]);
	} else if (p_type == "Projection" && _reals(p_value, 16, v)) {
		r_value = Projection(Vector4(v[0], v[1], v[2], v[3]), Vector4(v[4], v[5], v[6], v[7]), Vector4(v[8], v[9], v[10], v[11]), Vector4(v[12], v[13], v[14], v[15]));
	} else if (p_type == "Array" && p_value.get_type() == Variant::ARRAY) {
		r_value = p_value;
	} else if (p_type == "Dictionary" && p_value.get_type() == Variant::ARRAY) {
		Dictionary dictionary;
		for (const Variant &entry : Array(p_value)) {
			if (entry.get_type() != Variant::ARRAY || Array(entry).size() != 2) {
				return _fail(r_error, "Dictionary typed values require [key, value] entries.");
			}
			dictionary[Array(entry)[0]] = Array(entry)[1];
		}
		r_value = dictionary;
	} else if (p_type.begins_with("Packed") && p_value.get_type() == Variant::ARRAY) {
		Variant::Type type = Variant::NIL;
		for (int i = Variant::PACKED_BYTE_ARRAY; i < Variant::VARIANT_MAX; i++) {
			if (p_type == Variant::get_type_name(Variant::Type(i))) {
				type = Variant::Type(i);
				break;
			}
		}
		if (type == Variant::NIL) {
			return _fail(r_error, "Unsupported packed Variant type: " + p_type);
		}
		const Array values = p_value;
		for (const Variant &value : values) {
			int integer;
			double number;
			bool valid = false;
			switch (type) {
				case Variant::PACKED_BYTE_ARRAY:
					valid = _integer(value, integer, 0, 255);
					break;
				case Variant::PACKED_INT32_ARRAY:
					valid = _integer(value, integer, INT32_MIN, INT32_MAX);
					break;
				case Variant::PACKED_INT64_ARRAY:
					valid = value.get_type() == Variant::INT || (_number(value, number, -9007199254740991.0, 9007199254740991.0) && Math::floor(number) == number);
					break;
				case Variant::PACKED_FLOAT32_ARRAY:
				case Variant::PACKED_FLOAT64_ARRAY:
					valid = _number(value, number);
					break;
				case Variant::PACKED_STRING_ARRAY:
					valid = value.get_type() == Variant::STRING;
					break;
				case Variant::PACKED_VECTOR2_ARRAY:
					valid = value.get_type() == Variant::VECTOR2;
					break;
				case Variant::PACKED_VECTOR3_ARRAY:
					valid = value.get_type() == Variant::VECTOR3;
					break;
				case Variant::PACKED_VECTOR4_ARRAY:
					valid = value.get_type() == Variant::VECTOR4;
					break;
				case Variant::PACKED_COLOR_ARRAY:
					valid = value.get_type() == Variant::COLOR;
					break;
				default:
					break;
			}
			if (!valid) {
				return _fail(r_error, "Invalid element for " + p_type);
			}
		}
		const Variant *arguments[] = { &p_value };
		Callable::CallError error;
		Variant::construct(type, r_value, arguments, 1, error);
		return error.error == Callable::CallError::CALL_OK || _fail(r_error, "Cannot construct " + p_type);
	} else {
		return _fail(r_error, "Malformed or unsupported typed Variant: " + p_type);
	}
	return true;
}

static bool _decode(const Variant &p_encoded, Variant &r_value, int &r_items, String &r_error, int p_depth = 0) {
	if (p_depth > MAX_VALUE_DEPTH || --r_items < 0) {
		return _fail(r_error, "Value exceeds the 16-level / 10000-item Variant bounds.");
	}
	switch (p_encoded.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
			r_value = p_encoded;
			return true;
		case Variant::FLOAT: {
			double value;
			if (!_number(p_encoded, value)) {
				return _fail(r_error, "Numeric key values must be finite and within +/-1e30.");
			}
			r_value = p_encoded;
			return true;
		}
		case Variant::STRING: {
			String value;
			if (!_string(p_encoded, value, true) || (value.length() + 63) / 64 > r_items) {
				return _fail(r_error, "Value string exceeds 16384 characters or the Variant item budget.");
			}
			r_items -= (value.length() + 63) / 64;
			r_value = value;
			return true;
		}
		case Variant::ARRAY: {
			const Array input = p_encoded;
			if (input.size() > r_items) {
				return _fail(r_error, "Array exceeds the Variant item budget.");
			}
			Array output;
			for (const Variant &value : input) {
				Variant decoded;
				if (!_decode(value, decoded, r_items, r_error, p_depth + 1)) {
					return false;
				}
				output.push_back(decoded);
			}
			r_value = output;
			return true;
		}
		case Variant::DICTIONARY: {
			const Dictionary input = p_encoded;
			if (input.has("__type")) {
				String type;
				if (!_string(input["__type"], type) || !input.has("value")) {
					return _fail(r_error, "Typed Variants require __type and value.");
				}
				const Variant payload = input["value"];
				// Wrappers and Dictionary entry-pair arrays are transport, not
				// semantic Variant containers. Match _encode's depth/item accounting.
				if (type == "int") {
					return _decode_typed(type, payload, r_value, r_error);
				}
				if (type == "Dictionary") {
					if (payload.get_type() != Variant::ARRAY || Array(payload).size() > r_items / 2) {
						return _fail(r_error, "Dictionary wrappers require bounded [key, value] entries.");
					}
					Dictionary output;
					for (const Variant &entry : Array(payload)) {
						if (entry.get_type() != Variant::ARRAY || Array(entry).size() != 2) {
							return _fail(r_error, "Dictionary wrappers require [key, value] entries.");
						}
						const Array pair = entry;
						Variant key;
						Variant value;
						if (!_decode(pair[0], key, r_items, r_error, p_depth + 1) || !_decode(pair[1], value, r_items, r_error, p_depth + 1)) {
							return false;
						}
						output[key] = value;
					}
					r_value = output;
					return true;
				}
				if (type == "Array" || type.begins_with("Packed") || type == "Transform3D" || type == "Projection") {
					if (payload.get_type() != Variant::ARRAY || Array(payload).size() > r_items) {
						return _fail(r_error, "Array wrappers require a bounded value array.");
					}
					Array values;
					for (const Variant &element : Array(payload)) {
						Variant value;
						if (!_decode(element, value, r_items, r_error, p_depth + 1)) {
							return false;
						}
						values.push_back(value);
					}
					return _decode_typed(type, values, r_value, r_error);
				}
				Variant value;
				return _decode(payload, value, r_items, r_error, p_depth + 1) && _decode_typed(type, value, r_value, r_error);
			}
			if (input.has("resource_path") || input.has("external_resource")) {
				Ref<Resource> resource;
				if (!_load_resource(input.get("resource_path", input.get("external_resource", Variant())), resource, r_error)) {
					return false;
				}
				r_value = resource;
				return true;
			}
			if (input.size() > r_items) {
				return _fail(r_error, "Dictionary exceeds the Variant item budget.");
			}
			Dictionary output;
			for (const KeyValue<Variant, Variant> &E : input) {
				Variant key;
				Variant value;
				if (!_decode(E.key, key, r_items, r_error, p_depth + 1) || !_decode(E.value, value, r_items, r_error, p_depth + 1)) {
					return false;
				}
				output[key] = value;
			}
			r_value = output;
			return true;
		}
		default:
			return _fail(r_error, "Key values must be JSON primitives, containers, resource references or typed Variant wrappers.");
	}
}

// Bound the complete graph BEFORE asking Resource to deep-duplicate it. In
// particular, nested key resources and cyclic containers cannot bypass limits.
struct GraphBudget {
	int remaining = MAX_GRAPH_ITEMS;
	int remaining_keys = GodotMCPAnimationLibrary::MAX_DECODED_KEYS;
	uint64_t buffer_bytes = 64 * 1024 * 1024;
	HashSet<ObjectID> resources;
	HashSet<const void *> containers;

	bool check(const Variant &p_value, String &r_error, int p_depth = 0) {
		if (--remaining < 0 || p_depth > MAX_VALUE_DEPTH) {
			return _fail(r_error, "Animation graph exceeds 100000 items or 16 nested levels.");
		}
		if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME || p_value.get_type() == Variant::NODE_PATH) {
			const int64_t cost = (int64_t(String(p_value).length()) + 63) / 64;
			if (cost > remaining) {
				return _fail(r_error, "Animation graph strings exceed the item budget.");
			}
			remaining -= cost;
		}
		if (p_value.get_type() == Variant::OBJECT) {
			bool previously_freed = false;
			Object *object = p_value.get_validated_object_with_check(previously_freed);
			if (!object) {
				return !previously_freed || _fail(r_error, "Animation graphs containing freed Object references are not editable.");
			}
			Resource *resource = Object::cast_to<Resource>(object);
			if (!resource) {
				return _fail(r_error, "Animation graphs containing non-Resource Objects are not editable.");
			}
			if (resources.has(resource->get_instance_id())) {
				return true;
			}
			resources.insert(resource->get_instance_id());
			if (AnimationLibrary *library = Object::cast_to<AnimationLibrary>(resource)) {
				if (library->get_animation_list_size() > GodotMCPAnimationLibrary::MAX_LIST_LIMIT) {
					return _fail(r_error, "Animation graphs exceed 2000 clips per library.");
				}
			}
			if (Animation *animation = Object::cast_to<Animation>(resource)) {
				if (animation->get_track_count() > MAX_TRACKS) {
					return _fail(r_error, "Animation graphs exceed 2000 tracks per clip.");
				}
				// Do this BEFORE property getters/deep duplication/decompression:
				// compressed buffers can represent far more keys than their bytes
				// suggest, and fetching every compressed key is not constant-time.
				for (int t = 0; t < animation->get_track_count(); t++) {
					const int keys = animation->track_get_key_count(t);
					if (keys < 0 || keys > GodotMCPAnimationLibrary::MAX_DECODED_KEYS_PER_TRACK || keys > remaining_keys) {
						return _fail(r_error, "Decoded animation keys exceed 10000 per track or 25000 per cloned graph.");
					}
					remaining_keys -= keys;
				}
			}
			List<PropertyInfo> properties;
			resource->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if ((property.usage & PROPERTY_USAGE_STORAGE) && !check(resource->get(property.name), r_error, p_depth + 1)) {
					return false;
				}
			}
		} else if (p_value.get_type() == Variant::DICTIONARY) {
			const Dictionary dictionary = p_value;
			if (containers.has(dictionary.id())) {
				return _fail(r_error, "Cyclic Variant containers cannot be safely deep-duplicated by Resource.");
			}
			containers.insert(dictionary.id());
			if (dictionary.size() > remaining / 2) {
				return _fail(r_error, "Animation graph dictionary exceeds the item budget.");
			}
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				if (!check(E.key, r_error, p_depth + 1) || !check(E.value, r_error, p_depth + 1)) {
					return false;
				}
			}
			containers.erase(dictionary.id());
		} else if (p_value.get_type() == Variant::ARRAY) {
			// Variant::get_indexed_size() intentionally returns zero for Array in
			// this engine version. Use its real size and const elements: animation
			// serialization stores value/method keys inside ordinary Arrays, and
			// skipping them would bypass both cycle and freed-Object validation.
			const Array array = p_value;
			if (containers.has(array.id())) {
				return _fail(r_error, "Cyclic Variant containers cannot be safely deep-duplicated by Resource.");
			}
			containers.insert(array.id());
			if (array.size() > remaining) {
				return _fail(r_error, "Animation graph array exceeds the item budget.");
			}
			for (const Variant &element : array) {
				if (!check(element, r_error, p_depth + 1)) {
					return false;
				}
			}
			containers.erase(array.id());
		} else if (p_value.is_array()) {
			const uint64_t size = p_value.get_indexed_size();
			if (p_value.get_type() != Variant::PACKED_STRING_ARRAY) {
				// Packed buffers cannot contain resource references. Account in 1K
				// blocks, with a separate byte cap, so normal audio and compressed
				// clips do not consume the key budget but duplication stays bounded.
				int stride = 1;
				switch (p_value.get_type()) {
					case Variant::PACKED_INT32_ARRAY:
					case Variant::PACKED_FLOAT32_ARRAY:
						stride = 4;
						break;
					case Variant::PACKED_INT64_ARRAY:
					case Variant::PACKED_FLOAT64_ARRAY:
						stride = 8;
						break;
					case Variant::PACKED_VECTOR2_ARRAY:
						stride = sizeof(Vector2);
						break;
					case Variant::PACKED_VECTOR3_ARRAY:
						stride = sizeof(Vector3);
						break;
					case Variant::PACKED_VECTOR4_ARRAY:
						stride = sizeof(Vector4);
						break;
					case Variant::PACKED_COLOR_ARRAY:
						stride = sizeof(Color);
						break;
					default:
						break;
				}
				if (size > 16 * 1024 * 1024 || size * stride > buffer_bytes || (size + 1023) / 1024 > uint64_t(remaining)) {
					return _fail(r_error, "Animation resource buffers exceed 16M elements, 64 MiB or the graph budget.");
				}
				remaining -= (size + 1023) / 1024;
				buffer_bytes -= size * stride;
				return true;
			}
			if (size > uint64_t(remaining)) {
				return _fail(r_error, "Animation graph array exceeds the item budget.");
			}
			for (uint64_t i = 0; i < size; i++) {
				bool valid = false;
				bool out_of_bounds = false;
				if (!check(p_value.get_indexed(i, valid, out_of_bounds), r_error, p_depth + 1)) {
					return false;
				}
			}
		}
		return true;
	}
};

static bool _preserve_track_metadata(const Ref<Animation> &p_source, const Ref<Animation> &p_copy, String &r_error) {
	if (p_copy.is_null() || p_copy == p_source || p_copy->get_track_count() != p_source->get_track_count()) {
		return _fail(r_error, "Animation could not be independently deep-duplicated.");
	}
	for (int t = 0; t < p_source->get_track_count(); t++) {
		if (p_copy->track_get_type(t) != p_source->track_get_type(t)) {
			return _fail(r_error, "Animation duplication changed a track type.");
		}
		// Compressed tracks omit interp/loop_wrap from their storage property
		// list. Restore metadata from the ORIGINAL before another duplicate or
		// copy_track can propagate serialization's default values.
		p_copy->track_set_path(t, p_source->track_get_path(t));
		p_copy->track_set_enabled(t, p_source->track_is_enabled(t));
		p_copy->track_set_imported(t, p_source->track_is_imported(t));
		p_copy->track_set_interpolation_type(t, p_source->track_get_interpolation_type(t));
		p_copy->track_set_interpolation_loop_wrap(t, p_source->track_get_interpolation_loop_wrap(t));
		if (p_source->track_get_type(t) == Animation::TYPE_VALUE) {
			p_copy->value_track_set_update_mode(t, p_source->value_track_get_update_mode(t));
		} else if (p_source->track_get_type(t) == Animation::TYPE_AUDIO) {
			p_copy->audio_track_set_use_blend(t, p_source->audio_track_is_use_blend(t));
		}
	}
	return true;
}

static Ref<AnimationLibrary> _clone_library(const Ref<AnimationLibrary> &p_library, String &r_error) {
	GraphBudget budget;
	if (!budget.check(p_library, r_error)) {
		return Ref<AnimationLibrary>();
	}
	Ref<AnimationLibrary> copy = p_library->duplicate_deep(RESOURCE_DEEP_DUPLICATE_ALL);
	if (copy.is_null() || copy == p_library) {
		_fail(r_error, "Library could not be independently deep-duplicated.");
		return Ref<AnimationLibrary>();
	}
	LocalVector<StringName> clips;
	p_library->get_animation_list(&clips);
	for (const StringName &clip : clips) {
		if (!copy->has_animation(clip) || !_preserve_track_metadata(p_library->get_animation(clip), copy->get_animation(clip), r_error)) {
			return Ref<AnimationLibrary>();
		}
	}
	copy->set_local_to_scene(true);
	return copy;
}

static Ref<Animation> _clone_animation(const Ref<Animation> &p_animation, String &r_error) {
	GraphBudget budget;
	if (!budget.check(p_animation, r_error)) {
		return Ref<Animation>();
	}
	Ref<Animation> copy = p_animation->duplicate_deep(RESOURCE_DEEP_DUPLICATE_ALL);
	if (!_preserve_track_metadata(p_animation, copy, r_error)) {
		return Ref<Animation>();
	}
	copy->set_local_to_scene(true);
	bool compressed = false;
	for (int i = 0; i < copy->get_track_count(); i++) {
		compressed |= copy->track_is_compressed(i);
	}
	if (compressed) {
		// Decompress through the real track/key APIs, not serialization fields.
		// The deep copy is the source, so nested key resources stay isolated.
		Ref<Animation> tracks;
		tracks.instantiate();
		for (int i = 0; i < copy->get_track_count(); i++) {
			copy->copy_track(i, tracks);
			if (copy->track_get_type(i) == Animation::TYPE_BEZIER) {
				for (int k = 0; k < copy->track_get_key_count(i); k++) {
					tracks->bezier_track_set_key_handle_mode(i, k, copy->bezier_track_get_key_handle_mode(i, k));
				}
			}
		}
		const double length = copy->get_length();
		const Animation::LoopMode loop = copy->get_loop_mode();
		copy->clear();
		copy->set_length(length);
		copy->set_loop_mode(loop);
		for (int i = 0; i < tracks->get_track_count(); i++) {
			tracks->copy_track(i, copy);
			if (tracks->track_get_type(i) == Animation::TYPE_BEZIER) {
				for (int k = 0; k < tracks->track_get_key_count(i); k++) {
					copy->bezier_track_set_key_handle_mode(i, k, tracks->bezier_track_get_key_handle_mode(i, k));
				}
			}
		}
	}
	return copy;
}

static bool _fields(const Dictionary &p_dictionary, std::initializer_list<const char *> p_allowed, String &r_error) {
	for (const KeyValue<Variant, Variant> &E : p_dictionary) {
		bool allowed = false;
		for (const char *name : p_allowed) {
			allowed |= E.key == Variant(name);
		}
		if (!allowed) {
			return _fail(r_error, "Unknown field: " + String(E.key));
		}
	}
	return true;
}

static bool _properties(const Dictionary &p_operation, Dictionary &r_properties, String &r_error) {
	const Variant properties = p_operation.get("properties", Dictionary());
	if (properties.get_type() != Variant::DICTIONARY) {
		return _fail(r_error, "properties must be a dictionary.");
	}
	r_properties = properties;
	return true;
}

static bool _animation_properties(const Ref<Animation> &p_animation, const Dictionary &p_properties, String &r_error) {
	if (!_fields(p_properties, { "length", "loop_mode", "step", "resource_name" }, r_error)) {
		return false;
	}
	for (const KeyValue<Variant, Variant> &E : p_properties) {
		const String name = E.key;
		double value;
		int mode;
		String text;
		if (name == "length") {
			if (!_number(E.value, value, ANIM_MIN_LENGTH, MAX_TIME)) {
				return _fail(r_error, "length must be finite and in [0.001, 1000000].");
			}
			p_animation->set_length(value);
		} else if (name == "step") {
			if (!_number(E.value, value, 0.000001, MAX_TIME)) {
				return _fail(r_error, "step must be finite and in [0.000001, 1000000].");
			}
			p_animation->set_step(value);
		} else if (name == "loop_mode") {
			if (!_integer(E.value, mode, Animation::LOOP_NONE, Animation::LOOP_PINGPONG)) {
				return _fail(r_error, "loop_mode must be an integer in [0, 2].");
			}
			p_animation->set_loop_mode(Animation::LoopMode(mode));
		} else if (name == "resource_name") {
			if (!_string(E.value, text, true)) {
				return _fail(r_error, "resource_name must be a bounded string.");
			}
			p_animation->set_name(text);
		}
	}
	return true;
}

// Track paths are relative to the mixer's animation root. Unresolved paths are
// useful during authoring, but their lexical destination must stay in-scene.
static bool _track_path(const Variant &p_value, AnimationMixer *p_mixer, Node *p_scene_root, NodePath &r_path, String &r_error) {
	String path;
	if (!_string(p_value, path) || path.length() > 4096) {
		return _fail(r_error, "Track path must be a non-empty string of at most 4096 characters.");
	}
	r_path = NodePath(path);
	if (r_path.is_absolute()) {
		return _fail(r_error, "Track paths must be relative to the AnimationMixer root.");
	}
	Node *root = p_mixer->get_node_or_null(p_mixer->get_root_node());
	if (!root || (root != p_scene_root && !p_scene_root->is_ancestor_of(root))) {
		return _fail(r_error, "AnimationMixer root_node is not contained in the edited scene.");
	}
	int depth = 0;
	for (Node *node = root; node != p_scene_root; node = node->get_parent()) {
		depth++;
	}
	for (int i = 0; i < r_path.get_name_count(); i++) {
		if (r_path.get_name(i) == SNAME("..")) {
			if (--depth < 0) {
				return _fail(r_error, "Track path escapes the edited scene.");
			}
		} else if (r_path.get_name(i) != SNAME(".")) {
			depth++;
		}
	}
	Node *target = root->get_node_or_null(NodePath(String(r_path.get_concatenated_names())));
	if (target && target != p_scene_root && !p_scene_root->is_ancestor_of(target)) {
		return _fail(r_error, "Resolved track path is outside the edited scene.");
	}
	return true;
}

static bool _track_properties(const Ref<Animation> &p_animation, int p_track, const Dictionary &p_properties,
		AnimationMixer *p_mixer, Node *p_root, String &r_error) {
	if (!_fields(p_properties, { "path", "enabled", "interpolation_type", "interpolation_loop_wrap", "value_update_mode", "imported", "audio_use_blend" }, r_error)) {
		return false;
	}
	for (const KeyValue<Variant, Variant> &E : p_properties) {
		const String name = E.key;
		int mode;
		if (name == "path") {
			NodePath path;
			if (!_track_path(E.value, p_mixer, p_root, path, r_error)) {
				return false;
			}
			p_animation->track_set_path(p_track, path);
		} else if (name == "interpolation_type") {
			if (!_integer(E.value, mode, Animation::INTERPOLATION_NEAREST, Animation::INTERPOLATION_CUBIC_ANGLE)) {
				return _fail(r_error, "interpolation_type must be an integer in [0, 4].");
			}
			p_animation->track_set_interpolation_type(p_track, Animation::InterpolationType(mode));
		} else if (name == "value_update_mode") {
			if (p_animation->track_get_type(p_track) != Animation::TYPE_VALUE || !_integer(E.value, mode, Animation::UPDATE_CONTINUOUS, Animation::UPDATE_CAPTURE)) {
				return _fail(r_error, "value_update_mode requires a value track and an integer in [0, 2].");
			}
			p_animation->value_track_set_update_mode(p_track, Animation::UpdateMode(mode));
		} else {
			if (E.value.get_type() != Variant::BOOL) {
				return _fail(r_error, name + " must be a boolean.");
			}
			if (name == "enabled") {
				p_animation->track_set_enabled(p_track, E.value);
			} else if (name == "imported") {
				p_animation->track_set_imported(p_track, E.value);
			} else if (name == "interpolation_loop_wrap") {
				p_animation->track_set_interpolation_loop_wrap(p_track, E.value);
			} else if (name == "audio_use_blend") {
				if (p_animation->track_get_type(p_track) != Animation::TYPE_AUDIO) {
					return _fail(r_error, "audio_use_blend requires an audio track.");
				}
				p_animation->audio_track_set_use_blend(p_track, E.value);
			}
		}
	}
	return true;
}

static bool _handle(const Variant &p_value, Vector2 &r_handle, bool p_in, String &r_error) {
	double values[2];
	if (!_reals(p_value, 2, values) || (p_in ? values[0] > 0 : values[0] < 0)) {
		return _fail(r_error, "Bezier handles require finite [x, y] arrays; in.x <= 0 and out.x >= 0.");
	}
	r_handle = Vector2(values[0], values[1]);
	return true;
}

static bool _key_value(const Variant &p_encoded, Animation::TrackType p_type, Variant &r_value, String &r_error) {
	int items = MAX_VALUE_ITEMS;
	if (!_decode(p_encoded, r_value, items, r_error)) {
		return false;
	}
	switch (p_type) {
		case Animation::TYPE_VALUE:
			return true; // Variant-valued tracks intentionally support heterogeneous keys.
		case Animation::TYPE_POSITION_3D:
		case Animation::TYPE_SCALE_3D:
			return r_value.get_type() == Variant::VECTOR3 || r_value.get_type() == Variant::VECTOR3I || _fail(r_error, "Position/scale keys require a typed Vector3 or Vector3i.");
		case Animation::TYPE_ROTATION_3D: {
			if (r_value.get_type() != Variant::QUATERNION && r_value.get_type() != Variant::BASIS) {
				return _fail(r_error, "Rotation keys require a typed Quaternion or Basis.");
			}
			if (r_value.get_type() == Variant::BASIS) {
				const Basis basis = r_value;
				if (!basis.is_finite() || !basis.is_rotation()) {
					return _fail(r_error, "Rotation Basis must be a finite proper rotation (no scale, shear or reflection).");
				}
			}
			Quaternion rotation = r_value;
			if (!rotation.is_normalized()) {
				return _fail(r_error, "Rotation keys must represent a normalized quaternion.");
			}
			return true;
		}
		case Animation::TYPE_BLEND_SHAPE: {
			double value;
			return _number(r_value, value) || _fail(r_error, "Blend shape keys require finite numbers.");
		}
		case Animation::TYPE_METHOD: {
			if (r_value.get_type() != Variant::DICTIONARY) {
				return _fail(r_error, "Method keys require {method: string, args: Array}.");
			}
			const Dictionary value = r_value;
			String method;
			return _fields(value, { "method", "args" }, r_error) &&
					((_string(value.get("method", Variant()), method) && method.is_valid_identifier() && value.get("args", Variant()).get_type() == Variant::ARRAY) ||
							_fail(r_error, "Method keys require an identifier method and an args array."));
		}
		case Animation::TYPE_BEZIER: {
			if (r_value.get_type() == Variant::ARRAY) {
				double values[5];
				return (_reals(r_value, 5, values) && values[1] <= 0 && values[3] >= 0) || _fail(r_error, "Bezier engine representation is [value, in.x, in.y, out.x, out.y].");
			}
			if (r_value.get_type() != Variant::DICTIONARY) {
				return _fail(r_error, "Bezier keys require {value: number, in_handle?: [x,y], out_handle?: [x,y]}.");
			}
			const Dictionary value = r_value;
			double number;
			Vector2 in_handle;
			Vector2 out_handle;
			if (!_fields(value, { "value", "in_handle", "out_handle" }, r_error) || !_number(value.get("value", Variant()), number) ||
					!_handle(value.get("in_handle", _components({ 0, 0 })), in_handle, true, r_error) ||
					!_handle(value.get("out_handle", _components({ 0, 0 })), out_handle, false, r_error)) {
				return _fail(r_error, "Invalid bezier key: " + r_error);
			}
			r_value = _components({ number, in_handle.x, in_handle.y, out_handle.x, out_handle.y });
			return true;
		}
		case Animation::TYPE_AUDIO: {
			if (r_value.get_type() != Variant::DICTIONARY) {
				return _fail(r_error, "Audio keys require {stream?: res://path, start_offset?: number, end_offset?: number}.");
			}
			Dictionary value = r_value;
			if (!_fields(value, { "stream", "start_offset", "end_offset" }, r_error)) {
				return false;
			}
			Variant stream = value.get("stream", Variant());
			if (stream.get_type() == Variant::STRING) {
				Ref<Resource> resource;
				if (!_load_resource(stream, resource, r_error)) {
					return false;
				}
				stream = resource;
			}
			if (stream.get_type() != Variant::NIL) {
				if (stream.get_type() != Variant::OBJECT || !Object::cast_to<AudioStream>(static_cast<Object *>(stream))) {
					return _fail(r_error, "Audio stream must reference an AudioStream resource or be null.");
				}
			}
			double start;
			double end;
			if (!_number(value.get("start_offset", 0), start, 0, MAX_TIME) || !_number(value.get("end_offset", 0), end, 0, MAX_TIME)) {
				return _fail(r_error, "Audio offsets must be finite numbers in [0, 1000000].");
			}
			value["stream"] = stream;
			value["start_offset"] = start;
			value["end_offset"] = end;
			r_value = value;
			return true;
		}
		case Animation::TYPE_ANIMATION: {
			String value;
			return _string(r_value, value, true) || _fail(r_error, "Animation keys require a clip-name string (empty stops playback).");
		}
	}
	return _fail(r_error, "Unsupported track type.");
}

static bool _capture_player_settings(AnimationPlayer *p_player, Dictionary &r_settings, String &r_error) {
	if (!p_player) {
		return true;
	}
	// AnimationPlayer has native blend getters/setters but no public blend-list
	// accessor. Its storage getter is used only to enumerate pairs; writes below
	// exclusively use the real player methods (the aggregate setter is lossy).
	bool valid = false;
	const Variant blend_value = p_player->get(SNAME("blend_times"), &valid);
	if (!valid || blend_value.get_type() != Variant::ARRAY || Array(blend_value).size() % 3 || Array(blend_value).size() > MAX_GRAPH_ITEMS) {
		return _fail(r_error, "AnimationPlayer blend mappings are malformed or exceed the settings budget.");
	}
	const Array blends = blend_value;
	for (int i = 0; i < blends.size(); i += 3) {
		if (!blends[i].is_string() || !blends[i + 1].is_string() || !blends[i + 2].is_num() || !Math::is_finite(double(blends[i + 2])) || double(blends[i + 2]) < 0 ||
				!p_player->has_animation(blends[i]) || !p_player->has_animation(blends[i + 1])) {
			return _fail(r_error, "AnimationPlayer blend mappings must reference existing clips and finite times.");
		}
	}
	LocalVector<StringName> names;
	p_player->get_animation_list(&names);
	if (names.size() > MAX_GRAPH_ITEMS) {
		return _fail(r_error, "AnimationPlayer clip mappings exceed the settings budget.");
	}
	Dictionary next;
	for (const StringName &name : names) {
		const StringName target = p_player->animation_get_next(name);
		if (!target.is_empty()) {
			next[String(name)] = String(target);
		}
	}
	r_settings["autoplay"] = String(p_player->get_autoplay());
	r_settings["next"] = next;
	r_settings["blends"] = blends.duplicate();
	GraphBudget budget;
	return budget.check(r_settings, r_error);
}

static void _remap_player_settings(Dictionary &r_settings, const String &p_from, const String &p_to, bool p_remove = false) {
	if (r_settings.is_empty()) {
		return;
	}
	if (r_settings["autoplay"] == Variant(p_from)) {
		r_settings["autoplay"] = p_remove ? String() : p_to;
	}
	Dictionary next;
	for (const KeyValue<Variant, Variant> &E : Dictionary(r_settings["next"])) {
		if (p_remove && (E.key == Variant(p_from) || E.value == Variant(p_from))) {
			continue;
		}
		next[E.key == Variant(p_from) ? Variant(p_to) : E.key] = E.value == Variant(p_from) ? Variant(p_to) : E.value;
	}
	r_settings["next"] = next;
	Array blends;
	const Array before = r_settings["blends"];
	for (int i = 0; i < before.size(); i += 3) {
		const bool from = String(before[i]) == p_from;
		const bool to = String(before[i + 1]) == p_from;
		if (p_remove && (from || to)) {
			continue;
		}
		blends.push_back(from ? Variant(p_to) : before[i]);
		blends.push_back(to ? Variant(p_to) : before[i + 1]);
		blends.push_back(before[i + 2]);
	}
	r_settings["blends"] = blends;
}

static void _replace_library_bindings(uint64_t p_mixer_id, const Dictionary &p_bindings, const Dictionary &p_player_settings) {
	AnimationMixer *mixer = Object::cast_to<AnimationMixer>(ObjectDB::get_instance(ObjectID(p_mixer_id)));
	if (!mixer) {
		return;
	}
	AnimationPlayer *player = Object::cast_to<AnimationPlayer>(mixer);
	if (player) {
		// Clear old links while their source clips still exist, before replacing
		// libraries. Removing a library does not propagate per-clip renames.
		const Array blends = player->get(SNAME("blend_times"));
		for (int i = 0; i + 2 < blends.size(); i += 3) {
			if (player->has_animation(blends[i]) && player->has_animation(blends[i + 1])) {
				player->set_blend_time(blends[i], blends[i + 1], 0);
			}
		}
		LocalVector<StringName> names;
		player->get_animation_list(&names);
		for (const StringName &name : names) {
			if (!player->animation_get_next(name).is_empty()) {
				player->animation_set_next(name, StringName());
			}
		}
	}
	// Remove all changed bindings first, so renames and name swaps cannot
	// collide. Callable arguments retain both original and replacement graphs.
	for (const KeyValue<Variant, Variant> &E : p_bindings) {
		if (mixer->has_animation_library(E.key)) {
			mixer->remove_animation_library(E.key);
		}
	}
	for (const KeyValue<Variant, Variant> &E : p_bindings) {
		const Ref<AnimationLibrary> library = E.value;
		if (library.is_valid()) {
			mixer->add_animation_library(E.key, library);
		}
	}
	if (player && !p_player_settings.is_empty()) {
		player->set_autoplay(p_player_settings["autoplay"]);
		for (const KeyValue<Variant, Variant> &E : Dictionary(p_player_settings["next"])) {
			if (player->has_animation(E.key)) {
				player->animation_set_next(E.key, E.value);
			}
		}
		const Array blends = p_player_settings["blends"];
		for (int i = 0; i < blends.size(); i += 3) {
			if (player->has_animation(blends[i]) && player->has_animation(blends[i + 1])) {
				player->set_blend_time(blends[i], blends[i + 1], blends[i + 2]);
			}
		}
	}
	// Native remove/add updates animation_list_changed, but NOT this editor
	// signal. Dummy preview players must observe the completed replacement.
	mixer->emit_signal(SNAME("animation_libraries_updated"));
}

// UndoRedo skips operations without a live associated ObjectID, even when a
// static Callable is valid. Associate the native replacement with its mixer,
// retaining both graphs/settings without dispatching any overridable script.
class AnimationBindingCallable : public CallableCustom {
	ObjectID mixer_id;
	Dictionary bindings;
	Dictionary player_settings;

	static bool _compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
		return p_a == p_b;
	}
	static bool _compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
		return reinterpret_cast<uintptr_t>(p_a) < reinterpret_cast<uintptr_t>(p_b);
	}

public:
	uint32_t hash() const override { return uint32_t(uint64_t(mixer_id)); }
	String get_as_text() const override { return "Godot MCP: Replace Animation Library Bindings"; }
	// UndoRedo reads this annotation while recording the action. It is not a
	// bound Object method: execution remains exclusively in native call() below.
	StringName get_method() const override { return SNAME("_godot_mcp_replace_animation_library_bindings"); }
	CompareEqualFunc get_compare_equal_func() const override { return &_compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return &_compare_less; }
	ObjectID get_object() const override { return mixer_id; }
	bool is_valid() const override { return Object::cast_to<AnimationMixer>(ObjectDB::get_instance(mixer_id)) != nullptr; }
	int get_argument_count(bool &r_is_valid) const override {
		r_is_valid = true;
		return 0;
	}
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override {
		if (p_argcount != 0) {
			r_call_error.error = Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
			r_call_error.expected = 0;
			return;
		}
		if (!is_valid()) {
			r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
			return;
		}
		_replace_library_bindings(uint64_t(mixer_id), bindings, player_settings);
		r_call_error.error = Callable::CallError::CALL_OK;
	}
	AnimationBindingCallable(ObjectID p_mixer_id, const Dictionary &p_bindings, const Dictionary &p_settings) :
			mixer_id(p_mixer_id), bindings(p_bindings), player_settings(p_settings) {}
};

struct AnimationPlan {
	AnimationMixer *mixer = nullptr;
	Node *root = nullptr;
	Dictionary originals;
	Dictionary libraries;
	Dictionary player_settings;
	HashSet<ObjectID> editable_libraries;
	HashSet<ObjectID> editable_animations;

	AnimationPlan(AnimationMixer *p_mixer, Node *p_root) :
			mixer(p_mixer), root(p_root) {
		LocalVector<StringName> names;
		mixer->get_animation_library_list(&names);
		for (const StringName &name : names) {
			originals[String(name)] = mixer->get_animation_library(name);
			libraries[String(name)] = originals[String(name)];
		}
	}

	Ref<AnimationLibrary> editable_library(const String &p_name, String &r_error) {
		if (!libraries.has(p_name)) {
			_fail(r_error, "Library does not exist: " + p_name);
			return Ref<AnimationLibrary>();
		}
		Ref<AnimationLibrary> library = libraries[p_name];
		if (!editable_libraries.has(library->get_instance_id())) {
			library = _clone_library(library, r_error);
			if (library.is_null()) {
				return library;
			}
			library->set_local_to_scene(true);
			libraries[p_name] = library;
			editable_libraries.insert(library->get_instance_id());
		}
		return library;
	}

	Ref<Animation> editable_animation(const Ref<AnimationLibrary> &p_library, const String &p_clip, String &r_error) {
		if (!p_library->has_animation(p_clip)) {
			_fail(r_error, "Animation does not exist: " + p_clip);
			return Ref<Animation>();
		}
		Ref<Animation> animation = p_library->get_animation(p_clip);
		if (!editable_animations.has(animation->get_instance_id())) {
			animation = _clone_animation(animation, r_error);
			if (animation.is_null()) {
				return animation;
			}
			p_library->add_animation(p_clip, animation);
			editable_animations.insert(animation->get_instance_id());
		}
		return animation;
	}

	bool operation(const Dictionary &p_operation, Dictionary &r_summary, String &r_error) {
		String op;
		if (!_string(p_operation.get("op", Variant()), op)) {
			return _fail(r_error, "op must be a non-empty string.");
		}
		r_summary["op"] = op;
		if (op == "add_library" || op == "remove_library" || op == "rename_library") {
			if (!_fields(p_operation, op == "add_library" ? std::initializer_list<const char *>{ "op", "library", "resource_path" } : (op == "rename_library" ? std::initializer_list<const char *>{ "op", "library", "new_name" } : std::initializer_list<const char *>{ "op", "library" }), r_error)) {
				return false;
			}
			String name;
			if (!_name(p_operation.get("library", Variant()), name, true)) {
				return _fail(r_error, "library must be a valid library name (empty addresses the default library).");
			}
			r_summary["library"] = name;
			if (op == "add_library") {
				if (libraries.has(name) || libraries.size() >= GodotMCPAnimationLibrary::MAX_LIST_LIMIT) {
					return _fail(r_error, "Library already exists or the 2000-library limit is reached.");
				}
				Ref<AnimationLibrary> library;
				if (p_operation.has("resource_path")) {
					Ref<Resource> resource;
					if (!_load_resource(p_operation["resource_path"], resource, r_error)) {
						return false;
					}
					library = resource;
					if (library.is_null()) {
						return _fail(r_error, "resource_path must reference an AnimationLibrary.");
					}
					library = _clone_library(library, r_error);
					if (library.is_null()) {
						return false;
					}
				} else {
					library.instantiate();
				}
				if (library.is_null()) {
					return _fail(r_error, "Library could not be duplicated.");
				}
				library->set_local_to_scene(true);
				editable_libraries.insert(library->get_instance_id());
				libraries[name] = library;
			} else {
				if (!libraries.has(name)) {
					return _fail(r_error, "Library does not exist: " + name);
				}
				if (op == "rename_library") {
					String new_name;
					if (!_name(p_operation.get("new_name", Variant()), new_name, true) || libraries.has(new_name)) {
						return _fail(r_error, "new_name must be a valid, unused library name.");
					}
					const Ref<AnimationLibrary> source = libraries[name];
					LocalVector<StringName> clips;
					source->get_animation_list(&clips);
					for (const StringName &clip : clips) {
						_remap_player_settings(player_settings, _qualified(name, clip), _qualified(new_name, clip));
					}
					libraries[new_name] = libraries[name];
					r_summary["new_name"] = new_name;
				} else {
					const Ref<AnimationLibrary> source = libraries[name];
					LocalVector<StringName> clips;
					source->get_animation_list(&clips);
					for (const StringName &clip : clips) {
						_remap_player_settings(player_settings, _qualified(name, clip), String(), true);
					}
				}
				libraries.erase(name);
			}
			return true;
		}

		String library_name;
		String clip;
		if (!_clip_name(p_operation.get("animation", Variant()), library_name, clip)) {
			return _fail(r_error, "animation must be a valid qualified library/clip name, or clip for the default library.");
		}
		r_summary["animation"] = _qualified(library_name, clip);
		Ref<AnimationLibrary> library = editable_library(library_name, r_error);
		if (library.is_null()) {
			return false;
		}
		if (op == "create_animation") {
			if (!_fields(p_operation, { "op", "animation", "properties", "resource_path" }, r_error)) {
				return false;
			}
			if (library->has_animation(clip) || library->get_animation_list_size() >= GodotMCPAnimationLibrary::MAX_LIST_LIMIT) {
				return _fail(r_error, "Animation already exists or the 2000-clip library limit is reached.");
			}
			Ref<Animation> animation;
			if (p_operation.has("resource_path")) {
				Ref<Resource> resource;
				if (!_load_resource(p_operation["resource_path"], resource, r_error)) {
					return false;
				}
				animation = resource;
				if (animation.is_null()) {
					return _fail(r_error, "resource_path must reference an Animation.");
				}
				animation = _clone_animation(animation, r_error);
				if (animation.is_null()) {
					return false;
				}
			} else {
				animation.instantiate();
				animation->set_local_to_scene(true);
			}
			Dictionary properties;
			if (!_properties(p_operation, properties, r_error) || !_animation_properties(animation, properties, r_error)) {
				return false;
			}
			library->add_animation(clip, animation);
			editable_animations.insert(animation->get_instance_id());
			return true;
		}
		if (!library->has_animation(clip)) {
			return _fail(r_error, "Animation does not exist: " + _qualified(library_name, clip));
		}
		if (op == "remove_animation" || op == "rename_animation" || op == "duplicate_animation") {
			if (!_fields(p_operation, op == "remove_animation" ? std::initializer_list<const char *>{ "op", "animation" } : std::initializer_list<const char *>{ "op", "animation", "new_name" }, r_error)) {
				return false;
			}
			if (op == "remove_animation") {
				_remap_player_settings(player_settings, _qualified(library_name, clip), String(), true);
				library->remove_animation(clip);
				return true;
			}
			String name;
			if (!_name(p_operation.get("new_name", Variant()), name, false) || library->has_animation(name)) {
				return _fail(r_error, "new_name must be a valid, unused clip name within the same library.");
			}
			if (op == "rename_animation") {
				_remap_player_settings(player_settings, _qualified(library_name, clip), _qualified(library_name, name));
				library->rename_animation(clip, name);
			} else {
				if (library->get_animation_list_size() >= GodotMCPAnimationLibrary::MAX_LIST_LIMIT) {
					return _fail(r_error, "The 2000-clip library limit is reached.");
				}
				Ref<Animation> animation = _clone_animation(library->get_animation(clip), r_error);
				if (animation.is_null()) {
					return false;
				}
				library->add_animation(name, animation);
				editable_animations.insert(animation->get_instance_id());
			}
			r_summary["new_name"] = _qualified(library_name, name);
			return true;
		}
		Ref<Animation> animation = editable_animation(library, clip, r_error);
		if (animation.is_null()) {
			return false;
		}
		if (op == "set_animation_properties") {
			Dictionary properties;
			return _fields(p_operation, { "op", "animation", "properties" }, r_error) && _properties(p_operation, properties, r_error) && _animation_properties(animation, properties, r_error);
		}
		if (op == "clear_animation") {
			if (!_fields(p_operation, { "op", "animation" }, r_error)) {
				return false;
			}
			animation->clear(); // Same semantics as the engine: resets length and loop mode.
			return true;
		}
		if (op == "add_track") {
			if (!_fields(p_operation, { "op", "animation", "type", "path", "at_index", "properties" }, r_error)) {
				return false;
			}
			String type;
			int track_type = -1;
			if (_string(p_operation.get("type", Variant()), type)) {
				for (int i = Animation::TYPE_VALUE; i <= Animation::TYPE_ANIMATION; i++) {
					if (type == TRACK_NAMES[i]) {
						track_type = i;
					}
				}
			}
			int at_index = animation->get_track_count();
			if (track_type < 0 || animation->get_track_count() >= MAX_TRACKS ||
					(p_operation.has("at_index") && !_integer(p_operation["at_index"], at_index, 0, animation->get_track_count()))) {
				return _fail(r_error, "Invalid track type/at_index, or the 2000-track clip limit is reached.");
			}
			NodePath path;
			Dictionary properties;
			if (!_track_path(p_operation.get("path", Variant()), mixer, root, path, r_error) || !_properties(p_operation, properties, r_error)) {
				return false;
			}
			if (properties.has("path")) {
				return _fail(r_error, "add_track.path must not also be supplied in properties.");
			}
			const int track = animation->add_track(Animation::TrackType(track_type), at_index);
			animation->track_set_path(track, path);
			r_summary["track"] = track;
			return _track_properties(animation, track, properties, mixer, root, r_error);
		}
		int track;
		if (!_integer(p_operation.get("track", Variant()), track, 0, animation->get_track_count() - 1)) {
			return _fail(r_error, "track must be an existing zero-based integer index.");
		}
		r_summary["track"] = track;
		if (op == "remove_track") {
			if (!_fields(p_operation, { "op", "animation", "track" }, r_error)) {
				return false;
			}
			animation->remove_track(track);
			return true;
		}
		if (op == "set_track") {
			Dictionary properties;
			return _fields(p_operation, { "op", "animation", "track", "properties" }, r_error) && _properties(p_operation, properties, r_error) &&
					_track_properties(animation, track, properties, mixer, root, r_error);
		}
		if (op == "move_track") {
			int to_index;
			if (!_fields(p_operation, { "op", "animation", "track", "to_index" }, r_error) ||
					!_integer(p_operation.get("to_index", Variant()), to_index, 0, animation->get_track_count() - 1)) {
				return _fail(r_error, "to_index must be an existing zero-based FINAL track position.");
			}
			animation->track_move_to(track, to_index > track ? to_index + 1 : to_index);
			r_summary["to_index"] = to_index;
			return true;
		}
		int key = -1;
		if (op != "insert_key" && !_integer(p_operation.get("key", Variant()), key, 0, animation->track_get_key_count(track) - 1)) {
			return _fail(r_error, "key must be an existing zero-based integer index.");
		}
		const Animation::TrackType type = animation->track_get_type(track);
		if (op == "remove_key") {
			if (!_fields(p_operation, { "op", "animation", "track", "key" }, r_error)) {
				return false;
			}
			animation->track_remove_key(track, key);
			r_summary["key"] = key;
			return true;
		}
		if (op == "set_bezier_handles") {
			Vector2 in_handle;
			Vector2 out_handle;
			int mode = Animation::HANDLE_MODE_FREE;
			if (!_fields(p_operation, { "op", "animation", "track", "key", "in_handle", "out_handle", "handle_mode" }, r_error) || type != Animation::TYPE_BEZIER ||
					!_handle(p_operation.get("in_handle", Variant()), in_handle, true, r_error) ||
					!_handle(p_operation.get("out_handle", Variant()), out_handle, false, r_error) ||
					(p_operation.has("handle_mode") && !_integer(p_operation["handle_mode"], mode, Animation::HANDLE_MODE_FREE, Animation::HANDLE_MODE_MIRRORED))) {
				return _fail(r_error, "set_bezier_handles requires a bezier key, valid handles and handle_mode in [0, 3].");
			}
			// Set both handles in FREE mode to avoid the balanced/mirrored setters
			// adjusting the opposite handle, then restore the requested mode.
			const Animation::HandleMode old_mode = animation->bezier_track_get_key_handle_mode(track, key);
			animation->bezier_track_set_key_handle_mode(track, key, Animation::HANDLE_MODE_FREE);
			animation->bezier_track_set_key_in_handle(track, key, in_handle);
			animation->bezier_track_set_key_out_handle(track, key, out_handle);
			animation->bezier_track_set_key_handle_mode(track, key, p_operation.has("handle_mode") ? Animation::HandleMode(mode) : old_mode);
			r_summary["key"] = key;
			return true;
		}
		if (op != "insert_key" && op != "set_key") {
			return _fail(r_error, "Unknown animation operation: " + op);
		}
		if (!_fields(p_operation, { "op", "animation", "track", "key", "time", "value", "transition" }, r_error) ||
				(op == "insert_key" && p_operation.has("key"))) {
			return _fail(r_error, "Invalid key operation fields: " + r_error);
		}
		if (op == "insert_key" && (!p_operation.has("time") || !p_operation.has("value"))) {
			return _fail(r_error, "insert_key requires time and value.");
		}
		if (op == "set_key" && !p_operation.has("time") && !p_operation.has("value") && !p_operation.has("transition")) {
			return _fail(r_error, "set_key requires at least one of time, value or transition.");
		}
		double time = op == "insert_key" ? 0 : animation->track_get_key_time(track, key);
		double transition = op == "insert_key" ? 1 : animation->track_get_key_transition(track, key);
		if ((p_operation.has("time") && !_number(p_operation["time"], time, 0, MAX_TIME)) ||
				(p_operation.has("transition") && !_number(p_operation["transition"], transition, -MAX_TIME, MAX_TIME))) {
			return _fail(r_error, "time must be in [0, 1000000] and transition in [-1000000, 1000000], both finite.");
		}
		if (p_operation.has("transition") && type >= Animation::TYPE_BEZIER && transition != 1) {
			return _fail(r_error, "Bezier/audio/animation tracks do not support transitions other than 1.");
		}
		Variant value;
		if (p_operation.has("value") && !_key_value(p_operation["value"], type, value, r_error)) {
			return false;
		}
		const int collision = animation->track_find_key(track, time, Animation::FIND_MODE_APPROX);
		// Existing-time insertion follows Godot's replace semantics. Moving a key
		// onto another key is rejected rather than silently destroying that key.
		if (op == "set_key" && p_operation.has("time")) {
			for (int k = 0; k < animation->track_get_key_count(track); k++) {
				if (k != key && Math::is_equal_approx(animation->track_get_key_time(track, k), time)) {
					return _fail(r_error, "set_key.time collides with another key; remove it explicitly first.");
				}
			}
		}
		if (op == "insert_key") {
			if (animation->track_get_key_count(track) >= MAX_GRAPH_ITEMS && collision < 0) {
				return _fail(r_error, "The 100000-key track limit is reached.");
			}
			key = animation->track_insert_key(track, time, value, transition);
			if (key < 0) {
				return _fail(r_error, "Animation rejected the key value.");
			}
			if (type < Animation::TYPE_BEZIER) {
				// _insert preserves the old transition when replacing a key.
				animation->track_set_key_transition(track, key, transition);
			}
		} else {
			if (p_operation.has("value")) {
				animation->track_set_key_value(track, key, value);
			}
			if (p_operation.has("transition")) {
				animation->track_set_key_transition(track, key, transition);
			}
			if (p_operation.has("time")) {
				const int key_count = animation->track_get_key_count(track);
				animation->track_set_key_time(track, key, time);
				if (animation->track_get_key_count(track) != key_count) {
					return _fail(r_error, "Animation key move unexpectedly changed the key count.");
				}
				key = animation->track_find_key(track, time, Animation::FIND_MODE_EXACT);
				if (key < 0) {
					return _fail(r_error, "Animation key move did not preserve the requested exact time.");
				}
			}
		}
		r_summary["key"] = key;
		r_summary["time"] = time;
		return true;
	}
};

} // namespace

Dictionary GodotMCPAnimationLibrary::list(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
		int &r_error_code, String &r_error_message) const {
	if (!_guard(p_params, p_context, false, r_error_code, r_error_message)) {
		return Dictionary();
	}
	int limit;
	if (!_integer(p_params.get("limit", 256), limit, 1, MAX_LIST_LIMIT)) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: limit must be an integer in [1, 2000].";
		return Dictionary();
	}
	Vector<AnimationMixer *> mixers;
	int scanned_nodes = 0;
	bool scan_truncated = false;
	bool truncated = false;
	int mixer_count = 0;
	if (p_params.has("node_path")) {
		AnimationMixer *mixer = _target(p_params, p_context, r_error_code, r_error_message);
		if (!mixer) {
			return Dictionary();
		}
		mixers.push_back(mixer);
		mixer_count = 1;
		scanned_nodes = 1;
	} else {
		Vector<Node *> pending;
		pending.push_back(p_context.root);
		int scheduled = 1;
		while (!pending.is_empty()) {
			Node *node = pending[pending.size() - 1];
			pending.remove_at(pending.size() - 1);
			scanned_nodes++;
			if (AnimationMixer *mixer = Object::cast_to<AnimationMixer>(node)) {
				mixer_count++;
				if (mixers.size() < limit) {
					mixers.push_back(mixer);
				} else {
					truncated = true;
				}
			}
			const int children = node->get_child_count();
			const int take = MIN(children, MAX_SCENE_NODES - scheduled);
			scan_truncated |= children > take;
			// Reverse push preserves normal scene order without recursive C++ calls.
			for (int i = take - 1; i >= 0; i--) {
				pending.push_back(node->get_child(i));
			}
			scheduled += take;
		}
	}
	Array nodes;
	int library_count = 0;
	int animation_count = 0;
	int64_t track_count = 0;
	int returned_libraries = 0;
	int returned_animations = 0;
	int returned_tracks = 0;
	for (AnimationMixer *mixer : mixers) {
		Dictionary node;
		const String node_path = String(p_context.root->get_path_to(mixer));
		const String root_node = String(mixer->get_root_node());
		node["node_path"] = node_path.substr(0, 4096);
		node["type"] = mixer->get_class();
		node["root_node"] = root_node.substr(0, 4096);
		node["metadata_truncated"] = node_path.length() > 4096 || root_node.length() > 4096;
		truncated |= bool(node["metadata_truncated"]);
		LocalVector<StringName> library_names;
		mixer->get_animation_library_list(&library_names);
		node["library_count"] = library_names.size();
		library_count += library_names.size();
		Array libraries;
		for (const StringName &library_name : library_names) {
			if (returned_libraries >= limit) {
				truncated = true;
				break;
			}
			returned_libraries++;
			const Ref<AnimationLibrary> library = mixer->get_animation_library(library_name);
			Dictionary library_data = _resource_info(library);
			library_data["library"] = String(library_name).substr(0, 256);
			library_data["metadata_truncated"] = bool(library_data["metadata_truncated"]) || String(library_name).length() > 256;
			truncated |= bool(library_data["metadata_truncated"]);
			library_data["animation_count"] = library->get_animation_list_size();
			animation_count += library->get_animation_list_size();
			Array animations;
			LocalVector<StringName> clips;
			library->get_animation_list(&clips);
			for (const StringName &clip : clips) {
				if (returned_animations >= limit) {
					truncated = true;
					break;
				}
				returned_animations++;
				const Ref<Animation> animation = library->get_animation(clip);
				Dictionary animation_data = _animation_info(animation, library_name, clip);
				truncated |= bool(animation_data["metadata_truncated"]);
				track_count += animation->get_track_count();
				Array tracks;
				bool imported = false;
				for (int t = 0; t < animation->get_track_count() && returned_tracks < limit; t++) {
					const Dictionary track_data = _track_info(animation, t);
					truncated |= bool(track_data["metadata_truncated"]);
					tracks.push_back(track_data);
					imported |= animation->track_is_imported(t);
					returned_tracks++;
				}
				const bool tracks_truncated = tracks.size() < animation->get_track_count();
				animation_data["tracks"] = tracks;
				animation_data["returned_track_count"] = tracks.size();
				animation_data["tracks_truncated"] = tracks_truncated;
				animation_data["has_imported_tracks"] = imported;
				animation_data["imported_count_complete"] = !tracks_truncated;
				truncated |= tracks_truncated;
				animations.push_back(animation_data);
			}
			library_data["animations"] = animations;
			library_data["returned_animation_count"] = animations.size();
			library_data["truncated"] = animations.size() < library->get_animation_list_size();
			libraries.push_back(library_data);
		}
		node["libraries"] = libraries;
		node["returned_library_count"] = libraries.size();
		node["truncated"] = libraries.size() < int(library_names.size());
		nodes.push_back(node);
	}
	Dictionary data;
	data["scene_path"] = p_context.scene_path;
	data["scene_version"] = p_context.scene_version;
	data["nodes"] = nodes;
	data["limit"] = limit;
	data["node_count"] = mixer_count;
	data["returned_node_count"] = nodes.size();
	data["library_count"] = library_count;
	data["returned_library_count"] = returned_libraries;
	data["animation_count"] = animation_count;
	data["returned_animation_count"] = returned_animations;
	data["track_count"] = track_count;
	data["returned_track_count"] = returned_tracks;
	data["scanned_node_count"] = scanned_nodes;
	data["scan_truncated"] = scan_truncated;
	data["truncated"] = truncated || scan_truncated;
	// Counts for descendants of omitted descriptors are lower bounds, not totals.
	data["counts_complete"] = !truncated && !scan_truncated;
	return data;
}

Dictionary GodotMCPAnimationLibrary::get(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
		int &r_error_code, String &r_error_message) const {
	if (!_guard(p_params, p_context, false, r_error_code, r_error_message)) {
		return Dictionary();
	}
	AnimationMixer *mixer = _target(p_params, p_context, r_error_code, r_error_message);
	if (!mixer) {
		return Dictionary();
	}
	String library_name;
	String clip;
	int key_limit;
	const Variant include_keys = p_params.get("include_keys", false);
	if (!_clip_name(p_params.get("animation", Variant()), library_name, clip) ||
			!_integer(p_params.get("key_limit", 1000), key_limit, 1, MAX_KEY_LIMIT) || include_keys.get_type() != Variant::BOOL) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: animation must be a qualified clip name, include_keys a boolean, key_limit an integer in [1, 10000].";
		return Dictionary();
	}
	if (!mixer->has_animation_library(library_name) || !mixer->get_animation_library(library_name)->has_animation(clip)) {
		r_error_code = GodotMCPError::NOT_FOUND;
		r_error_message = "NOT_FOUND: animation does not exist: " + _qualified(library_name, clip);
		return Dictionary();
	}
	const Ref<AnimationLibrary> library = mixer->get_animation_library(library_name);
	const Ref<Animation> animation = library->get_animation(clip);
	Dictionary data = _animation_info(animation, library_name, clip);
	data["scene_path"] = p_context.scene_path;
	data["scene_version"] = p_context.scene_version;
	const String node_path = String(p_context.root->get_path_to(mixer));
	data["node_path"] = node_path.substr(0, 4096);
	data["library_resource"] = _resource_info(library);
	bool metadata_truncated = bool(data["metadata_truncated"]) || node_path.length() > 4096 || bool(Dictionary(data["library_resource"])["metadata_truncated"]);
	data["include_keys"] = include_keys;
	data["key_limit"] = key_limit;
	Array tracks;
	int returned_keys = 0;
	int value_items = MAX_VALUE_ITEMS;
	int64_t key_count = 0;
	bool values_truncated = false;
	bool keys_truncated = false;
	const int track_limit = MIN(animation->get_track_count(), MAX_TRACKS);
	for (int t = 0; t < track_limit; t++) {
		Dictionary track = _track_info(animation, t);
		metadata_truncated |= bool(track["metadata_truncated"]);
		const int keys = animation->track_get_key_count(t);
		key_count += keys;
		Array key_data;
		if (include_keys) {
			for (int k = 0; k < keys && returned_keys < key_limit; k++) {
				Dictionary key;
				key["key"] = k;
				key["time"] = animation->track_get_key_time(t, k);
				key["transition"] = animation->track_get_key_transition(t, k);
				const Animation::TrackType type = animation->track_get_type(t);
				// Method/audio dictionaries are protocol records, not generic Variant
				// dictionaries. Keeping their shape mirrors the insertion contract.
				if (type == Animation::TYPE_METHOD) {
					Dictionary value;
					value["method"] = _encode(String(animation->method_track_get_name(t, k)), value_items, values_truncated);
					value["args"] = _encode(Variant(animation->method_track_get_params(t, k)), value_items, values_truncated);
					key["value"] = value;
				} else if (type == Animation::TYPE_AUDIO) {
					Dictionary value;
					value["stream"] = _encode(animation->audio_track_get_key_stream(t, k), value_items, values_truncated);
					value["start_offset"] = animation->audio_track_get_key_start_offset(t, k);
					value["end_offset"] = animation->audio_track_get_key_end_offset(t, k);
					key["value"] = value;
				} else if (type == Animation::TYPE_BEZIER) {
					Dictionary value;
					const Vector2 in_handle = animation->bezier_track_get_key_in_handle(t, k);
					const Vector2 out_handle = animation->bezier_track_get_key_out_handle(t, k);
					value["value"] = animation->bezier_track_get_key_value(t, k);
					value["in_handle"] = _components({ in_handle.x, in_handle.y });
					value["out_handle"] = _components({ out_handle.x, out_handle.y });
					key["value"] = value;
					key["handle_mode"] = int(animation->bezier_track_get_key_handle_mode(t, k));
				} else if (type == Animation::TYPE_ANIMATION) {
					key["value"] = _encode(String(animation->animation_track_get_key_animation(t, k)), value_items, values_truncated);
				} else {
					key["value"] = _encode(animation->track_get_key_value(t, k), value_items, values_truncated);
				}
				key_data.push_back(key);
				returned_keys++;
			}
			track["keys"] = key_data;
			track["returned_key_count"] = key_data.size();
			track["keys_truncated"] = key_data.size() < keys;
			keys_truncated |= key_data.size() < keys;
		}
		tracks.push_back(track);
	}
	data["tracks"] = tracks;
	data["returned_track_count"] = tracks.size();
	data["tracks_truncated"] = track_limit < animation->get_track_count();
	data["key_count"] = key_count;
	data["key_count_complete"] = track_limit == animation->get_track_count();
	data["returned_key_count"] = returned_keys;
	data["keys_truncated"] = keys_truncated || (bool(include_keys) && track_limit < animation->get_track_count());
	data["values_truncated"] = values_truncated;
	data["metadata_truncated"] = metadata_truncated;
	data["truncated"] = keys_truncated || values_truncated || metadata_truncated || track_limit < animation->get_track_count();
	return data;
}

Dictionary GodotMCPAnimationLibrary::apply(const Dictionary &p_params, const GodotMCPScenePatch::Context &p_context,
		Array &r_warnings, int &r_error_code, String &r_error_message) const {
	if (!_guard(p_params, p_context, true, r_error_code, r_error_message)) {
		return Dictionary();
	}
	AnimationMixer *mixer = _target(p_params, p_context, r_error_code, r_error_message);
	if (!mixer) {
		return Dictionary();
	}
	const Variant dry_run = p_params.get("dry_run", false);
	const Variant inputs = p_params.get("operations", Variant());
	if (dry_run.get_type() != Variant::BOOL || inputs.get_type() != Variant::ARRAY || Array(inputs).is_empty() || Array(inputs).size() > MAX_OPERATIONS) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: dry_run must be boolean and operations must contain 1..512 ordered operations.";
		return Dictionary();
	}
	AnimationPlan plan(mixer, p_context.root);
	Dictionary original_player_settings;
	if (!_capture_player_settings(Object::cast_to<AnimationPlayer>(mixer), original_player_settings, r_error_message)) {
		r_error_code = GodotMCPError::INVALID_ARGUMENT;
		r_error_message = "INVALID_ARGUMENT: " + r_error_message;
		return Dictionary();
	}
	plan.player_settings = original_player_settings.duplicate(true);
	Array results;
	const Array operations = inputs;
	for (int i = 0; i < operations.size(); i++) {
		Dictionary summary;
		String error;
		if (operations[i].get_type() != Variant::DICTIONARY || !plan.operation(operations[i], summary, error)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: operation " + itos(i) + ": " + (error.is_empty() ? String("operation must be a dictionary.") : error);
			return Dictionary();
		}
		summary["operation_index"] = i;
		summary["status"] = bool(dry_run) ? "validated" : "applied";
		results.push_back(summary);
	}
	// Bound the final shadow graph too: successive insertions cannot grow an
	// initially small graph beyond the deep-duplication safety limit.
	GraphBudget final_budget;
	String graph_error;
	for (const KeyValue<Variant, Variant> &E : plan.libraries) {
		const Ref<AnimationLibrary> library = E.value;
		if (!plan.editable_libraries.has(library->get_instance_id())) {
			continue;
		}
		if (library->get_animation_list_size() > MAX_LIST_LIMIT) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: edited libraries must contain at most 2000 clips.";
			return Dictionary();
		}
		LocalVector<StringName> clips;
		library->get_animation_list(&clips);
		for (const StringName &clip : clips) {
			if (library->get_animation(clip)->get_track_count() > MAX_TRACKS) {
				r_error_code = GodotMCPError::INVALID_ARGUMENT;
				r_error_message = "INVALID_ARGUMENT: edited animation graphs must contain at most 2000 tracks per clip.";
				return Dictionary();
			}
		}
		if (!final_budget.check(library, graph_error)) {
			r_error_code = GodotMCPError::INVALID_ARGUMENT;
			r_error_message = "INVALID_ARGUMENT: final animation graph: " + graph_error;
			return Dictionary();
		}
	}
	Vector<String> changed_names;
	for (const KeyValue<Variant, Variant> &E : plan.originals) {
		if (!plan.libraries.has(E.key) || plan.libraries[E.key] != E.value) {
			changed_names.push_back(E.key);
		}
	}
	for (const KeyValue<Variant, Variant> &E : plan.libraries) {
		if (!plan.originals.has(E.key)) {
			changed_names.push_back(E.key);
		}
	}
	changed_names.sort();
	const bool applied = !bool(dry_run) && !changed_names.is_empty();
	if (!applied) {
		for (int i = 0; i < results.size(); i++) {
			Dictionary summary = results[i];
			summary["status"] = "validated";
		}
	}
	if (applied) {
		if (!p_context.undo_redo) {
			r_error_code = GodotMCPError::INTERNAL;
			r_error_message = "INTERNAL: the edited scene has no undo history.";
			return Dictionary();
		}
		if (p_context.undo_redo_manager) {
			p_context.undo_redo_manager->create_action_for_history(UNDO_ACTION, p_context.history_id);
		} else {
			p_context.undo_redo->create_action(UNDO_ACTION);
		}
		Dictionary before_bindings;
		Dictionary after_bindings;
		for (const String &name : changed_names) {
			before_bindings[name] = plan.originals.get(name, Variant());
			after_bindings[name] = plan.libraries.get(name, Variant());
		}
		p_context.undo_redo->add_do_method(Callable(memnew(AnimationBindingCallable(mixer->get_instance_id(), after_bindings, plan.player_settings))));
		p_context.undo_redo->add_undo_method(Callable(memnew(AnimationBindingCallable(mixer->get_instance_id(), before_bindings, original_player_settings))));
		if (p_context.undo_redo_manager) {
			p_context.undo_redo_manager->commit_action();
		} else {
			p_context.undo_redo->commit_action();
		}
	}
	Dictionary data;
	data["scene_path"] = p_context.scene_path;
	data["node_path"] = String(p_context.root->get_path_to(mixer));
	data["previous_scene_version"] = p_context.scene_version;
	data["updated_scene_version"] = applied ? GodotMCPScenePatch::compute_scene_version(p_context.root, p_context.history_id, p_context.undo_redo->get_version()) : p_context.scene_version;
	data["scene_version"] = data["updated_scene_version"];
	data["dry_run"] = dry_run;
	data["applied"] = applied;
	data["operation_count"] = operations.size();
	data["success_count"] = operations.size();
	data["operations"] = results;
	Array changed_libraries;
	for (const String &name : changed_names) {
		changed_libraries.push_back(name);
	}
	data["changed_libraries"] = changed_libraries;
	data["undo_action_name"] = applied ? String(UNDO_ACTION) : String();
	data["save_status"] = "not_requested";
	if (!changed_names.is_empty()) {
		Dictionary warning;
		warning["code"] = "LOCAL_ANIMATION_COPIES";
		warning["message"] = "Changed animation resources are scene-local copies; no external resources or scene files were saved.";
		r_warnings.push_back(warning);
	}
	return data;
}
