#pragma once
/* The screen's last picture, kept across the graphics context being
 * destroyed and made again (fullscreen, a change of video settings).
 *
 * A game that renders only part of the screen relies on the rest staying
 * as it was, and the rest is in images that go with the context. So the
 * picture is fetched when the context is about to go, and the first render
 * to the screen in the new one starts from it.
 *
 * R, G, B, A bytes, first line the top one. */
#include "types.h"

/* A context is going: room for a picture of this size, to be filled in. */
u8 *last_picture_keep(int width, int height);
/* The picture kept, or NULL. */
const u8 *last_picture(int *width, int *height);
/* It has been used, or is not wanted. */
void last_picture_drop(void);
