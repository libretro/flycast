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

		if (swap_bytes)
		{
			/* The other byte order, made on the way across: 16 bits at a
			 * time, which the compiler does sixteen bytes at a time. (It
			 * was copied, and then swapped where it lay a byte at a time.) */
			const u8 *from = hmem + hunk_ofs * (2352 + 96);
			for (u32 i = 0; i + 1 < fmt; i += 2)
			{
				u16 w;
				memcpy(&w, from + i, 2);
				w = (u16)((w << 8) | (w >> 8));
				memcpy(dst + i, &w, 2);
			}
		}
		else
			memcpy(dst,hmem+hunk_ofs*(2352+96),fmt);

		*sector_type=Format();

		//While space is reserved for it, the images contain no actual subcodes
		//memcpy(subcode,disc->hunk_mem+hunk_ofs*(2352+96)+2352,96);
		*subcode_type=SUBFMT_NONE;
	}

	// (a CD's mode 2 track can be kept without the sync and header: 2336 bytes)
	SectorFormat Format() const
	{
		return fmt == 2352 ? SECFMT_2352 : fmt == 2336 ? SECFMT_2336_MODE2 : SECFMT_2048_MODE1;
	}

	/* The compressed bytes of every hunk the sectors span, so neither
	 * this thread nor the read-ahead worker stalls on a cold page. Both
	 * handles are on the same file, so one hint serves both. */
	virtual void Prefetch(u32 FAD, u32 count)
	{
		u32 first = (FAD + Offset) / disc->sph;
		u32 last  = (FAD + Offset + count - 1) / disc->sph;

		if (count)
			chd_image_prefetch(disc->chd, first, last - first + 1);
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
		*sector_type = Format();
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
	bool gdrom = chd_image_version(chd) < 5;	// a CD's image is only read from version 5 on
	u32 last_pregap = 0;

	for(;;)
	{
		chd_image_track_t meta;

		if (!chd_image_track(chd, (uint32_t)tracks.size(), &meta))
			break;

		int tkid = meta.track, frames = meta.frames, pregap = meta.pregap, postgap = meta.postgap;
		const char *type = meta.type, *subtype = meta.subtype;
		if (meta.gdrom)
			gdrom = true;

		/* The sector sizes of the CD and GD-ROM track types. The subcode
		 * is kept in its 96 bytes after each sector, whatever the subtype,
		 * and the sectors are where they are with it or without. */
		u32 sector_size = 0;
		if (!strcmp(type, "AUDIO") || !strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW"))
			sector_size = 2352;
		else if (!strcmp(type, "MODE1"))
			sector_size = 2048;
		else if (!strcmp(type, "MODE2"))
			sector_size = 2336;
		/* A pregap of type "V..." is kept in the image, at the start of
		 * the track's frames; any other is on the disc and not in the
		 * image. A postgap is never in the image. */
		const u32 pregap_kept = pregap > 0 && meta.pgtype[0] == 'V' ? (u32)pregap : 0;
		if (tkid!=(tracks.size()+1) || sector_size == 0 || frames < 0 || pregap < 0 || postgap < 0 || (int)pregap_kept > frames)
		{
			INFO_LOG(GDROM, "chd: track %d of type %s, %d frames, pregap %d is not supported", tkid, type, frames, pregap);
			return false;
		}
		if (strcmp(subtype, "NONE") != 0)
			WARN_LOG(GDROM, "chd: track %d has subcode (%s): not used", tkid, subtype);
		// (a disc has 99 tracks at most, and so has its table of contents)
		if (tracks.size() >= 99)
		{
			INFO_LOG(GDROM, "chd: more than 99 tracks");
			return false;
		}
		DEBUG_LOG(GDROM, "chd: track %d %s %d frames", tkid, type, frames);
		Track t;
		// (where INDEX 01 is: after the pregap)
      t.StartFAD = total_frames + pregap;
		total_frames = t.StartFAD + frames - pregap_kept;
		t.EndFAD = total_frames - 1;
		t.ADDR = 0;
		t.CTRL = strcmp(type,"AUDIO") == 0 ? 0 : 4;
		last_pregap = pregap;
		/* Music is kept in the other byte order in a version 5 image (and
		 * in any of a CD, which is read from version 5 only). */
		t.file = new CHDTrack(this, t.StartFAD, Offset + pregap_kept - t.StartFAD, sector_size, t.CTRL == 0 && chd_image_version(chd) >= 5);

		int padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
		Offset += padded * CD_TRACK_PADDING;

		tracks.push_back(t);
	}

	// (a hard disk's image, or anything else with no track in it, is no disc)
	if (tracks.empty())
	{
		INFO_LOG(GDROM, "chd: no track found");
		return false;
	}

	if (gdrom)
	{
		if (total_frames!=549300 || tracks.size()<3)
			WARN_LOG(GDROM, "WARNING: chd: Total frames is wrong: %u frames in %zu tracks", total_frames, tracks.size());

		FillGDSession();
	}
	else
	{
		/* A CD. The image does not say where its sessions are: one that
		 * ends in a data track after others is taken for a MIL-CD, its
		 * last track alone in a second session; any other has one. */
		Session ses;
		ses.FirstTrack = 1;
		ses.StartFAD = tracks[0].StartFAD;
		sessions.push_back(ses);
		LeadOut.ADDR = 1;	// subcode-q channel
		LeadOut.CTRL = 4;	// data
		if (tracks.size() > 1 && tracks.back().CTRL == 4)
		{
			/* Between the sessions: the first one's lead-out (01:30:00),
			 * the second's lead-in (01:00:00) and the pregap of its
			 * track (00:02:00), which is counted already when the image
			 * has it. */
			const u32 gap = 6750 + 4500 + 150 - std::min(last_pregap, (u32)150);
			type = CdRom_XA;
			tracks.back().StartFAD += gap;
			tracks.back().EndFAD += gap;
			((CHDTrack *)tracks.back().file)->Offset -= gap;
			ses.FirstTrack = tracks.size();
			ses.StartFAD = tracks.back().StartFAD;
			sessions.push_back(ses);
			EndFAD = LeadOut.StartFAD = total_frames + gap;
		}
		else
		{
			type = CdRom;
			for (size_t i = 0; tracks.size() > 1 && i < tracks.size(); i++)
				if (tracks[i].CTRL == 4)
					type = CdRom_XA;
			EndFAD = LeadOut.StartFAD = total_frames;
		}
	}

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
