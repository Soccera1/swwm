#pragma once

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h> // For XKB_KEY_*
#include <wayland-server-core.h>        // For wl_list
#include <wlr/types/wlr_box.h>          // For wlr_box
#include <wlr/types/wlr_keyboard.h>     // For wlr_keyboard_modifiers definition

extern const Binding binds[];

// Forward declarations from swwm.c
struct swwm_server;
struct swwm_toplevel;
struct swwm_output;

#define NUM_WORKSPACES 10
#define MAX_CLIENTS 256   // Max total clients
#define MAX_MONITORS 8    // Max monitors for master_width array
#define MAX_ARGS 64       // From parserh.txt

// From config.txt (and sxwm)
#define CMD(name, cmd_str) static const char *name[] = {cmd_str, NULL}

// Modifier masks for config parsing (map to wlr_keyboard_modifiers)
enum {
    SWM_MOD_SHIFT = 1 << 0,
    SWM_MOD_CAPS  = 1 << 1,
    SWM_MOD_CTRL  = 1 << 2,
    SWM_MOD_ALT   = 1 << 3, // Mod1
    SWM_MOD_MOD2  = 1 << 4, // NumLock
    SWM_MOD_MOD3  = 1 << 5,
    SWM_MOD_LOGO  = 1 << 6, // Super/Win (Mod4)
    SWM_MOD_MOD5  = 1 << 7,
};

// Types of actions for bindings
typedef enum {
    TYPE_CMD,
    TYPE_FUNC,
    TYPE_CWKSP, // Change Workspace
    TYPE_MWKSP, // Move to Workspace
} ActionType;

// Structure for key/mouse bindings
typedef struct {
    uint32_t mods;       // Modifier mask (combination of SWM_MOD_*)
    xkb_keysym_t keysym; // Key symbol (e.g., XKB_KEY_Return)
    ActionType type;
    union {
        void (*fn)(struct swwm_server *server, const void *arg); // Function to call
        const char **cmd;                                 // Command to execute
        int ws;                                           // Workspace index
    } action;
    const void *arg; // Optional argument for functions (e.g. for spawn)
} Binding;

// Configuration structure
typedef struct {
    uint32_t modkey;       // Default modifier (e.g., SWM_MOD_LOGO)
    int gaps;              // Gaps between windows
    int border_width;      // Border width (visuals not fully implemented)
    // Colors would be uint32_t RGBA if implemented fully
    unsigned long border_foc_col_val; // Raw value from XParseColor equivalent
    unsigned long border_ufoc_col_val;
    unsigned long border_swap_col_val;

    float master_width[MAX_MONITORS]; // Master area width percentage per monitor
    int motion_throttle_hz;           // Hz for motion events, 60 -> ~16ms
    int resize_master_amt;            // Percentage to resize master by
    int snap_distance;                // For floating windows (visuals not fully implemented)

    Binding binds[256]; // Max bindings
    int bindsn;         // Number of active bindings

    char **should_float[256]; // app_id patterns that should float
    int should_floatn;
} Config;


// Workspace structure
struct swwm_workspace {
    struct wl_list link; // To link workspaces if needed (not used if array)
    struct wl_list toplevels; // List of swwm_toplevels in this workspace (ordered for tiling)
    struct wl_list floating_toplevels; // List of floating toplevels
    struct swwm_output *output; // Output this workspace is primarily on (can be NULL)
    int id; // Workspace ID (0 to NUM_WORKSPACES-1)
    // Layout specific data:
    // float master_factor; // Current master factor for this workspace/output (use config.master_width[output_idx])
    // int num_master_windows; // Typically 1 for master-stack
};

// Functions that will be bound from config
// These will be implemented in swwm.c
void quit_swwm(struct swwm_server *server, const void *arg);
void close_focused_swwm(struct swwm_server *server, const void *arg);
void focus_next_swwm(struct swwm_server *server, const void *arg);
void focus_prev_swwm(struct swwm_server *server, const void *arg);
void move_master_next_swwm(struct swwm_server *server, const void *arg);
void move_master_prev_swwm(struct swwm_server *server, const void *arg);
void resize_master_add_swwm(struct swwm_server *server, const void *arg);
void resize_master_sub_swwm(struct swwm_server *server, const void *arg);
void inc_gaps_swwm(struct swwm_server *server, const void *arg);
void dec_gaps_swwm(struct swwm_server *server, const void *arg);
void toggle_floating_swwm(struct swwm_server *server, const void *arg);
void toggle_floating_global_swwm(struct swwm_server *server, const void *arg);
void toggle_fullscreen_swwm(struct swwm_server *server, const void *arg);
void reload_config_swwm(struct swwm_server *server, const void *arg);
void spawn_swwm(struct swwm_server *server, const void *arg_cmd_array);

// Internal functions not directly in call_table but used by bindings
void change_workspace_action(struct swwm_server *server, const void *arg_ws_idx);
void move_to_workspace_action(struct swwm_server *server, const void *arg_ws_idx);


// Helper: get currently focused toplevel
struct swwm_toplevel *get_focused_toplevel(struct swwm_server *server);
struct swwm_output *get_focused_output(struct swwm_server *server);


#define LENGTH(X) (sizeof X / sizeof X[0])
#define WORKSPACE_NAMES "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10\0"
#define MF_MIN 0.05f
#define MF_MAX 0.95f

#endif // DEFS_H
