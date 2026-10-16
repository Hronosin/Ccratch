/*
 * ccr_pie - a Blender-style pie menu: everything the keyboard can do,
 * for people who only want the mouse.
 *
 * Right click (without dragging, a drag still pans) opens it around the
 * cursor. Each action sits in a fixed direction, so after a while it is
 * muscle memory: flick towards it and click. The direction is all that
 * counts, not hitting the button: anywhere past the small dead zone in
 * the middle picks the slice the mouse points into.
 *
 * Knows nothing about the editor: the app says which actions make sense
 * right now, the pie says which one was picked.
 */
#ifndef CCR_PIE_H
#define CCR_PIE_H

#include "ccr_gui.h"

#include <stdbool.h>

/* The eight slices, clockwise from the top. */
typedef enum ccr_pie_dir {
    CCR_PIE_N, CCR_PIE_NE, CCR_PIE_E, CCR_PIE_SE,
    CCR_PIE_S, CCR_PIE_SW, CCR_PIE_W, CCR_PIE_NW,
    CCR_PIE_DIRS
} ccr_pie_dir;

typedef enum ccr_action {
    CCR_ACTION_NONE = -1,
    CCR_ACTION_SAVE,        /* N   Ctrl+S */
    CCR_ACTION_COMMENT,     /* NE  Ctrl+/ */
    CCR_ACTION_REDO,        /* E   Ctrl+Y */
    CCR_ACTION_EDIT_TEXT,   /* SE  click a white field */
    CCR_ACTION_DELETE,      /* S   Delete */
    CCR_ACTION_FIT_VIEW,    /* SW  Home */
    CCR_ACTION_UNDO,        /* W   Ctrl+Z */
    CCR_ACTION_DUPLICATE,   /* NW  Ctrl+D */
    CCR_ACTION_COUNT
} ccr_action;

/* Where each action lives: action i is in direction i. */
_Static_assert((int)CCR_ACTION_COUNT == (int)CCR_PIE_DIRS, "one action per slice");

typedef struct ccr_pie {
    bool       open;
    ccr_vec2   center;                      /* screen position */
    bool       enabled[CCR_ACTION_COUNT];
    bool       uncomment;                   /* the comment slice says "Uncomment" */
    ccr_action hot;                         /* where the mouse points now */
    bool       animate;                     /* pops out instead of just appearing */
    double     opened_at;                   /* ccr_gui_time() when it opened */
} ccr_pie;

/* Beyond this distance from the centre the mouse picks a slice. */
#define CCR_PIE_DEAD_ZONE 26.0f

void        ccr_pie_open(ccr_pie *pie, ccr_vec2 center, const bool enabled[CCR_ACTION_COUNT],
                         bool uncomment);
void        ccr_pie_close(ccr_pie *pie);

/* The slice `mouse` points into, NONE inside the dead zone. Disabled
 * slices are still returned (they are drawn hot, but cannot be picked). */
ccr_action  ccr_pie_aim(const ccr_pie *pie, ccr_vec2 mouse);

/*
 * One frame. `click`: the left button went down; `cancel`: Esc or the
 * right button. Returns the action picked this frame, if any; the pie
 * closes on a pick, on cancel, and on a click in the dead zone or on a
 * disabled slice.
 */
ccr_action  ccr_pie_update(ccr_pie *pie, ccr_vec2 mouse, bool click, bool cancel);

void        ccr_pie_draw(const ccr_pie *pie);

const char *ccr_action_label(ccr_action a, bool uncomment);
const char *ccr_action_shortcut(ccr_action a);

#endif /* CCR_PIE_H */
