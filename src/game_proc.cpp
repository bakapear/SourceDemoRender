#include "game_proc.h"
#include "game_shared.h"
#include <d3d11.h>
#include <strsafe.h>
#include <dwrite.h>
#include <d2d1_1.h>
#include <malloc.h>
#include <assert.h>
#include <intrin.h>
#include "svr_prof.h"
#include "svr_stream.h"
#include "svr_sem.h"
#include "game_proc_profile.h"
#include <stb_sprintf.h>
#include "svr_api.h"
#include <Shlwapi.h>
#include <DirectXMath.h>

using namespace DirectX;

// Don't use fatal process ending errors in here as this is used both in standalone and in integration.
// It is only standalone SVR that can do fatal processs ending errors.

// Must be a power of 2.
const s32 NUM_BUFFERED_DL_TEXS = 2;

// Rotation of CPU textures to download to.
struct CpuTexDl
{
    ID3D11Texture2D* texs[NUM_BUFFERED_DL_TEXS];
    s32 i;

    ID3D11Texture2D* get_current()
    {
        return texs[i];
    }

    void advance()
    {
        i = (i + 1) & (NUM_BUFFERED_DL_TEXS - 1);
    }
};

// -------------------------------------------------

char svr_resource_path[MAX_PATH];

// -------------------------------------------------
// Graphics state.

// High precision texture used for the result of mosample (need to be 32 bits per channel).

ID3D11Texture2D* work_tex;
ID3D11RenderTargetView* work_tex_rtv;
ID3D11ShaderResourceView* work_tex_srv;
ID3D11UnorderedAccessView* work_tex_uav;

// -------------------------------------------------
// Audio state.

// We write wav for now because writing multiple streams over a single pipe is weird. Will be looked into later.

const s32 WAV_BUFFERED_SAMPLES = 32768;

HANDLE wav_f;
DWORD wav_data_length;
DWORD wav_file_length;
DWORD wav_header_pos;
DWORD wav_data_pos;

// Only write out samples when we have enough.
SvrWaveSample* wav_buf;
s32 wav_num_samples;

// -------------------------------------------------
// Velo state.

ID2D1Factory1* d2d1_factory;
ID2D1Device* d2d1_device;
ID2D1DeviceContext* d2d1_context;
IDWriteFactory* dwrite_factory;
ID2D1SolidColorBrush* d2d1_solid_brush;

IDWriteFontFace* velo_font_face;
ID2D1Bitmap1* velo_rtv_ref; // Not a real texture, but a reference to the used render target.

// Incoming externally.
float player_velo[3];

// -------------------------------------------------
// Mosample state.

__declspec(align(16)) struct MosampleCb
{
    float mosample_weight;
};

ID3D11ComputeShader* mosample_cs;

// Constains the mosample weight.
ID3D11Buffer* mosample_cb;

// To not upload data all the time.
float mosample_weight_cache;
float mosample_remainder;
float mosample_remainder_step;

// -------------------------------------------------
// Movie state.

s32 movie_width;
s32 movie_height;

char movie_path[MAX_PATH];

// -------------------------------------------------
// Time profiling.

SvrProf frame_prof;
SvrProf dl_prof;
SvrProf write_prof;
SvrProf mosample_prof;

// -------------------------------------------------
// Movie profile.

MovieProfile movie_profile;

// -------------------------------------------------
// FFmpeg process communication.

struct ThreadPipeData
{
    u8* ptr;
    s32 size;
};

// We write data to the ffmpeg process through this pipe.
// It is redirected to their stdin.
HANDLE ffmpeg_write_pipe;

HANDLE ffmpeg_proc;

// How many completed buffers we keep in memory waiting to be sent to ffmpeg.
const s32 MAX_BUFFERED_SEND_BUFS = 8;

// The buffers that are sent to the ffmpeg process.
// For SW encoding these buffers are uncompressed frames of equal size.
ThreadPipeData ffmpeg_send_bufs[MAX_BUFFERED_SEND_BUFS];

HANDLE ffmpeg_thread;

// Queues and semaphores for communicating between the threads.

SvrAsyncStream<ThreadPipeData> ffmpeg_write_queue;
SvrAsyncStream<ThreadPipeData> ffmpeg_read_queue;

// Semaphore that is signalled when there are frames to send to ffmpeg (pulls from ffmpeg_write_queue).
// This is incremented by the game thread when it has added a downloaded frame to the write queue.
SvrSemaphore ffmpeg_write_sem;

// Semaphore that is signalled when there are frames available to download into (pulls from ffmpeg_read_queue).
// This is incremented by the ffmpeg thread when it has sent a frame to the ffmpeg process.
SvrSemaphore ffmpeg_read_sem;

bool has_ffmpeg_proc_exited()
{
    return WaitForSingleObject(ffmpeg_proc, 0) == WAIT_TIMEOUT;
}

// -------------------------------------------------

void update_constant_buffer(ID3D11DeviceContext* d3d11_context, ID3D11Buffer* buffer, void* data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = d3d11_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    assert(SUCCEEDED(hr));

    memcpy(mapped.pData, data, size);

    d3d11_context->Unmap(buffer, 0);
}

// Calculates the thread group count to use for specific dimensions.
s32 calc_cs_thread_groups(s32 input)
{
    // Thread group divisor constant must match the thread count in the compute shaders!

    // This number is arbitrary, and may end up with better or worse performance depending on the hardware.
    return ((float)input / 8.0f) + 0.5f;
}

// -------------------------------------------------
// SW encoding.

struct PxConvText
{
    const char* format;
    const char* color_space;
};

enum PxConv
{
    PXCONV_YUV420_601 = 0,
    PXCONV_YUV444_601,
    PXCONV_NV12_601,
    PXCONV_NV21_601,

    PXCONV_YUV420_709,
    PXCONV_YUV444_709,
    PXCONV_NV12_709,
    PXCONV_NV21_709,

    PXCONV_BGR0,

    NUM_PXCONVS,
};

// Up to 3 planes used for YUV video.
ID3D11Texture2D* pxconv_texs[3];
ID3D11UnorderedAccessView* pxconv_uavs[3];
ID3D11ComputeShader* pxconv_cs[NUM_PXCONVS];

CpuTexDl pxconv_dls[3];

UINT pxconv_pitches[3];
UINT pxconv_widths[3];
UINT pxconv_heights[3];

s32 used_pxconv_planes;
s32 pxconv_plane_sizes[3];
s32 pxconv_total_plane_sizes;

PxConv movie_pxconv;

// All the pxconv arrays are synchronized!

