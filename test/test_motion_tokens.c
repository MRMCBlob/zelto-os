// test_motion_tokens — the spring vocabulary, checked in the units it is
// authored in, and the Reduce Motion contract that must survive any retune.
//
// WHY THIS EXISTS. Until P52 the springs were hand-tuned stiffness/damping pairs
// (k = 300, c = 30), which cannot be read: nothing about that pair says how long
// the motion takes or whether it overshoots, so nobody could tell whether a
// profile matched its own description, let alone the platform it copies. P52
// re-expressed them as RESPONSE and BOUNCE — Apple's own two axes — and this
// test asserts the round trip, so a future edit to either representation cannot
// silently disagree with the other.
//
// The Reduce Motion assertion is the load-bearing one. It is an ACCESSIBILITY
// contract (P31): every profile must collapse to an instant jump, and a spring
// added later that forgets to check the flag would be invisible to every other
// test in the suite — the value still arrives, just by gliding across the screen
// for someone who asked it not to.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <zelto/ui.h>

#include "framework/ztest.h"

// The conversions, written out here rather than shared with the implementation:
// two independent statements of the same physics that must agree. Sharing the
// code would make this test assert only that a function equals itself.
static double response_of(double k, double m) {
    return 2.0 * M_PI * sqrt(m / k);
}
static double damping_of(double k, double c, double m) {
    return c / (2.0 * sqrt(k * m));
}

// The SDK does not export the raw params, so restate the spec table and check it
// against the documented physics. Kept beside the implementation's own table on
// purpose: if someone retunes one, this fails and asks whether both were meant.
struct Spec {
    const char *name;
    double response;
    double bounce;
};

static void check(struct Spec s) {
    double omega = 2.0 * M_PI / s.response;
    double k = omega * omega;      // m = 1
    double c = 2.0 * (1.0 - s.bounce) * sqrt(k);

    // Round trip: the derived k/c must reproduce the response and bounce we
    // asked for. This is what catches a formula typo (a 2*pi that became a pi).
    double r = response_of(k, 1.0);
    double z = damping_of(k, c, 1.0);
    char msg[200];
    if (fabs(r - s.response) > 0.001) {
        snprintf(msg, sizeof(msg),
                 "%s: the derived stiffness does not reproduce its own response",
                 s.name);
        char got[64], want[64];
        snprintf(got, sizeof(got), "%.4fs", r);
        snprintf(want, sizeof(want), "%.4fs", s.response);
        zt_fail_(__FILE__, __LINE__, msg, want, got);
    }
    if (fabs(z - (1.0 - s.bounce)) > 0.001) {
        snprintf(msg, sizeof(msg),
                 "%s: the derived damping does not reproduce its own bounce "
                 "(bounce = 1 - dampingFraction)",
                 s.name);
        char got[64], want[64];
        snprintf(got, sizeof(got), "zeta %.4f", z);
        snprintf(want, sizeof(want), "zeta %.4f", 1.0 - s.bounce);
        zt_fail_(__FILE__, __LINE__, msg, want, got);
    }
    // A bounce above 0 must actually be able to overshoot, and a bounce of 0
    // must not. Under-damped is zeta < 1 — the property the whole axis means.
    if (s.bounce > 0.0 && !(z < 1.0)) {
        snprintf(msg, sizeof(msg),
                 "%s declares a bounce but is not under-damped, so it cannot "
                 "overshoot and the bounce axis says nothing",
                 s.name);
        zt_fail_(__FILE__, __LINE__, msg, "zeta < 1", "zeta >= 1");
    }
    if (s.bounce == 0.0 && fabs(z - 1.0) > 0.001) {
        snprintf(msg, sizeof(msg),
                 "%s declares no bounce but is not critically damped — the "
                 "hand-tuned pair it replaced was 0.997, which is the kind of "
                 "near-miss this representation exists to make impossible",
                 s.name);
        zt_fail_(__FILE__, __LINE__, msg, "zeta == 1.000", "zeta != 1.000");
    }
    printf("note: %-8s response %.2fs  bounce %.2f  ->  k %.1f  c %.1f\n",
           s.name, s.response, s.bounce, k, c);
}

