/*
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
#include "dx12_overlay.h"
#include "dx12_quad.h"

bool DX12Overlay::Make(Picture& picture, u32 width, u32 height)
{
	DX12Context *context = DX12Context::Instance();
	picture.image.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	picture.image.width = width;
	picture.image.height = height;
	picture.shown = false;
	if (!dx12_make_texture(context->GetDevice(), width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
			picture.image.state, picture.image.resource))
		return false;
	picture.image.srv = context->Srv().Alloc();
	if (picture.image.srv == ~0u)
		return false;
	context->MakeSrv(picture.image.resource, DXGI_FORMAT_R8G8B8A8_UNORM, 1, picture.image.srv);
	return true;
}

bool DX12Overlay::Init()
{
	Term();
	bool ok = true;
	for (int i = 0; i < 4; i++)
	{
		ok = ok && Make(vmus[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT);
		ok = ok && Make(crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE);
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: the overlay's pictures could not be made");
		Term();
	}
	return ok;
}

bool DX12Overlay::Upload(Picture& picture, int width, int height, const u8 *rgba)
{
	DX12Context *context = DX12Context::Instance();
	if (!picture.image.resource)
		return false;
	const u32 pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	DX12Upload upload;
	if (!context->Upload(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, upload))
		return false;
	for (int y = 0; y < height; y++)
		memcpy(upload.cpu + (size_t)y * pitch, rgba + (size_t)y * width * 4, (size_t)width * 4);
	context->Transition(picture.image, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = picture.image.resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = upload.buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload.offset;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	context->Cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	context->Transition(picture.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

void DX12Overlay::Prepare()
{
	for (int i = 0; i < 4; i++)
	{
		vmu_screen_params_t& param = vmu_screen_params[i];
		// a new picture from the game, if there is one
		const bool changed = vmu_lcd_refresh(i);

		if (!param.vmu_screen_display || vmu_lcd_pixels(i) == nullptr)
		{
			vmus[i].shown = false;
			continue;
		}
		if (vmus[i].shown && !changed && !param.vmu_screen_needs_update)
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
		{
			param.vmu_screen_needs_update = false;
			vmus[i].shown = true;
		}
	}

	for (int i = 0; i < 4; i++)
	{
		lightgun_params_t& gun = lightgun_params[i];
		if (gun.offscreen || gun.colour == 0)
		{
			crosshairs[i].shown = false;
			continue;
		}
		if (crosshairs[i].shown && !gun.dirty)
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
		{
			gun.dirty = false;
			crosshairs[i].shown = true;
		}
	}
}

void DX12Overlay::Draw(int width, int height)
{
	/* (pixels to clip space, y going up; the pictures' first line is their
	 * bottom one, as the other renderers draw them) */
	auto draw = [&](const Picture& picture, float x, float y, float w, float h, u32 special, const float *factor) {
		DX12Quad::Draw(picture.image.srv, false, x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height, w * 2.f / width, h * 2.f / height,
				0.f, 1.f, 1.f, 0.f, special, factor);
	};

	for (int i = 0; i < 4; i++)
	{
		if (!vmus[i].shown)
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
		draw(vmus[i], x, y, w, h, DX12_BLEND_FACTOR, factor);
	}

	for (int i = 0; i < 4; i++)
	{
		if (!crosshairs[i].shown)
			continue;
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		// (the game's 640x480 is in the middle of a widescreen picture)
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		const float x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		const float y = lightgun_params[i].y * height / 480 - h / 2;
		draw(crosshairs[i], x, y, w, h, DX12_BLEND_ALPHA, nullptr);
	}
}

void DX12Overlay::Term()
{
	DX12Context *context = DX12Context::Instance();
	for (Picture *pictures : { vmus, crosshairs })
		for (int i = 0; i < 4; i++)
		{
			Picture& p = pictures[i];
			if (p.image.resource)
			{
				p.image.resource->AddRef();
				context->Release(p.image.resource.get());
				p.image.resource.reset();
			}
			context->ReleaseSrv(p.image.srv);
			p.image = DX12Image();
			p.shown = false;
		}
}
