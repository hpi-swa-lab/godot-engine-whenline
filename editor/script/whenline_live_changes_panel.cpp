/**************************************************************************/
/*  whenline_live_changes_panel.cpp                                       */
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

#include "whenline_live_changes_panel.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"

// Bucket constants mirror `GDScriptLanguage::WhenlineReason`. We don't
// include the GDScript header here to keep this UI module independent of
// the language module; the values are stable and small.
static constexpr int BUCKET_UNKNOWN = 0;
static constexpr int BUCKET_INIT = 1;
static constexpr int BUCKET_PROCESS = 2;
static constexpr int BUCKET_INPUT = 3;
static constexpr int BUCKET_OTHER = 4;

String WhenlineLiveChangesPanel::_bucket_label(int p_bucket) {
	switch (p_bucket) {
		case BUCKET_INIT:
			return TTR("init");
		case BUCKET_PROCESS:
			return TTR("process");
		case BUCKET_INPUT:
			return TTR("input");
		case BUCKET_OTHER:
			return TTR("helper");
		default:
			return TTR("unknown");
	}
}

bool WhenlineLiveChangesPanel::_bucket_supports_force_run(int p_bucket) {
	// In v1 only init and loop buckets get a "Run anyway" affordance; the
	// remaining buckets show up as read-only diagnostics. See the design
	// notes in the agent thread for the full rationale.
	return p_bucket == BUCKET_INIT || p_bucket == BUCKET_PROCESS;
}

String WhenlineLiveChangesPanel::_format_lines(const PackedInt32Array &p_lines) {
	if (p_lines.is_empty()) {
		return String();
	}
	// Compress runs of consecutive lines into ranges so a 30-line edit
	// reads as "12-41" instead of a comma-separated dump. Caps the visible
	// segments so the row stays single-line; the tooltip can show the full
	// list later if we want.
	String out;
	int i = 0;
	int segments = 0;
	const int max_segments = 4;
	while (i < p_lines.size()) {
		int start = p_lines[i];
		int end = start;
		while (i + 1 < p_lines.size() && p_lines[i + 1] == end + 1) {
			i++;
			end = p_lines[i];
		}
		i++;
		if (segments == max_segments) {
			out += String::utf8(", \xe2\x80\xa6"); // ", …"
			break;
		}
		if (segments > 0) {
			out += ", ";
		}
		segments++;
		if (start == end) {
			out += itos(start);
		} else {
			out += itos(start) + "-" + itos(end);
		}
	}
	return out;
}

// =============================================================================
// Lifecycle
// =============================================================================

WhenlineLiveChangesPanel::WhenlineLiveChangesPanel() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	scroll = memnew(ScrollContainer);
	scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(scroll);

	batches_box = memnew(VBoxContainer);
	batches_box->set_h_size_flags(SIZE_EXPAND_FILL);
	batches_box->add_theme_constant_override("separation", int(8 * EDSCALE));
	scroll->add_child(batches_box);

	empty_label = memnew(Label);
	empty_label->set_text(TTR("No live edits yet. Save a script while a debug session is running to see them here."));
	empty_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_label->set_v_size_flags(SIZE_EXPAND_FILL);
	empty_label->set_h_size_flags(SIZE_EXPAND_FILL);
	empty_label->set_modulate(Color(1, 1, 1, 0.5));
	scroll->add_child(empty_label);

	_update_empty_state();
}

WhenlineLiveChangesPanel::~WhenlineLiveChangesPanel() {
	for (Batch *b : batches) {
		for (RowState *r : b->rows) {
			memdelete(r);
		}
		memdelete(b);
	}
	batches.clear();
}

void WhenlineLiveChangesPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_connect_debugger_signals();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_disconnect_debugger_signals();
		} break;
	}
}

void WhenlineLiveChangesPanel::_bind_methods() {
}

// =============================================================================
// Debugger plumbing
// =============================================================================

void WhenlineLiveChangesPanel::_connect_debugger_signals() {
	if (_connected_to_debugger) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger) {
		return;
	}
	debugger->connect("whenline_reload_diff_received", callable_mp(this, &WhenlineLiveChangesPanel::_on_reload_diff_received));
	debugger->connect("whenline_data_updated", callable_mp(this, &WhenlineLiveChangesPanel::_on_data_updated));
	_connected_to_debugger = true;
}

void WhenlineLiveChangesPanel::_disconnect_debugger_signals() {
	if (!_connected_to_debugger) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger) {
		_connected_to_debugger = false;
		return;
	}
	debugger->disconnect("whenline_reload_diff_received", callable_mp(this, &WhenlineLiveChangesPanel::_on_reload_diff_received));
	debugger->disconnect("whenline_data_updated", callable_mp(this, &WhenlineLiveChangesPanel::_on_data_updated));
	_connected_to_debugger = false;
}

void WhenlineLiveChangesPanel::_on_reload_diff_received(const String &p_script_path, int p_debugger) {
	ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_debugger(p_debugger);
	if (!dbg) {
		return;
	}
	const Dictionary watch = dbg->get_whenline_diff_for_script(p_script_path);
	if (watch.is_empty()) {
		return;
	}

	// Supersede any existing batch for the same script: a fresh diff means
	// the user just hot-reloaded again, and the previous batch describes
	// stale state.
	Batch *existing = _find_batch_for_script(p_script_path);
	if (existing) {
		_on_dismiss_batch(existing->id);
	}

	Batch *batch = memnew(Batch);
	batch->id = _next_batch_id++;
	batch->script_path = p_script_path;
	batch->arrived_msec = OS::get_singleton()->get_ticks_msec();

	batch->root = memnew(VBoxContainer);
	batch->root->set_h_size_flags(SIZE_EXPAND_FILL);
	batch->root->add_theme_constant_override("separation", int(4 * EDSCALE));

	HBoxContainer *header = memnew(HBoxContainer);
	header->set_h_size_flags(SIZE_EXPAND_FILL);
	batch->header_label = memnew(Label);
	batch->header_label->set_h_size_flags(SIZE_EXPAND_FILL);
	header->add_child(batch->header_label);

	Button *batch_close = memnew(Button);
	batch_close->set_flat(true);
	batch_close->set_text("\xe2\x9c\x95"); // ✕
	batch_close->set_tooltip_text(TTR("Dismiss this reload's changes."));
	batch_close->connect(SceneStringName(pressed), callable_mp(this, &WhenlineLiveChangesPanel::_on_dismiss_batch).bind(batch->id));
	header->add_child(batch_close);

	batch->root->add_child(header);

	batch->rows_box = memnew(VBoxContainer);
	batch->rows_box->set_h_size_flags(SIZE_EXPAND_FILL);
	batch->rows_box->add_theme_constant_override("separation", int(2 * EDSCALE));
	batch->root->add_child(batch->rows_box);

	HSeparator *sep = memnew(HSeparator);
	batch->root->add_child(sep);

	// Insert the new batch at the top so the most recent reload is most
	// visible. We move it explicitly because `add_child` always appends.
	batches_box->add_child(batch->root);
	batches_box->move_child(batch->root, 0);

	batches.insert(0, batch);

	_render_batch_from_watch(batch, watch);
	_mark_only_first_batch_as_latest();
	_update_empty_state();
}

void WhenlineLiveChangesPanel::_on_data_updated(int p_debugger) {
	(void)p_debugger;
	_refresh_all_rows();
}

WhenlineLiveChangesPanel::Batch *WhenlineLiveChangesPanel::_find_batch_for_script(const String &p_script_path) {
	for (Batch *b : batches) {
		if (b->script_path == p_script_path) {
			return b;
		}
	}
	return nullptr;
}

WhenlineLiveChangesPanel::Batch *WhenlineLiveChangesPanel::_find_batch_by_id(int p_batch_id) {
	for (Batch *b : batches) {
		if (b->id == p_batch_id) {
			return b;
		}
	}
	return nullptr;
}

WhenlineLiveChangesPanel::RowState *WhenlineLiveChangesPanel::_find_row_by_id(int p_batch_id, int p_row_id, Batch **r_batch) {
	Batch *b = _find_batch_by_id(p_batch_id);
	if (!b) {
		return nullptr;
	}
	for (RowState *r : b->rows) {
		if (r->id == p_row_id) {
			if (r_batch) {
				*r_batch = b;
			}
			return r;
		}
	}
	return nullptr;
}

// =============================================================================
// Batch / row construction
// =============================================================================

void WhenlineLiveChangesPanel::_render_batch_from_watch(Batch *p_batch, const Dictionary &p_watch) {
	const PackedInt32Array all_changed = p_watch.get("all_changed_lines", PackedInt32Array());
	const Dictionary line_buckets = p_watch.get("line_buckets", Dictionary());

	// Group lines by bucket. We use a small `HashMap<int, PackedInt32Array>`
	// keyed by bucket id; insertion order is preserved (HashMap iterates in
	// insertion order), so the first bucket encountered shows up first.
	HashMap<int, PackedInt32Array> grouped;
	for (int i = 0; i < all_changed.size(); i++) {
		const int line = all_changed[i];
		const int bucket = (int)line_buckets.get(line, BUCKET_UNKNOWN);
		PackedInt32Array &lines = grouped[bucket];
		lines.push_back(line);
	}

	// One row per bucket. Helper-bucket changes (BUCKET_OTHER) get rendered
	// the same way; the user just won't get a "Run" button.
	for (KeyValue<int, PackedInt32Array> &kv : grouped) {
		RowState *row = memnew(RowState);
		row->id = _next_row_id++;
		row->batch_id = p_batch->id;
		row->script_path = p_batch->script_path;
		row->bucket = kv.key;
		row->lines = kv.value;
		row->lines.sort();
		row->status = ROW_PENDING;

		row->root = memnew(HBoxContainer);
		row->root->set_h_size_flags(SIZE_EXPAND_FILL);

		row->title_label = memnew(Label);
		row->title_label->set_h_size_flags(SIZE_EXPAND_FILL);
		row->root->add_child(row->title_label);

		row->status_label = memnew(Label);
		row->status_label->set_h_size_flags(SIZE_SHRINK_END);
		row->root->add_child(row->status_label);

		if (_bucket_supports_force_run(row->bucket)) {
			row->run_button = memnew(Button);
			row->run_button->set_text(TTR("Run anyway"));
			row->run_button->set_tooltip_text(TTR("Re-invoke the containing method on every live instance, deferred to the next idle frame."));
			// TODO(whenline-force-run): wire to engine-side force-run handler in Phase 10.
			row->run_button->set_disabled(true);
			row->root->add_child(row->run_button);
		}

		row->dismiss_button = memnew(Button);
		row->dismiss_button->set_flat(true);
		row->dismiss_button->set_text("\xe2\x9c\x95"); // ✕
		row->dismiss_button->set_tooltip_text(TTR("Dismiss this row."));
		row->dismiss_button->connect(SceneStringName(pressed), callable_mp(this, &WhenlineLiveChangesPanel::_on_dismiss_row).bind(p_batch->id, row->id));
		row->root->add_child(row->dismiss_button);

		p_batch->rows_box->add_child(row->root);
		p_batch->rows.push_back(row);

		_refresh_row(row);
	}
}

void WhenlineLiveChangesPanel::_refresh_all_rows() {
	for (Batch *b : batches) {
		for (RowState *r : b->rows) {
			_refresh_row(r);
		}
	}
}

