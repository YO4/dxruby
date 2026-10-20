#define WINVER 0x0600
#define DIRECTSOUND_VERSION 0x0900
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "wav.h"

extern HANDLE g_hWnd;

/* WAVEのSubFormat GUID (KSDATAFORMAT_SUBTYPE_PCM / IEEE_FLOAT) */
static const GUID DXRUBY_SUBTYPE_PCM   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID DXRUBY_SUBTYPE_FLOAT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

typedef struct WavPcm {
    BYTE *data;                 /* 保持する全PCMデータ(自前確保、CreateDevice後に解放) */
    DWORD size;                 /* データバイト数 */
    WAVEFORMATEX wfx;
} WavPcm;

typedef struct DXRubyWav DXRubyWav;

struct DXRubyWav {
    const struct DXRubySoundDevVtbl *lpVtbl;
    LPDIRECTSOUNDBUFFER buffer;
    LPDIRECTSOUNDNOTIFY notify;
    WAVEFORMATEX wfx;
    DWORD dataBytes;
    DWORD blockAlign;

    int start;                  /* Sound#start相当(ms) */
    int loopCount;              /* SetRepeats相当(-1=無限) */

    /* 演奏ワーカー */
    HANDLE stopEvent;           /* DSBPN_OFFSETSTOP通知(自動リセット) */
    HANDLE quitEvent;
    HANDLE worker;
    volatile LONG terminate;
    volatile LONG looping;      /* DSBPLAY_LOOPING中 */
    DWORD playsLeft;            /* 残り再生回数(worker内でのみ更新) */

    /* 音量フェード(midi.cと同一機構。volumeは0..255のマスター音量) */
    CRITICAL_SECTION sendLock;
    volatile LONG volume;
    int volumeTarget;
    int volumeStart;
    int fadeTime;
    int fading;
    int lastSentVolume;
    unsigned long long fadeStart;
};

static IDirectSound8 *g_wavDSound;
static LONG g_wavDSoundRefs;

static HRESULT WavAcquireSound(void)
{
    HRESULT hr;
    if (g_wavDSoundRefs == 0 && !g_wavDSound) {
        hr = DirectSoundCreate8(&DSDEVID_DefaultPlayback, &g_wavDSound, NULL);
        if (FAILED(hr)) return hr;
        hr = g_wavDSound->lpVtbl->SetCooperativeLevel(g_wavDSound, g_hWnd, DSSCL_PRIORITY);
        if (FAILED(hr)) {
            g_wavDSound->lpVtbl->Release(g_wavDSound);
            g_wavDSound = NULL;
            return hr;
        }
    }
    InterlockedIncrement(&g_wavDSoundRefs);
    return S_OK;
}

static void WavReleaseSound(void)
{
    if (InterlockedDecrement(&g_wavDSoundRefs) == 0 && g_wavDSound) {
        g_wavDSound->lpVtbl->Release(g_wavDSound);
        g_wavDSound = NULL;
    }
}

static DWORD ReadLE32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static WORD ReadLE16(const BYTE *p)
{
    return (WORD)((WORD)p[0] | ((WORD)p[1] << 8));
}

