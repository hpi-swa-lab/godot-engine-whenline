/**************************************************************************/
/*  gdscript_diff_node.cpp                                                */
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

#include "gdscript_diff_node.h"

#include "core/templates/hashfuncs.h"

using GP = GDScriptParser;

// =============================================================================
// GDScriptDiffNode
// =============================================================================

StringName GDScriptDiffNode::get_type_name() const {
	// Build a stable type-name. We never display this string to end users;
	// it's used by `assign_shares_recurse` to compare structural roles, and
	// fed into the structural hash.
	switch (kind) {
		case KIND_IDENTIFIER_TEXT:
			return SNAME("text:identifier");
		case KIND_LITERAL_TEXT:
			return SNAME("text:literal");
		case KIND_OPERATOR_TEXT:
			return SNAME("text:operator");
		case KIND_STRUCTURAL:
			break;
	}

	// Reuse parser-type IDs as the structural type name. We just stringify
	// the integer; the value is only used for hashing/equality, never display.
	return StringName("gd:" + itos(int(parser_type)));
}

WhenlineDiffNode *GDScriptDiffNode::shallow_clone() const {
	return memnew(GDScriptDiffNode(kind, parser_type, text_value, parser_start_line, parser_end_line));
}

void GDScriptDiffNode::_compute_local_hashes() {
	uint32_t s = hash_murmur3_one_32(uint32_t(kind));
	s = hash_murmur3_one_32(uint32_t(parser_type), s);
	structure_local_hash = s;
	uint32_t l = s;
	if (is_text()) {
		l = hash_murmur3_one_32(text_value.hash(), l);
	}
	literal_local_hash = l;
}

// =============================================================================
// AST visitor
// =============================================================================

namespace {

// Forward declarations of the per-kind builders. Every builder takes a parser
// node and returns the wrapper (or nullptr if there's nothing meaningful to
// represent — currently never returned).
GDScriptDiffNode *_visit(const GP::Node *p_node);

GDScriptDiffNode *_make_struct(const GP::Node *p_node) {
	return memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_STRUCTURAL, p_node->type, String(), p_node->start_line, p_node->end_line));
}

void _add_child(GDScriptDiffNode *p_parent, GDScriptDiffNode *p_child) {
	if (p_child) {
		p_parent->children.push_back(p_child);
	}
}

GDScriptDiffNode *_make_identifier_leaf(const GP::IdentifierNode *p_id) {
	if (!p_id) {
		return nullptr;
	}
	return memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_IDENTIFIER_TEXT, GP::Node::IDENTIFIER, String(p_id->name), p_id->start_line, p_id->end_line));
}

GDScriptDiffNode *_make_literal_leaf(const GP::LiteralNode *p_lit) {
	if (!p_lit) {
		return nullptr;
	}
	// Stringify the literal value. For most types `String(value)` round-trips
	// well enough that two literals with the same source spell hash the same;
	// for floats, integers and booleans this is exact.
	String text = String(p_lit->value);
	return memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_LITERAL_TEXT, GP::Node::LITERAL, text, p_lit->start_line, p_lit->end_line));
}

GDScriptDiffNode *_make_operator_leaf(int p_op_value, GP::Node::Type p_parser_type, int p_line) {
	// Operators are a distinct synthetic leaf kind so two binary ops with
	// the same operands but different operators (e.g. `a + b` vs `a - b`)
	// don't accidentally get matched as literal-equal subtrees.
	return memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_OPERATOR_TEXT, p_parser_type, "op:" + itos(p_op_value), p_line, p_line));
}

// ---- Per-parser-node visitors ----------------------------------------------
//
// Convention: every visitor returns a structural wrapper for the parser node
// and adds children in source order, skipping null/empty optional slots.

