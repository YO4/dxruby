#ifndef DXRUBY_MIDI_H
#define DXRUBY_MIDI_H

#include <stddef.h>

#include "sounddev.h"

/* SMF(MIDI)を自前でパースし、DirectMusicポートへ直接送信して演奏するデバイス。
 * オブジェクトはsounddev.hのDXRubySoundDevインターフェースで操作する。 */

typedef struct DXRubyMidi DXRubyMidi;

DXRubySoundDev *DXRubyMidi_CreateFromMemory(const unsigned char *data, size_t length, const char **error);
DXRubySoundDev *DXRubyMidi_CreateFromFile(const char *filename, const char **error);

#endif
