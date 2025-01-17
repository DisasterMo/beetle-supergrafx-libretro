/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* VDC and VCE emulation */

/*
"Tonight I hooked up my Turbo Duo(with no games or CDs in it)'s video output to my PC sound card, recorded it,
and did a FFT and looked at the spectrum(around the line rate, 15-16KHz), and I also counted the number
of samples between the ~60Hz peaks(just to verify that the math shown below is remotely accurate).

The spectrum peaked at 15734 Hz.  21477272.727272... / 3 / 15734 = 455.00(CPU cycles per scanline)"
*/

#include "pce.h"
#include "vdc.h"
#include "pcecd.h"
#include "../FileStream.h"
#include <math.h>

static uint16 systemColorMap16[2][512];   // 0 = normal, 1 = strip colorburst
static uint32 userle;                     // User layer enable.
static uint32 disabled_layer_color;

static bool unlimited_sprites;

#define ULE_BG0   1
#define ULE_SPR0  2
#define ULE_BG1   4
#define ULE_SPR1  8

static int defined_width[3] = {256, 352, 512};

static unsigned int VDS;
static unsigned int VSW;
static unsigned int VDW;
static unsigned int VCR;
static unsigned int VBlankFL;

static unsigned int VDC_TotalChips;
vce_t vce;
vdc_t vdc_chips[2];

static INLINE void MDFN_FastU32MemsetM8(uint32_t *array, uint32_t value_32, unsigned int u32len)
{
   uint32_t *ai;
   for(ai = array; ai < array + u32len; ai += 2)
   {
      ai[0] = value_32;
      ai[1] = value_32;
   }
}


#define MAKECOLOR_PCE(val) ((((val & 0x038) >> 3) << 13) | (((((val & 0x038) >> 3) & 0x6) << 10) | (((val & 0x1c0) >> 6) << 8) | (((val & 0x1c0) >> 6) << 5) | ((val & 0x007) << 2) | ((val & 0x007) >> 1)))

static INLINE void FixPCache(int entry)
{
   const uint16 *cm16 = systemColorMap16[vce.CR >> 7];

   if (!(entry & 0xFF))
   {
      uint32 color = vce.color_table[entry & 0x100];
      for (int x = 0; x < 16; x++)
      {
         if (VDC_TotalChips == 2)
            vce.color_table_cache[(entry & 0x100) + (x << 4)] = color | ALPHA_MASK;
         else
            vce.color_table_cache[(entry & 0x100) + (x << 4)] = cm16[color];
      }
   }

   if (entry & 0xF)
   {

      uint32 color = vce.color_table[entry];

      // For SuperGrafx VPCsprite handling to work
      if (entry & 0x100)
         color |= ALPHA_MASK << 2;

      if (VDC_TotalChips == 2)
         vce.color_table_cache[entry] = color;
      else
         vce.color_table_cache[entry] = cm16[color & 0x1ff];
   }
}

static INLINE void FixTileCache(vdc_t *which_vdc, uint16 A)
{
   uint32 charname = (A >> 4);
   uint32 y = (A & 0x7);
   uint64 *tc = &which_vdc->bg_tile_cache[charname][y];

   uint32 bitplane01 = which_vdc->VRAM[y + charname * 16];
   uint32 bitplane23 = which_vdc->VRAM[y + 8 + charname * 16];

   *tc = 0;

   for (int x = 0; x < 8; x++)
   {
      uint32 raw_pixel = ((bitplane01 >> x) & 1);
      raw_pixel |= ((bitplane01 >> (x + 8)) & 1) << 1;
      raw_pixel |= ((bitplane23 >> x) & 1) << 2;
      raw_pixel |= ((bitplane23 >> (x + 8)) & 1) << 3;

#ifdef MSB_FIRST
      *tc |= (uint64)raw_pixel << ((x)*8);
#else
      *tc |= (uint64)raw_pixel << ((7 - x) * 8);
#endif
   }
}

static INLINE void CheckFixSpriteTileCache(vdc_t *which_vdc, uint16 no, uint32 special)
{
   if (special != 0x4 && special != 0x5)
      special = 0;

   if ((special | 0x80) == which_vdc->spr_tile_clean[no])
      return;

   if ((no * 64) >= VRAM_Size)
   {
   }
   else if (special)
   {
      for (int y = 0; y < 16; y++)
      {
         uint8 *tc = which_vdc->spr_tile_cache[no][y];

         uint32 bitplane0 = which_vdc->VRAM[y + 0x00 + no * 0x40 + ((special & 1) << 5)];
         uint32 bitplane1 = which_vdc->VRAM[y + 0x10 + no * 0x40 + ((special & 1) << 5)];

         for (int x = 0; x < 16; x++)
         {
            uint32 raw_pixel;
            raw_pixel = ((bitplane0 >> x) & 1) << 0;
            raw_pixel |= ((bitplane1 >> x) & 1) << 1;
            tc[x] = raw_pixel;
         }
      }
   }
   else
   {
      for (int y = 0; y < 16; y++)
      {
         uint8 *tc = which_vdc->spr_tile_cache[no][y];

         uint32 bitplane0 = which_vdc->VRAM[y + 0x00 + no * 0x40];
         uint32 bitplane1 = which_vdc->VRAM[y + 0x10 + no * 0x40];
         uint32 bitplane2 = which_vdc->VRAM[y + 0x20 + no * 0x40];
         uint32 bitplane3 = which_vdc->VRAM[y + 0x30 + no * 0x40];

         for (int x = 0; x < 16; x++)
         {
            uint32 raw_pixel;
            raw_pixel = ((bitplane0 >> x) & 1) << 0;
            raw_pixel |= ((bitplane1 >> x) & 1) << 1;
            raw_pixel |= ((bitplane2 >> x) & 1) << 2;
            raw_pixel |= ((bitplane3 >> x) & 1) << 3;
            tc[x] = raw_pixel;
         }
      }
   }

   which_vdc->spr_tile_clean[no] = special | 0x80;
}

