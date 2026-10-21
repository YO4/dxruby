#define WINVER 0x0500
#define _WIN32_WINNT WINVER

#include <windows.h>
#include <dmusici.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "midi.h"
#include "sounddev.h"

#define MIDI_GROUPS 64
#define MIDI_PPQ 768
#define MIDI_DEFAULT_TEMPO 500000
#define MIDI_DEFAULT_FREQUENCY 22050

extern HANDLE g_hWnd;

typedef struct MidiEvent {
    DWORD tick;
    DWORD order;
    unsigned long long usec;
    DWORD tempo;
    BYTE status;
    BYTE data1;
    BYTE data2;
    BYTE isTempo;
    BYTE isLong;
    BYTE orphan;        /* NoteOffの無いNoteOn(DirectMusicは直後にNoteOffを自動送出する) */
    BYTE *longData;
    DWORD longLength;
} MidiEvent;

typedef struct MidiTempo {
    DWORD tick;
    unsigned long long usec;
    DWORD tempo;
} MidiTempo;

struct DXRubyMidi {
    const struct DXRubySoundDevVtbl *lpVtbl;   /* sounddev.h共通インターフェース(必ず先頭) */
    MidiEvent *events;
    DWORD eventCount;
    DWORD eventCapacity;
    MidiTempo *tempos;
    DWORD tempoCount;
    DWORD division;
    DWORD totalTick;
    unsigned long long totalUsec;

    IDirectMusicBuffer8 *buffer;
    IDirectMusicDownloadedInstrument8 **downloaded;
    DWORD downloadedCount;
    IDirectMusicPort8 *port;            /* Sound毎のDMポート(DirectMusicのAudioPath相当) */
    LPDIRECTSOUND8 ds;                  /* ポートの出力先DirectSound */
    LPDIRECTSOUNDBUFFER dsb;            /* 出力先バッファ(pan/frequencyはここに対して行う) */
    IReferenceClock *clock;             /* ポートのレイテンシクロック */
    DWORD group;                        /* 常に1(ポート毎の先頭グループ) */
    int holdsMusicRef;                  /* g_musicRefsを加算済みか(二重解放防止) */
    BYTE noteOn[16][128];               /* 発音中ノート(stop時のNoteOff用) */
    HANDLE worker;
    HANDLE stopEvent;
    CRITICAL_SECTION sendLock;

    volatile LONG stopRequested;
    volatile LONG volume;       /* 現在のマスター音量(0-255) */
    volatile LONG pan;
    volatile LONG frequency;    /* ピッチ指定用周波数(デフォルト22050) */
    volatile LONG keyShift;     /* 半音単位のキーシフト量 */
    int volumeTarget;           /* フェード目標音量 */
    int volumeStart;            /* フェード開始時の音量 */
    int fadeTime;               /* フェード時間(ms) 0で即時 */
    int fading;                 /* フェード中フラグ */
    int lastSentVolume;         /* 最後に全チャンネルへ送信した音量 */
    REFERENCE_TIME fadeStart;   /* フェード開始時刻(レイテンシクロック) */
    BYTE channelVolume[16];     /* チャンネル毎の音量(CC7の値) */
    int start;
    int loopStart;
    int loopEnd;
    int loopCount;
};

/* sounddev.h共通インターフェース(midi.c末尾で実装) */
static void MidiDev_Destroy(DXRubySoundDev *dev);
static HRESULT MidiDev_PlaySegmentEx(DXRubySoundDev *dev);
static HRESULT MidiDev_StopEx(DXRubySoundDev *dev);
static void MidiDev_SetStartPoint(DXRubySoundDev *dev, int tick);
static void MidiDev_SetLoopPoints(DXRubySoundDev *dev, int start, int end);
static void MidiDev_SetRepeats(DXRubySoundDev *dev, int count);
static HRESULT MidiDev_SetVolume(DXRubySoundDev *dev, int volume, int time);
static HRESULT MidiDev_GetPan(DXRubySoundDev *dev, int *pan);
static HRESULT MidiDev_SetPan(DXRubySoundDev *dev, int pan);
static HRESULT MidiDev_GetFrequency(DXRubySoundDev *dev, int *frequency);
static HRESULT MidiDev_SetFrequency(DXRubySoundDev *dev, int frequency);

static const struct DXRubySoundDevVtbl MidiVtbl = {
    MidiDev_PlaySegmentEx,
    MidiDev_StopEx,
    MidiDev_Destroy,
    MidiDev_SetStartPoint,
    MidiDev_SetLoopPoints,
    MidiDev_SetRepeats,
    MidiDev_SetVolume,
    MidiDev_GetPan,
    MidiDev_SetPan,
    MidiDev_GetFrequency,
    MidiDev_SetFrequency,
};

static IDirectMusic8 *g_music;              /* 共有(CoCreateInstanceは一度だけ) */
static IDirectMusicLoader8 *g_loader;
static IDirectMusicCollection8 *g_collection;
static DWORD g_musicRefs;                   /* g_music系の利用カウント(最後の1本で解放) */
static CRITICAL_SECTION g_managerLock;
static volatile LONG g_managerLockState;
static const char *g_error;

static void MidiLock(void)
{
    if (InterlockedCompareExchange(&g_managerLockState, 1, 0) == 0) {
        InitializeCriticalSection(&g_managerLock);
        InterlockedExchange(&g_managerLockState, 2);
    } else {
        while (InterlockedCompareExchange(&g_managerLockState, 2, 2) != 2) SwitchToThread();
    }
    EnterCriticalSection(&g_managerLock);
}

static DWORD ReadBE32(const BYTE *p)
{
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3];
}

static WORD ReadBE16(const BYTE *p)
{
    return (WORD)(((WORD)p[0] << 8) | p[1]);
}

static int ReadVLQ(const BYTE **p, const BYTE *end, DWORD *value)
{
    DWORD result = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        BYTE c;
        if (*p >= end) return 0;
        c = *(*p)++;
        result = (result << 7) | (c & 0x7f);
        if (!(c & 0x80)) {
            *value = result;
            return 1;
        }
    }
    return 0;
}

static int AppendEvent(DXRubyMidi *m, const MidiEvent *event)
{
    if (m->eventCount == m->eventCapacity) {
        DWORD capacity = m->eventCapacity ? m->eventCapacity * 2 : 256;
        size_t bytes;
        MidiEvent *events;
        if (capacity < m->eventCapacity) return 0;
        bytes = (size_t)capacity * sizeof(MidiEvent);
        if (capacity && bytes / sizeof(MidiEvent) != capacity) return 0;
        events = (MidiEvent *)realloc(m->events, bytes);
        if (!events) return 0;
        m->events = events;
        m->eventCapacity = capacity;
    }
    m->events[m->eventCount++] = *event;
    return 1;
}

