#include "common.h"
#include <file/file_path.h>
#include <ctype.h>
#include <sstream>

Disc* load_gdi(const char* file)
{
	core_file* t=core_fopen(file);
	if (!t)
		return nullptr;

	size_t gdi_len = core_fsize(t);

	char gdi_data[8193] = { 0 };

	if (gdi_len >= sizeof(gdi_data))
	{
		WARN_LOG(GDROM, "GDI: file too big");
		core_fclose(t);
		return nullptr;
	}

	core_fread(t, gdi_data, gdi_len);
	core_fclose(t);

   std::istringstream gdi(gdi_data);

	u32 iso_tc = 0;
	gdi >> iso_tc;
	if (iso_tc == 0)
	{
		WARN_LOG(GDROM, "GDI: empty or invalid GDI file");
		return nullptr;
	}
	INFO_LOG(GDROM, "GDI : %d tracks", iso_tc);

	char path[512];
	if (strlen(file) >= sizeof(path))
	{
		WARN_LOG(GDROM, "GDI: path too long");
		return nullptr;
	}
	strcpy(path,file);
	ssize_t len=strlen(file);
	const char* delim = path_get_archive_delim(file);
	while (len>=0)
	{
		if (path[len]=='\\' || path[len]=='/' || (delim && &file[len] == delim))
			break;
		len--;
	}
	len++;
	char* pathptr=&path[len];

	Disc* disc = new Disc();
	u32 TRACK=0,FADS=0,CTRL=0,SSIZE=0;
	s32 OFFSET=0;
	for (u32 i=0;i<iso_tc;i++)
	{
      std::string track_filename;

		//TRACK FADS CTRL SSIZE file OFFSET
		gdi >> TRACK;
		gdi >> FADS;
		gdi >> CTRL;
		gdi >> SSIZE;

		/* (A file that ends here - in the middle of a line, or of a name
		 * in quotes that is never closed - ends the reading: these loops
		 * went on for ever on one, the second adding to the name until
		 * there was no memory left.) */
		char last = 0;

		do {
			gdi >> last;
		} while (gdi && isspace((unsigned char)last));
		
		if (last == '"')
		{
			gdi >> std::noskipws;
			for(;;) {
				if (!(gdi >> last) || last == '"')
					break;
				track_filename += last;
			}
			gdi >> std::skipws;
		}
		else
		{
			gdi >> track_filename;
			track_filename = last + track_filename;
		}

		gdi >> OFFSET;
		
		DEBUG_LOG(GDROM, "file[%d] \"%s\": FAD:%d, CTRL:%d, SSIZE:%d, OFFSET:%d", TRACK, track_filename.c_str(), FADS, CTRL, SSIZE, OFFSET);

		Track t;
		t.ADDR=0;
		t.StartFAD=FADS+150;
		t.EndFAD=0;		//fill it in
		t.file=0;
		t.CTRL = CTRL;

		if (SSIZE!=0)
		{
			/* (a name that does not fit is not copied past the end of
			 * the path; and a track whose file is not there is the image
			 * not loading, as for a cue sheet - it used to be a track
			 * with no file behind it, found out at the first read) */
			if (track_filename.size() >= sizeof(path) - len)
			{
				WARN_LOG(GDROM, "GDI: track %d: file name too long", TRACK);
				delete disc;
				return nullptr;
			}
			strcpy(pathptr, track_filename.c_str());
			core_file *track_file = core_fopen(path);
			if (track_file == nullptr)
			{
				WARN_LOG(GDROM, "GDI: cannot open track %d: %s", TRACK, path);
				delete disc;
				return nullptr;
			}
			t.file = new RawTrackFile(track_file,OFFSET,t.StartFAD,SSIZE);
		}
		if (!disc->tracks.empty())
			disc->tracks.back().EndFAD = t.StartFAD - 1;
		disc->tracks.push_back(t);
	}

	disc->FillGDSession();

	return disc;
}


Disc* gdi_parse(const char* file)
{
	size_t len=strlen(file);
	if (len>4)
	{
		if (stricmp( &file[len-4],".gdi")==0)
		{
			return load_gdi(file);
		}
	}
	return 0;
}

