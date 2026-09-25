/* [fps-dip] Keep CPU0 for the render (main) thread.
 *
 * MisterAudio pins the main thread to CPU0 and its pump to CPU1
 * (mister_native_audio.cpp). Every thread created before that pin keeps both
 * CPUs, and every thread the main thread creates after it inherits CPU0, so the
 * render thread shares CPU0 with the engine's own helpers as well as with the
 * USB interrupt and whatever else the system runs. The launcher moves the rest
 * of the system to CPU1 (launch.sh cpu_isolate); this moves the engine's own
 * threads. Same split as the Solarus core's cpu_isolate_sweep (SOLARUS_CPUISOLATE).
 *
 * CpuIsolate_Sweep() is cheap enough to call every frame; it only walks
 * /proc/self/task every CPUISO_PERIOD calls (threads appear lazily: FMOD, SDL
 * audio, ffmpeg video). GMLOADER_CPUISOLATE=0 disables it.
 */
#ifndef CPU_ISOLATE_H
#define CPU_ISOLATE_H
#ifdef __cplusplus
extern "C" {
#endif
void CpuIsolate_Sweep(void);
#ifdef __cplusplus
}
#endif
#endif
