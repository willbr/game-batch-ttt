#ifndef AUDIO_H
#define AUDIO_H

/* Lazy engine init on first audio_play; explicit shutdown on exit.
 * audio_play blocks until the clip finishes so short SFX don't get
 * cut off when the emulator exits right after the call. */

void audio_shutdown(void);
int  audio_play(const char *path);

#endif
