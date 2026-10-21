# Sound再生の実装差分記録（旧DirectMusic実装 vs 新実装）

本書は、旧実装（DXRuby 1.4.7 / DirectMusic 8 ベース）と、
本リポジトリの新実装（`ext\dxruby\wav.c` / `midi.c` / `sound.c`）の
**動作上の残差**を、実測に基づいて記録したものである。

- 記載のない項目は「実測で一致」を確認済み。
- 「未検証」と明記したものは実測していない（推測を含む）。

---

## 0. 検証方法

| 目的 | 方法 |
|---|---|
| 実際にポートへ送信されるMIDIメッセージを観測 | `IDirectMusicPort::PlayBuffer` のvtableをパッチし、`IDirectMusicBuffer::GetNextEvent` で全イベントを抽出 |
| シンセの実際の出力PCMを観測 | `IDirectMusicSynth8::Render` のvtableをパッチし、ピーク/RMSを計測 |
| DirectSoundバッファ操作を観測 | `IDirectSoundBuffer::SetVolume/SetPan/SetFrequency/Play/Stop` のvtableをパッチ |
| セグメント操作を観測 | `IDirectMusicSegment8::SetLength/SetStartPoint/SetRepeats/SetLoopPoints/Download` をパッチ |


---

## 1. MIDI（`.mid`）経路

### 1.1 差異（実測で確認）

| # | 項目 | 旧実装 | 新実装 | 影響 |
|---|---|---|---|---|
| 1 | **出力レイテンシ** | ポートの遅延を考慮したタイミングで送信 | 約 **7〜8ms 早い** | 曲全体の開始が7〜8msずれる。複数音を同期させる場合は要注意 |
| 2 | **同時刻イベントの発送順序** | ch9→ch2→ch5→ch6… の順 | ch2→ch5→ch6→ch9… の順 | 同一時刻のイベント順序のみ。音の成分・長さは同一（実測でノート列は一致） |
| 3 | **ループ境界の旧との差** | 旧側の反復規則が実測で確定した（6.3節） | **解消済み**（6.3節で送出順が完全一致） | — |
| 5 | **`pan=` の作用域** | Sound毎のAudioPathバッファに対するSetPan | **解消済み**: 各Soundに専用のDMポート＋出力先dsbを実装し、フックでバッファがSound毎に独立することを実測（6.1節） | — |
| 6 | **`frequency=` の作用域** | 〃（Sound単位） | **解消済み**（同上） | — |
| 7 | **停止時のメッセージ** | 発音中ノートへの個別NoteOff（減衰を伴う自然停止） | **解消済み**: 発音中ノートを記録しstop時に個別NoteOffを送出（実測で旧と同一のNoteOff系列）。旧にあった先行バッファ約130ms分の継続発音は再現しない（新実装はリアルタイム送信のため） | 旧ではstop後の約130msだけ余計に音が続いたが、これについては未再現 |
| 7b | **孤立NoteOn(NoteOffの無いNoteOn)の扱い** | NoteOnの直後にNoteOffを**自動生成して送出**（音は鳴り続けない） | **解消済み**: パース時に孤立NoteOnを検出し、送信時に直後にNoteOffを送出（6.8節参照） | — |
| 8 | **フェードの更新刻み** | DM内部クロック（≈20ms刻み） | ワーカーの10ms周期 | フェード曲線の刻み粒度が旧より細かい（線の形は同一） |

### 1.2 一致を確認済み