/* 任意のPCM/floatデータを16bit符号付きPCMに変換する(戻り値は確保したバッファ) */
static BYTE *ConvertTo16bit(const BYTE *data, DWORD size, WAVEFORMATEX *wfx)
{
    BYTE *out;
    int bytes = wfx->wBitsPerSample / 8;
    int samples;
    int i;
    if (bytes < 1 || size % (DWORD)bytes) return NULL;
    samples = (int)(size / (DWORD)bytes);
    out = (BYTE *)malloc((size_t)samples * 2);
    if (!out) return NULL;
    for (i = 0; i < samples; ++i) {
        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            float f = 0.0f;
            memcpy(&f, data + (size_t)i * bytes, sizeof(float));
            if (f >= 0.0f) ((short *)out)[i] = f >= 1.0f ? 32767 : (short)(f * 32767.0f);
            else           ((short *)out)[i] = f <= -1.0f ? -32768 : (short)(f * 32768.0f);
        } else {
            int v = 0;
            switch (bytes) {
            case 1: /* 8bit unsigned */
                v = ((int)data[i * bytes] - 128) << 8;
                break;
            case 2: /* 16bit */
                memcpy(&v, data + (size_t)i * bytes, 2);
                v = (short)v;
                break;
            case 3: /* 24bit */
                /* 範囲外アクセスを避けるため3バイトずつ組み立てる */
                v = (int)data[(size_t)i * bytes] | ((int)data[(size_t)i * bytes + 1] << 8) |
                    ((int)data[(size_t)i * bytes + 2] << 16);
                if (v & 0x00800000) v -= 0x01000000;
                v >>= 8;
                break;
            default: /* 32bit */
                memcpy(&v, data + (size_t)i * bytes, 4);
                v >>= 16;
                break;
            }
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            ((short *)out)[i] = (short)v;
        }
    }
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->wBitsPerSample = 16;
    wfx->nBlockAlign = (WORD)(wfx->nChannels * 2);
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = 0;
    return out;
}

/* RIFF/WAVEをパースしてPCMとフォーマットを取り出す */
static int ParseWav(const BYTE *data, size_t length, WavPcm *pcm, const char **error)
{
    size_t pos = 12, fmtPos = 0, fmtSize = 0, dataPos = 0, dataSize = 0;
    int isFloat = 0;
    ZeroMemory(pcm, sizeof(*pcm));
    if (length < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        if (error) *error = "Not a WAV file";
        return 0;
    }
    while (pos + 8 <= length) {
        DWORD idSize = ReadLE32(data + pos + 4);
        if (idSize > length - pos - 8) idSize = (DWORD)(length - pos - 8);
        if (!memcmp(data + pos, "fmt ", 4) && !fmtPos) {
            if (idSize < 16) { if (error) *error = "Invalid WAV format chunk"; return 0; }
            fmtPos = pos + 8;
            fmtSize = idSize;
        } else if (!memcmp(data + pos, "data", 4) && !dataPos) {
            dataPos = pos + 8;
            dataSize = idSize;
        }
        pos += 8 + (size_t)idSize + (idSize & 1);
    }
    if (!fmtPos || !dataPos) { if (error) *error = "Invalid WAV file"; return 0; }
    ZeroMemory(&pcm->wfx, sizeof(pcm->wfx));
    memcpy(&pcm->wfx, data + fmtPos, fmtSize < sizeof(WAVEFORMATEX) ? fmtSize : sizeof(WAVEFORMATEX));
    if (fmtSize >= sizeof(WAVEFORMATEXTENSIBLE) && pcm->wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const BYTE *sub = data + fmtPos + 22;  /* WAVEFORMATEXTENSIBLE.SubFormatの位置 */
        if (memcmp(sub, &DXRUBY_SUBTYPE_FLOAT, sizeof(GUID)) == 0) isFloat = 1;
        else if (memcmp(sub, &DXRUBY_SUBTYPE_PCM, sizeof(GUID)) == 0) isFloat = 0;
        else { if (error) *error = "Unsupported WAV format"; return 0; }
    } else if (pcm->wfx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = 1;
    }
    if (pcm->wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        /* 上でSubFormatを確認済み。PCMとして扱う */
        pcm->wfx.wFormatTag = WAVE_FORMAT_PCM;
    }
    if (!isFloat && pcm->wfx.wFormatTag != WAVE_FORMAT_PCM) {
        if (error) *error = "Unsupported WAV format";
        return 0;
    }
    if (isFloat && pcm->wfx.wBitsPerSample != 32) {
        if (error) *error = "Unsupported WAV float format";
        return 0;
    }
    if (!pcm->wfx.nChannels || pcm->wfx.nChannels > 8) { if (error) *error = "Invalid WAV format"; return 0; }
    if (!pcm->wfx.nSamplesPerSec || !pcm->wfx.wBitsPerSample) { if (error) *error = "Invalid WAV format"; return 0; }
    pcm->wfx.nBlockAlign = (WORD)(pcm->wfx.nChannels * pcm->wfx.wBitsPerSample / 8);
    if (!pcm->wfx.nBlockAlign) { if (error) *error = "Invalid WAV format"; return 0; }
    pcm->wfx.nAvgBytesPerSec = pcm->wfx.nSamplesPerSec * pcm->wfx.nBlockAlign;
    if (isFloat) pcm->wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    dataSize -= dataSize % pcm->wfx.nBlockAlign;
    if (!dataSize) { if (error) *error = "No PCM data"; return 0; }
    pcm->size = (DWORD)dataSize;
    pcm->data = (BYTE *)malloc(dataSize);
    if (!pcm->data) { if (error) *error = "Out of memory"; return 0; }
    memcpy(pcm->data, data + dataPos, dataSize);
    return 1;
}