static int ParseTrack(DXRubyMidi *m, const BYTE *data, size_t length, DWORD *order)
{
    const BYTE *p = data, *end = data + length;
    BYTE running = 0;
    DWORD tick = 0;
    while (p < end) {
        MidiEvent e;
        DWORD delta;
        BYTE status;
        if (!ReadVLQ(&p, end, &delta) || delta > MAXDWORD - tick) return 0;
        tick += delta;
        if (tick > m->totalTick) m->totalTick = tick;
        if (p >= end) return 0;
        status = *p;
        if (status < 0x80) {
            if (running < 0x80 || running >= 0xf0) return 0;
            status = running;
        } else {
            ++p;
            if (status < 0xf0) running = status;
            else running = 0;
        }
        ZeroMemory(&e, sizeof(e));
        e.tick = tick;
        e.order = (*order)++;
        e.status = status;
        if (status < 0xf0) {
            int count = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 1 : 2;
            if (p >= end) return 0;
            e.data1 = *p++;
            if (e.data1 & 0x80) return 0;
            if (count == 2) {
                if (p >= end) return 0;
                e.data2 = *p++;
                if (e.data2 & 0x80) return 0;
            }
            if (!AppendEvent(m, &e)) return 0;
        } else if (status == 0xf0 || status == 0xf7) {
            DWORD len;
            size_t prefix = status == 0xf0 ? 1 : 0;
            size_t suffix = status == 0xf0 ? 1 : 0;
            if (!ReadVLQ(&p, end, &len) || (size_t)(end - p) < len) return 0;
            if ((size_t)len > SIZE_MAX - prefix - suffix || len > MAXDWORD - prefix - suffix) return 0;
            e.longLength = (DWORD)(prefix + len + suffix);
            e.longData = (BYTE *)malloc(e.longLength ? e.longLength : 1);
            if (!e.longData) return 0;
            if (prefix) e.longData[0] = 0xf0;
            if (len) memcpy(e.longData + prefix, p, len);
            if (suffix) {
                if (len && p[len - 1] == 0xf7) --e.longLength;
                else e.longData[e.longLength - 1] = 0xf7;
            }
            e.isLong = 1;
            p += len;
            if (!AppendEvent(m, &e)) {
                free(e.longData);
                return 0;
            }
        } else if (status == 0xff) {
            BYTE type;
            DWORD len;
            if (p >= end) return 0;
            type = *p++;
            if (!ReadVLQ(&p, end, &len) || (size_t)(end - p) < len) return 0;
            if (type == 0x51 && len == 3) {
                e.isTempo = 1;
                e.tempo = ((DWORD)p[0] << 16) | ((DWORD)p[1] << 8) | p[2];
                if (!e.tempo || !AppendEvent(m, &e)) return 0;
            }
            p += len;
        } else {
            return 0;
        }
    }
    return 1;
}

static int __cdecl CompareEvents(const void *a, const void *b)
{
    const MidiEvent *x = (const MidiEvent *)a, *y = (const MidiEvent *)b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return 0;
}

static unsigned long long TimeAtTick(const DXRubyMidi *m, DWORD tick)
{
    DWORD i, index = 0;
    for (i = 1; i < m->tempoCount && m->tempos[i].tick <= tick; ++i) index = i;
    return m->tempos[index].usec +
        (unsigned long long)(tick - m->tempos[index].tick) * m->tempos[index].tempo / m->division;
}

static int BuildTempoMap(DXRubyMidi *m)
{
    DWORD i, tick = 0, tempo = MIDI_DEFAULT_TEMPO;
    unsigned long long usec = 0;
    m->tempos = (MidiTempo *)malloc((size_t)(m->eventCount + 1) * sizeof(MidiTempo));
    if (!m->tempos) return 0;
    m->tempos[0].tick = 0;
    m->tempos[0].usec = 0;
    m->tempos[0].tempo = tempo;
    m->tempoCount = 1;
    for (i = 0; i < m->eventCount; ++i) {
        MidiEvent *e = &m->events[i];
        if (e->tick > tick) {
            usec += (unsigned long long)(e->tick - tick) * tempo / m->division;
            tick = e->tick;
        }
        e->usec = usec;
        if (e->isTempo) {
            tempo = e->tempo;
            if (m->tempos[m->tempoCount - 1].tick == tick)
                m->tempos[m->tempoCount - 1].tempo = tempo;
            else {
                MidiTempo *t = &m->tempos[m->tempoCount++];
                t->tick = tick;
                t->usec = usec;
                t->tempo = tempo;
            }
        }
    }
    if (m->totalTick > tick)
        usec += (unsigned long long)(m->totalTick - tick) * tempo / m->division;
    m->totalUsec = usec;
    /* 各イベントの時刻はテンポマップから求め直す(累積時の丸め誤差を避ける) */
    for (i = 0; i < m->eventCount; ++i) {
        m->events[i].usec = TimeAtTick(m, m->events[i].tick);
    }
    return 1;
}

static int ParseSMF(DXRubyMidi *m, const BYTE *data, size_t length)
{
    DWORD headerLength, order = 0, pos;
    WORD format, tracks, division, i;
    if (length < 14 || length > MAXDWORD || memcmp(data, "MThd", 4)) return 0;
    /* SMF仕様ではヘッダ(およびMTrkの長さ)はビッグエンディアン */
    headerLength = ReadBE32(data + 4);
    if (headerLength < 6 || headerLength > length - 8) return 0;
    format = ReadBE16(data + 8);
    tracks = ReadBE16(data + 10);
    division = ReadBE16(data + 12);
    if (format > 1 || !tracks || !division || (division & 0x8000)) return 0;
    m->division = division;
    pos = 8 + headerLength;
    for (i = 0; i < tracks; ++i) {
        DWORD size;
        if ((size_t)pos + 8 > length || memcmp(data + pos, "MTrk", 4)) return 0;
        size = ReadBE32(data + pos + 4);
        pos += 8;
        if ((size_t)size > length - pos || !ParseTrack(m, data + pos, size, &order)) return 0;
        pos += size;
    }
    if (m->eventCount > 1) qsort(m->events, m->eventCount, sizeof(MidiEvent), CompareEvents);
    return BuildTempoMap(m);
}

static void CloseGlobalDevice(void)
{
    if (g_collection) { g_collection->lpVtbl->Release(g_collection); g_collection = NULL; }
    if (g_loader) { g_loader->lpVtbl->Release(g_loader); g_loader = NULL; }
    if (g_music) { g_music->lpVtbl->Release(g_music); g_music = NULL; }
}

/* 1つのSoundに専用のDMポートと出力先DirectSoundバッファを用意する。
 * DirectMusicは各Soundに専用のAudioPath(DirectSoundバッファ)を持ち、
 * Sound#pan=／Sound#frequency=はそのバッファに対して行うため、Sound単位で
 * 独立した出力経路を持つことで同じ作用範囲になる。 */
