/*
 * Copyright (C) 2016 Rockchip Electronics Co.Ltd
 * Authors:
 *	Zhiqin Wei <wzq@rock-chips.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#ifndef _rockchip_rga_api_
#define _rockchip_rga_api_

#include <stdint.h>
#include <vector>
#include <sys/types.h>

#include <system/window.h>

#include <utils/Thread.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

//////////////////////////////////////////////////////////////////////////////////
#include <hardware/hardware.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/stddef.h>

#include <hardware/rga.h>
#include "stdio.h"

#include "drmrga.h"
//////////////////////////////////////////////////////////////////////////////////

namespace android {
// -------------------------------------------------------------------------------

class RockchipRga :public Singleton<RockchipRga>
{
/************************************public**************************************/

public:

    static inline RockchipRga& get() {return getInstance();}

    int         RkRgaInit();
    int         RkRgaInitTables();

    int         RkRgaGetHandleFd(buffer_handle_t handle, int *fd);
    int         RkRgaGetHandleAttributes(buffer_handle_t handle,
                                                        std::vector<int> *attrs);
    int         RkRgaGetHandleMapAddress(buffer_handle_t handle,
                                                                     void **buf);

    int         RkRgaGetRgaFormat(int format);

    int         RkRgaBlit(buffer_handle_t src, buffer_handle_t dst,
                                      drm_rga_t *rects, int rotation, int blend);
    int         RkRgaBlit(void *src, buffer_handle_t dst,
                                      drm_rga_t *rects, int rotation, int blend);
    int         RkRgaBlit(buffer_handle_t src, void *dst,
                                      drm_rga_t *rects, int rotation, int blend);
    int         RkRgaBlit(void *src, void *dst,
                                      drm_rga_t *rects, int rotation, int blend);
    int         RkRgaPaletteTable(buffer_handle_t dst, 
                                               unsigned int v, drm_rga_t *rects);

    int         RkRgaStereo(buffer_handle_t src,
                                                    buffer_handle_t dst,int div);
    int         RkRgaScale();
    int         RkRgaRoate();
    int         RkRgaRoateScale();
    int         RkRgaGetRects(buffer_handle_t src, buffer_handle_t dst,
                                    int *sType, int *dType, drm_rga_t* tmpRects);
    /*
    @fun RkRgaSetRects:For use to set the rects esayly

    @param rect:The rect user want to set,like setting the src rect:
        drm_rga_t rects;
        RkRgaSetRects(rects.src,0,0,1920,1080,1920,NV12);
        mean to set the src rect to the value.
    */
    int         RkRgaSetRect(rga_rect_t *rect, int x, int y,
                                                   int w, int h, int s, int f);
    void        RkRgaSetLogOnceFlag(int log) {mLogOnce = log;}
    void        RkRgaSetAlwaysLogFlag(bool log) {mLogAlways = log;}
    void        RkRgaLogOutRgaReq(struct rga_req rgaReg);


    enum {
    	AWIDTH                      = 0,
    	AHEIGHT,
    	ASTRIDE,
    	AFORMAT,
    	ASIZE,
    	ATYPE,
    };
/************************************private***********************************/
private:
    int                             rgaFd;
    int                             mLogOnce;
    int                             mLogAlways;
    float                           mVersion;
    static Mutex                    mMutex;
    gralloc_module_t const          *mAllocMod;

    friend class Singleton<RockchipRga>;
                RockchipRga();
                 ~RockchipRga();

/***********************************rgahandle*********************************/
int         RkRgaSetFdsOffsets(struct rga_req *req,
                                uint16_t src_fd,     uint16_t dst_fd,
                                uint32_t src_offset, uint32_t dst_offset);

int         RkRgaSetSrcActiveInfo(struct rga_req *req,
                                unsigned int width, unsigned int height,
                                unsigned int x_off, unsigned int y_off);


#if defined(__arm64__) || defined(__aarch64__)
int         RkRgaSetSrcVirtualInfo(struct rga_req *req,
        	unsigned long yrgb_addr,unsigned long uv_addr,unsigned long v_addr,
            unsigned int vir_w ,unsigned int vir_h, unsigned char format,
                                            unsigned char a_swap_en);
#else
int         RkRgaSetSrcVirtualInfo(struct rga_req *req,
        	unsigned int yrgb_addr, unsigned int uv_addr,unsigned int v_addr,          
        	unsigned int vir_w, unsigned int vir_h, unsigned char format, 
        	                                        unsigned char a_swap_en);
#endif


int         RkRgaSetDstActiveInfo(struct rga_req *req,
                                    unsigned int width, unsigned int height,
                                    unsigned int x_off, unsigned int y_off);


#if defined(__arm64__) || defined(__aarch64__)
int         RkRgaSetDstVirtualInfo(struct rga_req *msg,
        	unsigned long yrgb_addr,unsigned long uv_addr,unsigned long v_addr,    
        	unsigned int  vir_w,    unsigned int vir_h,      
        	RECT          *clip,    unsigned char format, unsigned char a_swap_en);
#else
int         RkRgaSetDstVirtualInfo(struct rga_req *msg,
        	unsigned int yrgb_addr,unsigned int uv_addr,  unsigned int v_addr,     
        	unsigned int vir_w,    unsigned int vir_h,      
        	RECT           *clip,  unsigned char  format, unsigned char a_swap_en);
#endif


