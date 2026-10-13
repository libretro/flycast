#include "vmu_beep.h"

/* A VMU beeps through its own piezo speaker, not through the console's
 * sound: the beep is added to the AICA's output just before it goes to the
 * frontend, a frame at a time, so that the sound of a frame without a beep
 * (nearly all of them) costs one test.
 *
 * A game sets the alarm through the maple clock function with a period and
 * an on time, in counts of a 1 MHz timer. The piezo does not follow the duty
 * cycle much: what is heard is a tone of 1 MHz / period, made here from a
 * few recorded cycles of a real beep, played faster or slower. It fades
 * in and out over 128 samples, so starting and stopping it does not click. */

bool vmu_beep_enabled;

/* A beep at 2698 Hz, three cycles at 44100 Hz, that loops without a step. */
static const s16 beep_wave[] = {
	  503, -3519, -7540, -8214, -8209, -8214, -8209, -5199, -1172,  2843,
	 6873,  8207,  8214,  8209,  8212,  5866,  1840, -2175, -6203, -8210,
	-8215, -8209, -8214, -6533, -2516,  1507,  5526,  8212,  8210,  8212,
	 8210,  7206,  3187,  -841, -4856, -8215, -8209, -8214, -8208, -7882,
	-3850,   162,  4187,  8213,  8208,  8213,  8209,  8211,  4525
};
#define BEEP_WAVE_LEN   (sizeof(beep_wave) / sizeof(beep_wave[0]))
#define BEEP_WAVE_HZ    2698
/* The position in the wave is in 1/1024ths of a sample. */
#define BEEP_POS_END    ((u32)BEEP_WAVE_LEN << 10)
/* The step of the shortest period, 1. */
#define BEEP_STEP_MAX   (1000000u * 1024 / BEEP_WAVE_HZ)
/* Attenuation in 1/16ths of a halving: 0 is full volume, 256 silence. */
#define BEEP_ATT_OFF    256

/* 32768 * 2^(-k/16) */
static const u16 att_frac[16] = {
	32768, 31379, 30048, 28774, 27554, 26386, 25268, 24196,
	23170, 22188, 21247, 20347, 19484, 18658, 17867, 17109
};

static u8 beep_active;
static u32 beep_att = BEEP_ATT_OFF;
static u32 beep_pos;
static u32 beep_step;

void vmu_beep_set(u32 period, u32 on)
{
	period &= 0xFF;
	on &= 0xFF;
	if (on == 0 || period == 0 || on >= period)
	{
		/* (the tone it had fades out) */
		beep_active = 0;
		return;
	}
	beep_active = 1;
	beep_step = (1000000u / period) * 1024 / BEEP_WAVE_HZ;
}

void vmu_beep_reset(void)
{
	beep_active = 0;
	beep_att = BEEP_ATT_OFF;
	beep_pos = 0;
	beep_step = 0;
}

static inline s16 clamp16(s32 v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

void vmu_beep_mix(SoundFrame *frames, u32 count)
{
	if (!vmu_beep_enabled || (!beep_active && beep_att >= BEEP_ATT_OFF))
		return;

	for (u32 i = 0; i < count; i++)
	{
		if (!beep_active && beep_att >= BEEP_ATT_OFF)
			break;

		beep_pos = (beep_pos + beep_step) % BEEP_POS_END;
		const u32 idx = beep_pos >> 10;
		const s32 fp = beep_pos & 1023;
		const u32 next = idx + 1 < BEEP_WAVE_LEN ? idx + 1 : 0;
		s32 s = ((beep_wave[idx] * (1024 - fp)) >> 10) + ((beep_wave[next] * fp) >> 10);
		s = (s * 2 * (s32)(att_frac[beep_att & 15] >> (beep_att >> 4))) >> 15;

		if (!beep_active)
			beep_att += 2;
		else if (beep_att > 0)
			beep_att -= 2;

		frames[i].l = clamp16(frames[i].l + s);
		frames[i].r = clamp16(frames[i].r + s);
	}
}

/* 13 bytes */
void vmu_beep_serialize_v24(void **data, unsigned int *total_size)
{
	LIBRETRO_S(beep_active);
	LIBRETRO_S(beep_att);
	LIBRETRO_S(beep_pos);
	LIBRETRO_S(beep_step);
}

bool vmu_beep_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 active = 0;
	u32 att = BEEP_ATT_OFF, pos = 0, step = 0;

	LIBRETRO_US(active);
	LIBRETRO_US(att);
	LIBRETRO_US(pos);
	LIBRETRO_US(step);
	beep_active = active ? 1 : 0;
	/* (the fade moves by two, from 0 or from 256) */
	beep_att = att <= BEEP_ATT_OFF ? (att & ~1u) : BEEP_ATT_OFF;
	beep_pos = pos < BEEP_POS_END ? pos : 0;
	beep_step = step <= BEEP_STEP_MAX ? step : 0;
	return true;
}

/* An older state has no beep: it is silent, as it always was. */
void vmu_beep_state_before_v24(void)
{
	vmu_beep_reset();
}