static INLINE void SetVCECR(uint8 V)
{
   const bool bw_changed = (V ^ vce.CR) & 0x80;

   vce.dot_clock = V & 1;
   if (V & 2)
      vce.dot_clock = 2;
   vce.CR = V;

   if (bw_changed)
   {
      for (int x = 0; x < 512; x++)
         FixPCache(x);
   }
}

static unsigned int frame_counter;

vpc_t vpc;

// Some virtual vdc macros to make code simpler to read
#define M_vdc_HSW (vdc->HSR & 0x1F)        // Horizontal Synchro Width
#define M_vdc_HDS ((vdc->HSR >> 8) & 0x7F) // Horizontal Display Start
#define M_vdc_HDW (vdc->HDR & 0x7F)        // Horizontal Display Width
#define M_vdc_HDE ((vdc->HDR >> 8) & 0x7F) // Horizontal Display End

#define M_vdc_VSW (vdc->VSR & 0x1F)        // Vertical synchro width
#define M_vdc_VDS ((vdc->VSR >> 8) & 0xFF) // Vertical Display Start
#define M_vdc_VDW (vdc->VDR & 0x1FF)       // Vertical Display Width(Height? :b)
#define M_vdc_VCR (vdc->VCR & 0xFF)

#define VDCS_CR   0x01 // Sprite #0 collision interrupt occurred
#define VDCS_OR   0x02 // sprite overflow "" ""
#define VDCS_RR   0x04 // RCR             ""  ""
#define VDCS_DS   0x08 // VRAM to SAT DMA completion interrupt occurred
#define VDCS_DV   0x10 // VRAM to VRAM DMA completion interrupt occurred
#define VDCS_VD   0x20 // Vertical blank interrupt occurred
#define VDCS_BSY  0x40 // VDC is waiting for a CPU access slot during the active display area??

void VDC_SetPixelFormat(const uint8 *CustomColorMap, const uint32 CustomColorMapLen)
{
   for (int x = 0; x < 512; x++)
   {
      int r, g, b;
      int sc_r, sc_g, sc_b;

      if (CustomColorMap)
      {
         r = CustomColorMap[x * 3 + 0];
         g = CustomColorMap[x * 3 + 1];
         b = CustomColorMap[x * 3 + 2];
      }
      else
      {
         b = 36 * (x & 0x007);
         r = 36 * ((x & 0x038) >> 3);
         g = 36 * ((x & 0x1c0) >> 6);
      }

      if (CustomColorMap && CustomColorMapLen == 1024)
      {
         sc_r = CustomColorMap[(512 + x) * 3 + 0];
         sc_g = CustomColorMap[(512 + x) * 3 + 1];
         sc_b = CustomColorMap[(512 + x) * 3 + 2];
      }
      else
      {
         double y;

         y = floor(0.5 + 0.300 * r + 0.589 * g + 0.111 * b);

         if (y < 0)
            y = 0;

         if (y > 255)
            y = 255;

         sc_r = sc_g = sc_b = y;
      }
      systemColorMap16[0][x] = MAKECOLOR(r, g, b, 0);
      systemColorMap16[1][x] = MAKECOLOR(sc_r, sc_g, sc_b, 0);
   }
   // I know the temptation is there, but don't combine these two loops just
   // because they loop 512 times ;)
   for (int x = 0; x < 512; x++)
      FixPCache(x);

   disabled_layer_color = MAKECOLOR(0x00, 0xFE, 0x00, 0);
}

DECLFR(VCE_Read)
{
   switch (A & 0x7)
   {
   case 4:
      return (vce.color_table[vce.ctaddress & 0x1FF]);
   case 5:
   {
      uint8 ret = vce.color_table[vce.ctaddress & 0x1FF] >> 8;
      ret &= 1;
      ret |= 0xFE;
      vce.ctaddress++;
      vce.ctaddress &= 0x1FF;
      return (ret);
   }
   }
   return (0xFF);
}

DECLFW(VCE_Write)
{
   switch (A & 0x7)
   {
   case 0:
      SetVCECR(V);
      break;
   case 2:
      vce.ctaddress &= 0x100;
      vce.ctaddress |= V;
      break;
   case 3:
      vce.ctaddress &= 0x0FF;
      vce.ctaddress |= (V & 1) << 8;
      break;
   case 4:
      vce.color_table[vce.ctaddress & 0x1FF] &= 0x100;
      vce.color_table[vce.ctaddress & 0x1FF] |= V;
      FixPCache(vce.ctaddress & 0x1FF);
      break;
   case 5:
      vce.color_table[vce.ctaddress & 0x1FF] &= 0xFF;
      vce.color_table[vce.ctaddress & 0x1FF] |= (V & 1) << 8;
      FixPCache(vce.ctaddress & 0x1FF);
      vce.ctaddress++;
      vce.ctaddress &= 0x1FF;
      break;
   }
}

void VDC_SetLayerEnableMask(uint64 mask)
{
   userle = mask;
}

DECLFW(VDC_Write_ST)
{
   if (VDC_TotalChips == 2)
      A |= vpc.st_mode ? 0x10 : 0;

   VDC_Write(A, V);
}

static void DoDMA(vdc_t *vdc)
{
   // Assuming one cycle for reads, one cycle for write, with DMA?
   for (int i = 0; i < 455; i++)
   {
      if (!vdc->DMAReadWrite)
      {
         if (vdc->SOUR >= VRAM_Size)
            VDC_UNDEFINED("Unmapped VRAM DMA read");

         vdc->DMAReadBuffer = vdc->VRAM[vdc->SOUR];
      }
      else
      {
         if (vdc->DESR < VRAM_Size)
         {
            vdc->VRAM[vdc->DESR] = vdc->DMAReadBuffer;
            FixTileCache(vdc, vdc->DESR);
            vdc->spr_tile_clean[vdc->DESR >> 6] = 0;
         }

         vdc->SOUR += (((vdc->DCR & 0x4) >> 1) ^ 2) - 1;
         vdc->DESR += (((vdc->DCR & 0x8) >> 2) ^ 2) - 1;
         vdc->LENR--;
         if (vdc->LENR == 0xFFFF) // DMA is done.
         {
            vdc->DMARunning = 0;
            if (vdc->DCR & 0x02)
            {
               vdc->status |= VDCS_DV;
               HuC6280_IRQBegin(MDFN_IQIRQ1);
               VDC_DEBUG("DMA IRQ");
            }
            break;
         }
      }
      vdc->DMAReadWrite ^= 1;
   } // for()
}

