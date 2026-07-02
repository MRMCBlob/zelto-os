// Zelto demo app — "Store" (the P13 runtime installer UI).
//
// A minimal stand-in for an app store: it drives `zelto-install` on a staged
// .zap and reports the result on screen. Two buttons:
//
//   - "Install Widget"   -> zelto-install /usr/share/zelto/packages/widget.zap
//        A correctly signed package whose file hashes match. The installer
//        verifies the Ed25519 signature + every file hash, unpacks it onto the
//        persistent disk under /var/zelto, and registers its manifest; exit 0.
//        The launcher then shows a Widget tile (on its next startup scan / after
//        a reboot) and tapping it runs the installed binary.
//
//   - "Install tampered" -> zelto-install /usr/share/zelto/packages/widget-bad.zap
//        A package whose binary was altered after signing, so a file hash no
//        longer matches MANIFEST.sha256. The installer refuses it (non-zero exit);
//        nothing is registered. This proves integrity enforcement.
//
// The install runs synchronously in the button handler (fork + execl + waitpid):
// verifying + unpacking a tiny package is fast, and a modal "installing" beat is
// fine for the demo. See docs/packaging/zap-format.md.
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <zelto/ui.h>

#define INSTALL_BIN "/usr/bin/zelto-install"
#define WIDGET_ZAP "/usr/share/zelto/packages/widget.zap"
#define WIDGET_BAD_ZAP "/usr/share/zelto/packages/widget-bad.zap"

typedef struct StoreState {
    int last_rc;        // exit code of the last install (-1 = none yet)
    bool ran;           // have we run an install this session?
    bool last_ok;       // did the last install succeed?
    char last_msg[160]; // a human-readable result line
} StoreState;

// Run `zelto-install <zap>` to completion and return its exit code (-1 on a
// fork/exec failure). The installer prints its own diagnostics to stderr (the
// guest serial log); we only need the exit code to drive the UI.
static int run_install(const char *zap) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl(INSTALL_BIN, "zelto-install", zap, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        // retry on EINTR
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static void record(StoreState *s, const char *zap, const char *label) {
    int rc = run_install(zap);
    s->ran = true;
    s->last_rc = rc;
    s->last_ok = (rc == 0);
    if (rc == 0) {
        snprintf(s->last_msg, sizeof(s->last_msg), "%s installed OK (exit 0)",
                 label);
    } else {
        snprintf(s->last_msg, sizeof(s->last_msg),
                 "%s rejected (exit %d) — not installed", label, rc);
    }
}

static void install_widget(ZApp *app, void *state) {
    record(state, WIDGET_ZAP, "Widget");
    z_invalidate(app);
}

static void install_tampered(ZApp *app, void *state) {
    record(state, WIDGET_BAD_ZAP, "Tampered package");
    z_invalidate(app);
}

static ZView store_body(ZApp *app, StoreState *state) {
    (void)app;
    ZColor panel_bg = !state->ran     ? z_rgba(0x2a, 0x30, 0x38, 0xff)
                      : state->last_ok ? z_rgba(0x1d, 0x5e, 0x3a, 0xff)
                                       : z_rgba(0x6e, 0x1d, 0x2a, 0xff);
    return Background(z_rgba(0x0e, 0x14, 0x1c, 0xff),
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Store"))),
            Foreground(z_rgba(0x9d, 0xb4, 0xc8, 0xff),
                Font(Z_FONT_CALLOUT,
                     Text("Install signed .zap packages onto the disk"))),
            Spacer(),
            Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
                Button(install_widget, "Install Widget")),
            Background(z_rgba(0xc8, 0x6b, 0x3a, 0xff),
                Button(install_tampered, "Install tampered")),
            Background(panel_bg,
                CornerRadius(14,
                    Frame(620.0f, 96.0f,
                        VStack(
                            Foreground(z_rgba(0xbf, 0xd8, 0xe8, 0xff),
                                Font(Z_FONT_CAPTION, Text("result"))),
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_BODY,
                                    Text("%s", state->ran
                                                   ? state->last_msg
                                                   : "(tap a button to install)"))),
                            .spacing = 8, .align = Z_ALIGN_LEADING,
                            .padding = 18)))),
            Spacer(),
            .padding = 32, .spacing = 16, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(StoreState, store_body, "os.zelto.store")
