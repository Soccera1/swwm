#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "parser.h" // Changed to parser.h
#include "defs.h"   // Includes new defs.h
// For swwm_server, if needed during parsing (e.g. for logging, or immediate EWMH updates)
// #include "swwm.h" // Assuming swwm.h will exist and declare swwm_server

// Mapped to new function signatures in defs.h
static const struct {
	const char *name;
	void (*fn)(struct swwm_server *server, const void *arg);
} call_table[] = {
    {"close_window", close_focused_swwm},
    {"decrease_gaps", dec_gaps_swwm},
    {"focus_next", focus_next_swwm},
    {"focus_prev", focus_prev_swwm},
    {"increase_gaps", inc_gaps_swwm},
    {"master_next", move_master_next_swwm},
    {"master_previous", move_master_prev_swwm},
    {"quit", quit_swwm},
    {"reload_config", reload_config_swwm},
    {"master_increase", resize_master_add_swwm},
    {"master_decrease", resize_master_sub_swwm},
    {"toggle_floating", toggle_floating_swwm},
    {"global_floating", toggle_floating_global_swwm},
    {"fullscreen", toggle_fullscreen_swwm},
    {NULL, NULL}
};


static void remap_and_dedupe_binds(Config *cfg)
{
	for (int i = 0; i < cfg->bindsn; i++) {
		for (int j = i + 1; j < cfg->bindsn; j++) {
			if (cfg->binds[i].mods == cfg->binds[j].mods && cfg->binds[i].keysym == cfg->binds[j].keysym) {
				// Free duplicated command arrays if any
                if (cfg->binds[j].type == TYPE_CMD && cfg->binds[j].action.cmd) {
                    for(int k=0; cfg->binds[j].action.cmd[k] != NULL; ++k) {
                        free((void*)cfg->binds[j].action.cmd[k]);
                    }
                    free(cfg->binds[j].action.cmd);
                }
				memmove(&cfg->binds[j], &cfg->binds[j + 1], sizeof(Binding) * (cfg->bindsn - j - 1));
				cfg->bindsn--;
				j--;
			}
		}
	}
}

static char *strip(char *s)
{
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	char *e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e)) {
		*e-- = '\0';
	}
	return s;
}

static char *strip_quotes(char *s)
{
	size_t L = strlen(s);
	if (L > 0 && s[0] == '"') {
		s++;
		L--;
	}
	if (L > 0 && s[L - 1] == '"') {
		s[L - 1] = '\0';
	}
	return s;
}

static Binding *alloc_bind(Config *cfg, uint32_t mods, xkb_keysym_t ks)
{
	for (int i = 0; i < cfg->bindsn; i++) {
		if (cfg->binds[i].mods == mods && cfg->binds[i].keysym == ks) {
            // Free old command if overwriting
            if (cfg->binds[i].type == TYPE_CMD && cfg->binds[i].action.cmd) {
                 for(int k=0; cfg->binds[i].action.cmd[k] != NULL; ++k) {
                    free((void*)cfg->binds[i].action.cmd[k]);
                }
                free(cfg->binds[i].action.cmd);
                cfg->binds[i].action.cmd = NULL;
            }
			return &cfg->binds[i];
		}
	}
	if (cfg->bindsn >= (int)(sizeof(cfg->binds)/sizeof(cfg->binds[0]))) {
		fprintf(stderr, "swwm: too many binds, max %zu\n", sizeof(cfg->binds)/sizeof(cfg->binds[0]));
		return NULL;
	}
	Binding *b = &cfg->binds[cfg->bindsn++];
	b->mods = mods;
	b->keysym = ks;
	b->action.cmd = NULL; // Initialize
	b->action.fn = NULL;
	b->arg = NULL;
	return b;
}

uint32_t parse_mods_str(const char *combo, Config *cfg)
{
	uint32_t m = 0;
	char buf[256];
	strncpy(buf, combo, sizeof buf - 1);
    buf[sizeof buf -1] = '\0';

	for (char *p = buf; *p; p++) { // Normalize separators to '+'
		if (*p == '+' || isspace((unsigned char)*p)) {
			*p = '+';
		}
	}

	char *tok_ptr;
	for (char *tok = strtok_r(buf, "+", &tok_ptr); tok; tok = strtok_r(NULL, "+", &tok_ptr)) {
		// Convert token to lower case for matching
        for (char *q = tok; *q; q++) {
            *q = tolower((unsigned char)*q);
        }
		if (!strcmp(tok, "mod"))    m |= cfg->modkey; // modkey is already a SWM_MOD_* value
		else if (!strcmp(tok, "shift")) m |= SWM_MOD_SHIFT;
		else if (!strcmp(tok, "ctrl"))  m |= SWM_MOD_CTRL;
		else if (!strcmp(tok, "alt"))   m |= SWM_MOD_ALT;
		else if (!strcmp(tok, "super")) m |= SWM_MOD_LOGO;
        else if (!strcmp(tok, "caps")) m |= SWM_MOD_CAPS;
        else if (!strcmp(tok, "mod2")) m |= SWM_MOD_MOD2;
        else if (!strcmp(tok, "mod3")) m |= SWM_MOD_MOD3;
        else if (!strcmp(tok, "mod5")) m |= SWM_MOD_MOD5;
        // Keysym part is handled separately
	}
	return m;
}

