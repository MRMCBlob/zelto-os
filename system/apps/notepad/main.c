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
    s->db = z_db_open("notepad");
    if (s->db) {
        z_db_exec(s->db,
                  "CREATE TABLE IF NOT EXISTS notes ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT, n INTEGER, note TEXT)");
    }
    refresh_rows(s);
}

// "Add note": persist a bumped counter (prefs) + INSERT a row (DB), then reload.
static void add_note(ZApp *app, void *state) {
    NotepadState *s = state;
    s->count++;
    z_prefs_set_int("count", s->count);
    if (s->db) {
        char note[48];
        snprintf(note, sizeof(note), "note number %lld", (long long)s->count);
        z_db_run(s->db, "INSERT INTO notes(n, note) VALUES(?, ?)",
                 z_args(s->count, note));
        refresh_rows(s);
    }
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
    (void)app;
    ensure_init(state);

    return Background(z_rgba(0x14, 0x18, 0x24, 0xff),
        VStack(
            // Leading spacer balances the trailing one so the content stays
            // vertically centred (and the title isn't clipped under the bar).
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_TITLE, Text("Notepad"))),
            // The persisted counter, big and obvious for the capture.
            Frame(160.0f, 110.0f,
                Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
                    CornerRadius(24,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_LARGE_TITLE,
                                Text("%lld", (long long)state->count)))))),
            // The headline persistence proof: what we read back from disk.
            Foreground(z_rgba(0x9a, 0xc4, 0xf0, 0xff),
                Font(Z_FONT_CALLOUT,
                    Text("loaded count=%lld from disk", (long long)state->loaded))),
            Background(z_rgba(0xfa, 0x66, 0x26, 0xff),
                Button(add_note, "Add note")),
            rows_panel(state),
            Spacer(),
            .padding = 24, .spacing = 14, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(NotepadState, notepad_body, "os.zelto.notepad")
