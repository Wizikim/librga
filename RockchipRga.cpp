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
#define LOG_TAG "rockchiprga"

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
#include <utils/Mutex.h>
#include <utils/Singleton.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/DisplayInfo.h>
#include <ui/GraphicBufferMapper.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include "RockchipRga.h"

namespace android {

// ---------------------------------------------------------------------------
ANDROID_SINGLETON_STATIC_INSTANCE(RockchipRga)

Mutex RockchipRga::mMutex;

RockchipRga::RockchipRga():
    rgaFd(-1),
    mLogOnce(0),
    mLogAlways(0)
{
    RkRgaInit();
}

RockchipRga::~RockchipRga()
{
    if (rgaFd > -1) {
        close(rgaFd);
        rgaFd = -1;
    }
}

int RockchipRga::RkRgaInit()
{
    hw_module_t const* module;
    char buf[256];
    int ret = 0;
    if (rgaFd < 0)
        rgaFd = open("/dev/rga", O_RDWR, 0);

    if (rgaFd < 0) {
        printf("open rga fail\n");
        return -errno;
    }

    ret = ioctl(rgaFd, RGA_GET_VERSION, buf);
    mVersion = atof(buf);
    fprintf(stderr, "librga:RGA_GET_VERSION:%s,%f\n", buf, mVersion);

    ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
    if (ret) {
        printf("%s,%d faile get hw moudle\n",__func__,__LINE__);
        return ret;
    }

    mAllocMod = reinterpret_cast<gralloc_module_t const *>(module);

    printf("librga:load gralloc module success!\n");

    RkRgaInitTables();
    printf("librga:init table success!\n");
    return 0;
}

int RockchipRga::RkRgaGetHandleFd(buffer_handle_t handle, int *fd)
{
    int op = 0x80000001;
    int ret = 0;

    if (mAllocMod->perform)
        mAllocMod->perform(mAllocMod, op, handle, fd);
    else
        return -ENODEV;

    if (ret)
        ALOGE("RockchipRgaGetHandldFd fail %d for:%s",ret,strerror(ret));
    else if (mLogAlways || mLogOnce) {
        ALOGD("fd = %d",*fd);
        fprintf(stderr,"fd = %d\n", *fd);
    }

    return ret;
}

int RockchipRga::RkRgaGetHandleAttributes(buffer_handle_t handle,
                                                        std::vector<int> *attrs)
{
    int op = 0x80000002;
    int ret = 0;
    if (mAllocMod->perform)
        mAllocMod->perform(mAllocMod,op, handle, attrs);
    else
        return -ENODEV;

    if (ret)
        ALOGE("RockchipRgaGetHandldAttributes fail %d for:%s",ret,strerror(ret));
    else if (mLogAlways || mLogOnce) {
        ALOGD("%d,%d,%d,%d,%d,%d",attrs->at(0),attrs->at(1),attrs->at(2),
                                         attrs->at(3),attrs->at(4),attrs->at(5));
        fprintf(stderr,"%d,%d,%d,%d,%d,%d\n",
                               attrs->at(0),attrs->at(1),attrs->at(2),
                                         attrs->at(3),attrs->at(4),attrs->at(5));
    }
    return ret;
}

int RockchipRga::RkRgaGetHandleMapAddress(buffer_handle_t handle,
                                                                     void **buf)
{
    int usage = GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK;
    int ret = mAllocMod->lock(mAllocMod, handle, usage, 0, 0, 0, 0, buf);

    if (ret)
        ALOGE("GetHandleMapAddress fail %d for:%s",ret,strerror(ret));
    else if (mLogAlways || mLogOnce) {
        ALOGD("buf = %p\n", buf);
        fprintf(stderr, "buf = %p", buf);
    }
    return ret;
}

int RockchipRga::RkRgaBlit(buffer_handle_t src,
                 buffer_handle_t dst, drm_rga_t *rects, int rotation, int blend)
{
    Mutex::Autolock lock(mMutex);

    //check rects
    //check buffer_handle_t with rects
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,srcMmuFlag,dstMmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int ret = 0;
    drm_rga_t tmpRects,relRects;
    struct rga_req rgaReg;
    bool perpixelAlpha;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    RECT clip;

    if (rects && (mLogAlways || mLogOnce)) {
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
            rects->src.xoffset,rects->src.yoffset,
            rects->src.width, rects->src.height, 
            rects->src.wstride,rects->src.format, rects->src.size,
            rects->dst.xoffset,rects->dst.yoffset,
            rects->dst.width, rects->dst.height,
            rects->dst.wstride,rects->dst.format, rects->dst.size);
    }

    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;

    ret = RkRgaGetRects(src, dst, &srcType, &dstType, &tmpRects);
    if (ret && !rects) {
        ALOGE("%d:Has not rects for render", __LINE__);
        return ret;
    }

    if (rects) {
        if (rects->src.wstride > 0 && rects->dst.wstride > 0)
            memcpy(&relRects, rects, sizeof(drm_rga_t));
        else if (rects->src.wstride > 0) {
            memcpy(&(relRects.src), &(rects->src), sizeof(rga_rect_t));
            memcpy(&(relRects.dst), &(tmpRects.dst), sizeof(rga_rect_t));
        } else if (rects->dst.wstride > 0) {
            memcpy(&(relRects.src), &(tmpRects.src), sizeof(rga_rect_t));
            memcpy(&(relRects.dst), &(rects->dst), sizeof(rga_rect_t));
        }
    } else
        memcpy(&relRects, &tmpRects, sizeof(drm_rga_t));

