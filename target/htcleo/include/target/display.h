/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#ifndef _TARGET_QSD8250_SURF_DISPLAY_H
#define _TARGET_QSD8250_SURF_DISPLAY_H

#define TARGET_XRES 480
#define TARGET_YRES 800

#define LCDC_PIXCLK_IN_PS 26
#define LCDC_FB_PHYS      0x16600000
#define LCDC_FB_BPP       16
#define LCDC_FB_WIDTH     480
#define LCDC_FB_HEIGHT    800

#define LCDC_HSYNC_PULSE_WIDTH_DCLK 2
#define LCDC_HSYNC_BACK_PORCH_DCLK  30
#define LCDC_HSYNC_FRONT_PORCH_DCLK 2
#define LCDC_HSYNC_SKEW_DCLK        0

#define LCDC_VSYNC_PULSE_WIDTH_LINES 2
#define LCDC_VSYNC_BACK_PORCH_LINES  5
#define LCDC_VSYNC_FRONT_PORCH_LINES 2

#define LCD_CLK_PCOM_MHZ 40000000

#define DEFAULT_LCD_TIMING 0

/* used for setting custom timing parameters for different panels */
struct lcdc_timing_parameters
{
	unsigned  lcdc_fb_width;
	unsigned  lcdc_fb_height;

	unsigned  lcdc_hsync_pulse_width_dclk;
	unsigned  lcdc_hsync_back_porch_dclk;
	unsigned  lcdc_hsync_front_porch_dclk;
	unsigned  lcdc_hsync_skew_dclk;

	unsigned  lcdc_vsync_pulse_width_lines;
	unsigned  lcdc_vsync_back_porch_lines;
	unsigned  lcdc_vsync_front_porch_lines;
};

#endif
