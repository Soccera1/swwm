#define _POSIX_C_SOURCE 200809L // For fork, exec, setenv, SIGCHLD, SIG_IGN
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h> // For strcmp, strdup, etc.
#include <time.h>
#include <unistd.h>
#include <sys/wait.h> // For waitpid
#include <signal.h>   // For SIGCHLD, SIG_IGN
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>


#include "defs.h"   // Our new defs.h
#include "parser.h" // Our new parser.h
#include "config.h"

/* For brevity's sake, struct members are annotated where they are used. */
enum swwm_cursor_mode {
	SWM_CURSOR_PASSTHROUGH,
	SWM_CURSOR_MOVE,
	SWM_CURSOR_RESIZE,
    SWM_CURSOR_SWAP, // For dragging tiled windows to swap
};

struct swwm_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene; // Root scene node
    struct wlr_scene_tree *toplevel_layer; // Scene layer for toplevels
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	// struct wl_list toplevels; // Replaced by workspaces

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum swwm_cursor_mode cursor_mode;
	struct swwm_toplevel *grabbed_toplevel; // Toplevel being moved/resized
    struct swwm_toplevel *swap_target_toplevel; // Toplevel to swap with
	double grab_x, grab_y; // Cursor grab point relative to toplevel corner
	struct wlr_box grab_geobox; // Toplevel geometry at start of grab
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs; // swwm_output
	struct wl_listener new_output;

    // --- sxwm features ---
    Config config;
    struct swwm_workspace workspaces[NUM_WORKSPACES];
    int current_ws_idx;
    struct swwm_toplevel *focused_toplevel; // Currently keyboard-focused toplevel
    bool global_floating; // All new windows float, existing ones toggle
    bool next_toplevel_should_float; // For spawn commands configured to float
    long last_motion_time_msec; // For motion throttle
    // --- end sxwm features ---
};

struct swwm_output {
	struct wl_list link;
	struct swwm_server *server;
	struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output; // For rendering
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
    int idx; // Index in a potential server->output_array
    struct wlr_box usable_area; // Geometry excluding panels/docks (future)
};

struct swwm_toplevel {
	struct wl_list link; // Overall list in server (not used much now)
    struct wl_list workspace_link; // For linking within a workspace's list
	struct swwm_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree; // Scene node for this toplevel
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit; // Important for app_id
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
    struct wl_listener set_app_id; // To catch app_id changes

    // --- sxwm features integrated ---
    int ws_idx; // Workspace index it belongs to
    bool floating;
    bool fullscreen;
    // xdg_toplevel->surface->mapped is the equivalent of sxwm client->mapped
    struct wlr_box geom; // Last configured geometry (layout coords for tiling, absolute for floating)
    struct wlr_box saved_geom_tile; // Geometry before floating/fullscreen (if it was tiled)
    struct wlr_box saved_geom_float; // Geometry before fullscreen (if it was floating)
    // int mon_idx; // Implicit from output_layout and geom
    // --- end sxwm features ---
};

