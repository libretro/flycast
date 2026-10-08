/* The NAOMI 2's geometry processor ("ELAN", VideoLogic).
 *
 * It sits between the SH4 and the two PowerVR chips. The game leaves
 * models, matrices and lights in the chip's own 32 MB of memory and sends
 * it a display list; the chip transforms, lights and clips the models and
 * writes what comes out to the PowerVRs' tile accelerators, as polygons in
 * screen coordinates - the same parameters a NAOMI or Dreamcast game sends
 * there itself.
 *
 * That is what this does, on the processor: what leaves it is tile
 * accelerator data, handed to elan_host_ta(), and nothing downstream knows
 * a geometry processor was involved.
 *
 * The display list format, the registers and the lighting equations are
 * flycast's (core/hw/pvr/elan.cpp, core/rend/gles/naomi2.cpp, flyinghead
 * 2022), which does the transformation and lighting in vertex shaders.
 *
 * This file and elan.c are C89 and include nothing of the emulator: the
 * machine is reached through the elan_host_*() functions, which
 * elan_host.cpp provides (and tools/elan/elan_test.c, for the test).
 */
#ifndef ELAN_H_
#define ELAN_H_

#include <stddef.h>
#include <stdint.h>

#include <retro_common_api.h>
#include <retro_inline.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

#define ELAN_RAM_SIZE   (32 * 1024 * 1024)
#define ELAN_RAM_MASK   (ELAN_RAM_SIZE - 1)
#define ELAN_MAX_LIGHTS 16
/* "nowhere": the address of a model parameter that has not been set */
#define ELAN_NONE       0xffffffffu

/* The chip's state besides its memory: what a save state carries.
 * Everything else is worked out again from this and the memory. */
typedef struct elan_state
{
   uint32_t reg10;
   uint32_t reg30;
   uint32_t reg74;
   uint32_t cmd[8];                    /* the command port's 32 bytes     */
   uint32_t gmp;                       /* addresses in the chip's memory  */
   uint32_t instance;
   uint32_t light_model;
   uint32_t lights[ELAN_MAX_LIGHTS];
   float    proj[4];                   /* fx, tx, fy, ty                  */
   uint32_t dma_busy;                  /* a texture transfer is under way */
} elan_state_t;

/* @ram: ELAN_RAM_SIZE bytes, the chip's memory. */
void elan_init(uint8_t *ram);
/* Power on: registers and model state, not the memory. */
void elan_reset(void);

uint32_t elan_reg_read(uint32_t addr);
void elan_reg_write(uint32_t addr, uint32_t data);
/* A write to the command port. The eighth word of a command runs it. */
void elan_cmd_write(uint32_t addr, uint32_t data);
/* The texture transfer elan_host_texture_dma() started is over. */
void elan_dma_done(void);

void elan_get_state(elan_state_t *state);
void elan_set_state(const elan_state_t *state);

/* Vertices put through the chip, and vertices it wrote out, since the
 * last call. For measuring. */
void elan_counters(unsigned *in, unsigned *out);

/* ---- the machine around the chip ---- */

/* @count 32-byte blocks for the tile accelerator. */
void elan_host_ta(const uint32_t *blocks, unsigned count);

/* Room for @blocks blocks where the tile accelerator would put the next
 * ones itself, or NULL if it has none to give. Blocks made there are
 * handed to elan_host_ta() like any others - which sees where they are
 * and does not copy them. Good until the next thing is sent. */
uint32_t *elan_host_ta_room(unsigned blocks);
/* The list the tile accelerator has open (0-4), or -1. */
int elan_host_ta_list(void);
/* The tile accelerator is waiting for the second half of a 64-byte
 * parameter. */
int elan_host_ta_half(void);
/* A list is over: bit @bit of the normal interrupt status, on both
 * PowerVRs. */
void elan_host_list_end(unsigned bit);
/* Copy @size bytes to video memory at @vram, from main memory where the
 * SH4's DMA channel 2 points (@from_eram 0) or from the chip's memory at
 * @src, and call elan_dma_done() when the time that takes is up.
 * False if the transfer cannot be done. */
bool elan_host_texture_dma(uint32_t vram, uint32_t src, uint32_t size, int from_eram);

RETRO_END_DECLS

#endif