static xkb_keysym_t parse_keysym_part(const char *combo) {
    xkb_keysym_t ks = XKB_KEY_NoSymbol;
    char buf[256];
    strncpy(buf, combo, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    for (char *p = buf; *p; p++) {
		if (*p == '+' || isspace((unsigned char)*p)) {
			*p = '+';
		}
	}
    
    char *tok_ptr;
    for (char *tok = strtok_r(buf, "+", &tok_ptr); tok; tok = strtok_r(NULL, "+", &tok_ptr)) {
        // try parsing current token as keysym if it's not a known modifier string
        // (this is a bit naive, assumes keysym is the last non-modifier part)
        char lower_tok[256];
        strncpy(lower_tok, tok, sizeof(lower_tok)-1);
        lower_tok[sizeof(lower_tok)-1] = '\0';
        for(char *q = lower_tok; *q; ++q) *q = tolower((unsigned char)*q);

        if (strcmp(lower_tok, "mod") != 0 && strcmp(lower_tok, "shift") != 0 &&
            strcmp(lower_tok, "ctrl") != 0 && strcmp(lower_tok, "alt") != 0 &&
            strcmp(lower_tok, "super") != 0 && strcmp(lower_tok, "caps") != 0 &&
            strcmp(lower_tok, "mod2") != 0 && strcmp(lower_tok, "mod3") != 0 &&
            strcmp(lower_tok, "mod5") != 0) {
            ks = parse_keysym_str(tok);
            if (ks != XKB_KEY_NoSymbol) break; // Found a keysym
        }
    }
    return ks;
}


int parser(struct swwm_server *server, Config *cfg)
{
	char path[PATH_MAX];
	const char *home = getenv("HOME");
	if (!home) {
		fprintf(stderr, "swwm: HOME not set, cannot find config\n");
		return -1;
	}

	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    bool found_path = false;

	if (xdg_config_home) {
		snprintf(path, sizeof path, "%s/swwm/swwmrc", xdg_config_home);
		if (access(path, R_OK) == 0) { found_path = true; goto found; }
        snprintf(path, sizeof path, "%s/swwmrc", xdg_config_home); // Old sxwm location
		if (access(path, R_OK) == 0) { found_path = true; goto found; }
	}

	snprintf(path, sizeof path, "%s/.config/swwm/swwmrc", home);
	if (access(path, R_OK) == 0) { found_path = true; goto found; }
    snprintf(path, sizeof path, "%s/.config/sxwmrc", home); // Old sxwm location
	if (access(path, R_OK) == 0) { found_path = true; goto found; }
    
	// Fallback to system-wide, e.g. /usr/local/share/swwm/swwmrc or /etc/swwmrc
	snprintf(path, sizeof path, "/usr/local/share/swwm/swwmrc");
	if (access(path, R_OK) == 0) { found_path = true; goto found; }


found:;
    if (!found_path) {
        fprintf(stderr, "swwm: no config file found.\n");
        return -1; // No config found is an error for this parser
    }
    
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "swwm: cannot open config %s: %s\n", path, strerror(errno));
		return -1;
	}
    fprintf(stdout, "swwm: loading config from %s\n", path);


	char line[512];
	int lineno = 0;
	
    // Initialize should_float related parts of cfg
    cfg->should_floatn = 0;
    for (int j = 0; j < 256; j++) {
		cfg->should_float[j] = NULL; // Will be allocated by build_argv logic if needed
	}


	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *s = strip(line);
		if (!*s || *s == '#') {
			continue;
		}

		char *sep = strchr(s, ':');
		if (!sep) {
			fprintf(stderr, "swwmrc:%d: missing ':' in line: %s\n", lineno, s);
			continue;
		}
		*sep = '\0';
		char *key = strip(s);
		char *rest = strip(sep + 1);
        
        // Remove trailing comments from 'rest'
        char *comment_start = strchr(rest, '#');
        if (comment_start && (comment_start == rest || isspace((unsigned char)*(comment_start-1)) ) ) {
            *comment_start = '\0';
            rest = strip(rest);
        }


		if (!strcmp(key, "mod_key")) {
			uint32_t m = parse_mods_str(rest, cfg); // parse_mods_str itself doesn't use cfg->modkey
			if (m != 0) { // Check if any known modifier was parsed
				cfg->modkey = m;
			} else {
				fprintf(stderr, "swwmrc:%d: unknown mod_key '%s'\n", lineno, rest);
			}
		} else if (!strcmp(key, "gaps")) {
			cfg->gaps = atoi(rest);
		} else if (!strcmp(key, "border_width")) {
			cfg->border_width = atoi(rest);
		} else if (!strcmp(key, "focused_border_colour")) {
			cfg->border_foc_col_val = parse_col_str(rest);
		} else if (!strcmp(key, "unfocused_border_colour")) {
			cfg->border_ufoc_col_val = parse_col_str(rest);
		} else if (!strcmp(key, "swap_border_colour")) {
			cfg->border_swap_col_val = parse_col_str(rest);
		} else if (!strcmp(key, "master_width")) {
			float mf = atoi(rest) / 100.0f;
            if (mf < MF_MIN) mf = MF_MIN;
            if (mf > MF_MAX) mf = MF_MAX;
			for (int i = 0; i < MAX_MONITORS; i++) {
				cfg->master_width[i] = mf;
			}
		} else if (!strcmp(key, "motion_throttle_hz")) {
			cfg->motion_throttle_hz = atoi(rest);
            if (cfg->motion_throttle_hz <= 0) cfg->motion_throttle_hz = 60; // Default
		} else if (!strcmp(key, "resize_master_amount")) {
			cfg->resize_master_amt = atoi(rest);
		} else if (!strcmp(key, "snap_distance")) {
			cfg->snap_distance = atoi(rest);
		} else if (!strcmp(key, "should_float")) { // app_id,app_id2
            if (cfg->should_floatn >= 256) {
                fprintf(stderr, "swwmrc:%d: too many should_float entries\n", lineno);
                continue;
            }
            char* app_id_list = strip(rest);
            char* current_app_id = strtok(app_id_list, ",");
            while(current_app_id != NULL) {
                if (cfg->should_floatn < 256) {
                    // Each entry in should_float is a single app_id string
                    cfg->should_float[cfg->should_floatn++] = strdup(strip(current_app_id));
                } else {
                    fprintf(stderr, "swwmrc:%d: ran out of space for should_float entries\n", lineno);
                    break;
                }
                current_app_id = strtok(NULL, ",");
            }
        } else if (!strcmp(key, "call") || !strcmp(key, "bind")) {
			char *mid = strchr(rest, ':');
			if (!mid) {
				fprintf(stderr, "swwmrc:%d: '%s' missing action/command part after keys\n", lineno, key);
				continue;
			}
			*mid = '\0';
			char *combo_str = strip(rest);
			char *act_str = strip(mid + 1);

			uint32_t mods = parse_mods_str(combo_str, cfg);
            xkb_keysym_t ks = parse_keysym_part(combo_str);

			if (ks == XKB_KEY_NoSymbol) {
				fprintf(stderr, "swwmrc:%d: bad key in combo '%s'\n", lineno, combo_str);
				continue;
			}
			Binding *b = alloc_bind(cfg, mods, ks);
			if (!b) {
				// alloc_bind already printed error
				continue; 
			}

			if (!strcmp(key, "bind")) { // "bind" is for external commands
				b->type = TYPE_CMD;
				b->action.cmd = build_argv(strip_quotes(act_str));
                b->arg = (void*)b->action.cmd; // For spawn_swwm
			} else { // "call" is for internal functions
				b->type = TYPE_FUNC;
				bool found_fn = false;
				for (int i = 0; call_table[i].name; i++) {
					if (!strcmp(act_str, call_table[i].name)) {
						b->action.fn = call_table[i].fn;
                        b->arg = NULL; // No specific arg for these simple functions
						found_fn = true;
						break;
					}
				}
				if (!found_fn) {
					fprintf(stderr, "swwmrc:%d: unknown function '%s'\n", lineno, act_str);
                    // Invalidate this binding
                    b->keysym = XKB_KEY_NoSymbol; 
                    cfg->bindsn--; // Effectively remove it
				}
			}
		} else if (!strcmp(key, "workspace")) {
			char *mid = strchr(rest, ':');
			if (!mid) {
				fprintf(stderr, "swwmrc:%d: workspace binding missing action part (e.g., 'move 1' or 'swap 1')\n", lineno);
				continue;
			}
			*mid = '\0';
			char *combo_str = strip(rest);
			char *act_str = strip(mid + 1);

			uint32_t mods = parse_mods_str(combo_str, cfg);
            xkb_keysym_t ks = parse_keysym_part(combo_str);

			if (ks == XKB_KEY_NoSymbol) {
				fprintf(stderr, "swwmrc:%d: bad key in workspace combo '%s'\n", lineno, combo_str);
				continue;
			}
			Binding *b = alloc_bind(cfg, mods, ks);
			if (!b) {
				continue;
			}

			int ws_num_parsed;
			if (sscanf(act_str, "move %d", &ws_num_parsed) == 1) {
				if (ws_num_parsed >= 1 && ws_num_parsed <= NUM_WORKSPACES) {
					b->type = TYPE_CWKSP; // Change current view to workspace
					b->action.ws = ws_num_parsed - 1; // 0-indexed
                    b->arg = (void*)(intptr_t)b->action.ws;
				} else {
					fprintf(stderr, "swwmrc:%d: invalid workspace number '%d' for 'move'\n", lineno, ws_num_parsed);
				}
			} else if (sscanf(act_str, "swap %d", &ws_num_parsed) == 1) {
                 if (ws_num_parsed >= 1 && ws_num_parsed <= NUM_WORKSPACES) {
					b->type = TYPE_MWKSP; // Move focused window to workspace
					b->action.ws = ws_num_parsed - 1; // 0-indexed
                    b->arg = (void*)(intptr_t)b->action.ws;
				} else {
					fprintf(stderr, "swwmrc:%d: invalid workspace number '%d' for 'swap'\n", lineno, ws_num_parsed);
				}
			} else {
				fprintf(stderr, "swwmrc:%d: invalid workspace action '%s'\n", lineno, act_str);
			}
		}
		else {
			fprintf(stderr, "swwmrc:%d: unknown option '%s'\n", lineno, key);
		}
	}

	fclose(f);
	remap_and_dedupe_binds(cfg); // Must be done after all binds are potentially allocated
	return 0;
}


