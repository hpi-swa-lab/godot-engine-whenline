/**************************************************************************/
/*  truediff.h                                                            */
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

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// C++ port of the TrueDiff tree-diffing algorithm.
//
// The algorithm matches subtrees between an "old" tree (a) and a "new" tree (b)
// in three phases:
//
//   1. assignShares: top-down, group nodes that have the same `structureHash`
//      (type + child structure) into a shared "share" object. Already-equal
//      subtrees (identical `literalHash`) are matched directly.
//   2. assignSubtrees: bottom-up, walk b's nodes by descending tree height,
//      pulling the best available counterpart from a out of each share.
//   3. computeEditScript: produce a sequence of detach/attach/update/load/
//      remove ops describing how to turn a into b.
//
// The original JavaScript implementation lives in `truediff/diff.js` at the
// project root; this is a faithful, readable port.
//
// To diff your own AST, subclass `WhenlineDiffNode`, fill in the children
// vector during construction and implement `_compute_local_hashes` (which
// must populate `structure_local_hash` and `literal_local_hash` from the
// node's own type/text fields). After all subtrees are constructed, call
// `WhenlineDiffNode::compute_hashes(root)` once before passing it to
// `TrueDiff::detect_edits(...)`.

class WhenlineDiffNode {
public:
	enum OpKind {
		OP_ATTACH,
		OP_DETACH,
		OP_UPDATE,
		OP_LOAD,
		OP_REMOVE,
	};

	// One diff operation. Mirrors the `DiffOp` subclasses from the JS.
	struct Op {
		OpKind kind = OP_ATTACH;

		WhenlineDiffNode *node = nullptr;
		// For OP_ATTACH: where the node should be attached.
		// For OP_DETACH: the node's old parent (recorded when the op is created).
		WhenlineDiffNode *parent = nullptr;
		int index = -1;
		// For OP_UPDATE: the new and old text of the leaf node.
		String new_text;
		String old_text;
	};

	// Public, identity-only attributes.
	int id = 0;
	int sibling_index = 0;
	WhenlineDiffNode *parent = nullptr;

	// Hashing. Subclasses must compute the *_local_hash fields in
	// `_compute_local_hashes` (called once during `compute_hashes`).
	uint32_t structure_local_hash = 0; // hashes only the node's own type
	uint32_t literal_local_hash = 0; // hashes type + text/value for leaves
	uint32_t structure_hash = 0; // structural hash including children
	uint32_t literal_hash = 0; // literal hash including children
	int tree_height = 1;
	int subtree_size = 1;

	// Diff state, owned by TrueDiff during a single detect_edits() call.
	struct SubtreeShare *share = nullptr;
	WhenlineDiffNode *assigned = nullptr;
	bool literal_match = false;

	// Children. Subclasses populate this.
	LocalVector<WhenlineDiffNode *> children;

	WhenlineDiffNode() = default;
	virtual ~WhenlineDiffNode();

	// Whether the node is a "text" leaf (carries text content rather than a
	// structural type). Used by `update_literals` to emit `UpdateOp`s.
	virtual bool is_text() const { return false; }
	virtual String get_text() const { return String(); }
	virtual StringName get_type_name() const { return StringName(); }

	// Used by the diff to construct nodes representing newly inserted subtrees.
	// The returned node must be a node of the same dynamic type, with the same
	// `is_text`/`get_type_name`/`get_text` semantics, but with an empty
	// `children` vector and no parent. Hashes will be re-copied by the diff.
	virtual WhenlineDiffNode *shallow_clone() const = 0;

	// Subclasses fill in `structure_local_hash` and `literal_local_hash`.
	virtual void _compute_local_hashes() = 0;

	// Recursively compute the full structural and literal hashes plus
	// `tree_height` and `subtree_size`, and assign per-tree node IDs and
	// parent/sibling links. Call once on the root before diffing.
	static void compute_hashes(WhenlineDiffNode *p_root);

