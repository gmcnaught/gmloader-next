//
//  MiSTer native audio shim -- gmloader
//
//  Replaces the SDL audio *device* for every gmloader PCM producer
//  (android.media.AudioTrack, FMOD_SDL, video_ffmpeg) with a sink that mixes
//  into the DDR3 ring the FPGA drains. SDL is still used for rate/format
//  conversion via SDL_AudioStream -- only the device is replaced.
//
//  The sink runs at NA_SAMPLE_RATE, the game's NATIVE rate, so a track opened
//  at that rate converts as an identity copy instead of being resampled to
//  48 kHz on the A9. Producers at other rates (ffmpeg cutscene audio) still
//  convert, which is correct and rare.
//
//  Callers see their own requested spec back verbatim; the sink format is
//  never negotiated outward.
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

/// Bytes still staged for this track, in sink-format stereo S16 bytes. Reports
/// ONLY staging -- audio already moved into the ring counts as consumed, so
/// the ring keeps acting as the latency cushion.
uint32_t MisterAudio_QueuedBytes(MisterAudioTrack t);

void MisterAudio_Clear(MisterAudioTrack t);
void MisterAudio_Pause(MisterAudioTrack t, int pause_on);

/// Bytes of staging a blocking writer must be allowed to keep, mirroring the
/// buffer android.media.AudioTrack was opened with. 0 for a closed handle.
/// AudioTrack.write(WRITE_BLOCKING) blocks until the data is ACCEPTED into
/// that buffer, not until it has played, so this -- not zero -- is what a
/// blocking write waits down to.
uint32_t MisterAudio_StagingHighWater(MisterAudioTrack t);

/// Run one pump pass: top the ring back up to TARGET_FILL by mixing every
/// active, unpaused track. Returns the frames submitted.
///
/// Submits silence only when nothing is playing, or when the ring backlog has
/// fallen below the starvation floor -- the FPGA FIFO holds its last sample
/// when starved, so an empty ring would park the DAC at a DC offset. While a
/// track IS playing and the backlog is healthy, a momentarily dry producer
/// gets a SHORT submit (possibly zero) instead: the ring is the latency
/// cushion, and splicing silence into a live stream spends that cushion on an
/// audible gap. GMLOADER_AUDIO_SILENCE_PAD=1 restores the old always-pad
/// behaviour for A/B measurement on device.
/// The pump thread calls this in a loop; tests call it directly.
size_t MisterAudio_PumpOnce(void);

/// True when the pump thread is running. False when Init() failed, when
/// GMLOADER_AUDIO_PUMP_THREAD=0, or when pthread_create failed. A low core
/// count affects pinning only, not whether the thread runs.
bool MisterAudio_ThreadActive(void);

/// Frames refused by the staging cap since Init. Non-zero on device is a bug
/// signal, not normal operation.
uint64_t MisterAudio_DroppedFrames(void);

/// Frames of silence the pump spliced into the ring while at least one track
/// was open and unpaused, i.e. audible gaps in a live stream. This is the
/// starvation signal the FPGA side cannot see: gm_audio's underflow counter
/// stays at zero when the host pads, because the ring never actually runs dry.
uint64_t MisterAudio_StarvedFrames(void);

#ifdef __cplusplus
}
#endif

#endif
