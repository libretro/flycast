#include <algorithm>
#include "spg.h"
#include "Renderer_if.h"
#include "pvr_regs.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/maple/maple_if.h"
#include "hw/sh4/sh4_sched.h"

u32 in_vblank;
u32 clc_pvr_scanline;
static u32 pvr_numscanlines = 512;
u32 pvr_cur_scanline = -1;
static u32 vblk_cnt;

#define PIXEL_CLOCK (54*1000*1000/2)

/* How long a scanline lasts, in SH4 cycles, as the fraction it is:
 * Line_Num / Line_Den. An NTSC or VGA line is 6355.56 cycles (858 pixels at
 * 27 MHz, on a 200 MHz clock), and counting it as 6355, which this used to
 * do, made every frame 290 cycles short: 59.9453 frames a second where the
 * hardware makes 59.9401.
 *
 * Lines are still whole cycles each, but not all the same: line n of a frame
 * begins at floor(n * Line_Num / Line_Den) cycles into it, so they come out
 * 6355 or 6356 as the fraction requires and a frame is as long as it should
 * be to within a cycle. Nothing is carried from one frame to the next, so
 * there is no state to this beyond the scanline the beam is on. */
static u64 Line_Num;
static u64 Line_Den = 1;

// The cycle, counted from the start of the frame, at which a scanline begins
static inline u64 line_start(u32 line)
{
	return (u64)line * Line_Num / Line_Den;
}
u32 sh4_sched_remaining(int id);

int render_end_schid;
int vblank_schid;

static u32 lightgun_line = 0xffff;
static u32 lightgun_hpos;
static bool maple_int_pending;

static u32 spg_next_line();

void CalculateSync(bool reschedule)
{
	const u32 pixel_clock = PIXEL_CLOCK / (FB_R_CTRL.vclk_div ? 1 : 2);

	//We need to calculate the pixel clock
	pvr_numscanlines = SPG_LOAD.vcount + 1;

	Line_Num = (u64)SH4_MAIN_CLOCK * (u64)(SPG_LOAD.hcount + 1);
	Line_Den = pixel_clock;

	float scale_x = 1.f, scale_y = 1.f;

	if (SPG_CONTROL.interlace)
	{
		//this is a temp hack
		Line_Den *= 2;

		//u32 interl_mode      = VO_CONTROL.field_mode;
		
		//if (interl_mode==2)//3 will be funny =P
		//  scale_y=0.5f;//single interlace
		//else
			scale_y = 1.f;
	}
	else
	{
		if (FB_R_CTRL.vclk_div)
			scale_y           = 1.0f;//non interlaced VGA mode has full resolution :)
		else
			scale_y           = 0.5f;//non interlaced modes have half resolution
	}

	rend_set_fb_scale(scale_x, scale_y);

	/* On a real timing change (register write / reset) restart the raster at
	 * scanline 0 and arm the line scheduler. Skipped on savestate load, where
	 * the beam position and vblank event are restored from the state. */
	if (reschedule)
	{
		pvr_cur_scanline = 0;
		sh4_sched_request(vblank_schid, (int)line_start(spg_next_line()));
	}
}

/*
 * Actual emulated vertical refresh rate: the SH4 clock over the cycles a frame
 * takes, which is what the scheduler below runs on. This is the rate at which
 * frames (and thus audio) are actually produced, so reporting it to the
 * frontend via retro_get_system_av_info keeps timing.fps in step with the core
 * instead of a bucketed 60/59.94/50 guess. It yields the hardware's figures to
 * within a cycle a frame: 59.9401 for VGA and 480i, 59.8261 for 240p NTSC,
 * 50 for PAL 480i, and the higher NAOMI rates for boards that program the SPG
 * accordingly. Do not round or snap it: the frontend wants the exact figure
 * the core produces frames at. Falls back to 60 before CalculateSync has run.
 */
double spg_get_refresh_rate(void)
{
	const u64 frame = line_start(pvr_numscanlines);

	if (frame == 0)
		return 60.0;
	return (double)SH4_MAIN_CLOCK / (double)frame;
}


/* The first scanline after the beam's at which there is something to do:
 * what the pending event is aimed at. pvr_numscanlines stands for line 0 of
 * the frame after this one.
 *
 * Every register this reads has spg_write_timing() for its writes, so
 * between one event and the next the answer only changes by the beam
 * moving on - and spg_beam() never moves it past a line with something to
 * do. */
