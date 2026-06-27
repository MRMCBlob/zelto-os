// zcomp entry point. Brings up the compositor and runs the event loop until the
// display terminates. See docs/contributing/compositor-internals.md.
#include <stdlib.h>

#include <wlr/util/log.h>

#include "zcomp/server.h"
#include "zcomp/zcomp.h"

int main(void) {
    wlr_log_init(WLR_DEBUG, NULL);
    wlr_log(WLR_INFO, "%s %s starting", ZCOMP_NAME, ZCOMP_VERSION);

    ZcompServer server = {0};
    if (!zcomp_server_init(&server)) {
        wlr_log(WLR_ERROR, "zcomp: failed to initialize");
        return EXIT_FAILURE;
    }

    zcomp_server_run(&server);
    zcomp_server_finish(&server);

    wlr_log(WLR_INFO, "zcomp: clean exit");
    return EXIT_SUCCESS;
}
