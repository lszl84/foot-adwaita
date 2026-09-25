#pragma once

/*
 * libadwaita-style client side decorations: a GNOME header bar with
 * round window buttons, rounded window corners, and the layered
 * window shadow used by libadwaita (values from libadwaita 1.7).
 */

#include <stdbool.h>

#include "render.h"
#include "shm.h"
#include "terminal.h"

/* Logical (unscaled) sizes, matching libadwaita's stylesheet */
#define ADW_HEADER_HEIGHT       46  /* toolbarview > .top-bar headerbar */
#define ADW_HEADER_SHADE_HEIGHT  8  /* .top-bar.raised shadow below the header */
#define ADW_SHADOW_EXTENT       54  /* largest window.csd box-shadow extent */
#define ADW_RESIZE_HANDLE       12  /* resize area outside the window frame */
#define ADW_BUTTON_SIZE         34  /* windowcontrols > button */
#define ADW_BUTTON_SPACING       3
#define ADW_HEADER_PADDING_X     7
#define ADW_HEADER_PADDING_Y     6

struct wayland;
struct config;
void adw_prewarm(const struct wayland *wayl, const struct config *conf);

/* Returns false if the borders are unchanged since last time */
bool adw_borders_need_render(struct terminal *term);

void adw_render_border(struct terminal *term, enum csd_surface surf_idx,
                       const struct csd_data *info, struct buffer *buf);
void adw_render_title(struct terminal *term, const struct csd_data *info,
                      struct buffer *buf);
void adw_render_button(struct terminal *term, enum csd_surface surf_idx,
                       struct buffer *buf, bool hover, bool pressed);

/* Rounded bottom corners of the terminal grid surface */
void adw_grid_corners_restore(struct terminal *term, struct buffer *buf);
void adw_grid_corners_apply(struct terminal *term, struct buffer *buf);
void adw_grid_corners_reset(struct terminal *term);
