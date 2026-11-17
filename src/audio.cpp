// Fake libaaudio.so implemented on top of SDL3 audio. Supports both the data
// callback model and the blocking AAudioStream_write model (also used by Oboe).

#include "host.h"

#include <SDL3/SDL.h>
#include <android/audio.h>
#include <log.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Builder {
    int32_t sampleRate = 0, channels = 0, direction = AAUDIO_DIRECTION_OUTPUT;
    aaudio_format_t format = AAUDIO_FORMAT_UNSPECIFIED;
    int32_t bufferCapacity = 0, framesPerCallback = 0, perfMode = AAUDIO_PERFORMANCE_MODE_NONE;
    AAudioStream_dataCallback dataCb = nullptr;
    void* dataUser = nullptr;
    AAudioStream_errorCallback errorCb = nullptr;
    void* errorUser = nullptr;
};

struct Stream {
    Builder b;
    SDL_AudioStream* sdl = nullptr;
    std::atomic<aaudio_stream_state_t> state{AAUDIO_STREAM_STATE_OPEN};
    std::atomic<int64_t> framesWritten{0};
    int32_t bufferSize = 0;
    std::vector<uint8_t> scratch;

    int32_t bytesPerSample() const {
        return b.format == AAUDIO_FORMAT_PCM_I16 ? 2 : 4;
    }
    int32_t bytesPerFrame() const { return bytesPerSample() * b.channels; }
};

int32_t burst() { return 256; }

SDL_AudioFormat toSdl(aaudio_format_t f) {
    switch(f) {
    case AAUDIO_FORMAT_PCM_I16: return SDL_AUDIO_S16;
    case AAUDIO_FORMAT_PCM_I32: return SDL_AUDIO_S32;
    default: return SDL_AUDIO_F32;
    }
}

void SDLCALL feed(void* user, SDL_AudioStream* sdl, int additional, int) {
    auto s = (Stream*)user;
    if(!s->b.dataCb || s->state != AAUDIO_STREAM_STATE_STARTED || additional <= 0)
        return;
    int bpf = s->bytesPerFrame();
    int frames = additional / bpf;
    if(frames <= 0)
        return;
    int chunk = s->b.framesPerCallback > 0 ? s->b.framesPerCallback : frames;
    while(frames > 0) {
        int n = chunk;
        s->scratch.resize((size_t)n * bpf);
        auto r = s->b.dataCb((AAudioStream*)s, s->b.dataUser, s->scratch.data(), n);
        SDL_PutAudioStreamData(sdl, s->scratch.data(), n * bpf);
        s->framesWritten += n;
        frames -= n;
        if(r == AAUDIO_CALLBACK_RESULT_STOP) {
            s->state = AAUDIO_STREAM_STATE_STOPPED;
            break;
        }
    }
}