struct swwm_popup {
    // Unchanged from original swwm
	struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree; // For rendering popups
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct swwm_keyboard {
	struct wl_list link;
	struct swwm_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

// Forward declarations for internal functions
static void init_default_config(Config *config);
static void apply_config(struct swwm_server *server);
static void arrange_workspace(struct swwm_workspace *ws);
static void arrange_output(struct swwm_output *output);
static void arrange_all(struct swwm_server *server);
static void focus_toplevel(struct swwm_toplevel *toplevel, bool raise);
static void cycle_focus(struct swwm_server *server, bool forward);
static struct swwm_toplevel *get_toplevel_at(struct swwm_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
static void begin_interactive(struct swwm_toplevel *toplevel, enum swwm_cursor_mode mode, uint32_t edges);


// --- sxwm function ports (prototypes for clarity, definitions below) ---
void quit_swwm(struct swwm_server *server, const void *arg) {
    wl_display_terminate(server->wl_display);
}

void close_focused_swwm(struct swwm_server *server, const void *arg) {
    struct swwm_toplevel *toplevel = get_focused_toplevel(server);
    if (toplevel) {
        wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
    }
}

void focus_next_swwm(struct swwm_server *server, const void *arg) {
    cycle_focus(server, true);
}

void focus_prev_swwm(struct swwm_server *server, const void *arg) {
    cycle_focus(server, false);
}

void spawn_swwm(struct swwm_server *server, const void *arg_cmd_array) {
    const char **cmd = (const char **)arg_cmd_array;
    if (!cmd || !cmd[0]) return;

    // sxwm's pipe handling logic could be ported here if needed.
    // For now, simple exec.
    if (fork() == 0) {
        // Child process
        setsid(); // Create new session
        execvp(cmd[0], (char *const *)cmd);
        fprintf(stderr, "swwm: execvp '%s' failed: %s\n", cmd[0], strerror(errno));
        _exit(EXIT_FAILURE);
    }
    // Parent continues
    // Mark that next window might need special handling if configured via should_float
    // This is tricky as we don't know which window corresponds to this spawn yet.
    // sxwm sets `next_should_float`. We can do something similar.
    // Check if this command is in the should_float list (complex matching)
    // For now, this logic is simplified in `xdg_toplevel_map` using app_id.
}

void reload_config_swwm(struct swwm_server *server, const void *arg) {
    wlr_log(WLR_INFO, "Reloading config...");
    // Free old config resources (like should_float strings, command arrays in binds)
    for (int i = 0; i < server->config.bindsn; ++i) {
        if (server->config.binds[i].type == TYPE_CMD && server->config.binds[i].action.cmd) {
            for (int k = 0; server->config.binds[i].action.cmd[k]; ++k) {
                free((void*)server->config.binds[i].action.cmd[k]);
            }
            free(server->config.binds[i].action.cmd);
        }
    }
    for (int i = 0; i < server->config.should_floatn; ++i) {
        free(server->config.should_float[i]);
    }

    init_default_config(&server->config); // Re-init with defaults
    if (parser(server, &server->config) != 0) { // Parse user config file
        wlr_log(WLR_ERROR, "Failed to parse config file, using defaults for new settings.");
        // Stick to defaults already loaded by init_default_config
    }
    apply_config(server);
    arrange_all(server); // Re-tile everything with new settings (gaps, master_width)
    wlr_log(WLR_INFO, "Config reloaded.");
}

void change_workspace_action(struct swwm_server *server, const void *arg_ws_idx) {
    int new_ws_idx = (int)(intptr_t)arg_ws_idx;
    if (new_ws_idx < 0 || new_ws_idx >= NUM_WORKSPACES || new_ws_idx == server->current_ws_idx) {
        return;
    }

    wlr_log(WLR_DEBUG, "Changing to workspace %d", new_ws_idx);

    // Unmap/hide toplevels from old workspace
    struct swwm_workspace *old_ws = &server->workspaces[server->current_ws_idx];
    struct swwm_toplevel *toplevel_iter;
    wl_list_for_each(toplevel_iter, &old_ws->toplevels, workspace_link) {
        wlr_scene_node_set_enabled(&toplevel_iter->scene_tree->node, false);
    }
    wl_list_for_each(toplevel_iter, &old_ws->floating_toplevels, workspace_link) {
        wlr_scene_node_set_enabled(&toplevel_iter->scene_tree->node, false);
    }


    server->current_ws_idx = new_ws_idx;
    struct swwm_workspace *new_ws = &server->workspaces[server->current_ws_idx];

    // Map/show toplevels from new workspace
    wl_list_for_each(toplevel_iter, &new_ws->toplevels, workspace_link) {
        wlr_scene_node_set_enabled(&toplevel_iter->scene_tree->node, true);
    }
     wl_list_for_each(toplevel_iter, &new_ws->floating_toplevels, workspace_link) {
        wlr_scene_node_set_enabled(&toplevel_iter->scene_tree->node, true);
    }

    arrange_workspace(new_ws); // Arrange the new workspace

    // Focus the first or last focused toplevel on the new workspace
    struct swwm_toplevel *new_focus = NULL;
    if (!wl_list_empty(&new_ws->toplevels)) {
        new_focus = wl_container_of(new_ws->toplevels.next, new_focus, workspace_link);
    } else if (!wl_list_empty(&new_ws->floating_toplevels)) {
         new_focus = wl_container_of(new_ws->floating_toplevels.next, new_focus, workspace_link);
    }
    if (new_focus) {
        focus_toplevel(new_focus, true);
    } else {
        // No toplevels on new workspace, clear focus
        wlr_seat_keyboard_clear_focus(server->seat);
        server->focused_toplevel = NULL;
    }
}

void move_to_workspace_action(struct swwm_server *server, const void *arg_ws_idx) {
    int target_ws_idx = (int)(intptr_t)arg_ws_idx;
    struct swwm_toplevel *toplevel = get_focused_toplevel(server);

    if (!toplevel || target_ws_idx < 0 || target_ws_idx >= NUM_WORKSPACES || target_ws_idx == toplevel->ws_idx) {
        return;
    }

    wlr_log(WLR_DEBUG, "Moving toplevel to workspace %d", target_ws_idx);

    // Remove from current workspace list
    wl_list_remove(&toplevel->workspace_link);
    
    struct swwm_workspace *old_ws = &server->workspaces[toplevel->ws_idx];
    struct swwm_workspace *target_ws = &server->workspaces[target_ws_idx];

    toplevel->ws_idx = target_ws_idx;

    // Add to target workspace list (maintaining floating status)
    if (toplevel->floating) {
        wl_list_insert(target_ws->floating_toplevels.prev, &toplevel->workspace_link);
    } else {
        wl_list_insert(target_ws->toplevels.prev, &toplevel->workspace_link); // Add to end of tiled list
    }

    // Hide if current workspace is not the target workspace
    if (toplevel->ws_idx != server->current_ws_idx) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    } else {
         wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true); // Ensure visible if moved to current
    }


    arrange_workspace(old_ws); // Re-arrange old workspace
    if (target_ws_idx == server->current_ws_idx) {
        arrange_workspace(target_ws); // Re-arrange new workspace if it's the current one
    }
    
    // Focus next window in old workspace or clear focus if none left
    struct swwm_toplevel *new_focus_old_ws = NULL;
    if (!wl_list_empty(&old_ws->toplevels)) {
        new_focus_old_ws = wl_container_of(old_ws->toplevels.next, new_focus_old_ws, workspace_link);
    } else if (!wl_list_empty(&old_ws->floating_toplevels)) {
        new_focus_old_ws = wl_container_of(old_ws->floating_toplevels.next, new_focus_old_ws, workspace_link);
    }

    if (new_focus_old_ws && old_ws->id == server->current_ws_idx) {
        focus_toplevel(new_focus_old_ws, true);
    } else if (old_ws->id == server->current_ws_idx) { // No windows left on current old ws
        wlr_seat_keyboard_clear_focus(server->seat);
        server->focused_toplevel = NULL;
    }
    // If moved to current ws, the moved window itself should become focused.
    if (target_ws_idx == server->current_ws_idx) {
        focus_toplevel(toplevel, true);
    }
}


void toggle_floating_swwm(struct swwm_server *server, const void *arg) {
    struct swwm_toplevel *toplevel = get_focused_toplevel(server);
    if (!toplevel || toplevel->fullscreen) return;

    toplevel->floating = !toplevel->floating;

    wl_list_remove(&toplevel->workspace_link); // Remove from old list (tiled or floating)
    struct swwm_workspace *ws = &server->workspaces[toplevel->ws_idx];

    if (toplevel->floating) {
        wl_list_insert(&ws->floating_toplevels, &toplevel->workspace_link);
        // When becoming floating, try to restore its previous floating geometry or center it
        if (toplevel->saved_geom_float.width > 0 && toplevel->saved_geom_float.height > 0) {
            toplevel->geom = toplevel->saved_geom_float;
        } else { // Center on output (simplification)
            struct wlr_output *wlr_out = wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
            if (!wlr_out) wlr_out = wl_container_of(server->outputs.next, struct swwm_output, link)->wlr_output; // fallback
            if (wlr_out) {
                struct wlr_box output_box;
                wlr_output_layout_get_box(server->output_layout, wlr_out, &output_box);
                toplevel->geom.width = wlr_out->width / 2;
                toplevel->geom.height = wlr_out->height / 2;
                toplevel->geom.x = output_box.x + (output_box.width - toplevel->geom.width) / 2;
                toplevel->geom.y = output_box.y + (output_box.height - toplevel->geom.height) / 2;
            }
        }
        wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->geom.x, toplevel->geom.y);
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->geom.width, toplevel->geom.height);
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node); // Floating windows on top
    } else { // Becoming tiled
        wl_list_insert(ws->toplevels.prev, &toplevel->workspace_link); // Add to end of tiled list
        // Geometry will be set by arrange_workspace
        toplevel->saved_geom_float = toplevel->geom; // Save its current floating geometry
    }
    arrange_workspace(ws);
}

void toggle_fullscreen_swwm(struct swwm_server *server, const void *arg) {
    struct swwm_toplevel *toplevel = get_focused_toplevel(server);
    if (!toplevel) return;

    toplevel->fullscreen = !toplevel->fullscreen;
    struct swwm_workspace *ws = &server->workspaces[toplevel->ws_idx];

    if (toplevel->fullscreen) {
        if (toplevel->floating) {
            toplevel->saved_geom_float = toplevel->geom;
        } else {
            toplevel->saved_geom_tile = toplevel->geom;
        }

        struct wlr_output *wlr_out = wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
        if (!wlr_out && !wl_list_empty(&server->outputs)) {
             struct swwm_output *sout = wl_container_of(server->outputs.next, sout, link);
             wlr_out = sout->wlr_output;
        }
        if (!wlr_out) return; // No output to fullscreen on

        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
        // The actual geometry update for fullscreen might be driven by arrange_workspace
        // or directly here. sxwm moves/resizes it directly.
        struct wlr_box output_box;
        wlr_output_layout_get_box(server->output_layout, wlr_out, &output_box);
        toplevel->geom = output_box; // Fullscreen to the output box
        wlr_scene_node_set_position(&toplevel->scene_tree->node, output_box.x, output_box.y);
        // Size is handled by the fullscreen request, but we can configure it too
        // wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, output_box.width, output_box.height);
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    } else {
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
        // Restore pre-fullscreen geometry
        if (toplevel->floating) { // Was floating before fullscreen
             if(toplevel->saved_geom_float.width > 0) toplevel->geom = toplevel->saved_geom_float;
        } else { // Was tiled before fullscreen
             if(toplevel->saved_geom_tile.width > 0) toplevel->geom = toplevel->saved_geom_tile;
        }
        // If still tiled, arrange_workspace will handle geometry.
        // If floating, apply restored geometry.
        if (toplevel->floating) {
            wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->geom.x, toplevel->geom.y);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->geom.width, toplevel->geom.height);
        }
    }
    arrange_workspace(ws); // Rearrange to account for fullscreen/unfullscreen
}