| 項目 | 実測結果 |
|---|---|
| サンプルレート / エフェクト | 22050Hz / エフェクトはポートへ REVERB+CHORUS を要求（旧InitAudioデフォルトと同一）。dmsynthが適用するのは REVERB のみ（旧新とも Synth_Open の granted effects=0x1）で、聴感上のエフェクトは旧と同一 |
| ポート構成 | 旧: Performance+**Sound毎AudioPath** / 新: **Sound毎のDMポート+出力先dsb**（pan/frequency/pan/frequencyがSound単位になる。voices/groups/rate等は1項目目参照。旧の`fShare=1`→新は`0`） |
| 初期CC7（全ch） | 旧・新とも `58`（0x3A） |
| トラック別CC7合成 | `town.mid` のch0〜ch10で完全一致（61,62,74,54,56,74,74,74,66,56） |
| マスター音量→CC7対応 | パス音量 0/-40/-189/-565/-1318/-4782/-942 dB で **ネイティブDirectMusicと完全一致**（100,98,89,72,47,6,58） |
| フェードの演算領域 | **CC7(0..127)=振幅スケール**の線形ランプ（dBスケールではない）。新旧一致（実測: 旧 6→8→10→12/13→15…、新 6→10→15→20→24→29→31） |
| CC7量子化（丸め誤差） | 変換テーブルを数式で生成（`DXRUBY_CC7ToDB`／`DXRUBY_DBToCC7`[97]）→ 誤差の値まで旧と同一 |
| NoteOn vel=0 | 新はNoteOff(0x80)に変換して送出（旧の挙動に一致） |
| ループ終端（`loop_end`ちょうど） | **半開区間 `[loopStart, loopEnd)`** で一致。終端ちょうどのイベントは反復に含めず、繰り返し終了後に1回だけ再生（修正済み） |
| ループ回数 | `SetRepeats(n)`＝合計n+1回（DM互換）。無指定時はMIDIは-1（無限）／WAVは0（1回） |
| 例外メッセージ | `Set volume error - SetVolume` / `pan setting error - SetPan` / `frequency setting error - setFrequency` / `Can not be set to Wav data - Sound_loopStart=` すべて同一 |
| `start=`（シーク） | ノート時刻が±1msで一致（バイト順により1tick差の可能性がある個所あり） |
| テンポマップ | SMFのテンポイベントに従う。テンポマップからの時刻再計算で丸め誤差を除去（累積誤差なし） |

---

## 2. WAV（`.wav`）経路

### 2.1 差異（実測で確認）

| # | 項目 | 旧実装 | 新実装 | 影響 |
|---|---|---|---|---|
| 1 | **`start=` の挙動** | `SetLength(start+1)`＋`SetStartPoint(start)` で波形が `start+1`tickに切り詰められ、**ほぼ無音**（実測: `SetLength`/`SetStartPoint`が呼ばれる） | `SetCurrentPosition(start ms)` で**start位置から最後まで再生**（実測: 44100HzStereoの`176400`=1秒目から） | **最も大きい差分**。旧は実質使えない挙動のため、新は素直な動作に統一（意図的な仕様変更） |
| 2 | **再生サンプルレート** | 出力バッファが **22050Hz**（InitAudioデフォルト） | **元WAVのフォーマットを保持**（例: 44100Hz） | ピッチは同一だが、帯域/high-cutが僅かに異なる。WAVが22050Hz以外の場合に差が出る |
| 3 | **音量の適用先** | `AudioPath::SetVolume`（ボリュームカーブ＝CC7域） | `IDirectSoundBuffer::SetVolume` にdB値を直接設定 | 静的音量は**新の方が誤差0**（旧はCC7往復で最大約0.5dBの丸め）。フェードは旧の曲線（CC7域ランプ）に合わせてあるため同一 |
| 4 | **24bit / float32 WAV** | 未検証（DMの変換方式は未計測） | 16bit PCMへ変換してDirectSoundで再生 | 32bit floatはDirectSoundが受け付けないため変換が必要 |

### 2.2 一致を確認済み

| 項目 | 実測結果 |
|---|---|
| `set_volume` の値 | 旧: `AudioPath::SetVolume(-942)` → 新: `SetVolume(-942)`。`set_volume(200)` → 共に `-2071` ✓（dB↔マスター音量の往復誤差も修正済み） |
| フェードの演算領域 | **CC7(振幅)スケール**の線形ランプ（dBスケールではない）。実測: `-938→-985→-1059→…→-9600`（= `DXRUBY_CC7ToDB[74]→DXRUBY_CC7ToDB[72]→…→DXRUBY_CC7ToDB[0]`） |
| `pan=` | 旧: `dsb->SetPan(pan)`／`GetPan` → 新: 同じバッファに対する `SetPan`/`GetPan`。`pan=-5000`→`pan`戻り値`-5000` ✓ |
| `frequency=` | 旧: `dsb->SetFrequency(freq)`（出力の再サンプリング） → 新: 同じ方式。`frequency=44100`→戻り値`44100` ✓ |
| `loop_start=` / `loop_end=` | 両者とも例外 `Can not be set to Wav data - Sound_loopStart=` / `- Sound_loopEnd=` ✓ |
| `loop_count=` | 旧: `SetRepeats(n)`＝合計n+1回 → 新: 合計n+1回 ✓（修正済み）。無指定時は両者とも1回 |
| `loop_count=-1` | 旧: `SetRepeats(0xFFFFFFFF)`（無限）→ 新: `DSBPLAY_LOOPING`（シームレス無限ループ） ✓ |
| 初期音量 | 旧・新とも `-942`（= `230*9600/255-9600`） ✓。デバイス生成時の初期値は0（最大音量）とし、減衰はSound.cが読み込み時に`SetVolume(-942)`で適用（DMのAudioPath初期値と同じ） |

