#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vapoursynth/VSHelper.h>
#include <vapoursynth/VapourSynth.h>

#include <turbojpeg.h>

typedef struct JpegData {
    VSVideoInfo vi;
    VSFrameRef *frame;
} JpegData;

typedef struct JpegsData {
    VSVideoInfo vi;
    int height1, width1, height2, width2, jpegSubSamp;
    char **paths;
} JpegsData;

static void VS_CC jpegInit(VSMap *in, VSMap *out, void **instanceData,
                           VSNode *node, VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)*instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static void VS_CC jpegsInit(VSMap *in, VSMap *out, void **instanceData,
                            VSNode *node, VSCore *core, const VSAPI *vsapi) {
    JpegsData *d = (JpegsData *)*instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC jpegGetFrame(int n, int activationReason,
                                            void **instanceData,
                                            void **frameData,
                                            VSFrameContext *frameCtx,
                                            VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)*instanceData;
    return vsapi->cloneFrameRef(d->frame);
}

static const VSFrameRef *VS_CC jpegsGetFrame(int n, int activationReason,
                                             void **instanceData,
                                             void **frameData,
                                             VSFrameContext *frameCtx,
                                             VSCore *core, const VSAPI *vsapi) {
    JpegsData *d = (JpegsData *)*instanceData;

    VSFrameRef *dst =
        vsapi->newVideoFrame(d->vi.format, d->width1, d->height1, NULL, core);

    FILE *f;
    f = fopen(d->paths[n], "rb");
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    uint8_t *jpegBuf = (uint8_t *)malloc(size);
    fread(jpegBuf, 1, size, f);
    fclose(f);

    int strides[3] = {vsapi->getStride(dst, 0), vsapi->getStride(dst, 1),
                      vsapi->getStride(dst, 2)};
    uint8_t *buf[3] = {vsapi->getWritePtr(dst, 0), vsapi->getWritePtr(dst, 1),
                       vsapi->getWritePtr(dst, 2)};

    tjhandle handle = tjInitDecompress();
    if (d->vi.format->id == pfYUV420P8) {
        tjDecompressToYUVPlanes(handle, jpegBuf, size, buf, d->width1, strides,
                                d->height1, TJFLAG_ACCURATEDCT);
        free(jpegBuf);
        tjDestroy(handle);
    } else {
        uint8_t tmp[d->width1 * d->height1 * 3];
        tjDecompress2(handle, jpegBuf, size, tmp, d->width1, d->width1 * 3, 0,
                      TJPF_RGB, TJFLAG_ACCURATEDCT);
        free(jpegBuf);
        tjDestroy(handle);

        for (int y = 0; y < d->height1; y++) {
            for (int x = 0; x < d->width1; x++) {
                buf[0][(y * strides[0]) + x] = tmp[((y * d->width1) + x) * 3];
                buf[1][(y * strides[1]) + x] =
                    tmp[((y * d->width1) + x) * 3 + 1];
                buf[2][(y * strides[2]) + x] =
                    tmp[((y * d->width1) + x) * 3 + 2];
            }
        }
    }

    VSMap *props = vsapi->getFramePropsRW(dst);
    vsapi->propSetInt(props, "_ColorRange", 0, paReplace);

    return dst;
}

static void VS_CC jpegFree(void *instanceData, VSCore *core,
                           const VSAPI *vsapi) {
    JpegData *d = (JpegData *)instanceData;
    vsapi->freeFrame(d->frame);
    free(d);
}

static void VS_CC jpegsFree(void *instanceData, VSCore *core,
                            const VSAPI *vsapi) {
    JpegsData *d = (JpegsData *)instanceData;
    for (int i = 0; i < d->vi.numFrames; i++) free(d->paths[i]);
    free(d->paths);
    free(d);
}