DECLFW(VDC_Write)
{
   int msb = A & 1;
   int chip = 0;
   vdc_t *vdc;

   if (VDC_TotalChips == 2)
   {
      A &= 0x1F;
      switch (A)
      {
      case 0x8:
         vpc.priority[0] = V;
         break;
      case 0x9:
         vpc.priority[1] = V;
         break;
      case 0xA:
         vpc.winwidths[0] &= 0x300;
         vpc.winwidths[0] |= V;
         break;
      case 0xB:
         vpc.winwidths[0] &= 0x0FF;
         vpc.winwidths[0] |= (V & 3) << 8;
         break;
      case 0xC:
         vpc.winwidths[1] &= 0x300;
         vpc.winwidths[1] |= V;
         break;
      case 0xD:
         vpc.winwidths[1] &= 0x0FF;
         vpc.winwidths[1] |= (V & 3) << 8;
         break;
      case 0xE:
         vpc.st_mode = V & 1;
         break;
      }
      if (A & 0x8)
         return;

      chip = (A & 0x10) >> 4;
      vdc = &vdc_chips[chip];
      A &= 0x3;
   }
   else
   {
      vdc = &vdc_chips[0];
      A &= 0x3;
   }

   switch (A)
   {
   case 0x0:
      vdc->select = V & 0x1F;
      break;
   case 0x2:
   case 0x3:
      switch (vdc->select & 0x1F)
      {
      case 0x00:
         REGSETP(vdc->MAWR, V, msb);
         break;
      case 0x01:
         REGSETP(vdc->MARR, V, msb);
         if (msb)
         {
            if (vdc->MARR >= VRAM_Size)
               VDC_UNDEFINED("Unmapped VRAM VRR(MARR set) read");

            vdc->read_buffer = vdc->VRAM[vdc->MARR];
         }
         break;
      case 0x02:
         if (!msb)
            vdc->write_latch = V;
         else
         {
            if (vdc->MAWR < VRAM_Size)
            {
               // A hack to fix Crest of Wolf, and maybe others.
               while (vdc->DMARunning)
                  DoDMA(vdc);

               vdc->VRAM[vdc->MAWR] = (V << 8) | vdc->write_latch;
               FixTileCache(vdc, vdc->MAWR);
               vdc->spr_tile_clean[vdc->MAWR >> 6] = 0;
            }
            vdc->MAWR += vram_inc_tab[(vdc->CR >> 11) & 0x3];
         }
         break;
      case 0x05:
         REGSETP(vdc->CR, V, msb);
         break;
      case 0x06:
         REGSETP(vdc->RCR, V, msb);
         vdc->RCR &= 0x3FF;
         break;
      case 0x07:
         REGSETP(vdc->BXR, V, msb);
         vdc->BXR &= 0x3FF;
         break;
      case 0x08:
         REGSETP(vdc->BYR, V, msb);
         vdc->BYR &= 0x1FF;
         vdc->BG_YOffset = vdc->BYR; // Set it on LSB and MSB writes(only changing on MSB breaks Youkai Douchuuki)
         break;
      case 0x09:
         REGSETP(vdc->MWR, V, msb);
         break;
      case 0x0a:
         REGSETP(vdc->HSR, V, msb);
         break;
      case 0x0b:
         REGSETP(vdc->HDR, V, msb);
         break;
      case 0x0c:
         REGSETP(vdc->VSR, V, msb);
         break;
      case 0x0d:
         REGSETP(vdc->VDR, V, msb);
         break;
      case 0x0e:
         REGSETP(vdc->VCR, V, msb);
         break;
      case 0x0f:
         REGSETP(vdc->DCR, V, msb);
         break;
      case 0x10:
         REGSETP(vdc->SOUR, V, msb);
         break;
      case 0x11:
         REGSETP(vdc->DESR, V, msb);
         break;
      case 0x12:
         REGSETP(vdc->LENR, V, msb);
         if (msb)
         {
            vdc->DMARunning = 1;
            vdc->DMAReadWrite = 0;
            if (vdc->burst_mode && !(vdc->DCR & 0x02))
               DoDMA(vdc); // Do one line's worth of DMA transfers
                           // because Cosmic Fantasy 4 is evil
                           // and uses timed writes to the DMA
                           // start register, rather than waiting until
                           // the machine says we're done,
                           // which would require cycle-accurate VDC emulation...like that's
                           // going to happen when I don't even have accurate values
                           // for HuC6280 instruction timings. :b
         }
         break;
      case 0x13:
         REGSETP(vdc->SATB, V, msb);
         vdc->SATBPending = 1;
         break;
      }
      break;
   }
}

#define CB_EXL(n) (((n) << 4) | ((n) << 12) | ((n) << 20) | ((n) << 28) | ((n) << 36) | ((n) << 44) | ((n) << 52) | ((n) << 60))
static const uint8 bat_width_shift_tab[4] = {5, 6, 7, 7};
static const uint64 cblock_exlut[16] = {
    CB_EXL(0ULL), CB_EXL(1ULL), CB_EXL(2ULL), CB_EXL(3ULL), CB_EXL(4ULL), CB_EXL(5ULL), CB_EXL(6ULL), CB_EXL(7ULL),
    CB_EXL(8ULL), CB_EXL(9ULL), CB_EXL(10ULL), CB_EXL(11ULL), CB_EXL(12ULL), CB_EXL(13ULL), CB_EXL(14ULL), CB_EXL(15ULL)};