GDScriptDiffNode *_visit_class(const GP::ClassNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	// Class identifier (anonymous classes have none).
	_add_child(out, _make_identifier_leaf(p_n->identifier));
	// `extends` chain (each entry is an identifier).
	for (int i = 0; i < p_n->extends.size(); i++) {
		_add_child(out, _make_identifier_leaf(p_n->extends[i]));
	}
	// Members in declaration order, matching `members` for source line
	// alignment. We dispatch through the `Member`'s `get_source_node()` so
	// each member maps to its original AST root (variable/function/etc.).
	for (int i = 0; i < p_n->members.size(); i++) {
		const GP::ClassNode::Member &m = p_n->members[i];
		const GP::Node *src = m.get_source_node();
		if (src) {
			_add_child(out, _visit(src));
		}
	}
	return out;
}

GDScriptDiffNode *_visit_suite(const GP::SuiteNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	for (int i = 0; i < p_n->statements.size(); i++) {
		_add_child(out, _visit(p_n->statements[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_function(const GP::FunctionNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_identifier_leaf(p_n->identifier));
	for (int i = 0; i < p_n->parameters.size(); i++) {
		_add_child(out, _visit(p_n->parameters[i]));
	}
	if (p_n->rest_parameter) {
		_add_child(out, _visit(p_n->rest_parameter));
	}
	if (p_n->return_type) {
		_add_child(out, _visit(p_n->return_type));
	}
	if (p_n->body) {
		_add_child(out, _visit(p_n->body));
	}
	return out;
}

GDScriptDiffNode *_visit_assignable(const GP::AssignableNode *p_n) {
	// Common children for VariableNode/ConstantNode/ParameterNode.
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_identifier_leaf(p_n->identifier));
	if (p_n->datatype_specifier) {
		_add_child(out, _visit(p_n->datatype_specifier));
	}
	if (p_n->initializer) {
		_add_child(out, _visit(p_n->initializer));
	}
	return out;
}

GDScriptDiffNode *_visit_if(const GP::IfNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->condition) {
		_add_child(out, _visit(p_n->condition));
	}
	if (p_n->true_block) {
		_add_child(out, _visit(p_n->true_block));
	}
	if (p_n->false_block) {
		_add_child(out, _visit(p_n->false_block));
	}
	return out;
}

GDScriptDiffNode *_visit_for(const GP::ForNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_identifier_leaf(p_n->variable));
	if (p_n->datatype_specifier) {
		_add_child(out, _visit(p_n->datatype_specifier));
	}
	if (p_n->list) {
		_add_child(out, _visit(p_n->list));
	}
	if (p_n->loop) {
		_add_child(out, _visit(p_n->loop));
	}
	return out;
}

GDScriptDiffNode *_visit_while(const GP::WhileNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->condition) {
		_add_child(out, _visit(p_n->condition));
	}
	if (p_n->loop) {
		_add_child(out, _visit(p_n->loop));
	}
	return out;
}