---

## 3. その他の差異

| 項目 | 旧実装 | 新実装 |
|---|---|---|
| WAVのフォーマット | 24bit/8bit/floatの変換方式はDM内部控制 | 自作RIFF/WAVEパーサで自前変換（DirectSoundが受け付けない形式のみ16bitへ変換） |
| WAVのパース | DMローダ | 自作RIFF/WAVEパーサ（`fmt`/`data`チャンク、`WAVE_FORMAT_EXTENSIBLE`対応） |
| MIDIファイルローダ | `dmime`ローダ | 自作SMFパーサ |
| 複数MIDI同時再生 | Performance＋Sound毎のAudioPath | ポート共有＋チャンネルグループ割当（`MIDI_GROUPS`個） |
| 破棄時 | 個別NoteOff | `AllNotesOff`（CC120/CC123） |

---

## 4. 既知の未解決事項

- `loop_start` の境界（1.2節 #3）: 旧実装の反復規則が実測で確定し、新実装は
  送出順が完全一致するまで修正済み（6.3節・6.6節参照）。
- 旧にあった「stop後も先行バッファ約130ms分が発音され続ける」挙動は
  未再現（新実装はリアルタイム生成のため、この違いは残る、1.1節 #7）。
- 旧実装の24bit/float WAVの変換・再生結果は未計測（新は16bit変換で再生）。
- 旧実装の `start=`（wav）が「ほぼ無音」になる点の挙動は、
  旧ソース（`old_sound.c`）から推定したものであり、
  聴取による確認は未実施。

---

## 6. 追加突合せ結果（2026-10-21）

以下を実際にフック突合せした。

### 6.1 複数MIDI同時再生（1.1節 #5, #6、C-14）

- 旧: 同じ曲を5本同時play → チャンネルグループ 5,9,13,17,21 に分配され、
  5本とも全部イベントが送出（旧のgroups=4宣言を超えても音は尽きない）。
- 新: グループ 1〜5 に分配され同数のイベント。
- `pan=`/`frequency=`: 旧はSound毎のAudioPathバッファ（2本で2バッファ）、
  新はポート共有の1バッファに全SetPan/SetFrequencyが集中する
  （フックでthisポインタを確認）→ **1.1節 #5, #6 の差異を実測で確定**。

### 6.2 停止時（1.1節 #7）

- 旧: 発音中のノートへの個別NoteOff送出＋stop後も先行バッファ（約130ms分）が
  発音を続けた後、残りノートへのNoteOffで減衰停止。
- 新: 全chへCC120/CC123を即時送出し、先行バッファ含めて即カット
  （減衰なしの急停止）。詳細は1.1節 #7 に記載。

### 6.3 ループ境界の精密試験（1.1節 #3）

専用MIDI（division=384、イベント配置: A=file tick 100 / C=320 / B=400 /
D=484 / E=486 / F=700。ループは API値 start=640・end=968（= file tick
320・484、C と D がそれぞれ境界ちょうどに位置）loop_count=2）で新旧を突合せした:

- 旧の送出順: `A,C,B,D,E | B,D,E | B,D,E | F`
- 新の送出順（DM実測に合わせて修正済み）: 同じ `A,C,B,D,E | B,D,E | B,D,E | F`
  なお修正前の新実装は `A,B,C | (C,B)×3 | D,E,F` だった。

実測で確定した旧DirectMusic実装の量子化規則:
1. 繰り返し開始点は **loop_start より後の最初のイベント**
   （loop_start ちょうどのイベントは反復しない）