static void DrawBG(const vdc_t *vdc, const uint32 count, uint8 *target) NO_INLINE;
static void DrawBG(const vdc_t *vdc, const uint32 count, uint8 *target)
{
   int bat_width_shift = bat_width_shift_tab[(vdc->MWR >> 4) & 3];
   int bat_width_mask = (1U << bat_width_shift) - 1;
   int bat_height_mask = 31 + ((vdc->MWR & 0x40) >> 1);
   uint64 *target64 = (uint64 *)target;

   {
      int bat_y = ((vdc->BG_YOffset >> 3) & bat_height_mask) << bat_width_shift;

      int bat_boom = (vdc->BG_XOffset >> 3) & bat_width_mask;
      int line_sub = vdc->BG_YOffset & 7;

      const uint16 *BAT_Base = &vdc->VRAM[bat_y];
      const uint64 *CG_Base = &vdc->bg_tile_cache[0][line_sub];

      uint64 cg_mask = 0xFFFFFFFFFFFFFFFFULL;

      if ((vdc->MWR & 0x3) == 0x3) // 2-bit CG rendering, else normal CG rendering
         cg_mask = (vdc->MWR & 0x80) ? 0xCCCCCCCCCCCCCCCCULL : 0x3333333333333333ULL;

      {
         for (int x = count - 1; x >= 0; x -= 8)
         {
            const uint16 bat = BAT_Base[bat_boom];
            const uint64 color_or = cblock_exlut[bat >> 12];

            *target64 = (CG_Base[(bat & 0xFFF) * 8] & cg_mask) | color_or;

            bat_boom = (bat_boom + 1) & bat_width_mask;
            target64++;
         }
      }
   }
}

#define SPRF_PRIORITY   0x00080
#define SPRF_HFLIP      0x00800
#define SPRF_VFLIP      0x08000
#define SPRF_SPRITE0    0x10000

static const unsigned int sprite_height_tab[4] = {16, 32, 64, 64};
static const unsigned int sprite_height_no_mask[4] = {~0U, ~2U, ~6U, ~6U};

static INLINE void RebuildSATCache(vdc_t *vdc)
{
   SAT_Cache_t *sat_ptr = vdc->SAT_Cache;

   vdc->SAT_Cache_Valid = 0;

   for (int i = 0; i < 64; i++)
   {
      const uint16 SATR0 = vdc->SAT[i * 4 + 0x0];
      const uint16 SATR1 = vdc->SAT[i * 4 + 0x1];
      const uint16 SATR2 = vdc->SAT[i * 4 + 0x2];
      const uint16 SATR3 = vdc->SAT[i * 4 + 0x3];

      int16 y;
      uint16 height;
      uint16 x;
      uint16 no;
      uint16 flags;
      bool cgmode;
      uint32 width;

      y      = (int16)(SATR0 & 0x3FF) - 0x40;
      x      = SATR1 & 0x3FF;
      no     = (SATR2 >> 1) & 0x3FF;
      flags  = (SATR3);
      cgmode = SATR2 & 0x1;

      width  = ((flags >> 8) & 1);
      flags &= ~0x100;

      height = sprite_height_tab[(flags >> 12) & 3];
      no    &= sprite_height_no_mask[(flags >> 12) & 3];

      no = ((no & ~width) | 0) ^ ((flags & SPRF_HFLIP) ? width : 0);

      sat_ptr->y      = y;
      sat_ptr->height = height;
      sat_ptr->x      = x;
      sat_ptr->no     = no;
      sat_ptr->flags  = flags;
      sat_ptr->cgmode = cgmode;

      sat_ptr++;
      vdc->SAT_Cache_Valid++;

      if (width)
      {
         no = ((no & ~width) | 1) ^ ((flags & SPRF_HFLIP) ? width : 0);
         x += 16;

         *sat_ptr = *(sat_ptr - 1);

         sat_ptr->no = no;
         sat_ptr->x = x;

         sat_ptr++;
         vdc->SAT_Cache_Valid++;
      }
   }
}

static INLINE void DoSATDMA(vdc_t *vdc)
{
   if (vdc->SATB > (VRAM_Size - 0x100))
      VDC_UNDEFINED("Unmapped VRAM SATB DMA read");

   for (int i = 0; i < 256; i++)
      vdc->SAT[i] = vdc->VRAM[(vdc->SATB + i) & 0xFFFF];

   RebuildSATCache(vdc);
}

typedef struct
{
   uint32 x;
   uint32 flags;
   uint8 palette_index;
   uint16 no;
   uint16 sub_y;
} SPRLE;

static const unsigned int spr_hpmask = 0x8000; // High priority bit mask