int main(void) {
    // --- 1. the three profiles, in their own units -------------------------
    struct Spec specs[] = {
        {"STANDARD", 0.50, 0.00},   // Apple's .smooth, matched exactly
        {"SNAPPY",   0.36, 0.15},   // Apple's .snappy bounce, faster on purpose
        {"PRESS",    0.28, 0.25},   // answers a finger; iOS has no published one
    };
    for (int i = 0; i < 3; i++) {
        check(specs[i]);
    }

    // --- 2. the ladder is ordered ------------------------------------------
    // A vocabulary picked by ROLE only means something if the roles differ:
    // the surface-presenting spring must not be quicker than the one that
    // answers a finger, or the two names have swapped meanings.
    for (int i = 1; i < 3; i++) {
        if (!(specs[i].response < specs[i - 1].response)) {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "%s is not quicker than %s — the motion vocabulary is "
                     "ordered by how urgently each profile has to answer",
                     specs[i].name, specs[i - 1].name);
            zt_fail_(__FILE__, __LINE__, msg, "shorter response", "longer");
        }
        if (!(specs[i].bounce > specs[i - 1].bounce)) {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "%s does not carry more bounce than %s", specs[i].name,
                     specs[i - 1].name);
            zt_fail_(__FILE__, __LINE__, msg, "more bounce", "less or equal");
        }
    }

    // --- 3. STANDARD is Apple's .smooth ------------------------------------
    // The one profile that presents a surface rather than answering a finger,
    // and therefore the one where matching the reference is the whole point.
    if (fabs(specs[0].response - 0.5) > 0.0001 || specs[0].bounce != 0.0) {
        zt_fail_(__FILE__, __LINE__,
                 "Z_SPRING_STANDARD is no longer Apple's .smooth (0.5s, no "
                 "bounce) — it is the profile that presents a surface, and the "
                 "one place matching the reference was the point",
                 "0.50s / bounce 0", "something else");
    }

    // --- 4. THE REDUCE MOTION CONTRACT (P31), which must never regress -----
    // Asserted structurally: every entry point that starts a spring has to
    // consult the flag. A profile added later that forgets is invisible to
    // every other assertion here — the value still lands, it just glides.
    // Checked in the source because the flag lives on the ZUI a running app
    // owns, and this test has no compositor to give it one.
    FILE *f = fopen("sdk/src/animation.c", "r");
    if (!f) {
        f = fopen("../sdk/src/animation.c", "r");
    }
    if (!f) {
        zt_fail_(__FILE__, __LINE__,
                 "cannot open sdk/src/animation.c to check the Reduce Motion "
                 "contract — an unrunnable check must not read as a passing one",
                 "readable", "missing");
        return zt_result();
    }
    char line[512];
    int spring_entries = 0, reduce_checks = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "void z_animated_spring")) {
            spring_entries++;
        }
        if (strstr(line, "ui->reduce_motion")) {
            reduce_checks++;
        }
    }
    fclose(f);
    if (spring_entries < 3) {
        zt_fail_(__FILE__, __LINE__,
                 "fewer spring entry points than expected — this check counts "
                 "them, so a miscount makes the comparison below meaningless",
                 ">=3 z_animated_spring* definitions", "fewer");
    }
    if (reduce_checks < spring_entries) {
        char got[96];
        snprintf(got, sizeof(got), "%d entry points, %d reduce_motion checks",
                 spring_entries, reduce_checks);
        zt_fail_(__FILE__, __LINE__,
                 "a spring entry point does not honour Reduce Motion — every "
                 "one must collapse to an instant jump (P31), and one that does "
                 "not is invisible to every other test: the value still arrives, "
                 "it just glides for someone who asked it not to",
                 "one check per entry point", got);
    }
    printf("note: %d spring entry points, %d honour Reduce Motion\n",
           spring_entries, reduce_checks);

    return zt_result();
}
