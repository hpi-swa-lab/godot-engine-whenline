/**************************************************************************/
/*  truediff.cpp                                                          */
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

#include "truediff.h"

#include "core/error/error_macros.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

// =============================================================================
// SubtreeShare / SubtreeRegistry
// =============================================================================

// A "share" groups all trees with the same `structure_hash`. Trees from `a`
// (the old tree) that haven't been matched yet are tracked here as
// `available_trees`. Trees with the same `literal_hash` (i.e. structurally and
// textually identical) are looked up in O(1) via `preferred_trees`.
struct SubtreeShare {
	// Insertion order matters when no preferred match exists: we pick the
	// "first" available tree, which mirrors the JS `[...availableTrees.values()][0]`.
	HashMap<WhenlineDiffNode *, WhenlineDiffNode *> available_trees;
	HashMap<uint32_t, WhenlineDiffNode *> preferred_trees;
	bool has_preferred_trees_built = false;

	void register_available_tree(WhenlineDiffNode *p_node) {
		available_trees.insert(p_node, p_node);
	}

	void deregister_available_tree(WhenlineDiffNode *p_node, SubtreeRegistry &r_registry);

	void unassign_tree(WhenlineDiffNode *p_node) {
		// Mirrors `a.assigned.assigned = null; a.assigned = null;`.
		p_node->assigned->assigned = nullptr;
		p_node->assigned = nullptr;
	}

	void build_preferred_trees() {
		if (has_preferred_trees_built) {
			return;
		}
		has_preferred_trees_built = true;
		for (const KeyValue<WhenlineDiffNode *, WhenlineDiffNode *> &kv : available_trees) {
			// First insertion wins, like the JS implementation (Map.set
			// overrides, but since we iterate in insertion order and only
			// keep the first, we explicitly skip duplicates here so the
			// "preferred" pick is stable).
			if (!preferred_trees.has(kv.value->literal_hash)) {
				preferred_trees.insert(kv.value->literal_hash, kv.value);
			}
		}
	}

	WhenlineDiffNode *take_available_tree(WhenlineDiffNode *p_b, bool p_take_preferred, SubtreeRegistry &r_registry);
	void take_tree(WhenlineDiffNode *p_a, WhenlineDiffNode *p_b, SubtreeRegistry &r_registry);
};

struct SubtreeRegistry {
	// Owns all SubtreeShare instances created during a single diff so they
	// are destroyed exactly once when the diff returns.
	HashMap<uint32_t, SubtreeShare *> subtrees;

	~SubtreeRegistry() {
		for (KeyValue<uint32_t, SubtreeShare *> &kv : subtrees) {
			memdelete(kv.value);
		}
	}

	SubtreeShare *assign_share(WhenlineDiffNode *p_node) {
		p_node->assigned = nullptr;
		HashMap<uint32_t, SubtreeShare *>::Iterator it = subtrees.find(p_node->structure_hash);
		if (it) {
			p_node->share = it->value;
		} else {
			SubtreeShare *share = memnew(SubtreeShare);
			subtrees.insert(p_node->structure_hash, share);
			p_node->share = share;
		}
		return p_node->share;
	}

	SubtreeShare *assign_share_and_register_tree(WhenlineDiffNode *p_node) {
		SubtreeShare *share = assign_share(p_node);
		share->register_available_tree(p_node);
		return share;
	}
};

void SubtreeShare::deregister_available_tree(WhenlineDiffNode *p_node, SubtreeRegistry &r_registry) {
	if (p_node->share) {
		// Walk down the still-available subtree and detach it from its share.
		p_node->share->available_trees.erase(p_node);
		p_node->share = nullptr;
		for (WhenlineDiffNode *child : p_node->children) {
			deregister_available_tree(child, r_registry);
		}
	} else {
		// Already pulled out (assigned). Re-register all of its assigned
		// counterpart's children as fresh shares so they can be matched again.
		if (p_node->assigned) {
			WhenlineDiffNode *b = p_node->assigned;
			unassign_tree(p_node);
			b->for_each_node([&](WhenlineDiffNode *n) {
				r_registry.assign_share(n);
			});
		}
	}
}