// --- More sxwm function stubs/implementations ---
void move_master_next_swwm(struct swwm_server *server, const void *arg) { // Moves focused to next in stack list
    struct swwm_toplevel *focused = get_focused_toplevel(server);
    if (!focused || focused->floating || focused->fullscreen) return;

    struct swwm_workspace *ws = &server->workspaces[focused->ws_idx];
    if (wl_list_length(&ws->toplevels) < 2) return; // Need at least 2 tiled windows

    // If focused is master, move it to head of stack (second pos)
    // If focused is in stack, move it down in stack (circular)
    struct wl_list *current_link = &focused->workspace_link;
    struct wl_list *next_link = current_link->next;

    if (next_link == &ws->toplevels) { // Focused is last, move to become first (master)
        wl_list_remove(current_link);
        wl_list_insert(&ws->toplevels, current_link);
    } else { // Move after next
        wl_list_remove(current_link);
        wl_list_insert(next_link, current_link); // Insert after the 'next_link'
    }
    arrange_workspace(ws);
    // Focus remains on the same toplevel, but its position changes.
}


void move_master_prev_swwm(struct swwm_server *server, const void *arg) { // Makes focused the master
    struct swwm_toplevel *focused = get_focused_toplevel(server);
    if (!focused || focused->floating || focused->fullscreen) return;
    
    struct swwm_workspace *ws = &server->workspaces[focused->ws_idx];
    if (wl_list_empty(&ws->toplevels) || ws->toplevels.next == &focused->workspace_link) {
        return; // Already master or no tiled windows
    }
    wl_list_remove(&focused->workspace_link);
    wl_list_insert(&ws->toplevels, &focused->workspace_link); // Insert at head
    arrange_workspace(ws);
    // Focus remains
}

void resize_master_add_swwm(struct swwm_server *server, const void *arg) {
    struct swwm_output *output = get_focused_output(server);
    if (!output) return;
    float *mw = &server->config.master_width[output->idx];
    *mw += (float)server->config.resize_master_amt / 100.0f;
    if (*mw > MF_MAX) *mw = MF_MAX;
    arrange_all(server); // Or just current workspace if master_width is per-workspace/output
}

void resize_master_sub_swwm(struct swwm_server *server, const void *arg) {
    struct swwm_output *output = get_focused_output(server);
    if (!output) return;
    float *mw = &server->config.master_width[output->idx];
    *mw -= (float)server->config.resize_master_amt / 100.0f;
    if (*mw < MF_MIN) *mw = MF_MIN;
    arrange_all(server);
}

void inc_gaps_swwm(struct swwm_server *server, const void *arg) {
    server->config.gaps++;
    arrange_all(server);
}
void dec_gaps_swwm(struct swwm_server *server, const void *arg) {
    if (server->config.gaps > 0) server->config.gaps--;
    arrange_all(server);
}

void toggle_floating_global_swwm(struct swwm_server *server, const void *arg) {
    server->global_floating = !server->global_floating;
    struct swwm_workspace *ws = &server->workspaces[server->current_ws_idx];
    struct swwm_toplevel *toplevel_iter, *tmp;

    // Determine if we are making all tiled, or all floating
    bool make_all_floating = false;
    wl_list_for_each(toplevel_iter, &ws->toplevels, workspace_link) { // Check tiled list
        if (!toplevel_iter->floating) {
            make_all_floating = true;
            break;
        }
    }
    // If no non-floating found in tiled list, check floating list (less likely scenario here)
    if (!make_all_floating) {
         wl_list_for_each(toplevel_iter, &ws->floating_toplevels, workspace_link) {
            if (toplevel_iter->floating) { // if any is floating, and we didn't find non-floating, then make all tiled
                break; // This logic is a bit like sxwm's: if any tiled, make all floating. Else make all tiled.
            }
         }
    }


    // Apply to tiled windows
    wl_list_for_each_safe(toplevel_iter, tmp, &ws->toplevels, workspace_link) {
        if (toplevel_iter->fullscreen) continue;
        if (make_all_floating) {
            toplevel_iter->floating = true;
            wl_list_remove(&toplevel_iter->workspace_link);
            wl_list_insert(&ws->floating_toplevels, &toplevel_iter->workspace_link);
            // Restore/set floating geometry (simplified)
            toplevel_iter->geom = toplevel_iter->saved_geom_float.width > 0 ? toplevel_iter->saved_geom_float : toplevel_iter->geom;
             wlr_scene_node_set_position(&toplevel_iter->scene_tree->node, toplevel_iter->geom.x, toplevel_iter->geom.y);
             wlr_xdg_toplevel_set_size(toplevel_iter->xdg_toplevel, toplevel_iter->geom.width, toplevel_iter->geom.height);

        } // else: if it was in tiled list, it's already !floating, do nothing to it
    }
    // Apply to floating windows
    wl_list_for_each_safe(toplevel_iter, tmp, &ws->floating_toplevels, workspace_link) {
        if (toplevel_iter->fullscreen) continue;
        if (!make_all_floating) { // Make all tiled
            toplevel_iter->floating = false;
            toplevel_iter->saved_geom_float = toplevel_iter->geom; // Save current float geom
            wl_list_remove(&toplevel_iter->workspace_link);
            wl_list_insert(ws->toplevels.prev, &toplevel_iter->workspace_link);
        } // else: if it was in floating list, it's already floating, do nothing
    }
    arrange_workspace(ws);
}
// --- End sxwm function ports ---


static uint32_t wlr_mods_to_swm_mods(uint32_t wlr_mods) {
    uint32_t swm_mods = 0;
    if (wlr_mods & WLR_MODIFIER_SHIFT) swm_mods |= SWM_MOD_SHIFT;
    if (wlr_mods & WLR_MODIFIER_CAPS) swm_mods |= SWM_MOD_CAPS;
    if (wlr_mods & WLR_MODIFIER_CTRL) swm_mods |= SWM_MOD_CTRL;
    if (wlr_mods & WLR_MODIFIER_ALT) swm_mods |= SWM_MOD_ALT;
    if (wlr_mods & WLR_MODIFIER_MOD2) swm_mods |= SWM_MOD_MOD2;
    if (wlr_mods & WLR_MODIFIER_MOD3) swm_mods |= SWM_MOD_MOD3;
    if (wlr_mods & WLR_MODIFIER_LOGO) swm_mods |= SWM_MOD_LOGO;
    if (wlr_mods & WLR_MODIFIER_MOD5) swm_mods |= SWM_MOD_MOD5;
    return swm_mods;
}


