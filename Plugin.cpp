/*****************************************************************************
 * Plugin.cpp
 *****************************************************************************
 * Copyright (C) 2015
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

#include <string>
#include <stdexcept>

#include <VapourSynth.h>
#include <VSHelper.h>
#include "FFT3DFilter.h"

static inline void getPlanesArg(const VSMap *in, bool *process, const VSAPI *vsapi) {
    int m = vsapi->propNumElements(in, "planes");

    for (int i = 0; i < 3; i++)
        process[i] = (m <= 0);

    for (int i = 0; i < m; i++) {
        int o = int64ToIntS(vsapi->propGetInt(in, "planes", i, nullptr));

        if (o < 0 || o >= 3)
            throw std::runtime_error("plane index out of range");

        if (process[o])
            throw std::runtime_error("plane specified twice");

        process[o] = true;
    }
}

static inline void set_option_int64
(
    int64_t     *opt,
    int64_t      default_value,
    const char  *arg,
    const VSMap *in,
    const VSAPI *vsapi
)
{
    int e;
    *opt = vsapi->propGetInt( in, arg, 0, &e );
    if( e )
        *opt = default_value;
}

static inline void set_option_float
(
    float       *opt,
    float        default_value,
    const char  *arg,
    const VSMap *in,
    const VSAPI *vsapi
)
{
    int e;
    *opt = static_cast<float>(vsapi->propGetFloat( in, arg, 0, &e ));
    if( e )
        *opt = static_cast<float>(default_value);
}

static void VS_CC createFFT3DFilter
(
    const VSMap *in,
    VSMap       *out,
    void        *user_data,
    VSCore      *core,
    const VSAPI *vsapi
)
{
    float sigma1;
    float beta;
    bool process[3];
    int64_t bw;
    int64_t bh;
    int64_t bt;
    int64_t ow;
    int64_t oh;
    float   kratio;
    float   sharpen;
    float   scutoff;
    float   svr;
    float   smin;
    float   smax;
    int64_t measure;
    int64_t interlaced;
    int64_t wintype;
    int64_t pframe;
    int64_t px;
    int64_t py;
    int64_t pshow;
    float   pcutoff;
    float   pfactor;
    float   sigma2;
    float   sigma3;
    float   sigma4;
    float   degrid;
    float   dehalo;
    float   hr;
    float   ht;
    int64_t ncpu;
    try {
        int istat = fftwf_init_threads();
        if (istat == 0)
            throw std::runtime_error{ "fftwf_init_threads() failed!" };

        fftwf_make_planner_thread_safe();

        getPlanesArg(in, process, vsapi);

        int num_process = 0;
        for (int i = 0; i < 3; i++)
            if (process[i])
                num_process++;

        //fixme, should probably error out with 0 planes processed too

        set_option_float( &sigma1,    2.0, "sigma",      in, vsapi );
        set_option_float( &beta,      1.0, "beta",       in, vsapi );
        set_option_int64( &bw,          32, "bw",         in, vsapi );
        set_option_int64( &bh,          32, "bh",         in, vsapi );
        set_option_int64( &bt,          3, "bt",         in, vsapi );
        set_option_int64( &ow,       bw/3, "ow",         in, vsapi );
        set_option_int64( &oh,       bh/3, "oh",         in, vsapi );
        set_option_float( &kratio,    2.0, "kratio",     in, vsapi );
        set_option_float( &sharpen,     0, "sharpen",    in, vsapi );
        set_option_float( &scutoff,  0.3f, "scutoff",    in, vsapi );
        set_option_float( &svr,       1.0, "svr",        in, vsapi );
        set_option_float( &smin,      4.0, "smin",       in, vsapi );
        set_option_float( &smax,     20.0, "smax",       in, vsapi );
        set_option_int64( &measure,     1, "measure",    in, vsapi );
        set_option_int64( &interlaced,  0, "interlaced", in, vsapi );
        set_option_int64( &wintype,     0, "wintype",    in, vsapi );
        set_option_int64( &pframe,      0, "pframe",     in, vsapi );
        set_option_int64( &px,          0, "px",         in, vsapi );
        set_option_int64( &py,          0, "py",         in, vsapi );
        set_option_int64( &pshow,       0, "pshow",      in, vsapi );
        set_option_float( &pcutoff,  0.1f, "pcutoff",    in, vsapi );
        set_option_float( &pfactor,     0, "pfactor",    in, vsapi );
        set_option_float( &sigma2, sigma1, "sigma2",     in, vsapi );
        set_option_float( &sigma3, sigma1, "sigma3",     in, vsapi );
        set_option_float( &sigma4, sigma1, "sigma4",     in, vsapi );
        set_option_float( &degrid,    1.0, "degrid",     in, vsapi );
        set_option_float( &dehalo,      0, "dehalo",     in, vsapi );
        set_option_float( &hr,        2.0, "hr",         in, vsapi );
        set_option_float( &ht,       50.0, "ht",         in, vsapi );
        set_option_int64( &ncpu,        1, "ncpu",       in, vsapi );    

        if (bt < -1 || bt > 5)
            throw std::runtime_error{ "bt must be -1(Sharpen), 0(Kalman), 1,2,3,4,5(Wiener)" };
        if (ow * 2 > bw)
            throw std::runtime_error{ "Must not be 2*ow > bw" };
        if (oh * 2 > bh)
            throw std::runtime_error{ "Must not be 2*oh > bh" };
        if (beta < 1)
            throw std::runtime_error{ "beta must be not less 1.0" };


        VSNodeRef *node = vsapi->propGetNode(in, "clip", 0, nullptr);
        const VSVideoInfo *vi = vsapi->getVideoInfo(node);
        int plane = 0;
        VSNodeRef *mainnode = nullptr;
        VSMap *tmp = vsapi->createMap();

        bool pshowmode = (pshow && pfactor != 0);

        if (pshowmode) {
            FFT3DFilterTransform *pshowtransform = new FFT3DFilterTransform(true, node, plane, wintype, bw, bh, ow, oh, px, py, pcutoff, degrid, interlaced, measure, core, vsapi);

            vsapi->createFilter
            (
                in, tmp,
                "FFT3DFilterPShowHelper",
                FFT3DFilterTransform::Init,
                FFT3DFilterTransform::GetPShowFrame,
                FFT3DFilterTransform::Free,
                fmParallelRequests, 0, pshowtransform, core
            );

            VSNodeRef *pshownode = vsapi->propGetNode(tmp, "clip", 0, nullptr);
            vsapi->clearMap(tmp);

            FFT3DFilterPShow *pshowfilter = new FFT3DFilterPShow(pshownode, plane, bw, bh, ow, oh, interlaced, core, vsapi);

            vsapi->createFilter
            (
                in, out,
                "FFT3DFilterPShow",
                FFT3DFilterPShow::Init,
                FFT3DFilterPShow::GetFrame,
                FFT3DFilterPShow::Free,
                fmParallelRequests, 0, pshowfilter, core
            );

        } else {
            FFT3DFilterTransform *transform = new FFT3DFilterTransform(false, node, plane, wintype, bw, bh, ow, oh, px, py, pcutoff, degrid, interlaced, measure, core, vsapi);

            vsapi->createFilter
            (
                in, tmp,
                "FFT3DFilterTransform",
                FFT3DFilterTransform::Init,
                FFT3DFilterTransform::GetFrame,
                FFT3DFilterTransform::Free,
                fmParallelRequests, 0, transform, core
            );

            VSNodeRef *transformednode = vsapi->propGetNode(tmp, "clip", 0, nullptr);
            vsapi->clearMap(tmp);


            FFT3DFilter *mainFilter = new FFT3DFilter(transform, vi, sigma1, beta, plane, bw, bh, bt, ow, oh,
                kratio, sharpen, scutoff, svr, smin, smax,
                pframe, px, py, pshow, pcutoff, pfactor,
                sigma2, sigma3, sigma4, degrid, dehalo, hr, ht, ncpu,
                transformednode, core, vsapi);

            vsapi->createFilter
            (
                in, tmp,
                "FFT3DFilterMain",
                FFT3DFilter::Init,
                FFT3DFilter::GetFrame,
                FFT3DFilter::Free,
                fmParallelRequests, 0, mainFilter, core
            );

            mainnode = vsapi->propGetNode(tmp, "clip", 0, nullptr);           

            FFT3DFilterInvTransform *invtransform = new FFT3DFilterInvTransform(mainnode, vi, plane, wintype, bw, bh, ow, oh, interlaced, measure, core, vsapi);

            vsapi->createFilter
            (
                in, out,
                "FFT3DFilterInverseTransform",
                FFT3DFilterInvTransform::Init,
                FFT3DFilterInvTransform::GetFrame,
                FFT3DFilterInvTransform::Free,
                fmParallelRequests, 0, invtransform, core
            );
        }

        vsapi->freeMap(tmp);
    }
    catch (std::runtime_error &e) {
        vsapi->setError(out, (std::string("FFT3DFilter: ") + e.what()).c_str());
    }
}

//fixme, make interlaced handling based on field based property and per frame

VS_EXTERNAL_API( void ) VapourSynthPluginInit
(
    VSConfigPlugin     config_func,
    VSRegisterFunction register_func,
    VSPlugin *         plugin
)
{
    config_func
    (
        "systems.innocent.fft3dfilter", "fft3dfilter",
        "FFT3DFilter",
        VAPOURSYNTH_API_VERSION, 1, plugin
    );
    register_func
    (
        "FFT3DFilter",
        "clip:clip;sigma:float:opt;beta:float:opt;planes:int[]:opt;bw:int:opt;bh:int:opt;bt:int:opt;ow:int:opt;oh:int:opt;"
        "kratio:float:opt;sharpen:float:opt;scutoff:float:opt;svr:float:opt;smin:float:opt;smax:float:opt;"
        "measure:int:opt;interlaced:int:opt;wintype:int:opt;"
        "pframe:int:opt;px:int:opt;py:int:opt;pshow:int:opt;pcutoff:float:opt;pfactor:float:opt;"
        "sigma2:float:opt;sigma3:float:opt;sigma4:float:opt;degrid:float:opt;"
        "dehalo:float:opt;hr:float:opt;ht:float:opt;ncpu:int:opt;",
        createFFT3DFilter, nullptr, plugin
    );
}