xkb_keysym_t parse_keysym_str(const char *key_str) {
    xkb_keysym_t ks = xkb_keysym_from_name(key_str, XKB_KEYSYM_CASE_INSENSITIVE);
    if (ks == XKB_KEY_NoSymbol) {
        // sxwm tries various capitalizations. xkb_keysym_from_name with CASE_INSENSITIVE
        // should handle most common cases. If not, one might need more specific handling
        // for names like "Return" vs "return", etc.
        // Most single character keysyms are just the character e.g. "a", "1"
        // Special keys are like "space", "Return", "Escape", "F1"
        fprintf(stderr, "swwm: unknown keysym '%s'\n", key_str);
    }
    return ks;
}


unsigned long parse_col_str(const char *hex) {
    // This is a placeholder. XParseColor is X11 specific.
    // For Wayland, you'd typically parse into R, G, B, A components
    // and store them or a combined uint32_t.
    // Example: #RRGGBB or #AARRGGBB
    // For simplicity, let's just try to convert hex string to long.
    // This won't be directly usable for wlr_renderer without further processing.
    if (!hex || hex[0] != '#') return 0; // Invalid format
    char *endptr;
    unsigned long val = strtoul(hex + 1, &endptr, 16);
    if (*endptr != '\0') {
        fprintf(stderr, "swwm: invalid color string '%s'\n", hex);
        return 0; // Or a default color
    }
    // If it's RRGGBB, we might want to add full alpha FF000000
    if (strlen(hex + 1) == 6) { // RRGGBB
        val |= 0xFF000000; // Assume full alpha, set AARRGGBB for wlr_render_rect
    } else if (strlen(hex+1) == 8) { // AARRGGBB
        // It's already in AARRGGBB, but typical web is RRGGBBAA
        // Let's assume #RRGGBB for now and add FF alpha
        // Or if #AARRGGBB is expected, this logic needs to be more robust
    }
    return val; // This value is not directly usable as pixel for X11.
                // For wlroots, if drawing, this might be an AARRGGBB or RRGGBBAA value.
}


const char **build_argv(const char *cmd)
{
	char *dup = strdup(cmd);
    if (!dup) {
        perror("strdup in build_argv");
        return NULL;
    }

	char *saveptr = NULL;
	const char **argv = malloc(MAX_ARGS * sizeof(*argv));
    if (!argv) {
        perror("malloc for argv in build_argv");
        free(dup);
        return NULL;
    }
	int i = 0;

    // strtok_r is safer than strtok
	char *tok = strtok_r(dup, " \t", &saveptr);
	while (tok && i < MAX_ARGS - 1) {
        // sxwm's parser had more complex quote handling. This is simplified.
        // It assumes arguments are space-separated. Quotes are part of the arg.
        // For more robust shell-like parsing, a small state machine is needed.
        argv[i++] = strdup(tok);
        if (!argv[i-1]) {
            perror("strdup for argv element");
            // Free previously strdup'd elements
            for (int k = 0; k < i - 1; ++k) free((void*)argv[k]);
            free(argv);
            free(dup);
            return NULL;
        }
		tok = strtok_r(NULL, " \t", &saveptr);
	}
	argv[i] = NULL;
	free(dup);
	return argv;
}
