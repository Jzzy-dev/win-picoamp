// aacdec.c - AAC through Media Foundation (see aacdec.h)
#include <stdlib.h>
#include <string.h>
#include "aacdec.h"

size_t adts_frame(const uint8_t *p, int *rate, int *channels)
{
    static const int rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;          // sync, layer 0
    int sr = (p[2] >> 2) & 15;
    int ch = ((p[2] & 1) << 2) | (p[3] >> 6);
    size_t len = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    if (!rates[sr] || len < 7) return 0;
    if (rate) *rate = rates[sr];
    if (channels) *channels = ch ? ch : 2;
    return len;
}

#ifndef _WIN32
bool aacdec_available(void) { return false; }
aacdec *aacdec_open(int rate, int channels) { (void)rate; (void)channels; return NULL; }
int aacdec_decode(aacdec *d, const uint8_t *f, size_t n, int16_t *o, int m, int *r, int *c) { (void)d; (void)f; (void)n; (void)o; (void)m; (void)r; (void)c; return -1; }
void aacdec_close(aacdec *d) { (void)d; }
#else

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

// the Microsoft AAC decoder MFT (wmcodecdsp.h)
static const GUID CLSID_AACDec = { 0x32d186a7, 0x218f, 0x4c75, { 0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99 } };

struct aacdec
{
    IMFTransform *t;
    int rate, ch;            // of the PCM out
    DWORD out_size;
    bool provides;
};

static volatile LONG mf_started;
static int available = -1;

// Media Foundation (once for the process) and COM for this thread (the
// decoding thread: multithreaded)
static bool mf_start(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (InterlockedCompareExchange(&mf_started, 1, 0) == 0)
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { mf_started = 2; return false; }
    return mf_started == 1;
}

// (in a thread of its own: the screen's thread keeps its own COM apartment)
static DWORD WINAPI check(LPVOID arg)
{
    (void)arg;
    int ok = 0;
    if (mf_start())
    {
        IMFTransform *t = NULL;
        HRESULT hr = CoCreateInstance(&CLSID_AACDec, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&t);
        if (SUCCEEDED(hr) && t) { IMFTransform_Release(t); ok = 1; }
    }
    CoUninitialize();
    available = ok;
    return 0;
}

bool aacdec_available(void)
{
    if (available < 0)
    {
        HANDLE h = CreateThread(NULL, 0, check, NULL, 0, NULL);
        if (h) { WaitForSingleObject(h, 10000); CloseHandle(h); }
        if (available < 0) available = 0;
    }
    return available == 1;
}

// the PCM type the decoder offers: 16-bit wanted
static bool set_output(aacdec *d)
{
    for (DWORD i = 0;; i++)
    {
        IMFMediaType *mt = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->t, 0, i, &mt))) break;
        GUID sub;
        UINT32 bits = 0, rate = 0, ch = 0;
        IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 16 && SUCCEEDED(IMFTransform_SetOutputType(d->t, 0, mt, 0)))
        {
            d->rate = (int)rate;
            d->ch = (int)ch;
            IMFMediaType_Release(mt);
            MFT_OUTPUT_STREAM_INFO si;
            memset(&si, 0, sizeof si);
            IMFTransform_GetOutputStreamInfo(d->t, 0, &si);
            d->out_size = si.cbSize ? si.cbSize : 32768;
            if (d->out_size < 32768) d->out_size = 32768;
            d->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            return true;
        }
        IMFMediaType_Release(mt);
    }
    return false;
}

aacdec *aacdec_open(int rate, int channels)
{
    if (!mf_start()) return NULL;
    aacdec *d = calloc(1, sizeof *d);
    if (FAILED(CoCreateInstance(&CLSID_AACDec, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&d->t)) || !d->t)
    {
        free(d);
        return NULL;
    }
    IMFMediaType *in = NULL;
    MFCreateMediaType(&in);
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 1);                  // ADTS
    IMFMediaType_SetUINT32(in, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
    // the rest of HEAACWAVEINFO after the WAVEFORMATEX: payload type 1 (ADTS)
    static const UINT8 ud[12] = { 1, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, ud, sizeof ud);
    HRESULT hr = IMFTransform_SetInputType(d->t, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr) || !set_output(d)) { IMFTransform_Release(d->t); free(d); return NULL; }
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
}

int aacdec_decode(aacdec *d, const uint8_t *frame, size_t len, int16_t *out, int max_frames, int *rate, int *ch)
{
    if (!d) return -1;
    IMFMediaBuffer *buf = NULL;
    IMFSample *s = NULL;
    if (FAILED(MFCreateMemoryBuffer((DWORD)len, &buf))) return -1;
    BYTE *p = NULL;
    IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
    memcpy(p, frame, len);
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_SetCurrentLength(buf, (DWORD)len);
    MFCreateSample(&s);
    IMFSample_AddBuffer(s, buf);
    HRESULT hr = IMFTransform_ProcessInput(d->t, 0, s, 0);
    IMFSample_Release(s);
    IMFMediaBuffer_Release(buf);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return -1;
    int got = 0;
    for (;;)
    {
        MFT_OUTPUT_DATA_BUFFER od;
        memset(&od, 0, sizeof od);
        IMFSample *os = NULL;
        IMFMediaBuffer *ob = NULL;
        if (!d->provides)
        {
            MFCreateSample(&os);
            MFCreateMemoryBuffer(d->out_size, &ob);
            IMFSample_AddBuffer(os, ob);
            od.pSample = os;
        }
        DWORD status = 0;
        hr = IMFTransform_ProcessOutput(d->t, 0, 1, &od, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            // (HE-AAC: the rate doubles once the decoder sees the SBR)
            if (os) IMFSample_Release(os);
            if (ob) IMFMediaBuffer_Release(ob);
            if (od.pEvents) IMFCollection_Release(od.pEvents);
            if (!set_output(d)) return -1;
            continue;
        }
        if (FAILED(hr))
        {
            if (os) IMFSample_Release(os);
            if (ob) IMFMediaBuffer_Release(ob);
            if (od.pEvents) IMFCollection_Release(od.pEvents);
            break;                       // MF_E_TRANSFORM_NEED_MORE_INPUT: done
        }
        IMFMediaBuffer *cb = NULL;
        if (od.pSample && SUCCEEDED(IMFSample_ConvertToContiguousBuffer(od.pSample, &cb)))
        {
            BYTE *q = NULL;
            DWORD n = 0;
            IMFMediaBuffer_Lock(cb, &q, NULL, &n);
            int frames = d->ch ? (int)(n / 2 / (DWORD)d->ch) : 0;
            if (frames > max_frames - got) frames = max_frames - got;
            if (frames > 0) memcpy(out + (size_t)got * (size_t)d->ch, q, (size_t)frames * 2 * (size_t)d->ch);
            got += frames;
            IMFMediaBuffer_Unlock(cb);
            IMFMediaBuffer_Release(cb);
        }
        if (od.pSample && od.pSample != os) IMFSample_Release(od.pSample);
        if (os) IMFSample_Release(os);
        if (ob) IMFMediaBuffer_Release(ob);
        if (od.pEvents) IMFCollection_Release(od.pEvents);
        if (got >= max_frames) break;
    }
    *rate = d->rate;
    *ch = d->ch;
    return got;
}

void aacdec_close(aacdec *d)
{
    if (!d) return;
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    IMFTransform_Release(d->t);
    free(d);
}
#endif
