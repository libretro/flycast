/* See last_picture.h. */
#include "last_picture.h"
#include <cstdlib>

static u8 *pixels;
static int pixels_w, pixels_h;

u8 *last_picture_keep(int width, int height)
{
	last_picture_drop();
	if (width <= 0 || height <= 0)
		return NULL;
	pixels = (u8 *)malloc((size_t)width * height * 4);
	if (pixels != NULL)
	{
		pixels_w = width;
		pixels_h = height;
	}
	return pixels;
}

const u8 *last_picture(int *width, int *height)
{
	if (pixels != NULL)
	{
		*width = pixels_w;
		*height = pixels_h;
	}
	return pixels;
}

void last_picture_drop(void)
{
	free(pixels);
	pixels = NULL;
}