// Names of compiled shaders.
const char* PXCONV_SHADER_NAMES[] = {
    "80789c65c37c6acbf800e0a12e18e0fd4950065b",
    "cf005a5b5fc239779f3cf1e19cf2dab33e503ffc",
    "2a3229bd1f9d4785c87bc7913995d82dc4e09572",
    "58c00ca9b019bbba2dca15ec4c4c9c494ae7d842",

    "38668b40da377284241635b07e22215c204eb137",
    "659f7e14fec1e7018a590c5a01d8169be881438a",
    "b9978bbddaf801c44f71f8efa9f5b715ada90000",
    "5d5924a1a56d4d450743e939d571d04a82209673",

    "b44000e74095a254ef98a2cdfcbaf015ab6c295e",
};

// Names for ini.
PxConvText PXCONV_INI_TEXT_TABLE[] = {
    PxConvText { "yuv420", "601" },
    PxConvText { "yuv444", "601" },
    PxConvText { "nv12", "601" },
    PxConvText { "nv21", "601" },

    PxConvText { "yuv420", "709" },
    PxConvText { "yuv444", "709" },
    PxConvText { "nv12", "709" },
    PxConvText { "nv21", "709" },

    PxConvText { "bgr0", "rgb" },
};

// Names for ffmpeg.
PxConvText PXCONV_FFMPEG_TEXT_TABLE[] = {
    PxConvText { "yuv420p", "bt470bg" },
    PxConvText { "yuv444p", "bt470bg" },
    PxConvText { "nv12", "bt470bg" },
    PxConvText { "nv21", "bt470bg" },

    PxConvText { "yuv420p", "bt709" },
    PxConvText { "yuv444p", "bt709" },
    PxConvText { "nv12", "bt709" },
    PxConvText { "nv21", "bt709" },

    PxConvText { "bgr0", NULL },
};

// How many planes that are used in a pixel format.
s32 calc_format_planes(PxConv pxconv)
{
    switch (pxconv)
    {
        case PXCONV_YUV420_601:
        case PXCONV_YUV444_601:
        case PXCONV_YUV420_709:
        case PXCONV_YUV444_709:
        {
            return 3;
        }

        case PXCONV_NV12_601:
        case PXCONV_NV21_601:
        case PXCONV_NV12_709:
        case PXCONV_NV21_709:
        {
            return 2;
        }

        case PXCONV_BGR0:
        {
            return 1;
        }
    }

    assert(false);
    return 0;
}

UINT calc_bytes_pitch(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8_UINT: return 1;
        case DXGI_FORMAT_R8G8_UINT: return 2;
        case DXGI_FORMAT_R8G8B8A8_UINT: return 4;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    }

    assert(false);
    return 0;
}

// Retrieves the size of a plane in a pixel format.
void calc_plane_dims(PxConv pxconv, s32 width, s32 height, s32 plane, s32* out_width, s32* out_height)
{
    // Height and width chroma shifts for each plane dimension (luma is ignored, so set to 0).

    const s32 YUV420_SHIFTS[3] = { 0, 1, 1 };
    const s32 NV_SHIFTS[2] = { 0, 1 };

    switch (pxconv)
    {
        case PXCONV_YUV420_601:
        case PXCONV_YUV420_709:
        {
            assert(plane < 3);

            *out_width = width >> YUV420_SHIFTS[plane];
            *out_height = height >> YUV420_SHIFTS[plane];
            break;
        }

        case PXCONV_NV12_601:
        case PXCONV_NV21_601:
        case PXCONV_NV12_709:
        case PXCONV_NV21_709:
        {
            assert(plane < 2);

            *out_width = width >> NV_SHIFTS[plane];
            *out_height = height >> NV_SHIFTS[plane];
            break;
        }

        case PXCONV_YUV444_601:
        case PXCONV_YUV444_709:
        {
            assert(plane < 3);

            *out_width = width;
            *out_height = height;
            break;
        }

        case PXCONV_BGR0:
        {
            assert(plane < 1);

            *out_width = width;
            *out_height = height;
            break;
        }

        default:
        {
            assert(false);
            break;
        }
    }
}

// Put textures into system memory.
// The system memory destination msut be big enough to hold all textures.
void download_textures(ID3D11DeviceContext* d3d11_context, ID3D11Texture2D** gpu_texes, CpuTexDl* cpu_texes, s32 num_texes, void* dest, s32 size)
{
    // Need to copy the textures into readable memory.

    for (s32 i = 0; i < num_texes; i++)
    {
        d3d11_context->CopyResource(cpu_texes[i].get_current(), gpu_texes[i]);
    }

    D3D11_MAPPED_SUBRESOURCE* maps = (D3D11_MAPPED_SUBRESOURCE*)_alloca(sizeof(D3D11_MAPPED_SUBRESOURCE) * num_texes);
    void** map_datas = (void**)_alloca(sizeof(void*) * num_texes);
    UINT* row_pitches = (UINT*)_alloca(sizeof(UINT) * num_texes);

    // Mapping will take between 400 and 1500 us on average, not much to do about that. Probably has to do with waiting for CopyResource above to finish.
    // We cannot use D3D11_MAP_FLAG_DO_NOT_WAIT here (and advance the cpu texture queue) because of the CopyResource call above which
    // may not have finished yet.

    for (s32 i = 0; i < num_texes; i++)
    {
        d3d11_context->Map(cpu_texes[i].get_current(), 0, D3D11_MAP_READ, 0, &maps[i]);
    }

    for (s32 i = 0; i < num_texes; i++)
    {
        map_datas[i] = maps[i].pData;
        row_pitches[i] = maps[i].RowPitch;
    }

    // From MSDN:
    // The runtime might assign values to RowPitch and DepthPitch that are larger than anticipated
    // because there might be padding between rows and depth.

    // Mapped data will be aligned to 16 bytes.

    // This will take around 300 us for 1920x1080 YUV420.

    s32 offset = 0;

    for (s32 i = 0; i < num_texes; i++)
    {
        u8* source_ptr = (u8*)map_datas[i];
        u8* dest_ptr = (u8*)dest + offset;

        for (UINT j = 0; j < pxconv_heights[i]; j++)
        {
            memcpy(dest_ptr, source_ptr, pxconv_pitches[i]);

            source_ptr += row_pitches[i];
            dest_ptr += pxconv_pitches[i];
        }

        offset += pxconv_pitches[i] * pxconv_heights[i];
    }

    for (s32 i = 0; i < num_texes; i++)
    {
        d3d11_context->Unmap(cpu_texes[i].get_current(), 0);
    }

    for (s32 i = 0; i < num_texes; i++)
    {
        cpu_texes[i].advance();
    }
}

void free_all_static_sw_stuff()
{
    for (s32 i = 0; i < NUM_PXCONVS; i++)
    {
        svr_maybe_release(&pxconv_cs[i]);
    }
}

