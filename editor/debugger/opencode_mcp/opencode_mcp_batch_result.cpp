/**************************************************************************/
/*  opencode_mcp_batch_result.cpp                                         */
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

#include "opencode_mcp_batch_result.h"

void OpenCodeMCPBatchResult::add_success(const Variant &p_key, const Variant &p_value, const String &p_key_name) {
	Dictionary item;
	item[p_key_name] = p_key;
	item["ok"] = true;
	item["value"] = p_value;
	items.push_back(item);
	success_count++;
}

void OpenCodeMCPBatchResult::add_error(const Variant &p_key, const Dictionary &p_error, const String &p_key_name) {
	Dictionary item;
	item[p_key_name] = p_key;
	item["ok"] = false;
	item["error"] = p_error;
	items.push_back(item);
	failure_count++;
}

void OpenCodeMCPBatchResult::add_skipped(const Variant &p_key, const Dictionary &p_error, const String &p_key_name) {
	Dictionary item;
	item[p_key_name] = p_key;
	item["ok"] = false;
	item["skipped"] = true;
	item["error"] = p_error;
	items.push_back(item);
	skipped_count++;
}

Dictionary OpenCodeMCPBatchResult::to_data() const {
	Dictionary data;
	data["items"] = items;
	data["count"] = items.size();
	data["success_count"] = success_count;
	data["failure_count"] = failure_count;
	data["skipped_count"] = skipped_count;
	return data;
}