/* 音量をDirectSoundの減衰値から0..255マスター音量へ変換(midi.cのSetVolumeと同一の写像) */
static int DbToMasterVolume(int volume)
{
    volume = (volume + 9600) * 255 / 9600;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    return volume;
}

static void UpdateFade(DXRubyWav *w)
{
    unsigned long long now = GetTickCount64();
    int volume, sendNow;
    EnterCriticalSection(&w->sendLock);
    if (w->fading) {
        int elapsed = (int)(now - w->fadeStart);
        if (elapsed >= w->fadeTime) {
            w->volume = w->volumeTarget;
            w->fading = 0;
            w->fadeTime = 0;
        } else {
            w->volume = w->volumeStart + (w->volumeTarget - w->volumeStart) * elapsed / w->fadeTime;
        }
    } else {
        w->volume = w->volumeTarget;
    }
    volume = (int)w->volume;
    sendNow = volume != w->lastSentVolume;
    if (sendNow) w->lastSentVolume = volume;
    LeaveCriticalSection(&w->sendLock);
    if (sendNow && w->buffer) {
        w->buffer->lpVtbl->SetVolume(w->buffer, volume * 9600 / 255 - 9600);
    }
}

static DWORD StartPosition(const DXRubyWav *w)
{
    unsigned long long pos;
    if (w->start <= 0) return 0;
    pos = (unsigned long long)(unsigned int)w->start * w->wfx.nAvgBytesPerSec / 1000;
    pos /= w->blockAlign;
    pos *= w->blockAlign;
    if (w->dataBytes > w->blockAlign) {
        DWORD max = w->dataBytes - w->blockAlign;
        if (pos > max) pos = max;
    } else {
        pos = 0;
    }
    return (DWORD)pos;
}

static unsigned __stdcall WavWorker(void *arg)
{
    DXRubyWav *w = (DXRubyWav *)arg;
    HANDLE waitHandles[2];
    waitHandles[0] = w->stopEvent;
    waitHandles[1] = w->quitEvent;
    while (!InterlockedCompareExchange(&w->terminate, 0, 0)) {
        DWORD waited = WaitForMultipleObjects(2, waitHandles, FALSE, 10);
        if (InterlockedCompareExchange(&w->terminate, 0, 0)) break;
        UpdateFade(w);
        /* 終端まで到達して自然停止した場合、全ループ再生ならループ開始位置へ戻って再開する */
        if (waited == WAIT_OBJECT_0 && !InterlockedCompareExchange(&w->looping, 0, 0)) {
            if (w->playsLeft > 1) {
                w->playsLeft--;
                ResetEvent(w->stopEvent);
                w->buffer->lpVtbl->SetCurrentPosition(w->buffer, StartPosition(w));
                w->buffer->lpVtbl->Play(w->buffer, 0, 0, 0);
            }
        }
    }
    return 0;
}

static void StopWorker(DXRubyWav *w)
{
    if (w->worker) {
        InterlockedExchange(&w->terminate, 1);
        SetEvent(w->quitEvent);
        WaitForSingleObject(w->worker, INFINITE);
        CloseHandle(w->worker);
        w->worker = NULL;
    }
}

static void StopPlayback(DXRubyWav *w)
{
    StopWorker(w);
    if (w->buffer) w->buffer->lpVtbl->Stop(w->buffer);
    InterlockedExchange(&w->looping, 0);
}

