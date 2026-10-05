/*
	 Copyright 2018 flyinghead
 
	 This file is part of reicast.
 
	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.
 
	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.
 
	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "TexCache.h"
#include "stdclass.h"
#include "lockfree.h"

#include <string>
#include <vector>
#include <map>

class CustomTexture {
public:
	CustomTexture() : loader_thread(loader_thread_func, this) { retro_atomic_int_init(&initialized, 0); }
	~CustomTexture() { Terminate(); }
	u8* LoadCustomTexture(u32 hash, int& width, int& height);
	void LoadCustomTextureAsync(BaseTextureCacheData *texture_data);
   void DumpTexture(u32 hash, int w, int h, TextureType textype, void *src_buffer);
	void Terminate();

private:
	bool Init();
	void LoaderThread();
	u8* LoadPNG(const std::string& fname, int &width, int &height);
	std::string GetGameId();
	void LoadMap();
	
	static void *loader_thread_func(void *param) { ((CustomTexture *)param)->LoaderThread(); return NULL; }
	
	void Load(BaseTextureCacheData *texture);

	retro_atomic_int_t initialized;
	bool custom_textures_available = false;
	std::string textures_path;
	cThread loader_thread;
	cResetEvent wakeup_thread;
	/* Textures waiting for the loader thread. A texture is on the list at
	 * most once: it is pushed when its custom_load_in_progress count leaves
	 * zero, and the loader only lets the count return to zero once it is
	 * done with the texture. */
	cMpscList<BaseTextureCacheData, &BaseTextureCacheData::custom_load_next> work_queue;
	std::map<u32, std::string> texture_map;
};

extern CustomTexture custom_texture;