void free_all_dynamic_sw_stuff()
{
    for (s32 i = 0; i < used_pxconv_planes; i++)
    {
        svr_maybe_release(&pxconv_texs[i]);
        svr_maybe_release(&pxconv_uavs[i]);

        for (s32 j = 0; j < NUM_BUFFERED_DL_TEXS; j++)
        {
            svr_maybe_release(&pxconv_dls[i].texs[j]);
        }
    }

    for (s32 i = 0; i < MAX_BUFFERED_SEND_BUFS; i++)
    {
        ThreadPipeData& pipe_data = ffmpeg_send_bufs[i];

        if (pipe_data.ptr)
        {
            free(pipe_data.ptr);
        }

        pipe_data = {};
    }
}

// Put texture into video format.
void convert_pixel_formats(ID3D11DeviceContext* d3d11_context, ID3D11ShaderResourceView* source_srv)
{
    d3d11_context->CSSetShader(pxconv_cs[movie_pxconv], NULL, 0);
    d3d11_context->CSSetShaderResources(0, 1, &source_srv);
    d3d11_context->CSSetUnorderedAccessViews(0, used_pxconv_planes, pxconv_uavs, NULL);

    d3d11_context->Dispatch(calc_cs_thread_groups(movie_width), calc_cs_thread_groups(movie_height), 1);

    ID3D11ShaderResourceView* null_srvs[] = { NULL };
    ID3D11UnorderedAccessView* null_uavs[] = { NULL };

    d3d11_context->CSSetShaderResources(0, 1, null_srvs);
    d3d11_context->CSSetUnorderedAccessViews(0, 1, null_uavs, NULL);
}

