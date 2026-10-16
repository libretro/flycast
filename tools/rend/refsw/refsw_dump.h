/* A frame of the core's, as the reference software renderer takes it
 * (tools/rend/refsw): what the TA parse left in the render context, the
 * registers the frame is drawn with, and video memory, which holds the
 * textures, the palette-free texture data and the background polygon.
 *
 * The file is the header, then the blocks in the order of the header's
 * fields, each a plain array of the core's own structures (sizeof as the
 * header says, so a reader built from other headers can refuse it):
 *
 *   regs         pvr_RendRegSize bytes: registers, fog table, palette
 *   vram         vram_size bytes
 *   verts        n_verts Vertex
 *   idx          n_idx words: the index list the polygons' first/count
 *                address after the parse (make_index(): a polygon is a
 *                triangle strip of indices, strips of one state joined with
 *                repeated indices)
 *   op, pt, tr   PolyParam of the three polygon lists
 *   modtrig      n_modtrig ModTriangle
 *   mvo, mvo_tr  ModifierVolumeParam of the two modifier volume lists
 *   passes       n_passes RenderPass
 *   region       n_region_tiles words of the region array's table (may be 0)
 *
 * Written by tools/rend/tile_dump.cpp (REFSW_DUMP_DIR), read by
 * tools/rend/refsw/refsw_frame.cpp. */
#ifndef REFSW_DUMP_H
#define REFSW_DUMP_H

#define REFSW_DUMP_MAGIC 0x57534652u	/* "RFSW" little-endian */
#define REFSW_DUMP_VERSION 1u

struct refsw_dump_header
{
	unsigned magic;
	unsigned version;
	unsigned width, height;		/* the screen the frame is drawn to */
	unsigned regs_size;
	unsigned vram_size;
	unsigned sizeof_vertex;
	unsigned sizeof_polyparam;
	unsigned sizeof_modtriangle;
	unsigned sizeof_mvparam;
	unsigned sizeof_renderpass;
	unsigned n_verts;
	unsigned n_idx;
	unsigned n_op, n_pt, n_tr;
	unsigned n_modtrig;
	unsigned n_mvo, n_mvo_tr;
	unsigned n_passes;
	unsigned n_region_tiles;
	float fZ_min, fZ_max;
	unsigned fb_x_clip, fb_y_clip;
	unsigned fog_clamp_min, fog_clamp_max;
	unsigned frame;				/* the frame's number in the run */
	unsigned reserved[4];
};

#endif