void WhenlineLiveChangesPanel::_refresh_row(RowState *p_row) {
	if (!p_row || !p_row->root) {
		return;
	}

	// Rebuild the title in case translations/labels changed.
	const String script_basename = p_row->script_path.is_empty() ? String("<built-in>") : p_row->script_path.get_file();
	const String lines_text = _format_lines(p_row->lines);
	p_row->title_label->set_text(vformat("%s · %s · lines %s", script_basename, _bucket_label(p_row->bucket), lines_text));
	p_row->title_label->set_tooltip_text(p_row->script_path);

	// Determine current status from the live debugger watch. We re-query
	// per refresh so the row reflects the freshest data, even when many
	// `whenline_data_updated` signals fire in quick succession.
	ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton() ? EditorDebuggerNode::get_singleton()->get_current_debugger() : nullptr;
	HashSet<int> still_unhit;
	bool deadline_passed = false;
	if (dbg) {
		const Dictionary watch = dbg->get_whenline_diff_for_script(p_row->script_path);
		if (!watch.is_empty()) {
			const PackedInt32Array expected = watch.get("expected_lines", PackedInt32Array());
			for (int i = 0; i < expected.size(); i++) {
				still_unhit.insert(expected[i]);
			}
			deadline_passed = watch.get("deadline_passed", false);
		} else {
			// The debugger's watch entry is gone (deadline expired and was
			// erased, or the session restarted). Treat as "deadline passed
			// with no remaining info" \u2014 the row freezes in place.
			deadline_passed = true;
		}
	}

	// A row's status only depends on *its own* lines: even if other rows
	// have hits or misses, this row is independent.
	bool any_unhit = false;
	for (int i = 0; i < p_row->lines.size(); i++) {
		if (still_unhit.has(p_row->lines[i])) {
			any_unhit = true;
			break;
		}
	}

	if (!any_unhit) {
		p_row->status = ROW_RAN;
	} else if (deadline_passed) {
		p_row->status = ROW_MISSED;
	} else {
		p_row->status = ROW_PENDING;
	}

	// Pick a status string and modulate. Colors are picked from a palette
	// that reads in both light and dark themes; we deliberately avoid the
	// editor accent color so the panel doesn't compete with selection UI.
	switch (p_row->status) {
		case ROW_RAN:
			p_row->status_label->set_text(TTR("\u2713 ran"));
			p_row->status_label->set_modulate(Color(0.40, 0.80, 0.45));
			break;
		case ROW_MISSED:
			p_row->status_label->set_text(TTR("\u26a0 didn't run"));
			p_row->status_label->set_modulate(Color(1.0, 0.72, 0.15));
			break;
		case ROW_PENDING:
		default:
			p_row->status_label->set_text(TTR("\u23f3 waiting"));
			p_row->status_label->set_modulate(Color(0.7, 0.7, 0.7));
			break;
	}
}

// =============================================================================
// Dismissals & latest-marker
// =============================================================================

void WhenlineLiveChangesPanel::_on_dismiss_row(int p_batch_id, int p_row_id) {
	Batch *batch = nullptr;
	RowState *row = _find_row_by_id(p_batch_id, p_row_id, &batch);
	if (!row || !batch) {
		return;
	}
	int idx = -1;
	for (int i = 0; i < batch->rows.size(); i++) {
		if (batch->rows[i] == row) {
			idx = i;
			break;
		}
	}
	if (idx == -1) {
		return;
	}
	if (row->root) {
		row->root->queue_free();
	}
	batch->rows.remove_at(idx);
	memdelete(row);

	if (batch->rows.is_empty()) {
		_on_dismiss_batch(batch->id);
	}
}

void WhenlineLiveChangesPanel::_on_dismiss_batch(int p_batch_id) {
	int idx = -1;
	Batch *batch = nullptr;
	for (uint32_t i = 0; i < batches.size(); i++) {
		if (batches[i]->id == p_batch_id) {
			idx = int(i);
			batch = batches[i];
			break;
		}
	}
	if (idx == -1) {
		return;
	}
	for (RowState *r : batch->rows) {
		memdelete(r);
	}
	if (batch->root) {
		batch->root->queue_free();
	}
	memdelete(batch);
	batches.remove_at(idx);

	_mark_only_first_batch_as_latest();
	_update_empty_state();
}

void WhenlineLiveChangesPanel::_update_empty_state() {
	if (empty_label) {
		empty_label->set_visible(batches.is_empty());
	}
	if (batches_box) {
		batches_box->set_visible(!batches.is_empty());
	}
}

void WhenlineLiveChangesPanel::_mark_only_first_batch_as_latest() {
	for (uint32_t i = 0; i < batches.size(); i++) {
		Batch *b = batches[i];
		const bool is_latest = (i == 0);
		b->is_latest = is_latest;
		const String script_label = b->script_path.is_empty() ? String("<built-in>") : b->script_path.get_file();
		const String when = is_latest ? TTR("Latest reload") : TTR("Earlier reload");
		b->header_label->set_text(vformat("%s \u2014 %s", when, script_label));
		b->header_label->set_modulate(is_latest ? Color(1, 1, 1, 1) : Color(1, 1, 1, 0.65));
		b->header_label->set_tooltip_text(b->script_path);
	}
}
