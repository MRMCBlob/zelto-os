// Zelto demo app — "Notepad" (the persistent-storage demo, P11).
//
// Proves storage survives a reboot. On launch it reads a counter from the
// per-app preferences store (z_prefs_get_int) and loads recent rows from a
// SQLite database opened in its private documents dir (z_db_open + z_db_query).
// Tapping "Add note" bumps the counter (z_prefs_set_int) and INSERTs a row
// (z_db_run with z_args binding), then re-renders the read-back value.
//
// All of this lives under $ZELTO_DATA_DIR/apps/os.zelto.notepad/documents on the
// virtio-blk ext4 disk init mounts at /var/zelto, so after the VM reboots into
// the same disk image the relaunched app shows the same count (not reset to 0)
// and the same DB rows. Maps a plain xdg_toplevel below the bar.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#define NP_MAX_ROWS 6

typedef struct NotepadState {
    bool inited;
    ZDatabase *db;
    int64_t count;                 // persisted counter (prefs key "count")
    int64_t loaded;                // what we read from disk at startup
    int row_count;                 // rows currently loaded for display
    char rows[NP_MAX_ROWS][80];    // "#<n>: <note>" most-recent-first
    ZTextField note;               // typed note text (P21 soft-keyboard demo)
    ZTextField title;              // a 2nd field (P22): copy a word from `note`
                                   // and paste it here to prove in-app text move
    bool wrote_this_boot;          // an INSERT has happened in this process
    bool autosaved;                // ZELTO_NOTEPAD_AUTOSAVE has fired once
    bool autofocused;              // the note field was focused for the harness
    char echoed[Z_TEXTFIELD_CAP];  // last note text ZELTO_NOTEPAD_ECHO reported
} NotepadState;

// Reload the most-recent rows from the DB into the display snapshot.
static void refresh_rows(NotepadState *s) {
    s->row_count = 0;
    if (!s->db) {
        return;
    }
    ZRows *r = z_db_query(
        s->db, "SELECT n, note FROM notes ORDER BY id DESC LIMIT 6", Z_NO_ARGS);
    if (!r) {
        return;
    }
    while (z_rows_next(r) && s->row_count < NP_MAX_ROWS) {
        int64_t n = z_rows_int(r, 0);
        const char *note = z_rows_str(r, 1);
        snprintf(s->rows[s->row_count], sizeof(s->rows[s->row_count]),
                 "#%lld: %s", (long long)n, note ? note : "");
        // The read-back half of the pair above. Logged only for rows loaded
        // BEFORE this process has written anything, so a two-boot test can tell
        // "came off ext4" from "we just inserted it".
        if (!s->wrote_this_boot) {
            fprintf(stderr, "[notepad] loaded #%lld: %s\n", (long long)n,
                    note ? note : "");
            fflush(stderr);
        }
        s->row_count++;
    }
    z_rows_free(r);
}

// First build: open the store, read the persisted counter, ensure the schema,
// and load existing rows — so the very first frame reflects what is on disk.
static void ensure_init(NotepadState *s) {
    if (s->inited) {
        return;
    }
    s->inited = true;
    s->count = z_prefs_get_int("count", 0);
    s->loaded = s->count;
    // Pre-fill the note field so there is a word to select for the P22 clipboard
    // demo without first typing it (typing still works — tap the field). Two words
    // so a long-press selects just one (proving word-, not whole-field, selection).
    //
    // NOT pre-filled under the P45 KBD harness or the P46 key-cap one: those
    // tests' whole point is that what ends up in the field is what was TYPED, and
    // a field seeded with "hello world" would show a note nobody entered and pass
    // either way.
    if (!getenv("ZELTO_NOTEPAD_AUTOSAVE") && !getenv("ZELTO_NOTEPAD_ECHO")) {
        snprintf(s->note.text, sizeof(s->note.text), "hello world");
        s->note.len = (int)strlen(s->note.text);
        s->note.caret = s->note.anchor = s->note.len;
    }
    s->db = z_db_open("notepad");
    if (s->db) {
        z_db_exec(s->db,
                  "CREATE TABLE IF NOT EXISTS notes ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT, n INTEGER, note TEXT)");
    }
    refresh_rows(s);
}

