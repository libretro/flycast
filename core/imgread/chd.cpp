#include "common.h"
#include <streams/file_stream.h>

#include "deps/libchdr/include/libchdr/chd.h"
#include <thread>
#include <mutex>
#include <condition_variable>

/* tracks are padded to a multiple of this many frames */
const uint32_t CD_TRACK_PADDING = 4;

struct CHDDisc : Disc
{
	chd_file* chd;
	RFILE* chd_fp = nullptr;	/* owned here: chd_open_file does not take ownership */
	u8* hunk_mem;
	u32 old_hunk;

	u32 hunkbytes;
	u32 sph;

	/* --- async hunk prefetch (determinism-safe: CHD decompression is
	 * deterministic, so a background-decompressed hunk is byte-identical to a
	 * synchronously decompressed one; the emulated read gets the same data
	 * either way, this only moves the decompress CPU off the emu thread). --- */
	chd_file* chd2 = nullptr;          /* separate handle for the worker */
	RFILE*    chd_fp2 = nullptr;
	u8*  worker_buf = nullptr;         /* worker-private decompress target */
	u8*  next_buf = nullptr;           /* published prefetched hunk */
	static const u32 NO_HUNK = 0xFFFFFFFFu;
	u32  next_hunk = NO_HUNK;
	bool next_ready = false;
	u32  total_hunks = 0;
	u32  prefetch_req = NO_HUNK;
	bool worker_stop = false;
	bool prefetch_enabled = false;
	std::thread worker;
	std::mutex  mtx;
	std::condition_variable cv;

	void prefetch_worker()
	{
		std::unique_lock<std::mutex> lk(mtx);
		for (;;)
		{
			cv.wait(lk, [&]{ return worker_stop
				|| (prefetch_req != NO_HUNK && !(next_ready && next_hunk == prefetch_req)); });
			if (worker_stop)
				break;
			u32 req = prefetch_req;
			if (req == NO_HUNK || req >= total_hunks || (next_ready && next_hunk == req))
				continue;
			lk.unlock();
			chd_read(chd2, req, worker_buf);   /* slow decompress, off-thread, no lock */
			lk.lock();
			std::swap(worker_buf, next_buf);
			next_hunk = req;
			next_ready = true;
		}
	}

	/* Return a pointer to the decompressed hunk, preferring the prefetched copy. */
	u8* hunk_get(u32 hunk)
	{
		if (hunk == old_hunk)
			return hunk_mem;               /* still the current hunk */
		if (!prefetch_enabled)
		{
			chd_read(chd, hunk, hunk_mem);
			old_hunk = hunk;
			return hunk_mem;
		}
		std::unique_lock<std::mutex> lk(mtx);
		if (next_ready && next_hunk == hunk)
		{
			std::swap(next_buf, hunk_mem); /* take prefetched hunk -- no decompress */
			next_ready = false;
			old_hunk = hunk;
			prefetch_req = hunk + 1;
			cv.notify_one();
			return hunk_mem;
		}
		/* miss (first read or seek): decompress synchronously, re-aim prefetch */
		prefetch_req = hunk + 1;
		cv.notify_one();
		lk.unlock();
		chd_read(chd, hunk, hunk_mem);
		old_hunk = hunk;
		return hunk_mem;
	}

	void start_prefetch(const char* file)
	{
		chd_fp2 = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (chd_fp2 == nullptr)
			return;
		if (chd_open_file(chd_fp2, CHD_OPEN_READ, 0, &chd2) != CHDERR_NONE)
		{
			filestream_close(chd_fp2); chd_fp2 = nullptr; chd2 = nullptr;
			return;
		}
		worker_buf = new u8[hunkbytes];
		next_buf   = new u8[hunkbytes];
		prefetch_enabled = true;
		worker = std::thread(&CHDDisc::prefetch_worker, this);
	}

	void stop_prefetch()
	{
		if (worker.joinable())
		{
			{ std::unique_lock<std::mutex> lk(mtx); worker_stop = true; cv.notify_one(); }
			worker.join();
		}
		if (chd2) chd_close(chd2);
		if (chd_fp2) filestream_close(chd_fp2);
		delete [] worker_buf;
		delete [] next_buf;
		chd2 = nullptr; chd_fp2 = nullptr; worker_buf = nullptr; next_buf = nullptr;
		prefetch_enabled = false;
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
		if (chd)
			chd_close(chd);
		if (chd_fp)
			filestream_close(chd_fp);
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
};

bool CHDDisc::TryOpen(const char* file)
{
	chd_fp = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (chd_fp == nullptr)
		return false;
	chd_error err=chd_open_file(chd_fp,CHD_OPEN_READ,0,&chd);

	if (err!=CHDERR_NONE)
	{
		INFO_LOG(GDROM, "chd: chd_open failed for file %s: %d", file, err);
		return false;
	}

	INFO_LOG(GDROM, "chd: parsing file %s", file);

	const chd_header* head = chd_get_header(chd);

	hunkbytes = head->hunkbytes;
	total_hunks = head->totalhunks;
	hunk_mem = new u8[hunkbytes];
	old_hunk=0xFFFFFFF;

	sph = hunkbytes/(2352+96);

	if (hunkbytes%(2352+96)!=0)
	{
		INFO_LOG(GDROM, "chd: hunkbytes is invalid, %d\n",hunkbytes);
		return false;
	}

	u32 tag;
	u8 flags;
	char temp[512];
	u32 temp_len;
	u32 total_frames = 150;

	u32 Offset = 0;

	for(;;)
	{
		char type[16], subtype[16], pgtype[16], pgsub[16];
		int tkid=-1,frames=0,pregap=0,postgap=0, padframes=0;

		err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
		if (err == CHDERR_NONE)
		{
			//"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
			sscanf(temp, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
		}
		else if (CHDERR_NONE == (err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags)) )
		{
			//CDROM_TRACK_METADATA_FORMAT	"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d"
			sscanf(temp, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames);
		}
		else
		{
			err = chd_get_metadata(chd, GDROM_OLD_METADATA_TAG, tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
			if (err != CHDERR_NONE)
			{
				err = chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
			}

			if (err == CHDERR_NONE)
			{
				//GDROM_TRACK_METADATA_FORMAT	"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PAD:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
				sscanf(temp, GDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames, &padframes, &pregap, pgtype, pgsub, &postgap);
			}
			else
			{
				break;
			}
		}

		if (tkid!=(tracks.size()+1) || (strcmp(type,"MODE1_RAW")!=0 && strcmp(type,"AUDIO")!=0 && strcmp(type,"MODE1")!=0) || strcmp(subtype,"NONE")!=0 || pregap!=0 || postgap!=0)
		{
			INFO_LOG(GDROM, "chd: track type %s is not supported", type);
			return false;
		}
		DEBUG_LOG(GDROM, "%s", temp);
		Track t;
      t.StartFAD = total_frames;
		total_frames += frames;
		t.EndFAD = total_frames - 1;
		t.ADDR = 0;
		t.CTRL = strcmp(type,"AUDIO") == 0 ? 0 : 4;
		t.file = new CHDTrack(this, t.StartFAD, Offset - t.StartFAD, strcmp(type,"MODE1") ? 2352 : 2048, t.CTRL == 0 && head->version >= 5);

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