2. 繰り返し終端は **loop_end 時刻 + 約3.9ms（DMのミックスパケット）までの
   イベント**（loop_endちょうどのイベントと直後のイベントが反復される）
3. 反復の周期は「繰り返し開始点の量子化位置（loop_start以下の最後の
   イベント）.. loop_end 時刻」の期間
4. 最初のパスは先頭から（2）の終端まで通し、その後（1）〜（3）で
   loop_count 回反復し、その後残りを（終端より後のイベントのみ）再生

新実装はこの規則（midi.c の MidiWorker custom branch）に合わせ、
境界位置・スピル込みの 4 段構成で相同な順序になった。

### 6.4 wav複合操作（B-12）

- 旧: フェードは `AudioPath::SetVolume(減衰, duration)` の1回の呼び出しで
  DM内部のランプに任せる。pan/frequencyはAudioPath内部のラッパ経由で
  dsbレベルから観測不能だった。
- 新: 10ms刻みのdsb SetVolume連打（CC7域ランプ）＋dsb SetPan/SetFrequency。
  フェード中のpan/停止の到達時刻が新旧で異なる（旧はDM内クロック、新は
  ワーカー周期）。フェード曲線そのもの（CC7域）は2.2節のとおり一致済み。

### 6.5 wav無限ループ・再play（C-15、C-16）✓ 一致

- `loop_count=-1`: 旧 `SetRepeats(0xFFFFFFFF)`、新 `DSBPLAY_LOOPING`。等価。
- 再生中の再play: 両者とも先頭から再スタート。
  （旧: StopなしでPlaySegmentEx再送、新: Stop→SetCurrentPosition(0)→Play）。

### 6.6 ①②③の実装と比較検証（2026-10-21 追記）

| 変更 | 内容 | 検証結果 |
|---|---|---|
| ① 停止時に発音中ノートへ個別NoteOff | ワーカーが発音中ノート(キーシフト/チャンネル10の差異込みで正規化)を記録し、stop時にNoteOffを送る。CC120/CC123は廃止（CC120/123はシーク復帰のチャンネルリセットのみ） | town.mid＋stop で旧と同じノート集合{52,59,63,28}へのNoteOff系列が得られた。旧にあった「先行バッファ約130ms分の継続発音」は未再現 |
| ② CHORUS有効化 | ポート初期化の`dwEffectFlags`に`DMUS_EFFECT_CHORUS`を追加（旧InitAudioデフォルトと同じ要求） | Synth_Open の granted は旧新とも `effects=0x1`(REVERBのみ)。dmsynthはコーラス未実装のため**要求差は聴感に影響しない**ことが実測で確定 |
| ③ pan/frequencyをSound単位へ | ポートを共有から**Sound毎に専用に変更**(各SoundがDMポート+DirectSoundバッファを持つ)。pan/frequencyの操作先も専用dsbへ | フックで2つのMIDIに対して2つのdsbが生成され、`s1.pan=`/`s1.frequency=`のSetPan/SetFrequencyが s1 のバッファだけに適用されることを確認。旧のAudioPath分離と同じ作用範囲 |
| （追加）ループ反復の旧準拠化 | 旧に合わせ「phase1: 先頭→(loop_end+3.9ms)通し」「phase2: (loop_start直後のイベント..終端スピル)をloop_count回」「phase3: 終端以降を1回」に再実装 | 専用MIDI+loop_count=2 で旧送出順と**完全一致**。32bit(Ruby33)・64bit(Ruby40)で同結果 |
| （環境）32bit検証 | `C:\Ruby33\bin\ruby.exe`(i386-mingw32) 用に mingw32(i686) でビルド | test_midi/wav/mem/byteorder 全パス、stop>NoteOff系列・ループ順序とも64bitと同一 |

### 6.7 演奏終端到達時の挙動（孤立NoteOn・残響）

`stop()`を呼ばずに自然終了させる専用MIDIで新旧を比較した
(loop_count=0＝1周のみ、division=384、120bpm)。

