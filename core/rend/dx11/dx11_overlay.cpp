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
#include "dx11_overlay.h"
#include "dx11context_lr.h"

bool DX11Overlay::Upload(Picture& picture, int width, int height, const u8 *rgba)
{
	DX11Context *context = DX11Context::Instance();
	if (!picture.texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(context->GetDevice()->CreateTexture2D(&desc, nullptr, picture.texture.put()))
				|| FAILED(context->GetDevice()->CreateShaderResourceView(picture.texture, nullptr, picture.view.put())))
		{
			picture.view.reset();
			picture.texture.reset();
			return false;
		}
	}
	context->GetDeviceContext()->UpdateSubresource(picture.texture, 0, nullptr, rgba, width * 4, width * height * 4);
	return true;
}

void DX11Overlay::Prepare()
{
	for (int i = 0; i < 4; i++)
	{
		vmu_screen_params_t& param = vmu_screen_params[i];
		// a new picture from the game, if there is one
		const bool changed = vmu_lcd_refresh(i);

		if (!param.vmu_screen_display || vmu_lcd_pixels(i) == nullptr)
		{
			vmus[i] = Picture();
			continue;
		}
		if (vmus[i].texture && !changed && !param.vmu_screen_needs_update)
			continue;

		u8 pixels[VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH * 4];
		u8 *dst = pixels;
		const u8 *src = vmu_lcd_pixels(i);
		for (int n = 0; n < VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH; n++)
		{
			if (*src++)
			{
				*dst++ = param.vmu_pixel_on_R;
				*dst++ = param.vmu_pixel_on_G;
				*dst++ = param.vmu_pixel_on_B;
			}
			else
			{
				*dst++ = param.vmu_pixel_off_R;
				*dst++ = param.vmu_pixel_off_G;
				*dst++ = param.vmu_pixel_off_B;
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
			crosshairs[i] = Picture();
			continue;
		}
		if (crosshairs[i].texture && !gun.dirty)
			continue;

		u8 pixels[LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE * 4];
		u8 *dst = pixels;
		const u8 *colour = &lightgun_palette[gun.colour * 3];
		const u8 *src = lightgun_img_crosshair;
		for (int n = 0; n < LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE; n++)
		{
			const bool on = *src++ != 0;
			*dst++ = on ? colour[0] : 0;
			*dst++ = on ? colour[1] : 0;
			*dst++ = on ? colour[2] : 0;
			*dst++ = on ? 0xff : 0;
		}
		if (Upload(crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, pixels))
			gun.dirty = false;
	}
}

void DX11Overlay::Draw(int width, int height)
{
	DX11Context *context = DX11Context::Instance();
	DX11Quad& quad = context->GetQuad();
	ID3D11SamplerState *sampler = context->GetSamplers().GetPointSampler();

	/* (pixels to clip space, y going up; the pictures' first line is their
	 * bottom one, as the other renderers draw them) */
	auto draw = [&](ID3D11ShaderResourceView *view, float x, float y, float w, float h, ID3D11BlendState *blend, const float *factor) {
		quad.Draw(view, sampler, x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height, w * 2.f / width, h * 2.f / height,
				0.f, 1.f, 1.f, 0.f, blend, factor);
	};

	for (int i = 0; i < 4; i++)
	{
		if (!vmus[i].view)
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
		const float alpha = param.vmu_screen_opacity / 255.f;
		const float factor[4] = { alpha, alpha, alpha, alpha };
		draw(vmus[i].view, x, y, w, h, context->GetBlendStates().GetFactorAlpha(), factor);
	}

	for (int i = 0; i < 4; i++)
	{
		if (!crosshairs[i].view)
			continue;
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		// (the game's 640x480 is in the middle of a widescreen picture)
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		const float x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		const float y = lightgun_params[i].y * height / 480 - h / 2;
		draw(crosshairs[i].view, x, y, w, h, context->GetBlendStates().GetAlpha(), nullptr);
	}
}

void DX11Overlay::Term()
{
	for (auto& picture : vmus)
		picture = Picture();
	for (auto& picture : crosshairs)
		picture = Picture();
}
