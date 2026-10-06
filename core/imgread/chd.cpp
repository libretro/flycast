#include "common.h"

#include "chd_image.h"
#include "hunk_prefetch.h"

/* tracks are padded to a multiple of this many frames */
const uint32_t CD_TRACK_PADDING = 4;

struct CHDDisc : Disc
{
	chd_image_t* chd;
	u8* hunk_mem;
	u32 old_hunk;

	u32 hunkbytes;
	u32 sph;

	/* Read-ahead on a worker thread, with a handle of its own. */
	chd_image_t* chd2 = nullptr;
	HunkPrefetch prefetch;
	static const u32 NO_HUNK = HunkPrefetch::NO_HUNK;
	u32  total_hunks = 0;

	static bool prefetch_read(void *opaque, u32 hunk, u8 *dst)
	{
		return chd_image_read_hunk(((CHDDisc *)opaque)->chd2, hunk, dst);
	}

	/* Decompress @hunk into hunk_mem on this thread. A hunk that fails to
	 * decode reads as zeros and is not kept, so the next read retries it. */
	void hunk_read_sync(u32 hunk)
	{
		if (chd_image_read_hunk(chd, hunk, hunk_mem))
			old_hunk = hunk;
		else
		{
			WARN_LOG(GDROM, "chd: hunk %u failed to decode", hunk);
			memset(hunk_mem, 0, hunkbytes);
			old_hunk = NO_HUNK;
		}
	}

	/* Return a pointer to the decompressed hunk, preferring the prefetched copy. */
	u8* hunk_get(u32 hunk)
	{
		if (hunk == old_hunk)
			return hunk_mem;               /* still the current hunk */
		if (prefetch.Running())
		{
			/* Takes the prefetched hunk if there is one and aims the worker
			 * at the next; on a miss (first read or seek) the worker gets
			 * going on the next hunk while this thread decodes this one. */
			bool hit = prefetch.Fetch(hunk);
			hunk_mem = prefetch.Buffer();
			old_hunk = hit ? hunk : NO_HUNK;
			if (hit)
				return hunk_mem;
		}
		hunk_read_sync(hunk);
		return hunk_mem;
	}

	void start_prefetch(const char* file)
	{
		chd2 = chd_image_open(file, NULL, 0);
		if (chd2 == nullptr)
			return;
		if (!prefetch.Start(prefetch_read, this, hunkbytes, total_hunks))
		{
			chd_image_close(chd2);
			chd2 = nullptr;
			return;
		}
		/* The prefetcher's buffers take over from the one TryOpen made. */
		delete [] hunk_mem;
		hunk_mem = prefetch.Buffer();
		old_hunk = NO_HUNK;
	}

	void stop_prefetch()
	{
		if (prefetch.Running())
		{
			prefetch.Stop();
			hunk_mem = nullptr;
			old_hunk = NO_HUNK;
		}
		chd_image_close(chd2);
		chd2 = nullptr;
	}

	CHDDisc()
	{
		chd=0;
		hunk_mem=0;
	}

	bool TryOpen(const char* file);

	~CHDDisc()
	{
		stop_prefetch();
		if (hunk_mem)
			delete [] hunk_mem;
		chd_image_close(chd);
	}
};

struct CHDTrack : TrackFile
{
	CHDDisc* disc;
	u32 StartFAD;
	u32 Offset;
	u32 fmt;
	bool swap_bytes;

	CHDTrack(CHDDisc* disc, u32 StartFAD,u32 Offset, u32 fmt, bool swap_bytes)
	{
		this->disc=disc;
		this->StartFAD=StartFAD;
		this->Offset=Offset;
		this->fmt=fmt;
		this->swap_bytes = swap_bytes;
	}

