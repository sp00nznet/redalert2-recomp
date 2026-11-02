/*
 * WASAPI sound: the game's IDirectSound and IDirectSoundBuffer answered here,
 * mixed in software and played through a WASAPI shared-mode stream on a
 * thread of our own (event-driven). Ported from pcrecomp's win32hle/dsound.c
 * (the same 11 + 21 methods and the same mixer), as native COM objects.
 *
 * Why: DirectSound on Windows 10 and 11 is itself an emulation on WASAPI,
 * and it keeps its own per-program state (the waveOut channel volume --mute
 * set, which Windows kept after exit). Here the mix is ours: --mute is a gain
 * of 0, nothing is left behind, and --record can tap the mix (host.c).
 *
 * A secondary buffer is memory the game Locks and writes, as on Windows; the
 * mixer reads it at the buffer's frequency, volume and pan and moves its play
 * cursor in real time, so a game that streams into a looping buffer by
 * watching GetCurrentPosition (the music, Bink's movies) keeps doing so. The
 * primary buffer is only its format: everything is mixed at the device's
 * rate. One lock covers the buffers' state, between the mixer and the game.
 * Measured with --audio-trace (docs/audio.md): what the game and Bink call.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

/* the GUIDs, here: not every SDK's uuid.lib has the WASAPI ones */
static const GUID k_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_IID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID k_IID_IChannelAudioVolume = { 0x1C158861, 0xB533, 0x4B30, { 0xB1, 0xCF, 0xE8, 0x53, 0xE5, 0x1C, 0x59, 0xB8 } };
static const GUID k_IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID k_IID_IDirectSound = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_IID_IDirectSoundBuffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_SUBTYPE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID k_SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

typedef struct wbuf {
    IDirectSoundBufferVtbl* lpVtbl;
    volatile LONG refs;
    uint8_t* data;                      /* the samples, the game's to Lock */
    LONG* data_refs;                    /* shared with duplicates */
    DWORD bytes, flags;
    int channels, bits, rate, rate0, block;
    int primary, playing, looping;
    double pos;                         /* the play cursor, in frames */
    LONG volume, pan;                   /* hundredths of a dB; -10000..10000 */
    float gl, gr;
    struct wbuf* dead;                  /* the graveyard's next */
} wbuf;

typedef struct {
    IDirectSoundVtbl* lpVtbl;
    volatile LONG refs;
} wdev;

static CRITICAL_SECTION g_mix;
#define MAX_BUFS 256
static wbuf* g_buf[MAX_BUFS];
static wbuf* g_dead;                    /* released while a timer callback ran */
static volatile LONG g_rate;            /* the device's; 0 until it is open */
static volatile float g_gain = 1.0f;
static HANDLE g_ready;

static void gains(wbuf* b) {
    float v = b->volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (b->pan < 0) r = b->pan <= DSBPAN_LEFT ? 0.0f : powf(10.0f, (float)b->pan / 2000.0f);
    if (b->pan > 0) l = b->pan >= DSBPAN_RIGHT ? 0.0f : powf(10.0f, (float)-b->pan / 2000.0f);
    b->gl = v * l, b->gr = v * r;
}

static float sample(const wbuf* b, uint32_t frame, int ch) {
    const uint8_t* p = b->data + (size_t)frame * (uint32_t)b->block + (b->channels > 1 ? (size_t)ch * (uint32_t)(b->bits / 8) : 0);
    return b->bits == 16 ? (float)*(const int16_t*)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

/* ---- the recording tap: a ring of stereo s16, written by the mixer ---- */
#define TAP_FRAMES (1 << 17)            /* 2.7 s at 48000 */
static int16_t g_tap[TAP_FRAMES * 2];
static volatile LONG g_tap_on;
static volatile uint32_t g_tap_wr, g_tap_rd;

void audio_wasapi_tap(int on) { g_tap_rd = g_tap_wr; g_tap_on = on; }

int audio_wasapi_tap_read(int16_t* out, int frames) {
    uint32_t wr = g_tap_wr, rd = g_tap_rd, n;
    if (wr - rd > TAP_FRAMES) rd = wr - TAP_FRAMES;     /* overrun: the oldest are gone */
    n = wr - rd < (uint32_t)frames ? wr - rd : (uint32_t)frames;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (rd + i) % TAP_FRAMES;
        out[2 * i] = g_tap[2 * k], out[2 * i + 1] = g_tap[2 * k + 1];
    }
    g_tap_rd = rd + n;
    return (int)n;
}

