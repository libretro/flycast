/*
	Copyright 2019 flyinghead

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

#include <sstream>
#include "types.h"
#include "common.h"
#include <file/file_path.h>

static u32 getSectorSize(const std::string& type) {
		if (type == "AUDIO")
			return 2352;	// PCM Audio
		else if (type == "CDG")
			return 2352;	// Karaoke cd+g
		else if (type == "MODE1/2048")
			return 2048;	// CDROM Mode1 Data (cooked)
		else if (type == "MODE1/2352")
			return 2352;	// CDROM Mode1 Data (raw)
		else if (type == "MODE2/2336")
			return 2336;	// CDROM XA Mode2 Data
		else if (type == "MODE2/2352")
			return 2352;	// CDROM XA Mode2 Data
		else if (type == "CDI/2336")
			return 2336;	// CDI Mode2 Data
		else if (type == "CDI/2352")
			return 2352;	// CDI Mode2 Data
		else
			return 0;
}

Disc* cue_parse(const char* file)
{
	// Only try to open .cue files
	size_t len = strlen(file);
	if (len > 4 && stricmp( &file[len - 4], ".cue"))
		return nullptr;

	core_file* fsource = core_fopen(file);

	if (fsource == nullptr)
		return nullptr;

	size_t cue_len = core_fsize(fsource);

	char cue_data[64 * 1024] = { 0 };

	if (cue_len >= sizeof(cue_data))
	{
		WARN_LOG(GDROM, "CUE parse error: CUE file too big");
		core_fclose(fsource);
		return nullptr;
	}

	core_fread(fsource, cue_data, cue_len);
	core_fclose(fsource);

   std::istringstream cuesheet(cue_data);

	char path[512];
	if (len >= sizeof(path))
	{
		WARN_LOG(GDROM, "CUE: path too long");
		return nullptr;
	}
	strcpy(path, file);
	const char* delim = path_get_archive_delim(file);
	ssize_t dir_end = (ssize_t)len;
	while (dir_end >= 0)
	{
		if (path[dir_end]=='\\' || path[dir_end]=='/' || (delim && &file[dir_end] == delim))
			break;
		dir_end--;
	}
	dir_end++;
	char* pathptr = &path[dir_end];

	Disc* disc = new Disc();
	u32 current_fad = 150;
	u32 track_number = -1;
   std::string track_type;
	u32 session_number = 0;
	bool high_density = false;
	/* The FILE being read: where on the disc it starts, the file itself
	 * until its first track takes it, and the sector size of its tracks
	 * (0 until its first TRACK). A file can hold several tracks. */
	bool have_file = false;
	core_file* file_handle = nullptr;
	size_t file_size = 0;
	u32 file_start_fad = 0;
	u32 sector_size = 0;
	/* The first track of the second session: the gap that session starts
	 * after takes in that track's pregap already. */
	bool first_of_session = false;

	while (!cuesheet.eof())
	{
		/* A number that was not one - a session, track or index given in
		 * letters, or too large - stops the stream from reading at all,
		 * short of the end of the sheet, and this loop then went round for
		 * ever. The sheet is read on from where that was. */
		if (cuesheet.fail())
			cuesheet.clear();
      std::string token;
		cuesheet >> token;

		if (token == "REM")
		{
			cuesheet >> token;
			if (token == "SESSION")
			{
				// Multi-session
				u32 cur_session;
				cuesheet >> cur_session;
				if (cur_session == 0)
					WARN_LOG(GDROM, "CUE parse error: invalid session number %s", token.c_str());
				else if (cur_session != session_number)
				{
					session_number = cur_session;
					if (session_number == 2)
					{
						// session 1 lead-out: 01:30:00, session 2 lead-in: 01:00:00, pregap: 00:02:00
						current_fad += 11400;
						first_of_session = true;
					}

					Session ses;
					ses.FirstTrack = disc->tracks.size() + 1;
					ses.StartFAD = current_fad;
					disc->sessions.push_back(ses);
					DEBUG_LOG(GDROM, "session[%zd]: 1st track: %d FAD:%d", disc->sessions.size(), ses.FirstTrack, ses.StartFAD);
				}
			}
			else
			{
				// GD-Rom
				if (token == "HIGH-DENSITY")
				{
					current_fad = 45000 + 150;
					high_density = true;
				}
				else if (token != "SINGLE-DENSITY")
					WARN_LOG(GDROM, "CUE parse error: unrecognized REM token %s. Expected SINGLE-DENSITY, HIGH-DENSITY or SESSION", token.c_str());
				cuesheet >> token;
				if (token != "AREA")
					WARN_LOG(GDROM, "CUE parse error: unrecognized REM token %s. Expected AREA", token.c_str());
			}
			// (a new area or session starts somewhere else on the disc: its tracks are in a FILE of their own)
			if (file_handle != nullptr)
				core_fclose(file_handle);
			file_handle = nullptr;
			have_file = false;
		}
		else if (token == "FILE")
		{
			std::string track_filename;
			char last = 0;

			do {
				cuesheet >> last;
			} while (cuesheet && isspace((unsigned char)last));

			if (last == '"')
			{
				cuesheet >> std::noskipws;
				for (;;) {
					// (to the closing quote, or the end of a file that has none)
					if (!(cuesheet >> last) || last == '"')
						break;
					track_filename += last;
				}
				cuesheet >> std::skipws;
			}
			else
			{
				cuesheet >> track_filename;
				track_filename = last + track_filename;
			}
			cuesheet >> token;	// BINARY
			if (token != "BINARY")
				WARN_LOG(GDROM, "CUE parse error: unrecognized FILE token %s. Expected BINARY", token.c_str());

			if (file_handle != nullptr)
				core_fclose(file_handle);
			file_handle = nullptr;
			have_file = false;
			if (track_filename.size() >= sizeof(path) - dir_end)
			{
				WARN_LOG(GDROM, "CUE file: file name too long: %s", track_filename.c_str());
				delete disc;
				return nullptr;
			}
			strcpy(pathptr, track_filename.c_str());
			file_handle = core_fopen(path);
			if (file_handle == nullptr)
			{
				WARN_LOG(GDROM, "CUE file: cannot open %s", path);
				delete disc;
				return nullptr;
			}
			have_file = true;
			file_size = core_fsize(file_handle);
			file_start_fad = current_fad;
			sector_size = 0;
			track_type.clear();
		}
		else if (token == "TRACK")
		{
			cuesheet >> track_number;
			cuesheet >> track_type;
			if (!have_file)
			{
				WARN_LOG(GDROM, "CUE file: track %d is not in a FILE", track_number);
				if (file_handle != nullptr)
					core_fclose(file_handle);
				delete disc;
				return nullptr;
			}
			const bool first_of_file = sector_size == 0;
			sector_size = getSectorSize(track_type);
			if (sector_size == 0)
			{
				WARN_LOG(GDROM, "CUE file: track %d has unknown sector type: %s", track_number, track_type.c_str());
				if (file_handle != nullptr)
					core_fclose(file_handle);
				delete disc;
				return nullptr;
			}
			if (file_size % sector_size != 0)
				WARN_LOG(GDROM, "Warning: Size of track %s is not multiple of sector size %d", pathptr, sector_size);
			/* The file's sectors are on the disc one after the other from
			 * where it starts. (Tracks of different sizes in one file are
			 * not laid out right: the first one's size is taken for all.) */
			if (first_of_file)
				current_fad = file_start_fad + (u32)(file_size / sector_size);
		}
		else if (token == "INDEX")
		{
			u32 index_num;
			cuesheet >> index_num;
			if (index_num == 1 && sector_size != 0)
			{
				/* The track starts at INDEX 01, which is minutes, seconds
				 * and frames into its FILE: what comes before it in the
				 * file is the track's pregap (or the end of the track
				 * before, in the same file). (It was taken as the start
				 * of the file, and two seconds were added to the start of
				 * a track of music whatever the sheet said: a track with
				 * no pregap lost its first two seconds - the Sonic Team
				 * jingle of ChuChu Rocket - and a data track with one
				 * started early.) */
				cuesheet >> token;
				u32 min = 0, sec = 0, frame = 0;
				u32 index_fad = 0;
				if (token.size() == 8 && sscanf(token.c_str(), "%2u:%2u:%2u", &min, &sec, &frame) == 3
						&& sec < 60 && frame < 75)
					index_fad = frame + 75 * (sec + 60 * min);
				else
					WARN_LOG(GDROM, "CUE file: track %d: INDEX 01 %s is not a time", track_number, token.c_str());
				// (no further into the file than it goes)
				if (index_fad > current_fad - file_start_fad)
				{
					WARN_LOG(GDROM, "CUE file: track %d: INDEX 01 %s is past the end of its file", track_number, token.c_str());
					index_fad = current_fad - file_start_fad;
				}
				if (first_of_session && index_fad != 0)
				{
					/* The pregap of the first track of the second session
					 * is in the gap counted before it: what of it is in the
					 * file is not on the disc a second time. */
					const u32 pregap = std::min(index_fad, (u32)150);
					file_start_fad -= pregap;
					current_fad -= pregap;
				}
				first_of_session = false;

				Track t;
				t.ADDR = 0;
				t.StartFAD = file_start_fad + index_fad;
				t.EndFAD = current_fad - 1;
				t.CTRL = (track_type == "AUDIO" || track_type == "CDG") ? 0 : 4;

				// (a disc has 99 tracks at most, and so has its table of contents)
				if (disc->tracks.size() >= 99)
				{
					WARN_LOG(GDROM, "CUE file: more than 99 tracks");
					if (file_handle != nullptr)
						core_fclose(file_handle);
					delete disc;
					return nullptr;
				}
				// The file's first track takes it as it was opened; another track in it opens it again.
				core_file* track_file = file_handle;
				file_handle = nullptr;
				if (track_file == nullptr)
					track_file = core_fopen(path);
				if (track_file == nullptr)
				{
					WARN_LOG(GDROM, "CUE file: cannot open track %d: %s", track_number, path);
					delete disc;
					return nullptr;
				}
				DEBUG_LOG(GDROM, "file[%zd] \"%s\": session %d type %s FAD:%d -> %d", disc->tracks.size() + 1, pathptr, session_number, track_type.c_str(), t.StartFAD, t.EndFAD);

				t.file = new RawTrackFile(track_file, index_fad * sector_size, t.StartFAD, sector_size);
				// (the track before ends where this one starts, when they share a file)
				if (!disc->tracks.empty() && disc->tracks.back().EndFAD >= t.StartFAD)
					disc->tracks.back().EndFAD = t.StartFAD - 1;
				disc->tracks.push_back(t);

				track_number = -1;
			}
		}
	}
	if (file_handle != nullptr)
		core_fclose(file_handle);
	if (disc->tracks.empty())
	{
		WARN_LOG(GDROM, "CUE parse error: failed to parse or invalid file with 0 tracks");
		delete disc;
		return nullptr;
	}

	if (session_number == 0 && high_density)
		disc->FillGDSession();
	else
	{
		if (session_number == 0)
		{
			/* No GD-ROM area and no sessions: a CD of one session, an
			 * audio CD or one with data. (It was made a GD-ROM, of which
			 * it had nothing where a GD-ROM has it.) */
			Session ses;
			ses.FirstTrack = 1;
			ses.StartFAD = disc->tracks[0].StartFAD;
			disc->sessions.push_back(ses);
			disc->type = CdRom;
		}
		else
			disc->type = CdRom_XA;
		disc->LeadOut.ADDR = 1;	// subcode-q channel
		disc->LeadOut.CTRL = 4;	// data
		disc->EndFAD = disc->LeadOut.StartFAD = current_fad;
	}

	return disc;
}