// "Add note": persist a bumped counter (prefs) + INSERT a row (DB), then reload.
// The note text is whatever was typed into the soft-keyboard-backed field; if the
// field is empty it falls back to an auto-generated label so a tap still works.
static void add_note(ZApp *app, void *state) {
    NotepadState *s = state;
    s->wrote_this_boot = true;
    s->count++;
    z_prefs_set_int("count", s->count);
    if (s->db) {
        char note[Z_TEXTFIELD_CAP + 32];
        if (s->note.len > 0) {
            snprintf(note, sizeof(note), "%s", s->note.text);
        } else {
            snprintf(note, sizeof(note), "note number %lld", (long long)s->count);
        }
        z_db_run(s->db, "INSERT INTO notes(n, note) VALUES(?, ?)",
                 z_args(s->count, note));
        // On the record, so a two-boot test can assert what was WRITTEN against
        // what a later boot reads back. Without this the only evidence a note
        // survived is a screenshot of a list, which cannot distinguish "loaded
        // from disk" from "still in memory".
        fprintf(stderr, "[notepad] saved #%lld: %s\n", (long long)s->count, note);
        fflush(stderr);
        refresh_rows(s);
    }
    // Clear the field for the next note.
    s->note.len = 0;
    s->note.text[0] = '\0';
    z_invalidate(app);
}

// The DB rows list (most recent first), or a placeholder when empty.
static ZView rows_panel(NotepadState *s) {
    ZStackOpts opts = {.spacing = 6, .align = Z_ALIGN_LEADING, .padding = 16};
    int k = 0;
    opts.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_CAPTION, Text("DB rows (newest first)")));
    if (s->row_count == 0) {
        opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_BODY, Text("(no rows yet)")));
    }
    for (int i = 0; i < s->row_count && k < Z_MAX_CHILDREN; i++) {
        opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_BODY, Text("%s", s->rows[i])));
    }
    return Background(Z_COLOR_SURFACE_2,
        CornerRadius(14, Frame(520.0f, 0.0f, z_stack(Z_AXIS_VERTICAL, &opts))));
}

