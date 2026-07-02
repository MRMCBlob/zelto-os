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
    snprintf(s->note.text, sizeof(s->note.text), "hello world");
    s->note.len = (int)strlen(s->note.text);
    s->note.caret = s->note.anchor = s->note.len;
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
    opts.children[k++] = Foreground(z_rgba(0xbf, 0xd8, 0xcc, 0xff),
        Font(Z_FONT_CAPTION, Text("DB rows (newest first)")));
    if (s->row_count == 0) {
        opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_BODY, Text("(no rows yet)")));
    }
    for (int i = 0; i < s->row_count && k < Z_MAX_CHILDREN; i++) {
        opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_BODY, Text("%s", s->rows[i])));
    }
    return Background(z_rgba(0x1d, 0x6e, 0x44, 0xff),
        CornerRadius(14, Frame(520.0f, 0.0f, z_stack(Z_AXIS_VERTICAL, &opts))));
}

static ZView notepad_body(ZApp *app, NotepadState *state) {
    ensure_init(state);

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
    col.children[k++] = Frame(140.0f, 72.0f,
        Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
            CornerRadius(20,
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_TITLE,
                        Text("%lld", (long long)state->count))))));
    col.children[k++] = Background(z_rgba(0xfa, 0x66, 0x26, 0xff),
        Button(add_note, "Add note"));
    col.children[k++] = rows_panel(state);
    col.children[k++] = Spacer();

    return Background(z_rgba(0x14, 0x18, 0x24, 0xff),
        z_stack(Z_AXIS_VERTICAL, &col));
}

Z_APP_ID(NotepadState, notepad_body, "os.zelto.notepad")
