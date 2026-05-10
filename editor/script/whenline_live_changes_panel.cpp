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
#include "editor/docks/editor_dock_manager.h"
#include "editor/settings/editor_command_palette.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/main/timer.h"

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
			return TTR("Startup");
		case BUCKET_PROCESS:
			return TTR("Per-frame loop");
		case BUCKET_INPUT:
			return TTR("User input");
		case BUCKET_OTHER:
			return TTR("Helper function");
		default:
			return TTR("Unknown");
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
			out += ", ...";
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
	set_name("Live Changes");
	set_icon_name("Edit");
	set_layout_key("WhenlineLiveChanges");
	set_dock_shortcut(ED_SHORTCUT_AND_COMMAND(
			"bottom_panels/toggle_whenline_panel",
			"Toggle Whenline Dock"));
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	set_available_layouts(EditorDock::DOCK_LAYOUT_HORIZONTAL | EditorDock::DOCK_LAYOUT_FLOATING);
	set_global(false);
	set_transient(true);

	VBoxContainer *content = memnew(VBoxContainer);
	content->set_h_size_flags(SIZE_EXPAND_FILL);
	content->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(content);

	scroll = memnew(ScrollContainer);
	scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	content->add_child(scroll);

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

	// Drive deadline transitions even when no debugger samples are flowing.
	// `whenline_data_updated` only fires when the engine sends fresh data,
	// so an idle game would otherwise leave a row stuck on PENDING past
	// its deadline. One tick per second is enough granularity for a 10s
	// deadline and adds negligible CPU.
	deadline_tick = memnew(Timer);
	deadline_tick->set_wait_time(1.0);
	deadline_tick->set_autostart(true);
	deadline_tick->set_one_shot(false);
	deadline_tick->connect("timeout", callable_mp(this, &WhenlineLiveChangesPanel::_refresh_all_rows));
	add_child(deadline_tick);

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
	debugger->connect("whenline_force_run_result", callable_mp(this, &WhenlineLiveChangesPanel::_on_force_run_result));
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
	debugger->disconnect("whenline_force_run_result", callable_mp(this, &WhenlineLiveChangesPanel::_on_force_run_result));
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
	// Use the editor's "Close" theme icon so we don't depend on a font
	// having the right Unicode glyph. Falls back to a label if the icon
	// isn't found, but in the editor it always is.
	const Ref<Texture2D> close_icon = get_editor_theme_icon(SNAME("Close"));
	if (close_icon.is_valid()) {
		batch_close->set_button_icon(close_icon);
	} else {
		batch_close->set_text(TTR("Dismiss"));
	}
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