// DrawSprites will write up to 0x20 units before the start of the pointer it's passed.
static void DrawSprites(vdc_t *vdc, const int32 end, uint16 *spr_linebuf) NO_INLINE;
static void DrawSprites(vdc_t *vdc, const int32 end, uint16 *spr_linebuf)
{
   int active_sprites = 0;
   SPRLE SpriteList[64 * 2]; // (see unlimited_sprites option, *2 to accomodate 32-pixel-width sprites ) //16];

   // First, grab the up to 16(or 128 for unlimited_sprites) sprite units(16xWHATEVER; each 32xWHATEVER sprite counts as 2 sprite units when
   // rendering a scanline) for this scanline.
   for (int i = 0; i < vdc->SAT_Cache_Valid; i++)
   {
      const SAT_Cache_t *SATR       = &vdc->SAT_Cache[i];

      int16 y                       = SATR->y;
      uint16 x                      = SATR->x;
      uint16 no                     = SATR->no;
      const uint16 flags            = SATR->flags;
      const uint8 cgmode            = SATR->cgmode;
      const uint16 height           = SATR->height;
      const uint32 palette_index    = (flags & 0xF) << 4;

      uint32 y_offset               = vdc->RCRCount - y;

      if (y_offset < height)
      {
         if (active_sprites == 16)
         {
            if (vdc->CR & 0x2)
            {
               vdc->status |= VDCS_OR;
               HuC6280_IRQBegin(MDFN_IQIRQ1);
               VDC_DEBUG("Overflow IRQ");
            }
            if (!unlimited_sprites)
               break;
         }

         if (flags & SPRF_VFLIP)
            y_offset = height - 1 - y_offset;

         no |= (y_offset & 0x30) >> 3;

         SpriteList[active_sprites].flags = flags;

         SpriteList[active_sprites].x = x;
         SpriteList[active_sprites].palette_index = palette_index;

         SpriteList[active_sprites].no = no;
         SpriteList[active_sprites].sub_y = (y_offset & 15);

         CheckFixSpriteTileCache(vdc, no, (vdc->MWR & 0xC) | cgmode);

         SpriteList[active_sprites].flags |= i ? 0 : SPRF_SPRITE0;

         active_sprites++;
      }
   }

   //if(!active_sprites)
   // return;

   //memset(spr_linebuf, 0, sizeof(uint16) * end);
   MDFN_FastU32MemsetM8((uint32 *)spr_linebuf, 0, ((end + 3) >> 1) & ~1);

   if (!active_sprites)
      return;

   for (int i = (active_sprites - 1); i >= 0; i--)
   {
      int32 pos = SpriteList[i].x - 0x20;
      uint32 prio_or;
      uint16 *dest_pix;

      if (pos > end)
         continue;

      dest_pix = &spr_linebuf[pos];

      prio_or = 0x100 | SpriteList[i].palette_index;

      if (SpriteList[i].flags & SPRF_PRIORITY)
         prio_or |= spr_hpmask;

      if ((SpriteList[i].flags & SPRF_SPRITE0) && (vdc->CR & 0x01))
      {
         const uint8 *pix_source = vdc->spr_tile_cache[SpriteList[i].no][SpriteList[i].sub_y];

         int32 inc = -1;
         int32 pos  = 15;

         if (SpriteList[i].flags & SPRF_HFLIP)
         {
            inc = 1;
            pos  = 0;
         }

         {
            for (int32 x = 0; x < 16; x++, pos += inc)
            {
               const uint32 raw_pixel = pix_source[pos];
               if (raw_pixel)
               {
                  if (((uint32)pos + x) >= (uint32)end) // Covers negative and overflowing the right side(to prevent spurious sprite hits)
                     continue;

                  if (dest_pix[x] & 0x100)
                  {
                     vdc->status |= VDCS_CR;
                     VDC_DEBUG("Sprite hit IRQ");
                     HuC6280_IRQBegin(MDFN_IQIRQ1);
                  }
                  dest_pix[x] = raw_pixel | prio_or;
               }
            }
         }
      }    // End sprite0 handling
      else // No sprite0 hit:
      {
         const uint8 *pix_source = vdc->spr_tile_cache[SpriteList[i].no][SpriteList[i].sub_y];

         int32 inc = -1;
         int32 pos  = 15;

         if (SpriteList[i].flags & SPRF_HFLIP)
         {
            inc = 1;
            pos  = 0;
         }

         // x must be signed, for "pos + x" to not be promoted to unsigned, which will cause a stack overflow.
         {
            for (int32 x = 0; x < 16; x++, pos += inc)
            {
               const uint32 raw_pixel = pix_source[pos];
               if (raw_pixel)
                  dest_pix[x] = raw_pixel | prio_or;
            }
         }

      } // End no sprite0 hit
   }
}

static inline void MixBGSPR(const uint32 count_in, const uint8 *bg_linebuf_in, const uint16 *spr_linebuf_in, uint16_t *target_in)
{
   for (unsigned int x = 0; x < count_in; x++)
   {
      const uint32 bg_pixel   = bg_linebuf_in[x];
      const uint32 spr_pixel  = spr_linebuf_in[x];
      uint32 pixel            = bg_pixel;

      if (((int16)(spr_pixel | ((bg_pixel & 0x0F) - 1))) < 0)
         pixel = spr_pixel;

      target_in[x] = vce.color_table_cache[pixel & 0x1FF];
   }
}

static inline void MixBGOnly(const uint32 count, const uint8 *bg_linebuf, uint16_t *target)
{
   for (unsigned int x = 0; x < count; x++)
      target[x] = vce.color_table_cache[bg_linebuf[x]];
}

static inline void MixSPROnly(const uint32 count, const uint16 *spr_linebuf, uint16_t *target)
{
   for (unsigned int x = 0; x < count; x++)
      target[x] = vce.color_table_cache[(spr_linebuf[x] | 0x100) & 0x1FF];
}

static inline void MixNone(const uint32 count, uint16_t *target)
{
   uint32 bg_color = vce.color_table_cache[0x000];

   for (unsigned int x = 0; x < count; x++)
      target[x] = bg_color;
}

static const int prio_select[4] = {1, 1, 0, 0};
static const int prio_shift[4] = {4, 0, 4, 0};

static void MixVPC(const uint32 count, const uint16 *lb0, const uint16 *lb1, uint16_t *target)
{
   // Windowing disabled.
   if (vpc.winwidths[0] <= 0x40 && vpc.winwidths[1] <= 0x40)
   {
      const uint8 pb = (vpc.priority[prio_select[0]] >> prio_shift[0]) & 0xF;

      for (int x = 0; x < (int)count; x++)
      {
#include "vpc_mix_inner.inc"
      }

      //switch(pb & 0xF)
      //{
      // case 0x0: for(int x = 0; x < (int)count; x++)
      //	    {
      //	     #include "vpc_mix_inner.inc"
      //	    }
      //	    break;
      //}
   }
   else
      for (int x = 0; x < (int)count; x++)
      {
         int in_window = 0;

         if (x < (vpc.winwidths[0] - 0x40))
            in_window |= 1;

         if (x < (vpc.winwidths[1] - 0x40))
            in_window |= 2;

         uint8 pb = (vpc.priority[prio_select[in_window]] >> prio_shift[in_window]) & 0xF;

#include "vpc_mix_inner.inc"
      }
}

static void DrawOverscan(const vdc_t *vdc, uint16_t *target, const MDFN_Rect *lw, const bool full = true, const int32 vpl = 0, const int32 vpr = 0)
{
   uint32 os_color = vce.color_table_cache[0x100];

   // SuperGrafx uses bit 0-8 for color table data since this is not passed through MAKECOLOR_PCE until MixVPC()
   if (VDC_TotalChips == 2)
      os_color &= 0x1FF;

   int x = lw->x;

   if (!full)
   {
      for (; x < vpl; x++)
         target[x] = os_color;

      x = vpr;
   }

   for (; x < lw->x + lw->w; x++)
      target[x] = os_color;
}

