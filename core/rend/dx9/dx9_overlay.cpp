/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "dx9_overlay.h"
#include "dx9context_lr.h"

bool DX9Overlay::Upload(ComPtr<IDirect3DTexture9>& texture, int width, int height, const u8 *bgra)
{
	DX9Context *context = DX9Context::Instance();
	if (!texture)
	{
		texture = context->MakeTexture(width, height, 1, context->WriteUsage(), D3DFMT_A8R8G8B8);
		if (!texture)
			return false;
	}
	D3DLOCKED_RECT rect;
	if (FAILED(texture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
		return false;
	for (int y = 0; y < height; y++)
		memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, bgra + (size_t)y * width * 4, (size_t)width * 4);
	texture->UnlockRect(0);
	return true;
}

void DX9Overlay::Prepare()
{
	DX9Context *context = DX9Context::Instance();
	for (int i = 0; i < 4; i++)
	{
		vmu_screen_params_t& param = vmu_screen_params[i];
		// a new picture from the game, if there is one
		const bool changed = vmu_lcd_refresh(i);

		if (!param.vmu_screen_display || vmu_lcd_pixels(i) == nullptr)
		{
			context->Recycle(vmus[i]);
			continue;
		}
		if (vmus[i] && !changed && !param.vmu_screen_needs_update)
			continue;

		u8 pixels[VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH * 4];
		u8 *dst = pixels;
		const u8 *src = vmu_lcd_pixels(i);
		for (int n = 0; n < VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH; n++)
		{
			if (*src++)
			{
				*dst++ = param.vmu_pixel_on_B;
				*dst++ = param.vmu_pixel_on_G;
				*dst++ = param.vmu_pixel_on_R;
			}
			else
			{
				*dst++ = param.vmu_pixel_off_B;
				*dst++ = param.vmu_pixel_off_G;
				*dst++ = param.vmu_pixel_off_R;
			}
			*dst++ = 0xff;
		}
		if (Upload(vmus[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, pixels))
			param.vmu_screen_needs_update = false;
	}

	for (int i = 0; i < 4; i++)
	{
		lightgun_params_t& gun = lightgun_params[i];
		if (gun.offscreen || gun.colour == 0)
		{
			context->Recycle(crosshairs[i]);
			continue;
		}
		if (crosshairs[i] && !gun.dirty)
			continue;

		u8 pixels[LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE * 4];
		u8 *dst = pixels;
		const u8 *colour = &lightgun_palette[gun.colour * 3];
		const u8 *src = lightgun_img_crosshair;
		for (int n = 0; n < LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE; n++)
		{
			const bool on = *src++ != 0;
			*dst++ = on ? colour[2] : 0;
			*dst++ = on ? colour[1] : 0;
			*dst++ = on ? colour[0] : 0;
			*dst++ = on ? 0xff : 0;
		}
		if (Upload(crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, pixels))
			gun.dirty = false;
	}
}

void DX9Overlay::Draw(int width, int height)
{
	DX9Context *context = DX9Context::Instance();

	/* (pixels to clip space, y going up; the pictures' first line is their
	 * bottom one, as the other renderers draw them) */
	auto draw = [&](IDirect3DTexture9 *texture, float x, float y, float w, float h, float alpha) {
		context->DrawQuad(texture, false, false, x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height,
				w * 2.f / width, h * 2.f / height, 0.f, 1.f, 1.f, 0.f, true, alpha);
	};

	for (int i = 0; i < 4; i++)
	{
		if (!vmus[i])
			continue;
		const vmu_screen_params_t& param = vmu_screen_params[i];
		const float w = (float)(VMU_SCREEN_WIDTH * param.vmu_screen_size_mult);
		const float h = (float)(VMU_SCREEN_HEIGHT * param.vmu_screen_size_mult);
		float x = 0.f;
		float y = 0.f;
		switch (param.vmu_screen_position)
		{
		case UPPER_LEFT:
			break;
		case UPPER_RIGHT:
			x = width - w;
			break;
		case LOWER_LEFT:
			y = height - h;
			break;
		case LOWER_RIGHT:
			x = width - w;
			y = height - h;
			break;
		}
		// (the screen's pixels are opaque: blended by the opacity alone)
		draw(vmus[i], x, y, w, h, param.vmu_screen_opacity / 255.f);
	}

	for (int i = 0; i < 4; i++)
	{
		if (!crosshairs[i])
			continue;
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		// (the game's 640x480 is in the middle of a widescreen picture)
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		const float x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		const float y = lightgun_params[i].y * height / 480 - h / 2;
		draw(crosshairs[i], x, y, w, h, 1.f);
	}
}

void DX9Overlay::Term()
{
	for (auto& texture : vmus)
		texture.reset();
	for (auto& texture : crosshairs)
		texture.reset();
}
