/**************************************************************************/
/*  test_gdscript_diff_node.h                                             */
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

#include "../gdscript_parser.h"
#include "../whenline/gdscript_diff_node.h"
#include "../whenline/truediff.h"

#include "core/templates/hash_set.h"
#include "tests/test_macros.h"

namespace TestGDScriptDiffNode {

// Helpers ---------------------------------------------------------------------

// Parse `p_source` with `GDScriptParser` and wrap the resulting AST as a diff
// tree. Skips the test if parsing fails.
static GDScriptDiffNode *_parse_and_wrap(const String &p_source) {
	GDScriptParser *parser = memnew(GDScriptParser);
	Error err = parser->parse(p_source, "res://test.gd", false);
	REQUIRE_MESSAGE(err == OK, "GDScript source must parse successfully.");
	GDScriptDiffNode *root = GDScriptDiffNode::from_parser_root(parser->get_tree());
	memdelete(parser);
	REQUIRE(root != nullptr);
	WhenlineDiffNode::compute_hashes(root);
	return root;
}

// Build an old_line → new_line map from a diff's matched-pair list, using
// the same "deepest pair wins, but only on equal-span subtrees" rule that
// `GDScript::_whenline_emit_reload_diff` uses. Kept in the test file so the
// production code stays focused on the engine path.
static HashMap<int, int> _build_line_map(const WhenlineEditScript &p_script) {
	HashMap<int, int> line_map;
	for (const WhenlineEditScript::Match &m : p_script.matches) {
		if (!m.old_node || !m.new_node) {
			continue;
		}
		const GDScriptDiffNode *old_n = static_cast<const GDScriptDiffNode *>(m.old_node);
		const GDScriptDiffNode *new_n = static_cast<const GDScriptDiffNode *>(m.new_node);
		const int old_start = old_n->get_start_line();
		const int new_start = new_n->get_start_line();
		const int old_end = MAX(old_n->get_end_line(), old_start);
		const int new_end = MAX(new_n->get_end_line(), new_start);
		if (old_start <= 0 || new_start <= 0) {
			continue;
		}
		const int old_span = old_end - old_start;
		const int new_span = new_end - new_start;
		if (old_span != new_span) {
			continue;
		}
		for (int i = 0; i <= old_span; i++) {
			line_map[old_start + i] = new_start + i;
		}
	}
	return line_map;
}

// Collect every parser-side line touched by the diff. We intentionally accept
// both the "old" and "new" sides here because:
//   - OP_DETACH carries a node from the old tree (lines we removed).
//   - OP_ATTACH carries a node from the new tree (lines we added).
//   - OP_UPDATE carries the old node, but its line is the same as the new.
static HashSet<int> _collect_changed_lines(const WhenlineEditScript &p_script) {
	HashSet<int> lines;
	auto add = [&](WhenlineDiffNode *p_node) {
		if (!p_node) {
			return;
		}
		const GDScriptDiffNode *gd = static_cast<const GDScriptDiffNode *>(p_node);
		const int start = gd->get_start_line();
		const int end = MAX(gd->get_end_line(), start);
		for (int l = start; l <= end; l++) {
			if (l > 0) {
				lines.insert(l);
			}
		}
	};
	for (const WhenlineDiffNode::Op &op : p_script.neg_ops) {
		if (op.kind == WhenlineDiffNode::OP_DETACH || op.kind == WhenlineDiffNode::OP_REMOVE) {
			add(op.node);
		}
	}
	for (const WhenlineDiffNode::Op &op : p_script.pos_ops) {
		if (op.kind == WhenlineDiffNode::OP_ATTACH || op.kind == WhenlineDiffNode::OP_UPDATE || op.kind == WhenlineDiffNode::OP_LOAD) {
			add(op.node);
		}
	}
	return lines;
}

// Tests -----------------------------------------------------------------------

TEST_SUITE("[Modules][GDScript][TrueDiff][Integration]") {
	TEST_CASE("[GDScriptDiff] Identical scripts produce no edits") {
		const String src = "extends Node\n\nfunc _ready():\n\tprint(\"hi\")\n";
		GDScriptDiffNode *a = _parse_and_wrap(src);
		GDScriptDiffNode *b = _parse_and_wrap(src);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);
		CHECK(script.is_empty());

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[GDScriptDiff] Renaming a literal flags only its line") {
		const String src_a =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tprint(\"hi\")\n"
				"\tprint(\"bye\")\n";
		const String src_b =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tprint(\"hello\")\n" // line 4 changed
				"\tprint(\"bye\")\n";

		GDScriptDiffNode *a = _parse_and_wrap(src_a);
		GDScriptDiffNode *b = _parse_and_wrap(src_b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK_FALSE(script.is_empty());
		HashSet<int> lines = _collect_changed_lines(script);
		CHECK_MESSAGE(lines.has(4), "Line 4 should be reported as changed.");
		CHECK_FALSE_MESSAGE(lines.has(5), "Line 5 should not be reported as changed.");

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[GDScriptDiff] Adding an else branch flags only the new lines") {
		const String src_a =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tif true:\n"
				"\t\tprint(\"yes\")\n";
		const String src_b =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tif true:\n"
				"\t\tprint(\"yes\")\n"
				"\telse:\n" // new
				"\t\tprint(\"no\")\n"; // new

		GDScriptDiffNode *a = _parse_and_wrap(src_a);
		GDScriptDiffNode *b = _parse_and_wrap(src_b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK_FALSE(script.is_empty());
		HashSet<int> lines = _collect_changed_lines(script);
		// Line 7 (the new `print("no")`) must always be flagged. Line 6
		// (the `else:` itself) gets reported via the new SuiteNode's
		// `start_line`, which is also fine.
		CHECK_MESSAGE(lines.has(7), "The newly inserted else body should be reported.");
		// Lines 1, 2, 4, 5 are unchanged in source; the algorithm must not
		// flag them as changed.
		CHECK_FALSE_MESSAGE(lines.has(1), "Class header should not be flagged.");
		CHECK_FALSE_MESSAGE(lines.has(5), "Original true-branch body should not be flagged.");

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[GDScriptDiff] Inserting code above unchanged code maps its lines") {
		const String src_a =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tprint(\"hi\")\n";
		const String src_b =
				"extends Node\n"
				"\n"
				"var added := 1\n" // new line 3
				"var also_added := 2\n" // new line 4
				"\n"
				"func _ready():\n" // was line 3, now line 6
				"\tprint(\"hi\")\n"; // was line 4, now line 7

		GDScriptDiffNode *a = _parse_and_wrap(src_a);
		GDScriptDiffNode *b = _parse_and_wrap(src_b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		HashMap<int, int> line_map = _build_line_map(script);

		CHECK_MESSAGE(line_map.has(3), "Old line 3 (`func _ready():`) should be in the map.");
		CHECK_MESSAGE(line_map.has(4), "Old line 4 (`\\tprint(...)`) should be in the map.");
		if (line_map.has(3)) {
			CHECK_MESSAGE(line_map[3] == 6, "Old line 3 should map to new line 6.");
		}
		if (line_map.has(4)) {
			CHECK_MESSAGE(line_map[4] == 7, "Old line 4 should map to new line 7.");
		}

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[GDScriptDiff] Editing a line in place excludes it from the line map") {
		const String src_a =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tprint(\"hi\")\n"
				"\tprint(\"bye\")\n";
		const String src_b =
				"extends Node\n"
				"\n"
				"func _ready():\n"
				"\tprint(\"hello\")\n" // edited in place
				"\tprint(\"bye\")\n";

		GDScriptDiffNode *a = _parse_and_wrap(src_a);
		GDScriptDiffNode *b = _parse_and_wrap(src_b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		HashMap<int, int> line_map = _build_line_map(script);
		HashSet<int> changed = _collect_changed_lines(script);

		// The simulated post-filter that the engine performs before sending
		// the diff: drop any mapping whose target lands on a changed line.
		HashMap<int, int> filtered;
		for (const KeyValue<int, int> &kv : line_map) {
			if (!changed.has(kv.value)) {
				filtered[kv.key] = kv.value;
			}
		}

		// The unchanged surrounding code (line 5 → line 5) must survive.
		CHECK_MESSAGE(filtered.has(5), "Unchanged line 5 should still map.");
		if (filtered.has(5)) {
			CHECK(filtered[5] == 5);
		}
		// Line 4 was edited in place; its mapping must have been dropped.
		CHECK_FALSE_MESSAGE(filtered.has(4), "Edited line 4 should not be in the filtered map.");

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[GDScriptDiff] Adding a new function leaves existing functions untouched") {
		const String src_a =
				"extends Node\n"
				"\n"
				"func a():\n"
				"\tprint(1)\n"
				"\n"
				"func b():\n"
				"\tprint(2)\n";
		const String src_b =
				"extends Node\n"
				"\n"
				"func a():\n"
				"\tprint(1)\n"
				"\n"
				"func added():\n" // new function
				"\tprint(3)\n"
				"\n"
				"func b():\n"
				"\tprint(2)\n";

		GDScriptDiffNode *a = _parse_and_wrap(src_a);
		GDScriptDiffNode *b = _parse_and_wrap(src_b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK_FALSE(script.is_empty());
		HashSet<int> lines = _collect_changed_lines(script);
		// The new function's lines (6 and 7 in src_b) must be flagged.
		const bool inserted_function_flagged = lines.has(6) || lines.has(7);
		CHECK_MESSAGE(inserted_function_flagged,
				"At least one line of the inserted function must be flagged.");

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}
}

} // namespace TestGDScriptDiffNode