int audio_wasapi_rate(void) { return (int)g_rate; }
void audio_wasapi_gain(float gain) { g_gain = gain; }

/* frames of stereo float at `rate` into acc (zeroed first) */
static void mix(float* acc, int frames, int rate) {
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    EnterCriticalSection(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++) {
        wbuf* b = g_buf[i];
        if (!b || !b->playing || b->primary || !b->block || b->bytes < (DWORD)b->block) continue;
        uint32_t nframes = b->bytes / (uint32_t)b->block;
        double step = (double)b->rate / rate;
        for (int f = 0; f < frames && b->playing; f++) {
            uint32_t at = (uint32_t)b->pos, next;
            if (at >= nframes) {
                if (b->looping) { b->pos -= nframes * floor(b->pos / nframes); at = (uint32_t)b->pos % nframes; }
                else { b->playing = 0; b->pos = 0; break; }
            }
            next = at + 1 < nframes ? at + 1 : b->looping ? 0 : at;
            float t = (float)(b->pos - at);
            float l0 = sample(b, at, 0), l1 = sample(b, next, 0);
            float l = l0 + (l1 - l0) * t, r = l;
            if (b->channels > 1) {
                float r0 = sample(b, at, 1), r1 = sample(b, next, 1);
                r = r0 + (r1 - r0) * t;
            }
            acc[2 * f] += l * b->gl;
            acc[2 * f + 1] += r * b->gr;
            b->pos += step;
        }
    }
    LeaveCriticalSection(&g_mix);
    if (g_tap_on) {
        uint32_t wr = g_tap_wr;
        for (int f = 0; f < frames; f++, wr++) {
            uint32_t k = wr % TAP_FRAMES;
            for (int c = 0; c < 2; c++) {
                float v = acc[2 * f + c] * 32767.0f;
                g_tap[2 * k + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
            }
        }
        g_tap_wr = wr;
    }
}

/* ---- the output: WASAPI shared mode, event-driven ---- */
#define MAX_MIX 8192
static float g_acc[2 * MAX_MIX];

/* No device (or it went away): the mix at 48000 Hz into nothing, paced by the
 * clock, for `ms` milliseconds (forever if 0), so the cursors still move. */
static void run_silent(DWORD ms) {
    LARGE_INTEGER f, t0, t;
    long long done = 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(10);
        QueryPerformanceCounter(&t);
        long long due = (t.QuadPart - t0.QuadPart) * g_rate / f.QuadPart;
        while (done < due) {
            int n = (int)(due - done < MAX_MIX ? due - done : MAX_MIX);
            mix(g_acc, n, g_rate);
            done += n;
        }
        if (ms && (t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart >= ms) return;
    }
}

/* One stream on the default device, until it fails (the device unplugged or
 * changed). 0 if it would not open. */
static int run_device(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioClient* ac = NULL;
    IAudioRenderClient* rc = NULL;
    IChannelAudioVolume* cv = NULL;
    WAVEFORMATEX* wf = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT32 size = 0;
    int ok = 0, fl = 0, chans = 0;
    HRESULT hr = CoCreateInstance(&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &k_IID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->lpVtbl->Activate(dev, &k_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ac);
    if (SUCCEEDED(hr)) hr = ac->lpVtbl->GetMixFormat(ac, &wf);
    if (SUCCEEDED(hr)) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)wf;
        int ext = wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
        fl = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (ext && IsEqualGUID(&x->SubFormat, &k_SUBTYPE_FLOAT))) &&
             wf->wBitsPerSample == 32;
        int s16 = (wf->wFormatTag == WAVE_FORMAT_PCM || (ext && IsEqualGUID(&x->SubFormat, &k_SUBTYPE_PCM))) &&
                  wf->wBitsPerSample == 16;
        chans = wf->nChannels;
        if (!(fl || s16) || chans < 1) hr = E_FAIL;
    }
    /* 20 ms: the game streams its music a buffer ahead, and its sound
     * effects start when Play is called, so the latency is this */
    if (SUCCEEDED(hr)) hr = ac->lpVtbl->Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                   200000, 0, wf, NULL);
    if (SUCCEEDED(hr)) hr = ac->lpVtbl->SetEventHandle(ac, ev);
    if (SUCCEEDED(hr)) hr = ac->lpVtbl->GetBufferSize(ac, &size);
    if (SUCCEEDED(hr)) hr = ac->lpVtbl->GetService(ac, &k_IID_IAudioRenderClient, (void**)&rc);
    /* The session's channel volumes back at full: DirectSound's --mute set
     * them through waveOutSetVolume, and Windows keeps them for the program
     * after it exits (the Volume mixer shows only the master, which is the
     * player's and is left alone). */
    if (SUCCEEDED(hr) && SUCCEEDED(ac->lpVtbl->GetService(ac, &k_IID_IChannelAudioVolume, (void**)&cv))) {
        UINT32 n = 0;
        cv->lpVtbl->GetChannelCount(cv, &n);
        for (UINT32 c = 0; c < n; c++) cv->lpVtbl->SetChannelVolume(cv, c, 1.0f, NULL);
    }
    if (SUCCEEDED(hr)) {
        g_rate = (LONG)wf->nSamplesPerSec;
        fprintf(stderr, "[audio] WASAPI shared, %lu Hz, %d channels, %s, buffer %u frames\n", wf->nSamplesPerSec,
                chans, fl ? "float" : "16-bit", size);
        SetEvent(g_ready);
        hr = ac->lpVtbl->Start(ac);
    }
    ok = SUCCEEDED(hr);
    while (SUCCEEDED(hr)) {
        UINT32 pad = 0, n;
        BYTE* out;
        WaitForSingleObject(ev, 200);
        hr = ac->lpVtbl->GetCurrentPadding(ac, &pad);
        if (FAILED(hr)) break;
        n = size - pad;
        if (n > MAX_MIX) n = MAX_MIX;
        if (!n) continue;
        hr = rc->lpVtbl->GetBuffer(rc, n, &out);
        if (FAILED(hr)) break;
        mix(g_acc, (int)n, (int)wf->nSamplesPerSec);
        float g = g_gain;
        for (UINT32 f = 0; f < n; f++)
            for (int c = 0; c < chans; c++) {
                /* left and right to the first two; a mono device gets both */
                float v = chans == 1 ? (g_acc[2 * f] + g_acc[2 * f + 1]) * 0.5f * g : c < 2 ? g_acc[2 * f + c] * g : 0.0f;
                v =v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                if (fl) ((float*)out)[f * chans + c] = v;
                else ((int16_t*)out)[f * chans + c] = (int16_t)(v * 32767.0f);
            }
        hr = rc->lpVtbl->ReleaseBuffer(rc, n, 0);
    }
    if (ok) fprintf(stderr, "[audio] WASAPI stream ended (0x%08lX): reopening\n", hr);
    else fprintf(stderr, "[audio] WASAPI would not open (0x%08lX)\n", hr);
    if (ac && ok) ac->lpVtbl->Stop(ac);
    if (cv) cv->lpVtbl->Release(cv);
    if (rc) rc->lpVtbl->Release(rc);
    if (wf) CoTaskMemFree(wf);
    if (ac) ac->lpVtbl->Release(ac);
    if (dev) dev->lpVtbl->Release(dev);
    if (en) en->lpVtbl->Release(en);
    CloseHandle(ev);
    return ok;
}