ScriptEditorDebugger *WhenlineLiveChangesPanel::_find_debugger_for_script(const String &p_script_path) const {
	EditorDebuggerNode *node = EditorDebuggerNode::get_singleton();
	if (!node) {
		return nullptr;
	}

	// Prefer a debugger that already has either whenline_data or an active
	// reload-diff watch for this script. Iterate using `get_debugger(i)`
	// until it returns null — same loop pattern that `add_debugger_plugin`
	// uses to walk the tab list.
	for (int i = 0; ScriptEditorDebugger *dbg = node->get_debugger(i); i++) {
		if (!dbg->get_whenline_data_for_script(p_script_path).is_empty()) {
			return dbg;
		}
		if (!dbg->get_whenline_diff_for_script(p_script_path).is_empty()) {
			return dbg;
		}
	}

	// No debugger has data for this script yet. Fall back to the current
	// (or default) one so callers that want to *send* a force-run message
	// still have a target.
	if (ScriptEditorDebugger *cur = node->get_current_debugger()) {
		return cur;
	}
	return node->get_default_debugger();
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

	// Pull the editor theme icons we use across rows once. They're cheap
	// to fetch but doing it per-row would muddy the loop body.
	const Ref<Texture2D> dismiss_icon = get_editor_theme_icon(SNAME("Close"));
	const Ref<Texture2D> run_icon = get_editor_theme_icon(SNAME("Reload"));

	// One row per bucket. Every row gets a hint paragraph; the buckets that
	// don't support force-run still get the diagnostic so the panel always
	// answers "what do I do about this?".
	// Cache the deadline up front so each row inherits it rather than
	// re-reading the watch every refresh. The watch may be garbage-collected
	// on the engine side after it expires; we don't want to lose the
	// MISSED status when that happens.
	const int64_t watch_deadline = (int64_t)p_watch.get("deadline_msec", (int64_t)0);

	for (KeyValue<int, PackedInt32Array> &kv : grouped) {
		RowState *row = memnew(RowState);
		row->id = _next_row_id++;
		row->batch_id = p_batch->id;
		row->script_path = p_batch->script_path;
		row->bucket = kv.key;
		row->lines = kv.value;
		row->lines.sort();
		row->status = ROW_PENDING;
		row->deadline_msec = (uint64_t)watch_deadline;

		// Outer container is vertical so we can stack title and hint text.
		row->root = memnew(VBoxContainer);
		row->root->set_h_size_flags(SIZE_EXPAND_FILL);
		row->root->add_theme_constant_override("separation", int(2 * EDSCALE));

		// Top line: title · status · buttons.
		HBoxContainer *top = memnew(HBoxContainer);
		top->set_h_size_flags(SIZE_EXPAND_FILL);
		row->root->add_child(top);

		row->title_label = memnew(Label);
		row->title_label->set_h_size_flags(SIZE_EXPAND_FILL);
		row->title_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		top->add_child(row->title_label);

		row->status_label = memnew(Label);
		row->status_label->set_h_size_flags(SIZE_SHRINK_END);
		row->status_label->set_custom_minimum_size(Size2(int(80 * EDSCALE), 0));
		top->add_child(row->status_label);

		if (_bucket_supports_force_run(row->bucket)) {
			row->run_button = memnew(Button);
			row->run_button->set_text(TTR("Run anyway"));
			if (run_icon.is_valid()) {
				row->run_button->set_button_icon(run_icon);
			}
			row->run_button->set_tooltip_text(TTR("Re-invoke the containing lifecycle method (_ready / _enter_tree for startup, _process / _physics_process for the per-frame loop) on every live instance. The call is deferred to the next idle frame so it never interleaves mid-_process."));
			row->run_button->connect(SceneStringName(pressed), callable_mp(this, &WhenlineLiveChangesPanel::_on_run_anyway).bind(p_batch->id, row->id));
			top->add_child(row->run_button);
		}

		row->dismiss_button = memnew(Button);
		row->dismiss_button->set_flat(true);
		if (dismiss_icon.is_valid()) {
			row->dismiss_button->set_button_icon(dismiss_icon);
		} else {
			row->dismiss_button->set_text(TTR("Dismiss"));
		}
		row->dismiss_button->set_tooltip_text(TTR("Dismiss this row. The execution data and changed-line markers stay; only the row in this panel is removed."));
		row->dismiss_button->connect(SceneStringName(pressed), callable_mp(this, &WhenlineLiveChangesPanel::_on_dismiss_row).bind(p_batch->id, row->id));
		top->add_child(row->dismiss_button);

		// Hint line: explains in plain language *what* the user can do.
		// Always present, even on rows whose bucket has no force-run path,
		// so every entry in the panel tells you something actionable.
		row->hint_label = memnew(Label);
		row->hint_label->set_h_size_flags(SIZE_EXPAND_FILL);
		row->hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		row->hint_label->set_modulate(Color(1, 1, 1, 0.65));
		row->hint_label->add_theme_constant_override("line_spacing", 0);
		row->root->add_child(row->hint_label);

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

	// Rebuild the title in case translations/labels changed. We use plain
	// ASCII separators (" - ") rather than U+00B7 because not every editor
	// font carries it, and a bullet rendered as a missing-glyph box hurts
	// readability more than just using a hyphen.
	const String script_basename = p_row->script_path.is_empty() ? String("<built-in>") : p_row->script_path.get_file();
	const String lines_text = _format_lines(p_row->lines);
	p_row->title_label->set_text(vformat("%s - %s - lines %s", script_basename, _bucket_label(p_row->bucket), lines_text));
	p_row->title_label->set_tooltip_text(p_row->script_path);

	// Determine status from *positive evidence*: a line is considered
	// "hit" only if the debugger has actually received a sample for it
	// (count > 0 in `whenline_data`). The earlier design used the absence
	// of a line in `expected_lines` as a proxy for "hit", which had two
	// failure modes:
	//   1. If `get_current_debugger()` returned the wrong debugger (or
	//      none), `expected_lines` was empty by accident and rows wrongly
	//      flipped to RAN.
	//   2. After the engine-side watch's deadline expires and is erased,
	//      the same empty-expected-lines case occurred and rows lost
	//      their MISSED state.
	// Reading `count > 0` from `whenline_data` is direct and survives
	// both. We scan every available debugger so we don't miss data that
	// landed on a session other than the currently-focused one.
	ScriptEditorDebugger *dbg = _find_debugger_for_script(p_row->script_path);

	bool any_unhit = false;
	if (dbg) {
		const Dictionary line_data = dbg->get_whenline_data_for_script(p_row->script_path);
		for (int i = 0; i < p_row->lines.size(); i++) {
			const int line = p_row->lines[i];
			const Dictionary entry = line_data.get(line, Dictionary());
			const int64_t count = entry.get("count", (int64_t)0);
			if (count <= 0) {
				any_unhit = true;
				break;
			}
		}
	} else {
		// No debugger we can consult right now; assume nothing has run yet.
		// We won't flip to RAN without positive evidence, so this is safe.
		any_unhit = true;
	}

	const uint64_t now_msec = OS::get_singleton()->get_ticks_msec();
	const bool deadline_passed = p_row->deadline_msec > 0 && now_msec >= p_row->deadline_msec;

	if (!any_unhit) {
		p_row->status = ROW_RAN;
	} else if (deadline_passed) {
		p_row->status = ROW_MISSED;
	} else {
		p_row->status = ROW_PENDING;
	}

	// Pick a plain-text status string and modulate. We deliberately avoid
	// Unicode symbols here — the editor's default font isn't guaranteed to
	// have glyphs for every emoji, and a missing glyph renders as a square
	// or empty box that's worse than no symbol at all. Color carries the
	// urgency.
	switch (p_row->status) {
		case ROW_RAN:
			p_row->status_label->set_text(TTR("Ran"));
			p_row->status_label->set_modulate(Color(0.40, 0.80, 0.45));
			break;
		case ROW_MISSED:
			p_row->status_label->set_text(TTR("Didn't run"));
			p_row->status_label->set_modulate(Color(1.0, 0.72, 0.15));
			break;
		case ROW_PENDING:
		default:
			p_row->status_label->set_text(TTR("Waiting"));
			p_row->status_label->set_modulate(Color(0.7, 0.7, 0.7));
			break;
	}

	// Build the hint paragraph. Each bucket gets a tailored
	// "what-can-you-do-about-this" sentence, plus any captured-context
	// diagnostic the engine has sent over (currently only input).
	if (p_row->hint_label) {
		String hint;
		switch (p_row->bucket) {
			case BUCKET_INIT:
				hint = TTR("This code only runs when an instance is created. Click 'Run anyway' to re-invoke _ready and _enter_tree on every live instance of this script.");
				break;
			case BUCKET_PROCESS:
				hint = TTR("This code is in a per-frame method. Click 'Run anyway' to invoke _process and _physics_process once on every live instance, with the engine's current frame delta.");
				break;
			case BUCKET_INPUT:
				hint = TTR("This code only runs when the user produces input. Reproduce the input in the running game to test it.");
				break;
			case BUCKET_OTHER:
				hint = TTR("This code is only reached when something else calls into it. Trigger the relevant flow in the running game to test it.");
				break;
			default:
				hint = TTR("Reproduce the relevant flow in the running game to test this change.");
				break;
		}

		// Append captured-context diagnostics. For input handlers we have
		// the most recent InputEvent description per method; if any are
		// available, show them inline so the user has a concrete clue.
		if (p_row->bucket == BUCKET_INPUT && dbg) {
			const Dictionary captures = dbg->get_whenline_input_captures_for_script(p_row->script_path);
			if (!captures.is_empty()) {
				const Array keys = captures.keys();
				String caps_text;
				for (int ki = 0; ki < keys.size(); ki++) {
					const String method = keys[ki];
					const String description = captures[keys[ki]];
					if (!caps_text.is_empty()) {
						caps_text += "; ";
					}
					caps_text += vformat(TTR("%s last triggered by %s"), method, description);
				}
				hint += "\n" + caps_text + ".";
			}
		}

		p_row->hint_label->set_text(hint);
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

void WhenlineLiveChangesPanel::_on_run_anyway(int p_batch_id, int p_row_id) {
	Batch *batch = nullptr;
	RowState *row = _find_row_by_id(p_batch_id, p_row_id, &batch);
	if (!row || !batch) {
		return;
	}

	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	if (!debugger_node) {
		return;
	}
	ScriptEditorDebugger *dbg = debugger_node->get_current_debugger();
	if (!dbg) {
		// No active debug session — nothing to send to. Provide visible
		// feedback by repurposing the status label so the user knows.
		row->status_label->set_text(TTR("No session"));
		row->status_label->set_modulate(Color(1.0, 0.72, 0.15));
		return;
	}

	Array msg = { row->script_path, row->bucket };
	dbg->send_message("gdscript_force_run:run", msg);

	// Optimistically reflect that we just dispatched a request. The actual
	// outcome arrives via `_on_force_run_result`. We disable the button
	// briefly to discourage spamming repeated requests; it's re-enabled by
	// the next refresh.
	row->status_label->set_text(TTR("Running..."));
	row->status_label->set_modulate(Color(0.7, 0.85, 1.0));
	if (row->run_button) {
		row->run_button->set_disabled(true);
	}
}

void WhenlineLiveChangesPanel::_on_force_run_result(const String &p_script_path, int p_bucket, int p_succeeded, int p_errored, const String &p_error_text, int p_debugger) {
	(void)p_debugger;

	// Find the matching row(s). The result message identifies the script
	// and bucket but not the originating row id, so we update every row
	// that matches — in practice there's only one per (script, bucket).
	for (Batch *batch : batches) {
		if (batch->script_path != p_script_path) {
			continue;
		}
		for (RowState *row : batch->rows) {
			if (row->bucket != p_bucket) {
				continue;
			}
			if (p_errored > 0) {
				const String summary = vformat(TTR("%d error(s)%s"),
						p_errored,
						p_error_text.is_empty() ? String() : String(" (" + p_error_text + ")"));
				row->status_label->set_text(summary);
				row->status_label->set_modulate(Color(1.0, 0.45, 0.30));
			} else {
				const String summary = vformat(TTR("Ran %d call(s)"), p_succeeded);
				row->status_label->set_text(summary);
				row->status_label->set_modulate(Color(0.40, 0.80, 0.45));
			}
			if (row->run_button) {
				row->run_button->set_disabled(false);
			}
		}
	}
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
		b->header_label->set_text(vformat("%s - %s", when, script_label));
		b->header_label->set_modulate(is_latest ? Color(1, 1, 1, 1) : Color(1, 1, 1, 0.65));
		b->header_label->set_tooltip_text(b->script_path);
	}
}