bool create_pxconv_textures(ID3D11Device* d3d11_device, DXGI_FORMAT* formats)
{
    bool ret = false;
    HRESULT hr;

    PxConvText& text = PXCONV_INI_TEXT_TABLE[movie_pxconv];

    for (s32 i = 0; i < used_pxconv_planes; i++)
    {
        s32 dims[2];
        calc_plane_dims(movie_pxconv, movie_width, movie_height, i, &dims[0], &dims[1]);

        D3D11_TEXTURE2D_DESC tex_desc = {};
        tex_desc.Width = dims[0];
        tex_desc.Height = dims[1];
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = formats[i];
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        tex_desc.CPUAccessFlags = 0;

        hr = d3d11_device->CreateTexture2D(&tex_desc, NULL, &pxconv_texs[i]);

        if (FAILED(hr))
        {
            svr_log("ERROR: Could not create SWENC texture %d for PXCONV %s+%s (%#x)\n", i, text.format, text.color_space, hr);
            goto rfail;
        }

        d3d11_device->CreateUnorderedAccessView(pxconv_texs[i], NULL, &pxconv_uavs[i]);

        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.BindFlags = 0;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        for (s32 j = 0; j < NUM_BUFFERED_DL_TEXS; j++)
        {
            hr = d3d11_device->CreateTexture2D(&tex_desc, NULL, &pxconv_dls[i].texs[j]);

            if (FAILED(hr))
            {
                svr_log("ERROR: Could not create SWENC CPU texture %d (rotation %d) for PXCONV %s+%s (%#x)\n", i, j, text.format, text.color_space, hr);
                goto rfail;
            }
        }
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool start_with_sw(ID3D11Device* d3d11_device)
{
    bool ret = false;

    // We don't allow the pixel format and color space to be selectable anymore, but we will keep the support in for now.

    if (!strcmp(movie_profile.sw_encoder, "libx264"))
    {
        movie_pxconv = PXCONV_NV12_601;
    }

    else if (!strcmp(movie_profile.sw_encoder, "libx264rgb"))
    {
        movie_pxconv = PXCONV_BGR0;
    }

    used_pxconv_planes = calc_format_planes(movie_pxconv);

    DXGI_FORMAT formats[3];

    switch (movie_pxconv)
    {
        case PXCONV_YUV420_601:
        case PXCONV_YUV420_709:
        case PXCONV_YUV444_601:
        case PXCONV_YUV444_709:
        {
            formats[0] = DXGI_FORMAT_R8_UINT;
            formats[1] = DXGI_FORMAT_R8_UINT;
            formats[2] = DXGI_FORMAT_R8_UINT;
            break;
        }

        case PXCONV_NV12_601:
        case PXCONV_NV21_601:
        case PXCONV_NV12_709:
        case PXCONV_NV21_709:
        {
            formats[0] = DXGI_FORMAT_R8_UINT;
            formats[1] = DXGI_FORMAT_R8G8_UINT;
            break;
        }

        case PXCONV_BGR0:
        {
            // RGB only has one plane.
            formats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
            break;
        }
    }

    if (!create_pxconv_textures(d3d11_device, formats))
    {
        goto rfail;
    }

    for (s32 i = 0; i < used_pxconv_planes; i++)
    {
        D3D11_TEXTURE2D_DESC tex_desc;
        pxconv_texs[i]->GetDesc(&tex_desc);

        pxconv_plane_sizes[i] = calc_bytes_pitch(tex_desc.Format) * tex_desc.Width * tex_desc.Height;
        pxconv_widths[i] = tex_desc.Width;
        pxconv_heights[i] = tex_desc.Height;
        pxconv_pitches[i] = calc_bytes_pitch(tex_desc.Format) * tex_desc.Width;
    }

    // Combined size of all planes.
    pxconv_total_plane_sizes = 0;

    for (s32 i = 0; i < used_pxconv_planes; i++)
    {
        pxconv_total_plane_sizes += pxconv_plane_sizes[i];
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

// -------------------------------------------------

// This thread will write data to the ffmpeg process.
// Writing to the pipe is real slow and we want to buffer up a few to send which it can work on.
DWORD WINAPI ffmpeg_thread_proc(LPVOID lpParameter)
{
    while (true)
    {
        svr_sem_wait(&ffmpeg_write_sem);

        ThreadPipeData pipe_data;
        bool res1 = ffmpeg_write_queue.pull(&pipe_data);
        assert(res1);

        if (pipe_data.ptr == NULL)
        {
            return 0;
        }

        // This will not return until the data has been read by the remote process.
        // Writing with pipes is very inconsistent and can wary with several milliseconds.
        // It has been tested to use overlapped I/O with completion routines but that was also too inconsistent and way too complicated.
        // For SW encoding this will take about 300 - 6000 us (always sending the same size).

        // There is some issue with writing with pipes that if it starts off slower than it should be, then it will forever be slow until the computer restarts.
        // Therefore it is useful to measure this.

        svr_start_prof(&write_prof);
        WriteFile(ffmpeg_write_pipe, pipe_data.ptr, pipe_data.size, NULL, NULL);
        svr_end_prof(&write_prof);

        ffmpeg_read_queue.push(&pipe_data);

        svr_sem_release(&ffmpeg_read_sem);
    }

    return 0;
}

bool load_one_shader(const char* name, void* buf, s32 buf_size, DWORD* shader_size)
{
    char full_shader_path[MAX_PATH];
    full_shader_path[0] = 0;
    StringCchCatA(full_shader_path, MAX_PATH, svr_resource_path);
    StringCchCatA(full_shader_path, MAX_PATH, "\\data\\shaders\\");
    StringCchCatA(full_shader_path, MAX_PATH, name);

    HANDLE h = CreateFileA(full_shader_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE)
    {
        svr_log("Could not load shader %s (%lu)\n", name, GetLastError());
        return false;
    }

    ReadFile(h, buf, buf_size, shader_size, NULL);

    CloseHandle(h);

    return true;
}

bool create_a_cs_shader(const char* name, void* file_mem, s32 file_mem_size, ID3D11Device* d3d11_device, ID3D11ComputeShader** cs)
{
    bool ret = false;
    DWORD shader_size;
    HRESULT hr;

    if (!load_one_shader(name, file_mem, file_mem_size, &shader_size))
    {
        goto rfail;
    }

    hr = d3d11_device->CreateComputeShader(file_mem, shader_size, NULL, cs);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create shader %s (%#x)", name, hr);
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool create_shaders(ID3D11Device* d3d11_device)
{
    const s32 SHADER_MEM_SIZE = 8192;

    bool ret = false;
    DWORD shader_size;
    HRESULT hr;

    void* file_mem = malloc(SHADER_MEM_SIZE);

    // Pixel format conversion shaders are only used in SW encoding.

    for (s32 i = 0; i < NUM_PXCONVS; i++)
    {
        if (!load_one_shader(PXCONV_SHADER_NAMES[i], file_mem, SHADER_MEM_SIZE, &shader_size))
        {
            goto rfail;
        }

        hr = d3d11_device->CreateComputeShader(file_mem, shader_size, NULL, &pxconv_cs[i]);

        if (FAILED(hr))
        {
            svr_log("Could not create pxconv shader %d (%#x)\n", i, hr);
            goto rfail;
        }
    }

    if (!load_one_shader("c52620855f15b2c47b8ca24b890850a90fdc7017", file_mem, SHADER_MEM_SIZE, &shader_size))
    {
        goto rfail;
    }

    hr = d3d11_device->CreateComputeShader(file_mem, shader_size, NULL, &mosample_cs);

    if (FAILED(hr))
    {
        svr_log("Could not create mosample shader (%#x\n", hr);
        goto rfail;
    }

    if (!load_one_shader("34e7f561dcf7ccdd3b8f1568ebdbf4299b54f07d", file_mem, SHADER_MEM_SIZE, &shader_size))
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    free(file_mem);

    return ret;
}

void free_all_static_proc_stuff()
{
    svr_maybe_release(&mosample_cs);
    svr_maybe_release(&mosample_cb);

    svr_maybe_release(&d2d1_factory);
    svr_maybe_release(&d2d1_device);
    svr_maybe_release(&d2d1_context);
    svr_maybe_release(&dwrite_factory);
    svr_maybe_release(&d2d1_solid_brush);

    free(wav_buf);
    wav_buf = NULL;
}

void free_all_dynamic_proc_stuff()
{
    svr_maybe_release(&work_tex);
    svr_maybe_release(&work_tex_rtv);
    svr_maybe_release(&work_tex_srv);
    svr_maybe_release(&work_tex_uav);

    svr_maybe_release(&velo_rtv_ref);
    svr_maybe_release(&velo_font_face);

    if (wav_f)
    {
        CloseHandle(wav_f);
        wav_f = NULL;
    }
}

bool proc_init(const char* svr_path, ID3D11Device* d3d11_device)
{
    bool ret = false;
    HRESULT hr;
    D3D11_BUFFER_DESC mosample_cb_desc = {};

    StringCchCopyA(svr_resource_path, MAX_PATH, svr_path);

    IDXGIDevice* dxgi_device;
    d3d11_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));

    // We use D2D1 only for rasterizing vector fonts.

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d1_factory));

    if (FAILED(hr))
    {
        svr_log("ERROR: D2D1CreateFactory returned %#x\n", hr);
        goto rfail;
    }

    hr = d2d1_factory->CreateDevice(dxgi_device, &d2d1_device);

    if (FAILED(hr))
    {
        svr_log("ERROR: ID2D1Factory::CreateDevice returned %#x\n", hr);
        goto rfail;
    }

    hr = d2d1_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d1_context);

    if (FAILED(hr))
    {
        svr_log("ERROR: ID2D1Device::CreateDeviceContext returned %#x\n", hr);
        goto rfail;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwrite_factory);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create dwrite factory (#x)\n", hr);
        goto rfail;
    }

    d2d1_context->CreateSolidColorBrush({}, &d2d1_solid_brush);

    // Useful for debugging issues with velo rasterization.
    // d2d1_context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    mosample_cb_desc.ByteWidth = sizeof(MosampleCb);
    mosample_cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    mosample_cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    mosample_cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    mosample_cb_desc.MiscFlags = 0;

    hr = d3d11_device->CreateBuffer(&mosample_cb_desc, NULL, &mosample_cb);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create mosample constant buffer (%#x)\n", hr);
        goto rfail;
    }

    if (!create_shaders(d3d11_device))
    {
        goto rfail;
    }

    ffmpeg_write_queue.init(MAX_BUFFERED_SEND_BUFS);
    ffmpeg_read_queue.init(MAX_BUFFERED_SEND_BUFS);

    wav_buf = (SvrWaveSample*)_aligned_malloc(sizeof(SvrWaveSample) * WAV_BUFFERED_SAMPLES, 16);

    ret = true;
    goto rexit;

rfail:
    free_all_static_proc_stuff();

rexit:
    dxgi_device->Release();

    return ret;
}

void build_ffmpeg_process_args(char* full_args, s32 full_args_size)
{
    const s32 ARGS_BUF_SIZE = 128;

    char buf[ARGS_BUF_SIZE];

    StringCchCatA(full_args, full_args_size, "-hide_banner");

    #if 1
    StringCchCatA(full_args, full_args_size, " -loglevel quiet");
    #else
    StringCchCatA(full_args, full_args_size, " -loglevel debug");
    #endif

    // Parameters below here is regarding the input (the stuff we are sending).

    PxConvText& pxconv_text = PXCONV_FFMPEG_TEXT_TABLE[movie_pxconv];

    // We are sending uncompressed frames.
    StringCchCatA(full_args, full_args_size, " -f rawvideo -vcodec rawvideo");

    // Pixel format that goes through the pipe.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -pix_fmt %s", pxconv_text.format);
    StringCchCatA(full_args, full_args_size, buf);

    // Video size.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -s %dx%d", movie_width, movie_height);
    StringCchCatA(full_args, full_args_size, buf);

    // Input frame rate.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -r %d", movie_profile.movie_fps);
    StringCchCatA(full_args, full_args_size, buf);

    // Overwrite existing, and read from stdin.
    StringCchCatA(full_args, full_args_size, " -y -i -");

    // Parameters below here is regarding the output (the stuff that will be written to the file).

    // Number of encoding threads, or 0 for auto.
    // We used to allow this to be configured, its intended purpose was for game multiprocessing (opening multiple games) but
    // there are too many problems in the Source engine that we cannot control. It leads to many buggy and weird scenarios (like animations not playing or demos jumping).
    StringCchCatA(full_args, full_args_size, " -threads 0");

    // Output video codec.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -vcodec %s", movie_profile.sw_encoder);
    StringCchCatA(full_args, full_args_size, buf);

    if (pxconv_text.color_space)
    {
        // Output video color space (only for YUV).
        StringCchPrintfA(buf, ARGS_BUF_SIZE, " -colorspace %s", pxconv_text.color_space);
        StringCchCatA(full_args, full_args_size, buf);
    }

    // Output video framerate.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -framerate %d", movie_profile.movie_fps);
    StringCchCatA(full_args, full_args_size, buf);

    // Output quality factor.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -crf %d", movie_profile.sw_crf);
    StringCchCatA(full_args, full_args_size, buf);

    // Output x264 preset.
    StringCchPrintfA(buf, ARGS_BUF_SIZE, " -preset %s", movie_profile.sw_x264_preset);
    StringCchCatA(full_args, full_args_size, buf);

    if (movie_profile.sw_x264_intra)
    {
        StringCchCatA(full_args, full_args_size, " -x264-params keyint=1");
    }

    // The path can be specified as relative here because we set the working directory of the ffmpeg process
    // to the SVR directory.

    StringCchCatA(full_args, full_args_size, " \"");
    StringCchCatA(full_args, full_args_size, movie_path);
    StringCchCatA(full_args, full_args_size, "\"");
}

// We start a separate process for two reasons:
// 1) Source is a 32-bit engine, and it was common to run out of memory in games such as CSGO that uses a lot of memory.
// 2) The ffmpeg API is horrible to work with with an incredible amount of pitfalls that will grant you a media that is slighly incorrect
//    and there is no reliable documentation.
// Data is sent to this process through a pipe that we create.
// For SW encoding we send uncompressed frames which are then encoded and muxed in the ffmpeg process.
bool start_ffmpeg_proc()
{
    const s32 FULL_ARGS_SIZE = 1024;

    bool ret = false;

    STARTUPINFOA start_info = {};
    DWORD create_flags = 0;

    char full_args[FULL_ARGS_SIZE];
    full_args[0] = 0;

    HANDLE read_h = NULL;
    HANDLE write_h = NULL;

    SECURITY_ATTRIBUTES sa;
    PROCESS_INFORMATION proc_info;

    char full_ffmpeg_path[MAX_PATH];

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&read_h, &write_h, &sa, 4 * 1024))
    {
        svr_log("ERROR: Could not create ffmpeg process pipes (%lu)\n", GetLastError());
        goto rfail;
    }

    // Since we start the ffmpeg process with inherited handles, it would try to inherit the writing endpoint of this pipe too.
    // We have to remove the inheritance of this pipe. Otherwise it's never able to exit.
    SetHandleInformation(write_h, HANDLE_FLAG_INHERIT, 0);

    #if 1
    create_flags |= CREATE_NO_WINDOW;
    #endif

    // Working directory for the FFmpeg process should be in the SVR directory.

    full_ffmpeg_path[0] = 0;
    StringCchCatA(full_ffmpeg_path, MAX_PATH, svr_resource_path);
    StringCchCatA(full_ffmpeg_path, MAX_PATH, "\\ffmpeg.exe");

    start_info.cb = sizeof(STARTUPINFOA);
    start_info.hStdInput = read_h;
    start_info.dwFlags |= STARTF_USESTDHANDLES;

    build_ffmpeg_process_args(full_args, FULL_ARGS_SIZE);

    if (!CreateProcessA(full_ffmpeg_path, full_args, NULL, NULL, TRUE, create_flags, NULL, svr_resource_path, &start_info, &proc_info))
    {
        svr_log("ERROR: Could not create ffmpeg process (%lu)\n", GetLastError());
        goto rfail;
    }

    ffmpeg_proc = proc_info.hProcess;
    CloseHandle(proc_info.hThread);

    ffmpeg_write_pipe = write_h;

    ret = true;
    goto rexit;

