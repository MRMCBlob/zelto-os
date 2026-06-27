// zcomp stub entry point.
//
// Built only when the graphics stack (wlroots/EGL/...) is not available, so that
// `meson setup` + `ninja` work on a bare host and inside an early initramfs
// before DRM is wired up. It prints its identity and exits 0, which is enough to
// prove the boot path reaches the compositor (Step 5 in the bootstrap task).
#include <stdio.h>

#include "zcomp/zcomp.h"

int main(void) {
    printf("%s %s: stub build (no graphics stack)\n", ZCOMP_NAME, ZCOMP_VERSION);
    printf("%s: boot path reached compositor; exiting 0\n", ZCOMP_NAME);
    return 0;
}