static void focus_toplevel(struct swwm_toplevel *toplevel, bool raise) {
	if (toplevel == NULL) {
        // If no toplevel to focus, clear seat focus
        if (get_focused_toplevel(toplevel->server)) { // Check if server available
             wlr_seat_keyboard_clear_focus(toplevel->server->seat);
             toplevel->server->focused_toplevel = NULL;
        }
		return;
	}
	struct swwm_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
    struct swwm_toplevel *prev_focused_toplevel = server->focused_toplevel;
    
	if (prev_focused_toplevel == toplevel) {
        if (raise) wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		return; // Already focused
	}

	if (prev_focused_toplevel) {
        if (prev_focused_toplevel->xdg_toplevel && prev_focused_toplevel->xdg_toplevel->base->surface) {
		    wlr_xdg_toplevel_set_activated(prev_focused_toplevel->xdg_toplevel, false);
        }
	}

    server->focused_toplevel = toplevel;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	if (raise) {
        // Move the toplevel to the front of its workspace list (visual stacking order for tiled)
        // For floating, they are typically above tiled ones anyway.
        // sxwm raises the window; wlr_scene_node_raise_to_top does this within its parent.
        // If layers are used, ensure it's raised within the correct layer.
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    }
	
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
        // If no keyboard, still mark as focused for internal logic
        wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
    }
    // Update borders or other visual cues if implemented
}

static void cycle_focus(struct swwm_server *server, bool forward) {
    struct swwm_workspace *ws = &server->workspaces[server->current_ws_idx];
    if (wl_list_empty(&ws->toplevels) && wl_list_empty(&ws->floating_toplevels)) {
        return; // No windows to focus
    }

    struct swwm_toplevel *current_focus = server->focused_toplevel;
    struct swwm_toplevel *next_focus = NULL;

    // Try to cycle within the current type (tiled or floating) first
    struct wl_list *target_list = NULL;
    if (current_focus && current_focus->floating) {
        target_list = &ws->floating_toplevels;
    } else { // current_focus is tiled or NULL
        target_list = &ws->toplevels;
    }

    if (current_focus && current_focus->ws_idx == server->current_ws_idx) { // If current focus is on this workspace
        struct wl_list *link = &current_focus->workspace_link;
        if (forward) {
            link = link->next;
            if (link == target_list) link = target_list->next; // Wrap around
        } else {
            link = link->prev;
            if (link == target_list) link = target_list->prev; // Wrap around
        }
        if (link != target_list) { // Check if list is not empty after potential removal
             next_focus = wl_container_of(link, next_focus, workspace_link);
        }
    }
    
    // If no next_focus found in the primary list, or no current_focus, try the primary list from start/end
    if (!next_focus) {
        if (forward) {
            if (!wl_list_empty(target_list)) {
                 next_focus = wl_container_of(target_list->next, next_focus, workspace_link);
            }
        } else {
             if (!wl_list_empty(target_list)) {
                 next_focus = wl_container_of(target_list->prev, next_focus, workspace_link);
            }
        }
    }

    // If still no focus and we tried one list, try the other list
    if (!next_focus) {
        if (target_list == &ws->toplevels && !wl_list_empty(&ws->floating_toplevels)) {
            target_list = &ws->floating_toplevels;
        } else if (target_list == &ws->floating_toplevels && !wl_list_empty(&ws->toplevels)) {
            target_list = &ws->toplevels;
        } else { // Only one list had items, or both empty
            goto end_cycle;
        }
        // Get first/last from the other list
        if (forward && !wl_list_empty(target_list)) {
            next_focus = wl_container_of(target_list->next, next_focus, workspace_link);
        } else if (!wl_list_empty(target_list)) { // backward
            next_focus = wl_container_of(target_list->prev, next_focus, workspace_link);
        }
    }

end_cycle:
    if (next_focus) {
        focus_toplevel(next_focus, true);
    }
}


static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct swwm_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static bool handle_compositor_keybinding(struct swwm_server *server, xkb_keysym_t sym, uint32_t wlr_mods) {
    uint32_t swm_mods = wlr_mods_to_swm_mods(wlr_mods);

    for (int i = 0; i < server->config.bindsn; ++i) {
        Binding *b = &server->config.binds[i];
        if (b->keysym == sym && b->mods == swm_mods) {
            switch (b->type) {
                case TYPE_CMD:
                    spawn_swwm(server, b->arg);
                    break;
                case TYPE_FUNC:
                    if (b->action.fn) {
                        b->action.fn(server, b->arg);
                    }
                    break;
                case TYPE_CWKSP:
                    change_workspace_action(server, b->arg);
                    break;
                case TYPE_MWKSP:
                    move_to_workspace_action(server, b->arg);
                    break;
            }
            return true; // Binding handled
        }
    }
	return false; // No binding handled
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	struct swwm_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct swwm_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8; // libinput to xkbcommon offset
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		for (int i = 0; i < nsyms; i++) {
			if (handle_compositor_keybinding(server, syms[i], modifiers)) {
                handled = true;
                break;
            }
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct swwm_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct swwm_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct swwm_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names rules = { 0 }; // Use system defaults
    // You can set rules.layout, rules.model, rules.variant, rules.options here
    // e.g. rules.layout = "us"; rules.options = "ctrl:nocaps";
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600); // Default repeat info

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct swwm_server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct swwm_server *server = wl_container_of(
			listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct swwm_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}


static struct swwm_toplevel *get_toplevel_from_scene_node(struct wlr_scene_node *node) {
    if (!node || node->type != WLR_SCENE_NODE_TREE) { // swwm_toplevel's root is a scene_tree
        return NULL;
    }
    // In server_new_xdg_toplevel, we set scene_tree->node.data = toplevel
    return node->data;
}


static struct swwm_toplevel *get_toplevel_at(
		struct swwm_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
    // Search within the toplevel_layer
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->toplevel_layer->node, lx, ly, sx, sy);
	
    if (node == NULL || node->type == WLR_SCENE_NODE_BUFFER) { // Hit a surface directly
        if (node && node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
            struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
            if (scene_surface) {
                *surface = scene_surface->surface;
            }
        }
        // Find the swwm_toplevel this surface belongs to by traversing up the scene graph
        struct wlr_scene_tree *tree_node = node ? node->parent : NULL;
        while(tree_node) {
            if (tree_node->node.data && get_toplevel_from_scene_node(&tree_node->node)) { // Check if data is a swwm_toplevel
                return tree_node->node.data;
            }
            tree_node = tree_node->node.parent;
        }
        return NULL; // Could not find parent swwm_toplevel
    }
    // If node itself is a scene_tree that is a toplevel
    if (node && node->data && get_toplevel_from_scene_node(node)) {
        // Need to find the actual surface under the pointer within this toplevel's tree
        struct wlr_scene_node *surface_node = wlr_scene_node_at(node, lx, ly, sx, sy);
         if (surface_node && surface_node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(surface_node);
            struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
            if (scene_surface) {
                *surface = scene_surface->surface;
            }
        }
        return node->data;
    }
	return NULL;
}

static void reset_cursor_mode(struct swwm_server *server) {
	server->cursor_mode = SWM_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
    server->swap_target_toplevel = NULL;
    // Reset cursor image
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}

static void process_cursor_move_interactive(struct swwm_server *server) {
	struct swwm_toplevel *toplevel = server->grabbed_toplevel;
    if (!toplevel) return;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
    toplevel->geom.x = server->cursor->x - server->grab_x;
    toplevel->geom.y = server->cursor->y - server->grab_y;
}