static u32 spg_next_line()
{
	const u32 next = pvr_cur_scanline + 1;

	if (SPG_HBLANK_INT.hblank_int_mode == 2)
		return next;		// an interrupt on every line

	u32 line = pvr_numscanlines;
	if (next <= SPG_VBLANK_INT.vblank_in_interrupt_line_number)
		line = std::min(line, (u32)SPG_VBLANK_INT.vblank_in_interrupt_line_number);
	if (next <= SPG_VBLANK_INT.vblank_out_interrupt_line_number)
		line = std::min(line, (u32)SPG_VBLANK_INT.vblank_out_interrupt_line_number);
	if (next <= SPG_VBLANK.vstart)
		line = std::min(line, (u32)SPG_VBLANK.vstart);
	if (next <= SPG_VBLANK.vbend)
		line = std::min(line, (u32)SPG_VBLANK.vbend);
	if (lightgun_line != 0xffff && next <= lightgun_line)
		line = std::min(line, lightgun_line);
	// the line the horizontal blank interrupt is asked for on
	if (SPG_HBLANK_INT.hblank_int_mode == 0 && next <= SPG_HBLANK_INT.line_comp_val)
		line = std::min(line, (u32)SPG_HBLANK_INT.line_comp_val);
	return std::max(line, next);
}

// The beam goes down @lines scanlines, and what is due on each is done
static void spg_run_lines(u32 lines)
{
	for (; lines != 0; lines--)//60 ~hertz = 200 mhz / 60=3333333.333 cycles per screen refresh
	{
		//ok .. here , after much effort , we did one line
		//now , we must check for raster beam interrupts and vblank
		pvr_cur_scanline = (pvr_cur_scanline + 1) % pvr_numscanlines;
		//Check for scanline interrupts -- really need to test the scanline values
		
      /* Vblank in */
		if (SPG_VBLANK_INT.vblank_in_interrupt_line_number == pvr_cur_scanline)
      {
         if (maple_int_pending)
         {
            maple_int_pending = false;
            maple_dma_done();
         }

         asic_RaiseInterrupt(holly_SCANINT1);
      }

      /* Vblank Out */
		if (SPG_VBLANK_INT.vblank_out_interrupt_line_number == pvr_cur_scanline)
			asic_RaiseInterrupt(holly_SCANINT2);

		if (SPG_VBLANK.vstart == pvr_cur_scanline)
			in_vblank = 1;

		if (SPG_VBLANK.vbend == pvr_cur_scanline)
			in_vblank = 0;

		SPG_STATUS.vsync    = in_vblank;
		SPG_STATUS.scanline = pvr_cur_scanline;
		
		switch (SPG_HBLANK_INT.hblank_int_mode)
		{
		case 0x0:
			if (pvr_cur_scanline == SPG_HBLANK_INT.line_comp_val)
				asic_RaiseInterrupt(holly_HBLank);
			break;
		case 0x2:
			asic_RaiseInterrupt(holly_HBLank);
			break;
		default:
			// mode 1 is not emulated, and 3 is not a mode; neither is a reason to stop
			break;
		}

		//Vblank start -- really need to test the scanline values
		if (pvr_cur_scanline==0)
		{
			if (SPG_CONTROL.interlace)
				SPG_STATUS.fieldnum = ~SPG_STATUS.fieldnum;
			else
				SPG_STATUS.fieldnum=0;

			/* Vblank counter */
			vblk_cnt++;

			rend_vblank(); // notify for vblank
		}
		if (lightgun_line != 0xffff && lightgun_line == pvr_cur_scanline)
		{
         maple_int_pending = false;
			SPG_TRIGGER_POS = ((lightgun_line & 0x3FF) << 16) | (lightgun_hpos & 0x3FF);
			maple_dma_done();
			lightgun_line = 0xffff;
		}
	}

}

/* Where the beam is in the frame, in cycles, from how long the pending
 * event still has; and pvr_cur_scanline and SPG_STATUS are brought up to
 * the line that is. The lines in between have nothing due on them, or the
 * event would have been aimed at one of them. */
