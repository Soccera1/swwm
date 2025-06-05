#pragma once
#include "defs.h" // Uses new defs.h
#include <xkbcommon/xkbcommon.h> // For KeySym

#define MAX_ARGS 64 // Already in defs.h, keep for direct include

const char **build_argv(const char *cmd);
int parser(struct swwm_server *server, Config *user_config); // Now takes server for potential immediate actions or logging
uint32_t parse_mods_str(const char *mods_str, Config *user_config); // Renamed to avoid conflict if any
xkb_keysym_t parse_keysym_str(const char *key_str); // Renamed
unsigned long parse_col_str(const char *hex); // For color parsing

#endif // PARSER_H
