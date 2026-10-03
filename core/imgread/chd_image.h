/* chd_image: a CD or GD-ROM image in a CHD file, read through
 * libretro-common's rchd.
 *
 * Opens the image and any parent images it differences against (found by
 * SHA-1 in the image's own directory), reports the track metadata, and
 * decodes hunks into the caller's buffer. A hunk holds whole frames of
 * 2352 bytes of sector data followed by 96 of subchannel, in the CHD's
 * logical frame order. One handle decodes on one thread at a time; a
 * second thread opens a handle of its own.
 */
#ifndef FLYCAST_IMGREAD_CHD_IMAGE_H
#define FLYCAST_IMGREAD_CHD_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHD_IMAGE_FRAME_BYTES (2352 + 96)

typedef struct chd_image chd_image_t;

/* One entry of the track metadata: 'CHT2', else 'CHTR' (which leaves the
 * gap fields zero and the gap strings empty), else the GD-ROM 'CHGT' or
 * 'CHGD', which add @pad. */
typedef struct chd_image_track
{
   int32_t track;
   int32_t frames;
   int32_t pad;
   int32_t pregap;
   int32_t postgap;
   char    type[64];
   char    subtype[32];
   char    pgtype[32];
   char    pgsub[32];
} chd_image_track_t;

/**
 * chd_image_open:
 * @path    : the .chd file
 * @err     : receives a message on failure; may be NULL
 * @err_len : capacity of @err
 *
 * Returns: the image, or NULL with @err filled in.
 */
chd_image_t *chd_image_open(const char *path, char *err, size_t err_len);

void chd_image_close(chd_image_t *img);

/**
 * chd_image_track:
 * @img   : image
 * @index : zero-based position in the metadata, not the track number
 * @out   : receives the entry
 *
 * Returns: true if the image has an entry at @index.
 */
bool chd_image_track(const chd_image_t *img, uint32_t index,
      chd_image_track_t *out);

/* Geometry of an open image: bytes per hunk (a multiple of
 * CHD_IMAGE_FRAME_BYTES for a disc image), how many hunks, and the CHD
 * format version. */
uint32_t chd_image_hunk_bytes(const chd_image_t *img);
uint32_t chd_image_hunk_count(const chd_image_t *img);
uint32_t chd_image_version(const chd_image_t *img);

/**
 * chd_image_read_hunk:
 * @img  : image
 * @hunk : hunk index, below chd_image_hunk_count()
 * @dst  : at least chd_image_hunk_bytes() bytes
 *
 * Returns: true if the hunk decoded into @dst.
 */
bool chd_image_read_hunk(chd_image_t *img, uint32_t hunk, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif
