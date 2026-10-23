#include <algorithm>
#include "spg.h"
#include "Renderer_if.h"
#include "pvr_regs.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/maple/maple_if.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/oslib.h"

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
static u8 lightgun_flip;
/* A transfer has asked a gun where it points since the last vertical blank
 * out: its end is reported then, with the interrupt, when the game started
 * it itself. (A transfer the vertical blank starts, a hardware trigger,
 * gets no interrupt: Virtua Cop 2 stops for a moment at every shot if it
 * does; RevolveR, which starts its own, waits for one.) */
static bool gun_dma_owed;

static u32 spg_next_line();

/* Where a retro_run ends: where the frontend reads its pad and shows what
 * was drawn, once a frame.
 *
 * At line 0 it was, always. Most games read their pads (a maple bus
 * transfer) and start the render of the frame they have made in their
 * vertical blank in interrupt: Jet Grind Radio and Virtua Fighter 3tb at
 * line 260, rendering and then reading; Sega Rally 2 (under Windows CE)
 * at line 0, rendering a line after the interrupt; Virtua Fighter 4 and
 * Dead or Alive 2 render before theirs at line 520 of 530 and read after
 * line 0. Where the render came just after the end of a run, its picture
 * waited for the end of the next one (Sega Rally 2); where the pads were
 * read in the middle of a run after the render, what the game made of
 * them waited a run more (Virtua Fighter 3tb).
 *
 * So a run may end in the lines after the vertical blank in interrupt,
 * the window (FRAME_WINDOW_LINES of them, and never on or past the
 * vertical blank out line, where the hardware's own maple transfer reads
 * the pads): at a render to the screen the game starts in it - its
 * picture is shown at once - or, before one, at a maple transfer the game
 * starts in it, which is then made at the start of the next run, with
 * that run's pad (the machine has not moved on between the two: no time
 * goes by between runs). A run that ended in a window ends in the next
 * window, at whichever of the two comes, or at its end in a frame with
 * neither (a game at 30 frames a second, a loading screen) - unless it
 * rendered to the screen elsewhere in that frame: then the game draws at
 * another time and it goes on to line 0, if line 0 is still to come (not
 * where it came while the window was open: there it ends at the window's
 * end, or it would take in the next window too). A run that started at line 0
 * ends at line 0 (at the window's end, if line 0 comes while it is open),
 * or in the window if a render or a transfer comes in it. A transfer
 * started once a run's end has come - the processor runs on to the end of
 * its time slice - waits for the next run too.
 * A game that does nothing in the window (renders in the middle of the
 * frame, and has the hardware read its pads) has its runs end at line 0,
 * where nothing it does is put off.
 *
 * Which of the two a run is, is seen from where the beam is when it
 * starts: in a window, or not. Nothing else about it is kept from one run
 * to the next, and a transfer waiting for the next run is one that has
 * been started with nothing scheduled to end it (maple_if.cpp); whether
 * the game has started a render since the last vertical blank, which a
 * run ending at line 0 never had, is in the state too (Renderer_if.cpp's
 * pvr_serialize_v22()), and no run ends with the window open (the
 * backstop closes it: spg_frame_cut()). So a state - saved between two
 * runs - says it all, and the same frames come out of it however it was
 * reached. Each run has one vertical blank in interrupt in it: a game
 * that renders once a frame has one picture in each. */
#define FRAME_WINDOW_LINES 32
static bool frame_window;	// the window is open: a render to the screen or a maple transfer ends the run
static u32 frame_window_end;	// the line it closes on
static bool frame_window_line0;	// line 0 came while it was open
static bool frame_at_window;	// this run started in a window: it ends in the next one
static bool frame_render_late;	// a render to the screen in this run, outside the window
static bool frame_ended;	// this run's end has come

// How many lines after the vertical blank in interrupt the window has (0: none)
static u32 frame_window_lines()
{
	const u32 vin = SPG_VBLANK_INT.vblank_in_interrupt_line_number;
	const u32 vout = SPG_VBLANK_INT.vblank_out_interrupt_line_number;
	if (vin >= pvr_numscanlines)
		return 0;
	u32 lines = FRAME_WINDOW_LINES;
	if (vout < pvr_numscanlines && vout != vin)
	{
		const u32 to_out = (vout + pvr_numscanlines - vin) % pvr_numscanlines;
		lines = std::min(lines, to_out - 1);
	}
	return lines;
}

static u64 spg_beam();

/* The window closes before its end: the pending event was aimed with it
 * open, at its end perhaps, and is aimed again without it - as for a write
 * to a timing register - or the event, coming at its end, would be taken
 * for the line after it that there is something to do on, and the lines in
 * between lost. */
static void frame_window_close()
{
	if (!frame_window)
		return;
	if (sh4_sched_remaining(vblank_schid) == (u32)-1)
	{
		frame_window = false;
		return;
	}
	const u64 beam = spg_beam();
	frame_window = false;
	sh4_sched_request(vblank_schid, (int)(line_start(spg_next_line()) - beam));
}