//////////////////////////////////////////////////////////////////////////////
// 外部インターフェース(DirectMusicPerformance/DirectSoundメソッド互換)
//////////////////////////////////////////////////////////////////////////////

static HRESULT WavDev_PlaySegmentEx(DXRubySoundDev *dev)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    int master;
    HRESULT hr;
    StopPlayback(w);
    ResetEvent(w->stopEvent);
    /* 前回再生途中のフェード状態を破棄し、目標音量で再生を開始する */
    EnterCriticalSection(&w->sendLock);
    master = w->volumeTarget;
    w->volume = master;
    w->fading = 0;
    w->fadeTime = 0;
    w->lastSentVolume = master;
    LeaveCriticalSection(&w->sendLock);
    w->buffer->lpVtbl->SetVolume(w->buffer, master * 9600 / 255 - 9600);
    w->playsLeft = (w->loopCount < 0) ? 1 : ((w->loopCount == 0) ? 1 : (DWORD)w->loopCount);
    hr = w->buffer->lpVtbl->SetCurrentPosition(w->buffer, StartPosition(w));
    if (FAILED(hr)) return hr;
    InterlockedExchange(&w->looping, w->loopCount < 0 ? 1 : 0);
    hr = w->buffer->lpVtbl->Play(w->buffer, 0, 0, w->loopCount < 0 ? DSBPLAY_LOOPING : 0);
    if (FAILED(hr)) { InterlockedExchange(&w->looping, 0); return hr; }
    InterlockedExchange(&w->terminate, 0);
    w->worker = (HANDLE)_beginthreadex(NULL, 0, WavWorker, w, 0, NULL);
    if (!w->worker) {
        /* ワーカが作れない場合は演出なしで即停止してエラーを返す */
        w->buffer->lpVtbl->Stop(w->buffer);
        InterlockedExchange(&w->looping, 0);
        return E_FAIL;
    }
    return S_OK;
}

static HRESULT WavDev_StopEx(DXRubySoundDev *dev)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    StopPlayback(w);
    return S_OK;
}

static void WavDev_Destroy(DXRubySoundDev *dev)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    StopPlayback(w);
    if (w->notify) w->notify->lpVtbl->Release(w->notify);
    if (w->buffer) w->buffer->lpVtbl->Release(w->buffer);
    if (w->stopEvent) CloseHandle(w->stopEvent);
    if (w->quitEvent) CloseHandle(w->quitEvent);
    DeleteCriticalSection(&w->sendLock);
    WavReleaseSound();
    free(w);
}

static void WavDev_SetStartPoint(DXRubySoundDev *dev, int tick)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    w->start = tick;
}

static void WavDev_SetLoopPoints(DXRubySoundDev *dev, int start, int end)
{
    /* WAVではループ範囲指定は不可(sound.cで例外になった後)。予約として保持のみ */
    (void)dev; (void)start; (void)end;
}

static void WavDev_SetRepeats(DXRubySoundDev *dev, int count)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    w->loopCount = count;
}

static HRESULT WavDev_SetVolume(DXRubySoundDev *dev, int volume, int time)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    unsigned long long now = GetTickCount64();
    int master;
    int fade;
    if (volume < -10000) volume = -10000;
    if (volume > 0) volume = 0;
    if (time < 0) time = 0;
    master = DbToMasterVolume(volume);
    fade = (time > 0 && w->worker != NULL);
    EnterCriticalSection(&w->sendLock);
    if (fade) {
        /* 演奏中は現在値から目標値へtimeミリ秒かけてなめらかに変化させる */
        w->volumeStart = (int)w->volume;
        w->volumeTarget = master;
        w->fadeTime = time;
        w->fadeStart = now;
        w->fading = 1;
    } else {
        w->volume = master;
        w->volumeTarget = master;
        w->fading = 0;
        w->fadeTime = 0;
        w->lastSentVolume = master;
    }
    LeaveCriticalSection(&w->sendLock);
    if (!fade && w->buffer) {
        w->buffer->lpVtbl->SetVolume(w->buffer, master * 9600 / 255 - 9600);
    }
    return S_OK;
}