void VDC_RunFrame(EmulateSpecStruct *espec, bool IsHES)
{
   vdc_t *vdc              = &vdc_chips[0];
   int max_dc              = 0;
   MDFN_Surface *surface   = espec->surface;
   MDFN_Rect *DisplayRect  = &espec->DisplayRect;
   int32 *LineWidths       = espec->LineWidths;
   bool skip               = espec->skip || IsHES;

   // x and w should be overwritten in the big loop

   if (!skip)
   {
      DisplayRect->x = 0;
      DisplayRect->w = 256;

      DisplayRect->y = MDFN_GetSettingUI("pce_fast.slstart");
      DisplayRect->h = MDFN_GetSettingUI("pce_fast.slend") - DisplayRect->y + 1;

      // Hack for the input latency-reduction hack, part 1.
      for (int y = DisplayRect->y; y < DisplayRect->y + DisplayRect->h; y++)
         LineWidths[y] = 0;
   }

   //Change 352 mode width without restart
   if (defined_width[1] != MDFN_GetSettingUI("pce_fast.hoverscan"))
      defined_width[1] = MDFN_GetSettingUI("pce_fast.hoverscan");

   do
   {
      vdc = &vdc_chips[0];

      if (frame_counter == 0)
      {
         VDS = M_vdc_VDS;
         VSW = M_vdc_VSW;
         VDW = M_vdc_VDW;
         VCR = M_vdc_VCR;
         VBlankFL = VDS + VSW + VDW + 1;

         if (VBlankFL > 261)
            VBlankFL = 261;
      }

      bool need_vbi[2] = {false, false};

      int32 line_leadin1   = 0;
      int32 magical        = 0;
      int32 cyc_tot        = 0;

      magical = M_vdc_HDS + (M_vdc_HDW + 1) + M_vdc_HDE;
      magical = (magical + 2) & ~1;
      magical -= M_vdc_HDW + 1;
      cyc_tot = magical * 8; //ClockPixelWidths[vce.dot_clock] - magical * 8;
      cyc_tot -= 2;
      switch (vce.dot_clock)
      {
      case 0:
         cyc_tot = 4 * cyc_tot / 3;
         break;
      case 1:
         break;
      case 2:
         cyc_tot = 2 * cyc_tot / 3;
         break;
      }

      if (cyc_tot < 0)
         cyc_tot = 0;
      line_leadin1 = cyc_tot;

      if (max_dc < vce.dot_clock)
         max_dc = vce.dot_clock;

      if (!skip)
      {
         DisplayRect->x = 0;

         // Order of Griffon semi-hack
         if (OrderOfGriffonFix)
            // Force to use specified width to fit status bar inside frame.
            defined_width[0] = defined_width[1] = 320;

         DisplayRect->w = defined_width[vce.dot_clock];
      }

      for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      {
         vdc = &vdc_chips[chip];
         if (frame_counter == 0)
         {
            vdc->display_counter = 0;
            vdc->burst_mode = !(vdc->CR & 0xC0);
         }

         if (vdc->display_counter == (VDS + VSW))
         {
            vdc->burst_mode = !(vdc->CR & 0xC0);
            vdc->RCRCount = 0;
         }
         int have_free_time = 1;

         if (!vdc->burst_mode && vdc->display_counter >= (VDS + VSW) && vdc->display_counter < (VDS + VSW + VDW + 1))
            have_free_time = 0;

         if (have_free_time) // We're outside of the active display area.  Weehee
         {
            if (vdc->DMARunning)
               DoDMA(vdc);
         }

         if (vdc->display_counter == VBlankFL)
         {
            need_vbi[chip] = true;
            if (vdc->SATBPending || (vdc->DCR & 0x10))
            {
               vdc->SATBPending = 0;
               vdc->sat_dma_slcounter = 2;

               DoSATDMA(vdc);
            }
         }
         if ((int)vdc->RCRCount == ((int)vdc->RCR - 0x40) && (vdc->CR & 0x04))
         {
            VDC_DEBUG("RCR IRQ");
            vdc->status |= VDCS_RR;
            HuC6280_IRQBegin(MDFN_IQIRQ1);
         }
      }

      HuC6280_Run(line_leadin1);

      MDFN_ALIGN(8) uint16 line_buffer[2][1024]; // For super grafx emulation
      MDFN_ALIGN(8) uint8 bg_linebuf[8 + 1024];
      MDFN_ALIGN(8) uint16 spr_linebuf[16 + 1024];

      const bool SHOULD_DRAW = (!skip && (int)frame_counter >= (DisplayRect->y + 14) && (int)frame_counter < (DisplayRect->y + DisplayRect->h + 14));
      const bool fc_vrm = (frame_counter >= 14 && frame_counter < (14 + 242 + 1));

      for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      {
         uint16 *target_ptr16 = surface->pixels16 + (frame_counter - 14) * surface->pitchinpix;

         vdc = &vdc_chips[chip];

         if (VDC_TotalChips == 2)
            target_ptr16 = line_buffer[chip];

         if (fc_vrm && !skip)
            LineWidths[frame_counter - 14] = DisplayRect->w;

         if (vdc->burst_mode)
         {
            if (fc_vrm && SHOULD_DRAW)
            {
               DrawOverscan(vdc, target_ptr16, DisplayRect);
            }
         }
         else if (vdc->display_counter >= (VDS + VSW) && vdc->display_counter < (VDS + VSW + VDW + 1))
         {
            if (vdc->display_counter == (VDS + VSW))
               vdc->BG_YOffset = vdc->BYR;
            else
               vdc->BG_YOffset++;
            vdc->BG_XOffset = vdc->BXR;

            if (fc_vrm)
            {
               uint32 start = (M_vdc_HDS + 1) * 8;
               uint32 end = start + (M_vdc_HDW + 1) * 8;

               if ((vdc->CR & 0x80) && SHOULD_DRAW)
               {
                  if (userle & (chip ? ULE_BG1 : ULE_BG0))
                     DrawBG(vdc, end - start + (vdc->BG_XOffset & 7), bg_linebuf);
                  else
                     memset(bg_linebuf, 0, end - start + (vdc->BG_XOffset & 7));
               }

               if ((vdc->CR & 0x40) && (SHOULD_DRAW || (vdc->CR & 0x03))) // Don't skip sprite drawing if we can generate sprite #0 or sprite overflow IRQs.
               {
                  if ((userle & (chip ? ULE_SPR1 : ULE_SPR0)) || (vdc->CR & 0x03))
                     DrawSprites(vdc, end - start, spr_linebuf + 0x20);

                  if (!(userle & (chip ? ULE_SPR1 : ULE_SPR0)))
                     memset(spr_linebuf + 0x20, 0, sizeof(uint16) * (end - start));
               }

               if (SHOULD_DRAW)
               {
                  int32 width = end - start;
                  int32 source_offset = 0;
                  int32 target_offset = 0;

                  //Centre any picture thinner than its display mode width
                  if (width > 0 && width < defined_width[vce.dot_clock])
                  {
                     target_offset = (defined_width[vce.dot_clock] - width) / 2;
                  }

                  //Centre overscan cropping
                  if (vce.dot_clock == 1 && defined_width[1] < width)
                  {
                     target_offset += (defined_width[1] - width) / 2;
                  }

                  // Align TV Sport Basketball
                  if (vce.dot_clock == 2 && width > 512)
                  {
                     target_offset = -16;
                  }

                  // Semi-hack for Asuka 120%
                  if (vce.dot_clock == 1 && M_vdc_HDS == 5 && M_vdc_HDE == 6 && M_vdc_HDW == 43 && M_vdc_HSW == 2)
                     target_offset += 8;
                  else if (vce.dot_clock == 0 && M_vdc_HDS == 2 && M_vdc_HDE == 3 && M_vdc_HDW == 33 && M_vdc_HSW == 2)
                     target_offset = 0;
                  // and for Addams Family
                  else if (vce.dot_clock == 1 && M_vdc_HDS == 4 && M_vdc_HDE == 4 && M_vdc_HDW == 43 && M_vdc_HSW == 9)
                     target_offset += 4;
                  // Final Blaster intro fix
                  else if (vce.dot_clock == 0 && M_vdc_HDS == 2 && M_vdc_HDE == 26 && M_vdc_HDW == 9 && M_vdc_HSW == 2)
                     target_offset -= 88;
                  else if (vce.dot_clock == 0 && M_vdc_HDS == 24 && M_vdc_HDE == 4 && M_vdc_HDW == 9 && M_vdc_HSW == 2)
                     target_offset += 88;

                  if (target_offset < 0)
                  {
                     width += target_offset;
                     source_offset += 0 - target_offset;
                     target_offset = 0;
                  }

                  if ((target_offset + width) > DisplayRect->w)
                     width = (int32)DisplayRect->w - target_offset;

                  //if(vdc->display_counter == 50)
                  //	MDFN_DispMessage("soffset=%d, toffset=%d, width=%d", source_offset, target_offset, width);

                  if (width > 0)
                  {
                     switch (vdc->CR & 0xC0)
                     {
                     case 0xC0:
                        MixBGSPR(width, bg_linebuf + (vdc->BG_XOffset & 7) + source_offset, spr_linebuf + 0x20 + source_offset, target_ptr16 + target_offset);
                        break;
                     case 0x80:
                        MixBGOnly(width, bg_linebuf + (vdc->BG_XOffset & 7) + source_offset, target_ptr16 + target_offset);
                        break;
                     case 0x40:
                        MixSPROnly(width, spr_linebuf + 0x20 + source_offset, target_ptr16 + target_offset);
                        break;
                     case 0x00:
                        MixNone(width, target_ptr16 + target_offset);
                        break;
                     }
                  }
                  DrawOverscan(vdc, target_ptr16, DisplayRect, false, target_offset, target_offset + width);
               } // end if(SHOULD_DRAW)
            }
         }
         else if (SHOULD_DRAW && fc_vrm) // Hmm, overscan...
         {
            //else if(target_ptr16)
            DrawOverscan(vdc, target_ptr16, DisplayRect);
         }
      }

      if (VDC_TotalChips == 2 && SHOULD_DRAW && fc_vrm)
      {
         //else if(surface->format.bpp == 16)
         MixVPC(DisplayRect->w, line_buffer[0] + DisplayRect->x, line_buffer[1] + DisplayRect->x, surface->pixels16 + (frame_counter - 14) * surface->pitchinpix + DisplayRect->x);
      }

      if (SHOULD_DRAW && fc_vrm)
      {
         MDFN_MidLineUpdate(espec, frame_counter - 14);
      }

      for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      {
         if ((vdc_chips[chip].CR & 0x08) && need_vbi[chip])
            vdc_chips[chip].status |= VDCS_VD;
      }

      HuC6280_Run(2);

      for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      {
         if (vdc_chips[chip].status & VDCS_VD)
         {
            VDC_DEBUG("VBlank IRQ");
            HuC6280_IRQBegin(MDFN_IQIRQ1);
         }
      }

      HuC6280_Run(455 - line_leadin1 - 2);

      if (PCE_IsCD)
      {
         PCECD_Run(HuCPU.timestamp * 3);
      }

      for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      {
         vdc = &vdc_chips[chip];
         vdc->RCRCount++;

         //vdc->BG_YOffset = (vdc->BG_YOffset + 1);
         vdc->display_counter++;

         if (vdc->sat_dma_slcounter)
         {
            vdc->sat_dma_slcounter--;
            if (!vdc->sat_dma_slcounter)
            {
               if (vdc->DCR & 0x01)
               {
                  VDC_DEBUG("Sprite DMA IRQ");
                  vdc->status |= VDCS_DS;
                  HuC6280_IRQBegin(MDFN_IQIRQ1);
               }
            }
         }

         if (vdc->display_counter == (VDS + VSW + VDW + VCR + 3))
         {
            vdc->display_counter = 0;
         }
      }

      frame_counter = (frame_counter + 1) % ((vce.CR & 0x04) ? 263 : 262);
   } while (frame_counter != VBlankFL); // big frame loop!

   DisplayRect->w = defined_width[vce.dot_clock];

   // Hack for the input latency-reduction hack, part 2.
   if (!skip)
   {
      for (int y = DisplayRect->y; y < DisplayRect->y + DisplayRect->h; y++)
      {
         if (!LineWidths[y])
         {
            LineWidths[y] = DisplayRect->w;

            DrawOverscan(&vdc_chips[0], surface->pixels16 + y * surface->pitchinpix, DisplayRect);

            MDFN_MidLineUpdate(espec, y);
         }
      }
   }
}