static void frame_end()
{
	frame_ended = true;
	os_DoEvents();
}

/* dc_run()'s backstop stops a run that has gone on too long, wherever it
 * is. A window still open is closed, as at every other end: the next run
 * begins with none open, whether it follows this one or a state saved
 * here (which has the line event as closing it aims it). */
void spg_frame_cut()
{
	frame_window_close();
	frame_ended = true;
}

/* dc_run(): a run starts. Where the last one ended says which kind this is. */
void spg_frame_begin()
{
	const u32 lines = frame_window_lines();
	frame_at_window = false;
	if (lines != 0 && pvr_cur_scanline < pvr_numscanlines)
	{
		const u32 since_in = (pvr_cur_scanline + pvr_numscanlines - SPG_VBLANK_INT.vblank_in_interrupt_line_number)
				% pvr_numscanlines;
		frame_at_window = since_in <= lines;
	}
	frame_window_close();
	frame_window_line0 = false;
	frame_render_late = false;
	frame_ended = false;
}

/* A render to the screen has been started (rend_start_render(): not one to a
 * texture, nor the framebuffer the game wrote itself, shown) */
void spg_screen_render()
{
	if (frame_ended)
		return;
	if (frame_window)
	{
		frame_window_close();
		frame_end();
	}
	else
		frame_render_late = true;
}

/* The game starts a maple transfer: true if it is to be made at the start
 * of the next run instead - the run ends here, in the window, or has
 * already ended (the processor runs on to the end of its time slice) */
bool spg_maple_defer()
{
	if (frame_ended)
		return true;
	if (!frame_window)
		return false;
	frame_window_close();
	frame_end();
	return true;
}

// a transfer that asked a light gun where it points is under way
bool spg_gun_pending()
{
	return maple_int_pending;
}

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
	if (frame_window && next <= frame_window_end)
		line = std::min(line, frame_window_end);
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

		/* The window closes with nothing in it. A run that started in the
		 * last one ends here, unless it rendered to the screen elsewhere:
		 * then it goes on to line 0. One that started at line 0 ends here
		 * if line 0 came while the window was open, and at line 0
		 * otherwise. Either ends here if line 0 came while the window was
		 * open (an interrupt at line 0, as Sega Rally 2 has, or a window
		 * that runs over it): the line 0 to go on to would be the next
		 * frame's, past the next window, and the run two frames long - a
		 * frame the game rendered in it never shown. Each run has one
		 * vertical blank in interrupt in it, the window it ends in or
		 * after. */
		if (frame_window && pvr_cur_scanline == frame_window_end)
		{
			frame_window = false;
			if (!frame_ended)
			{
				if (frame_window_line0 || (frame_at_window && !frame_render_late))
					frame_end();
				else
					frame_at_window = false;
			}
		}

      /* Vblank in */
		if (SPG_VBLANK_INT.vblank_in_interrupt_line_number == pvr_cur_scanline)
      {
			const u32 lines = frame_window_lines();
			if (lines != 0 && !frame_ended)
			{
				frame_window = true;
				frame_window_end = (pvr_cur_scanline + lines) % pvr_numscanlines;
				frame_window_line0 = false;
			}
         if (maple_int_pending)
         {
            maple_int_pending = false;
            SB_MDST = 0;
         }

         asic_RaiseInterrupt(holly_SCANINT1);
      }

      /* Vblank Out */
		if (SPG_VBLANK_INT.vblank_out_interrupt_line_number == pvr_cur_scanline)
		{
			if (SB_MDEN & 1)
			{
				if (gun_dma_owed && !(SB_MDTSEL & 1))
					maple_dma_done();
				gun_dma_owed = false;
			}
			asic_RaiseInterrupt(holly_SCANINT2);
		}

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
			// the end of a run that did not start in a window, unless one is open
			if (frame_window)
				frame_window_line0 = true;
			else if (!frame_ended && !frame_at_window)
				frame_end();
		}
		if (lightgun_line != 0xffff && lightgun_line == pvr_cur_scanline)
		{
         maple_int_pending = false;
			SPG_TRIGGER_POS = ((lightgun_line & 0x3FF) << 16) | (lightgun_hpos & 0x3FF);
			// the bus is free again; the answers and the interrupt come at vblank out
			SB_MDST = 0;
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
   /* The pending event was aimed without this line, or at the one asked
    * for before it. As for a write to a timing register: where the beam is
    * is worked out with things as they were, and the event is aimed again
    * from there once the line is set. (Not when this is reached from the
    * event itself, by the transfer that starts at vblank: that aims the
    * next one when it returns.) */
   const bool pending = sh4_sched_remaining(vblank_schid) != (u32)-1;
   const u64 beam     = pending ? spg_beam() : 0;

   maple_int_pending = true;
   // (a transfer the vertical blank started is never reported)
   gun_dma_owed      = gun_dma_owed || !(SB_MDTSEL & 1);
	if (y < 0 || y >= 480 || x < 0 || x >= 640)
   {
		// Off screen
		lightgun_line = 0xffff;
   }
	else
	{
		lightgun_line = y / (SPG_CONTROL.interlace ? 2 : 1) + SPG_VBLANK_INT.vblank_out_interrupt_line_number;
      // For some reason returning the same position twice makes it register off screen
		lightgun_hpos = (x + 286) ^ lightgun_flip;
		lightgun_flip ^= 1;
	}
   if (pending)
      sh4_sched_request(vblank_schid, (int)(line_start(spg_next_line()) - beam));
}

