// Shared launch helper for the System UI (launcher + zsysd).
//
// A manifest's `exec=` is a COMMAND, not just a path: it may carry arguments,
// because an interpreted app is launched through its runtime —
//
//   exec=/usr/bin/zelto-script --id os.zelto.jsdemo /usr/share/zelto/scripts/jsdemo.js
//
// Both launch sites (the launcher's tile tap, zsysd's launch-if-needed intent
// delivery) split that command the same way here, so a script app and a native
// app are launched by identical machinery.
//
// Splitting is on whitespace, with no quoting or shell involvement: a manifest
// is a trusted, installer-written file, not a shell script, and keeping /bin/sh
// out of the launch path means an odd character in a path cannot become code.
// A path with spaces is therefore not expressible — an acceptable trade for a
// launch surface with no shell in it.
#ifndef ZELTO_EXEC_CMD_H
#define ZELTO_EXEC_CMD_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Max argv entries (including argv[0]) a manifest command may expand to.
#define ZELTO_EXEC_MAX_ARGS 8

// Split `cmd` into argv and exec it. Only ever called in the forked child, so
// it either replaces the process image or returns (leaving the caller to _exit).
// `cmd` is copied into `buf` (caller-owned, size `buf_len`) because splitting
// writes NULs into it.
static inline void z_exec_cmd(const char *cmd, char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%s", cmd);

    char *argv[ZELTO_EXEC_MAX_ARGS + 1];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save);
         tok && argc < ZELTO_EXEC_MAX_ARGS;
         tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) { return; }
    argv[argc] = NULL;

    execvp(argv[0], argv);
}

#endif  // ZELTO_EXEC_CMD_H