static HRESULT WavDev_GetPan(DXRubySoundDev *dev, int *pan)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    long value;
    HRESULT hr;
    if (!pan || !w->buffer) return E_POINTER;
    hr = w->buffer->lpVtbl->GetPan(w->buffer, &value);
    if (SUCCEEDED(hr)) *pan = (int)value;
    return hr;
}

static HRESULT WavDev_SetPan(DXRubySoundDev *dev, int pan)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    if (pan < -10000) pan = -10000;
    if (pan > 10000) pan = 10000;
    return w->buffer->lpVtbl->SetPan(w->buffer, pan);
}

static HRESULT WavDev_GetFrequency(DXRubySoundDev *dev, int *frequency)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    DWORD value;
    HRESULT hr;
    if (!frequency || !w->buffer) return E_POINTER;
    hr = w->buffer->lpVtbl->GetFrequency(w->buffer, &value);
    if (SUCCEEDED(hr)) *frequency = (int)value;
    return hr;
}

static HRESULT WavDev_SetFrequency(DXRubySoundDev *dev, int frequency)
{
    DXRubyWav *w = (DXRubyWav *)dev;
    if (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX) return E_INVALIDARG;
    return w->buffer->lpVtbl->SetFrequency(w->buffer, (DWORD)frequency);
}

static const struct DXRubySoundDevVtbl WavVtbl = {
    WavDev_PlaySegmentEx,
    WavDev_StopEx,
    WavDev_Destroy,
    WavDev_SetStartPoint,
    WavDev_SetLoopPoints,
    WavDev_SetRepeats,
    WavDev_SetVolume,
    WavDev_GetPan,
    WavDev_SetPan,
    WavDev_GetFrequency,
    WavDev_SetFrequency,
};

/* WavPcmからデバイスを完成させる。戻るとき *pcm は責任を持って解放される */
static DXRubySoundDev *CreateDevice(WavPcm *pcm, const char **error)
{
    DXRubyWav *w;
    HRESULT hr;
    DSBUFFERDESC desc;
    void *lock1, *lock2;
    DWORD size1, size2;
    DSBPOSITIONNOTIFY notifyPos;
    int acquired = 0;

    w = (DXRubyWav *)calloc(1, sizeof(*w));
    if (!w) { if (error) *error = "Out of memory"; free(pcm->data); return NULL; }
    w->lpVtbl = &WavVtbl;
    InitializeCriticalSection(&w->sendLock);
    w->stopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->quitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!w->stopEvent || !w->quitEvent) { if (error) *error = "CreateEvent"; goto fail; }
    w->wfx = pcm->wfx;
    w->dataBytes = pcm->size;
    w->blockAlign = pcm->wfx.nBlockAlign;
    /* ループ回数はN=合計再生回数(-1=無限)。midiのloopCount(-1=無限)と同じ扱い */
    w->loopCount = 1;
    w->start = 0;
    w->volume = 230;
    w->volumeTarget = 230;
    w->lastSentVolume = 230;

    hr = WavAcquireSound();
    if (FAILED(hr)) { if (error) *error = "DirectSound initialize failed"; goto fail; }
    acquired = 1;

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN |
                   DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    if (pcm->wfx.wBitsPerSample > 16) desc.dwFlags |= DSBCAPS_LOCSOFTWARE;
    desc.dwBufferBytes = pcm->size;
    desc.lpwfxFormat = &pcm->wfx;
    w->buffer = NULL;
    hr = g_wavDSound->lpVtbl->CreateSoundBuffer(g_wavDSound, &desc, &w->buffer, NULL);
    if (FAILED(hr) && pcm->wfx.wBitsPerSample != 16) {
        /* DirectSoundが受け付けない形式は16bit PCMに変換して再試行する */
        DWORD oldBytes = (DWORD)(pcm->wfx.wBitsPerSample / 8);
        BYTE *converted = ConvertTo16bit(pcm->data, pcm->size, &pcm->wfx);
        if (converted) {
            /* 変換後のサイズはサンプル数×2バイト。wfxはConvertTo16bit内で更新済み */
            free(pcm->data);
            pcm->data = converted;
            pcm->size = (oldBytes ? pcm->size / oldBytes : 0) * 2;
            w->wfx = pcm->wfx;
            w->blockAlign = pcm->wfx.nBlockAlign;
            w->dataBytes = pcm->size;
            desc.dwFlags &= ~DSBCAPS_LOCSOFTWARE;
            desc.lpwfxFormat = &pcm->wfx;
            desc.dwBufferBytes = pcm->size;
            w->buffer = NULL;
            hr = g_wavDSound->lpVtbl->CreateSoundBuffer(g_wavDSound, &desc, &w->buffer, NULL);
        }
    }
    if (FAILED(hr)) { if (error) *error = "DirectSound initialize failed"; goto fail; }

    hr = w->buffer->lpVtbl->QueryInterface(w->buffer, &IID_IDirectSoundNotify, (void **)&w->notify);
    if (FAILED(hr)) { if (error) *error = "DirectSound initialize failed"; goto fail; }

    hr = w->buffer->lpVtbl->Lock(w->buffer, 0, 0, &lock1, &size1, &lock2, &size2, DSBLOCK_ENTIREBUFFER);
    if (FAILED(hr) || size1 < pcm->size) { if (error) *error = "DirectSound initialize failed"; goto fail; }
    memcpy(lock1, pcm->data, pcm->size);
    hr = w->buffer->lpVtbl->Unlock(w->buffer, lock1, size1, lock2, size2);
    if (FAILED(hr)) { if (error) *error = "DirectSound initialize failed"; goto fail; }

    notifyPos.dwOffset = DSBPN_OFFSETSTOP;
    notifyPos.hEventNotify = w->stopEvent;
    hr = w->notify->lpVtbl->SetNotificationPositions(w->notify, 1, &notifyPos);
    if (FAILED(hr)) { if (error) *error = "DirectSound initialize failed"; goto fail; }

    free(pcm->data);
    pcm->data = NULL;
    return (DXRubySoundDev *)w;