	// Helpers used internally by the diff and by tests.
	template <typename F>
	void for_each_node(F p_cb) {
		p_cb(this);
		for (WhenlineDiffNode *c : children) {
			c->for_each_node(p_cb);
		}
	}
	template <typename F>
	void for_each_child(F p_cb) {
		for (WhenlineDiffNode *c : children) {
			c->for_each_node(p_cb);
		}
	}

	// True if this node equals `p_other` or has it as an ancestor.
	bool or_has_ancestor(const WhenlineDiffNode *p_other) const;

	// Debug pretty-printer used by tests and developers.
	String print(int p_indent = 0) const;
};

// Sequence of edit operations describing how to turn the "old" tree into the
// "new" tree.
class WhenlineEditScript {
public:
	// Operations are ordered as: first all "negative" ops (DETACH/REMOVE),
	// then all "positive" ops (LOAD/ATTACH/UPDATE), matching the original
	// algorithm's apply order.
	LocalVector<WhenlineDiffNode::Op> neg_ops;
	LocalVector<WhenlineDiffNode::Op> pos_ops;

	bool is_empty() const { return neg_ops.is_empty() && pos_ops.is_empty(); }
	int size() const { return int(neg_ops.size() + pos_ops.size()); }

	// Convenience: collect every node mentioned by a positive op (Update/Attach)
	// or by a Detach whose old parent still has a connected ancestor in the new
	// tree. Useful for "which lines were touched by this diff".
	void collect_touched_nodes(Vector<WhenlineDiffNode *> &r_out) const;
};

// Forward declarations of the per-diff bookkeeping types. They are only used
// internally but exposed in the header so consumers can reuse them when
// extending TrueDiff (e.g. to add new diff op kinds).
struct SubtreeShare;
struct SubtreeRegistry;

class TrueDiff {
public:
	// Compute the edit script that transforms `p_a` into `p_b`. Both trees are
	// expected to have already been hashed via `WhenlineDiffNode::compute_hashes`.
	// The diff temporarily sets `share`/`assigned` fields on nodes; they are
	// always cleaned up before this call returns.
	WhenlineEditScript detect_edits(WhenlineDiffNode *p_a, WhenlineDiffNode *p_b);

private:
	// Phase 1.
	void assign_shares(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry);
	void assign_shares_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry);
	void assign_shares_list(LocalVector<WhenlineDiffNode *> &a_list, LocalVector<WhenlineDiffNode *> &b_list, SubtreeRegistry &r_registry, bool p_match_literal);
	void assign_tree(WhenlineDiffNode *a, WhenlineDiffNode *b, bool p_literal_match);
	void assign_tree_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b);

	// Phase 2.
	void assign_subtrees(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry);
	void select_available_tree(LocalVector<WhenlineDiffNode *> &r_unassigned, bool p_literal_match, SubtreeRegistry &r_registry, LocalVector<WhenlineDiffNode *> &r_remaining);

	// Phase 3.
	WhenlineDiffNode *compute_edit_script(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineDiffNode *p_parent, int p_link, WhenlineEditScript &r_buf);
	WhenlineDiffNode *compute_edit_script_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineEditScript &r_buf);
	void compute_edit_script_list(LocalVector<WhenlineDiffNode *> &a_list, LocalVector<WhenlineDiffNode *> &b_list, WhenlineDiffNode *p_parent, WhenlineEditScript &r_buf);
	WhenlineDiffNode *update_literals(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineEditScript &r_buf);
	void unload_unassigned(WhenlineDiffNode *a, WhenlineEditScript &r_buf);
	WhenlineDiffNode *load_unassigned(WhenlineDiffNode *b, WhenlineEditScript &r_buf, LocalVector<WhenlineDiffNode *> &r_owned_clones);

	// Track clones that we synthesize during `load_unassigned` so we can free
	// them after the diff has been consumed by the caller.
	LocalVector<WhenlineDiffNode *> _owned_clones;

public:
	// Free any nodes synthesized by `detect_edits` (clones for inserted
	// subtrees). Call after you've finished inspecting the script and no
	// longer need the new-tree side.
	void free_owned_clones();
};