WhenlineDiffNode *SubtreeShare::take_available_tree(WhenlineDiffNode *p_b, bool p_take_preferred, SubtreeRegistry &r_registry) {
	WhenlineDiffNode *a = nullptr;
	if (p_take_preferred) {
		build_preferred_trees();
		HashMap<uint32_t, WhenlineDiffNode *>::Iterator it = preferred_trees.find(p_b->literal_hash);
		if (it) {
			a = it->value;
		}
	} else if (!available_trees.is_empty()) {
		// "Pick any". HashMap is insertion-ordered, so this picks the oldest
		// remaining tree, matching the JS `[...availableTrees.values()][0]`.
		a = available_trees.begin()->value;
	}
	if (a) {
		take_tree(a, p_b, r_registry);
	}
	return a;
}

void SubtreeShare::take_tree(WhenlineDiffNode *p_a, WhenlineDiffNode *p_b, SubtreeRegistry &r_registry) {
	p_a->share->available_trees.erase(p_a);
	if (p_a->share->has_preferred_trees_built) {
		p_a->share->preferred_trees.erase(p_b->literal_hash);
	}
	p_a->share = nullptr;
	for (WhenlineDiffNode *child : p_a->children) {
		deregister_available_tree(child, r_registry);
	}
	// Any descendants of b that were previously assigned must be put back into
	// the registry as available, since b itself just snapped onto a.
	p_b->for_each_child([&](WhenlineDiffNode *n) {
		if (n->assigned) {
			r_registry.assign_share_and_register_tree(n->assigned);
		}
	});
}

// =============================================================================
// WhenlineDiffNode
// =============================================================================

WhenlineDiffNode::~WhenlineDiffNode() {
	for (WhenlineDiffNode *c : children) {
		memdelete(c);
	}
}

bool WhenlineDiffNode::or_has_ancestor(const WhenlineDiffNode *p_other) const {
	const WhenlineDiffNode *cur = this;
	while (cur) {
		if (cur == p_other) {
			return true;
		}
		cur = cur->parent;
	}
	return false;
}

static void _assign_ids_and_parents(WhenlineDiffNode *p_node, WhenlineDiffNode *p_parent, int p_sibling_index, int &r_next_id) {
	p_node->id = r_next_id++;
	p_node->parent = p_parent;
	p_node->sibling_index = p_sibling_index;
	for (uint32_t i = 0; i < p_node->children.size(); i++) {
		_assign_ids_and_parents(p_node->children[i], p_node, int(i), r_next_id);
	}
}

static void _compute_hashes_recursive(WhenlineDiffNode *p_node) {
	p_node->_compute_local_hashes();
	uint32_t s = p_node->structure_local_hash;
	uint32_t l = p_node->literal_local_hash;
	int height = 0;
	int size = 1;
	for (WhenlineDiffNode *c : p_node->children) {
		_compute_hashes_recursive(c);
		s = hash_murmur3_one_32(c->structure_hash, s);
		l = hash_murmur3_one_32(c->literal_hash, l);
		if (c->tree_height > height) {
			height = c->tree_height;
		}
		size += c->subtree_size;
	}
	p_node->structure_hash = s;
	p_node->literal_hash = l;
	p_node->tree_height = height + 1;
	p_node->subtree_size = size;
}

void WhenlineDiffNode::compute_hashes(WhenlineDiffNode *p_root) {
	int next_id = 1;
	_assign_ids_and_parents(p_root, nullptr, 0, next_id);
	_compute_hashes_recursive(p_root);
}

String WhenlineDiffNode::print(int p_indent) const {
	String pad = String("  ").repeat(p_indent);
	String out = pad;
	if (is_text()) {
		out += "\"" + get_text().replace("\n", "\\n") + "\"";
	} else {
		out += String(get_type_name());
	}
	out += " #" + itos(id);
	out += "\n";
	for (WhenlineDiffNode *c : children) {
		out += c->print(p_indent + 1);
	}
	return out;
}

// =============================================================================
// WhenlineEditScript
// =============================================================================

void WhenlineEditScript::collect_touched_nodes(Vector<WhenlineDiffNode *> &r_out) const {
	for (const WhenlineDiffNode::Op &op : pos_ops) {
		if (op.kind == WhenlineDiffNode::OP_UPDATE || op.kind == WhenlineDiffNode::OP_ATTACH) {
			r_out.push_back(op.node);
		}
	}
	for (const WhenlineDiffNode::Op &op : neg_ops) {
		if (op.kind == WhenlineDiffNode::OP_DETACH && op.parent) {
			// "Old parent" — only meaningful if it's still in the tree.
			r_out.push_back(op.parent);
		}
	}
}

// =============================================================================
// TrueDiff
// =============================================================================