static void process_cursor_resize_interactive(struct swwm_server *server) {
	struct swwm_toplevel *toplevel = server->grabbed_toplevel;
    if (!toplevel) return;

	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    // Minimum size
    const int min_width = 50;
    const int min_height = 30;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom - min_height) {
			new_top = new_bottom - min_height;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top + min_height) {
			new_bottom = new_top + min_height;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right - min_width) {
			new_left = new_right - min_width;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left + min_width) {
			new_right = new_left + min_width;
		}
	}

	// Current xdg_surface geometry (relative to its own tree)
	// struct wlr_box *current_geo_box = &toplevel->xdg_toplevel->base->geometry; 
    // We operate in layout coordinates for scene_node_set_position

    // New position for the scene node
	wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left, new_top);
    toplevel->geom.x = new_left;
    toplevel->geom.y = new_top;

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
    toplevel->geom.width = new_width;
    toplevel->geom.height = new_height;

	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_swap_interactive(struct swwm_server *server) {
    // Find toplevel under cursor (excluding the one being dragged)
    // If it's a tiled window, highlight it as a swap target.
    // Actual swap happens on button release.
    double sx, sy;
	struct wlr_surface *surface = NULL;
	struct swwm_toplevel *target = get_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (target == server->grabbed_toplevel) target = NULL; // Can't swap with itself

    if (server->swap_target_toplevel && server->swap_target_toplevel != target) {
        // TODO: Reset visual cue for old swap_target_toplevel
    }
    server->swap_target_toplevel = NULL;
    if (target && !target->floating && !target->fullscreen) {
        server->swap_target_toplevel = target;
        // TODO: Set visual cue for new swap_target_toplevel (e.g., border color)
    }
}


static void process_cursor_motion(struct swwm_server *server, uint32_t time_msec) {
    // Throttle motion events if configured
    if (server->config.motion_throttle_hz > 0) {
        long throttle_ms = 1000 / server->config.motion_throttle_hz;
        if (time_msec - server->last_motion_time_msec < throttle_ms) {
            return;
        }
        server->last_motion_time_msec = time_msec;
    }


	if (server->cursor_mode == SWM_CURSOR_MOVE) {
		process_cursor_move_interactive(server);
		return;
	} else if (server->cursor_mode == SWM_CURSOR_RESIZE) {
		process_cursor_resize_interactive(server);
		return;
	} else if (server->cursor_mode == SWM_CURSOR_SWAP) {
        process_cursor_swap_interactive(server);
        return;
    }

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct swwm_toplevel *toplevel = get_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr"); // Default cursor
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	
    wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);

    // sxwm style interactions
    uint32_t current_wlr_mods = wlr_keyboard_get_modifiers(wlr_seat_get_keyboard(server->seat));
    uint32_t current_swm_mods = wlr_mods_to_swm_mods(current_wlr_mods);


	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode == SWM_CURSOR_SWAP && server->grabbed_toplevel && server->swap_target_toplevel) {
            // Perform the swap
            struct swwm_toplevel *dragged = server->grabbed_toplevel;
            struct swwm_toplevel *target = server->swap_target_toplevel;
            struct swwm_workspace *ws = &server->workspaces[dragged->ws_idx];

            // Simple swap: exchange positions in the ws->toplevels list
            struct wl_list *link_dragged = &dragged->workspace_link;
            struct wl_list *link_target = &target->workspace_link;

            struct wl_list temp_link;
            wl_list_remove(link_dragged);
            wl_list_insert(link_target, &temp_link); // placeholder before target
            wl_list_remove(link_target);
            wl_list_insert(&temp_link, link_target); // target where dragged was
            wl_list_remove(&temp_link);
            wl_list_insert(link_target->prev, link_dragged); // dragged where target was

            arrange_workspace(ws);
        }
		reset_cursor_mode(server);
	} else if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct swwm_toplevel *toplevel = get_toplevel_at(server,
				server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        
        if (toplevel) {
            focus_toplevel(toplevel, true); // Focus and raise on click

            // Mod + Button1 for move, Mod + Button3 for resize (if floating)
            // Mod + Shift + Button1 for swap (if tiled)
            if ((current_swm_mods & server->config.modkey) && toplevel->floating) {
                if (event->button == BTN_LEFT) { // Assuming BTN_LEFT from linux/input-event-codes.h
                    begin_interactive(toplevel, SWM_CURSOR_MOVE, 0);
                    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing"); // or "move"
                } else if (event->button == BTN_RIGHT) {
                    // For resize, we need edge detection or default to bottom-right
                    begin_interactive(toplevel, SWM_CURSOR_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
                     wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "bottom_right_corner");
                }
            } else if ((current_swm_mods & server->config.modkey) && (current_swm_mods & SWM_MOD_SHIFT) &&
                       !toplevel->floating && event->button == BTN_LEFT) {
                begin_interactive(toplevel, SWM_CURSOR_SWAP, 0);
                 wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "exchange");
            } else if ((current_swm_mods & server->config.modkey) && !toplevel->floating &&
                       (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
                // Mod + Click on tiled window -> toggle floating
                toggle_floating_swwm(server, NULL);
            }
        }
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}


static void output_frame(struct wl_listener *listener, void *data) {
	struct swwm_output *output = wl_container_of(listener, output, frame);
    if (!output->scene_output) return;

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct swwm_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct swwm_output *output = wl_container_of(listener, output, destroy);
    // TODO: When an output is destroyed, need to move its workspaces/toplevels
    // to another output. For now, this is simplified.
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
    if(output->scene_output) {
        // wlr_scene_output_destroy(output->scene_output); // This might be handled by scene_layout destroy
    }
	free(output);
    // After removing an output, re-evaluate workspace assignments and re-arrange.
    // struct swwm_server* server = output->server; (need server pointer if output is freestanding)
    // arrange_all(server);
}

static int output_idx_counter = 0; // Simple way to assign unique idx to outputs

static void server_new_output(struct wl_listener *listener, void *data) {
	struct swwm_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct swwm_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
    output->idx = output_idx_counter++;
    if (output->idx >= MAX_MONITORS) {
        wlr_log(WLR_ERROR, "Exceeded MAX_MONITORS, output %d may not have specific config.", output->idx);
        output->idx = MAX_MONITORS -1; // Clamp to avoid crash, share last config
    }
    wlr_output_layout_get_box(server->output_layout, wlr_output, &output->usable_area);


	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    // Associate the scene_output with the layout_output for the scene_layout
    if (l_output && output->scene_output) {
        // This part is tricky, wlr_scene_attach_output_layout manages this link.
        // wlr_scene_output_layout_add_output is for custom layouts.
        // Since we use wlr_scene_attach_output_layout, it should handle new outputs.
    } else {
        wlr_log(WLR_ERROR, "Failed to create or link scene_output for new wlr_output");
    }


    // Assign workspaces to this new output if they don't have one yet
    // This is a simple distribution, more complex logic might be needed for sxwm feature parity
    for (int i = 0; i < NUM_WORKSPACES; ++i) {
        if (server->workspaces[i].output == NULL) {
             // A simple heuristic: assign first N/M workspaces to first output, etc.
             // Or assign based on output index.
            if (i % wl_list_length(&server->outputs) == (wl_list_length(&server->outputs) -1) ) { // Last added output gets some
                 server->workspaces[i].output = output;
            }
        }
    }
    // If it's the first output, make workspace 0 active on it.
    if (wl_list_length(&server->outputs) == 1) {
        server->workspaces[0].output = output;
        // server->current_ws_idx is already 0 by default.
        // change_workspace_action(server, (void*) (intptr_t) 0); // to show it
    }

    arrange_all(server);
}

