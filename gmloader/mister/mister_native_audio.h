//
//  MiSTer native audio shim -- gmloader
//
//  Replaces the SDL audio *device* for every gmloader PCM producer
//  (android.media.AudioTrack, FMOD_SDL, video_ffmpeg) with a sink that mixes
//  into the DDR3 ring the FPGA drains. SDL is still used for rate/format
//  conversion via SDL_AudioStream -- only the device is replaced.
//
//  Callers see their own requested spec back verbatim; the 48 kHz stereo S16
//  sink format is never negotiated outward.
//
//  Copyright (C) 2026 -- GPL-3.0
//

#ifndef MISTER_NATIVE_AUDIO_H
#define MISTER_NATIVE_AUDIO_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Track handle. 0 == failure, mirroring SDL_AudioDeviceID.
typedef int MisterAudioTrack;

#define MISTER_AUDIO_MAX_TRACKS      4
#define MISTER_AUDIO_STAGING_CAP_MS  500

/// Map the DDR ring. Returns false if /dev/mem is unavailable, in which case
/// IsActive() stays false and callers must fall back to real SDL devices.
bool MisterAudio_Init(void);

/// Close all tracks and unmap. Idempotent.
void MisterAudio_Shutdown(void);

/// True when the ring is mapped and the shim owns the audio path.
bool MisterAudio_IsActive(void);

/// Open a track. If desired->callback is non-NULL the track is PULL mode and
/// the pump invokes that callback; otherwise it is PUSH mode and the caller
/// feeds it with MisterAudio_Queue. `obtained` (if non-NULL) is filled with
/// exactly `desired`, so callers see no format change.
MisterAudioTrack MisterAudio_Open(const SDL_AudioSpec *desired,
                                  SDL_AudioSpec *obtained);
void MisterAudio_Close(MisterAudioTrack t);

/// Push PCM in the track's own format. Returns 0 on success, -1 if refused
/// because staging is already at MISTER_AUDIO_STAGING_CAP_MS.
int MisterAudio_Queue(MisterAudioTrack t, const void *data, uint32_t len);

/// Bytes still staged for this track, in 48 kHz stereo S16 bytes. Reports
/// ONLY staging -- audio already moved into the ring counts as consumed, so
/// the ring keeps acting as the latency cushion.
uint32_t MisterAudio_QueuedBytes(MisterAudioTrack t);

void MisterAudio_Clear(MisterAudioTrack t);
void MisterAudio_Pause(MisterAudioTrack t, int pause_on);

/// Frames refused by the staging cap since Init. Non-zero on device is a bug
/// signal, not normal operation.
uint64_t MisterAudio_DroppedFrames(void);

#ifdef __cplusplus
}
#endif

#endif