    if (mLogAlways || mLogOnce) {
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
            tmpRects.src.xoffset,tmpRects.src.yoffset,
            tmpRects.src.width, tmpRects.src.height, 
            tmpRects.src.wstride,tmpRects.src.format, tmpRects.src.size,
            tmpRects.dst.xoffset,tmpRects.dst.yoffset,
            tmpRects.dst.width, tmpRects.dst.height,
            tmpRects.dst.wstride,tmpRects.dst.format, tmpRects.dst.size);
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
            relRects.src.xoffset,relRects.src.yoffset,
            relRects.src.width, relRects.src.height, 
            relRects.src.wstride,relRects.src.format, relRects.src.size,
            relRects.dst.xoffset,relRects.dst.yoffset,
            relRects.dst.width, relRects.dst.height,
            relRects.dst.wstride,relRects.dst.format, relRects.dst.size);
    }

    RkRgaGetHandleMapAddress(src, &srcBuf);
    RkRgaGetHandleFd(src, &srcFd);
    if (srcFd == -1 && !srcBuf) {
        ALOGE("%d:src has not fd and address for render", __LINE__);
        return ret;
    }

    if (srcFd == 0 && !srcBuf) {
        ALOGE("srcFd is zero, now driver not support");
        return -EINVAL;
    } else
        srcFd = -1;

    RkRgaGetHandleMapAddress(dst, &dstBuf);
    RkRgaGetHandleFd(dst, &dstFd);
    if (dstFd == -1 && !dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    if (dstFd == 0 && !dstBuf) {
        ALOGE("dstFd is zero, now driver not support");
        return -EINVAL;
    } else
        dstFd = -1;

    planeAlpha = (blend & 0xFF0000) >> 16;
    perpixelAlpha = relRects.src.format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    relRects.src.format == HAL_PIXEL_FORMAT_BGRA_8888;

    switch ((blend & 0xFFFF)) {
        case 0x0105:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 1, 9, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 1, 3, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0405:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 0, 0, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 0, 0, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0100:
        default:
            /* Tips: BLENDING_NONE is non-zero value, handle zero value as
             * BLENDING_NONE. */
            /* C = Cs
             * A = As */
            break;
    }

    switch (rotation) {
        case HAL_TRANSFORM_FLIP_H:
            orientation = 0;
            rotateMode = 2;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_FLIP_V:
            orientation = 0;
            rotateMode = 3;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_90:
            orientation = 90;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            //dstYPos = relRects.dst.yoffset;
            dstYPos = 0;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        case HAL_TRANSFORM_ROT_180:
            orientation = 180;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_270:
            orientation = 270;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            //dstXPos = relRects.dst.xoffset;
            dstXPos = 0;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        default:
            orientation = 0;
            rotateMode = 0;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
    }

    clip.xmin = 0;
    clip.xmax = dstActW - 1;
    clip.ymin = 0;
    clip.ymax = dstActH - 1;

    //scale up use bicubic
    if (srcActW / dstActW < 1 || srcActH / dstActH < 1)
        scaleMode = 2;

    /*
    if (scaleMode && (srcFormat == RK_FORMAT_RGBA_8888 ||
                                            srcFormat == RK_FORMAT_BGRA_8888)) {
        scale_mode = 0;     //  force change scale_mode to 0 ,for rga not support
    }*/

    ditherEn = (android::bytesPerPixel(relRects.src.format) 
                        != android::bytesPerPixel(relRects.src.format) ? 1 : 0);

    if (mVersion <= 1.003) {
        srcMmuFlag = dstMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    } else if (mVersion < 2.0) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            RkRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
            RkRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            srcMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#else
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = srcType ? 1 : 0;
            RkRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
    	    /*src dst fd*/
            RkRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            dstMmuFlag = 1;
#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
        }
    } else {
        srcMmuFlag = ((srcFd != -1 && srcType) || srcFd == -1) ? 1 : 0;
        dstMmuFlag = ((dstFd != -1 && dstType) || dstFd == -1) ? 1 : 0;
#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned long)srcBuf, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned int)srcBuf, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    }

    RkRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    RkRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);

    /*mode*/
    RkRgaSetBitbltMode(&rgaReg, scaleMode, rotateMode, orientation, ditherEn, 0, 0);

    if (srcMmuFlag || dstMmuFlag) {
        RkRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        RkRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    if(ioctl(rgaFd, RGA_BLIT_SYNC, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
    }

    if (mLogOnce)
        mLogOnce = 0;

    return 0;
}

int RockchipRga::RkRgaBlit(void *src,
                    buffer_handle_t dst, drm_rga_t *rects, int rotation, int blend)
{
    Mutex::Autolock lock(mMutex);

    //check rects
    //check buffer_handle_t with rects
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,srcMmuFlag,dstMmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int ret = 0;
    drm_rga_t tmpRects,relRects;
    struct rga_req rgaReg;
    bool perpixelAlpha;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    RECT clip;

    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;

    if (!rects) {
        ALOGE("%d:Has not user rects for render", __LINE__);
        return ret;
    }

    if (rects->src.wstride <= 0) {
        ALOGE("%d:Has invalid rects for render", __LINE__);
        return -EINVAL;
    }

    ret = RkRgaGetRects(NULL, dst, &srcType, &dstType, &tmpRects);
    if (ret) {
        ALOGE("%d:Has not rects for render", __LINE__);
        return ret;
    }

    if (rects->dst.wstride > 0)
        memcpy(&relRects, rects, sizeof(drm_rga_t));
    else {
        memcpy(&(relRects.src), &(rects->src), sizeof(rga_rect_t));
        memcpy(&(relRects.dst), &(tmpRects.dst), sizeof(rga_rect_t));
    }

    srcBuf = src;
    if (!srcBuf) {
        ALOGE("%d:src has not fd and address for render", __LINE__);
        return ret;
    }

    RkRgaGetHandleMapAddress(dst, &dstBuf);
    RkRgaGetHandleFd(dst, &dstFd);
    if (dstFd == -1 && !dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    if (dstFd == 0 && !dstBuf) {
        ALOGE("dstFd is zero, now driver not support");
        return -EINVAL;
    } else
        dstFd = -1;

    planeAlpha = (blend & 0xFF0000) >> 16;
    perpixelAlpha = relRects.src.format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    relRects.src.format == HAL_PIXEL_FORMAT_BGRA_8888;

    switch ((blend & 0xFFFF)) {
        case 0x0105:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 1, 9, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 1, 3, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0405:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 0, 0, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 0, 0, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0100:
        default:
            /* Tips: BLENDING_NONE is non-zero value, handle zero value as
             * BLENDING_NONE. */
            /* C = Cs
             * A = As */
            break;
    }


    switch (rotation) {
        case HAL_TRANSFORM_FLIP_H:
            orientation = 0;
            rotateMode = 2;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_FLIP_V:
            orientation = 0;
            rotateMode = 3;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_90:
            orientation = 90;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            //dstYPos = relRects.dst.yoffset;
            dstYPos = 0;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        case HAL_TRANSFORM_ROT_180:
            orientation = 180;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_270:
            orientation = 270;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            //dstXPos = relRects.dst.xoffset;
            dstXPos = 0;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        default:
            orientation = 0;
            rotateMode = 0;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
    }

    clip.xmin = 0;
    clip.xmax = dstActW - 1;
    clip.ymin = 0;
    clip.ymax = dstActH - 1;

    //scale up use bicubic
    if (srcActW / dstActW < 1 || srcActH / dstActH < 1)
        scaleMode = 2;

    /*
    if (scaleMode && (srcFormat == RK_FORMAT_RGBA_8888 ||
                                            srcFormat == RK_FORMAT_BGRA_8888)) {
        scale_mode = 0;     //  force change scale_mode to 0 ,for rga not support
    }*/

    ditherEn = (android::bytesPerPixel(relRects.src.format) 
                        != android::bytesPerPixel(relRects.src.format) ? 1 : 0);

    if (mVersion <= 1.003) {
        srcMmuFlag = dstMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    } else if (mVersion < 2.0) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            RkRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
            RkRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            srcMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#else
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = srcType ? 1 : 0;
            RkRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
    	    /*src dst fd*/
            RkRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            dstMmuFlag = 1;
#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
        }
    } else {
        srcMmuFlag = ((srcFd != -1 && srcType) || srcFd == -1) ? 1 : 0;
        dstMmuFlag = ((dstFd != -1 && dstType) || dstFd == -1) ? 1 : 0;
#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned long)srcBuf, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned int)srcBuf, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    }

    RkRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    RkRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);

    /*mode*/
    RkRgaSetBitbltMode(&rgaReg, scaleMode, rotateMode, orientation, ditherEn, 0, 0);

    if (srcMmuFlag || dstMmuFlag) {
        RkRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        RkRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    if(ioctl(rgaFd, RGA_BLIT_SYNC, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
    }

    return 0;
}

int RockchipRga::RkRgaBlit(buffer_handle_t src,
                            void *dst, drm_rga_t *rects, int rotation, int blend)
{
    Mutex::Autolock lock(mMutex);

    //check rects
    //check buffer_handle_t with rects
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,srcMmuFlag,dstMmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int ret = 0;
    drm_rga_t tmpRects,relRects;
    struct rga_req rgaReg;
    bool perpixelAlpha;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    RECT clip;

    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;

    if (!rects) {
        ALOGE("%d:Has not user rects for render", __LINE__);
        return ret;
    }

    if (rects->dst.wstride <= 0) {
        ALOGE("%d:Has invalid rects for render", __LINE__);
        return -EINVAL;
    }

    ret = RkRgaGetRects(src, NULL, &srcType, &dstType, &tmpRects);
    if (ret) {
        ALOGE("%d:Has not rects for render", __LINE__);
        return ret;
    }

    if (rects->src.wstride > 0)
        memcpy(&relRects, rects, sizeof(drm_rga_t));
    else {
        memcpy(&(relRects.src), &(tmpRects.src), sizeof(rga_rect_t));
        memcpy(&(relRects.dst), &(rects->dst), sizeof(rga_rect_t));
    }

    RkRgaGetHandleMapAddress(src, &srcBuf);
    RkRgaGetHandleFd(src, &srcFd);
    if (srcFd == -1 && !srcBuf) {
        ALOGE("%d:src has not fd and address for render", __LINE__);
        return ret;
    }

    if (srcFd == 0 && !srcBuf) {
        ALOGE("srcFd is zero, now driver not support");
        return -EINVAL;
    } else
        srcFd = -1;

    dstBuf = dst;
    if (!dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    planeAlpha = (blend & 0xFF0000) >> 16;
    perpixelAlpha = relRects.src.format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    relRects.src.format == HAL_PIXEL_FORMAT_BGRA_8888;

    switch ((blend & 0xFFFF)) {
        case 0x0105:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 1, 9, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 1, 3, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0405:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 0, 0, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 0, 0, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0100:
        default:
            /* Tips: BLENDING_NONE is non-zero value, handle zero value as
             * BLENDING_NONE. */
            /* C = Cs
             * A = As */
            break;
    }


    switch (rotation) {
        case HAL_TRANSFORM_FLIP_H:
            orientation = 0;
            rotateMode = 2;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_FLIP_V:
            orientation = 0;
            rotateMode = 3;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_90:
            orientation = 90;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            //dstYPos = relRects.dst.yoffset;
            dstYPos = 0;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        case HAL_TRANSFORM_ROT_180:
            orientation = 180;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_270:
            orientation = 270;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            //dstXPos = relRects.dst.xoffset;
            dstXPos = 0;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        default:
            orientation = 0;
            rotateMode = 0;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
    }

    clip.xmin = 0;
    clip.xmax = dstActW - 1;
    clip.ymin = 0;
    clip.ymax = dstActH - 1;

    //scale up use bicubic
    if (srcActW / dstActW < 1 || srcActH / dstActH < 1)
        scaleMode = 2;

    /*
    if (scaleMode && (srcFormat == RK_FORMAT_RGBA_8888 ||
                                            srcFormat == RK_FORMAT_BGRA_8888)) {
        scale_mode = 0;     //  force change scale_mode to 0 ,for rga not support
    }*/

    ditherEn = (android::bytesPerPixel(relRects.src.format) 
                        != android::bytesPerPixel(relRects.src.format) ? 1 : 0);

    if (mVersion <= 1.003) {
        srcMmuFlag = dstMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    } else if (mVersion < 2.0) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            RkRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
            RkRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            srcMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#else
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = srcType ? 1 : 0;
            RkRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
    	    /*src dst fd*/
            RkRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            dstMmuFlag = 1;
#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
        }
    } else {
        srcMmuFlag = ((srcFd != -1 && srcType) || srcFd == -1) ? 1 : 0;
        dstMmuFlag = ((dstFd != -1 && dstType) || dstFd == -1) ? 1 : 0;
#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned long)srcBuf, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned int)srcBuf, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    }

    RkRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    RkRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);

    /*mode*/
    RkRgaSetBitbltMode(&rgaReg, scaleMode, rotateMode, orientation, ditherEn, 0, 0);

    if (srcMmuFlag || dstMmuFlag) {
        RkRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        RkRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    if(ioctl(rgaFd, RGA_BLIT_SYNC, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
    }

    return 0;
}

int RockchipRga::RkRgaBlit(void *src, void *dst,
                                      drm_rga_t *rects, int rotation, int blend)
{
    Mutex::Autolock lock(mMutex);

    //check rects
    //check buffer_handle_t with rects
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,srcMmuFlag,dstMmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int ret = 0;
    drm_rga_t relRects;
    struct rga_req rgaReg;
    bool perpixelAlpha;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    RECT clip;

    if (rects && (mLogAlways || mLogOnce)) {
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
            rects->src.xoffset,rects->src.yoffset,
            rects->src.width, rects->src.height,
            rects->src.wstride,rects->src.format, rects->src.size,
            rects->dst.xoffset,rects->dst.yoffset,
            rects->dst.width, rects->dst.height,
            rects->dst.wstride,rects->dst.format, rects->dst.size);
    }

    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;

    if (!rects) {
        ALOGE("%d:Has not user rects for render", __LINE__);
        return ret;
    }

    if (rects->src.wstride <= 0 || rects->dst.wstride <= 0) {
        ALOGE("%d:Has invalid rects for render", __LINE__);
        return -EINVAL;
    }

    memcpy(&relRects, rects, sizeof(drm_rga_t));

    srcBuf = src;
    if (!srcBuf) {
        ALOGE("%d:src has not fd and address for render", __LINE__);
        return ret;
    }

    dstBuf = dst;
    if (!dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    planeAlpha = (blend & 0xFF0000) >> 16;
    perpixelAlpha = relRects.src.format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    relRects.src.format == HAL_PIXEL_FORMAT_BGRA_8888;

    switch ((blend & 0xFFFF)) {
        case 0x0105:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 1, 9, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 1, 3, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0405:
            if (perpixelAlpha && planeAlpha < 255)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha , 0, 0, 0);
            else if (perpixelAlpha)
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 0, 0, 0);
            else
                RkRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha , 0, 0, 0);
            break;

        case 0x0100:
        default:
            /* Tips: BLENDING_NONE is non-zero value, handle zero value as
             * BLENDING_NONE. */
            /* C = Cs
             * A = As */
            break;
    }


    switch (rotation) {
        case HAL_TRANSFORM_FLIP_H:
            orientation = 0;
            rotateMode = 2;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_FLIP_V:
            orientation = 0;
            rotateMode = 3;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_90:
            orientation = 90;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            //dstYPos = relRects.dst.yoffset;
            dstYPos = 0;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        case HAL_TRANSFORM_ROT_180:
            orientation = 180;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.width - 1;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
        case HAL_TRANSFORM_ROT_270:
            orientation = 270;
            rotateMode = 1;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            //dstXPos = relRects.dst.xoffset;
            dstXPos = 0;
            dstYPos = relRects.dst.height - 1;
            dstActW = relRects.dst.height;
            dstActH = relRects.dst.width;
            break;
        default:
            orientation = 0;
            rotateMode = 0;
            srcVirW = relRects.src.wstride;
            srcVirH = relRects.src.height;
            srcXPos = relRects.src.xoffset;
            srcYPos = relRects.src.yoffset;
            srcActW = relRects.src.width;
            srcActH = relRects.src.height;

            dstVirW = relRects.dst.wstride;
            dstVirH = relRects.dst.height;
            dstXPos = relRects.dst.xoffset;
            dstYPos = relRects.dst.yoffset;
            dstActW = relRects.dst.width;
            dstActH = relRects.dst.height;
            break;
    }

    clip.xmin = 0;
    clip.xmax = dstActW - 1;
    clip.ymin = 0;
    clip.ymax = dstActH - 1;

    //scale up use bicubic
    if (srcActW / dstActW < 1 || srcActH / dstActH < 1)
        scaleMode = 2;

    /*
    if (scaleMode && (srcFormat == RK_FORMAT_RGBA_8888 ||
                                            srcFormat == RK_FORMAT_BGRA_8888)) {
        scale_mode = 0;     //  force change scale_mode to 0 ,for rga not support
    }*/

    ditherEn = (android::bytesPerPixel(relRects.src.format) 
                        != android::bytesPerPixel(relRects.src.format) ? 1 : 0);

    if (mVersion <= 1.003) {
        srcMmuFlag = dstMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    } else if (mVersion < 2.0) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            RkRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
            RkRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            srcMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                        (unsigned long)srcBuf + srcVirW * srcVirH, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#else
            RkRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                        (unsigned int)srcBuf + srcVirW * srcVirH, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH * 1.25,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = srcType ? 1 : 0;
            RkRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
    	    /*src dst fd*/
            RkRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            dstMmuFlag = 1;
#if defined(__arm64__) || defined(__aarch64__)
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        (unsigned long)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
            RkRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        (unsigned int)dstBuf + dstVirW * dstVirH * 1.25,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
        }
    } else {
        srcMmuFlag = ((srcFd != -1 && srcType) || srcFd == -1) ? 1 : 0;
        dstMmuFlag = ((dstFd != -1 && dstType) || dstFd == -1) ? 1 : 0;
#if defined(__arm64__) || defined(__aarch64__)
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned long)srcBuf, 
                                        (unsigned long)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned long)dstBuf,
                                        (unsigned long)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#else
        RkRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                        (unsigned int)srcBuf, 
                                        (unsigned int)srcBuf + srcVirW * srcVirH,
                                        srcVirW, srcVirH,
                                        RkRgaGetRgaFormat(relRects.src.format),0);
        /*dst*/
        RkRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                        (unsigned int)dstBuf,
                                        (unsigned int)dstBuf + dstVirW * dstVirH,
                                        dstVirW, dstVirH, &clip,
                                        RkRgaGetRgaFormat(relRects.dst.format),0);