static void CloseMidiDevice(DXRubyMidi *m)
{
    if (m->clock) { m->clock->lpVtbl->Release(m->clock); m->clock = NULL; }
    if (m->port) {
        m->port->lpVtbl->Activate(m->port, FALSE);
        m->port->lpVtbl->Release(m->port);
        m->port = NULL;
    }
    if (m->dsb) {
        m->dsb->lpVtbl->Stop(m->dsb);
        m->dsb->lpVtbl->Release(m->dsb);
        m->dsb = NULL;
    }
    if (m->ds) { m->ds->lpVtbl->Release(m->ds); m->ds = NULL; }
    MidiLock();
    /* 加算済みのインスタンスだけが減算する。
     * AcquirePort失敗時とReleaseMidiの両方から呼ばれても二重減算しない。 */
    if (m->holdsMusicRef) {
        m->holdsMusicRef = 0;
        if (g_musicRefs) --g_musicRefs;
    }
    /* 加算前に失敗した場合でも、他に利用者がいなければ
     * 途中まで生成したグローバルオブジェクトを解放する。 */
    if (!g_musicRefs) CloseGlobalDevice();
    LeaveCriticalSection(&g_managerLock);
}

/* ポートの出力先にDirectSoundバッファを割り当てる。
 * DirectMusicはAudioPathのDirectSoundバッファに対して
 * Sound#pan(Sound#pan=)／Sound#frequency(Sound#frequency=)を行っていたため、
 * ここでも同じ方式で定位・ピッチ・音量を制御する。 */
static HRESULT SetupPortBuffer(DXRubyMidi *m)
{
    HRESULT hr;
    WAVEFORMATEX wfx;
    DWORD wfxSize = sizeof(wfx), bufferSize = 0;
    DSBUFFERDESC desc;
    hr = DirectSoundCreate8(&DSDEVID_DefaultPlayback, &m->ds, NULL);
    if (FAILED(hr) || !m->ds) { g_error = "DirectSoundCreate8"; return FAILED(hr) ? hr : E_FAIL; }
    hr = m->ds->lpVtbl->SetCooperativeLevel(m->ds, g_hWnd, DSSCL_PRIORITY);
    if (FAILED(hr)) { g_error = "SetCooperativeLevel"; return hr; }
    hr = m->port->lpVtbl->GetFormat(m->port, &wfx, &wfxSize, &bufferSize);
    if (FAILED(hr)) { g_error = "GetFormat"; return hr; }
    if (!bufferSize) bufferSize = 88200;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME |
                   DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = bufferSize;
    desc.lpwfxFormat = &wfx;
    hr = m->ds->lpVtbl->CreateSoundBuffer(m->ds, &desc, &m->dsb, NULL);
    if (FAILED(hr) || !m->dsb) { g_error = "CreateSoundBuffer"; return FAILED(hr) ? hr : E_FAIL; }
    hr = m->port->lpVtbl->SetDirectSound(m->port, (LPDIRECTSOUND)m->ds, m->dsb);
    if (FAILED(hr)) { g_error = "SetDirectSound"; return hr; }
    hr = m->dsb->lpVtbl->Play(m->dsb, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) { g_error = "Play"; return hr; }
    return S_OK;
}

static HRESULT OpenMidiDevice(DXRubyMidi *m)
{
    HRESULT hr;
    GUID portGuid;
    DMUS_PORTPARAMS params;
    DMUS_OBJECTDESC desc;
    WCHAR directory[MAX_PATH];
    if (!g_music) {
        hr = CoCreateInstance(&CLSID_DirectMusic, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectMusic8, (void **)&g_music);
        if (FAILED(hr)) { g_error = "CoCreateInstance(DirectMusic)"; return hr; }
        hr = g_music->lpVtbl->SetDirectSound(g_music, NULL, g_hWnd);
        if (FAILED(hr)) { g_error = "SetDirectSound"; return hr; }
    }
    if (!g_loader) {
        hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IDirectMusicLoader8, (void **)&g_loader);
        if (FAILED(hr)) { g_error = "CoCreateInstance(DirectMusicLoader)"; return hr; }
        if (!GetCurrentDirectoryW(MAX_PATH, directory)) { g_error = "GetCurrentDirectory"; return E_FAIL; }
        hr = g_loader->lpVtbl->SetSearchDirectory(g_loader, &GUID_DirectMusicAllTypes, directory, FALSE);
        if (FAILED(hr)) { g_error = "SetSearchDirectory"; return hr; }
        ZeroMemory(&desc, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.guidClass = CLSID_DirectMusicCollection;
        desc.guidObject = GUID_DefaultGMCollection;
        desc.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_OBJECT;
        hr = g_loader->lpVtbl->GetObject(g_loader, &desc, &IID_IDirectMusicCollection8, (void **)&g_collection);
        if (FAILED(hr) || !g_collection) { g_error = "Load default GM collection"; return FAILED(hr) ? hr : E_FAIL; }
    }
    hr = g_music->lpVtbl->GetDefaultPort(g_music, &portGuid);
    if (FAILED(hr)) { g_error = "GetDefaultPort"; return hr; }
    ZeroMemory(&params, sizeof(params));
    params.dwSize = sizeof(params);
    params.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS |
                           DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS;
    params.dwVoices = 128;
    params.dwChannelGroups = 1;
    /* DirectMusicのInitAudioデフォルトに合わす */
    params.dwSampleRate = 22050;
    params.dwEffectFlags = DMUS_EFFECT_REVERB | DMUS_EFFECT_CHORUS;
    hr = g_music->lpVtbl->CreatePort(g_music, &portGuid, &params, &m->port, NULL);
    if (FAILED(hr)) { g_error = "CreatePort"; return hr; }
    hr = SetupPortBuffer(m);
    if (FAILED(hr)) return hr;
    hr = m->port->lpVtbl->Activate(m->port, TRUE);
    if (FAILED(hr)) { g_error = "Activate"; return hr; }
    hr = m->port->lpVtbl->GetLatencyClock(m->port, &m->clock);
    if (FAILED(hr)) { g_error = "GetLatencyClock"; return hr; }
    m->group = 1;
    ++g_musicRefs;
    m->holdsMusicRef = 1;
    return S_OK;
}

static void SendShort(DXRubyMidi *m, REFERENCE_TIME time, BYTE status, BYTE d1, BYTE d2)
{
    DWORD value = status | ((DWORD)d1 << 8) | ((DWORD)d2 << 16);
    if (!m->port) return;
    MidiLock();
    EnterCriticalSection(&m->sendLock);
    if (SUCCEEDED(m->buffer->lpVtbl->PackStructured(m->buffer, time, m->group, value))) {
        m->port->lpVtbl->PlayBuffer(m->port, m->buffer);
        m->buffer->lpVtbl->Flush(m->buffer);
    }
    LeaveCriticalSection(&m->sendLock);
    LeaveCriticalSection(&g_managerLock);
}

static void SendLong(DXRubyMidi *m, REFERENCE_TIME time, const BYTE *data, DWORD length)
{
    if (!m->port) return;
    MidiLock();
    EnterCriticalSection(&m->sendLock);
    if (SUCCEEDED(m->buffer->lpVtbl->PackUnstructured(m->buffer, time, m->group, length, (LPBYTE)data))) {
        m->port->lpVtbl->PlayBuffer(m->port, m->buffer);
        m->buffer->lpVtbl->Flush(m->buffer);
    }
    LeaveCriticalSection(&m->sendLock);
    LeaveCriticalSection(&g_managerLock);
}