fail:
    if (w->notify) w->notify->lpVtbl->Release(w->notify);
    if (w->buffer) w->buffer->lpVtbl->Release(w->buffer);
    if (acquired) WavReleaseSound();
    if (w->stopEvent) CloseHandle(w->stopEvent);
    if (w->quitEvent) CloseHandle(w->quitEvent);
    DeleteCriticalSection(&w->sendLock);
    free(w);
    free(pcm->data);
    return NULL;
}

DXRubySoundDev *DXRubyWav_CreateFromMemory(const unsigned char *data, size_t length, const char **error)
{
    WavPcm pcm;
    if (!data || !length) { if (error) *error = "Invalid WAV data"; return NULL; }
    if (!ParseWav(data, length, &pcm, error)) return NULL;
    return CreateDevice(&pcm, error);
}

DXRubySoundDev *DXRubyWav_CreateFromFile(const char *filename, const char **error)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    BYTE *data = NULL;
    DWORD readTotal = 0;
    DXRubySoundDev *dev;
    if (!filename || !MultiByteToWideChar(CP_ACP, 0, filename, -1, path, MAX_PATH)) {
        if (error) *error = "Invalid WAV filename";
        return NULL;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { if (error) *error = "Unable to open WAV file"; return NULL; }
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (unsigned long long)size.QuadPart > MAXDWORD) {
        CloseHandle(file);
        if (error) *error = "Invalid WAV file size";
        return NULL;
    }
    data = (BYTE *)malloc((size_t)size.QuadPart);
    if (!data) { CloseHandle(file); if (error) *error = "Out of memory"; return NULL; }
    while (readTotal < (DWORD)size.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(file, data + readTotal, (DWORD)size.QuadPart - readTotal, &got, NULL) || !got) break;
        readTotal += got;
    }
    CloseHandle(file);
    if (readTotal != (DWORD)size.QuadPart) {
        free(data);
        if (error) *error = "Unable to read WAV file";
        return NULL;
    }
    dev = DXRubyWav_CreateFromMemory(data, (size_t)size.QuadPart, error);
    free(data);
    return dev;
}