GDScriptDiffNode *_visit_match(const GP::MatchNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->test) {
		_add_child(out, _visit(p_n->test));
	}
	for (int i = 0; i < p_n->branches.size(); i++) {
		_add_child(out, _visit(p_n->branches[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_match_branch(const GP::MatchBranchNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	for (int i = 0; i < p_n->patterns.size(); i++) {
		_add_child(out, _visit(p_n->patterns[i]));
	}
	if (p_n->guard_body) {
		_add_child(out, _visit(p_n->guard_body));
	}
	if (p_n->block) {
		_add_child(out, _visit(p_n->block));
	}
	return out;
}

GDScriptDiffNode *_visit_pattern(const GP::PatternNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	switch (p_n->pattern_type) {
		case GP::PatternNode::PT_LITERAL:
			_add_child(out, _make_literal_leaf(p_n->literal));
			break;
		case GP::PatternNode::PT_EXPRESSION:
			if (p_n->expression) {
				_add_child(out, _visit(p_n->expression));
			}
			break;
		case GP::PatternNode::PT_BIND:
			_add_child(out, _make_identifier_leaf(p_n->bind));
			break;
		case GP::PatternNode::PT_ARRAY:
			for (int i = 0; i < p_n->array.size(); i++) {
				_add_child(out, _visit(p_n->array[i]));
			}
			break;
		case GP::PatternNode::PT_DICTIONARY:
			for (int i = 0; i < p_n->dictionary.size(); i++) {
				const GP::PatternNode::Pair &pair = p_n->dictionary[i];
				if (pair.key) {
					_add_child(out, _visit(pair.key));
				}
				if (pair.value_pattern) {
					_add_child(out, _visit(pair.value_pattern));
				}
			}
			break;
		case GP::PatternNode::PT_REST:
		case GP::PatternNode::PT_WILDCARD:
			break;
	}
	return out;
}

GDScriptDiffNode *_visit_assignment(const GP::AssignmentNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	// Operator first so the diff doesn't mismatch `a = b` and `a += b`.
	_add_child(out, _make_operator_leaf(int(p_n->operation), GP::Node::ASSIGNMENT, p_n->start_line));
	if (p_n->assignee) {
		_add_child(out, _visit(p_n->assignee));
	}
	if (p_n->assigned_value) {
		_add_child(out, _visit(p_n->assigned_value));
	}
	return out;
}

GDScriptDiffNode *_visit_binary_op(const GP::BinaryOpNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_operator_leaf(int(p_n->operation), GP::Node::BINARY_OPERATOR, p_n->start_line));
	if (p_n->left_operand) {
		_add_child(out, _visit(p_n->left_operand));
	}
	if (p_n->right_operand) {
		_add_child(out, _visit(p_n->right_operand));
	}
	return out;
}

GDScriptDiffNode *_visit_unary_op(const GP::UnaryOpNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_operator_leaf(int(p_n->operation), GP::Node::UNARY_OPERATOR, p_n->start_line));
	if (p_n->operand) {
		_add_child(out, _visit(p_n->operand));
	}
	return out;
}

GDScriptDiffNode *_visit_ternary(const GP::TernaryOpNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->condition) {
		_add_child(out, _visit(p_n->condition));
	}
	if (p_n->true_expr) {
		_add_child(out, _visit(p_n->true_expr));
	}
	if (p_n->false_expr) {
		_add_child(out, _visit(p_n->false_expr));
	}
	return out;
}

GDScriptDiffNode *_visit_call(const GP::CallNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->callee) {
		_add_child(out, _visit(p_n->callee));
	}
	for (int i = 0; i < p_n->arguments.size(); i++) {
		_add_child(out, _visit(p_n->arguments[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_subscript(const GP::SubscriptNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->base) {
		_add_child(out, _visit(p_n->base));
	}
	if (p_n->is_attribute) {
		_add_child(out, _make_identifier_leaf(p_n->attribute));
	} else if (p_n->index) {
		_add_child(out, _visit(p_n->index));
	}
	return out;
}

GDScriptDiffNode *_visit_array(const GP::ArrayNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	for (int i = 0; i < p_n->elements.size(); i++) {
		_add_child(out, _visit(p_n->elements[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_dictionary(const GP::DictionaryNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	for (int i = 0; i < p_n->elements.size(); i++) {
		const GP::DictionaryNode::Pair &pair = p_n->elements[i];
		if (pair.key) {
			_add_child(out, _visit(pair.key));
		}
		if (pair.value) {
			_add_child(out, _visit(pair.value));
		}
	}
	return out;
}

GDScriptDiffNode *_visit_type(const GP::TypeNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	for (int i = 0; i < p_n->type_chain.size(); i++) {
		_add_child(out, _make_identifier_leaf(p_n->type_chain[i]));
	}
	for (int i = 0; i < p_n->container_types.size(); i++) {
		_add_child(out, _visit(p_n->container_types[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_type_test(const GP::TypeTestNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->operand) {
		_add_child(out, _visit(p_n->operand));
	}
	if (p_n->test_type) {
		_add_child(out, _visit(p_n->test_type));
	}
	return out;
}

GDScriptDiffNode *_visit_cast(const GP::CastNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->operand) {
		_add_child(out, _visit(p_n->operand));
	}
	if (p_n->cast_type) {
		_add_child(out, _visit(p_n->cast_type));
	}
	return out;
}

GDScriptDiffNode *_visit_await(const GP::AwaitNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->to_await) {
		_add_child(out, _visit(p_n->to_await));
	}
	return out;
}

GDScriptDiffNode *_visit_return(const GP::ReturnNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->return_value) {
		_add_child(out, _visit(p_n->return_value));
	}
	return out;
}

GDScriptDiffNode *_visit_assert(const GP::AssertNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->condition) {
		_add_child(out, _visit(p_n->condition));
	}
	if (p_n->message) {
		_add_child(out, _visit(p_n->message));
	}
	return out;
}

GDScriptDiffNode *_visit_lambda(const GP::LambdaNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->function) {
		_add_child(out, _visit(p_n->function));
	}
	for (int i = 0; i < p_n->captures.size(); i++) {
		_add_child(out, _make_identifier_leaf(p_n->captures[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_signal(const GP::SignalNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_identifier_leaf(p_n->identifier));
	for (int i = 0; i < p_n->parameters.size(); i++) {
		_add_child(out, _visit(p_n->parameters[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_enum(const GP::EnumNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	_add_child(out, _make_identifier_leaf(p_n->identifier));
	// Enum values aren't `Node`s; we emit a synthetic structural slot
	// per value with the value's identifier and (optional) custom value.
	for (int i = 0; i < p_n->values.size(); i++) {
		const GP::EnumNode::Value &v = p_n->values[i];
		// Reuse the LITERAL parser-type slot to give each value a stable
		// structural shape; the contained identifier and optional value
		// expression carry the semantically interesting data.
		GDScriptDiffNode *slot = memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_STRUCTURAL, GP::Node::ENUM, String(), v.line, v.line));
		_add_child(slot, _make_identifier_leaf(v.identifier));
		if (v.custom_value) {
			_add_child(slot, _visit(v.custom_value));
		}
		_add_child(out, slot);
	}
	return out;
}

GDScriptDiffNode *_visit_annotation(const GP::AnnotationNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	out->children.push_back(memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_IDENTIFIER_TEXT, GP::Node::IDENTIFIER, String(p_n->name), p_n->start_line, p_n->end_line)));
	for (int i = 0; i < p_n->arguments.size(); i++) {
		_add_child(out, _visit(p_n->arguments[i]));
	}
	return out;
}

GDScriptDiffNode *_visit_preload(const GP::PreloadNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	if (p_n->path) {
		_add_child(out, _visit(p_n->path));
	}
	return out;
}

GDScriptDiffNode *_visit_get_node(const GP::GetNodeNode *p_n) {
	GDScriptDiffNode *out = _make_struct(p_n);
	out->children.push_back(memnew(GDScriptDiffNode(GDScriptDiffNode::KIND_LITERAL_TEXT, GP::Node::LITERAL, p_n->full_path, p_n->start_line, p_n->end_line)));
	return out;
}

// ---- Dispatch --------------------------------------------------------------

GDScriptDiffNode *_visit(const GP::Node *p_node) {
	if (!p_node) {
		return nullptr;
	}

	switch (p_node->type) {
		case GP::Node::CLASS:
			return _visit_class(static_cast<const GP::ClassNode *>(p_node));
		case GP::Node::SUITE:
			return _visit_suite(static_cast<const GP::SuiteNode *>(p_node));
		case GP::Node::FUNCTION:
			return _visit_function(static_cast<const GP::FunctionNode *>(p_node));
		case GP::Node::VARIABLE:
		case GP::Node::CONSTANT:
		case GP::Node::PARAMETER:
			return _visit_assignable(static_cast<const GP::AssignableNode *>(p_node));
		case GP::Node::IF:
			return _visit_if(static_cast<const GP::IfNode *>(p_node));
		case GP::Node::FOR:
			return _visit_for(static_cast<const GP::ForNode *>(p_node));
		case GP::Node::WHILE:
			return _visit_while(static_cast<const GP::WhileNode *>(p_node));
		case GP::Node::MATCH:
			return _visit_match(static_cast<const GP::MatchNode *>(p_node));
		case GP::Node::MATCH_BRANCH:
			return _visit_match_branch(static_cast<const GP::MatchBranchNode *>(p_node));
		case GP::Node::PATTERN:
			return _visit_pattern(static_cast<const GP::PatternNode *>(p_node));
		case GP::Node::ASSIGNMENT:
			return _visit_assignment(static_cast<const GP::AssignmentNode *>(p_node));
		case GP::Node::BINARY_OPERATOR:
			return _visit_binary_op(static_cast<const GP::BinaryOpNode *>(p_node));
		case GP::Node::UNARY_OPERATOR:
			return _visit_unary_op(static_cast<const GP::UnaryOpNode *>(p_node));
		case GP::Node::TERNARY_OPERATOR:
			return _visit_ternary(static_cast<const GP::TernaryOpNode *>(p_node));
		case GP::Node::CALL:
			return _visit_call(static_cast<const GP::CallNode *>(p_node));
		case GP::Node::SUBSCRIPT:
			return _visit_subscript(static_cast<const GP::SubscriptNode *>(p_node));
		case GP::Node::ARRAY:
			return _visit_array(static_cast<const GP::ArrayNode *>(p_node));
		case GP::Node::DICTIONARY:
			return _visit_dictionary(static_cast<const GP::DictionaryNode *>(p_node));
		case GP::Node::TYPE:
			return _visit_type(static_cast<const GP::TypeNode *>(p_node));
		case GP::Node::TYPE_TEST:
			return _visit_type_test(static_cast<const GP::TypeTestNode *>(p_node));
		case GP::Node::CAST:
			return _visit_cast(static_cast<const GP::CastNode *>(p_node));
		case GP::Node::AWAIT:
			return _visit_await(static_cast<const GP::AwaitNode *>(p_node));
		case GP::Node::RETURN:
			return _visit_return(static_cast<const GP::ReturnNode *>(p_node));
		case GP::Node::ASSERT:
			return _visit_assert(static_cast<const GP::AssertNode *>(p_node));
		case GP::Node::LAMBDA:
			return _visit_lambda(static_cast<const GP::LambdaNode *>(p_node));
		case GP::Node::SIGNAL:
			return _visit_signal(static_cast<const GP::SignalNode *>(p_node));
		case GP::Node::ENUM:
			return _visit_enum(static_cast<const GP::EnumNode *>(p_node));
		case GP::Node::ANNOTATION:
			return _visit_annotation(static_cast<const GP::AnnotationNode *>(p_node));
		case GP::Node::PRELOAD:
			return _visit_preload(static_cast<const GP::PreloadNode *>(p_node));
		case GP::Node::GET_NODE:
			return _visit_get_node(static_cast<const GP::GetNodeNode *>(p_node));
		case GP::Node::IDENTIFIER:
			return _make_identifier_leaf(static_cast<const GP::IdentifierNode *>(p_node));
		case GP::Node::LITERAL:
			return _make_literal_leaf(static_cast<const GP::LiteralNode *>(p_node));
		// Pure structural nodes with no children worth diffing — represent as
		// a simple structural wrapper so they can still hash distinctly.
		case GP::Node::BREAK:
		case GP::Node::BREAKPOINT:
		case GP::Node::CONTINUE:
		case GP::Node::PASS:
		case GP::Node::SELF:
		case GP::Node::NONE:
			return _make_struct(p_node);
	}
	// Fallback: structural wrapper; ensures we never silently drop a node.
	return _make_struct(p_node);
}

} // namespace

GDScriptDiffNode *GDScriptDiffNode::from_parser_root(const GP::ClassNode *p_root) {
	if (!p_root) {
		return nullptr;
	}
	return _visit(p_root);
}