	virtual void Read(u32 FAD,u8* dst,SectorFormat* sector_type,u8* subcode,SubcodeFormat* subcode_type)
	{
		u32 fad_offs = FAD + Offset;
		u32 hunk=(fad_offs)/disc->sph;
		u8* hmem = disc->hunk_get(hunk);

		u32 hunk_ofs=fad_offs%disc->sph;

		memcpy(dst,hmem+hunk_ofs*(2352+96),fmt);

		if (swap_bytes)
		{
			for (int i = 0; i < fmt; i += 2)
			{
				u8 b = dst[i];
				dst[i] = dst[i + 1];
				dst[i + 1] = b;
			}
		}

		*sector_type=fmt==2352?SECFMT_2352:SECFMT_2048_MODE1;

		//While space is reserved for it, the images contain no actual subcodes
		//memcpy(subcode,disc->hunk_mem+hunk_ofs*(2352+96)+2352,96);
		*subcode_type=SUBFMT_NONE;
	}

	/* The sector inside the decoded hunk; a track whose bytes have to
	 * be swapped is read through a buffer. */
	virtual const u8* View(u32 FAD, SectorFormat* sector_type)
	{
		u32 fad_offs = FAD + Offset;
		u8* hmem;

		if (swap_bytes)
			return NULL;
		hmem = disc->hunk_get(fad_offs / disc->sph);
		*sector_type = fmt==2352?SECFMT_2352:SECFMT_2048_MODE1;
		return hmem + (fad_offs % disc->sph) * (2352 + 96);
	}
};

bool CHDDisc::TryOpen(const char* file)
{
	char err[512];

	err[0] = '\0';
	chd = chd_image_open(file, err, sizeof(err));
	if (chd == nullptr)
	{
		INFO_LOG(GDROM, "chd: %s", err[0] ? err : "out of memory");
		return false;
	}

	INFO_LOG(GDROM, "chd: parsing file %s", file);

	hunkbytes = chd_image_hunk_bytes(chd);
	total_hunks = chd_image_hunk_count(chd);
	hunk_mem = new u8[hunkbytes];
	old_hunk = NO_HUNK;

	sph = hunkbytes/(2352+96);

	if (hunkbytes%(2352+96)!=0)
	{
		INFO_LOG(GDROM, "chd: hunkbytes is invalid, %d\n",hunkbytes);
		return false;
	}

	u32 total_frames = 150;

	u32 Offset = 0;

	for(;;)
	{
		chd_image_track_t meta;

		if (!chd_image_track(chd, (uint32_t)tracks.size(), &meta))
			break;

		int tkid = meta.track, frames = meta.frames, pregap = meta.pregap, postgap = meta.postgap;
		const char *type = meta.type, *subtype = meta.subtype;

		if (tkid!=(tracks.size()+1) || (strcmp(type,"MODE1_RAW")!=0 && strcmp(type,"AUDIO")!=0 && strcmp(type,"MODE1")!=0) || strcmp(subtype,"NONE")!=0 || pregap!=0 || postgap!=0)
		{
			INFO_LOG(GDROM, "chd: track type %s is not supported", type);
			return false;
		}
		DEBUG_LOG(GDROM, "chd: track %d %s %d frames", tkid, type, frames);
		Track t;
      t.StartFAD = total_frames;
		total_frames += frames;
		t.EndFAD = total_frames - 1;
		t.ADDR = 0;
		t.CTRL = strcmp(type,"AUDIO") == 0 ? 0 : 4;
		t.file = new CHDTrack(this, t.StartFAD, Offset - t.StartFAD, strcmp(type,"MODE1") ? 2352 : 2048, t.CTRL == 0 && chd_image_version(chd) >= 5);

		int padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
		Offset += padded * CD_TRACK_PADDING;

		tracks.push_back(t);
	}

	if (total_frames!=549300 || tracks.size()<3)
		WARN_LOG(GDROM, "WARNING: chd: Total frames is wrong: %u frames in %zu tracks", total_frames, tracks.size());

	FillGDSession();

	start_prefetch(file);

	return true;
}


Disc* chd_parse(const char* file)
{
	// Only try to open .chd files
	size_t len = strlen(file);
	if (len > 4 && stricmp( &file[len - 4], ".chd"))
		return nullptr;

	CHDDisc* rv = new CHDDisc();

	if (rv->TryOpen(file))
		return rv;
	else
	{
		delete rv;
		return 0;
	}
}
