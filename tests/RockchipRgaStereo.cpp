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

#define LOG_NDEBUG 0
#define LOG_TAG "RockchipRgaStereo"

#include <stdint.h>
#include <sys/types.h>
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>
#include <time.h>

#include <cutils/properties.h>

#include <binder/IPCThreadState.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/DisplayInfo.h>
#include <ui/GraphicBufferMapper.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <RockchipRga.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/eglext.h>

#include <stdint.h>
#include <sys/types.h>

#include <system/window.h>

#include <utils/Thread.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

///////////////////////////////////////////////////////
//#include "../drmrga.h"
#include <hardware/hardware.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/stddef.h>
///////////////////////////////////////////////////////

using namespace android;

int main()
{
    int ret = 0;

    int srcWidth,srcHeight,srcFormat;
    int dstWidth,dstHeight,dstFormat;

    srcWidth = 1920;
    srcHeight = 1088;
    srcFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;

    dstWidth = 3840;
    dstHeight = 1080;
    dstFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    RockchipRga& rkRga(RockchipRga::get());

    GraphicBufferMapper &mgbMapper = GraphicBufferMapper::get();


    char* buf = NULL;
    sp<GraphicBuffer> gbs(new GraphicBuffer(srcWidth, srcHeight, srcFormat,
                          GRALLOC_USAGE_PROTECTED | GRALLOC_USAGE_SW_WRITE_OFTEN));

    if (gbs->initCheck()) {
        printf("GraphicBuffer error : %s\n",strerror(errno));
        return ret;
    } else
        printf("GraphicBuffer ok : %s\n","**************************************");
        

    sp<GraphicBuffer> gbd(new GraphicBuffer(dstWidth, dstHeight, dstFormat,
                          GRALLOC_USAGE_PROTECTED | GRALLOC_USAGE_SW_WRITE_OFTEN));

    if (gbd->initCheck()) {
        printf("GraphicBuffer error : %s\n",strerror(errno));
        return ret;
    } else
        printf("GraphicBuffer ok : %s\n","*************************************");


    mgbMapper.registerBuffer(gbs->handle);
    mgbMapper.registerBuffer(gbd->handle);

    ret = gbs->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)&buf);
    
    if (ret) {
        printf("lock buffer error : %s\n",strerror(errno));
        return ret;
    } else 
        printf("lock buffer ok : %s\n","**************************************");

#if 1
    const char *yuvFilePath = "/data/inputBuffer.bin";
    FILE *file = fopen(yuvFilePath, "rb");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", yuvFilePath);
        return false;
    } else
        fprintf(stderr, "open %s ok\n", yuvFilePath);

    fread(buf, 1.5 * srcWidth * srcHeight, 1, file);
    #if 0
    {
        char *pbuf = (char*)malloc(2 * mHeight * 4864);
        for (int i = 0; i < 2160 * 1.6; i++)
            memcpy(pbuf+i*4800,buf+i*6080,4800);
        const char *outFilePath = "/data/fb3840x2160-2.yuv";
        FILE *file = fopen(outFilePath, "wb+");
        if (!file) {
            fprintf(stderr, "Could not open %s\n", outFilePath);
            return false;
        }
        fwrite(pbuf, 2 * 4864 * 2160, 1, file);
        free(pbuf);
        fclose(file);
    }
    #endif
    fclose(file);
#else
    memset(buf,0x55,4*1200*1920);
#endif
    ret = gbs->unlock();
    ret = gbs->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)&buf);
    if (ret) {
        printf("unlock buffer error : %s\n",strerror(errno));
        return ret;
    } else 
        printf("unlock buffer ok : %s\n","*************************************");

    while(1) {
        
        rkRga.RkRgaSetLogOnceFlag(1);
        drm_rga_t rects;
        /*******************************left***********************************/
        memset(&rects, 0, sizeof(drm_rga_t));
        rga_set_rect(&rects.src, 0, 0, srcWidth, srcHeight, srcWidth, srcFormat);
        rga_set_rect(&rects.dst, 0, 0, dstWidth / 2, dstHeight,
                                                            dstWidth, dstFormat);
        ret = rkRga.RkRgaBlit(gbs->handle, gbd->handle, &rects, 0, 0);

        /*******************************right**********************************/
        rkRga.RkRgaSetLogOnceFlag(1);
        rga_set_rect(&rects.src, 0, 0, srcWidth, srcHeight, srcWidth, srcFormat);
        rga_set_rect(&rects.dst, dstWidth / 2, 0, dstWidth / 2, 
                                                 dstHeight, dstWidth, dstFormat);
        ret = rkRga.RkRgaBlit(gbs->handle, gbd->handle, &rects, 0, 0);


        if (ret) {
            printf("rgaFillColor error : %s,hnd=%p\n",
                                          strerror(errno),(void*)(gbd->handle));
            ALOGD("rgaFillColor error : %s,hnd=%p\n",
                                          strerror(errno),(void*)(gbd->handle));
        }

        {
            char* dstbuf = NULL;
            ret = gbd->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)&dstbuf);
            //for(int i =0; i < mHeight * 1.5; i++)
            //    memcpy(dstbuf + i * 2400,buf + i * 3000,2400);
#if 0
            const char *yuvFilePath = "/data/fb1920x1088.yuv";
            FILE *file = fopen(yuvFilePath, "rb");
            if (!file) {
                fprintf(stderr, "Could not open %s\n", yuvFilePath);
                return false;
            }
            #if 0
            {
                char *pbuf = (char*)malloc(2 * mHeight * 4864);
                fread(pbuf, 2 * 4864 * 2160, 1, file);
                for (int i = 0; i < 2160; i++)
                    memcpy(buf+i*4800,pbuf+i*6080,4800);
                memset(buf+2160*4800,0x80,4800 * 2160);
            }
            #else
            fread(dstbuf, 2 * 1920 * 1088, 1, file);
            #endif
            fclose(file);
#else
            const char *outFilePath = "/data/outBuffer.yuv";
            FILE *file = fopen(outFilePath, "wb+");
            if (!file) {
                fprintf(stderr, "Could not open %s\n", outFilePath);
                return false;
            } else
	            fprintf(stderr, "open %s and write ok\n", outFilePath);
            fwrite(dstbuf, 4 * dstWidth * dstHeight, 1, file);
            fclose(file);
#endif
            ret = gbd->unlock();
        }
        printf("threadloop\n");
        usleep(500000);
	break;
    }
    return 0;
}