static void xdg_toplevel_set_app_id_notify(struct wl_listener *listener, void *data) {
    struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, set_app_id);
    // App ID is now set (or updated). Re-check `should_float`.
    // This is less critical if checked at map time, but good for completeness.
    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (!app_id) return;

    bool should_be_floating = false;
    if (toplevel->server->global_floating || toplevel->server->next_toplevel_should_float) {
        should_be_floating = true;
    } else {
        for (int i = 0; i < toplevel->server->config.should_floatn; ++i) {
            if (toplevel->server->config.should_float[i] &&
                strcmp(app_id, toplevel->server->config.should_float[i]) == 0) {
                should_be_floating = true;
                break;
            }
        }
    }
    
    if (should_be_floating && !toplevel->floating && !toplevel->fullscreen) {
        toggle_floating_swwm(toplevel->server, NULL); // Pass server, toggle operates on focused
                                                    // This needs to operate on *this* toplevel.
                                                    // Simplification: assume it will be focused soon.
                                                    // Proper way: a toggle_floating_specific(toplevel)
    }
    // Reset for next window
    if (toplevel->server->next_toplevel_should_float) {
        toplevel->server->next_toplevel_should_float = false;
    }
}


static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    struct swwm_server *server = toplevel->server;

    toplevel->ws_idx = server->current_ws_idx; // Assign to current workspace
    struct swwm_workspace *ws = &server->workspaces[toplevel->ws_idx];

    // Determine if it should float
    bool should_be_floating_initial = false;
    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (server->global_floating || server->next_toplevel_should_float) {
        should_be_floating_initial = true;
    } else if (app_id) {
         for (int i = 0; i < server->config.should_floatn; ++i) {
            if (server->config.should_float[i] &&
                strcmp(app_id, server->config.should_float[i]) == 0) {
                should_be_floating_initial = true;
                break;
            }
        }
    }
    // Also check for XDG_TOPLEVEL_STATE_TILED / MAXIMIZED if client requests it
    // and if PMinSize == PMaxSize (fixed size hint from sxwm) -> XDG equivalent?
    // wlr_xdg_toplevel_requested.maximized or client setting fixed size (less common in Wayland)
    // For dialogs/transients: toplevel->xdg_toplevel->parent != NULL
    if (toplevel->xdg_toplevel->parent != NULL) { // Transient window (dialog, etc)
        should_be_floating_initial = true;
    }


    if (should_be_floating_initial) {
        toplevel->floating = true;
        wl_list_insert(ws->floating_toplevels.prev, &toplevel->workspace_link);
        // Set initial floating geometry (e.g., centered)
        struct wlr_output *wlr_out = wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
        if (!wlr_out && !wl_list_empty(&server->outputs)) {
            struct swwm_output *sout = wl_container_of(server->outputs.next, sout, link);
            wlr_out = sout->wlr_output;
        }
        if (wlr_out) {
            struct wlr_box output_box;
            wlr_output_layout_get_box(server->output_layout, wlr_out, &output_box);
            // Get initial size from client if available, else default
            struct wlr_box req_geom;
            wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &req_geom);

            toplevel->geom.width = req_geom.width > 0 ? req_geom.width : wlr_out->width / 2;
            toplevel->geom.height = req_geom.height > 0 ? req_geom.height : wlr_out->height / 2;
            toplevel->geom.x = output_box.x + (output_box.width - toplevel->geom.width) / 2;
            toplevel->geom.y = output_box.y + (output_box.height - toplevel->geom.height) / 2;
            
            wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->geom.x, toplevel->geom.y);
            // Client might resize itself after this configure.
            // wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->geom.width, toplevel->geom.height);
        }
    } else {
        toplevel->floating = false;
        wl_list_insert(ws->toplevels.prev, &toplevel->workspace_link); // Add to end of tiled list
    }
    
    if (server->next_toplevel_should_float) server->next_toplevel_should_float = false;

    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true); // Ensure visible
	focus_toplevel(toplevel, true);
    arrange_workspace(ws);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    struct swwm_server *server = toplevel->server;
    
	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}
    if (toplevel == server->focused_toplevel) {
        server->focused_toplevel = NULL; // Clear focused if it's unmapping
    }

	wl_list_remove(&toplevel->workspace_link); // Remove from its workspace list
    
    struct swwm_workspace *ws = &server->workspaces[toplevel->ws_idx];
    arrange_workspace(ws); // Re-tile the workspace

    // Focus next available window on unmap
    if (server->focused_toplevel == NULL && server->current_ws_idx == toplevel->ws_idx) {
        cycle_focus(server, true); // Try to focus something else
    }
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    // struct wlr_surface *surface = data; // This is incorrect, data is NULL for surface commit.
                                        // data is specific to the event. For surface.commit, it's NULL.
                                        // The surface is toplevel->xdg_toplevel->base->surface.

	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0); // Let client pick initial size
        // App ID might be available now or after a few commits.
        // Listener for set_app_id is better.
	}
    // If geometry changed by client, and it's tiled, we might need to re-evaluate or force our size.
    // For now, assume compositor dictates size for tiled windows primarily via arrange_workspace.
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    struct swwm_server *server = toplevel->server;

    if (toplevel == server->grabbed_toplevel) reset_cursor_mode(server);
    if (toplevel == server->focused_toplevel) server->focused_toplevel = NULL;

    // Remove from global list if any (not currently used this way)
	// wl_list_remove(&toplevel->link); 
    if (toplevel->workspace_link.next) { // Check if it's actually in a list
        wl_list_remove(&toplevel->workspace_link);
    }
    
    struct swwm_workspace *ws = &server->workspaces[toplevel->ws_idx];

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->set_app_id.link);

    wlr_scene_node_destroy(&toplevel->scene_tree->node); // Destroy scene representation

	free(toplevel);

    arrange_workspace(ws); // Re-tile the workspace
    // Focus next if the destroyed window was focused.
    if (server->focused_toplevel == NULL && server->current_ws_idx == ws->id) {
         cycle_focus(server, true);
    }
}


