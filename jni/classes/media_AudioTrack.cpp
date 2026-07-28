#include <vector>
#include <SDL2/SDL.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "media_AudioTrack.h"
#ifdef MISTER_NATIVE_VIDEO
#include "mister/mister_native_audio.h"
#endif

static int GetSDLFormat(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return AUDIO_S16;
    case ENCODING_PCM_8BIT: return AUDIO_U8;
    case ENCODING_PCM_FLOAT: return AUDIO_F32SYS;
    default: return AUDIO_U8;
    }
}

static int GetSDLFormatBytes(int audioFormat)
{
    switch (audioFormat) {
    case ENCODING_PCM_16BIT: return 2;
    case ENCODING_PCM_8BIT: return 1;
    case ENCODING_PCM_FLOAT: return 4;
    default: return 1;
    }
}

AudioTrack::AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{    
    SDL_zero(desired);
    desired.freq = sampleRateInHz;
    desired.format = GetSDLFormat(audioFormat);
    desired.channels = (channelConfig == 4) ? 1 : 2;
    desired.samples = bufferSizeInBytes / (desired.channels * GetSDLFormatBytes(audioFormat));
    desired.callback = NULL;
    playing = 0;
    
    needed_bytes = bufferSizeInBytes;
#ifdef MISTER_NATIVE_VIDEO
    nativeTrack = MisterAudio_IsActive() ? MisterAudio_Open(&desired, &obtained) : 0;
    deviceId = nativeTrack ? 0 : SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
#else
    deviceId = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
#endif
    this->mode = mode;
}

static void AudioClass_ctor1(JNIEnv *env, jobject obj, jclass clazz, int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode)
{
    AudioTrack *track = new (obj) AudioTrack(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode);
}

int AudioTrack::getMinBufferSize(JNIEnv *env, jclass clazz, int sampleRateInHz, int channelConfig, int audioFormat)
{
    return 2048 * GetSDLFormatBytes(audioFormat);
}

void AudioTrack::play(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 1;
#ifdef MISTER_NATIVE_VIDEO
    if (track->nativeTrack) MisterAudio_Pause(track->nativeTrack, 0);
    else SDL_PauseAudioDevice(track->deviceId, 0);
#else
    SDL_PauseAudioDevice(track->deviceId, 0);
#endif
}

void AudioTrack::stop(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
    track->playing = 0;
#ifdef MISTER_NATIVE_VIDEO
    if (track->nativeTrack) MisterAudio_Pause(track->nativeTrack, 1);
    else SDL_PauseAudioDevice(track->deviceId, 1);
#else
    SDL_PauseAudioDevice(track->deviceId, 1);
#endif
}

void AudioTrack::release(JNIEnv *env, jobject obj, jclass clazz)
{
    AudioTrack *track = (AudioTrack*)obj;
#ifdef MISTER_NATIVE_VIDEO
    if (track->nativeTrack) {
        // Android's AudioTrack.release() frees the underlying resources and the
        // object must not be used afterwards, so free the slot. The existing SDL
        // mapping only cleared the queue and never closed -- harmless with SDL,
        // where devices are cheap, but MISTER_AUDIO_MAX_TRACKS is 4, so never
        // closing would exhaust the table after the 4th AudioTrack a game
        // creates and silently kill all later audio.
        MisterAudio_Close(track->nativeTrack);
        track->nativeTrack = 0;
    } else {
        SDL_ClearQueuedAudio(track->deviceId);
    }
#else
    SDL_ClearQueuedAudio(track->deviceId);
#endif
}

int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes)
{
    return AudioTrack::write(env, obj, clazz, audioData, offsetInBytes, sizeInBytes, WRITE_BLOCKING);
}


int AudioTrack::write(JNIEnv *env, jobject obj, jclass clazz, jbyteArray audioData, int offsetInBytes, int sizeInBytes, int writeMode)
{
    Class *clz = (Class*)clazz;
    AudioTrack *track = (AudioTrack*)obj;
    ArrayObject *data = (ArrayObject*)audioData;
    uintptr_t where = (uintptr_t)data->elements + offsetInBytes;

    int ret;
#ifdef MISTER_NATIVE_VIDEO
    if (track->nativeTrack)
        ret = MisterAudio_Queue(track->nativeTrack, (void*)where, sizeInBytes);
    else
        ret = SDL_QueueAudio(track->deviceId, (void*)where, sizeInBytes);
#else
    ret = SDL_QueueAudio(track->deviceId, (void*)where, sizeInBytes);
#endif

    if (track->playing == 0)
        AudioTrack::play(env, obj, clazz);

    if (writeMode == WRITE_BLOCKING) {
#ifdef MISTER_NATIVE_VIDEO
        // Waits on STAGING only -- audio already moved into the DDR ring counts
        // as consumed, so the ring stays the latency cushion and the runner
        // paces against the real 48 kHz drain.
        do {
            SDL_Delay(0);
        } while (track->nativeTrack
                    ? MisterAudio_QueuedBytes(track->nativeTrack)
                    : SDL_GetQueuedAudioSize(track->deviceId));
#else
        do {
            SDL_Delay(0);
        } while (SDL_GetQueuedAudioSize(track->deviceId));
#endif
    }

    if (ret == 0)
        return sizeInBytes;
    else
        return 0;
}

const FieldId mediaAudioTrackClassFields[] = {
    {NULL},
};

const ManagedMethod mediaAudioTrackClassMethods[] = {
    REGISTER_INIT_METHOD(AudioTrack, AudioClass_ctor1, "(IIIIII)V"),
    REGISTER_STATIC_METHOD(AudioTrack, getMinBufferSize, "(III)I"),
    REGISTER_NONVIRTUAL(AudioTrack, play   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, stop   , "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, release, "()V"),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BII)I", int, jbyteArray, int, int),
    REGISTER_NONVIRTUAL(AudioTrack, write, "([BIII)I", int, jbyteArray, int, int, int),
    NULL
};

Class AudioTrack::clazz = {
    .classpath = "android/media/AudioTrack",
    .classname = "AudioClass",
    .managed_methods = mediaAudioTrackClassMethods,
    .fields = mediaAudioTrackClassFields,
    .instance_size = sizeof(AudioTrack)
};

static const int registered = ClassRegistry::register_class(AudioTrack::clazz);