static ZView notepad_body(ZApp *app, NotepadState *state) {
    ensure_init(state);

    // P45 KBD harness hooks. The old harness opened the app drawer P40 deleted
    // and tapped key caps at coordinates measured off a screenshot; both are
    // gone, so the field is focused here and the save fires on TYPED LENGTH
    // rather than on a timer. Length, not time, is what makes it deterministic:
    // under TCG the guest clock lags wall time by an unpredictable amount, so
    // any "save 40s after launch" would race the keys instead of following them.
    // P46 KEY-CAP harness hook. ZELTO_NOTEPAD_ECHO=1 reports the note field's
    // exact content every time it changes. The autosave hook above cannot serve
    // that test: it fires on a LENGTH, and a sequence with a backspace in it
    // passes through the trigger length before it is finished, so what got saved
    // would not be what was typed. An echo per change is also the only way to see
    // the INTERMEDIATE states — that shift applied to exactly one letter, that
    // backspace removed exactly one — rather than only the end of the string.
    const char *echo = getenv("ZELTO_NOTEPAD_ECHO");
    const char *autosave = getenv("ZELTO_NOTEPAD_AUTOSAVE");
    if (echo && echo[0] && strcmp(state->echoed, state->note.text) != 0) {
        snprintf(state->echoed, sizeof(state->echoed), "%s", state->note.text);
        fprintf(stderr, "[notepad] field='%s' len=%d\n", state->note.text,
                state->note.len);
        fflush(stderr);
    }
    // P47. ZELTO_NOTEPAD_SECURE=1 marks the note field a PASSWORD field, which is
    // not a notepad feature — it is the only way to get a secure field in front of
    // the real keyboard without inventing a login app for one test. What it
    // exercises is the whole relay: the field declares content purpose PASSWORD
    // through text-input-v3, the compositor forwards it to input-method-v2, and
    // the keyboard turns its language model off. Everything in that chain has been
    // in place since P21 except the two ends.
    const char *secure = getenv("ZELTO_NOTEPAD_SECURE");
    state->note.secure = secure && secure[0] == '1';
    // A note is prose, so it asks the keyboard for sentence case (content hint
    // AUTO_CAPITALIZATION). ZELTO_NOTEPAD_NOAUTOCAP=1 turns it off for the
    // keyboard tests that are about WHICH KEY a press resolved to: a capital in
    // every expected string would be a second variable in every assertion, and
    // the first thing a reader would have to rule out on a failure.
    state->note.autocap = !getenv("ZELTO_NOTEPAD_NOAUTOCAP");
    if (echo && echo[0] && !state->autofocused) {
        state->autofocused = true;
        z_app_focus_field(app, &state->note);
    }
    if (autosave && autosave[0]) {
        if (!state->autofocused) {
            state->autofocused = true;
            z_app_focus_field(app, &state->note);   // no tap, no coordinates
        }
        int want = atoi(autosave);
        if (!state->autosaved && want > 0 && state->note.len >= want) {
            state->autosaved = true;
            add_note(app, state);
        }
    }

    // Content is TOP-anchored (a leading fixed gap + a single trailing Spacer), so
    // the two text fields keep the same y whether the keyboard is up or down — the
    // keyboard's exclusive zone only shrinks the app from the bottom, which the
    // trailing Spacer absorbs. That stability is what makes the copy/paste flow
    // reproducible (fields don't move mid-gesture).
    // Content is TOP-anchored (a leading fixed gap + a single trailing Spacer) so
    // the fields keep the same y whether the keyboard is up or down (the keyboard's
    // exclusive zone shrinks the app from the bottom, which the trailing Spacer
    // absorbs). The action bar lives INLINE as a fixed-height slot, not a ZStack
    // overlay, because an overlay's buttons proved un-hittable; a fixed slot (bar
    // when a field is focused, else an empty placeholder) keeps the layout stable.
    ZView bar = z_selection_bar(app);
    ZStackOpts col = {.padding = 20, .spacing = 12, .align = Z_ALIGN_CENTER};
    int k = 0;
    // Fixed gap clears the system shade's top grab strip (~72px below the 40px
    // status bar catches the pull-down gesture) so the bar slot below it is tappable.
    col.children[k++] = Rect(.height = 84.0f);
    col.children[k++] =
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("Notepad")));
    // Two fields: the note is pre-filled "hello world"; long-press a word to select
    // it, Copy, then Paste into the title field (in-app text move).
    col.children[k++] = Frame(360.0f, 0.0f,
        TextField(app, &state->note, "Type a note..."));
    col.children[k++] = Frame(360.0f, 0.0f,
        TextField(app, &state->title, "Title (paste here)..."));
    // The action bar in a fixed-height slot (bar when focused, else empty).
    col.children[k++] =
        Frame(0.0f, 52.0f, bar ? bar : z_rect(&(ZRectOpts){.height = 1.0f}));
    // ZStack, not a bare Text: Frame only sets a size, so a Text made its direct
    // child lands at the frame's origin and hugs the top-left corner. A depth
    // stack is what centres it.
    col.children[k++] = Frame(140.0f, 72.0f,
        Background(Z_COLOR_PRIMARY,
            CornerRadius(20,
                ZStack(
                    Foreground(Z_COLOR_ON_PRIMARY,
                        Font(Z_FONT_TITLE,
                            Text("%lld", (long long)state->count))),
                    .align = Z_ALIGN_CENTER))));
    col.children[k++] = Background(Z_COLOR_PRIMARY,
        Button(add_note, "Add note"));
    col.children[k++] = rows_panel(state);
    col.children[k++] = Spacer();

    return Background(Z_COLOR_BG,
        z_stack(Z_AXIS_VERTICAL, &col));
}

Z_APP_ID(NotepadState, notepad_body, "os.zelto.notepad")