WhenlineEditScript TrueDiff::detect_edits(WhenlineDiffNode *p_a, WhenlineDiffNode *p_b) {
	ERR_FAIL_NULL_V(p_a, WhenlineEditScript());
	ERR_FAIL_NULL_V(p_b, WhenlineEditScript());

	_owned_clones.clear();

	SubtreeRegistry registry;
	assign_shares(p_a, p_b, registry);
	assign_subtrees(p_a, p_b, registry);

	WhenlineEditScript script;
	compute_edit_script(p_a, p_b, nullptr, 0, script);

	// At the end of the run, every node should have its `share`/`assigned`
	// fields cleared (the algorithm drops them as it produces ops). We don't
	// rely on this for correctness, but it keeps the trees in a clean state.
	return script;
}

void TrueDiff::free_owned_clones() {
	for (WhenlineDiffNode *c : _owned_clones) {
		// Clones never have children of their own (children are siblings owned
		// by the original new-tree side or by other clones; we attach them
		// without taking ownership). We clear the children list before delete
		// so the destructor doesn't double-free.
		c->children.clear();
		memdelete(c);
	}
	_owned_clones.clear();
}

// ---------- Phase 1: assign shares ------------------------------------------

void TrueDiff::assign_shares(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry) {
	SubtreeShare *a_share = r_registry.assign_share(a);
	SubtreeShare *b_share = r_registry.assign_share(b);
	if (a_share == b_share && a->literal_hash == b->literal_hash) {
		assign_tree(a, b, true);
	} else {
		assign_shares_recurse(a, b, r_registry);
	}
}

void TrueDiff::assign_tree(WhenlineDiffNode *a, WhenlineDiffNode *b, bool p_literal_match) {
	a->share = nullptr;
	a->literal_match = p_literal_match;
	if (p_literal_match) {
		a->assigned = b;
		b->assigned = a;
	} else {
		assign_tree_recurse(a, b);
	}
}

void TrueDiff::assign_tree_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b) {
	a->assigned = b;
	b->assigned = a;
	// Both children arrays must have the same length (same structure_hash).
	for (uint32_t i = 0; i < a->children.size(); i++) {
		assign_tree_recurse(a->children[i], b->children[i]);
	}
}

static void _reverse_in_place(LocalVector<WhenlineDiffNode *> &r_v) {
	const uint32_t n = r_v.size();
	for (uint32_t i = 0; i < n / 2; i++) {
		SWAP(r_v[i], r_v[n - 1 - i]);
	}
}

void TrueDiff::assign_shares_list(LocalVector<WhenlineDiffNode *> &a_list, LocalVector<WhenlineDiffNode *> &b_list, SubtreeRegistry &r_registry, bool p_match_literal) {
	while (!a_list.is_empty() && !b_list.is_empty()) {
		WhenlineDiffNode *a = a_list[0];
		WhenlineDiffNode *b = b_list[0];
		SubtreeShare *a_share = r_registry.assign_share(a);
		SubtreeShare *b_share = r_registry.assign_share(b);
		if (a_share == b_share && (!p_match_literal || a->literal_hash == b->literal_hash)) {
			a_list.remove_at(0);
			b_list.remove_at(0);
			assign_tree(a, b, false);
		} else {
			break;
		}
	}
}

