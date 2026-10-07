#pragma once
/* The Vulkan renderers' side of rend/rtt_watch.h: a render to a texture
 * stays in the texture it was drawn into, and is brought to video memory
 * when the game first touches it there. Both renderers use this. */
#include "types.h"
#include "vulkan.h"

class Texture;

/* Before a render to (addr, width by height pixels) touches its texture:
 * what was waiting in that memory is dealt with while its picture is still
 * what it was. */
void vk_rtt_supersede(u32 addr, u32 width, u32 height);
/* The render is finished and submitted. */
void vk_rtt_watch(Texture *texture, u32 addr, u32 width, u32 height);
/* The renderer is going. */
void vk_rtt_term(void);

/* The pixels of an image that can be copied from and is in the layout it
 * is sampled in: width by height, R G B A. Waits for the card. */
void vk_read_picture(vk::Image image, vk::Format format, u32 width, u32 height, u8 *rgba);
