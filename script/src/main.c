// `zelto-script` — run a Zelto Script app.
//
//   zelto-script [--id APP_ID] [--title TITLE] ENTRY.js
//
// This is what a script app's manifest points `exec=` at; the id should match
// the manifest's `id=` so the shell (launcher, switcher, permissions) sees one
// identity for the app. See docs/zelto-script/runtime.md.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zelto/script.h"

static int usage(void) {
    fprintf(stderr,
            "usage: zelto-script [--android] [--id APP_ID] [--title TITLE] "
            "ENTRY.js\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *entry = NULL;
    const char *app_id = NULL;
    const char *title = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            app_id = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "--android") == 0) {
            // andemu app: mark the runtime so the guest can tune its Android
            // compat layer. The android/* modules are always resolvable; this
            // flag is the app's declared identity (installer sets it from
            // runtime=andemu). The host reads $ZELTO_ANDROID.
            setenv("ZELTO_ANDROID", "1", 1);
        } else if (argv[i][0] == '-') {
            return usage();
        } else if (!entry) {
            entry = argv[i];
        } else {
            return usage();
        }
    }

    if (!entry) { return usage(); }
    return z_script_main(entry, app_id, title);
}