static void begin_interactive(struct swwm_toplevel *toplevel,
		enum swwm_cursor_mode mode, uint32_t edges) {
	struct swwm_server *server = toplevel->server;
    struct wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;

    // Ensure the toplevel is focused for keyboard events during interaction
    focus_toplevel(toplevel, true);

    // Check if pointer is actually on this toplevel before starting drag
    // This is more relevant for CSD initiated drags. For Mod+Click, it's fine.
	// if (focused_surface != toplevel->xdg_toplevel->base->surface) {
	// 	return; // Interaction requested for a surface that doesn't have pointer focus
	// }

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == SWM_CURSOR_MOVE || mode == SWM_CURSOR_SWAP) {
        // grab_x, grab_y is offset from toplevel's top-left corner to cursor position
		server->grab_x = server->cursor->x - toplevel->geom.x;
		server->grab_y = server->cursor->y - toplevel->geom.y;
        server->grab_geobox = toplevel->geom; // Store current geometry
	} else if (mode == SWM_CURSOR_RESIZE) {
		// For resize, grab_x, grab_y is offset from cursor to the active resize edge/corner
        // server->grab_geobox stores the toplevel's geometry in layout coordinates
        server->grab_geobox = toplevel->geom;

		double border_x = toplevel->geom.x + ((edges & WLR_EDGE_RIGHT) ? toplevel->geom.width : 0);
		double border_y = toplevel->geom.y + ((edges & WLR_EDGE_BOTTOM) ? toplevel->geom.height : 0);
        if (edges & WLR_EDGE_LEFT) border_x = toplevel->geom.x;
        if (edges & WLR_EDGE_TOP) border_y = toplevel->geom.y;
        
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    // TODO: Check serial from event data if available/needed for security.
    // struct wlr_xdg_toplevel_move_event *event = data;
    if (toplevel->floating) { // Only allow CSD move if floating
	    begin_interactive(toplevel, SWM_CURSOR_MOVE, 0);
    }
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct swwm_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    if (toplevel->floating) { // Only allow CSD resize if floating
	    begin_interactive(toplevel, SWM_CURSOR_RESIZE, event->edges);
    }
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
    // sxwm doesn't support maximization explicitly, maps to fullscreen or nothing.
    // We can choose to implement it as toggle fullscreen or just send configure.
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct swwm_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
    // struct wlr_xdg_toplevel_set_fullscreen_event *event = data;
    // if (event->fullscreen && event->output) { ... }
    toggle_fullscreen_swwm(toplevel->server, NULL); // Toggle fullscreen for focused
    // This should ideally toggle for *this* toplevel, not necessarily the focused one.
    // For now, assume this toplevel will be focused.
	if (toplevel->xdg_toplevel->base->initialized) {
		// wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base); // toggle_fullscreen sends configure
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct swwm_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct swwm_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
    // Create scene node in the toplevel_layer
	toplevel->scene_tree = wlr_scene_xdg_surface_create(server->toplevel_layer, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel; // Link back from scene node to swwm_toplevel
	xdg_toplevel->base->data = toplevel; // User data for the xdg_surface can be swwm_toplevel

    // Initialize sxwm properties
    toplevel->floating = false; // Will be determined on map
    toplevel->fullscreen = false;
    toplevel->ws_idx = server->current_ws_idx; // Default to current, refined on map
    // geom and saved_geoms are zeroed by calloc

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

    toplevel->set_app_id.notify = xdg_toplevel_set_app_id_notify;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);
    
    // Initially disable scene node, will be enabled on map and workspace change
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct swwm_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct swwm_popup *popup = wl_container_of(listener, popup, destroy);
    if (popup->scene_tree) wlr_scene_node_destroy(&popup->scene_tree->node);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct swwm_server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct swwm_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent_xdg_surface = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent_xdg_surface != NULL);
    // struct swwm_toplevel *parent_toplevel = parent_xdg_surface->data; // if xdg_surface->data is swwm_toplevel
    // If xdg_surface->data points to scene_tree as in original swwm:
    struct wlr_scene_tree *parent_scene_tree = parent_xdg_surface->data;
    assert(parent_scene_tree != NULL);

	popup->scene_tree = wlr_scene_xdg_surface_create(parent_scene_tree, xdg_popup->base);
    popup->scene_tree->node.data = popup; // Link back if needed
    xdg_popup->base->data = popup->scene_tree; // Original swwm way

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}


// --- Tiling logic (Master-Stack) ---
static void arrange_workspace(struct swwm_workspace *ws) {
    if (!ws) return;
    struct swwm_server *server = wl_container_of(ws, struct swwm_server, workspaces[ws->id]); // Get server ptr
    
    // Determine output for this workspace. sxwm tiles per monitor.
    // In Wayland, a workspace isn't strictly tied to one output, but for tiling it's simpler.
    struct swwm_output *output = ws->output; // Use the assigned output
    if (!output) { // If no specific output, try to find one (e.g., the first one)
        if (!wl_list_empty(&server->outputs)) {
            output = wl_container_of(server->outputs.next, output, link);
        } else {
            return; // No outputs to arrange on
        }
    }
    
    struct wlr_box output_geom;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &output_geom);

    // Apply usable_area adjustments (e.g., for panels, from config or layer-shell)
    // For now, just use output_geom minus gaps.
    int gaps = server->config.gaps;
    struct wlr_box tile_area = {
        .x = output_geom.x + gaps,
        .y = output_geom.y + gaps,
        .width = output_geom.width - 2 * gaps,
        .height = output_geom.height - 2 * gaps,
    };
    if (tile_area.width < 1) tile_area.width = 1;
    if (tile_area.height < 1) tile_area.height = 1;

    int tiled_count = 0;
    struct swwm_toplevel *iter;
    wl_list_for_each(iter, &ws->toplevels, workspace_link) {
        if (!iter->floating && !iter->fullscreen && iter->xdg_toplevel->base->surface->mapped) {
            tiled_count++;
        }
    }

    if (tiled_count == 0) return;

    // Master-stack layout
    struct swwm_toplevel *master = NULL;
    if (!wl_list_empty(&ws->toplevels)) {
        master = wl_container_of(ws->toplevels.next, master, workspace_link);
        while (master && (master->floating || master->fullscreen)) { // Skip non-tiled as master
            if (master->workspace_link.next == &ws->toplevels) { master = NULL; break; } // End of list
            master = wl_container_of(master->workspace_link.next, master, workspace_link);
        }
    }


    if (!master && tiled_count > 0) { /* Should not happen if tiled_count > 0 and list is consistent */ }
    if (!master) return; // No suitable master found

    float master_factor = server->config.master_width[output->idx];
    int master_width = tile_area.width;
    if (tiled_count > 1) { // Only split if more than one tiled window
        master_width = tile_area.width * master_factor;
    }
    
    // Configure master
    master->geom.x = tile_area.x;
    master->geom.y = tile_area.y;
    master->geom.width = master_width - (tiled_count > 1 ? gaps / 2 : 0);
    master->geom.height = tile_area.height;
    wlr_scene_node_set_position(&master->scene_tree->node, master->geom.x, master->geom.y);
    wlr_xdg_toplevel_set_size(master->xdg_toplevel, master->geom.width, master->geom.height);
    wlr_xdg_toplevel_set_tiled(master->xdg_toplevel, WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM);


    // Configure stack windows
    int stack_count = tiled_count - 1;
    if (stack_count > 0) {
        int stack_x = tile_area.x + master_width + (gaps / 2);
        int stack_width = tile_area.width - master_width - (gaps / 2);
        int stack_y = tile_area.y;
        int stack_win_height = (tile_area.height - (stack_count - 1) * gaps) / stack_count;
        if (stack_win_height < 1) stack_win_height = 1;

        int current_stack_idx = 0;
        struct swwm_toplevel *stack_iter = master; // Start from master
        for (int i=0; i < tiled_count; ++i) { // Iterate up to tiled_count times over the list
            if (stack_iter->workspace_link.next == &ws->toplevels) stack_iter = wl_container_of(ws->toplevels.next, stack_iter, workspace_link);
            else stack_iter = wl_container_of(stack_iter->workspace_link.next, stack_iter, workspace_link);

            if(stack_iter == master) break; // Cycled back to master
            if (stack_iter->floating || stack_iter->fullscreen || !stack_iter->xdg_toplevel->base->surface->mapped) continue;


            stack_iter->geom.x = stack_x;
            stack_iter->geom.y = stack_y + current_stack_idx * (stack_win_height + gaps);
            stack_iter->geom.width = stack_width;
            stack_iter->geom.height = stack_win_height;
            if (current_stack_idx == stack_count - 1) { // Last stack window takes remaining height
                stack_iter->geom.height = tile_area.y + tile_area.height - stack_iter->geom.y;
            }
             if (stack_iter->geom.height < 1) stack_iter->geom.height = 1;


            wlr_scene_node_set_position(&stack_iter->scene_tree->node, stack_iter->geom.x, stack_iter->geom.y);
            wlr_xdg_toplevel_set_size(stack_iter->xdg_toplevel, stack_iter->geom.width, stack_iter->geom.height);
            wlr_xdg_toplevel_set_tiled(stack_iter->xdg_toplevel, WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
            current_stack_idx++;
            if (current_stack_idx >= stack_count) break;
        }
    }

    // Floating windows are not arranged by this function, but ensure they are on top
    wl_list_for_each(iter, &ws->floating_toplevels, workspace_link) {
        if (iter->xdg_toplevel->base->surface->mapped) {
             wlr_scene_node_raise_to_top(&iter->scene_tree->node);
        }
    }
    // Fullscreen windows are also on top
    wl_list_for_each(iter, &ws->toplevels, workspace_link) { // Fullscreen could be in tiled list internally
        if (iter->fullscreen && iter->xdg_toplevel->base->surface->mapped) {
            wlr_scene_node_raise_to_top(&iter->scene_tree->node);
        }
    }

}

