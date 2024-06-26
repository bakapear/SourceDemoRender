#include "proc_priv.h"

bool ProcState::encoder_init()
{
    bool ret = false;

    // The shared memory handle must be created before the encoder process.
    if (!encoder_create_shared_mem())
    {
        goto rfail;
    }

    // Start the encoder process early.
    // The process will always be ready and when movie starts we will notify it that we will send data to it.
    if (!encoder_start_process())
    {
        goto rfail;
    }

    svr_console_msg_and_log("Started encoder process\n");

    encoder_pending_samples.init(ENCODER_MAX_SAMPLES * 2);

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

void ProcState::encoder_free_static()
{
    if (encoder_proc)
    {
        CloseHandle(encoder_proc);
        encoder_proc = NULL;
    }

    if (encoder_shared_mem_h)
    {
        CloseHandle(encoder_shared_mem_h);
        encoder_shared_mem_h = NULL;
        encoder_audio_buffer = NULL;
    }

    if (encoder_shared_ptr)
    {
        UnmapViewOfFile(encoder_shared_ptr);
        encoder_shared_ptr = NULL;
    }

    if (game_wake_event_h)
    {
        CloseHandle(game_wake_event_h);
        game_wake_event_h = NULL;
    }

    if (encoder_wake_event_h)
    {
        CloseHandle(encoder_wake_event_h);
        encoder_wake_event_h = NULL;
    }

    encoder_pending_samples.free();
}

void ProcState::encoder_free_dynamic()
{
    svr_maybe_release(&encoder_share_tex);
    svr_maybe_release(&encoder_share_tex_srv);
    svr_maybe_release(&encoder_share_tex_uav);
    svr_maybe_release(&encoder_share_tex_rtv);

    if (encoder_share_tex_h)
    {
        CloseHandle(encoder_share_tex_h);
        encoder_share_tex_h = NULL;
    }

    svr_maybe_release(&encoder_d2d1_share_tex);
    svr_maybe_release(&encoder_share_tex_lock);
}

bool ProcState::encoder_create_shared_mem()
{
    bool ret = false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // Allow encoder process to use this handle too.

    s32 mem_size = sizeof(EncoderSharedMem);
    mem_size += sizeof(SvrWaveSample) * ENCODER_MAX_SAMPLES; // Space for audio buffer.

    // Create shared memory handle without a name. The handle will be passed as a parameter to the encoder process
    // and it will open in that way, since we use inherited handles.
    encoder_shared_mem_h = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, mem_size, NULL);

    if (encoder_shared_mem_h == NULL)
    {
        svr_log("ERROR: Could not create encoder shared memory (%lu)\n", GetLastError());
        goto rfail;
    }

    encoder_shared_ptr = (EncoderSharedMem*)MapViewOfFile(encoder_shared_mem_h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);

    // This can't fail in this case, but check anyway I guess.
    if (encoder_shared_ptr == NULL)
    {
        svr_log("ERROR: Could not view encoder shared memory (%lu)\n", GetLastError());
        goto rfail;
    }

    // These must be auto reset events so there are no race conditions!
    game_wake_event_h = CreateEventA(&sa, FALSE, FALSE, NULL);
    encoder_wake_event_h = CreateEventA(&sa, FALSE, FALSE, NULL);

    memset(encoder_shared_ptr, 0, mem_size); // Put to known state.

    // Fill some initial data. The encoder process will need these right away.
    // Also build the messy offsets because we are mixing 32-bit and 64-bit.

    encoder_shared_ptr->game_pid = GetCurrentProcessId();
    encoder_shared_ptr->game_wake_event_h = (u32)game_wake_event_h;
    encoder_shared_ptr->encoder_wake_event_h = (u32)encoder_wake_event_h;

    s32 offset = 0;

    offset += sizeof(EncoderSharedMem);
    encoder_shared_ptr->audio_buffer_offset = offset;

    encoder_audio_buffer = (u8*)encoder_shared_ptr + encoder_shared_ptr->audio_buffer_offset;

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool ProcState::encoder_start_process()
{
    bool ret = false;

    char full_args[1024];
    full_args[0] = 0;

    // Put the handle to the shared memory as a parameter, we can pass the rest in there.
    // All handles are 32-bit, so this is safe for the 64-bit svr_encoder too.
    // The executable path must be quoted!
    SVR_SNPRINTF(full_args, "\"%s\\svr_encoder.exe\" %u", svr_resource_path, (u32)encoder_shared_mem_h);

    STARTUPINFOA start_info = {};
    start_info.cb = sizeof(STARTUPINFOA);

    PROCESS_INFORMATION proc_info;

    DWORD flags = CREATE_SUSPENDED;

    // Allow a window so you can show debug messages easier.
#if 1
    flags |= CREATE_NO_WINDOW;
#endif

    // Working directory for the encoder process should be in the SVR directory.
    // Need to inherit handles so we can pass the shared memory handle as a parameter.
    if (!CreateProcessA(NULL, full_args, NULL, NULL, TRUE, flags, NULL, svr_resource_path, &start_info, &proc_info))
    {
        svr_log("ERROR: Could not create encoder process (%lu)\n", GetLastError());
        goto rfail;
    }

    // Let the process actually start now.
    // You want to place a breakpoint on this line when debugging svr_encoder!
    // When this breakpoint is hit, attach to the svr_encoder process and then continue this process.
    ResumeThread(proc_info.hThread);

    encoder_proc = proc_info.hProcess;
    CloseHandle(proc_info.hThread);

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool ProcState::encoder_start()
{
    bool ret = false;

    if (!encoder_create_share_textures())
    {
        goto rfail;
    }

    if (!encoder_create_d2d1_bitmap())
    {
        goto rfail;
    }

    if (!encoder_set_shared_mem_params())
    {
        goto rfail;
    }

    encoder_share_tex_lock->AcquireSync(ENCODER_GAME_ID, INFINITE); // Set initial owner now.

    encoder_pending_samples.clear();

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool ProcState::encoder_set_shared_mem_params()
{
    bool ret = false;

    // Set movie parameters to svr_encoder.

    EncoderSharedMovieParams* params = &encoder_shared_ptr->movie_params;

    params->video_fps = movie_profile.video_fps;
    params->video_width = movie_width;
    params->video_height = movie_height;
    params->audio_channels = svr_audio_params.audio_channels;
    params->audio_hz = svr_audio_params.audio_hz;
    params->audio_bits = svr_audio_params.audio_bits;
    params->x264_crf = movie_profile.video_x264_crf;
    params->x264_intra = movie_profile.video_x264_intra;
    params->use_audio = movie_profile.audio_enabled;

    SVR_COPY_STRING(movie_path, params->dest_file);
    SVR_COPY_STRING(movie_profile.video_encoder, params->video_encoder);
    SVR_COPY_STRING(movie_profile.video_x264_preset, params->x264_preset);
    SVR_COPY_STRING(movie_profile.video_dnxhr_profile, params->dnxhr_profile);
    SVR_COPY_STRING(movie_profile.audio_encoder, params->audio_encoder);

    // Must duplicate the handle for the encoder to be able to open it.
    // Doesn't matter if you specify to inherit handles when creating the DXGI handle.

    HANDLE new_handle;
    BOOL res = DuplicateHandle(GetCurrentProcess(), encoder_share_tex_h, encoder_proc, &new_handle, 0, TRUE, DUPLICATE_SAME_ACCESS);

    if (res == 0)
    {
        svr_log("ERROR: Could not duplicate share texture handle (%lu)\n", GetLastError());
        goto rfail;
    }

    encoder_shared_ptr->waiting_audio_samples = 0;
    encoder_shared_ptr->game_texture_h = (u32)new_handle; // Transfer to encoder process, so don't close here.

    encoder_shared_ptr->error = 0;
    encoder_shared_ptr->error_message[0] = 0;

    // Now wake svr_encoder up and let it wait for new data.
    if (!encoder_send_event(ENCODER_EVENT_START))
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool ProcState::encoder_create_share_textures()
{
    bool ret = false;
    HRESULT hr;

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = movie_width;
    tex_desc.Height = movie_height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET; // Must have these flags!
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    hr = vid_d3d11_device->CreateTexture2D(&tex_desc, NULL, &encoder_share_tex);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create share texture (%#x)\n", hr);
        goto rfail;
    }

    vid_d3d11_device->CreateShaderResourceView(encoder_share_tex, NULL, &encoder_share_tex_srv);
    vid_d3d11_device->CreateUnorderedAccessView(encoder_share_tex, NULL, &encoder_share_tex_uav);
    vid_d3d11_device->CreateRenderTargetView(encoder_share_tex, NULL, &encoder_share_tex_rtv);

    IDXGIResource1* dxgi_res = NULL;
    hr = encoder_share_tex->QueryInterface(IID_PPV_ARGS(&dxgi_res));

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not query for newer D3D11 resource features (%#x)\n", hr);
        goto rfail;
    }

    hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ, NULL, &encoder_share_tex_h);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create share texture handle (%#x)\n", hr);
        goto rfail;
    }

    encoder_share_tex->QueryInterface(IID_PPV_ARGS(&encoder_share_tex_lock));

    ret = true;
    goto rexit;

rfail:

rexit:
    svr_maybe_release(&dxgi_res);
    return ret;
}

void ProcState::encoder_end()
{
    if (movie_profile.audio_enabled)
    {
        encoder_flush_audio();
    }

    encoder_send_event(ENCODER_EVENT_STOP);
}

// Call this to resume svr_encoder from a known state.
// You want to call this after you have changed something in the shared memory.
// The variable event_type will be read by svr_encoder once it resumes.
//
// Check the enum for what events can fail. If an event can fail you need to handle it properly by
// checking the return value of this function.
bool ProcState::encoder_send_event(EncoderSharedEvent event)
{
    encoder_shared_ptr->event_type = event;

    SetEvent(encoder_wake_event_h); // Let svr_encoder wake up and handle the event.

    // Block the calling thread until the event has been processed by svr_encoder.
    // We need to do this to ensure the audio and video data access doesn't suffer from any race condition.
    // All the event handling is short and fast so this is a very short wait.
    // When this returns, svr_encoder will be paused and in a known state waiting to be woken up again.
    // This call also makes synchronization easier in this process.

    HANDLE handles[] =
    {
        encoder_proc,
        game_wake_event_h,
    };

    DWORD waited = WaitForMultipleObjects(SVR_ARRAY_SIZE(handles), handles, FALSE, INFINITE);
    HANDLE waited_h = handles[waited - WAIT_OBJECT_0];

    // Encoder exited or crashed or something.
    if (waited_h == encoder_proc)
    {
        svr_console_msg_and_log("Encoder exited or crashed\n");
        return false;
    }

    if (waited_h == game_wake_event_h)
    {
        if (encoder_shared_ptr->error)
        {
            // Any error in svr_encoder is written to its log.
            // We also want to log the error in the console and in our log.
            svr_console_msg_and_log(encoder_shared_ptr->error_message);
            svr_console_msg_and_log("See ENCODER_LOG.txt for more information\n");
            return false;
        }
    }

    return true;
}

bool ProcState::encoder_send_shared_tex()
{
    bool ret = false;

    encoder_share_tex_lock->ReleaseSync(ENCODER_PROC_ID); // Allow encoder to read.

    if (!encoder_send_event(ENCODER_EVENT_NEW_VIDEO))
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    encoder_share_tex_lock->AcquireSync(ENCODER_GAME_ID, INFINITE); // Give back to us now.
    return ret;
}

bool ProcState::encoder_send_audio_samples(SvrWaveSample* samples, s32 num_samples)
{
    // During motion blur capture, we will be getting really low number of samples in here (like 12).
    // This is way too little to wake up the encoder for and block the game.
    // Queue up a larger amount and send that instead.
    encoder_pending_samples.push_range(samples, num_samples);

    bool ret = false;

    if (!encoder_submit_pending_samples())
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

// Send any remaining samples just before the rendering stops.
void ProcState::encoder_flush_audio()
{
    while (encoder_pending_samples.size() > 0)
    {
        s32 samples_to_write = svr_min(encoder_pending_samples.size(), ENCODER_MAX_SAMPLES);

        if (!encoder_send_audio_from_pending(samples_to_write))
        {
            break;
        }
    }
}

// Send full batches of audio.
bool ProcState::encoder_submit_pending_samples()
{
    bool ret = false;

    while (encoder_pending_samples.size() >= ENCODER_MAX_SAMPLES)
    {
        s32 samples_to_write = ENCODER_MAX_SAMPLES;

        if (!encoder_send_audio_from_pending(samples_to_write))
        {
            goto rfail;
        }
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

// Shared code between encoder_submit_pending_samples and encoder_flush_audio.
bool ProcState::encoder_send_audio_from_pending(s32 num_samples)
{
    bool ret = false;

    assert(encoder_pending_samples.size() >= num_samples);

    encoder_pending_samples.pull_range((SvrWaveSample*)encoder_audio_buffer, num_samples);
    encoder_shared_ptr->waiting_audio_samples = num_samples;

    if (!encoder_send_event(ENCODER_EVENT_NEW_AUDIO))
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    return ret;
}

bool ProcState::encoder_create_d2d1_bitmap()
{
    bool ret = false;
    HRESULT hr;

    IDXGISurface* dxgi_surface = NULL;
    encoder_share_tex->QueryInterface(IID_PPV_ARGS(&dxgi_surface));

    // Create passthrough reference to the used render target. This is not a real texture.
    hr = vid_d2d1_context->CreateBitmapFromDxgiSurface(dxgi_surface, NULL, &encoder_d2d1_share_tex);

    if (FAILED(hr))
    {
        svr_log("ERROR: Could not create SRV passthrough (%#x)\n", hr);
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:

rexit:
    svr_maybe_release(&dxgi_surface);
    return ret;
}