void TrueDiff::assign_shares_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry) {
	if (a->get_type_name() == b->get_type_name()) {
		LocalVector<WhenlineDiffNode *> a_list;
		LocalVector<WhenlineDiffNode *> b_list;
		a_list.reserve(a->children.size());
		b_list.reserve(b->children.size());
		for (WhenlineDiffNode *c : a->children) {
			a_list.push_back(c);
		}
		for (WhenlineDiffNode *c : b->children) {
			b_list.push_back(c);
		}

		// Walk children from both ends, matching literal-equal ones first,
		// then structurally-equal ones, and finally falling back to recursion.
		assign_shares_list(a_list, b_list, r_registry, true);
		_reverse_in_place(a_list);
		_reverse_in_place(b_list);
		assign_shares_list(a_list, b_list, r_registry, true);
		// Note: a_list/b_list are already reversed, so this run goes from the
		// "back" of the original. We reverse again to restore the original
		// orientation before the structural pass.
		_reverse_in_place(a_list);
		_reverse_in_place(b_list);
		assign_shares_list(a_list, b_list, r_registry, false);
		_reverse_in_place(a_list);
		_reverse_in_place(b_list);
		assign_shares_list(a_list, b_list, r_registry, false);
		_reverse_in_place(a_list);
		_reverse_in_place(b_list);

		// At this point the shared prefixes/suffixes are paired up; recurse
		// pairwise on what remains and unload the rest.
		const uint32_t max_len = MAX(a_list.size(), b_list.size());
		for (uint32_t i = 0; i < max_len; i++) {
			WhenlineDiffNode *ac = i < a_list.size() ? a_list[i] : nullptr;
			WhenlineDiffNode *bc = i < b_list.size() ? b_list[i] : nullptr;
			if (ac && bc) {
				r_registry.assign_share_and_register_tree(ac);
				r_registry.assign_share(bc);
				assign_shares_recurse(ac, bc, r_registry);
			} else if (ac) {
				ac->for_each_node([&](WhenlineDiffNode *n) { r_registry.assign_share_and_register_tree(n); });
			} else if (bc) {
				bc->for_each_node([&](WhenlineDiffNode *n) { r_registry.assign_share(n); });
			}
		}
	} else {
		a->for_each_child([&](WhenlineDiffNode *n) { r_registry.assign_share_and_register_tree(n); });
		b->for_each_child([&](WhenlineDiffNode *n) { r_registry.assign_share(n); });
	}
}

// ---------- Phase 2: assign subtrees ----------------------------------------

void TrueDiff::assign_subtrees(WhenlineDiffNode *a, WhenlineDiffNode *b, SubtreeRegistry &r_registry) {
	(void)a;
	// Process nodes from b in descending tree-height order. For each height
	// level, try to find a counterpart in a's available trees.
	// We use a simple unordered list and re-scan it each pass for the current
	// max height. This is O(n^2) in the worst case but trivial to read; the
	// trees we diff are small enough that it doesn't matter in practice, and
	// we can swap in a heap later without changing the algorithm.
	LocalVector<WhenlineDiffNode *> queue;
	queue.push_back(b);

	while (!queue.is_empty()) {
		// Find the current max tree_height.
		int level = -1;
		for (WhenlineDiffNode *n : queue) {
			if (n->tree_height > level) {
				level = n->tree_height;
			}
		}

		// Pull every node at that level out of the queue.
		LocalVector<WhenlineDiffNode *> next_nodes;
		LocalVector<WhenlineDiffNode *> kept;
		kept.reserve(queue.size());
		for (WhenlineDiffNode *n : queue) {
			if (n->tree_height == level) {
				if (!n->assigned) {
					next_nodes.push_back(n);
				}
			} else {
				kept.push_back(n);
			}
		}
		queue = kept;

		LocalVector<WhenlineDiffNode *> remaining;
		select_available_tree(next_nodes, true, r_registry, remaining);
		next_nodes = remaining;
		remaining.clear();
		select_available_tree(next_nodes, false, r_registry, remaining);
		next_nodes = remaining;

		// Anything still unassigned: descend into its children for the next
		// iteration of the while loop.
		for (WhenlineDiffNode *n : next_nodes) {
			for (WhenlineDiffNode *c : n->children) {
				queue.push_back(c);
			}
		}
	}
}

void TrueDiff::select_available_tree(LocalVector<WhenlineDiffNode *> &r_unassigned, bool p_literal_match, SubtreeRegistry &r_registry, LocalVector<WhenlineDiffNode *> &r_remaining) {
	for (WhenlineDiffNode *b : r_unassigned) {
		if (b->assigned) {
			continue;
		}
		WhenlineDiffNode *a = b->share->take_available_tree(b, p_literal_match, r_registry);
		if (a) {
			assign_tree(a, b, p_literal_match);
		} else {
			r_remaining.push_back(b);
		}
	}
}

// ---------- Phase 3: compute edit script ------------------------------------

// Walk a pair of literally-equal subtrees and record every (old, new)
// descendant pair into the edit script. We do this for the literal-match
// fast path because `update_literals` is not called there (there's nothing
// to update), yet we still want consumers to know about the pairing so
// they can preserve per-line state across hot-reloads.
static void _record_literal_match_subtree(WhenlineDiffNode *p_a, WhenlineDiffNode *p_b, WhenlineEditScript &r_buf) {
	WhenlineEditScript::Match match;
	match.old_node = p_a;
	match.new_node = p_b;
	match.literal = true;
	r_buf.matches.push_back(match);
	for (uint32_t i = 0; i < p_a->children.size() && i < p_b->children.size(); i++) {
		_record_literal_match_subtree(p_a->children[i], p_b->children[i], r_buf);
	}
}

