/**************************************************************************/
/*  whenline_live_changes_panel.h                                         */
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

#include "scene/gui/box_container.h"

class Button;
class Label;
class ScrollContainer;
class VBoxContainer;

// Bottom-panel UI that journals every reload-diff received during a debugging
// session. Each "batch" is the result of one hot-reload: it contains one row
// per (script, bucket) pair from the diff, and the rows update live as the
// referenced lines hit during execution.
//
// The panel is the user-facing replacement for the old `error_dialog` popup:
// it never steals focus, persists changes across reloads, and (in later
// phases) hosts the "Run anyway" force-execution buttons.
//
// Data flow:
//   `EditorDebuggerNode::whenline_reload_diff_received` → add a fresh batch
//                                                        from the watch state
//   `EditorDebuggerNode::whenline_data_updated`         → refresh existing
//                                                        batches' run state
class WhenlineLiveChangesPanel : public VBoxContainer {
	GDCLASS(WhenlineLiveChangesPanel, VBoxContainer);

public:
	// Status of a single row, derived from the diff watch and the live
	// execution data. Drives the row's color, status text, and which
	// affordances (run / dismiss) are shown.
	enum RowStatus {
		ROW_PENDING, // Lines flagged but the deadline hasn't passed and lines are still unhit.
		ROW_RAN, // Every line in the row has been observed running.
		ROW_MISSED, // Deadline passed and some lines never ran.
	};

private:
	// Persisted, panel-local state for one row of one batch. Survives
	// `whenline_data_updated` re-renders so dismissals stick across signal
	// bursts. Also survives subsequent diffs for *other* scripts; only a
	// fresh diff for the *same* script supersedes the row.
	struct RowState {
		int id = 0; // Stable id used by `callable_mp().bind(int)` callbacks.
		int batch_id = 0;
		String script_path;
		int bucket = 0; // GDScriptLanguage::WhenlineReason value.
		PackedInt32Array lines; // All lines of this group (for display).
		RowStatus status = ROW_PENDING;
		// UI handles — owned by the scene tree.
		HBoxContainer *root = nullptr;
		Label *title_label = nullptr;
		Label *status_label = nullptr;
		Button *run_button = nullptr; // Null for buckets we can't force-run.
		Button *dismiss_button = nullptr;
	};

	// One reload's worth of rows. Batches are appended to `batches_box` so
	// the most recent appears at the top (visually distinguished).
	struct Batch {
		int id = 0; // Stable id used by `callable_mp().bind(int)` callbacks.
		String script_path;
		uint64_t arrived_msec = 0;
		bool is_latest = false;
		VBoxContainer *root = nullptr;
		Label *header_label = nullptr;
		VBoxContainer *rows_box = nullptr;
		Vector<RowState *> rows;
	};

	ScrollContainer *scroll = nullptr;
	VBoxContainer *batches_box = nullptr;
	Label *empty_label = nullptr;
	LocalVector<Batch *> batches;

	// Monotonically incrementing id sources used so dismiss / run
	// callbacks can refer to a row or batch by id without dragging raw
	// pointers through `callable_mp().bind(...)` (which requires Variant-
	// compatible types). Lookup is O(n) but n is tiny.
	int _next_batch_id = 1;
	int _next_row_id = 1;

	// Connected only while the panel is in the tree, so we can safely
	// rebuild rows in `_handle_*` even if no debugger session is active.
	bool _connected_to_debugger = false;

	void _connect_debugger_signals();
	void _disconnect_debugger_signals();

	void _on_reload_diff_received(const String &p_script_path, int p_debugger);
	void _on_data_updated(int p_debugger);

	Batch *_find_batch_for_script(const String &p_script_path);
	Batch *_find_batch_by_id(int p_batch_id);
	RowState *_find_row_by_id(int p_batch_id, int p_row_id, Batch **r_batch = nullptr);
	void _render_batch_from_watch(Batch *p_batch, const Dictionary &p_watch);
	void _refresh_all_rows();
	void _refresh_row(RowState *p_row);
	void _on_dismiss_row(int p_batch_id, int p_row_id);
	void _on_dismiss_batch(int p_batch_id);

	void _update_empty_state();
	void _mark_only_first_batch_as_latest();

	static String _bucket_label(int p_bucket);
	static bool _bucket_supports_force_run(int p_bucket);
	static String _format_lines(const PackedInt32Array &p_lines);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	WhenlineLiveChangesPanel();
	~WhenlineLiveChangesPanel() override;
};