#endif
    }

    RkRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    RkRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);

    /*mode*/
    RkRgaSetBitbltMode(&rgaReg, scaleMode, rotateMode, orientation, ditherEn, 0, 0);

    if (srcMmuFlag || dstMmuFlag) {
        RkRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        RkRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    if(ioctl(rgaFd, RGA_BLIT_SYNC, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
    }

    if (mLogOnce)
        mLogOnce = 0;

    return 0;
}

int RockchipRga::RkRgaScale()
{
    return 1;
}

int RockchipRga::RkRgaRoate()
{
    return 1;
}

int RockchipRga::RkRgaRoateScale()
{
    return 1;
}

/**********************************************************************
=======================================================================
**********************************************************************/
int RockchipRga::RkRgaGetRects(buffer_handle_t src,
          buffer_handle_t dst,int* sType,int* dType,drm_rga_t* tmpRects)
{
    int ret = 0;
    std::vector<int> srcAttrs,dstAttrs;
    if (src)
        ret = RkRgaGetHandleAttributes(src, &srcAttrs);
    if (ret) {
        ALOGE("dst handle get Attributes fail ret = %d,hnd=%p",ret,&src);
        printf("dst handle get Attributes fail ret = %d,hnd=%p",ret,&src);
        return ret;
    }

    if (dst)
        ret = RkRgaGetHandleAttributes(dst, &dstAttrs);
    if (ret) {
        ALOGE("dst handle get Attributes fail ret = %d,hnd=%p",ret,&dst);
        printf("dst handle get Attributes fail ret = %d,hnd=%p",ret,&dst);
        return ret;
    }

    memset(tmpRects,0,sizeof(drm_rga_t));

    if (src) {
        tmpRects->src.size = srcAttrs.at(ASIZE);
        tmpRects->src.width   = srcAttrs.at(AWIDTH);
        tmpRects->src.height  = srcAttrs.at(AHEIGHT);
        tmpRects->src.wstride = srcAttrs.at(ASTRIDE);
        tmpRects->src.format  = srcAttrs.at(AFORMAT);
        if (sType)
            *sType = srcAttrs.at(ATYPE);
    }

    if (dst) {
        tmpRects->dst.size = dstAttrs.at(ASIZE);
        tmpRects->dst.width   = dstAttrs.at(AWIDTH);
        tmpRects->dst.height  = dstAttrs.at(AHEIGHT);
        tmpRects->dst.wstride = dstAttrs.at(ASTRIDE);
        tmpRects->dst.format  = dstAttrs.at(AFORMAT);
        if (dType)
            *dType = dstAttrs.at(ATYPE);
    }

    return ret;
}

int RockchipRga::RkRgaSetRect(rga_rect_t *rect, int x, int y,
                                            int w, int h, int s, int f)
{
    if (!rect)
        return -EINVAL;

    rect->xoffset = x;
    rect->yoffset = y;
    rect->width = w;
    rect->height = h;
    rect->wstride = s;
    rect->format = f;

    return 0;
}

int RockchipRga::RkRgaGetRgaFormat(int format)
{
    switch (format)
    {
        case HAL_PIXEL_FORMAT_RGB_565:
            return RK_FORMAT_RGB_565;
        case HAL_PIXEL_FORMAT_RGB_888:
            return RK_FORMAT_RGB_888;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            return RK_FORMAT_RGBA_8888;
        case HAL_PIXEL_FORMAT_RGBX_8888:
            return RK_FORMAT_RGBX_8888;
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return RK_FORMAT_BGRA_8888;
        case HAL_PIXEL_FORMAT_YCrCb_NV12:
            return RK_FORMAT_YCbCr_420_SP;
    	case HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO:
    	    return RK_FORMAT_YCbCr_420_SP;
#ifdef HAL_PIXEL_FORMAT_YCrCb_NV12_10
        case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
            return RK_FORMAT_YCbCr_420_SP_10B;
#endif
        default:
            return -1;
    }
}

int RockchipRga::RkRgaSetSrcActiveInfo(struct rga_req *req,
                                    unsigned int width, unsigned int height,
                                    unsigned int x_off, unsigned int y_off)
{
    req->src.act_w = width;
    req->src.act_h = height;
    req->src.x_offset = x_off;
    req->src.y_offset = y_off;
    
    return 1;
}

int RockchipRga::RkRgaSetFdsOffsets(struct rga_req *req,
                                uint16_t src_fd,     uint16_t dst_fd,
                                uint32_t src_offset, uint32_t dst_offset)
{
	req->line_draw_info.color = src_fd | (dst_fd << 16);
	req->line_draw_info.flag = src_offset;
	req->line_draw_info.line_width = dst_offset;
	return 0;
}

#if defined(__arm64__) || defined(__aarch64__)
int RockchipRga::RkRgaSetSrcVirtualInfo(struct rga_req *req,
		unsigned long yrgb_addr,unsigned long uv_addr,unsigned long v_addr,
        unsigned int vir_w ,unsigned int vir_h, unsigned char format,
                                                unsigned char a_swap_en)
#else
int RockchipRga::RkRgaSetSrcVirtualInfo(struct rga_req *req,
		unsigned int yrgb_addr, unsigned int uv_addr,unsigned int v_addr,          
		unsigned int vir_w, unsigned int vir_h, unsigned char format, 
		                                        unsigned char a_swap_en)
#endif
{
    req->src.yrgb_addr = yrgb_addr;
    req->src.uv_addr  = uv_addr;
    req->src.v_addr   = v_addr;
    req->src.vir_w = vir_w;
    req->src.vir_h = vir_h;
    req->src.format = format;
    req->src.alpha_swap |= (a_swap_en & 1);

    return 1;
}

int RockchipRga::RkRgaSetDstActiveInfo(struct rga_req *req,
                                unsigned int width, unsigned int height,
                                unsigned int x_off, unsigned int y_off)
{
    req->dst.act_w = width;
    req->dst.act_h = height;
    req->dst.x_offset = x_off;
    req->dst.y_offset = y_off;
    
    return 1;
}

#if defined(__arm64__) || defined(__aarch64__)
int RockchipRga::RkRgaSetDstVirtualInfo(struct rga_req *msg,
		unsigned long yrgb_addr,unsigned long uv_addr,unsigned long v_addr,    
		unsigned int  vir_w,    unsigned int vir_h,
		RECT          *clip,    unsigned char format, unsigned char a_swap_en)
#else
int RockchipRga::RkRgaSetDstVirtualInfo(struct rga_req *msg,
		unsigned int yrgb_addr,unsigned int uv_addr,  unsigned int v_addr, 
		unsigned int vir_w,    unsigned int vir_h,
		RECT           *clip,  unsigned char  format, unsigned char a_swap_en)
#endif
{
    msg->dst.yrgb_addr = yrgb_addr;
    msg->dst.uv_addr  = uv_addr;
    msg->dst.v_addr   = v_addr;
    msg->dst.vir_w = vir_w;
    msg->dst.vir_h = vir_h;
    msg->dst.format = format;

    msg->clip.xmin = clip->xmin;
    msg->clip.xmax = clip->xmax;
    msg->clip.ymin = clip->ymin;
    msg->clip.ymax = clip->ymax;

    msg->dst.alpha_swap |= (a_swap_en & 1);

    return 1;
}

int RockchipRga::RkRgaSetPatInfo(struct rga_req *msg,
            unsigned int width,unsigned int height,unsigned int x_off,
                               unsigned int y_off, unsigned int pat_format)
{
    msg->pat.act_w = width;
    msg->pat.act_h = height;
    msg->pat.x_offset = x_off;
    msg->pat.y_offset = y_off;

    msg->pat.format = pat_format;

    return 1;
}

#if defined(__arm64__) || defined(__aarch64__)
int RockchipRga::RkRgaSetRopMaskInfo(struct rga_req *msg,
		unsigned long rop_mask_addr,unsigned int rop_mask_endian_mode)
#else
int RockchipRga::RkRgaSetRopMaskInfo(struct rga_req *msg,
		unsigned int rop_mask_addr,unsigned int rop_mask_endian_mode)
#endif
{
    msg->rop_mask_addr = rop_mask_addr;
    msg->endian_mode = rop_mask_endian_mode;
    return 1;
}

/* 0:alpha' = alpha + (alpha>>7) | alpha' = alpha */
/* 0 global alpha / 1 per pixel alpha / 2 mix mode */

/* porter duff alpha mode en */ 

/* use dst alpha  */

int RockchipRga::RkRgaSetAlphaEnInfo(struct rga_req *msg,
        		unsigned int alpha_cal_mode, unsigned int alpha_mode,        
        		unsigned int global_a_value, unsigned int PD_en,             
        		unsigned int PD_mode,        unsigned int dst_alpha_en )     
{
    msg->alpha_rop_flag |= 1;
    msg->alpha_rop_flag |= ((PD_en & 1) << 3);
    msg->alpha_rop_flag |= ((alpha_cal_mode & 1) << 4);

    msg->alpha_global_value = global_a_value;
    msg->alpha_rop_mode |= (alpha_mode & 3);    
    msg->alpha_rop_mode |= (dst_alpha_en << 5);

    msg->PD_mode = PD_mode;
    
    
    return 1;
}


int RockchipRga::RkRgaSetRopEnInfo(struct rga_req *msg,
                		unsigned int ROP_mode, unsigned int ROP_code,
                		unsigned int color_mode,unsigned int solid_color)
{
    msg->alpha_rop_flag |= (0x3);
    msg->alpha_rop_mode |= ((ROP_mode & 3) << 2);
      
    msg->rop_code = ROP_code;
    msg->color_fill_mode = color_mode;
    msg->fg_color = solid_color;
    return 1;
}

int RockchipRga::RkRgaSetFadingEnInfo(struct rga_req *msg,
		                unsigned char r,unsigned char g,unsigned char b)
{
    msg->alpha_rop_flag |= (0x1 << 2);

    msg->fading.b = b;
    msg->fading.g = g;
    msg->fading.r = r;
    return 1;
}

int RockchipRga::RkRgaSetSrcTransModeInfo(struct rga_req *msg,
		unsigned char trans_mode,unsigned char a_en,unsigned char b_en,
		unsigned char g_en,unsigned char r_en,unsigned char color_key_min,
		unsigned char color_key_max,unsigned char zero_mode_en
		)
{
    msg->src_trans_mode = ((a_en & 1) << 4) | ((b_en & 1) << 3) | 
                    ((g_en & 1) << 2) | ((r_en & 1) << 1) | (trans_mode & 1);
    
    msg->color_key_min = color_key_min;
    msg->color_key_max = color_key_max;
    msg->alpha_rop_mode |= (zero_mode_en << 4);
    return 1;
}

// 0/near  1/bilnear  2/bicubic  
// 0/copy 1/rotate_scale 2/x_mirror 3/y_mirror 
// rotate angle     
// dither en flag   
// AA flag          
int RockchipRga::RkRgaSetBitbltMode(struct rga_req *msg,
		unsigned char scale_mode,  unsigned char rotate_mode, 
		unsigned int  angle,       unsigned int  dither_en,   
		unsigned int  AA_en,       unsigned int  yuv2rgb_mode)
{
    unsigned int alpha_mode;
    msg->render_mode = bitblt_mode;

    if(((msg->src.act_w >> 1) > msg->dst.act_w) || ((msg->src.act_h >> 1) > msg->dst.act_h))
        return -1;
    
    msg->scale_mode = scale_mode;
    msg->rotate_mode = rotate_mode;
    
    msg->sina = sina_table[angle];
    msg->cosa = cosa_table[angle];

    msg->yuv2rgb_mode = yuv2rgb_mode;

    msg->alpha_rop_flag |= ((AA_en << 7) & 0x80);

    alpha_mode = msg->alpha_rop_mode & 3;
    if(rotate_mode == BB_ROTATE)
    {
        if (AA_en == ENABLE) 
        {   
            if ((msg->alpha_rop_flag & 0x3) == 0x1)
            {
                if (alpha_mode == 0)
                {
                msg->alpha_rop_mode = 0x2;            
                }
                else if (alpha_mode == 1)
                {
                    msg->alpha_rop_mode = 0x1;
                }
            }
            else
            {
                msg->alpha_rop_flag |= 1;
                msg->alpha_rop_mode = 1;
            }                        
        }        
    }
   
    if (msg->src_trans_mode)
        msg->scale_mode = 0;

    msg->alpha_rop_flag |= (dither_en << 5);
    
    return 0;
}

/* 1bpp/2bpp/4bpp/8bpp */
/* src endian mode sel */
/* BPP1 = 0 */
/* BPP1 = 1 */
int RockchipRga::RkRgaSetColorPaletteMode(struct rga_req *msg,
                		unsigned char  palette_mode,unsigned char  endian_mode, 
                		unsigned int  bpp1_0_color, unsigned int  bpp1_1_color)
{
    msg->render_mode = color_palette_mode;
    
    msg->palette_mode = palette_mode;
    msg->endian_mode = endian_mode;
    msg->fg_color = bpp1_0_color;
    msg->bg_color = bpp1_1_color;
    
    return 1;
}

/* gradient color part         */
     /* saturation mode             */
     /* patten fill or solid fill   */
   /* solid color                 */
     /* pattern width               */
     /* pattern height              */  
     /* pattern x offset            */
     /* pattern y offset            */
     /* alpha en                    */
int RockchipRga::RkRgaSetColorFillMode(
            struct rga_req *msg,                COLOR_FILL  *gr_color,
            unsigned char  gr_satur_mode,       unsigned char  cf_mode,              
            unsigned int color,                 unsigned short pat_width,
            unsigned short pat_height,          unsigned char pat_x_off,
            unsigned char pat_y_off,            unsigned char aa_en)
{
    msg->render_mode = color_fill_mode;

    msg->gr_color.gr_x_a = ((int)(gr_color->gr_x_a * 256.0))& 0xffff;
    msg->gr_color.gr_x_b = ((int)(gr_color->gr_x_b * 256.0))& 0xffff;
    msg->gr_color.gr_x_g = ((int)(gr_color->gr_x_g * 256.0))& 0xffff;
    msg->gr_color.gr_x_r = ((int)(gr_color->gr_x_r * 256.0))& 0xffff;

    msg->gr_color.gr_y_a = ((int)(gr_color->gr_y_a * 256.0))& 0xffff;
    msg->gr_color.gr_y_b = ((int)(gr_color->gr_y_b * 256.0))& 0xffff;
    msg->gr_color.gr_y_g = ((int)(gr_color->gr_y_g * 256.0))& 0xffff;
    msg->gr_color.gr_y_r = ((int)(gr_color->gr_y_r * 256.0))& 0xffff;

    msg->color_fill_mode = cf_mode;
    
    msg->pat.act_w = pat_width;
    msg->pat.act_h = pat_height;

    msg->pat.x_offset = pat_x_off;
    msg->pat.y_offset = pat_y_off;

    msg->fg_color = color;

    msg->alpha_rop_flag |= ((gr_satur_mode & 1) << 6);
    
    if(aa_en)
    {
    	msg->alpha_rop_flag |= 0x1;
    	msg->alpha_rop_mode  = 1;
    }
    return 1;
}

/* start point              */
/* end   point              */
/* line point drawing color */
/* line width               */
/* AA en                    */
/* last point en            */
int RockchipRga::RkRgaSetLineDrawingMode(struct rga_req *msg,
            		POINT sp,                     POINT ep,                     
            		unsigned int color,           unsigned int line_width,      
            		unsigned char AA_en,          unsigned char last_point_en)

{
    msg->render_mode = line_point_drawing_mode;

    msg->line_draw_info.start_point.x = sp.x;
    msg->line_draw_info.start_point.y = sp.y;
    msg->line_draw_info.end_point.x = ep.x;
    msg->line_draw_info.end_point.y = ep.y;

    msg->line_draw_info.color = color;
    msg->line_draw_info.line_width = line_width;
    msg->line_draw_info.flag |= (AA_en & 1);
    msg->line_draw_info.flag |= ((last_point_en & 1) << 1);
    
    if (AA_en == 1)
    {
        msg->alpha_rop_flag = 1;
        msg->alpha_rop_mode = 0x1;
    }
    
    return 1;
}

/* blur/sharpness   */
/* filter intensity */
/* dither_en flag   */

int RockchipRga::RkRgaSetBlurSharpFilterMode(
        		struct rga_req *msg,         unsigned char filter_mode,   
        		unsigned char filter_type,   unsigned char dither_en)
{
    msg->render_mode = blur_sharp_filter_mode;
    
    msg->bsfilter_flag |= (filter_type & 3);
    msg->bsfilter_flag |= ((filter_mode & 1) << 2);
    msg->alpha_rop_flag |= ((dither_en & 1) << 5);
    return 1;
}

int RockchipRga::RkRgaSetPreScalingMode(
                            struct rga_req *msg, unsigned char dither_en)
{
    msg->render_mode = pre_scaling_mode;
    
    msg->alpha_rop_flag |= ((dither_en & 1) << 5);
    return 1;
}

/* LUT table addr      */
/* 1bpp/2bpp/4bpp/8bpp */
#if defined(__arm64__) || defined(__aarch64__)
int RockchipRga::RkRgaUpdatePaletteTableMode(
    struct rga_req *msg,unsigned long LUT_addr,unsigned int palette_mode)
#else
int RockchipRga::RkRgaUpdatePaletteTableMode(
    struct rga_req *msg,unsigned int LUT_addr, unsigned int palette_mode)
#endif
{
    msg->render_mode = update_palette_table_mode;
    
    msg->LUT_addr = LUT_addr;
    msg->palette_mode = palette_mode;
    return 1;
}

/* patten addr    */
/* patten width   */
/* patten height  */
/* patten format  */

int RockchipRga::RkRgaUpdatePattenBuffMode(struct rga_req *msg,
                                    unsigned int pat_addr, unsigned int w,        
                                    unsigned int h,        unsigned int format)
{
    msg->render_mode = update_patten_buff_mode;
    
    msg->pat.yrgb_addr   = pat_addr;
    msg->pat.act_w  = w*h;   // hxx
    msg->pat.act_h  = 1;     // hxx
    msg->pat.format = format;    
    return 1;
}

#if defined(__arm64__) || defined(__aarch64__)
int RockchipRga::RkRgaMmuInfo(struct rga_req *msg,
                        unsigned char  mmu_en,   unsigned char  src_flush,
                        unsigned char  dst_flush,unsigned char  cmd_flush,
                        unsigned long base_addr, unsigned char  page_size)
#else
int RockchipRga::RkRgaMmuInfo(struct rga_req *msg,
                		unsigned char  mmu_en,   unsigned char  src_flush,
                		unsigned char  dst_flush,unsigned char  cmd_flush,
                		unsigned int base_addr,  unsigned char  page_size)
#endif
{
    msg->mmu_info.mmu_en    = mmu_en;
    msg->mmu_info.base_addr = base_addr;
    msg->mmu_info.mmu_flag  = ((page_size & 0x3) << 4) |
                              ((cmd_flush & 0x1) << 3) |
                              ((dst_flush & 0x1) << 2) | 
                              ((src_flush & 0x1) << 1) | mmu_en;
    return 1;
}

int RockchipRga::RkRgaMmuFlag(struct rga_req *msg,
                                    int  src_mmu_en,   int  dst_mmu_en)
{
    if (src_mmu_en || dst_mmu_en)
        msg->mmu_info.mmu_flag |= (0x1 << 31);

    if (src_mmu_en)
        msg->mmu_info.mmu_flag |= (0x1 << 8);

    if (dst_mmu_en)
        msg->mmu_info.mmu_flag |= (0x1 << 10);

    return 1;
}

int RockchipRga::RkRgaInitTables()
{
    int sinaTable[360] = {
            0,   1144,   2287,   3430,   4572,   5712,   6850,   7987,   9121,  10252,
        11380,  12505,  13626,  14742,  15855,  16962,  18064,  19161,  20252,  21336,
        22415,  23486,  24550,  25607,  26656,  27697,  28729,  29753,  30767,  31772,
        32768,  33754,  34729,  35693,  36647,  37590,  38521,  39441,  40348,  41243,
        42126,  42995,  43852,  44695,  45525,  46341,  47143,  47930,  48703,  49461,
        50203,  50931,  51643,  52339,  53020,  53684,  54332,  54963,  55578,  56175,
        56756,  57319,  57865,  58393,  58903,  59396,  59870,  60326,  60764,  61183,
        61584,  61966,  62328,  62672,  62997,  63303,  63589,  63856,  64104,  64332,
        64540,  64729,  64898,  65048,  65177,  65287,  65376,  65446,  65496,  65526,
        65536,  65526,  65496,  65446,  65376,  65287,  65177,  65048,  64898,  64729,
        64540,  64332,  64104,  63856,  63589,  63303,  62997,  62672,  62328,  61966,
        61584,  61183,  60764,  60326,  59870,  59396,  58903,  58393,  57865,  57319,
        56756,  56175,  55578,  54963,  54332,  53684,  53020,  52339,  51643,  50931,
        50203,  49461,  48703,  47930,  47143,  46341,  45525,  44695,  43852,  42995,
        42126,  41243,  40348,  39441,  38521,  37590,  36647,  35693,  34729,  33754,
        32768,  31772,  30767,  29753,  28729,  27697,  26656,  25607,  24550,  23486,
        22415,  21336,  20252,  19161,  18064,  16962,  15855,  14742,  13626,  12505,
        11380,  10252,   9121,   7987,   6850,   5712,   4572,   3430,   2287,   1144,
            0,  -1144,  -2287,  -3430,  -4572,  -5712,  -6850,  -7987,  -9121, -10252,
        -11380, -12505, -13626, -14742, -15855, -16962, -18064, -19161, -20252, -21336,
        -22415, -23486, -24550, -25607, -26656, -27697, -28729, -29753, -30767, -31772,
        -32768, -33754, -34729, -35693, -36647, -37590, -38521, -39441, -40348, -41243,
        -42126, -42995, -43852, -44695, -45525, -46341, -47143, -47930, -48703, -49461,
        -50203, -50931, -51643, -52339, -53020, -53684, -54332, -54963, -55578, -56175,
        -56756, -57319, -57865, -58393, -58903, -59396, -59870, -60326, -60764, -61183,
        -61584, -61966, -62328, -62672, -62997, -63303, -63589, -63856, -64104, -64332,
        -64540, -64729, -64898, -65048, -65177, -65287, -65376, -65446, -65496, -65526,
        -65536, -65526, -65496, -65446, -65376, -65287, -65177, -65048, -64898, -64729, 
        -64540, -64332, -64104, -63856, -63589, -63303, -62997, -62672, -62328, -61966,
        -61584, -61183, -60764, -60326, -59870, -59396, -58903, -58393, -57865, -57319,
        -56756, -56175, -55578, -54963, -54332, -53684, -53020, -52339, -51643, -50931,
        -50203, -49461, -48703, -47930, -47143, -46341, -45525, -44695, -43852, -42995,
        -42126, -41243, -40348, -39441, -38521, -37590, -36647, -35693, -34729, -33754,
        -32768, -31772, -30767, -29753, -28729, -27697, -26656, -25607, -24550, -23486, 
        -22415, -21336, -20252, -19161, -18064, -16962, -15855, -14742, -13626, -12505,
        -11380, -10252, -9121,   -7987,  -6850,  -5712,  -4572,  -3430,  -2287,  -1144
    };
    int cosaTable[360] = {
         65536,  65526,  65496,  65446,  65376,  65287,  65177,  65048,  64898,  64729,
         64540,  64332,  64104,  63856,  63589,  63303,  62997,  62672,  62328,  61966,
         61584,  61183,  60764,  60326,  59870,  59396,  58903,  58393,  57865,  57319,
         56756,  56175,  55578,  54963,  54332,  53684,  53020,  52339,  51643,  50931,
         50203,  49461,  48703,  47930,  47143,  46341,  45525,  44695,  43852,  42995,
         42126,  41243,  40348,  39441,  38521,  37590,  36647,  35693,  34729,  33754,
         32768,  31772,  30767,  29753,  28729,  27697,  26656,  25607,  24550,  23486,
         22415,  21336,  20252,  19161,  18064,  16962,  15855,  14742,  13626,  12505,
         11380,  10252,   9121,   7987,   6850,   5712,   4572,   3430,   2287,   1144,
             0,  -1144,  -2287,  -3430,  -4572,  -5712,  -6850,  -7987,  -9121, -10252,
        -11380, -12505, -13626, -14742, -15855, -16962, -18064, -19161, -20252, -21336,
        -22415, -23486, -24550, -25607, -26656, -27697, -28729, -29753, -30767, -31772,
        -32768, -33754, -34729, -35693, -36647, -37590, -38521, -39441, -40348, -41243,
        -42126, -42995, -43852, -44695, -45525, -46341, -47143, -47930, -48703, -49461,
        -50203, -50931, -51643, -52339, -53020, -53684, -54332, -54963, -55578, -56175,
        -56756, -57319, -57865, -58393, -58903, -59396, -59870, -60326, -60764, -61183,
        -61584, -61966, -62328, -62672, -62997, -63303, -63589, -63856, -64104, -64332,
        -64540, -64729, -64898, -65048, -65177, -65287, -65376, -65446, -65496, -65526,
        -65536, -65526, -65496, -65446, -65376, -65287, -65177, -65048, -64898, -64729, 
        -64540, -64332, -64104, -63856, -63589, -63303, -62997, -62672, -62328, -61966,
        -61584, -61183, -60764, -60326, -59870, -59396, -58903, -58393, -57865, -57319,
        -56756, -56175, -55578, -54963, -54332, -53684, -53020, -52339, -51643, -50931,
        -50203, -49461, -48703, -47930, -47143, -46341, -45525, -44695, -43852, -42995,
        -42126, -41243, -40348, -39441, -38521, -37590, -36647, -35693, -34729, -33754,
        -32768, -31772, -30767, -29753, -28729, -27697, -26656, -25607, -24550, -23486,
        -22415, -21336, -20252, -19161, -18064, -16962, -15855, -14742, -13626, -12505, 
        -11380, -10252,  -9121,  -7987,  -6850,  -5712,  -4572,  -3430,  -2287,  -1144,
             0,   1144,   2287,   3430,   4572,   5712,   6850,   7987,   9121,  10252,
         11380,  12505,  13626,  14742,  15855,  16962,  18064,  19161,  20252,  21336,
         22415,  23486,  24550,  25607,  26656,  27697,  28729,  29753,  30767,  31772,
         32768,  33754,  34729,  35693,  36647,  37590,  38521,  39441,  40348,  41243,
         42126,  42995,  43852,  44695,  45525,  46341,  47143,  47930,  48703,  49461,
         50203,  50931,  51643,  52339,  53020,  53684,  54332,  54963,  55578,  56175,
         56756,  57319,  57865,  58393,  58903,  59396,  59870,  60326,  60764,  61183,
         61584,  61966,  62328,  62672,  62997,  63303,  63589,  63856,  64104,  64332,
         64540,  64729,  64898,  65048,  65177,  65287,  65376,  65446,  65496,  65526
    };
    memcpy(sina_table, sinaTable, sizeof(sina_table));
    memcpy(cosa_table, cosaTable, sizeof(cosa_table));
    return 0;
}


// ---------------------------------------------------------------------------

}
; // namespace android