| ケース | 構成 | 旧の実測 | 新（修正後）の実測 |
|---|---|---|---|
| A: 孤立NoteOnのみ | NoteOn 60(tick 0) / EoF(tick 100) | `NoteOn 60 (0ms)` → **`NoteOff 60 (0ms, 自動生成)`** | 同左 ✓ |
| B: ペアあり＋孤立 | NoteOn 60 / NoteOff 60(50) / NoteOn 64(100,孤立) / EoF(200) | `NoteOn 60(0)` → `NoteOff 60(64ms)` → `NoteOn 64(130ms)` → **`NoteOff 64(130ms, 自動生成)`** | 同左 ✓ |
| C: ペアありのみ | NoteOn 60 / NoteOff 60(50) / EoF(200) | `NoteOn 60(0)` → `NoteOff 60(64ms)` | 同左 ✓ |
| D: 複合 | NoteOn 60+NoteOff / NoteOn 64(孤立) / NoteOn 67+NoteOff(終端跨ぎ) / EoF | `ON60(0) → OFF60(399) → ON64(520) → OFF64(521,自動) → ON67(572) → OFF67(585)`、終端後に追加送信なし | 同左 ✓（新は520ms、旧は521msでNoteOff、0.06msの送出遅延差のみ） |

結論:
- **key-onのままのノート**: 旧はNoteOnの直後にNoteOffを自動生成するため
  **鳴り続かない**。新も同一に修正済み（パース時に孤立NoteOnを検出し、
  送信時に直後にNoteOff）。修正前は新が鳴り続いていた。- **key-off済みで残響が残るノート**: 旧も新も **追加のNoteOffを送らない**
  （残響はそのまま鳴り切って減衰する）。終端到達時に旧は
  何も追加送信しない（≒1.1節#7の「停止時」と区別）。
- **演奏終端到達時**: 新旧とも「孤立NoteOnは既に切られている」「ペア
  済みのノートは終端で自然に減衰」で**一致**。

### 6.8 実施できなかった突合せと理由

| 項目 | 理由 |
|---|---|
| 旧の24bit/32bit float WAVの変換・再生 | 旧のDMローダは内部実装でPCMを観測する手段がない（dmsynthのRenderフックは32bit不可） |
| MIDI出力PCMの全曲突合せ（1.1節 #2の音への影響） | 同上。イベント系列一致＋単音PCM測定での妥当化まで |
| Chorusエフェクトの有無の聴感 | 旧REVERB+CHORUS / 新REVERBのパラメータ差は既知だが、旧側のPCM観測手段がなく聴感突合せ不可。新に`DMUS_EFFECT_CHORUS`を足すかどうかは**要判断** |
| 旧のwav `start=`の聴取確認 | 聴取作業が必要で自動化不能 |
| 64bit以外の環境（32bit Ruby 3.4相当） | 32bit検証は `C:\Ruby33`(Ruby 3.3.10 i386-mingw32) で実施済み（6.6節）。他バージョンの32bit環境は未実施 |
| wavの高帯域（22050→44100）の音質差 | フォーマット差は計測済みだが聴感突合せは自動化不能 |
| `loop_count=-1`の数時間規模の安定性 | 時間がかかるため未実施 |

---

## 5. 変更履歴

| 日時 | 内容 |
|---|---|
| 2026-10-21 | 初版作成（DirectMusic廃止リファクタの検証結果を反映） |
| 2026-10-21 | 追加突合せを実施（6節参照）。確定した新差分: ①pan/frequency作用範囲（Sound毎→ポート共有）はフックで実測確定 ②停止時は旧=個別NoteOff+先行130ms流れ継続、新=CC120/123即カット ③ループ境界は旧がD,Eを含んだパス構造で反復位相が異なる（1tick差定義を修正） ④wav無限ループ・再playは等価 ⑤複数MIDI同時5本は両者とも制限なく動作 |
| 2026-10-21 | ①stop時の発音中ノート個別NoteOff化 ②ポートへCHORUS要求追加 ③pan/frequencyをSound毎の専用ポート/dsbへ変更、ループ反復規則を旧実測準拠に再実装。フック比較で送出系列が旧と一致（6.6節）。32bitでも同結果を確認 |
| 2026-10-21 | 孤立NoteOn(対応するNoteOffが無いNoteOn)の扱いを旧実装に合わせる。旧dmime実装はNoteOnの直後にNoteOffを自動生成して送出しており(音は鳴り続けない)、新もパース時に孤立を検出して直後にNoteOffを送出。演奏終端到達時の挙動(孤立NoteOn/残響ノート/終端での追加送信なし)も旧と一致(6.7節)。32bit/64bitともフック比較で確認 |