void VDC_Reset(void)
{
   vdc_chips[0].read_buffer = 0xFFFF;

   vpc.priority[0] = vpc.priority[1] = 0x11;

   vdc_chips[0].HSR = vdc_chips[0].HDR = vdc_chips[0].VSR = vdc_chips[0].VDR = vdc_chips[0].VCR = 0xFF; // Needed for Body Conquest 2

   if (VDC_TotalChips == 2)
   {
      vdc_chips[1].read_buffer = 0xFFFF;
      vdc_chips[1].HSR = vdc_chips[1].HDR = vdc_chips[1].VSR = vdc_chips[1].VDR = vdc_chips[1].VCR = 0xFF; // and for HES playback to not go bonkers
   }
   frame_counter = 0;
}

void VDC_Power(void)
{
   for (unsigned chip = 0; chip < VDC_TotalChips; chip++)
      memset(&vdc_chips[chip], 0, sizeof(vdc_t));
   VDC_Reset();
}

void VDC_Init(int sgx)
{
   unlimited_sprites = MDFN_GetSettingB("pce_fast.nospritelimit");
   defined_width[1] = MDFN_GetSettingUI("pce_fast.hoverscan");
   userle = ~0;

   VDC_TotalChips = sgx ? 2 : 1;
}

void VDC_SetSettings(const bool nospritelimit)
{
   unlimited_sprites = nospritelimit;
}