static void VS_CC jpegCreate(const VSMap *in, VSMap *out, void *userData,
                             VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)malloc(sizeof(JpegData));

    tjhandle handle = tjInitDecompress();
    if (handle == NULL) {
        vsapi->setError(out, tjGetErrorStr2(NULL));
        return;
    }

    FILE *f;
    if ((f = fopen(vsapi->propGetData(in, "filename", 0, NULL), "rb")) ==
        NULL) {
        vsapi->setError(out, "Jpeg: unable to open provided path");
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    uint8_t *jpegBuf = (uint8_t *)malloc(size);
    fread(jpegBuf, 1, size, f);
    fclose(f);

    int jpegColorspace, jpegSubSamp, width, height;
    if (tjDecompressHeader3(handle, jpegBuf, size, &width, &height,
                            &jpegSubSamp, &jpegColorspace) == -1) {
        free(jpegBuf);
        tjDestroy(handle);
        vsapi->setError(out, tjGetErrorStr2(handle));
        return;
    }

    d->vi.numFrames = 1;
    int err;
    d->vi.fpsNum = vsapi->propGetInt(in, "fpsnum", 0, &err);
    if (d->vi.fpsNum <= 0) d->vi.fpsNum = 1;
    d->vi.fpsDen = vsapi->propGetInt(in, "fpsden", 0, &err);
    if (d->vi.fpsDen <= 0) d->vi.fpsDen = 1;

    if (jpegColorspace == TJCS_YCbCr) {
        int actualHeight = height;
        int actualWidth = width;
        int subW, subH;
        switch (jpegSubSamp) {
            case TJSAMP_420:
                d->vi.format = vsapi->getFormatPreset(pfYUV420P8, core);
                width &= -2;
                height &= -2;
                subW = subH = 1;
                break;
            case TJSAMP_444:
                d->vi.format = vsapi->getFormatPreset(pfYUV444P8, core);
                subW = subH = 0;
                break;
            case TJSAMP_422:
                d->vi.format = vsapi->getFormatPreset(pfYUV422P8, core);
                width &= -2;
                subW = 1;
                subH = 0;
                break;
            case TJSAMP_440:
                d->vi.format = vsapi->getFormatPreset(pfYUV440P8, core);
                height &= -2;
                subW = 0;
                subH = 1;
                break;
            case TJSAMP_411:
                d->vi.format = vsapi->getFormatPreset(pfYUV411P8, core);
                width &= -4;
                subW = 2;
                subH = 0;
                break;
        }

        d->frame =
            vsapi->newVideoFrame(d->vi.format, width, height, NULL, core);
        int stride = vsapi->getStride(d->frame, 0);

        if (height < actualHeight || stride < actualWidth) {
            int chromaStride = (actualWidth << subW) + 31 & -32;
            int chromaMem = chromaStride * (actualHeight << subH);
            int strides[3] = {actualWidth + 31 & -32, chromaStride,
                              chromaStride};
            uint8_t *buf[3] = {(uint8_t *)malloc(strides[0] * actualHeight),
                               (uint8_t *)malloc(chromaMem),
                               (uint8_t *)malloc(chromaMem)};
            if (tjDecompressToYUVPlanes(handle, jpegBuf, size, buf, 0, strides,
                                        0, TJFLAG_ACCURATEDCT) == -1) {
                free(jpegBuf);
                tjDestroy(handle);
                vsapi->setError(out, tjGetErrorStr2(handle));
                return;
            }
            for (int i = 0; i < 3; i++) {
                vs_bitblt(vsapi->getWritePtr(d->frame, i),
                          vsapi->getStride(d->frame, i), buf[i], strides[i],
                          width, height);
                free(buf[i]);
            }
        } else {
            uint8_t *buf[3] = {vsapi->getWritePtr(d->frame, 0),
                               vsapi->getWritePtr(d->frame, 1),
                               vsapi->getWritePtr(d->frame, 2)};
            int strides[3] = {stride, vsapi->getStride(d->frame, 1),
                              vsapi->getStride(d->frame, 2)};

            if (tjDecompressToYUVPlanes(handle, jpegBuf, size, buf, 0, strides,
                                        0, TJFLAG_ACCURATEDCT) == -1) {
                free(jpegBuf);
                tjDestroy(handle);
                vsapi->setError(out, tjGetErrorStr2(handle));
                return;
            }
        }
    } else if (jpegColorspace == TJCS_GRAY) {
        d->vi.format = vsapi->getFormatPreset(pfGray8, core);
        d->frame =
            vsapi->newVideoFrame(d->vi.format, width, height, NULL, core);
        int stride = vsapi->getStride(d->frame, 0);
        uint8_t *plane = vsapi->getWritePtr(d->frame, 0);
        if (tjDecompressToYUVPlanes(handle, jpegBuf, size, &plane, 0, &stride,
                                    0, TJFLAG_ACCURATEDCT) == -1) {
            free(jpegBuf);
            tjDestroy(handle);
            vsapi->setError(out, tjGetErrorStr2(handle));
            return;
        }
    } else if (jpegColorspace == TJCS_RGB) {
        d->vi.format = vsapi->getFormatPreset(pfRGB24, core);
        d->frame =
            vsapi->newVideoFrame(d->vi.format, width, height, NULL, core);

        int pixels = width * height;
        uint8_t *buf = (uint8_t *)malloc(pixels * 3);
        if (tjDecompress2(handle, jpegBuf, size, buf, 0, 0, 0, TJPF_RGB,
                          TJFLAG_ACCURATEDCT) == -1) {
            free(jpegBuf);
            free(buf);
            tjDestroy(handle);
            vsapi->setError(out, tjGetErrorStr2(handle));
            return;
        }

        for (int i = 0; i < 3; i++) {
            uint8_t *plane = vsapi->getWritePtr(d->frame, i);
            int stride = vsapi->getStride(d->frame, i);

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++)
                    plane[y * stride + x] = buf[3 * y * height + x + i];
            }
        }
        free(buf);
    } else {
        free(jpegBuf);
        tjDestroy(handle);
        vsapi->setError(out, "Jpeg: unsupported color space");
        return;
    }
    d->vi.width = width;
    d->vi.height = height;

    VSMap *props = vsapi->getFramePropsRW(d->frame);
    vsapi->propSetInt(props, "_ColorRange", 0, paReplace);

    free(jpegBuf);
    tjDestroy(handle);

    vsapi->createFilter(in, out, "Jpeg", jpegInit, jpegGetFrame, jpegFree,
                        fmParallel, nfNoCache, d, core);
}

