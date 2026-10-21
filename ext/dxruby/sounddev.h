#ifndef DXRUBY_SOUNDDEV_H
#define DXRUBY_SOUNDDEV_H

/* Sound再生デバイスの共通インターフェース
 *
 * DirectMusicのセグメント/オーディオパス/パフォーマンスが提供していた
 * 機能のうちSoundクラスが使用しているもののみを、COMを使用せず
 * 同一のメソッド名・引数の単位・HRESULT返しで提供する。
 * MIDI(DXRubyMidi)とWAV(DXRubyWav)の両方がこのインターフェースを実装し、
 * sound.cはメソッド呼び出しを共通化する。
 *
 * 時間(tick)の単位はDirectMusicのミュージックタイム
 * (DMUS_PPQ=768/4分音符)。WAVは実装ではミリ秒として扱う
 * (旧DirectMusicのwaveセグメントが実時間基準で動いていたことに倣う)。

 * 音量(volume)の単位はDirectSoundの減衰(-10000..0、1/100dB単位)。
 * timeの単位はミリ秒。
 */

#include <windows.h>

#ifndef DMUS_PPQ
#define DMUS_PPQ 768    /* DirectMusic PPQ。将来dmusici.hを含めた場合に衝突しないようガード */
#endif

typedef struct DXRubySoundDev DXRubySoundDev;

struct DXRubySoundDevVtbl {
    /* 演奏開始。失敗はHRESULT。 */
    HRESULT (*PlaySegmentEx)(DXRubySoundDev *dev);
    /* 演奏停止。失敗はHRESULT。 */
    HRESULT (*StopEx)(DXRubySoundDev *dev);
    /* 解放 */
    void (*Destroy)(DXRubySoundDev *dev);

    /* 演奏開始位置をtick単位で設定 */
    void (*SetStartPoint)(DXRubySoundDev *dev, int tick);
    /* ループ範囲をtick単位で設定 */
    void (*SetLoopPoints)(DXRubySoundDev *dev, int start, int end);
    /* 繰り返し回数を設定(-1は無限) */
    void (*SetRepeats)(DXRubySoundDev *dev, int count);

    /* 音量(DirectSound減衰単位)とフェード時間(ms)を設定 */
    HRESULT (*SetVolume)(DXRubySoundDev *dev, int volume, int time);
    /* パン(-10000..10000)を取得/設定 */
    HRESULT (*GetPan)(DXRubySoundDev *dev, int *pan);
    HRESULT (*SetPan)(DXRubySoundDev *dev, int pan);
    /* 周波数(DSBFREQUENCY_MIN..MAX)を取得/設定 */
    HRESULT (*GetFrequency)(DXRubySoundDev *dev, int *frequency);
    HRESULT (*SetFrequency)(DXRubySoundDev *dev, int frequency);
};

struct DXRubySoundDev {
    const struct DXRubySoundDevVtbl *lpVtbl;
};

/* 音量(dB)とMIDI CC7の相互変換テーブル。
 * 旧DirectMusic実装のボリュームカーブと同じ、CC7=振幅スケールの曲線。
 *   DXRUBY_CC7ToDB[n]  : CC7値(0..127) → DirectSoundの減衰値(-9600..0、1/100dB単位)
 *                        INT(-log10(n^2/127^2)*2000)、ただしn==0では-9600
 *   DXRUBY_DBToCC7[i]  : 減衰量(i dB、0..96) → CC7値(0..127)
 *                        INT(pow(10, -i/40)*127)
 * DXRuby_InitSoundVolumeTables()がInit_dxruby_Sound()から一度だけ呼ばれる。
 * 減衰値からの参照はDXRUBY_VolumeToCC7()(減衰値-9600..0を受け取り、
 * 1dB(100)刻みでテーブル引き+線形補間する)を使用する。 */
extern long DXRUBY_CC7ToDB[128];
extern BYTE DXRUBY_DBToCC7[97];

void DXRuby_InitSoundVolumeTables(void);
BYTE DXRUBY_VolumeToCC7(long volume);

#endif