WhenlineDiffNode *TrueDiff::compute_edit_script(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineDiffNode *p_parent, int p_link, WhenlineEditScript &r_buf) {
	// Case 1: a was assigned to b directly. If they're a literal match the
	// subtree is unchanged; otherwise update the literal leaves.
	if (a->assigned && a->assigned == b) {
		WhenlineDiffNode *new_tree;
		if (a->literal_match) {
			_record_literal_match_subtree(a, b, r_buf);
			new_tree = a;
		} else {
			new_tree = update_literals(a, b, r_buf);
		}
		a->assigned = nullptr;
		return new_tree;
	}

	// Case 2: neither side is matched. Try to pair their children up.
	if (!a->assigned && !b->assigned) {
		WhenlineDiffNode *new_tree = compute_edit_script_recurse(a, b, r_buf);
		if (new_tree) {
			return new_tree;
		}
	}

	// Case 3: same type, neither side matched — rebuild the children list.
	if (a->get_type_name() == b->get_type_name() && !a->assigned && !b->assigned) {
		for (WhenlineDiffNode *child : a->children) {
			WhenlineDiffNode::Op op;
			op.kind = WhenlineDiffNode::OP_DETACH;
			op.node = child;
			op.parent = child->parent;
			r_buf.neg_ops.push_back(op);
			unload_unassigned(child, r_buf);
		}
		int index = 0;
		for (WhenlineDiffNode *child : b->children) {
			WhenlineDiffNode *new_tree = load_unassigned(child, r_buf, _owned_clones);
			WhenlineDiffNode::Op op;
			op.kind = WhenlineDiffNode::OP_ATTACH;
			op.node = new_tree;
			op.parent = a;
			op.index = index++;
			r_buf.pos_ops.push_back(op);
		}
		return a;
	}

	// Case 4: full replacement.
	{
		WhenlineDiffNode::Op detach_op;
		detach_op.kind = WhenlineDiffNode::OP_DETACH;
		detach_op.node = a;
		detach_op.parent = a->parent;
		r_buf.neg_ops.push_back(detach_op);

		unload_unassigned(a, r_buf);
		WhenlineDiffNode *new_tree = load_unassigned(b, r_buf, _owned_clones);

		WhenlineDiffNode::Op attach_op;
		attach_op.kind = WhenlineDiffNode::OP_ATTACH;
		attach_op.node = new_tree;
		attach_op.parent = p_parent;
		attach_op.index = p_link;
		r_buf.pos_ops.push_back(attach_op);
		return new_tree;
	}
}

WhenlineDiffNode *TrueDiff::compute_edit_script_recurse(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineEditScript &r_buf) {
	if (a->get_type_name() != b->get_type_name()) {
		return nullptr;
	}

	LocalVector<WhenlineDiffNode *> a_list;
	LocalVector<WhenlineDiffNode *> b_list;
	a_list.reserve(a->children.size());
	b_list.reserve(b->children.size());
	for (WhenlineDiffNode *c : a->children) {
		a_list.push_back(c);
	}
	for (WhenlineDiffNode *c : b->children) {
		b_list.push_back(c);
	}

	// Match shared prefixes and suffixes of children that are already paired
	// up by `assigned`.
	compute_edit_script_list(a_list, b_list, a, r_buf);
	_reverse_in_place(a_list);
	_reverse_in_place(b_list);
	compute_edit_script_list(a_list, b_list, a, r_buf);
	_reverse_in_place(a_list);
	_reverse_in_place(b_list);

	// Walk what's left and rebuild it. We use the original sibling indices
	// from b for attach positions.
	const uint32_t max_len = MAX(a_list.size(), b_list.size());
	for (uint32_t i = 0; i < max_len; i++) {
		WhenlineDiffNode *a_child = i < a_list.size() ? a_list[i] : nullptr;
		WhenlineDiffNode *b_child = i < b_list.size() ? b_list[i] : nullptr;

		if (a_child && a_child->assigned && a_child->assigned == b_child) {
			update_literals(a_child, b_child, r_buf);
			a_child->assigned = nullptr;
			continue;
		}
		if (a_child && b_child &&
				a_child->get_type_name() == b_child->get_type_name() &&
				!a_child->assigned && !b_child->assigned) {
			compute_edit_script_recurse(a_child, b_child, r_buf);
			continue;
		}
		if (a_child) {
			WhenlineDiffNode::Op op;
			op.kind = WhenlineDiffNode::OP_DETACH;
			op.node = a_child;
			op.parent = a_child->parent;
			r_buf.neg_ops.push_back(op);
			unload_unassigned(a_child, r_buf);
		}
		if (b_child) {
			WhenlineDiffNode *new_tree = load_unassigned(b_child, r_buf, _owned_clones);
			WhenlineDiffNode::Op op;
			op.kind = WhenlineDiffNode::OP_ATTACH;
			op.node = new_tree;
			op.parent = a;
			op.index = b_child->sibling_index;
			r_buf.pos_ops.push_back(op);
		}
	}
	return a;
}

void TrueDiff::compute_edit_script_list(LocalVector<WhenlineDiffNode *> &a_list, LocalVector<WhenlineDiffNode *> &b_list, WhenlineDiffNode *p_parent, WhenlineEditScript &r_buf) {
	while (!a_list.is_empty() && !b_list.is_empty()) {
		WhenlineDiffNode *a = a_list[0];
		WhenlineDiffNode *b = b_list[0];
		if (a->assigned == b) {
			a_list.remove_at(0);
			b_list.remove_at(0);
			compute_edit_script(a, b, p_parent, b->sibling_index, r_buf);
		} else {
			break;
		}
	}
}

WhenlineDiffNode *TrueDiff::update_literals(WhenlineDiffNode *a, WhenlineDiffNode *b, WhenlineEditScript &r_buf) {
	// Every call to `update_literals` corresponds to a matched node pair: by
	// the time we get here the algorithm has decided `a` (old) and `b` (new)
	// are the same subtree, modulo possibly-different text on leaves. Record
	// the pair so consumers can derive an old-line → new-line mapping for
	// out-of-band per-line state (e.g. execution counters).
	WhenlineEditScript::Match match;
	match.old_node = a;
	match.new_node = b;
	match.literal = !(a->is_text() && b->is_text() && a->get_text() != b->get_text());
	r_buf.matches.push_back(match);

	if (a->is_text() && b->is_text() && a->get_text() != b->get_text()) {
		WhenlineDiffNode::Op op;
		op.kind = WhenlineDiffNode::OP_UPDATE;
		op.node = a;
		op.paired_node = b;
		op.new_text = b->get_text();
		op.old_text = a->get_text();
		r_buf.pos_ops.push_back(op);
	}
	for (uint32_t i = 0; i < a->children.size() && i < b->children.size(); i++) {
		update_literals(a->children[i], b->children[i], r_buf);
	}
	return a;
}

void TrueDiff::unload_unassigned(WhenlineDiffNode *a, WhenlineEditScript &r_buf) {
	if (a->assigned) {
		a->assigned = nullptr;
		return;
	}
	WhenlineDiffNode::Op op;
	op.kind = WhenlineDiffNode::OP_REMOVE;
	op.node = a;
	r_buf.neg_ops.push_back(op);
	for (WhenlineDiffNode *c : a->children) {
		unload_unassigned(c, r_buf);
	}
}

WhenlineDiffNode *TrueDiff::load_unassigned(WhenlineDiffNode *b, WhenlineEditScript &r_buf, LocalVector<WhenlineDiffNode *> &r_owned_clones) {
	if (b->assigned) {
		WhenlineDiffNode *tree = update_literals(b->assigned, b, r_buf);
		b->assigned->assigned = nullptr;
		b->assigned = nullptr;
		return tree;
	}
	WhenlineDiffNode *new_tree = b->shallow_clone();
	new_tree->id = b->id;
	new_tree->sibling_index = b->sibling_index;
	r_owned_clones.push_back(new_tree);

	WhenlineDiffNode::Op load_op;
	load_op.kind = WhenlineDiffNode::OP_LOAD;
	load_op.node = new_tree;
	r_buf.pos_ops.push_back(load_op);

	for (uint32_t i = 0; i < b->children.size(); i++) {
		WhenlineDiffNode *new_child = load_unassigned(b->children[i], r_buf, r_owned_clones);
		WhenlineDiffNode::Op attach_op;
		attach_op.kind = WhenlineDiffNode::OP_ATTACH;
		attach_op.node = new_child;
		attach_op.parent = new_tree;
		attach_op.index = int(i);
		r_buf.pos_ops.push_back(attach_op);
	}
	return new_tree;
}