static REFERENCE_TIME CurrentTime(DXRubyMidi *m)
{
    REFERENCE_TIME t = 0;
    if (m && m->clock) m->clock->lpVtbl->GetTime(m->clock, &t);
    return t;
}

/* キーシフト適用後のノート番号を得る(チャンネル10はドラムなのでシフトしない) */
static BYTE ShiftNote(DXRubyMidi *m, BYTE note, BYTE channel)
{
    int shifted;
    if (channel == 9) return note;
    shifted = (int)note + (int)m->keyShift;
    if (shifted < 0) return 0;
    if (shifted > 127) return 127;
    return (BYTE)shifted;
}

/* 音量(dB)とMIDI CC7の相互変換テーブル(数式で初期化する。sounddev.hを参照) */
long DXRUBY_CC7ToDB[128];
BYTE DXRUBY_DBToCC7[97];

void DXRuby_InitSoundVolumeTables(void)
{
    int i;
    for (i = 0; i < 128; ++i) {
        if (i == 0) {
            DXRUBY_CC7ToDB[0] = -9600;   /* CC7=0は無音 */
        } else {
            DXRUBY_CC7ToDB[i] = -(long)(-log10((double)i * (double)i / (127.0 * 127.0)) * 2000.0);
        }
    }
    for (i = 0; i < 97; ++i) {
        DXRUBY_DBToCC7[i] = (BYTE)(pow(10.0, -(double)i / 40.0) * 127.0);
    }
}

BYTE DXRUBY_VolumeToCC7(long volume)
{
    long fraction, result;
    if (volume < -9600) volume = -9600;
    if (volume > 0) volume = 0;
    volume = -volume;
    fraction = volume % 100;
    volume = volume / 100;
    result = DXRUBY_DBToCC7[volume];
    if (volume < 96) {
        result += ((DXRUBY_DBToCC7[volume + 1] - result) * fraction) / 100;
    }
    return (BYTE)result;
}

/* チャンネル音量(曲側CC7)にマスター音量(DXRUBY_VolumeToCC7変換後の0..127)を合成して送信する。
 * dmime.dllの0x10009b0fと同一の式: DXRUBY_VolumeToCC7(DXRUBY_CC7ToDB[cc] + DXRUBY_CC7ToDB[master]) */
static void SendChannelVolume(DXRubyMidi *m, BYTE ch, REFERENCE_TIME time)
{
    BYTE cc = m->channelVolume[ch];
    int master = (int)m->volume;
    BYTE vol;
    if (cc > 127) cc = 127;
    if (master < 0) master = 0;
    if (master > 127) master = 127;
    vol = (master == 127) ? cc : DXRUBY_VolumeToCC7(DXRUBY_CC7ToDB[cc] + DXRUBY_CC7ToDB[master]);
    SendShort(m, time, (BYTE)(0xb0 | ch), 7, vol);
}

static void SendChannelVolumesAll(DXRubyMidi *m, REFERENCE_TIME time)
{
    BYTE ch;
    for (ch = 0; ch < 16; ++ch) SendChannelVolume(m, ch, time);
}

/* フェード中の音量を更新し、変化があれば全チャンネルへ送信する */
static void UpdateFade(DXRubyMidi *m)
{
    int vol, changed;
    EnterCriticalSection(&m->sendLock);
    if (m->fading) {
        LONGLONG elapsed = (CurrentTime(m) - m->fadeStart) / 10000;
        if (elapsed <= 0) {
            m->volume = m->volumeStart;
        } else if (elapsed >= m->fadeTime) {
            m->volume = m->volumeTarget;
            m->fading = 0;
        } else {
            m->volume = m->volumeStart +
                (m->volumeTarget - m->volumeStart) * (int)elapsed / m->fadeTime;
        }
    }
    vol = (int)m->volume;
    changed = vol != m->lastSentVolume;
    if (changed) m->lastSentVolume = vol;
    LeaveCriticalSection(&m->sendLock);
    if (changed) SendChannelVolumesAll(m, CurrentTime(m));
}

static void AllNotesOff(DXRubyMidi *m, REFERENCE_TIME t)
{
    BYTE ch;
    for (ch = 0; ch < 16; ++ch) {
        SendShort(m, t, (BYTE)(0xb0 | ch), 120, 0);
        SendShort(m, t, (BYTE)(0xb0 | ch), 123, 0);
    }
}

static DWORD ApiToFileTickRaw(const DXRubyMidi *m, unsigned long long tick)
{
    unsigned long long result;
    if (tick == 0) return 0;
    result = tick * m->division / MIDI_PPQ;
    return result > MAXDWORD ? MAXDWORD : (DWORD)result;
}

static DWORD ApiToFileTick(const DXRubyMidi *m, int tick)
{
    DWORD result;
    if (tick <= 0) return 0;
    result = ApiToFileTickRaw(m, (unsigned int)tick);
    return result > m->totalTick ? m->totalTick : result;
}

static int WaitUntil(DXRubyMidi *m, REFERENCE_TIME target)
{
    while (!InterlockedCompareExchange(&m->stopRequested, 0, 0)) {
        REFERENCE_TIME now;
        LONGLONG remaining;
        DWORD delay;
        UpdateFade(m);
        now = CurrentTime(m);
        remaining = target - now;
        if (remaining <= 0) return 1;
        delay = (DWORD)(remaining / 10000);
        if (delay > 10) delay = 10;
        if (!delay) delay = 1;
        if (WaitForSingleObject(m->stopEvent, delay) == WAIT_OBJECT_0) return 0;
    }
    return 0;
}

/* 孤立NoteOn(対応するNoteOffが無いNoteOn)を検出する。
 * DirectMusicはNoteOffの無いNoteOnに対して直後にNoteOffを
 * 自動生成して送出する(音は鳴り続けない)ため、同じ扱いにする。 */
static void MarkOrphanNotes(DXRubyMidi *m)
{
    WORD noteOffCount[16][128];
    DWORD i;
    ZeroMemory(noteOffCount, sizeof(noteOffCount));
    for (i = 0; i < m->eventCount; ++i) {
        MidiEvent *e = &m->events[i];
        BYTE kind, ch;
        if (e->isLong || e->isTempo) continue;
        kind = e->status & 0xf0;
        ch = e->status & 0x0f;
        /* NoteOn vel=0もNoteOffとして扱う(DirectMusicのSendEventと同じ) */
        if (kind == 0x80 || (kind == 0x90 && e->data2 == 0)) noteOffCount[ch][e->data1]++;
    }
    for (i = 0; i < m->eventCount; ++i) {
        MidiEvent *e = &m->events[i];
        BYTE kind, ch;
        if (e->isLong || e->isTempo) continue;
        kind = e->status & 0xf0;
        ch = e->status & 0x0f;
        if (kind == 0x90 && e->data2 != 0) {
            if (noteOffCount[ch][e->data1] > 0) noteOffCount[ch][e->data1]--;
            else e->orphan = 1;
        }
    }
}