// The game cancelled the gun's occupation of the bus: its transfer ends in its own time
void spg_gun_occupy_cancel()
{
   gun_dma_owed = false;
}

/* The light gun in a save state, from version 21 on (pvr_serialize_v21()):
 * the line and the place on it where the gun will be found to point, 0xffff
 * for no line; whether the transfer that asked is still to be ended; and
 * which way the place is nudged next time. Five bytes.
 *
 * The line is one of those the pending event is aimed at, spg_next_line(),
 * and the event's time is in the state: without the line, the event came
 * at the time of one line and was taken for another. */
void spg_gun_serialize(void **data, unsigned int *total_size)
{
	u16 line  = (u16)lightgun_line;
	u16 hpos  = (u16)lightgun_hpos;
	u8  flags = (maple_int_pending ? 1 : 0) | (lightgun_flip ? 2 : 0);

	LIBRETRO_S(line);
	LIBRETRO_S(hpos);
	LIBRETRO_S(flags);
}

void spg_gun_unserialize(void **data, unsigned int *total_size)
{
	u16 line;
	u16 hpos;
	u8  flags;

	LIBRETRO_US(line);
	LIBRETRO_US(hpos);
	LIBRETRO_US(flags);

	/* A line is at most 479 down from the line the picture starts on, which
	 * is a 10-bit register; the place goes into ten bits of a register. */
	lightgun_line     = line > 479 + 1023 ? 0xffff : line;
	lightgun_hpos     = hpos & 0x3FF;
	maple_int_pending = (flags & 1) != 0;
	lightgun_flip     = (flags >> 1) & 1;
}

/* A state from before version 21 has none of it. No line is pending. A
 * transfer that the state says is under way with nothing scheduled to end
 * it, though, is one a gun was asked its position in: the next vertical
 * blank ends it, as it would have. (Called once the registers and the
 * scheduler's events have been read.) */
void spg_gun_before_v21(void)
{
	extern int maple_sched;

	lightgun_line     = 0xffff;
	lightgun_hpos     = 0;
	lightgun_flip     = 0;
	maple_int_pending = (SB_MDST & 1) != 0 && !sh4_sched_is_scheduled(maple_sched);
}

/* The light gun in a save state from version 28 (pvr_serialize_v28()): one
 * byte, bit 0 being gun_dma_owed. */
void spg_gun_serialize_v28(void **data, unsigned int *total_size)
{
	u8 flags = gun_dma_owed ? 1 : 0;

	LIBRETRO_S(flags);
}

void spg_gun_unserialize_v28(void **data, unsigned int *total_size)
{
	u8 flags = 0;

	LIBRETRO_US(flags);
	gun_dma_owed = (flags & 1) != 0;
}

/* An older state: a transfer that asked a gun and has not ended yet ended
 * at the gun's line then, with the interrupt; now it is reported at the
 * next vertical blank out. One that has ended is reported already. (Called
 * once the part of version 21, or spg_gun_before_v21(), has been read.) */
void spg_gun_before_v28(void)
{
	gun_dma_owed = maple_int_pending;
}

int rend_end_sch(int tag, int cycl, int jitt)
{
	asic_RaiseInterruptBothCLX(holly_RENDER_DONE);
	asic_RaiseInterruptBothCLX(holly_RENDER_DONE_isp);
	asic_RaiseInterruptBothCLX(holly_RENDER_DONE_vd);

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
   // no light gun position is latched (before the first event is aimed, which looks at it)
   maple_int_pending = false;
   gun_dma_owed      = false;
   lightgun_line     = 0xffff;
   lightgun_hpos     = 0;
   lightgun_flip     = 0;
   // a render the last run started: its end is not an interrupt for this one
   if (sh4_sched_is_scheduled(render_end_schid))
      sh4_sched_request(render_end_schid, -1);
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
	if (cntx && settings.System == DC_PLATFORM_NAOMI2)
	{
		// a NAOMI 2 frame is always given the longest time, however little is in it
		sh4_sched_request(render_end_schid, 1500000);
	}
	else if (cntx)
	{
		const int size = (int)(cntx->tad.thd_data - cntx->tad.thd_root);
		// (megabytes of it on a NAOMI 2: no overflow)
		sh4_sched_request(render_end_schid, size >= 10500 ? 1500000 : 450000 + size * 100);
	}
	else
		sh4_sched_request(render_end_schid, 4096);
}
