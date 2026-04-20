/* audio.c -- miniaudio-backed WAV playback for cmd.c's sndrec32 hook. */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"
#include <unistd.h>

static ma_engine g_engine;
static int g_engine_ok = 0;

static int engine_lazy_init(void) {
    if (g_engine_ok) return 1;
    if (ma_engine_init(NULL, &g_engine) != MA_SUCCESS) return 0;
    g_engine_ok = 1;
    return 1;
}

void audio_shutdown(void) {
    if (g_engine_ok) { ma_engine_uninit(&g_engine); g_engine_ok = 0; }
}

int audio_play(const char *path) {
    if (!engine_lazy_init()) return 0;

    ma_sound sound;
    if (ma_sound_init_from_file(&g_engine, path, MA_SOUND_FLAG_DECODE, NULL, NULL, &sound) != MA_SUCCESS)
        return 0;

    ma_sound_start(&sound);
    while (ma_sound_is_playing(&sound)) usleep(5 * 1000);
    ma_sound_uninit(&sound);
    return 1;
}