static DWORD WINAPI mixer(LPVOID unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        if (run_device()) continue;     /* it ran, then failed: open the (new) default */
        /* none: silent at 48000 (or at the last device's rate) for a while, then try again */
        if (!g_rate) {
            g_rate = 48000;
            fprintf(stderr, "[audio] no output device: silent at 48000 Hz\n");
        }
        SetEvent(g_ready);
        run_silent(5000);
    }
}

int audio_wasapi_start(void) {
    static LONG started;
    if (!InterlockedExchange(&started, 1)) {
        InitializeCriticalSection(&g_mix);
        g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, mixer, NULL, 0, NULL));
    }
    WaitForSingleObject(g_ready, 3000);
    return g_rate != 0;
}

/* ---- IDirectSoundBuffer ---- */
static IDirectSoundBufferVtbl g_buf_vt;

static wbuf* buf_add(wbuf* b) {
    int slot = -1;
    b->lpVtbl = &g_buf_vt, b->refs = 1;
    EnterCriticalSection(&g_mix);
    for (int i = 0; i < MAX_BUFS && slot < 0; i++)
        if (!g_buf[i]) slot = i, g_buf[i] = b;
    LeaveCriticalSection(&g_mix);
    return slot < 0 ? NULL : b;
}

static void buf_free(wbuf* b) {
    if (InterlockedDecrement(b->data_refs) == 0) { free(b->data); free(b->data_refs); }
    free(b);
}