rfail:
    if (write_h) CloseHandle(write_h);

rexit:
    if (read_h) CloseHandle(read_h);

    return ret;
}

void end_ffmpeg_proc()
{
    svr_sem_wait(&ffmpeg_read_sem);

    ThreadPipeData pipe_data;
    pipe_data.ptr = NULL;
    pipe_data.size = 0;

    ffmpeg_write_queue.push(&pipe_data);

    svr_sem_release(&ffmpeg_write_sem);

    WaitForSingleObject(ffmpeg_thread, INFINITE);

    CloseHandle(ffmpeg_thread);
    ffmpeg_thread = NULL;

    // Both queues should not be updated anymore at this point so they should be the same when observed.
    // Since we exit with a sentinel value, the semaphores will be out of sync from the queues but they are reinit on movie start.
    assert(ffmpeg_write_queue.read_buffer_health() == 0);
    assert(ffmpeg_read_queue.read_buffer_health() == MAX_BUFFERED_SEND_BUFS);

    // Close our end of the pipe.
    // This will mark the completion of the stream, and the process will finish its work.
    CloseHandle(ffmpeg_write_pipe);
    ffmpeg_write_pipe = NULL;

    WaitForSingleObject(ffmpeg_proc, INFINITE);

    CloseHandle(ffmpeg_proc);
    ffmpeg_proc = NULL;
}

s32 to_utf16(const char* value, s32 value_length, wchar* buf, s32 buf_chars)
{
    s32 length = MultiByteToWideChar(CP_UTF8, 0, value, value_length, buf, buf_chars);

    if (length < buf_chars)
    {
        buf[length] = 0;
    }

    return length;
}

// Try to find the font in the system.
bool create_velo_font_face(MovieProfile* p, IDWriteFontFace** font_face)
{
    bool ret = false;

    wchar stupid_buf[128];
    to_utf16(p->veloc_font, strlen(p->veloc_font), stupid_buf, 128);

    HRESULT hr;
    IDWriteFontCollection* coll = NULL;
    IDWriteFontFamily* font_fam = NULL;
    IDWriteFont* font = NULL;

    dwrite_factory->GetSystemFontCollection(&coll, FALSE);

    UINT font_index;
    BOOL font_exists;
    coll->FindFamilyName(stupid_buf, &font_index, &font_exists);

    if (!font_exists)
    {
        game_log("The specified velo font %s is not installed in the system\n", p->veloc_font);
        goto rfail;
    }

    coll->GetFontFamily(font_index, &font_fam);

    hr = font_fam->GetFirstMatchingFont(p->veloc_font_weight, DWRITE_FONT_STRETCH_NORMAL, p->veloc_font_style, &font);

    if (FAILED(hr))
    {
        game_log("Could not find the combination of font parameters (weight, stretch, style) in the font %s\n", p->veloc_font);
        goto rfail;
    }

    font->CreateFontFace(font_face);

    ret = true;
    goto rexit;

rfail:
rexit:
    svr_maybe_release(&coll);
    svr_maybe_release(&font_fam);
    svr_maybe_release(&font);

    return ret;
}