aaudio_result_t openStream(Builder* b, Stream** out) {
    if(b->direction == AAUDIO_DIRECTION_INPUT) {
        Log::warn("AAudio", "Audio input is not supported");
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    if(!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Log::error("AAudio", "SDL audio init failed: %s", SDL_GetError());
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    auto s = new Stream;
    s->b = *b;
    if(s->b.sampleRate <= 0) s->b.sampleRate = 48000;
    if(s->b.channels <= 0) s->b.channels = 2;
    if(s->b.format == AAUDIO_FORMAT_UNSPECIFIED || s->b.format == AAUDIO_FORMAT_PCM_I24_PACKED)
        s->b.format = AAUDIO_FORMAT_PCM_FLOAT;
    if(s->b.bufferCapacity <= 0) s->b.bufferCapacity = burst() * 8;
    s->bufferSize = burst() * 2;
    SDL_AudioSpec spec{toSdl(s->b.format), s->b.channels, s->b.sampleRate};
    s->sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, s->b.dataCb ? feed : nullptr, s);
    if(!s->sdl) {
        Log::error("AAudio", "SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        delete s;
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    *out = s;
    return AAUDIO_OK;
}

aaudio_result_t start(Stream* s) {
    s->state = AAUDIO_STREAM_STATE_STARTED;
    SDL_ResumeAudioStreamDevice(s->sdl);
    return AAUDIO_OK;
}
aaudio_result_t pause(Stream* s) {
    s->state = AAUDIO_STREAM_STATE_PAUSED;
    SDL_PauseAudioStreamDevice(s->sdl);
    return AAUDIO_OK;
}
aaudio_result_t stop(Stream* s) {
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    SDL_PauseAudioStreamDevice(s->sdl);
    return AAUDIO_OK;
}
aaudio_result_t flush(Stream* s) {
    SDL_ClearAudioStream(s->sdl);
    s->state = AAUDIO_STREAM_STATE_FLUSHED;
    return AAUDIO_OK;
}
aaudio_result_t closeStream(Stream* s) {
    if(!s)
        return AAUDIO_ERROR_NULL;
    s->state = AAUDIO_STREAM_STATE_CLOSED;
    if(s->sdl)
        SDL_DestroyAudioStream(s->sdl);
    delete s;
    return AAUDIO_OK;
}

aaudio_result_t write(Stream* s, const void* buffer, int32_t frames, int64_t timeoutNanos) {
    if(s->state == AAUDIO_STREAM_STATE_OPEN)
        start(s);
    int bpf = s->bytesPerFrame();
    // Block while the queue is full (like a real audio device would)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeoutNanos);
    int limit = s->b.bufferCapacity * bpf;
    while(SDL_GetAudioStreamQueued(s->sdl) > limit) {
        if(timeoutNanos <= 0 || std::chrono::steady_clock::now() > deadline)
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SDL_PutAudioStreamData(s->sdl, buffer, frames * bpf);
    s->framesWritten += frames;
    return frames;
}

aaudio_result_t waitForStateChange(Stream* s, aaudio_stream_state_t input, aaudio_stream_state_t* next, int64_t timeout) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout);
    while(s->state == input && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(next)
        *next = s->state;
    return AAUDIO_OK;
}

aaudio_result_t getTimestamp(Stream* s, clockid_t, int64_t* frame, int64_t* time) {
    if(frame) *frame = s->framesWritten;
    if(time) *time = host::uptimeNanos();
    return AAUDIO_OK;
}

}  // namespace

void fakelibs::installAudio(std::unordered_map<std::string, void*>& s) {
#define B(x) ((Builder*)(x))
#define S(x) ((Stream*)(x))
    s["AAudio_createStreamBuilder"] = (void*)+[](void** out) -> aaudio_result_t {
        *out = new Builder;
        return AAUDIO_OK;
    };
    s["AAudioStreamBuilder_delete"] = (void*)+[](void* b) -> aaudio_result_t { delete B(b); return AAUDIO_OK; };
    s["AAudioStreamBuilder_openStream"] = (void*)+[](void* b, void** out) -> aaudio_result_t { return openStream(B(b), (Stream**)out); };
    s["AAudioStreamBuilder_setSampleRate"] = (void*)+[](void* b, int32_t v) { B(b)->sampleRate = v; };
    s["AAudioStreamBuilder_setChannelCount"] = (void*)+[](void* b, int32_t v) { B(b)->channels = v; };
    s["AAudioStreamBuilder_setSamplesPerFrame"] = (void*)+[](void* b, int32_t v) { B(b)->channels = v; };
    s["AAudioStreamBuilder_setFormat"] = (void*)+[](void* b, aaudio_format_t v) { B(b)->format = v; };
    s["AAudioStreamBuilder_setDirection"] = (void*)+[](void* b, int32_t v) { B(b)->direction = v; };
    s["AAudioStreamBuilder_setBufferCapacityInFrames"] = (void*)+[](void* b, int32_t v) { B(b)->bufferCapacity = v; };
    s["AAudioStreamBuilder_setPerformanceMode"] = (void*)+[](void* b, int32_t v) { B(b)->perfMode = v; };
    s["AAudioStreamBuilder_setFramesPerDataCallback"] = (void*)+[](void* b, int32_t v) { B(b)->framesPerCallback = v; };
    s["AAudioStreamBuilder_setDataCallback"] = (void*)+[](void* b, AAudioStream_dataCallback cb, void* u) { B(b)->dataCb = cb; B(b)->dataUser = u; };
    s["AAudioStreamBuilder_setErrorCallback"] = (void*)+[](void* b, AAudioStream_errorCallback cb, void* u) { B(b)->errorCb = cb; B(b)->errorUser = u; };
    for(auto n : {"AAudioStreamBuilder_setDeviceId", "AAudioStreamBuilder_setSharingMode", "AAudioStreamBuilder_setUsage",
                  "AAudioStreamBuilder_setContentType", "AAudioStreamBuilder_setInputPreset", "AAudioStreamBuilder_setSessionId",
                  "AAudioStreamBuilder_setAllowedCapturePolicy", "AAudioStreamBuilder_setPrivacySensitive",
                  "AAudioStreamBuilder_setChannelMask", "AAudioStreamBuilder_setPackageName",
                  "AAudioStreamBuilder_setAttributionTag", "AAudioStreamBuilder_setSpatializationBehavior",
                  "AAudioStreamBuilder_setIsContentSpatialized"})
        s[n] = (void*)+[](void*, intptr_t) {};

    s["AAudioStream_close"] = (void*)+[](void* st) -> aaudio_result_t { return closeStream(S(st)); };
    s["AAudioStream_release"] = (void*)+[](void* st) -> aaudio_result_t { return stop(S(st)); };
    s["AAudioStream_requestStart"] = (void*)+[](void* st) -> aaudio_result_t { return start(S(st)); };
    s["AAudioStream_requestPause"] = (void*)+[](void* st) -> aaudio_result_t { return pause(S(st)); };
    s["AAudioStream_requestStop"] = (void*)+[](void* st) -> aaudio_result_t { return stop(S(st)); };
    s["AAudioStream_requestFlush"] = (void*)+[](void* st) -> aaudio_result_t { return flush(S(st)); };
    s["AAudioStream_getState"] = (void*)+[](void* st) -> aaudio_stream_state_t { return S(st)->state; };
    s["AAudioStream_waitForStateChange"] = (void*)waitForStateChange;
    s["AAudioStream_write"] = (void*)write;
    s["AAudioStream_read"] = (void*)+[](void*, void*, int32_t, int64_t) -> aaudio_result_t { return AAUDIO_ERROR_UNIMPLEMENTED; };
    s["AAudioStream_setBufferSizeInFrames"] = (void*)+[](void* st, int32_t v) -> aaudio_result_t {
        S(st)->bufferSize = v > S(st)->b.bufferCapacity ? S(st)->b.bufferCapacity : v;
        return S(st)->bufferSize;
    };
    s["AAudioStream_getBufferSizeInFrames"] = (void*)+[](void* st) -> int32_t { return S(st)->bufferSize; };
    s["AAudioStream_getBufferCapacityInFrames"] = (void*)+[](void* st) -> int32_t { return S(st)->b.bufferCapacity; };
    s["AAudioStream_getFramesPerBurst"] = (void*)+[](void*) -> int32_t { return burst(); };
    s["AAudioStream_getFramesPerDataCallback"] = (void*)+[](void* st) -> int32_t { return S(st)->b.framesPerCallback; };
    s["AAudioStream_getXRunCount"] = (void*)+[](void*) -> int32_t { return 0; };
    s["AAudioStream_getSampleRate"] = (void*)+[](void* st) -> int32_t { return S(st)->b.sampleRate; };
    s["AAudioStream_getChannelCount"] = (void*)+[](void* st) -> int32_t { return S(st)->b.channels; };
    s["AAudioStream_getSamplesPerFrame"] = (void*)+[](void* st) -> int32_t { return S(st)->b.channels; };
    s["AAudioStream_getFormat"] = (void*)+[](void* st) -> aaudio_format_t { return S(st)->b.format; };
    s["AAudioStream_getDirection"] = (void*)+[](void* st) -> int32_t { return S(st)->b.direction; };
    s["AAudioStream_getPerformanceMode"] = (void*)+[](void* st) -> int32_t { return S(st)->b.perfMode; };
    s["AAudioStream_getSharingMode"] = (void*)+[](void*) -> int32_t { return AAUDIO_SHARING_MODE_SHARED; };
    s["AAudioStream_getDeviceId"] = (void*)+[](void*) -> int32_t { return 1; };
    s["AAudioStream_getSessionId"] = (void*)+[](void*) -> int32_t { return AAUDIO_SESSION_ID_NONE; };
    s["AAudioStream_getUsage"] = (void*)+[](void*) -> int32_t { return AAUDIO_USAGE_GAME; };
    s["AAudioStream_getContentType"] = (void*)+[](void*) -> int32_t { return AAUDIO_CONTENT_TYPE_MUSIC; };
    s["AAudioStream_getFramesWritten"] = (void*)+[](void* st) -> int64_t { return S(st)->framesWritten; };
    s["AAudioStream_getFramesRead"] = (void*)+[](void* st) -> int64_t {
        int bpf = S(st)->bytesPerFrame();
        return S(st)->framesWritten - (bpf ? SDL_GetAudioStreamQueued(S(st)->sdl) / bpf : 0);
    };
    s["AAudioStream_getTimestamp"] = (void*)getTimestamp;
    s["AAudioStream_isMMapUsed"] = (void*)+[](void*) -> bool { return false; };
    s["AAudio_convertResultToText"] = (void*)+[](aaudio_result_t r) -> const char* { return r == AAUDIO_OK ? "AAUDIO_OK" : "AAUDIO_ERROR"; };
    s["AAudio_convertStreamStateToText"] = (void*)+[](aaudio_stream_state_t) -> const char* { return "AAUDIO_STREAM_STATE"; };
#undef B
#undef S
}