static HRESULT WINAPI b_QueryInterface(IDirectSoundBuffer* self, REFIID iid, LPVOID* out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &k_IID_IUnknown) || IsEqualGUID(iid, &k_IID_IDirectSoundBuffer)) {
        *out = self;
        InterlockedIncrement(&((wbuf*)self)->refs);
        return S_OK;
    }
    *out = NULL;
    fprintf(stderr, "[audio] buffer QueryInterface {%08lX-...}: not here\n", iid->Data1);
    return E_NOINTERFACE;
}
static ULONG WINAPI b_AddRef(IDirectSoundBuffer* self) { return (ULONG)InterlockedIncrement(&((wbuf*)self)->refs); }

/* The host's rule for sound buffers (host.c, timeKillEvent): none is freed
 * while a guest timer callback may be using it. One released then waits in
 * the graveyard for a moment with no callback running. */
static ULONG WINAPI b_Release(IDirectSoundBuffer* self) {
    wbuf* b = (wbuf*)self;
    LONG n = InterlockedDecrement(&b->refs);
    if (n) return (ULONG)n;
    EnterCriticalSection(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++)
        if (g_buf[i] == b) g_buf[i] = NULL;
    b->playing = 0;
    b->dead = g_dead, g_dead = b;
    if (!host_guest_timer_busy()) {
        wbuf* d = g_dead;
        g_dead = NULL;
        while (d) { wbuf* next = d->dead; buf_free(d); d = next; }
    }
    LeaveCriticalSection(&g_mix);
    return 0;
}
static HRESULT WINAPI b_GetCaps(IDirectSoundBuffer* self, LPDSBCAPS c) {
    wbuf* b = (wbuf*)self;
    if (!c || c->dwSize < sizeof *c) return DSERR_INVALIDPARAM;
    c->dwFlags = (b->flags & ~DSBCAPS_LOCHARDWARE) | DSBCAPS_LOCSOFTWARE;
    c->dwBufferBytes = b->bytes, c->dwUnlockTransferRate = 0, c->dwPlayCpuOverhead = 0;
    return DS_OK;
}
static HRESULT WINAPI b_GetCurrentPosition(IDirectSoundBuffer* self, LPDWORD play, LPDWORD write) {
    wbuf* b = (wbuf*)self;
    uint32_t at, lead;
    EnterCriticalSection(&g_mix);
    at = b->block ? (uint32_t)b->pos * (uint32_t)b->block : 0;
    lead = b->block ? (uint32_t)(b->rate / 100) * (uint32_t)b->block : 0;   /* 10 ms ahead, as DirectSound's */
    LeaveCriticalSection(&g_mix);
    if (b->bytes) at %= b->bytes;
    if (play) *play = at;
    if (write) *write = b->bytes ? (at + (b->playing ? lead : 0)) % b->bytes : 0;
    return DS_OK;
}
static HRESULT WINAPI b_GetFormat(IDirectSoundBuffer* self, LPWAVEFORMATEX w, DWORD size, LPDWORD written) {
    wbuf* b = (wbuf*)self;
    if (w && size >= sizeof(PCMWAVEFORMAT)) {
        WAVEFORMATEX f = { WAVE_FORMAT_PCM, (WORD)b->channels, (DWORD)b->rate, (DWORD)(b->rate * b->block),
                           (WORD)b->block, (WORD)b->bits, 0 };
        memcpy(w, &f, size < sizeof f ? size : sizeof f);
    }
    if (written) *written = sizeof(WAVEFORMATEX);
    return DS_OK;
}
static HRESULT WINAPI b_GetVolume(IDirectSoundBuffer* self, LPLONG v) { if (v) *v = ((wbuf*)self)->volume; return DS_OK; }
static HRESULT WINAPI b_GetPan(IDirectSoundBuffer* self, LPLONG v) { if (v) *v = ((wbuf*)self)->pan; return DS_OK; }
static HRESULT WINAPI b_GetFrequency(IDirectSoundBuffer* self, LPDWORD v) { if (v) *v = (DWORD)((wbuf*)self)->rate; return DS_OK; }
static HRESULT WINAPI b_GetStatus(IDirectSoundBuffer* self, LPDWORD s) {
    wbuf* b = (wbuf*)self;
    if (s) *s = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0);
    return DS_OK;
}
static HRESULT WINAPI b_Initialize(IDirectSoundBuffer* self, LPDIRECTSOUND ds, LPCDSBUFFERDESC d) {
    (void)self, (void)ds, (void)d;
    return DSERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI b_Lock(IDirectSoundBuffer* self, DWORD off, DWORD n, LPVOID* p1, LPDWORD n1, LPVOID* p2,
                             LPDWORD n2, DWORD flags) {
    wbuf* b = (wbuf*)self;
    DWORD a;
    if (b->primary) return DSERR_PRIOLEVELNEEDED;
    if (flags & DSBLOCK_ENTIREBUFFER) off = 0, n = b->bytes;
    else if (flags & DSBLOCK_FROMWRITECURSOR) {
        DWORD play, write;
        b_GetCurrentPosition(self, &play, &write);
        off = write;
    }
    if (!p1 || !n1 || off >= b->bytes || n > b->bytes || !n) return DSERR_INVALIDPARAM;
    a = off + n <= b->bytes ? n : b->bytes - off;
    *p1 = b->data + off, *n1 = a;
    if (p2) *p2 = a < n ? b->data : NULL;
    if (n2) *n2 = n - a;
    return DS_OK;
}
static HRESULT WINAPI b_Play(IDirectSoundBuffer* self, DWORD reserved, DWORD prio, DWORD flags) {
    wbuf* b = (wbuf*)self;
    (void)reserved, (void)prio;
    EnterCriticalSection(&g_mix);
    b->playing = 1, b->looping = (flags & DSBPLAY_LOOPING) != 0;
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_SetCurrentPosition(IDirectSoundBuffer* self, DWORD at) {
    wbuf* b = (wbuf*)self;
    if (!b->block || !b->bytes) return DSERR_INVALIDCALL;
    EnterCriticalSection(&g_mix);
    b->pos = (double)((at % b->bytes) / (DWORD)b->block);
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_SetFormat(IDirectSoundBuffer* self, LPCWAVEFORMATEX w) {
    wbuf* b = (wbuf*)self;
    if (!b->primary) return DSERR_INVALIDCALL;
    if (w) b->channels = w->nChannels, b->rate = b->rate0 = (int)w->nSamplesPerSec, b->bits = w->wBitsPerSample,
           b->block = w->nBlockAlign;  /* the primary's: the mix is at the device's rate anyway */
    return DS_OK;
}
static HRESULT WINAPI b_SetVolume(IDirectSoundBuffer* self, LONG v) {
    wbuf* b = (wbuf*)self;
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&g_mix);
    b->volume = v;
    gains(b);
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_SetPan(IDirectSoundBuffer* self, LONG v) {
    wbuf* b = (wbuf*)self;
    if (v < DSBPAN_LEFT || v > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&g_mix);
    b->pan = v;
    gains(b);
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_SetFrequency(IDirectSoundBuffer* self, DWORD hz) {
    wbuf* b = (wbuf*)self;
    if (hz && (hz < DSBFREQUENCY_MIN || hz > 200000)) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&g_mix);
    b->rate = hz ? (int)hz : b->rate0;  /* 0: DSBFREQUENCY_ORIGINAL */
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_Stop(IDirectSoundBuffer* self) {
    EnterCriticalSection(&g_mix);
    ((wbuf*)self)->playing = 0;
    LeaveCriticalSection(&g_mix);
    return DS_OK;
}
static HRESULT WINAPI b_Unlock(IDirectSoundBuffer* self, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) {
    (void)self, (void)p1, (void)n1, (void)p2, (void)n2;   /* the memory is the buffer */
    return DS_OK;
}
static HRESULT WINAPI b_Restore(IDirectSoundBuffer* self) { (void)self; return DS_OK; }

static IDirectSoundBufferVtbl g_buf_vt = {
    b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume, b_GetPan,
    b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
    b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore,
};

/* ---- IDirectSound ---- */
static HRESULT WINAPI d_QueryInterface(IDirectSound* self, REFIID iid, LPVOID* out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &k_IID_IUnknown) || IsEqualGUID(iid, &k_IID_IDirectSound)) {
        *out = self;
        InterlockedIncrement(&((wdev*)self)->refs);
        return S_OK;
    }
    *out = NULL;
    fprintf(stderr, "[audio] device QueryInterface {%08lX-...}: not here\n", iid->Data1);
    return E_NOINTERFACE;
}
static ULONG WINAPI d_AddRef(IDirectSound* self) { return (ULONG)InterlockedIncrement(&((wdev*)self)->refs); }
static ULONG WINAPI d_Release(IDirectSound* self) {
    LONG n = InterlockedDecrement(&((wdev*)self)->refs);
    if (!n) free(self);
    return (ULONG)n;
}
static HRESULT WINAPI d_CreateSoundBuffer(IDirectSound* self, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) {
    wbuf* b;
    const WAVEFORMATEX* w;
    (void)self;
    if (!d || !out || outer) return DSERR_INVALIDPARAM;
    *out = NULL;
    w = d->lpwfxFormat;
    b = (wbuf*)calloc(1, sizeof *b);
    if (!b) return DSERR_OUTOFMEMORY;
    b->flags = d->dwFlags, b->primary = (d->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->channels = 2, b->bits = 16, b->rate = b->rate0 = 22050, b->block = 4;
    if (w) {
        b->channels = w->nChannels, b->rate = b->rate0 = (int)w->nSamplesPerSec;
        b->block = w->nBlockAlign, b->bits = w->wBitsPerSample;
        if (w->wFormatTag != WAVE_FORMAT_PCM || (b->bits != 8 && b->bits != 16) || b->channels < 1 ||
            b->channels > 2 || b->block != b->channels * b->bits / 8) {
            fprintf(stderr, "[audio] buffer format %u, %d bits, %d channels: not PCM this mixer plays\n",
                    w->wFormatTag, b->bits, b->channels);
            free(b);
            return DSERR_BADFORMAT;
        }
    } else if (!b->primary) {
        free(b);
        return DSERR_INVALIDPARAM;
    }
    b->bytes = b->primary ? 4096 * 4 : d->dwBufferBytes;
    if (!b->primary && (b->bytes < DSBSIZE_MIN || b->bytes > DSBSIZE_MAX)) { free(b); return DSERR_INVALIDPARAM; }
    b->data = (uint8_t*)malloc(b->bytes);
    b->data_refs = (LONG*)malloc(sizeof(LONG));
    if (!b->data || !b->data_refs) { free(b->data); free(b->data_refs); free(b); return DSERR_OUTOFMEMORY; }
    memset(b->data, b->bits == 8 ? 128 : 0, b->bytes);
    *b->data_refs = 1;
    gains(b);
    if (!buf_add(b)) { buf_free(b); return DSERR_OUTOFMEMORY; }
    *out = (LPDIRECTSOUNDBUFFER)b;
    return DS_OK;
}
static HRESULT WINAPI d_GetCaps(IDirectSound* self, LPDSCAPS c) {
    (void)self;
    if (!c || c->dwSize < sizeof *c) return DSERR_INVALIDPARAM;
    memset((uint8_t*)c + 4, 0, sizeof *c - 4);
    /* software only, as DirectSound on Windows Vista and later says */
    c->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                 DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT |
                 DSCAPS_SECONDARY16BIT;
    c->dwMinSecondarySampleRate = DSBFREQUENCY_MIN, c->dwMaxSecondarySampleRate = 200000;
    c->dwPrimaryBuffers = 1;
    return DS_OK;
}
static HRESULT WINAPI d_DuplicateSoundBuffer(IDirectSound* self, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) {
    wbuf* s = (wbuf*)src, *b;
    (void)self;
    if (!s || !out || s->primary) return DSERR_INVALIDPARAM;
    if (!(b = (wbuf*)malloc(sizeof *b))) return DSERR_OUTOFMEMORY;
    EnterCriticalSection(&g_mix);
    *b = *s;
    LeaveCriticalSection(&g_mix);
    InterlockedIncrement(b->data_refs);
    b->playing = 0, b->pos = 0, b->dead = NULL;
    if (!buf_add(b)) { buf_free(b); *out = NULL; return DSERR_OUTOFMEMORY; }
    *out = (LPDIRECTSOUNDBUFFER)b;
    return DS_OK;
}
static HRESULT WINAPI d_SetCooperativeLevel(IDirectSound* self, HWND w, DWORD level) { (void)self, (void)w, (void)level; return DS_OK; }
static HRESULT WINAPI d_Compact(IDirectSound* self) { (void)self; return DS_OK; }
static HRESULT WINAPI d_GetSpeakerConfig(IDirectSound* self, LPDWORD c) { (void)self; if (c) *c = DSSPEAKER_STEREO; return DS_OK; }
static HRESULT WINAPI d_SetSpeakerConfig(IDirectSound* self, DWORD c) { (void)self, (void)c; return DS_OK; }
static HRESULT WINAPI d_Initialize(IDirectSound* self, LPCGUID g) { (void)self, (void)g; return DS_OK; }

static IDirectSoundVtbl g_dev_vt = {
    d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps, d_DuplicateSoundBuffer,
    d_SetCooperativeLevel, d_Compact, d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize,
};

HRESULT audio_wasapi_create(LPDIRECTSOUND* out) {
    wdev* d;
    if (!out) return DSERR_INVALIDPARAM;
    audio_wasapi_start();
    if (!(d = (wdev*)calloc(1, sizeof *d))) return DSERR_OUTOFMEMORY;
    d->lpVtbl = &g_dev_vt, d->refs = 1;
    *out = (LPDIRECTSOUND)d;
    return DS_OK;
}