static void arrange_all(struct swwm_server *server) {
    // Arrange toplevels on the current workspace
    arrange_workspace(&server->workspaces[server->current_ws_idx]);
    // In a multi-output setup where workspaces can span outputs or are per-output,
    // this would iterate all relevant workspaces/outputs.
    // For now, current workspace implies current output context.
}

// --- Helper function implementations ---
struct swwm_toplevel *get_focused_toplevel(struct swwm_server *server) {
    return server->focused_toplevel;
}

struct swwm_output *get_focused_output(struct swwm_server *server) {
    struct wlr_output *wlr_out = wlr_output_layout_output_at(
        server->output_layout, server->cursor->x, server->cursor->y);
    if (!wlr_out) {
        if (wl_list_empty(&server->outputs)) return NULL;
        // Fallback to first output
        return wl_container_of(server->outputs.next, struct swwm_output, link);
    }
    struct swwm_output *out_iter;
    wl_list_for_each(out_iter, &server->outputs, link) {
        if (out_iter->wlr_output == wlr_out) {
            return out_iter;
        }
    }
    return NULL; // Should not happen if wlr_out was found
}

static void init_default_config(Config *config) {
    memset(config, 0, sizeof(Config)); // Zero out the config struct first

    config->modkey = SWM_MOD_LOGO; // Super/Win key
    config->gaps = 10;
    config->border_width = 1; // Visuals not implemented
    config->border_foc_col_val = 0xFFFF0000; // Red (example, not used)
    config->border_ufoc_col_val = 0xFF888888; // Gray (example, not used)
    config->border_swap_col_val = 0xFFFFFF00; // Yellow (example, not used)

    for (int i = 0; i < MAX_MONITORS; i++) {
        config->master_width[i] = 0.5f; // 50%
    }
    config->motion_throttle_hz = 60;
    config->resize_master_amt = 5; // 5%
    config->snap_distance = 10;    // Pixels (visuals not implemented)

    config->bindsn = 0; // Will be populated by parser or default binds array
    // Copy default binds from config.txt's `binds` array
    // This is a bit meta; the parser usually does this.
    // If parser fails, these defaults can be used.
    // For now, parser is expected to load them.
    //memcpy(config->binds, binds_default_array, sizeof(binds_default_array));
    //config->bindsn = LENGTH(binds_default_array);

    config->should_floatn = 0;
    for (int j = 0; j < 256; j++) {
		config->should_float[j] = NULL;
	}
}

static void apply_config(struct swwm_server *server) {
    // Apply settings that affect global server state or visuals
    // e.g., cursor theme, if configurable, would be set here.
    // Gaps, master_width are used by arrange_workspace.
    // Keybindings are already loaded.
    // For now, most config is used on-demand.
}


int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL); // Changed to INFO for less verbosity
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct swwm_server server = {0};
	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
    server.toplevel_layer = wlr_scene_tree_create(server.scene); // Layer for app windows
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // --- sxwm feature initialization ---
    init_default_config(&server.config);
    if (parser(&server, &server.config) != 0) { // Pass server for context if parser needs it
        wlr_log(WLR_ERROR, "Failed to parse config file, using defaults.");
        // Load built-in defaults from config.txt if parser fails badly
        // This part is tricky; parser.c doesn't have access to the `binds` array from config.txt directly
        // For now, assume parser errors mean sticking to `init_default_config` values + whatever it managed to parse
    }
    apply_config(&server);
    server.current_ws_idx = 0;
    for (int i = 0; i < NUM_WORKSPACES; ++i) {
        wl_list_init(&server.workspaces[i].toplevels);
        wl_list_init(&server.workspaces[i].floating_toplevels);
        server.workspaces[i].output = NULL; // Will be assigned when outputs appear
        server.workspaces[i].id = i;
    }
    server.focused_toplevel = NULL;
    server.global_floating = false;
    server.next_toplevel_should_float = false;
    // --- end sxwm feature initialization ---


	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server.cursor_mgr, 1.0); // Load default theme at scale 1
    // Set initial cursor, sxwm uses "left_ptr", "fleur", "bottom_right_corner"
    // wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "left_ptr"); // Default


	server.cursor_mode = SWM_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    signal(SIGCHLD, SIG_IGN); // Prevent zombie processes from exec

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
            perror("execl startup_cmd");
            _exit(1);
		}
	} else { // Default startup if nothing specified (e.g. a terminal)
        const char *term_cmd[] = {"foot", NULL}; // Example, use CMD(terminal) from config
        // spawn_swwm(&server, server.config.binds[findIndexFor("terminal")].arg);
        // This requires a more robust way to get default commands if not via keybind
    }

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);

	// Cleanup
    // Free config resources
    for (int i = 0; i < server.config.bindsn; ++i) {
        if (server.config.binds[i].type == TYPE_CMD && server.config.binds[i].action.cmd) {
            for (int k = 0; server.config.binds[i].action.cmd[k]; ++k) {
                free((void*)server.config.binds[i].action.cmd[k]);
            }
            free(server.config.binds[i].action.cmd);
        }
    }
    for (int i = 0; i < server.config.should_floatn; ++i) {
        free(server.config.should_float[i]);
    }

	wl_display_destroy_clients(server.wl_display);
    wlr_scene_node_destroy(&server.scene->tree.node); // Destroys all children including toplevel_layer
	wlr_output_layout_destroy(server.output_layout);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
    wlr_seat_destroy(server.seat); // Destroy seat before backend usually
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