bool create_velo(ID3D11Device* d3d11_device, ID3D11DeviceContext* d3d11_context, ID3D11RenderTargetView* used_rtv)
{
    bool ret = false;
    HRESULT hr;

    if (!create_velo_font_face(&movie_profile, &velo_font_face))
    {
        goto rfail;
    }

    ID3D11Resource* content_tex_res;
    used_rtv->GetResource(&content_tex_res);

    ID3D11Texture2D* content_tex;
    content_tex_res->QueryInterface(IID_PPV_ARGS(&content_tex));

    IDXGISurface* content_surface;
    content_tex->QueryInterface(IID_PPV_ARGS(&content_surface));

    // Create passthrough reference to the used render target. This is not a real texture.
    hr = d2d1_context->CreateBitmapFromDxgiSurface(content_surface, NULL, &velo_rtv_ref);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create SRV passthrough (%#x)\n", hr);
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:
rexit:
    content_tex_res->Release();
    content_tex->Release();
    content_surface->Release();

    return ret;
}

#define ALLOCA(T) (T*)_alloca(sizeof(T))
#define ALLOCA_NUM(T, NUM) (T*)_alloca(sizeof(T) * NUM)

D2D1_COLOR_F fill_d2d1_color(s32* color)
{
    D2D1_COLOR_F ret;
    ret.r = color[0] / 255.0f;
    ret.g = color[1] / 255.0f;
    ret.b = color[2] / 255.0f;
    ret.a = color[3] / 255.0f;
    return ret;
}

D2D1_POINT_2F fill_d2d1_pt(XMINT2 p)
{
    return D2D1::Point2F(p.x, p.y);
}

void draw_velo(const wchar* text, XMINT2 pos)
{
    s32 text_length = wcslen(text);

    d2d1_context->BeginDraw();
    d2d1_context->SetTarget(velo_rtv_ref);

    UINT32* cps = ALLOCA_NUM(UINT32, text_length);
    UINT16* idxs = ALLOCA_NUM(UINT16, text_length);
    FLOAT* advances = ALLOCA_NUM(FLOAT, text_length);
    DWRITE_GLYPH_METRICS* glyph_metrix = ALLOCA_NUM(DWRITE_GLYPH_METRICS, text_length);

    for (s32 i = 0; i < text_length; i++)
    {
        cps[i] = text[i];
    }

    velo_font_face->GetGlyphIndicesW(cps, text_length, idxs);
    velo_font_face->GetDesignGlyphMetrics(idxs, text_length, glyph_metrix, FALSE);

    DWRITE_FONT_METRICS font_metrix;
    velo_font_face->GetMetrics(&font_metrix);

    float scale = (float)movie_profile.veloc_font_size / (float)font_metrix.designUnitsPerEm;

    // Emulation of tabular font feature where every number is monospaced.
    // For the full feature, the font itself also changes its shaping for the glyphs to be wider, but that is not important.
    // This also prevents the text from jittering when it changes during the centering logic. Typically caused by the 1 character sometimes being thinner than other characters.
    UINT32 tab_cp = L'0';

    UINT16 tab_idx;
    DWRITE_GLYPH_METRICS tab_metrix;

    velo_font_face->GetGlyphIndicesW(&tab_cp, 1, &tab_idx);
    velo_font_face->GetDesignGlyphMetrics(&tab_idx, 1, &tab_metrix, FALSE);

    for (s32 i = 0; i < text_length; i++)
    {
        float aw = tab_metrix.advanceWidth * scale;
        advances[i] = aw;
    }

    // Base horizontal positioning from the center.
    // Vertical positioning is done from the baseline.

    float w = 0.0f;

    for (s32 i = 0; i < text_length; i++)
    {
        w += advances[i];
    }

    s32 real_w = (s32)ceilf(w);

    s32 shift_x = real_w / 2;
    pos.x -= shift_x;

    DWRITE_GLYPH_RUN run = {};
    run.fontFace = velo_font_face;
    run.fontEmSize = movie_profile.veloc_font_size;
    run.glyphCount = text_length;
    run.glyphIndices = idxs;
    run.glyphAdvances = advances;

    ID2D1PathGeometry* geom;
    d2d1_factory->CreatePathGeometry(&geom);

    ID2D1GeometrySink* sink;
    geom->Open(&sink);

    velo_font_face->GetGlyphRunOutline(movie_profile.veloc_font_size, idxs, advances, NULL, text_length, FALSE, FALSE, sink);

    sink->Close();

    d2d1_context->SetTransform(D2D1::Matrix3x2F::Translation(pos.x, pos.y));

    d2d1_solid_brush->SetColor(fill_d2d1_color(movie_profile.veloc_font_color));
    d2d1_context->FillGeometry(geom, d2d1_solid_brush);

    if (movie_profile.veloc_font_border_size > 0)
    {
        d2d1_solid_brush->SetColor(fill_d2d1_color(movie_profile.veloc_font_border_color));
        d2d1_context->DrawGeometry(geom, d2d1_solid_brush, movie_profile.veloc_font_border_size);
    }

    geom->Release();
    sink->Release();

    d2d1_context->EndDraw();
    d2d1_context->SetTarget(NULL);
}

bool create_audio()
{
    char wav_path[MAX_PATH];
    StringCchCopyA(wav_path, MAX_PATH, movie_path);
    PathRenameExtensionA(wav_path, ".wav");

    wav_f = CreateFileA(wav_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (wav_f == INVALID_HANDLE_VALUE)
    {
        game_log("Could not create wave file %s (%lu)\n", wav_path, GetLastError());
        return false;
    }

    const DWORD RIFF = MAKEFOURCC('R', 'I', 'F', 'F');
    const DWORD WAVE = MAKEFOURCC('W', 'A', 'V', 'E');
    const DWORD FMT_ = MAKEFOURCC('f', 'm', 't', ' ');
    const DWORD DATA = MAKEFOURCC('d', 'a', 't', 'a');

    const DWORD WAV_PLACEHOLDER = 0;

    WriteFile(wav_f, &RIFF, sizeof(DWORD), NULL, NULL);
    wav_header_pos = SetFilePointer(wav_f, 0, NULL, FILE_CURRENT);
    WriteFile(wav_f, &WAV_PLACEHOLDER, sizeof(DWORD), NULL, NULL);

    WriteFile(wav_f, &WAVE, sizeof(DWORD), NULL, NULL);

    WORD channels = 2;
    WORD sample_rate = 44100;
    WORD sample_bits = 16;

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = sample_bits;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    DWORD wfx_size = sizeof(WAVEFORMATEX);

    WriteFile(wav_f, &FMT_, sizeof(DWORD), NULL, NULL);
    WriteFile(wav_f, &wfx_size, sizeof(DWORD), NULL, NULL);
    WriteFile(wav_f, &wfx, sizeof(WAVEFORMATEX), NULL, NULL);

    WriteFile(wav_f, &DATA, sizeof(DWORD), NULL, NULL);
    wav_data_pos = SetFilePointer(wav_f, 0, NULL, FILE_CURRENT);
    WriteFile(wav_f, &WAV_PLACEHOLDER, sizeof(DWORD), NULL, NULL);

    wav_file_length = SetFilePointer(wav_f, 0, NULL, FILE_CURRENT);

    return true;
}

bool proc_start(ID3D11Device* d3d11_device, ID3D11DeviceContext* d3d11_context, const char* dest, const char* profile, ID3D11RenderTargetView* game_content_rtv)
{
    bool ret = false;
    HRESULT hr;

    ID3D11RenderTargetView* used_rtv = game_content_rtv;

    if (*profile == 0)
    {
        profile = "default";
    }

    char full_profile_path[MAX_PATH];
    full_profile_path[0] = 0;
    StringCchCatA(full_profile_path, MAX_PATH, svr_resource_path);
    StringCchCatA(full_profile_path, MAX_PATH, "\\data\\profiles\\");
    StringCchCatA(full_profile_path, MAX_PATH, profile);
    StringCchCatA(full_profile_path, MAX_PATH, ".ini");

    if (!read_profile(full_profile_path, &movie_profile))
    {
        goto rfail;
    }

    ID3D11Resource* content_tex_res;
    game_content_rtv->GetResource(&content_tex_res);

    ID3D11Texture2D* content_tex;
    content_tex_res->QueryInterface(IID_PPV_ARGS(&content_tex));

    D3D11_TEXTURE2D_DESC tex_desc;
    content_tex->GetDesc(&tex_desc);

    content_tex_res->Release();
    content_tex->Release();

    movie_width = tex_desc.Width;
    movie_height = tex_desc.Height;

    movie_path[0] = 0;
    StringCchCatA(movie_path, MAX_PATH, svr_resource_path);
    StringCchCatA(movie_path, MAX_PATH, "\\movies\\");
    CreateDirectoryA(movie_path, NULL);
    StringCchCatA(movie_path, MAX_PATH, dest);

    if (!start_with_sw(d3d11_device))
    {
        goto rfail;
    }

    if (movie_profile.mosample_enabled)
    {
        D3D11_TEXTURE2D_DESC work_tex_desc = {};
        work_tex_desc.Width = tex_desc.Width;
        work_tex_desc.Height = tex_desc.Height;
        work_tex_desc.MipLevels = 1;
        work_tex_desc.ArraySize = 1;
        work_tex_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        work_tex_desc.SampleDesc.Count = 1;
        work_tex_desc.Usage = D3D11_USAGE_DEFAULT;
        work_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        work_tex_desc.CPUAccessFlags = 0;

        hr = d3d11_device->CreateTexture2D(&work_tex_desc, NULL, &work_tex);

        if (FAILED(hr))
        {
            svr_log("ERROR: Could not create work texture (%#x)\n", hr);
            goto rfail;
        }

        d3d11_device->CreateShaderResourceView(work_tex, NULL, &work_tex_srv);
        d3d11_device->CreateRenderTargetView(work_tex, NULL, &work_tex_rtv);
        d3d11_device->CreateUnorderedAccessView(work_tex, NULL, &work_tex_uav);

        used_rtv = work_tex_rtv;
    }

    if (movie_profile.veloc_enabled)
    {
        if (!create_velo(d3d11_device, d3d11_context, used_rtv))
        {
            goto rfail;
        }
    }

    if (movie_profile.audio_enabled)
    {
        if (!create_audio())
        {
            goto rfail;
        }
    }

    // We have a controlled environment until the ffmpeg thread is started.
    // Set the semaphore and queues to known states.

    svr_sem_init(&ffmpeg_write_sem, 0, MAX_BUFFERED_SEND_BUFS);
    svr_sem_init(&ffmpeg_read_sem, MAX_BUFFERED_SEND_BUFS, MAX_BUFFERED_SEND_BUFS);

    // Need to overwrite with new data.
    ffmpeg_read_queue.reset();
    ffmpeg_write_queue.reset();

    // Each buffer contains 1 uncompressed frame.

    for (s32 i = 0; i < MAX_BUFFERED_SEND_BUFS; i++)
    {
        ThreadPipeData pipe_data;
        pipe_data.ptr = (u8*)malloc(pxconv_total_plane_sizes);
        pipe_data.size = pxconv_total_plane_sizes;

        ffmpeg_send_bufs[i] = pipe_data;
    }

    for (s32 i = 0; i < MAX_BUFFERED_SEND_BUFS; i++)
    {
        ffmpeg_read_queue.push(&ffmpeg_send_bufs[i]);
    }

    if (movie_profile.mosample_enabled)
    {
        mosample_remainder = 0.0f;

        s32 sps = movie_profile.movie_fps * movie_profile.mosample_mult;
        mosample_remainder_step = (1.0f / sps) / (1.0f / movie_profile.movie_fps);
    }

    if (!start_ffmpeg_proc())
    {
        goto rfail;
    }

    _ReadWriteBarrier();

    ffmpeg_thread = CreateThread(NULL, 0, ffmpeg_thread_proc, NULL, 0, NULL);

    ret = true;
    goto rexit;

rfail:
    free_all_dynamic_proc_stuff();

rexit:
    return ret;
}

// For SW encoding, send uncompressed frame over pipe.
void send_converted_video_frame_to_ffmpeg(ID3D11DeviceContext* d3d11_context)
{
    svr_sem_wait(&ffmpeg_read_sem);

    ThreadPipeData pipe_data;
    bool res1 = ffmpeg_read_queue.pull(&pipe_data);
    assert(res1);

    svr_start_prof(&dl_prof);
    download_textures(d3d11_context, pxconv_texs, pxconv_dls, used_pxconv_planes, pipe_data.ptr, pipe_data.size);
    svr_end_prof(&dl_prof);

    ffmpeg_write_queue.push(&pipe_data);

    svr_sem_release(&ffmpeg_write_sem);
}

void motion_sample(ID3D11DeviceContext* d3d11_context, ID3D11ShaderResourceView* game_content_srv, float weight)
{
    if (weight != mosample_weight_cache)
    {
        MosampleCb cb_data;
        cb_data.mosample_weight = weight;

        mosample_weight_cache = weight;
        update_constant_buffer(d3d11_context, mosample_cb, &cb_data, sizeof(MosampleCb));
    }

    d3d11_context->CSSetShader(mosample_cs, NULL, 0);
    d3d11_context->CSSetShaderResources(0, 1, &game_content_srv);

    d3d11_context->CSSetConstantBuffers(0, 1, &mosample_cb);
    d3d11_context->CSSetUnorderedAccessViews(0, 1, &work_tex_uav, NULL);

    d3d11_context->Dispatch(calc_cs_thread_groups(movie_width), calc_cs_thread_groups(movie_height), 1);

    svr_start_prof(&mosample_prof);
    d3d11_context->Flush();
    svr_end_prof(&mosample_prof);

    ID3D11ShaderResourceView* null_srv = NULL;
    ID3D11UnorderedAccessView* null_uav = NULL;

    d3d11_context->CSSetShaderResources(0, 1, &null_srv);
    d3d11_context->CSSetUnorderedAccessViews(0, 1, &null_uav, NULL);
}

// Percentage alignments based from the center of the screen.
XMINT2 get_velo_pos()
{
    s32 scr_pos_x = movie_width / 2;
    s32 scr_pos_y = movie_height / 2;

    scr_pos_x += (movie_profile.veloc_align[0] / 200.0f) * movie_width;
    scr_pos_y += (movie_profile.veloc_align[1] / 200.0f) * movie_height;

    return XMINT2(scr_pos_x, scr_pos_y);
}

void encode_video_frame(ID3D11DeviceContext* d3d11_context, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv)
{
    if (movie_profile.veloc_enabled)
    {
        // We only deal with XY velo.
        s32 vel = (s32)(sqrt(player_velo[0] * player_velo[0] + player_velo[1] * player_velo[1]) + 0.5f);

        wchar buf[128];
        StringCchPrintfW(buf, 128, L"%d", vel);

        draw_velo(buf, get_velo_pos());
    }

    convert_pixel_formats(d3d11_context, srv);
    send_converted_video_frame_to_ffmpeg(d3d11_context);
}

void mosample_game_frame(ID3D11DeviceContext* d3d11_context, ID3D11ShaderResourceView* game_content_srv)
{
    float old_rem = mosample_remainder;
    float exposure = movie_profile.mosample_exposure;

    mosample_remainder += mosample_remainder_step;

    if (mosample_remainder <= (1.0f - exposure))
    {

    }

    else if (mosample_remainder < 1.0f)
    {
        float weight = (mosample_remainder - svr_max(1.0f - exposure, old_rem)) * (1.0f / exposure);
        motion_sample(d3d11_context, game_content_srv, weight);
    }

    else
    {
        float weight = (1.0f - svr_max(1.0f - exposure, old_rem)) * (1.0f / exposure);
        motion_sample(d3d11_context, game_content_srv, weight);

        encode_video_frame(d3d11_context, work_tex_srv, work_tex_rtv);

        mosample_remainder -= 1.0f;

        s32 additional = mosample_remainder;

        if (additional > 0)
        {
            for (s32 i = 0; i < additional; i++)
            {
                encode_video_frame(d3d11_context, work_tex_srv, work_tex_rtv);
            }

            mosample_remainder -= additional;
        }

        float clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        d3d11_context->ClearRenderTargetView(work_tex_rtv, clear_color);

        if (mosample_remainder > FLT_EPSILON && mosample_remainder > (1.0f - exposure))
        {
            weight = ((mosample_remainder - (1.0f - exposure)) * (1.0f / exposure));
            motion_sample(d3d11_context, game_content_srv, weight);
        }
    }
}

void proc_frame(ID3D11DeviceContext* d3d11_context, ID3D11ShaderResourceView* game_content_srv, ID3D11RenderTargetView* game_content_rtv)
{
    svr_start_prof(&frame_prof);

    if (movie_profile.mosample_enabled)
    {
        mosample_game_frame(d3d11_context, game_content_srv);
    }

    else
    {
        encode_video_frame(d3d11_context, game_content_srv, game_content_rtv);
    }

    svr_end_prof(&frame_prof);
}

void proc_give_velocity(float* xyz)
{
    memcpy(player_velo, xyz, sizeof(float) * 3);
}

bool proc_is_velo_enabled()
{
    return movie_profile.veloc_enabled;
}

bool proc_is_audio_enabled()
{
    return movie_profile.audio_enabled;
}

void write_wav_samples()
{
    s32 buf_size = sizeof(SvrWaveSample) * wav_num_samples;

    wav_data_length += buf_size;
    wav_file_length += buf_size;

    WriteFile(wav_f, wav_buf, buf_size, NULL, NULL);

    wav_num_samples = 0;
}

void proc_give_audio(SvrWaveSample* samples, s32 num_samples)
{
    if (wav_num_samples + num_samples >= WAV_BUFFERED_SAMPLES)
    {
        write_wav_samples();
    }

    memcpy(wav_buf + wav_num_samples, samples, sizeof(SvrWaveSample) * num_samples);
    wav_num_samples += num_samples;
}

void show_total_prof(const char* name, SvrProf* prof)
{
    game_log("%s: %lld\n", name, prof->total);
}

void show_prof(const char* name, SvrProf* prof)
{
    if (prof->runs > 0)
    {
        game_log("%s: %lld\n", name, prof->total / prof->runs);
    }
}

void end_audio()
{
    if (wav_num_samples > 0)
    {
        write_wav_samples();
    }

    SetFilePointer(wav_f, wav_header_pos, NULL, FILE_BEGIN);
    WriteFile(wav_f, &wav_file_length, sizeof(DWORD), NULL, NULL);

    SetFilePointer(wav_f, wav_data_pos, NULL, FILE_BEGIN);
    WriteFile(wav_f, &wav_data_length, sizeof(DWORD), NULL, NULL);

    wav_data_length = 0;
    wav_file_length = 0;
    wav_header_pos = 0;
    wav_data_pos = 0;
    wav_num_samples = 0;
}

void proc_end()
{
    end_ffmpeg_proc();

    if (movie_profile.audio_enabled)
    {
        end_audio();
    }

    free_all_dynamic_sw_stuff();
    free_all_dynamic_proc_stuff();

    #if SVR_PROF
    show_total_prof("Total work time", &frame_prof);
    show_prof("Download", &dl_prof);
    show_prof("Write", &write_prof);
    show_prof("Mosample", &mosample_prof);
    #endif

    svr_reset_prof(&frame_prof);
    svr_reset_prof(&dl_prof);
    svr_reset_prof(&write_prof);
    svr_reset_prof(&mosample_prof);
}

s32 proc_get_game_rate()
{
    if (movie_profile.mosample_enabled)
    {
        return movie_profile.movie_fps * movie_profile.mosample_mult;
    }

    return movie_profile.movie_fps;
}