static void SendEvent(DXRubyMidi *m, const MidiEvent *e, REFERENCE_TIME time)
{
    BYTE kind = e->status & 0xf0;
    BYTE ch = e->status & 0x0f;
    if (e->isLong) {
        SendLong(m, time, e->longData, e->longLength);
    } else if (kind == 0x90 || kind == 0x80 || kind == 0xa0) {
        /* NoteOn vel=0はNoteOffとして送る */
        BYTE off = (kind == 0x90 && e->data2 == 0) || kind == 0x80;
        BYTE st = off ? (BYTE)(0x80 | ch) : e->status;
        BYTE note = ShiftNote(m, e->data1, ch);
        if (!off && kind == 0x90 && e->orphan) {
            /* 孤立NoteOn: 直後にNoteOffを送出する(DirectMusicと同じ) */
            SendShort(m, time, st, e->data1, e->data2);
            SendShort(m, time, (BYTE)(0x80 | ch), note, 0);
            return;
        }
        /* 発音中ノートを記録する(stop時に個別のNoteOffを送るため) */
        if (kind != 0xa0) {
            if (off) {
                if (ch != 9) m->noteOn[ch][note] = 0;
                else m->noteOn[9][e->data1] = 0;
            } else {
                if (ch != 9) m->noteOn[ch][note] = 1;
                else m->noteOn[9][e->data1] = 1;
            }
        }
        SendShort(m, time, st, e->data1, e->data2);
    } else if (kind == 0xb0 && e->data1 == 7) {
        /* チャンネル音量を記憶してマスター音量を掛けて送信 */
        m->channelVolume[ch] = e->data2;
        SendChannelVolume(m, ch, time);
    } else {
        SendShort(m, time, e->status, e->data1, e->data2);
    }
}

/* 記録済みの発音中ノートへ個別のNoteOffを送る(DirectMusicのstopと同じ) */
static void SendTrackedNoteOffs(DXRubyMidi *m)
{
    BYTE ch, note;
    REFERENCE_TIME now = CurrentTime(m);
    for (ch = 0; ch < 16; ++ch) {
        for (note = 0; note < 128; ++note) {
            if (m->noteOn[ch][note]) {
                /* 記録されているのはシフト適用済みのノート番号 */
                SendShort(m, now, (BYTE)(0x80 | ch), note, 0);
            }
        }
    }
    ZeroMemory(m->noteOn, sizeof(m->noteOn));
}

static int PlayRange(DXRubyMidi *m, DWORD first, DWORD last, int includeLast,
                     unsigned long long base, unsigned long long sourceBase, REFERENCE_TIME start)
{
    DWORD i;
    for (i = 0; i < m->eventCount; ++i) {
        MidiEvent *e = &m->events[i];
        REFERENCE_TIME due;
        if (e->isTempo || e->tick < first) continue;
        if (e->tick > last || (!includeLast && e->tick == last)) break;
        if (e->usec < sourceBase) continue;
        due = start + (REFERENCE_TIME)((base + e->usec - sourceBase) * 10ULL);
        if (!WaitUntil(m, due)) return 0;
        SendEvent(m, e, due);
    }
    if (last >= first) {
        unsigned long long end = TimeAtTick(m, last);
        if (end >= sourceBase && !WaitUntil(m, start + (REFERENCE_TIME)((base + end - sourceBase) * 10ULL)))
            return 0;
    }
    return !InterlockedCompareExchange(&m->stopRequested, 0, 0);
}

static void ResetChannels(DXRubyMidi *m, REFERENCE_TIME time)
{
    BYTE ch;
    for (ch = 0; ch < 16; ++ch) {
        SendShort(m, time, (BYTE)(0xb0 | ch), 120, 0);
        SendShort(m, time, (BYTE)(0xb0 | ch), 123, 0);
        SendShort(m, time, (BYTE)(0xb0 | ch), 121, 0);
        SendShort(m, time, (BYTE)(0xb0 | ch), 0, 0);
        SendShort(m, time, (BYTE)(0xb0 | ch), 32, 0);
        SendShort(m, time, (BYTE)(0xc0 | ch), 0, 0);
        m->channelVolume[ch] = 100;
        SendChannelVolume(m, ch, time);
        SendShort(m, time, (BYTE)(0xb0 | ch), 10, 64);
        SendShort(m, time, (BYTE)(0xb0 | ch), 91, 40);
        SendShort(m, time, (BYTE)(0xb0 | ch), 93, 0);
    }
    EnterCriticalSection(&m->sendLock);
    m->lastSentVolume = (int)m->volume;
    LeaveCriticalSection(&m->sendLock);
}

/* 開始位置が曲の途中(seek)の場合に、そこまでのチャンネル状態を復元する。
 * GuruGuruSMF4 GGS4Playのseek処理(Ggs4Controller/Player::Seek)に準拠:
 * - システムエクスクルーシブ、バンクセレクト/RPN系(CC0,6,32,38,98,99,100,101,126,127)は
 *   走査中にそのまま即時送信する
 * - 上記以外の状態系CC(モジュレーション、エクスプレッション、ペダル等)は
 *   種別毎に最後の値を記憶し、走査後に復元送信する
 * - ノートイベント、チャンネルモードメッセージ(CC120-125)、CC111は復元しない */
static void SeekRestore(DXRubyMidi *m, DWORD first, REFERENCE_TIME time)
{
    BYTE cc[16][128];
    BYTE ccValid[16][128];
    BYTE program[16], programValid[16];
    WORD pitch[16];
    BYTE pitchValid[16];
    BYTE cAf[16], cAfValid[16];
    DWORD i, ch, ccnum;

    ZeroMemory(ccValid, sizeof(ccValid));
    ZeroMemory(programValid, sizeof(programValid));
    ZeroMemory(pitchValid, sizeof(pitchValid));
    ZeroMemory(cAfValid, sizeof(cAfValid));

    for (i = 0; i < m->eventCount; ++i) {
        const MidiEvent *e = &m->events[i];
        BYTE kind, ch;
        if (e->tick >= first) break;
        if (e->isTempo) continue;
        if (e->isLong) {
            SendLong(m, time, e->longData, e->longLength);
            continue;
        }
        kind = e->status & 0xf0;
        ch = e->status & 0x0f;
        switch (kind) {
        case 0xb0:
            switch (e->data1) {
            case 0: case 6: case 32: case 38: case 98: case 99:
            case 100: case 101: case 126: case 127:
                /* バンクセレクト・RPN系はシーケンスの順序が重要なので即時送信 */
                SendShort(m, time, e->status, e->data1, e->data2);
                break;
            case 111: /* GGSのループマーカー */
            case 120: case 121: case 123: case 124: case 125:
                break;
            default:
                cc[ch][e->data1] = e->data2;
                ccValid[ch][e->data1] = 1;
                break;
            }
            break;
        case 0xc0:
            program[ch] = e->data1;
            programValid[ch] = 1;
            break;
        case 0xe0:
            pitch[ch] = (WORD)(e->data1 | (e->data2 << 7));
            pitchValid[ch] = 1;
            break;
        case 0xd0:
            cAf[ch] = e->data1;
            cAfValid[ch] = 1;
            break;
        default:
            break;
        }
    }

    /* 記憶した状態を復元送信する(CC7/CC10はSound#volume/panとのミックスが効く) */
    for (ch = 0; ch < 16; ++ch) {
        for (ccnum = 0; ccnum < 128; ++ccnum) {
            MidiEvent ev;
            if (!ccValid[ch][ccnum]) continue;
            ZeroMemory(&ev, sizeof(ev));
            ev.status = (BYTE)(0xb0 | ch);
            ev.data1 = (BYTE)ccnum;
            ev.data2 = cc[ch][ccnum];
            SendEvent(m, &ev, time);
        }
        if (programValid[ch]) SendShort(m, time, (BYTE)(0xc0 | ch), program[ch], 0);
        if (pitchValid[ch]) {
            SendShort(m, time, (BYTE)(0xe0 | ch), (BYTE)(pitch[ch] & 0x7f), (BYTE)((pitch[ch] >> 7) & 0x7f));
        }
        if (cAfValid[ch]) SendShort(m, time, (BYTE)(0xd0 | ch), cAf[ch], 0);
    }
}

