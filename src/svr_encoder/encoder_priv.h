#pragma once
#include "svr_common.h"
#include "encoder_shared.h"
#include "svr_log.h"
#include "svr_alloc.h"
#include "svr_locked_array.h"
#include "svr_locked_queue.h"
#include "svr_atom.h"
#include "svr_defs.h"
#include <stdio.h>
#include <Windows.h>
#include <d3d11_1.h>
#include <d3d11shadertracing.h>
#include <dxgi.h>
#include <assert.h>

extern "C"
{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
    #include <libavutil/avutil.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/opt.h>
    #include <libavutil/audio_fifo.h>
}

#include "encoder_state.h"
