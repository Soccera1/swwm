/* See LICENSE for more information on use */
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h> // For XKB_KEY_*
#include "defs.h" // Uses SWM_MOD_* now

CMD(terminal, "st"); // Example, replace with your Wayland terminal e.g. "foot" or "kitty"
CMD(browser, "firefox");

const Binding binds[] = {
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_e, {.fn = quit_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_q, {.fn = close_focused_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO, XKB_KEY_j, {.fn = focus_next_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO, XKB_KEY_k, {.fn = focus_prev_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_j, {.fn = move_master_next_swwm}, TYPE_FUNC, NULL}, // Move focused to next in stack
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_k, {.fn = move_master_prev_swwm}, TYPE_FUNC, NULL}, // Move focused to prev in stack / make master

    {SWM_MOD_LOGO, XKB_KEY_l, {.fn = resize_master_add_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO, XKB_KEY_h, {.fn = resize_master_sub_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO, XKB_KEY_equal, {.fn = inc_gaps_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO, XKB_KEY_minus, {.fn = dec_gaps_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO, XKB_KEY_space, {.fn = toggle_floating_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_space, {.fn = toggle_floating_global_swwm}, TYPE_FUNC, NULL},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_f, {.fn = toggle_fullscreen_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO, XKB_KEY_Return, {.cmd = terminal}, TYPE_CMD, (void*)terminal},
    {SWM_MOD_LOGO, XKB_KEY_b, {.cmd = browser}, TYPE_CMD, (void*)browser},
    {SWM_MOD_LOGO, XKB_KEY_p, {.cmd = (const char *[]){"dmenu_run", NULL}}, TYPE_CMD, (void*)((const char *[]){"dmenu_run", NULL})}, // dmenu might need Wayland alternative like wofi or bemenu

    {SWM_MOD_LOGO, XKB_KEY_r, {.fn = reload_config_swwm}, TYPE_FUNC, NULL},

    {SWM_MOD_LOGO, XKB_KEY_1, {.ws = 0}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_1, {.ws = 0}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_2, {.ws = 1}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_2, {.ws = 1}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_3, {.ws = 2}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_3, {.ws = 2}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_4, {.ws = 3}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_4, {.ws = 3}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_5, {.ws = 4}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_5, {.ws = 4}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_6, {.ws = 5}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_6, {.ws = 5}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_7, {.ws = 6}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_7, {.ws = 6}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_8, {.ws = 7}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_8, {.ws = 7}, TYPE_MWKSP},
    {SWM_MOD_LOGO, XKB_KEY_9, {.ws = 8}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_9, {.ws = 8}, TYPE_MWKSP},
    // Workspace 10 example (0-indexed 9)
    {SWM_MOD_LOGO, XKB_KEY_0, {.ws = 9}, TYPE_CWKSP},
    {SWM_MOD_LOGO | SWM_MOD_SHIFT, XKB_KEY_0, {.ws = 9}, TYPE_MWKSP},
};