static unsigned __stdcall MidiWorker(void *arg)
{
    DXRubyMidi *m = (DXRubyMidi *)arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    REFERENCE_TIME start = CurrentTime(m);
    DWORD first = ApiToFileTick(m, m->start);
    int custom = (m->loopStart != 0 || m->loopEnd != 0) && m->loopStart <= m->loopEnd;
    int repeats = m->loopCount;
    unsigned long long firstTime = TimeAtTick(m, first);
    int complete = 0;
    ZeroMemory(m->noteOn, sizeof(m->noteOn));   /* 前回playの発音状態をクリア */
    ResetChannels(m, start);
    if (first > 0) SeekRestore(m, first, start);
    if (custom) {
        /* 旧32bit DirectMusic(旧26)の実測に合わせる:
         * - loop_start は次の192 DM tick(4分音符)境界へ切り上げる。
         * - ループ長は元の loop_end - loop_start を維持する。
         * - 実効ループ範囲は [量子化開始, 量子化開始+ループ長)。
         *   開始側のイベントは含み、終端側のイベントは含まない。
         * - 初回は曲頭から実効終端まで通し、以降は実効範囲を反復し、
         *   最後に実効終端から残りを再生する。 */
        const unsigned long long quantum = MIDI_PPQ / 4;   /* 192 */
        unsigned long long startApi = m->loopStart > 0 ? (unsigned int)m->loopStart : 0;
        unsigned long long endApi = m->loopEnd > 0 ? (unsigned int)m->loopEnd : 0;
        unsigned long long startQApi = (startApi + quantum - 1) / quantum * quantum;
        unsigned long long durationApi = endApi > startApi ? endApi - startApi : 0;
        unsigned long long loopStartTime, effectiveEndTime, initialDuration, duration;
        DWORD loopStartTick = ApiToFileTickRaw(m, startQApi);
        DWORD effectiveEndTick = ApiToFileTickRaw(m, startQApi + durationApi);
        loopStartTime = TimeAtTick(m, loopStartTick);
        effectiveEndTime = TimeAtTick(m, effectiveEndTick);
        initialDuration = effectiveEndTime > firstTime ? effectiveEndTime - firstTime : 0;
        duration = effectiveEndTime > loopStartTime ? effectiveEndTime - loopStartTime : 0;
        {
            char *isSpill = calloc(m->eventCount ? m->eventCount : 1, 1);
            unsigned long long lastBodyTime = effectiveEndTime;
            DWORD i;
            /* ループ内NoteOnに対応するNoteOffがループ外にある場合、
             * DirectMusicはそのNoteOffをループ本体に含めて毎回送出する。 */
            if (isSpill) {
                for (i = 0; i < m->eventCount; ++i) {
                    MidiEvent *on = &m->events[i];
                    BYTE kind, ch, note;
                    DWORD j;
                    if (on->isTempo || on->isLong) continue;
                    kind = on->status & 0xf0;
                    ch = on->status & 0x0f;
                    if (kind != 0x90 || on->data2 == 0) continue;
                    if (on->tick >= effectiveEndTick) continue;
                    note = on->data1;
                    for (j = i + 1; j < m->eventCount; ++j) {
                        MidiEvent *off = &m->events[j];
                        BYTE okind, och;
                        if (off->isTempo || off->isLong) continue;
                        okind = off->status & 0xf0;
                        och = off->status & 0x0f;
                        if (och != ch) continue;
                        if (okind == 0x90 && off->data2 != 0 && off->data1 == note) break;
                        if (okind == 0x80 || (okind == 0x90 && off->data2 == 0)) {
                            if (off->data1 == note) {
                                if (off->tick > effectiveEndTick &&
                                    off->usec <= effectiveEndTime + 3906) {
                                    isSpill[j] = 1;
                                    if (off->usec > lastBodyTime) lastBodyTime = off->usec;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            /* 初回パス: ループ本体(終端排他)を再生する */
            complete = PlayRange(m, first, effectiveEndTick, 0, 0, firstTime, start);
            if (complete && isSpill) {
                for (i = 0; i < m->eventCount; ++i) {
                    MidiEvent *e = &m->events[i];
                    REFERENCE_TIME due;
                    if (!isSpill[i]) continue;
                    if (e->tick < first) continue;
                    due = start + (REFERENCE_TIME)(e->usec * 10ULL);
                    if (!WaitUntil(m, due)) { complete = 0; break; }
                    SendEvent(m, e, due);
                }
            }
            if (complete && duration) {
                int n = 0;
                while (!InterlockedCompareExchange(&m->stopRequested, 0, 0) &&
                       (repeats == -1 || n < repeats)) {
                    if (!PlayRange(m, loopStartTick, effectiveEndTick, 0,
                                   initialDuration + (unsigned long long)n * duration,
                                   loopStartTime, start)) { complete = 0; break; }
                    if (complete && isSpill) {
                        for (i = 0; i < m->eventCount; ++i) {
                            MidiEvent *e = &m->events[i];
                            REFERENCE_TIME due;
                            if (!isSpill[i]) continue;
                            if (e->tick < loopStartTick) continue;
                            due = start + (REFERENCE_TIME)((initialDuration +
                                (unsigned long long)n * duration + e->usec - loopStartTime) * 10ULL);
                            if (!WaitUntil(m, due)) { complete = 0; break; }
                            SendEvent(m, e, due);
                        }
                    }
                    ++n;
                }
            }
            if (complete && (repeats != -1 || duration == 0 || effectiveEndTick == loopStartTick)) {
                unsigned long long base = initialDuration +
                    (unsigned long long)(repeats > 0 ? repeats : 0) * duration;
                for (i = 0; i < m->eventCount; ++i) {
                    MidiEvent *e = &m->events[i];
                    REFERENCE_TIME due;
                    if (e->isTempo) continue;
                    if (isSpill && isSpill[i]) continue;
                    if (e->tick < effectiveEndTick) continue;
                    if (e->usec < lastBodyTime) continue;
                    due = start + (REFERENCE_TIME)((base + e->usec - lastBodyTime) * 10ULL);
                    if (!WaitUntil(m, due)) { complete = 0; break; }
                    SendEvent(m, e, due);
                }
            }
            if (isSpill) free(isSpill);
        }
    } else {
        unsigned long long duration = m->totalUsec > firstTime ? m->totalUsec - firstTime : 0;
        complete = PlayRange(m, first, m->totalTick, 1, 0, firstTime, start);
        if (complete && duration) {
            int n = 0;
            while (!InterlockedCompareExchange(&m->stopRequested, 0, 0) &&
                   (repeats == -1 || n < repeats)) {
                if (!PlayRange(m, first, m->totalTick, 1, (unsigned long long)(n + 1) * duration,
                               firstTime, start)) { complete = 0; break; }
                ++n;
            }
        }
    }
    /* stop時は発音中のノートへ個別のNoteOffを送る(DirectMusicと同じ) */
    if (InterlockedCompareExchange(&m->stopRequested, 0, 0)) SendTrackedNoteOffs(m);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

static void AddVoice(DWORD *voices, DWORD *count, DWORD voice)
{
    DWORD i;
    for (i = 0; i < *count; ++i) if (voices[i] == voice) return;
    voices[(*count)++] = voice;
}

static HRESULT DownloadVoices(DXRubyMidi *m)
{
    DWORD *voices, count = 0, i, bankMSB[16] = {0}, bankLSB[16] = {0}, program[16] = {0};
    DWORD capacity = m->eventCount + 16;
    HRESULT hr = S_OK;
    voices = (DWORD *)malloc((size_t)capacity * sizeof(DWORD));
    if (!voices) return E_OUTOFMEMORY;
    for (i = 0; i < 16; ++i) AddVoice(voices, &count, (i == 9 ? 0x80000000UL : 0));
    for (i = 0; i < m->eventCount; ++i) {
        MidiEvent *e = &m->events[i];
        BYTE kind = e->status & 0xf0, ch = e->status & 0x0f;
        DWORD voice;
        if (e->isTempo || e->isLong) continue;
        if (kind == 0xb0 && e->data1 == 0) bankMSB[ch] = e->data2;
        else if (kind == 0xb0 && e->data1 == 32) bankLSB[ch] = e->data2;
        else if (kind == 0xc0) program[ch] = e->data1;
        else continue;
        voice = bankMSB[ch] << 16 | bankLSB[ch] << 8 | program[ch];
        if (ch == 9) voice |= 0x80000000UL;
        AddVoice(voices, &count, voice);
    }
    m->downloaded = (IDirectMusicDownloadedInstrument8 **)calloc(count, sizeof(*m->downloaded));
    if (!m->downloaded) { free(voices); return E_OUTOFMEMORY; }
    for (i = 0; i < count; ++i) {
        IDirectMusicInstrument8 *instrument = NULL;
        IDirectMusicDownloadedInstrument8 *downloaded = NULL;
        DWORD voice = voices[i];
        hr = g_collection->lpVtbl->GetInstrument(g_collection, voice, &instrument);
        if (FAILED(hr) || !instrument) {
            hr = g_collection->lpVtbl->GetInstrument(g_collection, voice & 0xff0000ffUL, &instrument);
            if (FAILED(hr) || !instrument) continue;
            instrument->lpVtbl->SetPatch(instrument, voice);
        }
        hr = m->port->lpVtbl->DownloadInstrument(m->port, instrument, &downloaded, NULL, 0);
        instrument->lpVtbl->Release(instrument);
        if (SUCCEEDED(hr) && downloaded) m->downloaded[m->downloadedCount++] = downloaded;
    }
    free(voices);
    m->port->lpVtbl->Compact(m->port);
    return S_OK;
}

static HRESULT AcquirePort(DXRubyMidi *m)
{
    HRESULT hr = S_OK;
    DWORD group;
    DMUS_BUFFERDESC desc;
    DWORD bufferSize = 4096, i;
    MidiLock();
    /* Sound毎に専用のDMポート(旧実装のAudioPath相当)を用意する */
    hr = OpenMidiDevice(m);
    if (FAILED(hr)) { LeaveCriticalSection(&g_managerLock); CloseMidiDevice(m); return hr; }
    for (i = 0; i < m->eventCount; ++i) if (m->events[i].isLong && m->events[i].longLength + 64 > bufferSize)
        bufferSize = m->events[i].longLength + 64;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.guidBufferFormat = GUID_NULL;
    desc.cbBuffer = bufferSize;
    hr = g_music->lpVtbl->CreateMusicBuffer(g_music, &desc, &m->buffer, NULL);
    if (FAILED(hr)) { LeaveCriticalSection(&g_managerLock); CloseMidiDevice(m); return hr; }
    LeaveCriticalSection(&g_managerLock);
    return hr;
}

static void ReleaseMidi(DXRubyMidi *m)
{
    DWORD i;
    if (!m) return;
    MidiDev_StopEx((DXRubySoundDev *)m);
    if (m->downloaded && m->port) {
        MidiLock();
        for (i = 0; i < m->downloadedCount; ++i) if (m->downloaded[i]) {
            m->port->lpVtbl->UnloadInstrument(m->port, m->downloaded[i]);
            m->downloaded[i]->lpVtbl->Release(m->downloaded[i]);
        }
        LeaveCriticalSection(&g_managerLock);
    }
    free(m->downloaded);
    if (m->buffer) m->buffer->lpVtbl->Release(m->buffer);
    CloseMidiDevice(m);
    for (i = 0; i < m->eventCount; ++i) free(m->events[i].longData);
    free(m->events);
    free(m->tempos);
    if (m->stopEvent) CloseHandle(m->stopEvent);
    DeleteCriticalSection(&m->sendLock);
    free(m);
}

DXRubySoundDev *DXRubyMidi_CreateFromMemory(const unsigned char *data, size_t length, const char **error)
{
    DXRubyMidi *m;
    HRESULT hr;
    DWORD i;
    g_error = "Invalid MIDI file";
    if (!data || !length) { if (error) *error = g_error; return NULL; }
    m = (DXRubyMidi *)calloc(1, sizeof(*m));
    if (!m) { if (error) *error = "Out of memory"; return NULL; }
    m->lpVtbl = &MidiVtbl;
    InitializeCriticalSection(&m->sendLock);
    m->stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    m->loopCount = -1;
    /* 初期音量は0(最大音量)。DirectMusicのAudioPathの初期値と同じで、
     * 実際の減衰はSound.cが読み込み時にSetVolume(-942)で行う */
    m->volume = DXRUBY_VolumeToCC7(0);
    m->volumeTarget = m->volume;
    m->frequency = MIDI_DEFAULT_FREQUENCY;
    m->keyShift = 0;
    for (i = 0; i < 16; ++i) m->channelVolume[i] = 100;
    if (!m->stopEvent || !ParseSMF(m, data, length)) {
        if (error) *error = m->stopEvent ? g_error : "CreateEvent";
        ReleaseMidi(m);
        return NULL;
    }
    MarkOrphanNotes(m);
    g_error = "DirectMusic MIDI initialization failed";
    hr = AcquirePort(m);
    if (SUCCEEDED(hr)) {
        MidiLock();
        hr = DownloadVoices(m);
        if (FAILED(hr)) g_error = "Download instruments";
        LeaveCriticalSection(&g_managerLock);
    }
    if (FAILED(hr)) { if (error) *error = g_error; ReleaseMidi(m); return NULL; }
    if (error) *error = NULL;
    return (DXRubySoundDev *)m;
}

DXRubySoundDev *DXRubyMidi_CreateFromFile(const char *filename, const char **error)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    BYTE *data;
    DWORD readTotal = 0;
    DXRubySoundDev *m;
    if (!filename || !MultiByteToWideChar(CP_ACP, 0, filename, -1, path, MAX_PATH)) {
        if (error) *error = "Invalid MIDI filename";
        return NULL;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { if (error) *error = "Unable to open MIDI file"; return NULL; }
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (unsigned long long)size.QuadPart > MAXDWORD) {
        CloseHandle(file); if (error) *error = "Invalid MIDI file size"; return NULL;
    }
    data = (BYTE *)malloc((size_t)size.QuadPart);
    if (!data) { CloseHandle(file); if (error) *error = "Out of memory"; return NULL; }
    while (readTotal < (DWORD)size.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(file, data + readTotal, (DWORD)size.QuadPart - readTotal, &got, NULL) || !got) break;
        readTotal += got;
    }
    CloseHandle(file);
    if (readTotal != (DWORD)size.QuadPart) { free(data); if (error) *error = "Unable to read MIDI file"; return NULL; }
    m = DXRubyMidi_CreateFromMemory(data, (size_t)size.QuadPart, error);
    free(data);
    return m;
}

/*------------------------------------------------------------------------
   sounddev.h共通インターフェースの実装。
   メソッド名・引数の単位はDirectMusic(DirectSound)のものに合わせてある。
 ------------------------------------------------------------------------*/

static void MidiDev_Destroy(DXRubySoundDev *dev) { ReleaseMidi((DXRubyMidi *)dev); }

static HRESULT MidiDev_StopEx(DXRubySoundDev *dev)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    if (m->worker) {
        InterlockedExchange(&m->stopRequested, 1);
        SetEvent(m->stopEvent);
        WaitForSingleObject(m->worker, INFINITE);
        CloseHandle(m->worker);
        m->worker = NULL;
        /* 発音中ノートの個別NoteOffはMidiWorkerの終了処理で送られる */
    }
    return S_OK;
}

static HRESULT MidiDev_PlaySegmentEx(DXRubySoundDev *dev)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    unsigned id;
    MidiDev_StopEx(dev);
    ResetEvent(m->stopEvent);
    InterlockedExchange(&m->stopRequested, 0);
    /* 前回再生途中のフェード状態を破棄し、目標音量で再生を開始する */
    EnterCriticalSection(&m->sendLock);
    m->volume = m->volumeTarget;
    m->fading = 0;
    m->fadeTime = 0;
    m->lastSentVolume = (int)m->volume;
    LeaveCriticalSection(&m->sendLock);
    m->worker = (HANDLE)_beginthreadex(NULL, 0, MidiWorker, m, 0, &id);
    return m->worker != NULL ? S_OK : E_FAIL;
}

static void MidiDev_SetStartPoint(DXRubySoundDev *dev, int tick)
{
    ((DXRubyMidi *)dev)->start = tick;
}

static void MidiDev_SetLoopPoints(DXRubySoundDev *dev, int start, int end)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    m->loopStart = start;
    m->loopEnd = end;
}

static void MidiDev_SetRepeats(DXRubySoundDev *dev, int count)
{
    ((DXRubyMidi *)dev)->loopCount = count;
}

/* 音量はDirectSoundの減衰(-9600..0、1/100dB単位)で受け取り、VolumeToMidiで0..127へ変換する */
static HRESULT MidiDev_SetVolume(DXRubySoundDev *dev, int volume, int time)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    REFERENCE_TIME now = CurrentTime(m);
    int sendNow;
    volume = (int)DXRUBY_VolumeToCC7(volume);
    EnterCriticalSection(&m->sendLock);
    if (time > 0 && m->worker) {
        /* 演奏中は現在値から目標値へtimeミリ秒かけてなめらかに変化させる */
        m->volumeStart = (int)m->volume;
        m->volumeTarget = volume;
        m->fadeTime = time;
        m->fadeStart = now;
        m->fading = 1;
        sendNow = 0;
    } else {
        m->volume = volume;
        m->volumeTarget = volume;
        m->fading = 0;
        m->fadeTime = 0;
        m->lastSentVolume = volume;
        sendNow = 1;
    }
    LeaveCriticalSection(&m->sendLock);
    if (sendNow) SendChannelVolumesAll(m, now);
    return S_OK;
}

static HRESULT MidiDev_GetPan(DXRubySoundDev *dev, int *pan)
{
    long value = 0;
    DXRubyMidi *m = (DXRubyMidi *)dev;
    if (!pan) return E_POINTER;
    if (!m->dsb) return E_FAIL;
    if (FAILED(m->dsb->lpVtbl->GetPan(m->dsb, &value))) return E_FAIL;
    *pan = (int)value;
    return S_OK;
}

static HRESULT MidiDev_SetPan(DXRubySoundDev *dev, int pan)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    if (!m->dsb) return E_FAIL;
    /* DirectMusicと同じく、DirectSoundバッファのパンを設定する(曲側のCC10は変更しない) */
    return m->dsb->lpVtbl->SetPan(m->dsb, pan);
}

static HRESULT MidiDev_GetFrequency(DXRubySoundDev *dev, int *frequency)
{
    DWORD value = 0;
    DXRubyMidi *m = (DXRubyMidi *)dev;
    if (!frequency) return E_POINTER;
    if (!m->dsb) return E_FAIL;
    if (FAILED(m->dsb->lpVtbl->GetFrequency(m->dsb, &value))) return E_FAIL;
    *frequency = (int)value;
    return S_OK;
}

static HRESULT MidiDev_SetFrequency(DXRubySoundDev *dev, int frequency)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    if (!m->dsb) return E_FAIL;
    /* DirectMusicと同じく、DirectSoundバッファの周波数(再サンプリング)を設定する */
    if (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX) return E_INVALIDARG;
    return m->dsb->lpVtbl->SetFrequency(m->dsb, (DWORD)frequency);
}