static u64 spg_beam()
{
	const u32 target = spg_next_line();
	const u64 target_at = line_start(target);
	const u64 line_at = line_start(pvr_cur_scanline);
	const u32 remaining = sh4_sched_remaining(vblank_schid);

	// nothing pending, or the event is due: the beam is where it was put last
	if (remaining == (u32)-1 || (u64)remaining >= target_at - line_at)
		return line_at;

	const u64 beam = target_at - remaining;
	u32 line = (u32)(beam * Line_Den / Line_Num);
	while (line_start(line + 1) <= beam)
		line++;
	while (line > pvr_cur_scanline && line_start(line) > beam)
		line--;
	if (line >= target)
		line = target - 1;
	if (line > pvr_cur_scanline)
		spg_run_lines(line - pvr_cur_scanline);
	return beam;
}

// SPG_STATUS is about to be read: its scanline is the one the beam is on
void spg_sync()
{
	spg_beam();
}

/* A write to a register that says on which lines things happen. The event
 * that is pending was aimed by the old value: it is aimed again, from where
 * the beam is now, so that a line asked for is not found out about only
 * when the beam gets to whatever was asked for before. */
void spg_write_timing(u32 addr, u32 data)
{
	if (sh4_sched_remaining(vblank_schid) == (u32)-1)
	{
		// the beam is not running yet
		PvrReg(addr, u32) = data;
		return;
	}
	const u64 beam = spg_beam();

	PvrReg(addr, u32) = data;
	sh4_sched_request(vblank_schid, (int)(line_start(spg_next_line()) - beam));
}

//called from sh4 context , should update pvr/ta state and everything else
int spg_line_sched(int tag, int cycl, int jit)
{
	/* The event was aimed at the start of a line, spg_next_line(), and the
	 * scheduler takes how late it called (jit) off the next request: so
	 * this is that line, however much of the way there a read of SPG_STATUS
	 * has already taken pvr_cur_scanline. Counting lines by the time since
	 * the last call would count those twice.
	 *
	 * Lines differ by a cycle from one to the next (see Line_Num), and the
	 * requests are differences of line_start(), so nothing is lost or
	 * carried from one frame to the next. */
	clc_pvr_scanline = 0;
	spg_run_lines(spg_next_line() - pvr_cur_scanline);

	// from the start of the line the beam is on to the start of the next one with something to do
	return (int)(line_start(spg_next_line()) - line_start(pvr_cur_scanline));
}

void read_lightgun_position(int x, int y)
{
   static u8 flip;
   maple_int_pending = true;
	if (y < 0 || y >= 480 || x < 0 || x >= 640)
   {
		// Off screen
		lightgun_line = 0xffff;
   }
	else
	{
		lightgun_line = y / (SPG_CONTROL.interlace ? 2 : 1) + SPG_VBLANK_INT.vblank_out_interrupt_line_number;
      // For some reason returning the same position twice makes it register off screen
		lightgun_hpos = (x + 286) ^ flip;
		flip ^= 1;
	}
}

int rend_end_sch(int tag, int cycl, int jitt)
{
	asic_RaiseInterrupt(holly_RENDER_DONE);
	asic_RaiseInterrupt(holly_RENDER_DONE_isp);
	asic_RaiseInterrupt(holly_RENDER_DONE_vd);

   rend_end_render();
	return 0;
}

bool spg_Init()
{
   render_end_schid = sh4_sched_register(0,&rend_end_sch);
   vblank_schid     = sh4_sched_register(0,&spg_line_sched);

   return true;
}

void spg_Term()
{
}

void spg_Reset(bool hard)
{
   CalculateSync();
}

void SetREP(TA_context* cntx)
{
	/* How long the chip takes to draw depends on how much there is to draw.
	 * This was 1500000 cycles, 7.5 ms, whatever the scene. Upstream goes by
	 * the size of what the game sent, and so does this: its figures, arrived
	 * at over games - the video of some Windows CE games (Resident Evil 2,
	 * The Next Tetris, Nightmare Creatures 2) wants the interrupt sooner
	 * than that for a scene with little in it, and Marvel vs. Capcom 2 wants
	 * no less than these. */
	if (cntx)
	{
		const int size = (int)(cntx->tad.thd_data - cntx->tad.thd_root);
		sh4_sched_request(render_end_schid, std::min(450000 + size * 100, 1500000));
	}
	else
		sh4_sched_request(render_end_schid, 4096);
}
