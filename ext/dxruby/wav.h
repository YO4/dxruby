#ifndef DXRUBY_WAV_H
#define DXRUBY_WAV_H

#include "sounddev.h"

/* WAVをDirectMusicを経由せずDirectSoundで演奏するデバイス */

DXRubySoundDev *DXRubyWav_CreateFromMemory(const unsigned char *data, size_t length, const char **error);
DXRubySoundDev *DXRubyWav_CreateFromFile(const char *filename, const char **error);

#endif
