#pragma once
#include "loader/so_loader.h"

// game-specific code patches for v0.0.29. call after relocation but before
// so_finalize, text is still writable then. every patch checks the original
// bytes first so a different apk version just skips instead of breaking.
void patches_apply(so_module *game);

// ux0:data/nba2k16/config.txt, "key=value" lines, read once
int config_int(const char *key, int def);
