/**************************************************************************/
/*  test_truediff.h                                                       */
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

#include "../whenline/truediff.h"

#include "core/templates/hashfuncs.h"
#include "tests/test_macros.h"

namespace TestTrueDiff {

// Minimal node classes mirroring the JS test fixture (SBBlock / SBText).
//
// `MockBlock` is an internal node identified by a `type` name (like "list" or
// "item"); its children form its content. `MockText` is a leaf carrying a
// piece of literal text and contributing no child structure.

class MockBlock : public WhenlineDiffNode {
	StringName type;

public:
	MockBlock(const StringName &p_type, const std::initializer_list<WhenlineDiffNode *> &p_children) {
		type = p_type;
		for (WhenlineDiffNode *c : p_children) {
			children.push_back(c);
		}
	}
	StringName get_type_name() const override { return type; }
	WhenlineDiffNode *shallow_clone() const override {
		return memnew(MockBlock(type, {}));
	}
	void _compute_local_hashes() override {
		structure_local_hash = type.hash();
		literal_local_hash = type.hash();
	}
};

class MockText : public WhenlineDiffNode {
	String text;

public:
	MockText(const String &p_text) { text = p_text; }
	bool is_text() const override { return true; }
	String get_text() const override { return text; }
	WhenlineDiffNode *shallow_clone() const override {
		return memnew(MockText(text));
	}
	void _compute_local_hashes() override {
		// All text nodes share the same structural slot; only the literal
		// hash differentiates them.
		structure_local_hash = hash_murmur3_one_32(0xdeadbeef);
		literal_local_hash = hash_murmur3_one_32(text.hash(), structure_local_hash);
	}
};

// Helpers ---------------------------------------------------------------------

static int _count_ops_of(const WhenlineEditScript &p_script, WhenlineDiffNode::OpKind p_kind) {
	int n = 0;
	for (const WhenlineDiffNode::Op &op : p_script.neg_ops) {
		if (op.kind == p_kind) {
			n++;
		}
	}
	for (const WhenlineDiffNode::Op &op : p_script.pos_ops) {
		if (op.kind == p_kind) {
			n++;
		}
	}
	return n;
}

// Tests -----------------------------------------------------------------------

TEST_SUITE("[Modules][GDScript][TrueDiff]") {
	TEST_CASE("[TrueDiff] Identical trees produce no edits") {
		WhenlineDiffNode *a = memnew(MockBlock("list", { memnew(MockBlock("item", { memnew(MockText("hello")) })) }));
		WhenlineDiffNode *b = memnew(MockBlock("list", { memnew(MockBlock("item", { memnew(MockText("hello")) })) }));

		WhenlineDiffNode::compute_hashes(a);
		WhenlineDiffNode::compute_hashes(b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK_MESSAGE(script.is_empty(), "Identical trees should not produce any edit ops.");

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[TrueDiff] Updating a leaf produces a single update op") {
		WhenlineDiffNode *a = memnew(MockBlock("list", { memnew(MockBlock("item", { memnew(MockText("hello")) })) }));
		WhenlineDiffNode *b = memnew(MockBlock("list", { memnew(MockBlock("item", { memnew(MockText("world")) })) }));

		WhenlineDiffNode::compute_hashes(a);
		WhenlineDiffNode::compute_hashes(b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK(_count_ops_of(script, WhenlineDiffNode::OP_UPDATE) == 1);
		CHECK(_count_ops_of(script, WhenlineDiffNode::OP_ATTACH) == 0);
		CHECK(_count_ops_of(script, WhenlineDiffNode::OP_DETACH) == 0);

		// The update op should target the original "hello" node and carry the
		// new text "world".
		bool found = false;
		for (const WhenlineDiffNode::Op &op : script.pos_ops) {
			if (op.kind == WhenlineDiffNode::OP_UPDATE) {
				CHECK(op.old_text == "hello");
				CHECK(op.new_text == "world");
				found = true;
			}
		}
		CHECK(found);

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[TrueDiff] Deleting one item from the middle of a list does not touch the others") {
		WhenlineDiffNode *a = memnew(MockBlock("list", {
															   memnew(MockBlock("item", { memnew(MockText("a")) })),
															   memnew(MockBlock("item", { memnew(MockText("b")) })),
															   memnew(MockBlock("item", { memnew(MockText("c")) })),
													   }));
		WhenlineDiffNode *b = memnew(MockBlock("list", {
															   memnew(MockBlock("item", { memnew(MockText("a")) })),
															   memnew(MockBlock("item", { memnew(MockText("c")) })),
													   }));

		WhenlineDiffNode::compute_hashes(a);
		WhenlineDiffNode::compute_hashes(b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		// Mirrors `diff.test.ts`: no positive ops besides "the b detach", and
		// in particular no updates (i.e. existing "a" and "c" subtrees stay).
		CHECK_MESSAGE(_count_ops_of(script, WhenlineDiffNode::OP_UPDATE) == 0,
				"Existing items should be reused; no updates expected.");
		CHECK_MESSAGE(_count_ops_of(script, WhenlineDiffNode::OP_ATTACH) == 0,
				"No new attachments expected when only deleting.");
		// A detach for the dropped middle item plus a remove of its subtree.
		CHECK(_count_ops_of(script, WhenlineDiffNode::OP_DETACH) == 1);

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[TrueDiff] Inserting a leaf produces an attach op") {
		WhenlineDiffNode *a = memnew(MockBlock("list", {
															   memnew(MockBlock("item", { memnew(MockText("a")) })),
															   memnew(MockBlock("item", { memnew(MockText("c")) })),
													   }));
		WhenlineDiffNode *b = memnew(MockBlock("list", {
															   memnew(MockBlock("item", { memnew(MockText("a")) })),
															   memnew(MockBlock("item", { memnew(MockText("b")) })),
															   memnew(MockBlock("item", { memnew(MockText("c")) })),
													   }));

		WhenlineDiffNode::compute_hashes(a);
		WhenlineDiffNode::compute_hashes(b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		CHECK_MESSAGE(_count_ops_of(script, WhenlineDiffNode::OP_UPDATE) == 0,
				"Existing items should be reused; no updates expected.");
		// An "item" block must be attached for "b" plus its inner text.
		CHECK(_count_ops_of(script, WhenlineDiffNode::OP_ATTACH) >= 1);

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}

	TEST_CASE("[TrueDiff] Type mismatch at the root falls back to full replacement") {
		WhenlineDiffNode *a = memnew(MockBlock("list", { memnew(MockBlock("item", { memnew(MockText("x")) })) }));
		WhenlineDiffNode *b = memnew(MockBlock("paragraph", { memnew(MockText("x")) }));

		WhenlineDiffNode::compute_hashes(a);
		WhenlineDiffNode::compute_hashes(b);

		TrueDiff diff;
		WhenlineEditScript script = diff.detect_edits(a, b);

		// Top-level types differ, so the algorithm must detach the old root
		// and attach the new one.
		bool detached_root = false;
		for (const WhenlineDiffNode::Op &op : script.neg_ops) {
			if (op.kind == WhenlineDiffNode::OP_DETACH && op.node == a) {
				detached_root = true;
			}
		}
		CHECK(detached_root);

		diff.free_owned_clones();
		memdelete(a);
		memdelete(b);
	}
}

} // namespace TestTrueDiff