int         RkRgaSetPatInfo(struct rga_req *msg,
            unsigned int width,unsigned int height,unsigned int x_off,
                               unsigned int y_off, unsigned int pat_format);


#if defined(__arm64__) || defined(__aarch64__)
int         RkRgaSetRopMaskInfo(struct rga_req *msg,
	        unsigned long rop_mask_addr,unsigned int rop_mask_endian_mode);
#else
int         RkRgaSetRopMaskInfo(struct rga_req *msg,
	        unsigned int rop_mask_addr,unsigned int rop_mask_endian_mode);
#endif


/* 0:alpha' = alpha + (alpha>>7) | alpha' = alpha */
/* 0 global alpha / 1 per pixel alpha / 2 mix mode */

/* porter duff alpha mode en */ 

/* use dst alpha  */

int         RkRgaSetAlphaEnInfo(struct rga_req *msg,
    		unsigned int alpha_cal_mode, unsigned int alpha_mode,        
    		unsigned int global_a_value, unsigned int PD_en,             
    		unsigned int PD_mode,        unsigned int dst_alpha_en );     



int         RkRgaSetRopEnInfo(struct rga_req *msg,
            		unsigned int ROP_mode, unsigned int ROP_code,
            		unsigned int color_mode,unsigned int solid_color);


int         RkRgaSetFadingEnInfo(struct rga_req *msg,
	                unsigned char r,unsigned char g,unsigned char b);


int         RkRgaSetSrcTransModeInfo(struct rga_req *msg,
        	unsigned char trans_mode,unsigned char a_en,unsigned char b_en,
        	unsigned char g_en,unsigned char r_en,unsigned char color_key_min,
        	unsigned char color_key_max,unsigned char zero_mode_en);


// 0/near  1/bilnear  2/bicubic  
// 0/copy 1/rotate_scale 2/x_mirror 3/y_mirror 
// rotate angle     
// dither en flag   
// AA flag          
int         RkRgaSetBitbltMode(struct rga_req *msg,
                	unsigned char scale_mode,  unsigned char rotate_mode, 
                	unsigned int  angle,       unsigned int  dither_en,   
                	unsigned int  AA_en,       unsigned int  yuv2rgb_mode);


/* 1bpp/2bpp/4bpp/8bpp */
/* src endian mode sel */
/* BPP1 = 0 */
/* BPP1 = 1 */
int         RkRgaSetColorPaletteMode(struct rga_req *msg,
            		unsigned char  palette_mode,unsigned char  endian_mode, 
            		unsigned int  bpp1_0_color, unsigned int  bpp1_1_color);


/* gradient color part         */
 /* saturation mode             */
 /* patten fill or solid fill   */
/* solid color                 */
 /* pattern width               */
 /* pattern height              */  
 /* pattern x offset            */
 /* pattern y offset            */
 /* alpha en                    */
int         RkRgaSetColorFillMode(
                struct rga_req *msg,                COLOR_FILL  *gr_color,
                unsigned char  gr_satur_mode,       unsigned char  cf_mode,              
                unsigned int color,                 unsigned short pat_width,
                unsigned short pat_height,          unsigned char pat_x_off,
                unsigned char pat_y_off,            unsigned char aa_en);


/* start point              */
/* end   point              */
/* line point drawing color */
/* line width               */
/* AA en                    */
/* last point en            */
int         RkRgaSetLineDrawingMode(struct rga_req *msg,
        		POINT sp,                     POINT ep,                     
        		unsigned int color,           unsigned int line_width,      
        		unsigned char AA_en,          unsigned char last_point_en);



/* blur/sharpness   */
/* filter intensity */
/* dither_en flag   */

int         RkRgaSetBlurSharpFilterMode(
            		struct rga_req *msg,         unsigned char filter_mode,   
            		unsigned char filter_type,   unsigned char dither_en);


int         RkRgaSetPreScalingMode(
                            struct rga_req *msg, unsigned char dither_en);


/* LUT table addr      */
/* 1bpp/2bpp/4bpp/8bpp */
#if defined(__arm64__) || defined(__aarch64__)
int         RkRgaUpdatePaletteTableMode(
            struct rga_req *msg,unsigned long LUT_addr,unsigned int palette_mode);
#else
int         RkRgaUpdatePaletteTableMode(
            struct rga_req *msg,unsigned int LUT_addr, unsigned int palette_mode);
#endif


/* patten addr    */
/* patten width   */
/* patten height  */
/* patten format  */

int         RkRgaUpdatePattenBuffMode(struct rga_req *msg,
                                unsigned int pat_addr, unsigned int w,        
                                unsigned int h,        unsigned int format);


#if defined(__arm64__) || defined(__aarch64__)
int         RkRgaMmuInfo(struct rga_req *msg,
                    unsigned char  mmu_en,   unsigned char  src_flush,
                    unsigned char  dst_flush,unsigned char  cmd_flush,
                    unsigned long base_addr, unsigned char  page_size);
#else
int         RkRgaMmuInfo(struct rga_req *msg,
            		unsigned char  mmu_en,   unsigned char  src_flush,
            		unsigned char  dst_flush,unsigned char  cmd_flush,
            		unsigned int base_addr,  unsigned char  page_size);
#endif

int         RkRgaMmuFlag(struct rga_req *msg,
                                    int  src_mmu_en,   int  dst_mmu_en);

int         sina_table[360];

int         cosa_table[360];

};

// ---------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_BOOTANIMATION_H

