#include "encoder_priv.h"

// Conversion from game texture format to video encoder format.

const s32 VID_SHADER_SIZE = 8192; // Max size one shader can be when loading.

bool EncoderState::vid_init()
{
    bool ret = false;
    HRESULT hr;

    if (!vid_create_device())
    {
        goto rfail;
    }

    if (!vid_create_shaders())
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool EncoderState::vid_create_device()
{
    bool ret = false;
    HRESULT hr;

    UINT device_create_flags = D3D11_CREATE_DEVICE_SINGLETHREADED;

    #if SVR_DEBUG
    device_create_flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif

    // Should be good enough for all the features that we make use of.
    const D3D_FEATURE_LEVEL MINIMUM_DEVICE_LEVEL = D3D_FEATURE_LEVEL_12_0;

    D3D_FEATURE_LEVEL created_device_level;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, device_create_flags, &MINIMUM_DEVICE_LEVEL, 1, D3D11_SDK_VERSION, &vid_d3d11_device, &created_device_level, &vid_d3d11_context);

    if (FAILED(hr))
    {
        error("ERROR: Could not create D3D11 device (%#x)\n", hr);
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool EncoderState::vid_create_shaders()
{
    bool ret = false;

    vid_shader_mem = malloc(VID_SHADER_SIZE);

    if (!vid_create_shader("convert_nv12", (void**)&vid_nv12_cs, D3D11_COMPUTE_SHADER))
    {
        goto rfail;
    }

    if (!vid_create_shader("convert_yuv422", (void**)&vid_yuv422_cs, D3D11_COMPUTE_SHADER))
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    free(vid_shader_mem);
    return ret;
}

void EncoderState::vid_free_static()
{
    svr_maybe_release(&vid_d3d11_device);
    svr_maybe_release(&vid_d3d11_context);

    svr_maybe_release(&vid_nv12_cs);
    svr_maybe_release(&vid_yuv422_cs);
}

void EncoderState::vid_free_dynamic()
{
    svr_maybe_release(&vid_game_tex);
    svr_maybe_release(&vid_game_tex_srv);

    for (s32 i = 0; i < VID_MAX_PLANES; i++)
    {
        svr_maybe_release(&vid_converted_texs[i]);
    }

    vid_conversion_cs = NULL;
    vid_num_planes = 0;
}

bool EncoderState::vid_load_shader(const char* name)
{
    bool ret = false;

    char full_shader_path[MAX_PATH];
    snprintf(full_shader_path, SVR_ARRAY_SIZE(full_shader_path), "data\\shaders\\%s", name);

    HANDLE h = CreateFileA(full_shader_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE)
    {
        error("Could not load shader %s (%lu)\n", name, GetLastError());
        goto rfail;
    }

    DWORD shader_size;
    ReadFile(h, vid_shader_mem, VID_SHADER_SIZE, &shader_size, NULL);

    vid_shader_size = shader_size;

    ret = true;
    goto rexit;

rfail:

rexit:
    if (h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(h);
    }

    return ret;
}

bool EncoderState::vid_create_shader(const char* name, void** shader, D3D11_SHADER_TYPE type)
{
    bool ret = false;
    HRESULT hr;

    if (!vid_load_shader(name))
    {
        goto rfail;
    }

    switch (type)
    {
        case D3D11_COMPUTE_SHADER:
        {
            hr = vid_d3d11_device->CreateComputeShader(vid_shader_mem, vid_shader_size, NULL, (ID3D11ComputeShader**)shader);
            break;
        }

        case D3D11_PIXEL_SHADER:
        {
            hr = vid_d3d11_device->CreatePixelShader(vid_shader_mem, vid_shader_size, NULL, (ID3D11PixelShader**)shader);
            break;
        }

        case D3D11_VERTEX_SHADER:
        {
            hr = vid_d3d11_device->CreateVertexShader(vid_shader_mem, vid_shader_size, NULL, (ID3D11VertexShader**)shader);
            break;
        }
    }

    if (FAILED(hr))
    {
        error("ERROR: Could not create shader %s (%#x)\n", name, hr);
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool EncoderState::vid_start()
{
    bool ret = false;

    if (!vid_open_game_texture())
    {
        goto rfail;
    }

    vid_create_conversion_texs();

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool EncoderState::vid_open_game_texture()
{
    bool ret = false;
    HRESULT hr;

    hr = vid_d3d11_device->OpenSharedResource((HANDLE)shared_mem_ptr->game_texture_h, IID_PPV_ARGS(&vid_game_tex));

    if (FAILED(hr))
    {
        error("ERROR: Could not open the shared svr_game texture (%#x)\n", hr);
        goto rfail;
    }

    vid_d3d11_device->CreateShaderResourceView(vid_game_tex, NULL, &vid_game_tex_srv);

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

struct VidPlaneShifts
{
    s32 x;
    s32 y;
};

// Setup state and create the textures in the format that can be sent to the encoder.
void EncoderState::vid_create_conversion_texs()
{
    DXGI_FORMAT plane_formats[VID_MAX_PLANES];
    VidPlaneShifts plane_shifts[VID_MAX_PLANES];

    switch (render_video_info->pixel_format)
    {
        case AV_PIX_FMT_NV12:
        {
            vid_conversion_cs = vid_nv12_cs;
            vid_num_planes = 2;

            plane_shifts[0] = VidPlaneShifts { 0, 0 };
            plane_shifts[1] = VidPlaneShifts { 1, 1 };
            plane_formats[0] = DXGI_FORMAT_R8_UINT;
            plane_formats[1] = DXGI_FORMAT_R8G8_UINT;
            break;
        }

        case AV_PIX_FMT_YUV422P:
        {
            vid_conversion_cs = vid_yuv422_cs;
            vid_num_planes = 3;

            plane_shifts[0] = VidPlaneShifts { 0, 0 };
            plane_shifts[1] = VidPlaneShifts { 1, 0 };
            plane_shifts[2] = VidPlaneShifts { 1, 0 };
            plane_formats[0] = DXGI_FORMAT_R8_UINT;
            plane_formats[1] = DXGI_FORMAT_R8_UINT;
            plane_formats[2] = DXGI_FORMAT_R8_UINT;
            break;
        }

        // This must work because the render info is our own thing.
        default: assert(false);
    }

    assert(vid_num_planes <= VID_MAX_PLANES);

    for (s32 i = 0; i < vid_num_planes; i++)
    {
        VidPlaneShifts* shifts = &plane_shifts[i];

        D3D11_TEXTURE2D_DESC tex_desc = {};
        tex_desc.Width = movie_params.video_width >> shifts->x;
        tex_desc.Height = movie_params.video_height >> shifts->y;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = plane_formats[i];
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        tex_desc.CPUAccessFlags = 0;

        vid_plane_heights[i] = tex_desc.Height;

        vid_d3d11_device->CreateTexture2D(&tex_desc, NULL, &vid_converted_texs[i]);
        vid_d3d11_device->CreateUnorderedAccessView(vid_converted_texs[i], NULL, &vid_converted_uavs[i]);

        // Also need to create an equivalent texture on the CPU side that we can copy into and then read from.

        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.BindFlags = 0;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        vid_d3d11_device->CreateTexture2D(&tex_desc, NULL, &vid_converted_dl_texs[i]);
    }
}

// Convert pixel formats and then download textures from graphics memory to system memory.
// At this point these textures are in the correct format ready for encoding.
void EncoderState::vid_convert_to_codec_textures(AVFrame* dest_frame)
{
    vid_d3d11_context->CSSetShader(vid_conversion_cs, NULL, 0);
    vid_d3d11_context->CSSetShaderResources(0, 1, &vid_game_tex_srv);
    vid_d3d11_context->CSSetUnorderedAccessViews(0, vid_num_planes, vid_converted_uavs, NULL);

    vid_d3d11_context->Dispatch(svr_align32(movie_params.video_width, 8) >> 3, svr_align32(movie_params.video_height, 8) >> 3, 1);

    ID3D11ShaderResourceView* null_srvs[] = { NULL };
    ID3D11UnorderedAccessView* null_uavs[] = { NULL };

    vid_d3d11_context->CSSetShaderResources(0, 1, null_srvs);
    vid_d3d11_context->CSSetUnorderedAccessViews(0, 1, null_uavs, NULL);

    for (s32 i = 0; i < vid_num_planes; i++)
    {
        vid_d3d11_context->CopyResource(vid_converted_dl_texs[i], vid_converted_texs[i]);
    }

    D3D11_MAPPED_SUBRESOURCE maps[VID_MAX_PLANES];

    for (s32 i = 0; i < vid_num_planes; i++)
    {
        vid_d3d11_context->Map(vid_converted_dl_texs[i], 0, D3D11_MAP_READ, 0, &maps[i]);
    }

    for (s32 i = 0; i < vid_num_planes; i++)
    {
        D3D11_MAPPED_SUBRESOURCE* map = &maps[i];
        s32 height = vid_plane_heights[i];
        u8* source_ptr = (u8*)map->pData;
        s32 source_line_size = map->RowPitch;
        u8* dest_ptr = dest_frame->data[i];
        s32 dest_line_size = dest_frame->linesize[i];

        for (s32 j = 0; j < height; j++)
        {
            memcpy(dest_ptr, source_ptr, dest_line_size);

            source_ptr += source_line_size;
            dest_ptr += dest_line_size;
        }
    }

    for (s32 i = 0; i < vid_num_planes; i++)
    {
        vid_d3d11_context->Unmap(vid_converted_dl_texs[i], 0);
    }
}