static void VS_CC stitchCreate(const VSMap *in, VSMap *out, void *userData,
                               VSCore *core, const VSAPI *vsapi) {
    JpegData *d = (JpegData *)malloc(sizeof(JpegData));

    tjhandle handle = tjInitDecompress();
    if (handle == NULL) {
        vsapi->setError(out, tjGetErrorStr2(NULL));
        return;
    }

    d->vi.numFrames = 1;
    int err;
    d->vi.fpsNum = vsapi->propGetInt(in, "fpsnum", 0, &err);
    if (d->vi.fpsNum <= 0) d->vi.fpsNum = 1;
    d->vi.fpsDen = vsapi->propGetInt(in, "fpsden", 0, &err);
    if (d->vi.fpsDen <= 0) d->vi.fpsDen = 1;

    int allocatedPlanes = 0;
    int colorspace, subSamp, height, subW, subH;
    int totalWidth = 0;
    int numPlanes;

    int numFiles = vsapi->propNumElements(in, "filename");
    uint8_t ***planes = (uint8_t ***)malloc(numFiles * sizeof(uint8_t **));
    int **strides = (int **)malloc(numFiles * sizeof(int *));
    int **widths = (int **)malloc(numFiles * sizeof(int *));
    if (planes == NULL || widths == NULL || strides == NULL) {
        vsapi->setError(
            out,
            "Stitch: unable to allocate memory for planes, strides, or widths");
        goto free1;
    }

    uint8_t *jpegBuf;

    for (int fileNum = 0; fileNum < numFiles; fileNum++) {
        FILE *f;
        if ((f = fopen(vsapi->propGetData(in, "filename", fileNum, NULL),
                       "rb")) == NULL) {
            vsapi->setError(out, "Stitch: unable to open provided path");
            goto free2;
        }
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        rewind(f);
        jpegBuf = (uint8_t *)malloc(size);
        if (jpegBuf == NULL) {
            vsapi->setError(out, "Stitch: unable to allocate memory for JPEG");
            goto free2;
        }
        fread(jpegBuf, 1, size, f);
        fclose(f);

        int imageWidth, imageHeight, imageSubSamp, imageColorspace;
        if (tjDecompressHeader3(handle, jpegBuf, size, &imageWidth,
                                &imageHeight, &imageSubSamp,
                                &imageColorspace) == -1) {
            vsapi->setError(out, tjGetErrorStr2(handle));
            goto free3;
        }
        if (fileNum == 0) {
            height = imageHeight;
            subSamp = imageSubSamp;
            colorspace = imageColorspace;

            switch (colorspace) {
                case TJCS_YCbCr:
                    numPlanes = 3;
                    switch (subSamp) {
                        case TJSAMP_420:
                            d->vi.format =
                                vsapi->getFormatPreset(pfYUV420P8, core);
                            subW = subH = 1;
                            break;
                        case TJSAMP_444:
                            d->vi.format =
                                vsapi->getFormatPreset(pfYUV444P8, core);
                            subW = subH = 0;
                            break;
                        case TJSAMP_422:
                            d->vi.format =
                                vsapi->getFormatPreset(pfYUV422P8, core);
                            subW = 1;
                            subH = 0;
                            break;
                        case TJSAMP_440:
                            d->vi.format =
                                vsapi->getFormatPreset(pfYUV440P8, core);
                            subW = 0;
                            subH = 1;
                            break;
                        case TJSAMP_411:
                            d->vi.format =
                                vsapi->getFormatPreset(pfYUV411P8, core);
                            subW = 2;
                            subH = 0;
                            break;
                    }
                    break;
                case TJCS_GRAY:
                    numPlanes = 1;
                    subW = subH = 0;
                    d->vi.format = vsapi->getFormatPreset(pfGray8, core);
                    break;
                case TJCS_RGB:
                    numPlanes = 3;
                    subW = subH = 0;
                    d->vi.format = vsapi->getFormatPreset(pfRGB24, core);
                    break;
                default:
                    vsapi->setError(out, "Jpeg: unsupported color space");
                    goto free3;
            }
        } else if (imageHeight != height || imageColorspace != colorspace ||
                   imageSubSamp != subSamp) {
            vsapi->setError(out, "Stitch: mismatched images");
            goto free3;
        }
        totalWidth += imageWidth;

        planes[fileNum] = (uint8_t **)malloc(numPlanes * sizeof(uint8_t *));
        strides[fileNum] = (int *)malloc(numPlanes * sizeof(int));
        widths[fileNum] = (int *)malloc(numPlanes * sizeof(int));
        if (planes[fileNum] == NULL || strides[fileNum] == NULL ||
            widths[fileNum] == NULL) {
            vsapi->setError(
                out, "Stitch: unable to allocate memory for planes or strides");
            goto free3;
        }
        allocatedPlanes++;

        switch (colorspace) {
            case TJCS_YCbCr: {
                for (int i = 0; i < 3; i++) {
                    strides[fileNum][i] = tjPlaneWidth(i, imageWidth, subSamp);
                    planes[fileNum][i] =
                        (uint8_t *)malloc(strides[fileNum][i] *
                                          tjPlaneHeight(i, height, subSamp));
                    widths[fileNum][i] = imageWidth >> subW;

                    if (planes[fileNum][i] == NULL) {
                        vsapi->setError(
                            out,
                            "Stitch: unable to allocate memory for planes");
                        goto free3;
                    }
                }
                widths[fileNum][0] = imageWidth;

                if (tjDecompressToYUVPlanes(
                        handle, jpegBuf, size, planes[fileNum], 0,
                        strides[fileNum], 0, TJFLAG_ACCURATEDCT) == -1) {
                    vsapi->setError(out, tjGetErrorStr2(handle));
                    goto free3;
                }
                break;
            }
            case TJCS_GRAY: {
                strides[fileNum][0] = tjPlaneWidth(0, imageWidth, subSamp);
                planes[fileNum][0] = (uint8_t *)malloc(
                    strides[fileNum][0] * tjPlaneHeight(0, height, subSamp));
                if (planes[fileNum][0] == NULL) {
                    vsapi->setError(
                        out, "Stitch: unable to allocate memory for planes");
                    goto free3;
                }
                widths[fileNum][0] = imageWidth;

                if (tjDecompressToYUVPlanes(
                        handle, jpegBuf, size, planes[fileNum], 0,
                        strides[fileNum], 0, TJFLAG_ACCURATEDCT) == -1) {
                    vsapi->setError(out, tjGetErrorStr2(handle));
                    goto free3;
                }
                break;
            }
            case TJCS_RGB: {
                int pixels = imageWidth * height;
                uint8_t *buf = (uint8_t *)malloc(pixels * 3);
                if (buf == NULL) {
                    vsapi->setError(
                        out,
                        "Stitch: unable to allocate memory for RGB buffer");
                    goto free3;
                }
                if (tjDecompress2(handle, jpegBuf, size, buf, 0, 0, 0, TJPF_RGB,
                                  TJFLAG_ACCURATEDCT) == -1) {
                    free(buf);
                    vsapi->setError(out, tjGetErrorStr2(handle));
                    goto free3;
                }

                for (int i = 0; i < 3; i++) {
                    widths[fileNum][i] = strides[fileNum][i] = imageWidth;
                    planes[fileNum][i] = (uint8_t *)malloc(pixels);

                    if (planes[fileNum][i] == NULL) {
                        free(buf);
                        vsapi->setError(
                            out,
                            "Stitch: unable to allocate memory for planes");
                        goto free3;
                    }
                }

                for (int i = 0; i < pixels; i++) {
                    planes[fileNum][0][i] = buf[i * 3];
                    planes[fileNum][1][i] = buf[i * 3 + 1];
                    planes[fileNum][2][i] = buf[i * 3 + 2];
                }
                free(buf);
                break;
            }
        }
        free(jpegBuf);
    }

    d->vi.width = totalWidth;
    if (subW > 0) {
        int remain = totalWidth % (subW * 2);
        if (remain != 0) {
            // compensate for subsampling with a width that is not divisible by
            // the subsampling factor

            // crop the right side of the last image by the remainder
            widths[numFiles - 1][0] -= remain;
            d->vi.width -= remain;

            // expand the chroma width by the remainder
            for (int i = 0; i < remain; i++) {
                if (i < numFiles) {
                    widths[i][1] += 1;
                    widths[i][2] += 1;
                } else {
                    widths[0][1] += 1;
                    widths[0][2] += 1;
                }
            }
        }
    }

    int heights[3] = {height, height, height};
    if (subH > 0) {
        heights[0] = d->vi.height = height & -(subH * 2);
        for (int i = 1; i < numPlanes; i++)
            heights[i] >>= subH;
    } else
        d->vi.height = height;

    d->frame = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height,
                                    NULL, core);

    int pos[3] = {0, 0, 0};
    for (int i = 0; i < numFiles; i++) {
        for (int j = 0; j < numPlanes; j++) {
            vs_bitblt(vsapi->getWritePtr(d->frame, j) + pos[j],
                      vsapi->getStride(d->frame, j), planes[i][j],
                      strides[i][j], widths[i][j], heights[j]);

            free(planes[i][j]);
            pos[j] += widths[i][j];
        }
        free(planes[i]);
        free(strides[i]);
        free(widths[i]);
    }
    VSMap *props = vsapi->getFramePropsRW(d->frame);
    vsapi->propSetInt(props, "_ColorRange", 0, paReplace);

    vsapi->createFilter(in, out, "Stitch", jpegInit, jpegGetFrame, jpegFree,
                        fmParallel, nfNoCache, d, core);
    goto free1;

free3:
    free(jpegBuf);
free2:
    for (int i = 0; i < allocatedPlanes; i++) {
        for (int j = 0; j < numPlanes; j++) free(planes[i][j]);
        free(planes[i]);
        free(strides[i]);
        free(widths[i]);
    }
free1:
    free(strides);
    free(planes);
    free(widths);
    tjDestroy(handle);
}

static void VS_CC jpegsCreate(const VSMap *in, VSMap *out, void *userData,
                              VSCore *core, const VSAPI *vsapi) {
    JpegsData *d = (JpegsData *)calloc(sizeof(JpegsData), 1);

    tjhandle handle = tjInitDecompress();

    d->vi.numFrames = vsapi->propNumElements(in, "filename");
    d->paths = (char **)malloc(d->vi.numFrames * sizeof(char *));
    for (int i = 0; i < d->vi.numFrames; i++) {
        d->paths[i] =
            (char *)malloc(vsapi->propGetDataSize(in, "filename", i, NULL) + 1);
        strcpy(d->paths[i], vsapi->propGetData(in, "filename", i, NULL));
    }

    FILE *f;
    if ((f = fopen(d->paths[0], "rb")) == NULL) {
        vsapi->setError(out, "Jpeg: unable to open provided path");
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    uint8_t jpegBuf[size];
    fread(jpegBuf, 1, size, f);
    fclose(f);

    int jpegColorspace;
    tjDecompressHeader3(handle, jpegBuf, size, &d->width1, &d->height1,
                        &jpegColorspace, &d->jpegSubSamp);

    d->vi.width = d->width1;
    d->vi.height = d->height1;

    int err;
    d->vi.fpsNum = vsapi->propGetInt(in, "fpsnum", 0, &err);
    if (d->vi.fpsNum <= 0) d->vi.fpsNum = 1;
    d->vi.fpsDen = vsapi->propGetInt(in, "fpsden", 0, &err);
    if (d->vi.fpsDen <= 0) d->vi.fpsDen = 1;

    if (jpegColorspace == 2) {
        d->width2 = d->width1 / 2;
        d->height2 = d->height1 / 2;
        d->vi.format = vsapi->getFormatPreset(pfYUV420P8, core);
    } else if (jpegColorspace == 0) {
        d->width2 = d->width1;
        d->height2 = d->height1;
        d->vi.format = vsapi->getFormatPreset(pfRGB24, core);
    } else {
        vsapi->setError(out, "Jpeg: unsupported color space");
        return;
    }

    tjDestroy(handle);

    vsapi->createFilter(in, out, "Jpegs", jpegsInit, jpegsGetFrame, jpegsFree,
                        fmParallel, 0, d, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc,
                      VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("xyz.noctem.jpeg", "jpeg", "Source filter for jpeg images.",
               VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Jpeg", "filename:data;fpsnum:int:opt;fpsden:int:opt;",
                 jpegCreate, NULL, plugin);
    registerFunc("Stitch", "filename:data[];fpsnum:int:opt;fpsden:int:opt;",
                 stitchCreate, NULL, plugin);
    registerFunc("Jpegs", "filename:data[];fpsnum:int:opt;fpsden:int:opt;",
                 jpegsCreate, NULL, plugin);
}
