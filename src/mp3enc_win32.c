// mp3enc_win32.c - PCM to MP3 through Media Foundation's MP3 encoder (see mp3enc.h).
//
// The encoder is found by what it makes (MFTEnumEx: an audio encoder with
// MP3 out), so its class id does not matter. Its input: 16-bit PCM; its output
// type: MP3 at the rate, channels and bytes a second asked for (the encoder
// takes 128, 160, 192 ... kbit/s; when it wants the output type first, that
// order is tried too).
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp3enc.h"

struct mp3enc
{
    IMFTransform *t;
    int rate, ch;
    DWORD out_size;
    bool provides;
    LONGLONG time;           // 100 ns
};

static char err[120];
const char *mp3enc_error(void) { return err; }

static bool mf_start(void)
{
    static volatile LONG started;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (InterlockedCompareExchange(&started, 1, 0) == 0)
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { started = 2; return false; }
    return started == 1;
}

static IMFMediaType *pcm_type(int rate, int ch)
{
    IMFMediaType *t = NULL;
    if (FAILED(MFCreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(2 * ch));
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(2 * ch * rate));
    IMFMediaType_SetUINT32(t, &MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return t;
}

static IMFMediaType *mp3_type(int rate, int ch, int kbps)
{
    IMFMediaType *t = NULL;
    if (FAILED(MFCreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_MP3);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(kbps * 1000 / 8));
    return t;
}

// the output types the encoder offers: the one of this rate and channels nearest kbps
static bool pick_output(mp3enc *e, int kbps)
{
    IMFMediaType *best = NULL;
    int best_d = 1 << 30;
    for (DWORD i = 0;; i++)
    {
        IMFMediaType *mt = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(e->t, 0, i, &mt))) break;
        UINT32 r = 0, c = 0, b = 0;
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS, &c);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &b);
        int d = abs((int)b * 8 / 1000 - kbps);
        if ((int)r == e->rate && (int)c == e->ch && d < best_d)
        {
            if (best) IMFMediaType_Release(best);
            best = mt;
            best_d = d;
        }
        else IMFMediaType_Release(mt);
    }
    if (!best) return false;
    HRESULT hr = IMFTransform_SetOutputType(e->t, 0, best, 0);
    IMFMediaType_Release(best);
    return SUCCEEDED(hr);
}

mp3enc *mp3enc_open(int rate, int channels, int kbps)
{
    err[0] = 0;
    if (!mf_start()) { snprintf(err, sizeof err, "Media Foundation does not start"); return NULL; }
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Audio, MFAudioFormat_MP3 };
    IMFActivate **acts = NULL;
    UINT32 n = 0;
    HRESULT he = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           NULL, &out, &acts, &n);
    if (FAILED(he) || !n)
    {
        snprintf(err, sizeof err, "This PC has no MP3 encoder (%08lX, %u)", (unsigned long)he, (unsigned)n);
        if (acts) CoTaskMemFree(acts);
        return NULL;
    }
    mp3enc *e = calloc(1, sizeof *e);
    e->rate = rate;
    e->ch = channels;
    for (UINT32 i = 0; i < n; i++)
    {
        if (!e->t && FAILED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform, (void **)&e->t))) e->t = NULL;
        IMFActivate_Release(acts[i]);
    }
    CoTaskMemFree(acts);
    if (!e->t) { snprintf(err, sizeof err, "The MP3 encoder does not start"); free(e); return NULL; }

    // the input first, then the output it offers; else the output first
    IMFMediaType *in = pcm_type(rate, channels), *o = NULL;
    HRESULT h1 = IMFTransform_SetInputType(e->t, 0, in, 0), h2 = S_OK, h3 = S_OK;
    bool ok = SUCCEEDED(h1) && pick_output(e, kbps);
    if (!ok)
    {
        o = mp3_type(rate, channels, kbps);
        ok = o && SUCCEEDED(h2 = IMFTransform_SetOutputType(e->t, 0, o, 0)) && SUCCEEDED(h3 = IMFTransform_SetInputType(e->t, 0, in, 0));
    }
    if (in) IMFMediaType_Release(in);
    if (o) IMFMediaType_Release(o);
    if (!ok)
    {
        snprintf(err, sizeof err, "The MP3 encoder does not take %d Hz (%08lX %08lX %08lX)", rate, (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
        IMFTransform_Release(e->t);
        free(e);
        return NULL;
    }
    MFT_OUTPUT_STREAM_INFO si;
    memset(&si, 0, sizeof si);
    IMFTransform_GetOutputStreamInfo(e->t, 0, &si);
    e->out_size = si.cbSize > 16384 ? si.cbSize : 16384;
    e->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return e;
}

// what the encoder has made: into out
static int drain(mp3enc *e, uint8_t *out, int cap)
{
    int got = 0;
    for (;;)
    {
        MFT_OUTPUT_DATA_BUFFER od;
        memset(&od, 0, sizeof od);
        IMFSample *os = NULL;
        IMFMediaBuffer *ob = NULL;
        if (!e->provides)
        {
            MFCreateSample(&os);
            MFCreateMemoryBuffer(e->out_size, &ob);
            IMFSample_AddBuffer(os, ob);
            od.pSample = os;
        }
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(e->t, 0, 1, &od, &status);
        if (SUCCEEDED(hr) && od.pSample)
        {
            IMFMediaBuffer *cb = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(od.pSample, &cb)))
            {
                BYTE *q = NULL;
                DWORD len = 0;
                IMFMediaBuffer_Lock(cb, &q, NULL, &len);
                int k = (int)len < cap - got ? (int)len : cap - got;
                if (k > 0) memcpy(out + got, q, (size_t)k);
                got += k;
                IMFMediaBuffer_Unlock(cb);
                IMFMediaBuffer_Release(cb);
            }
        }
        if (od.pSample && od.pSample != os) IMFSample_Release(od.pSample);
        if (os) IMFSample_Release(os);
        if (ob) IMFMediaBuffer_Release(ob);
        if (od.pEvents) IMFCollection_Release(od.pEvents);
        if (FAILED(hr) || got >= cap) break;          // MF_E_TRANSFORM_NEED_MORE_INPUT: all of it
    }
    return got;
}

int mp3enc_encode(mp3enc *e, const int16_t *pcm, int frames, uint8_t *out, int cap)
{
    if (!e) return -1;
    int got = 0;
    if (frames > 0)
    {
        DWORD len = (DWORD)frames * 2u * (DWORD)e->ch;
        IMFMediaBuffer *buf = NULL;
        IMFSample *s = NULL;
        if (FAILED(MFCreateMemoryBuffer(len, &buf))) return -1;
        BYTE *p = NULL;
        IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
        memcpy(p, pcm, len);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, len);
        MFCreateSample(&s);
        IMFSample_AddBuffer(s, buf);
        LONGLONG dur = (LONGLONG)frames * 10000000 / e->rate;
        IMFSample_SetSampleTime(s, e->time);
        IMFSample_SetSampleDuration(s, dur);
        e->time += dur;
        HRESULT hr = IMFTransform_ProcessInput(e->t, 0, s, 0);
        if (hr == MF_E_NOTACCEPTING)
        {
            got += drain(e, out, cap);
            hr = IMFTransform_ProcessInput(e->t, 0, s, 0);
        }
        IMFSample_Release(s);
        IMFMediaBuffer_Release(buf);
        if (FAILED(hr)) return got ? got : -1;
    }
    return got + drain(e, out + got, cap - got);
}

void mp3enc_close(mp3enc *e)
{
    if (!e) return;
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    IMFTransform_Release(e->t);
    free(e);
}
