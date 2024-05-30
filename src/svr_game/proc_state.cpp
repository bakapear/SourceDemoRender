#include "proc_priv.h"

#include <string>

int* demo_tick_ptr;

void* game_get_pointer(const char* dll, uintptr_t address)
{
    MODULEINFO info;

    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(dll), &info, sizeof(MODULEINFO)))
    {
        // Module is not loaded. Not an error because we allow fallthrough scanning of multiple patterns.
        return NULL;
    }

    uintptr_t baseAddress = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    uintptr_t targetAddress = baseAddress + address;

    assert(targetAddress >= baseAddress && targetAddress < (baseAddress + info.SizeOfImage));

    return reinterpret_cast<void*>(targetAddress);
}

bool ProcState::init(const char* in_resource_path, ID3D11Device* in_d3d11_device)
{
    bool ret = false;

    if(demo_tick_ptr == NULL) demo_tick_ptr = (int*)game_get_pointer("engine.dll", 0x4621A4);

    SVR_COPY_STRING(in_resource_path, svr_resource_path);

    if (!vid_init(in_d3d11_device))
    {
        goto rfail;
    }

    if (!velo_init())
    {
        goto rfail;
    }

    if (!mosample_init())
    {
        goto rfail;
    }

    if (!encoder_init())
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:
    free_static();

rexit:

    return ret;
}

void ProcState::new_video_frame()
{
    // If we are using mosample, we will have to accumulate enough frames before we can start sending.
    // Mosample will internally send the frames when they are ready.
    if (movie_profile.mosample_enabled)
    {
        mosample_new_video_frame();
    }

    // No mosample, just send the frame over directly.
    else
    {
        vid_d3d11_context->CopyResource(encoder_share_tex, svr_game_texture.tex);
        process_finished_shared_tex();
    }
}

void ProcState::new_audio_samples(SvrWaveSample* samples, s32 num_samples)
{
    encoder_send_audio_samples(samples, num_samples);
}

bool ProcState::is_velo_enabled()
{
    return movie_profile.velo_enabled;
}

bool ProcState::is_audio_enabled()
{
    return movie_profile.audio_enabled;
}

// Call this when you have written everything you need to encoder_share_tex.
void ProcState::process_finished_shared_tex()
{
    // Now is the time to draw the velo if we have it.
    if (movie_profile.velo_enabled)
    {
        if (movie_profile.velo_output == NULL) velo_draw();
        else
        {
            velo_file << svr_va("%i %.2f %.2f %.2f\n", *demo_tick_ptr, velo_vector.x, velo_vector.y, velo_vector.z);
        }
    }

    encoder_send_shared_tex();
}

inline bool ends_with(std::string const& value, std::string const& ending)
{
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

bool ProcState::start(const char* dest_file, const char* profile, ProcGameTexture* game_texture, SvrAudioParams* audio_params)
{
    bool ret = false;

    svr_game_texture = *game_texture;
    svr_audio_params = *audio_params;

    // Build output video path.

    SVR_SNPRINTF(movie_path, "%s\\movies\\", svr_resource_path);
    CreateDirectoryA(movie_path, NULL);
    SVR_SNPRINTF(movie_path, "%s\\movies\\%s", svr_resource_path, dest_file);

    movie_setup_params();

    // Must load the profiles first!
    // The default profile is the base profile, and other profiles can override individual options.

    if (!movie_load_profile("default", true))
    {
        goto rfail;
    }

    if (profile)
    {
        if (profile[0])
        {
            if (!movie_load_profile(profile, false))
            {
                goto rfail;
            }
        }
    }

    // Override movie path if specified in config file.
    if (movie_profile.video_output != NULL) 
    {
        char* output = movie_profile.video_output;

        if (!PathIsRelativeA(output))
        {
            StringCchCopyA(movie_path, MAX_PATH, output);

            if (!ends_with(output, "\\") && !ends_with(output, "/"))
            {
                StringCchCatA(movie_path, MAX_PATH, "\\");
            }

            StringCchCatA(movie_path, MAX_PATH, dest_file);
        }
    }


    if (!vid_start())
    {
        goto rfail;
    }

    if (!mosample_start())
    {
        goto rfail;
    }

    if (!velo_start())
    {
        goto rfail;
    }

    if (!encoder_start())
    {
        goto rfail;
    }

    ret = true;
    goto rexit;

rfail:
    free_dynamic();

rexit:
    return ret;
}

void ProcState::end()
{
    encoder_end();
    mosample_end();
    velo_end();
    vid_end();

    svr_game_texture = {};
}

void ProcState::free_static()
{
    encoder_free_static();
    mosample_free_static();
    velo_free_static();
    vid_free_static();
}

void ProcState::free_dynamic()
{
    encoder_free_dynamic();
    mosample_free_dynamic();
    velo_free_dynamic();
    vid_free_dynamic();
}

s32 ProcState::get_game_rate()
{
    if (movie_profile.mosample_enabled)
    {
        return movie_profile.video_fps * movie_profile.mosample_mult;
    }

    return movie_profile.video_fps;
}
