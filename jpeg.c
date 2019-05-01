#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vapoursynth/VapourSynth.h>

#include <turbojpeg.h>

typedef struct JpegData {
    VSVideoInfo vi;
    int height1, width1, height2, width2;
    uint8_t *plane1;
    uint8_t *plane2;
    uint8_t *plane3;
} JpegData;

static void VS_CC jpegInit(VSMap *in, VSMap *out, void **instanceData,
                           VSNode *node, VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)*instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC jpegGetFrame(int n, int activationReason,
                                            void **instanceData,
                                            void **frameData,
                                            VSFrameContext *frameCtx,
                                            VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)*instanceData;

    VSFrameRef *dst =
        vsapi->newVideoFrame(d->vi.format, d->width1, d->height1, NULL, core);
    int stride1 = vsapi->getStride(dst, 0);
    int stride2 = vsapi->getStride(dst, 1);
    uint8_t *plane1 = vsapi->getWritePtr(dst, 0);
    uint8_t *plane2 = vsapi->getWritePtr(dst, 1);
    uint8_t *plane3 = vsapi->getWritePtr(dst, 2);

    for (int i = 0; i < d->height1; i++)
        memcpy(plane1 + (i * stride1), d->plane1 + (i * d->width1), d->width1);

    for (int i = 0; i < d->height2; i++) {
        memcpy(plane2 + (i * stride2), d->plane2 + (i * d->width2), d->width2);
        memcpy(plane3 + (i * stride2), d->plane3 + (i * d->width2), d->width2);
    }

    return dst;
}

static void VS_CC jpegFree(void *instanceData, VSCore *core,
                           const VSAPI *vsapi) {
    JpegData *d = (JpegData *)instanceData;
    free(d->plane1);
    free(d->plane2);
    free(d->plane3);
    free(d);
}

static void VS_CC jpegCreate(const VSMap *in, VSMap *out, void *userData,
                             VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)calloc(sizeof(JpegData), 1);

    tjhandle handle = tjInitDecompress();

    FILE *f;
    if ((f = fopen(vsapi->propGetData(in, "path", 0, NULL), "rb")) == NULL) {
        vsapi->setError(out, "Jpeg: unable to open provided path");
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    uint8_t *jpegBuf = malloc(size);
    fread(jpegBuf, 1, size, f);
    fclose(f);

    int jpegColorspace, jpegSubSamp;
    tjDecompressHeader3(handle, jpegBuf, size, &d->width1, &d->height1,
                        &jpegColorspace, &jpegSubSamp);

    d->vi.width = d->width1;
    d->vi.height = d->height1;
    d->vi.numFrames = 1;

    if (jpegColorspace == 2) {
        d->width2 = d->width1 / 2;
        d->height2 = d->height1 / 2;
        uint8_t *buf[3];
        buf[0] =
            malloc(tjPlaneSizeYUV(0, d->width1, 0, d->height1, jpegSubSamp));
        buf[1] =
            malloc(tjPlaneSizeYUV(1, d->width1, 0, d->height1, jpegSubSamp));
        buf[2] =
            malloc(tjPlaneSizeYUV(2, d->width1, 0, d->height1, jpegSubSamp));
        tjDecompressToYUVPlanes(handle, jpegBuf, size, buf, d->width1, NULL, 0,
                                TJFLAG_ACCURATEDCT);
        d->vi.format = vsapi->getFormatPreset(pfYUV420P8, core);
        d->plane1 = buf[0];
        d->plane2 = buf[1];
        d->plane3 = buf[2];
    } else if (jpegColorspace == 0) {
        d->width2 = d->width1;
        d->height2 = d->height1;
        uint8_t tmp[d->width1 * d->height1 * 3];
        d->plane1 = malloc(d->width1 * d->height1);
        d->plane2 = malloc(d->width2 * d->height2);
        d->plane3 = malloc(d->width2 * d->height2);
        tjDecompress2(handle, jpegBuf, size, tmp, d->width1, d->width1 * 3, 0,
                      TJPF_RGB, TJFLAG_ACCURATEDCT);
        d->vi.format = vsapi->getFormatPreset(pfRGB24, core);
        for (int i = 0; i < d->width1 * d->height1; i++) {
            d->plane1[i] = tmp[i * 3];
            d->plane2[i] = tmp[i * 3 + 1];
            d->plane3[i] = tmp[i * 3 + 2];
        }
    } else {
        free(jpegBuf);
        tjDestroy(handle);
        vsapi->setError(out, "Jpeg: unsupported color space");
        return;
    }

    free(jpegBuf);
    tjDestroy(handle);

    vsapi->createFilter(in, out, "Jpeg", jpegInit, jpegGetFrame, jpegFree,
                        fmParallel, 0, d, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc,
                      VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("xyz.noctem.jpeg", "jpeg", "Source filter for jpeg images.",
               VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Jpeg", "path:data[];", jpegCreate, NULL, plugin);
}