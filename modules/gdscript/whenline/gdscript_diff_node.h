/**************************************************************************/
/*  gdscript_diff_node.h                                                  */
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
#include "truediff.h"

// Wraps a GDScript AST so it can be diffed by `TrueDiff`.
//
// We don't subclass `WhenlineDiffNode` per parser node type. Instead, every
// GDScript node maps to a single concrete `GDScriptDiffNode` whose `kind`
// field carries the parser node's type. Children correspond to the parser
// node's child slots, with optional slots (e.g. `IfNode::false_block`) only
// added when present. Text leaves wrap identifiers and literals.
//
// The wrapper owns its children, like all `WhenlineDiffNode` subclasses, so
// freeing the root frees the whole diff-side tree. The original parser tree
// is not touched.
//
// Each diff node remembers its `start_line`/`end_line` (from the parser), and
// optional text content (`text_value`) is filled in for identifier/literal
// leaves so `update_literals` can detect renames and value changes.

class GDScriptDiffNode : public WhenlineDiffNode {
public:
	enum Kind {
		// Synthesized "leaf" kinds. They carry meaningful text and no children.
		KIND_IDENTIFIER_TEXT,
		KIND_LITERAL_TEXT,
		KIND_OPERATOR_TEXT, // For binary/unary/assignment operators.
		// Generic structural wrapper. The exact role is encoded by `parser_type`.
		KIND_STRUCTURAL,
	};

private:
	Kind kind = KIND_STRUCTURAL;
	GDScriptParser::Node::Type parser_type = GDScriptParser::Node::NONE;
	String text_value; // Empty for non-text nodes.
	int parser_start_line = 0;
	int parser_end_line = 0;

public:
	GDScriptDiffNode(Kind p_kind, GDScriptParser::Node::Type p_parser_type, const String &p_text, int p_start_line, int p_end_line) {
		kind = p_kind;
		parser_type = p_parser_type;
		text_value = p_text;
		parser_start_line = p_start_line;
		parser_end_line = p_end_line;
	}

	Kind get_kind() const { return kind; }
	GDScriptParser::Node::Type get_parser_type() const { return parser_type; }
	int get_start_line() const { return parser_start_line; }
	int get_end_line() const { return parser_end_line; }

	// WhenlineDiffNode interface.
	bool is_text() const override {
		return kind == KIND_IDENTIFIER_TEXT || kind == KIND_LITERAL_TEXT || kind == KIND_OPERATOR_TEXT;
	}
	String get_text() const override { return text_value; }
	StringName get_type_name() const override;
	WhenlineDiffNode *shallow_clone() const override;
	void _compute_local_hashes() override;

	// Build a `GDScriptDiffNode` tree from the parser's root class node.
	// The returned node owns all of its descendants.
	static GDScriptDiffNode *from_parser_root(const GDScriptParser::ClassNode *p_root);
};