void VDC_Close(void)
{
}

int VDC_StateAction(StateMem *sm, int load, int data_only)
{
   SFORMAT VCE_StateRegs[] =
   {
      SFVARN(vce.CR, "VCECR"),
      SFVARN(vce.dot_clock, "dot clock"),
      SFVARN(vce.ctaddress, "ctaddress"),
      SFARRAY16N(vce.color_table, 0x200, "color_table"),
      SFEND
   };

   int ret = MDFNSS_StateAction(sm, load, data_only, VCE_StateRegs, "VCE", false);

   if (load)
   {
      vce.ctaddress &= 0x1FF;
      vce.dot_clock %= 2 + 1;
   }

   unsigned max_chips = VDC_TotalChips;

   if (VDC_TotalChips == 2)
   {
      SFORMAT VPC_StateRegs[] =
      {
         SFVARN(vpc.st_mode, "st_mode"),
         SFARRAYN(vpc.priority, 2, "priority"),
         SFARRAY16N(vpc.winwidths, 2, "winwidths"),
         SFEND
      };
      ret &= MDFNSS_StateAction(sm, load, data_only, VPC_StateRegs, "VPC", false);
   }

   for (unsigned chip = 0; chip < max_chips; chip++)
   {
      vdc_t *vdc = &vdc_chips[chip];
      SFORMAT VDC_StateRegs[] =
      {
         SFVARN(vdc->display_counter, "display_counter"),
         SFVARN(vdc->sat_dma_slcounter, "sat_dma_slcounter"),

         SFVARN(vdc->select, "select"),
         SFVARN(vdc->MAWR, "MAWR"),
         SFVARN(vdc->MARR, "MARR"),
         SFVARN(vdc->CR, "CR"),
         SFVARN(vdc->RCR, "RCR"),
         SFVARN(vdc->BXR, "BXR"),
         SFVARN(vdc->BYR, "BYR"),
         SFVARN(vdc->MWR, "MWR"),

         SFVARN(vdc->HSR, "HSR"),
         SFVARN(vdc->HDR, "HDR"),
         SFVARN(vdc->VSR, "VSR"),
         SFVARN(vdc->VDR, "VDR"),

         SFVARN(vdc->VCR, "VCR"),
         SFVARN(vdc->DCR, "DCR"),
         SFVARN(vdc->SOUR, "SOUR"),
         SFVARN(vdc->DESR, "DESR"),
         SFVARN(vdc->LENR, "LENR"),
         SFVARN(vdc->SATB, "SATB"),

         SFVARN(vdc->RCRCount, "RCRCount"),

         SFVARN(vdc->read_buffer, "read_buffer"),
         SFVARN(vdc->write_latch, "write_latch"),
         SFVARN(vdc->status, "status"),

         SFARRAY16N(vdc->SAT, 0x100, "SAT"),

         SFARRAY16N(vdc->VRAM, VRAM_Size, "VRAM"),
         SFVARN(vdc->DMAReadBuffer, "DMAReadBuffer"),
         SFVARN(vdc->DMAReadWrite, "DMAReadWrite"),
         SFVARN(vdc->DMARunning, "DMARunning"),
         SFVARN(vdc->SATBPending, "SATBPending"),
         SFVARN(vdc->burst_mode, "burst_mode"),

         SFVARN(vdc->BG_YOffset, "BG_YOffset"),
         SFVARN(vdc->BG_XOffset, "BG_XOffset"),
         SFVAR(frame_counter),
         SFVARN(VDS, "VDS_cache"),
         SFVARN(VSW, "VSW_cache"),
         SFVARN(VDW, "VDW_cache"),
         SFVARN(VCR, "VCR_cache"),
         SFVARN(VBlankFL, "VBlankFL_cache"),
         SFEND
      };

      ret &= MDFNSS_StateAction(sm, load, data_only, VDC_StateRegs, chip ? "VDC1" : "VDC0", false);

      if (load)
      {
         frame_counter %= ((vce.CR & 0x04) ? 263 : 262);
         if (VBlankFL > 261)
            VBlankFL = 261;

         for (int x = 0; x < VRAM_Size; x++)
         {
            FixTileCache(vdc, x);
            vdc->spr_tile_clean[x >> 6] = 0;
         }
         for (int x = 0; x < 512; x++)
            FixPCache(x);
         RebuildSATCache(vdc);
      }
   }

   return (ret);
}
