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
    DWORD group;
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

static IDirectMusic8 *g_music;
static IDirectMusicPort8 *g_port;
static IDirectMusicLoader8 *g_loader;
static IDirectMusicCollection8 *g_collection;
static IReferenceClock *g_clock;
static DWORD g_groupUsed[MIDI_GROUPS];
static DWORD g_portRefs;
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
    return 1;
}

static unsigned long long TimeAtTick(const DXRubyMidi *m, DWORD tick)
{
    DWORD i, index = 0;
    for (i = 1; i < m->tempoCount && m->tempos[i].tick <= tick; ++i) index = i;
    return m->tempos[index].usec +
        (unsigned long long)(tick - m->tempos[index].tick) * m->tempos[index].tempo / m->division;
}

static int ParseSMF(DXRubyMidi *m, const BYTE *data, size_t length)
{
    DWORD headerLength, order = 0, pos;
    WORD format, tracks, division, i;
    if (length < 14 || length > MAXDWORD || memcmp(data, "MThd", 4)) return 0;
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

static void CloseGlobalPort(void)
{
    if (g_collection) { g_collection->lpVtbl->Release(g_collection); g_collection = NULL; }
    if (g_loader) { g_loader->lpVtbl->Release(g_loader); g_loader = NULL; }
    if (g_clock) { g_clock->lpVtbl->Release(g_clock); g_clock = NULL; }
    if (g_port) {
        g_port->lpVtbl->Activate(g_port, FALSE);
        g_port->lpVtbl->Release(g_port);
        g_port = NULL;
    }
    if (g_music) { g_music->lpVtbl->Release(g_music); g_music = NULL; }
}

static HRESULT OpenGlobalPort(void)
{
    HRESULT hr;
    GUID portGuid;
    DMUS_PORTPARAMS params;
    DMUS_OBJECTDESC desc;
    WCHAR directory[MAX_PATH];
    hr = CoCreateInstance(&CLSID_DirectMusic, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectMusic8, (void **)&g_music);
    if (FAILED(hr)) { g_error = "CoCreateInstance(DirectMusic)"; return hr; }
    hr = g_music->lpVtbl->SetDirectSound(g_music, NULL, g_hWnd);
    if (FAILED(hr)) { g_error = "SetDirectSound"; return hr; }
    hr = g_music->lpVtbl->GetDefaultPort(g_music, &portGuid);
    if (FAILED(hr)) { g_error = "GetDefaultPort"; return hr; }
    ZeroMemory(&params, sizeof(params));
    params.dwSize = sizeof(params);
    params.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS |
                           DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS;
    params.dwVoices = 128;
    params.dwChannelGroups = MIDI_GROUPS;
    params.dwSampleRate = 44100;
    params.dwEffectFlags = DMUS_EFFECT_REVERB | DMUS_EFFECT_CHORUS;
    hr = g_music->lpVtbl->CreatePort(g_music, &portGuid, &params, &g_port, NULL);
    if (FAILED(hr)) { g_error = "CreatePort"; return hr; }
    hr = g_port->lpVtbl->Activate(g_port, TRUE);
    if (FAILED(hr)) { g_error = "Activate"; return hr; }
    hr = g_port->lpVtbl->GetLatencyClock(g_port, &g_clock);
    if (FAILED(hr)) { g_error = "GetLatencyClock"; return hr; }
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
    return S_OK;
}

static void SendShort(DXRubyMidi *m, REFERENCE_TIME time, BYTE status, BYTE d1, BYTE d2)
{
    DWORD value = status | ((DWORD)d1 << 8) | ((DWORD)d2 << 16);
    MidiLock();
    EnterCriticalSection(&m->sendLock);
    if (SUCCEEDED(m->buffer->lpVtbl->PackStructured(m->buffer, time, m->group, value))) {
        g_port->lpVtbl->PlayBuffer(g_port, m->buffer);
        m->buffer->lpVtbl->Flush(m->buffer);
    }
    LeaveCriticalSection(&m->sendLock);
    LeaveCriticalSection(&g_managerLock);
}

static void SendLong(DXRubyMidi *m, REFERENCE_TIME time, const BYTE *data, DWORD length)
{
    MidiLock();
    EnterCriticalSection(&m->sendLock);
    if (SUCCEEDED(m->buffer->lpVtbl->PackUnstructured(m->buffer, time, m->group, length, (LPBYTE)data))) {
        g_port->lpVtbl->PlayBuffer(g_port, m->buffer);
        m->buffer->lpVtbl->Flush(m->buffer);
    }
    LeaveCriticalSection(&m->sendLock);
    LeaveCriticalSection(&g_managerLock);
}

static REFERENCE_TIME CurrentTime(void)
{
    REFERENCE_TIME t = 0;
    if (g_clock) g_clock->lpVtbl->GetTime(g_clock, &t);
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

/* チャンネル音量(曲側CC7)にマスター音量を掛けて送信する */
static void SendChannelVolume(DXRubyMidi *m, BYTE ch, REFERENCE_TIME time)
{
    int vol = (int)m->channelVolume[ch] * (int)m->volume / 255;
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    SendShort(m, time, (BYTE)(0xb0 | ch), 7, (BYTE)vol);
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
        LONGLONG elapsed = (CurrentTime() - m->fadeStart) / 10000;
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
    if (changed) SendChannelVolumesAll(m, CurrentTime());
}

static void AllNotesOff(DXRubyMidi *m, REFERENCE_TIME t)
{
    BYTE ch;
    for (ch = 0; ch < 16; ++ch) {
        SendShort(m, t, (BYTE)(0xb0 | ch), 120, 0);
        SendShort(m, t, (BYTE)(0xb0 | ch), 123, 0);
    }
}

static DWORD ApiToFileTick(const DXRubyMidi *m, int tick)
{
    unsigned long long result;
    if (tick <= 0) return 0;
    result = (unsigned long long)(unsigned int)tick * m->division / MIDI_PPQ;
    return result > m->totalTick ? m->totalTick : (DWORD)result;
}

static int WaitUntil(DXRubyMidi *m, REFERENCE_TIME target)
{
    while (!InterlockedCompareExchange(&m->stopRequested, 0, 0)) {
        REFERENCE_TIME now;
        LONGLONG remaining;
        DWORD delay;
        UpdateFade(m);
        now = CurrentTime();
        remaining = target - now;
        if (remaining <= 0) return 1;
        delay = (DWORD)(remaining / 10000);
        if (delay > 10) delay = 10;
        if (!delay) delay = 1;
        if (WaitForSingleObject(m->stopEvent, delay) == WAIT_OBJECT_0) return 0;
    }
    return 0;
}

static void SendEvent(DXRubyMidi *m, const MidiEvent *e, REFERENCE_TIME time)
{
    BYTE kind = e->status & 0xf0;
    BYTE ch = e->status & 0x0f;
    if (e->isLong) {
        SendLong(m, time, e->longData, e->longLength);
    } else if (kind == 0x90 || kind == 0x80 || kind == 0xa0) {
        /* ノート番号はキーシフト(周波数指定)で移調する */
        SendShort(m, time, e->status, ShiftNote(m, e->data1, ch), e->data2);
    } else if (kind == 0xb0 && e->data1 == 7) {
        /* チャンネル音量を記憶してマスター音量を掛けて送信 */
        m->channelVolume[ch] = e->data2;
        SendChannelVolume(m, ch, time);
    } else if (kind == 0xb0 && e->data1 == 10) {
        /* 曲側パンにSound#panのオフセットを加える(中心で0) */
        int offset = (int)m->pan * 127 / 20000;
        int value = (int)e->data2 + offset;
        if (value < 0) value = 0;
        if (value > 127) value = 127;
        SendShort(m, time, e->status, e->data1, (BYTE)value);
    } else {
        SendShort(m, time, e->status, e->data1, e->data2);
    }
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
        m->channelVolume[ch] = 127;
        SendChannelVolume(m, ch, time);
        SendShort(m, time, (BYTE)(0xb0 | ch), 10, (BYTE)(64 + (int)m->pan * 127 / 20000));
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
    REFERENCE_TIME start = CurrentTime();
    DWORD first = ApiToFileTick(m, m->start);
    DWORD loopStart = ApiToFileTick(m, m->loopStart);
    DWORD loopEnd = ApiToFileTick(m, m->loopEnd);
    int custom = (m->loopStart != 0 || m->loopEnd != 0) && loopStart <= loopEnd;
    int repeats = m->loopCount;
    unsigned long long firstTime = TimeAtTick(m, first);
    int complete = 0;
    ResetChannels(m, start);
    if (first > 0) SeekRestore(m, first, start);
    if (custom) {
        unsigned long long loopStartTime, loopEndTime, initialDuration, duration;
        if (loopEnd > m->totalTick) loopEnd = m->totalTick;
        if (loopStart > loopEnd) loopStart = loopEnd;
        loopStartTime = TimeAtTick(m, loopStart);
        loopEndTime = TimeAtTick(m, loopEnd);
        initialDuration = loopEndTime > firstTime ? loopEndTime - firstTime : 0;
        duration = loopEndTime > loopStartTime ? loopEndTime - loopStartTime : 0;
        complete = PlayRange(m, first, loopEnd, 1, 0, firstTime, start);
        if (complete && duration) {
            int n = 0;
            while (!InterlockedCompareExchange(&m->stopRequested, 0, 0) &&
                   (repeats == -1 || n < repeats)) {
                if (!PlayRange(m, loopStart, loopEnd, 1, initialDuration + (unsigned long long)n * duration,
                               loopStartTime, start)) { complete = 0; break; }
                ++n;
            }
        }
        if (complete && (repeats != -1 || duration == 0)) {
            unsigned long long base = initialDuration + (unsigned long long)(repeats > 0 ? repeats : 0) * duration;
            DWORD i;
            for (i = 0; i < m->eventCount; ++i) {
                MidiEvent *e = &m->events[i];
                REFERENCE_TIME due;
                if (e->isTempo || e->tick <= loopEnd || e->usec < loopEndTime) continue;
                due = start + (REFERENCE_TIME)((base + e->usec - loopEndTime) * 10ULL);
                if (!WaitUntil(m, due)) { complete = 0; break; }
                SendEvent(m, e, due);
            }
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
    if (complete && !InterlockedCompareExchange(&m->stopRequested, 0, 0)) AllNotesOff(m, CurrentTime());
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
    DMUS_NOTERANGE range;
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
    range.dwLowNote = 0;
    range.dwHighNote = 127;
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
        hr = g_port->lpVtbl->DownloadInstrument(g_port, instrument, &downloaded, &range, 1);
        instrument->lpVtbl->Release(instrument);
        if (SUCCEEDED(hr) && downloaded) m->downloaded[m->downloadedCount++] = downloaded;
    }
    free(voices);
    g_port->lpVtbl->Compact(g_port);
    return S_OK;
}

static HRESULT AcquirePort(DXRubyMidi *m)
{
    HRESULT hr = S_OK;
    DWORD group;
    DMUS_BUFFERDESC desc;
    DWORD bufferSize = 4096, i;
    MidiLock();
    if (!g_portRefs) {
        ZeroMemory(g_groupUsed, sizeof(g_groupUsed));
        hr = OpenGlobalPort();
        if (FAILED(hr)) { CloseGlobalPort(); LeaveCriticalSection(&g_managerLock); return hr; }
    }
    for (group = 0; group < MIDI_GROUPS && g_groupUsed[group]; ++group) {}
    if (group == MIDI_GROUPS) { g_error = "No DirectMusic channel groups available"; LeaveCriticalSection(&g_managerLock); return E_OUTOFMEMORY; }
    g_groupUsed[group] = 1;
    ++g_portRefs;
    m->group = group;
    for (i = 0; i < m->eventCount; ++i) if (m->events[i].isLong && m->events[i].longLength + 64 > bufferSize)
        bufferSize = m->events[i].longLength + 64;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.guidBufferFormat = GUID_NULL;
    desc.cbBuffer = bufferSize;
    hr = g_music->lpVtbl->CreateMusicBuffer(g_music, &desc, &m->buffer, NULL);
    if (FAILED(hr)) {
        g_groupUsed[group] = 0;
        --g_portRefs;
        if (!g_portRefs) CloseGlobalPort();
    }
    LeaveCriticalSection(&g_managerLock);
    return hr;
}

static void ReleaseMidi(DXRubyMidi *m)
{
    DWORD i;
    if (!m) return;
    MidiDev_StopEx((DXRubySoundDev *)m);
    if (m->downloaded && g_port) {
        MidiLock();
        for (i = 0; i < m->downloadedCount; ++i) if (m->downloaded[i]) {
            g_port->lpVtbl->UnloadInstrument(g_port, m->downloaded[i]);
            m->downloaded[i]->lpVtbl->Release(m->downloaded[i]);
        }
        LeaveCriticalSection(&g_managerLock);
    }
    free(m->downloaded);
    if (m->buffer) m->buffer->lpVtbl->Release(m->buffer);
    if (m->group < MIDI_GROUPS && m->buffer) {
        MidiLock();
        g_groupUsed[m->group] = 0;
        if (g_portRefs) --g_portRefs;
        if (!g_portRefs) CloseGlobalPort();
        LeaveCriticalSection(&g_managerLock);
    }
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
    m->volume = 230;
    m->volumeTarget = 230;
    m->frequency = MIDI_DEFAULT_FREQUENCY;
    m->keyShift = 0;
    for (i = 0; i < 16; ++i) m->channelVolume[i] = 127;
    if (!m->stopEvent || !ParseSMF(m, data, length)) {
        if (error) *error = m->stopEvent ? g_error : "CreateEvent";
        ReleaseMidi(m);
        return NULL;
    }
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
    }
    if (m->buffer && g_port && g_clock) AllNotesOff(m, CurrentTime());
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

/* 音量はDirectSoundの減衰(-10000..0)で受け取り、内部の0..255マスター音量へ変換する */
static HRESULT MidiDev_SetVolume(DXRubySoundDev *dev, int volume, int time)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    REFERENCE_TIME now = CurrentTime();
    int sendNow;
    volume = (volume + 9600) * 255 / 9600;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
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
    if (!pan) return E_POINTER;
    *pan = (int)((DXRubyMidi *)dev)->pan;
    return S_OK;
}

static HRESULT MidiDev_SetPan(DXRubySoundDev *dev, int pan)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    BYTE ch;
    if (pan < -10000) pan = -10000;
    if (pan > 10000) pan = 10000;
    m->pan = pan;
    for (ch = 0; ch < 16; ++ch) SendShort(m, CurrentTime(), (BYTE)(0xb0 | ch), 10,
        (BYTE)(((pan + 10000) * 127 + 10000) / 20000));
    return S_OK;
}

static HRESULT MidiDev_GetFrequency(DXRubySoundDev *dev, int *frequency)
{
    if (!frequency) return E_POINTER;
    *frequency = (int)((DXRubyMidi *)dev)->frequency;
    return S_OK;
}

static HRESULT MidiDev_SetFrequency(DXRubySoundDev *dev, int frequency)
{
    DXRubyMidi *m = (DXRubyMidi *)dev;
    double semitones;
    /* 範囲はDirectSoundのDSBFREQUENCY_MIN/MAXに合わせる。範囲外はエラーを返す */
    if (frequency < 100 || frequency > 200000) return E_INVALIDARG;
    m->frequency = frequency;
    /* 周波数比を半音単位のキーシフトに変換する(テンポは変化させない) */
    semitones = 12.0 * log((double)frequency / (double)MIDI_DEFAULT_FREQUENCY) / log(2.0);
    if (semitones >= 0) m->keyShift = (int)(semitones + 0.5);
    else m->keyShift = (int)(semitones - 0.5);
    return S_OK;
}
