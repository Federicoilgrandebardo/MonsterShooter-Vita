/**
 * @file  audio.h
 * @brief Uscita audio su Vita per il mixer del Claw Engine.
 */

#ifndef SOLOADER_AUDIO_H
#define SOLOADER_AUDIO_H

/// Registra il formato scelto dal motore (Claw::AudioFormat: canali, frequenza).
void audio_set_format(const void *claw_audio_format);

void audio_start(void);
void audio_stop(void);

#endif // SOLOADER_AUDIO_H
