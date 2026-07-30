//
//  Native Audio DDR3 Writer -- gmloader MiSTer
//
//  Pushes NA_SAMPLE_RATE stereo S16 PCM into a DDR3 ring buffer drained by the
//  FPGA (gm_audio, on its own ddr_svc channel). No ALSA, no Linux sound kernel.
//
//  Memory map (must match openbor_video_reader.sv):
//    0x3A000030  audio_wr_ptr  (32-bit byte offset into ring; ARM writes)
//    0x3A000038  audio_rd_ptr  (32-bit byte offset into ring; FPGA writes)
//    0x3A0D0000  audio ring    (65,536 bytes = 16,384 stereo frames)
//
//  Copyright (C) 2026 -- GPL-3.0
//

#ifndef NATIVE_AUDIO_WRITER_H
#define NATIVE_AUDIO_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ring carries the game's NATIVE rate. The FPGA (fpga/rtl/gm_audio.sv in
// maldita.castilla-mister) resamples 22.05k -> 48k with a fractional phase
// accumulator, so nothing upsamples on the A9 any more.
//
// LOCKSTEP: gm_audio's SRC_RATE parameter must equal this. They are two copies
// of one constant in two repos; changing one alone detunes the resampler.
#define NA_SAMPLE_RATE      22050
#define NA_CHANNELS         2
#define NA_BYTES_PER_FRAME  4   /* 2 ch * int16 */

/// Map the DDR3 region and zero the ring + both pointers.
/// GMLOADER_AUDIO_DDR overrides /dev/mem with a regular file (host tests).
/// Returns true on success. Safe to call repeatedly.
bool NativeAudioWriter_Init(void);

/// Zero the write pointer, unmap, close the fd.
void NativeAudioWriter_Shutdown(void);

/// True once Init() has succeeded.
bool NativeAudioWriter_IsActive(void);

/// Submit stereo S16 frames. Returns frames actually written; the tail of an
/// oversized batch is dropped rather than overwriting unread samples.
/// Never blocks, never sleeps. Single-producer only.
size_t NativeAudioWriter_Submit(const int16_t *frames, size_t frame_count);

/// Free space in the ring, in stereo frames.
size_t NativeAudioWriter_FreeFrames(void);

/// Usable ring capacity in stereo frames (the maximum FreeFrames can return).
/// used = CapacityFrames() - FreeFrames().
size_t NativeAudioWriter_CapacityFrames(void);

#ifdef __cplusplus
}
#endif

#endif
