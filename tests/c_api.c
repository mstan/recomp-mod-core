#include "recomp_mod.h"
int main(void) {
    rmod_context* context = rmod_create();
    if (!context) return 1;
    if (rmod_initialize(context, "unused", "fixture", "invalid-hash")) return 2;
    if (!rmod_error(context) || !rmod_error(context)[0]) return 3;
    rmod_destroy(context);
    rmod_destroy(0);
    return 0;
}
