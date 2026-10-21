#define WINVER 0x0500                                  /* バージョン定義 Windows2000以上 */
#define DIRECTSOUND_VERSION 0x0900
#define _WIN32_WINNT WINVER

#include "ruby.h"
#ifndef RUBY_ST_H
#include "st.h"
#endif

#include <dsound.h>
#include <string.h>

#define DXRUBY_EXTERN 1
#include "dxruby.h"
#include "sound.h"
#include "sounddev.h"
#include "midi.h"
#include "wav.h"

#ifndef DS3DALG_DEFAULT
GUID DS3DALG_DEFAULT = {0};
#endif

#define WAVE_RECT 0
#define WAVE_SIN 1
#define WAVE_SAW 2
#define WAVE_TRI 3

static VALUE cSound;        /* サウンドクラス       */
static VALUE cSoundEffect;  /* 生成効果音クラス     */

static LPDIRECTSOUND8           g_pDSound = NULL;               /* DirectSoundインターフェイス */
static int g_iRefDS = 0; /* DirectSoundの参照カウント */

/* Soundオブジェクト */
struct DXRubySound {
    DXRubySoundDev           *dev;               /* 再生デバイス(midi/wav共通) */
    int start;
    int loopstart;
    int loopend;
    int loopcount;
    int midwavflag; /* midは0、wavは1 */
    VALUE vbuffer;
};

static int Sound_is_disposed( struct DXRubySound *sound )
{
    return sound->dev == NULL;
}

static int Sound_is_midi_filename( const char *filename )
{
    size_t length = strlen( filename );
    if( length < 4 ) return 0;
    if( _stricmp( filename + length - 4, ".mid" ) == 0 ) return 1;
    return length >= 5 && _stricmp( filename + length - 5, ".midi" ) == 0;
}

/* SoundEffectオブジェクト */
struct DXRubySoundEffect {
    LPDIRECTSOUNDBUFFER pDSBuffer;    /* バッファ         */
};



/*********************************************************************
 * Soundクラス
 *
 * midi.c(SMF)とwav.c(WAV)のDXRubySoundDev再生デバイスで音を鳴らす。
 *********************************************************************/

/*--------------------------------------------------------------------
   参照されなくなったときにGCから呼ばれる関数
 ---------------------------------------------------------------------*/
static void Sound_free( struct DXRubySound *sound )
{
    /* 再生デバイスを開放 */
    if( sound->dev )
    {
        sound->dev->lpVtbl->Destroy( sound->dev );
        sound->dev = NULL;
    }
}

static void Sound_mark( struct DXRubySound *sound )
{
    rb_gc_mark( sound->vbuffer );
}

static void Sound_release( struct DXRubySound *sound )
{
    if ( sound->dev )
    {
        Sound_free( sound );
    }
    free( sound );
    sound = NULL;

    g_iRefAll--;
    if( g_iRefAll == 0 )
    {
        CoUninitialize();
    }
}

#ifdef DXRUBY_USE_TYPEDDATA
const rb_data_type_t Sound_data_type = {
    "Sound",
    {
    Sound_mark,
    Sound_release,
    0,
    },
    NULL, NULL
};
#endif

/*--------------------------------------------------------------------
   Soundクラスのdispose。
 ---------------------------------------------------------------------*/
static VALUE Sound_dispose( VALUE self )
{
    struct DXRubySound *sound = DXRUBY_GET_STRUCT( Sound, self );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    Sound_free( sound );
    return self;
}

/*--------------------------------------------------------------------
   Soundクラスのdisposed?。
 ---------------------------------------------------------------------*/
static VALUE Sound_check_disposed( VALUE self )
{
    if( Sound_is_disposed( DXRUBY_GET_STRUCT( Sound, self ) ) )
    {
        return Qtrue;
    }

    return Qfalse;
}

/*--------------------------------------------------------------------
   Soundクラスのallocate。メモリを確保する為にinitialize前に呼ばれる。
 ---------------------------------------------------------------------*/
static VALUE Sound_allocate( VALUE klass )
{
    VALUE obj;
    struct DXRubySound *sound;

    /* DXRubyImageのメモリ取得＆Imageオブジェクト生成 */
    sound = malloc(sizeof(struct DXRubySound));
    if( sound == NULL ) rb_raise( eDXRubyError, "Out of memory - Sound_allocate" );
#ifdef DXRUBY_USE_TYPEDDATA
    obj = TypedData_Wrap_Struct( klass, &Sound_data_type, sound );
#else
    obj = Data_Wrap_Struct(klass, 0, Sound_release, sound);
#endif
    /* とりあえずサウンドデバイスはNULLにしておく */
    sound->dev = NULL;
    sound->vbuffer = Qnil;

    return obj;
}


/*--------------------------------------------------------------------
   Soundクラスのload_from_memory。メモリからロードする。
  ---------------------------------------------------------------------*/
static VALUE Sound_load_from_memory( VALUE klass, VALUE vstr, VALUE vtype )
{
    HRESULT hr;
    VALUE obj;
    struct DXRubySound *sound;
    const char *error = NULL;

    g_iRefAll++;

    Check_Type( vstr, T_STRING );

    /* サウンドオブジェクト取得 */
    obj = Sound_allocate( klass );
    sound = DXRUBY_GET_STRUCT( Sound, obj );
    sound->vbuffer = vstr;
    sound->start = 0;
    sound->loopstart = 0;
    sound->loopend = 0;

    if( NUM2INT( vtype ) == 0 )    /* MIDIの場合 */
    {
        sound->dev = DXRubyMidi_CreateFromMemory((const unsigned char *)RSTRING_PTR(vstr),
                                                 (size_t)RSTRING_LEN(vstr), &error);
        if( !sound->dev )
            rb_raise( eDXRubyError, "MIDI load error - %s", error ? error : "unknown error" );
        sound->loopcount = -1;
        sound->midwavflag = 0;
    }
    else                           /* wavの場合 */
    {
        sound->dev = DXRubyWav_CreateFromMemory((const unsigned char *)RSTRING_PTR(vstr),
                                                (size_t)RSTRING_LEN(vstr), &error);
        if( !sound->dev )
            rb_raise( eDXRubyError, "Load error - LoadObjectFromFile" );
        sound->loopcount = 0;
        sound->midwavflag = 1;
    }

    /* ループ回数設定 */
    sound->dev->lpVtbl->SetRepeats( sound->dev, sound->loopcount );

    /* 音量設定 */
    hr = sound->dev->lpVtbl->SetVolume( sound->dev, 230 * 9600 / 255 - 9600, 0 );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Set volume failed - SetVolume" );
    }

    return obj;
}


/*--------------------------------------------------------------------
   Soundクラスのload_from_memory。ファイルをメモリからロードする。
 ---------------------------------------------------------------------*/
/*--------------------------------------------------------------------
   Soundクラスのinitialize。ファイルをロードする。
  ---------------------------------------------------------------------*/
static VALUE Sound_initialize( VALUE obj, VALUE vfilename )
{
    struct DXRubySound *sound;
    VALUE vsjisstr;

    g_iRefAll++;

	Check_Type(vfilename, T_STRING);

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( rb_enc_get_index( vfilename ) != 0 )
        vsjisstr = rb_str_export_to_enc( vfilename, g_enc_sys );
    else
        vsjisstr = vfilename;

    if( sound->dev )
    {
        Sound_free( sound );
        g_iRefAll--;
    }

    if( Sound_is_midi_filename( RSTRING_PTR( vsjisstr ) ) )
    {
        const char *error = NULL;
        sound->dev = DXRubyMidi_CreateFromFile( RSTRING_PTR( vsjisstr ), &error );
        if( !sound->dev )
            rb_raise( eDXRubyError, "MIDI load error - %s", error ? error : "unknown error" );
        sound->vbuffer = Qnil;
        sound->start = sound->loopstart = sound->loopend = 0;
        sound->loopcount = -1;
        sound->midwavflag = 0;
    }
    else
    {
        const char *error = NULL;
        sound->dev = DXRubyWav_CreateFromFile( RSTRING_PTR( vsjisstr ), &error );
        if( !sound->dev )
            rb_raise( eDXRubyError, "Load error - LoadObjectFromFile" );
        sound->vbuffer = Qnil;
        sound->start = sound->loopstart = sound->loopend = 0;
        sound->loopcount = 0;
        sound->midwavflag = 1;
    }

    /* ループ回数設定 */
    sound->dev->lpVtbl->SetRepeats( sound->dev, sound->loopcount );

    /* 音量設定 */
    {
        HRESULT hr = sound->dev->lpVtbl->SetVolume( sound->dev, 230 * 9600 / 255 - 9600, 0 );
        if( FAILED( hr ) )
        {
            rb_raise( eDXRubyError, "Set volume failed - SetVolume" );
        }
    }

    return obj;
}

/*--------------------------------------------------------------------
   開始位置を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setStart( VALUE obj, VALUE vstart )
{
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    sound->start = NUM2INT( vstart );

    /* セグメント再生スタート位置設定 */
    sound->dev->lpVtbl->SetStartPoint( sound->dev, sound->start * DMUS_PPQ / 768 );

    return obj;
}


/*--------------------------------------------------------------------
   ループ開始位置を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setLoopStart( VALUE obj, VALUE vloopstart )
{
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    sound->loopstart = NUM2INT( vloopstart );

    if( sound->midwavflag == 1 )
    {
        rb_raise( eDXRubyError, "Can not be set to Wav data - Sound_loopStart=" );
    }

    if( sound->loopstart <= sound->loopend )
    {
        /* ループ範囲設定 */
        sound->dev->lpVtbl->SetLoopPoints( sound->dev, sound->loopstart * DMUS_PPQ / 768
                                                          , sound->loopend * DMUS_PPQ / 768 );
    }
    else
    {
        /* ループ範囲設定 */
        sound->dev->lpVtbl->SetLoopPoints( sound->dev, 0, 0 );
    }

    return obj;
}


/*--------------------------------------------------------------------
   ループ終了位置を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setLoopEnd( VALUE obj, VALUE vloopend )
{
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    sound->loopend = NUM2INT( vloopend );

    if( sound->midwavflag == 1 )
    {
        rb_raise( eDXRubyError, "Can not be set to Wav data - Sound_loopEnd=" );
    }

    if( sound->loopstart <= sound->loopend )
    {
        /* ループ範囲設定 */
        sound->dev->lpVtbl->SetLoopPoints( sound->dev, sound->loopstart * DMUS_PPQ / 768
                                                          , sound->loopend * DMUS_PPQ / 768 );
    }
    else
    {
        /* ループ範囲設定 */
        sound->dev->lpVtbl->SetLoopPoints( sound->dev, 0, 0 );
    }


    return obj;
}


/*--------------------------------------------------------------------
   ループ回数を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setLoopCount( VALUE obj, VALUE vloopcount )
{
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    sound->loopcount = NUM2INT( vloopcount );

    /* ループ回数設定 */
    sound->dev->lpVtbl->SetRepeats( sound->dev, sound->loopcount );

    return obj;
}


/*--------------------------------------------------------------------
   音量を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setVolume( int argc, VALUE *argv, VALUE obj )
{
    HRESULT hr;
    struct DXRubySound *sound;
    VALUE vvolume, vtime;
    int volume, time;

    rb_scan_args( argc, argv, "11", &vvolume, &vtime );

    time = vtime == Qnil ? 0 : NUM2INT( vtime );
    volume = NUM2INT( vvolume ) > 255 ? 255 : NUM2INT( vvolume );
    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    if( volume < 0 ) rb_raise( eDXRubyError, "Set volume error - SetVolume" );

    /* 音量設定 */
    hr = sound->dev->lpVtbl->SetVolume( sound->dev, volume * 9600 / 255 - 9600, time );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Set volume error - SetVolume" );
    }

    return obj;
}


/*--------------------------------------------------------------------
   パンを取得する
 ---------------------------------------------------------------------*/
static VALUE Sound_getPan( VALUE self )
{
    HRESULT hr;
    struct DXRubySound *sound;
    int result;

    sound = DXRUBY_GET_STRUCT( Sound, self );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );

    /* パン取得 */
    hr = sound->dev->lpVtbl->GetPan( sound->dev, &result );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "pan get error - GetPan" );
    }

    return INT2NUM( result );
}


/*--------------------------------------------------------------------
   パンを設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setPan( VALUE self, VALUE vpan )
{
    HRESULT hr;
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, self );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );
    if( NUM2INT( vpan ) < -10000 || NUM2INT( vpan ) > 10000 )
        rb_raise( eDXRubyError, "pan setting error - SetPan" );

    /* パン設定 */
    hr = sound->dev->lpVtbl->SetPan( sound->dev, NUM2INT( vpan ) );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "pan setting error - SetPan" );
    }

    return self;
}


/*--------------------------------------------------------------------
   周波数を取得する
 ---------------------------------------------------------------------*/
static VALUE Sound_getFrequency( VALUE self )
{
    HRESULT hr;
    struct DXRubySound *sound;
    int result;

    sound = DXRUBY_GET_STRUCT( Sound, self );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );

    /* 周波数取得 */
    hr = sound->dev->lpVtbl->GetFrequency( sound->dev, &result );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "frequency get error - getFrequency" );
    }

    return UINT2NUM( (unsigned int)result );
}


/*--------------------------------------------------------------------
   周波数を設定する
 ---------------------------------------------------------------------*/
static VALUE Sound_setFrequency( VALUE self, VALUE vfrequency )
{
    HRESULT hr;
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, self );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );

    /* 周波数設定(範囲外はデバイスがエラーを返す) */
    hr = sound->dev->lpVtbl->SetFrequency( sound->dev, NUM2INT( vfrequency ) );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "frequency setting error - setFrequency" );
    }

    return self;
}


/*--------------------------------------------------------------------
   音を鳴らす
 ---------------------------------------------------------------------*/
static VALUE Sound_play( VALUE obj )
{
    HRESULT hr;
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );

    /* 再生 */
    hr = sound->dev->lpVtbl->PlaySegmentEx( sound->dev );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Sound play failed - PlaySegmentEx" );
    }

    return obj;
}


/*--------------------------------------------------------------------
   音を止める
 ---------------------------------------------------------------------*/
static VALUE Sound_stop( VALUE obj )
{
    HRESULT hr;
    struct DXRubySound *sound;

    sound = DXRUBY_GET_STRUCT( Sound, obj );
    if( Sound_is_disposed( sound ) ) rb_raise( eDXRubyError, "disposed object" );

    /* 停止 */
    hr = sound->dev->lpVtbl->StopEx( sound->dev );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Sound stop failed - StopEx" );
    }

    return obj;

}


/*********************************************************************
 * SoundEffectクラス
 *
 * DirectSoundを使用して音を鳴らす。
 * とりあえず音を出そうと頑張っている。
 *********************************************************************/

/*--------------------------------------------------------------------
   参照されなくなったときにGCから呼ばれる関数
 ---------------------------------------------------------------------*/
static void SoundEffect_free( struct DXRubySoundEffect *soundeffect )
{
    /* サウンドバッファを開放 */
    RELEASE( soundeffect->pDSBuffer );

    g_iRefDS--;

    if( g_iRefDS == 0 )
    {
        RELEASE( g_pDSound );
    }

}

static void SoundEffect_release( struct DXRubySoundEffect *soundeffect )
{
    if( soundeffect->pDSBuffer )
    {
        SoundEffect_free( soundeffect );
    }
    free( soundeffect );
    soundeffect = NULL;

    g_iRefAll--;
    if( g_iRefAll == 0 )
    {
        CoUninitialize();
    }
}

#ifdef DXRUBY_USE_TYPEDDATA
const rb_data_type_t SoundEffect_data_type = {
    "SoundEffect",
    {
    0,
    SoundEffect_release,
    0,
    },
    NULL, NULL
};
#endif

/*--------------------------------------------------------------------
   SoundEffectクラスのdispose。
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_dispose( VALUE self )
{
    struct DXRubySoundEffect *soundeffect = DXRUBY_GET_STRUCT( SoundEffect, self );
    DXRUBY_CHECK_DISPOSE( soundeffect, pDSBuffer );
    SoundEffect_free( soundeffect );
    return self;
}

/*--------------------------------------------------------------------
   SoundEffectクラスのdisposed?。
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_check_disposed( VALUE self )
{
    if( DXRUBY_GET_STRUCT( SoundEffect, self )->pDSBuffer == NULL )
    {
        return Qtrue;
    }

    return Qfalse;
}

/*--------------------------------------------------------------------
   SoundEffectクラスのallocate。メモリを確保する為にinitialize前に呼ばれる。
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_allocate( VALUE klass )
{
    VALUE obj;
    struct DXRubySoundEffect *soundeffect;

    /* DXRubySoundEffectのメモリ取得＆SoundEffectオブジェクト生成 */
    soundeffect = malloc(sizeof(struct DXRubySoundEffect));
    if( soundeffect == NULL ) rb_raise( eDXRubyError, "Out of memory - SoundEffect_allocate" );
#ifdef DXRUBY_USE_TYPEDDATA
    obj = TypedData_Wrap_Struct( klass, &SoundEffect_data_type, soundeffect );
#else
    obj = Data_Wrap_Struct(klass, 0, SoundEffect_release, soundeffect);
#endif

    /* とりあえずサウンドオブジェクトはNULLにしておく */
    soundeffect->pDSBuffer = NULL;

    return obj;
}


static short calcwave(int type, double vol, double count, double p, double duty)
{
	switch( type )
	{
	case 1: /* サイン波 */
		return (short)((sin( (3.141592653589793115997963468544185161590576171875f * 2) * (double)count / (double)p )) * (double)vol * 128);
		break;
	case 2: /* ノコギリ波 */
		return (short)(((double)count / (double)p - 0.5) * (double)vol * 256);
		break;
	case 3: /* 三角波 */
		if( count < p / 4 )			/* 1/4 */
		{
			return (short)((double)count / ((double)p / 4) * (double)vol * 128);
		}
		else if( count < p / 2 )		/* 2/4 */
		{
			return (short)(((double)p / 2 - (double)count) / ((double)p / 4) * (double)vol * 128);
		}
		else if( count < p * 3 / 4 )	/* 3/4 */
		{
			return (short)(-((double)count - (double)p / 2)/ ((double)p / 4) * (double)vol * 128);
		}
		else											/* 最後 */
		{
			return (short)(-((double)p - (double)count) / ((double)p / 4) * (double)vol * 128);
		}
		break;
	case 0: /* 矩形波 */
	default: /* デフォルト */
		if( count < p * duty )	/* 前半 */
		{
			return (short)(vol * 128);
		}
		else									/* 後半 */
		{
		    return (short)(-vol * 128);
		}
		break;
	}
}


/*--------------------------------------------------------------------
   SoundEffectクラスのinitialize。波形を生成する。
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_initialize( int argc, VALUE *argv, VALUE obj )
{
    HRESULT hr;
    struct DXRubySoundEffect *soundeffect;
    DSBUFFERDESC desc;
    WAVEFORMATEX pcmwf;
    int i;
    short *pointer, *pointer2;
    DWORD size, size2;
    VALUE vf;
    double count, duty = 0.5;
    double vol;
    double f;
    VALUE vsize, vtype, vresolution;
    int type, resolution;

    g_iRefAll++;

    rb_scan_args( argc, argv, "12", &vsize, &vtype, &vresolution );

    type       = vtype       == Qnil ? 0    : NUM2INT( vtype );
    resolution = vresolution == Qnil ? 1000 : (NUM2INT( vresolution ) > 44100 ? 44100 : NUM2INT( vresolution ));

    /* DirectSoundオブジェクトの作成 */
    if( g_iRefDS == 0 )
    {
        hr = DirectSoundCreate8( &DSDEVID_DefaultPlayback, &g_pDSound, NULL );
        if( FAILED( hr ) )
        {
            rb_raise( eDXRubyError, "DirectSound initialize error - DirectSoundCreate8" );
        }

        hr = g_pDSound->lpVtbl->SetCooperativeLevel( g_pDSound, g_hWnd, DSSCL_PRIORITY );
        if( FAILED( hr ) )
        {
            rb_raise( eDXRubyError, "DirectSound initialize error - SetCooperativeLevel" );
        }
    }
    g_iRefDS++;

    /* サウンドオブジェクト作成 */
    soundeffect = DXRUBY_GET_STRUCT( SoundEffect, obj );
    if( soundeffect->pDSBuffer )
    {
        g_iRefDS++;
        SoundEffect_free( soundeffect );
        g_iRefDS--;
        g_iRefAll++;
    }

    /* サウンドバッファ作成 */
    pcmwf.wFormatTag = WAVE_FORMAT_PCM;
    pcmwf.nChannels = 1;
    pcmwf.nSamplesPerSec = 44100;
    pcmwf.wBitsPerSample = 16;
    pcmwf.nBlockAlign = pcmwf.nChannels * pcmwf.wBitsPerSample / 8;
    pcmwf.nAvgBytesPerSec = pcmwf.nSamplesPerSec * pcmwf.nBlockAlign;
    pcmwf.cbSize = 0;

    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GLOBALFOCUS;
#ifdef DXRUBY15
    if( TYPE( vsize ) == T_ARRAY )
    {
        desc.dwBufferBytes = RARRAY_LEN( vsize ) * (pcmwf.nChannels * pcmwf.wBitsPerSample / 8);
    }
    else
    {
#endif
        desc.dwBufferBytes = pcmwf.nAvgBytesPerSec / 100 * NUM2INT(vsize) / 10;
#ifdef DXRUBY15
    }
#endif
    desc.dwReserved = 0;
    desc.lpwfxFormat = &pcmwf;
    desc.guid3DAlgorithm = DS3DALG_DEFAULT;

    hr = g_pDSound->lpVtbl->CreateSoundBuffer( g_pDSound, &desc, &soundeffect->pDSBuffer, NULL );

    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Failure to create the SoundBuffer - CreateSoundBuffer" );
    }

    /* ロック */
    hr = soundeffect->pDSBuffer->lpVtbl->Lock( soundeffect->pDSBuffer, 0, 0, (LPVOID *)&pointer, &size, (LPVOID *)&pointer2, &size2, DSBLOCK_ENTIREBUFFER );
    if( FAILED( hr ) || size2 != 0 )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    if( TYPE( vsize ) == T_ARRAY )
    {
        /* 音バッファ生成 */
        for( i = 0; i < desc.dwBufferBytes / (pcmwf.wBitsPerSample / 8); i++ )
        {
            double tmp = NUM2DBL( RARRAY_AREF( vsize, i ) );
            if( tmp < 0.0 )
            {
                if( tmp < -1.0 ) tmp = -1.0;
                pointer[i] = (short)(tmp * 32768.0);
            }
            else
            {
                if( tmp > 1.0 ) tmp = 1.0;
                pointer[i] = (short)(tmp * 32767.0);
            }
        }
    }
    else
    {
        /* 音バッファ初期化 */
        for( i = 0; i < desc.dwBufferBytes / (pcmwf.wBitsPerSample / 8); i++ )
        {
            pointer[i] = 0;
        }

        count = 0;

        /* 波形生成 */
        for( i = 0; i < desc.dwBufferBytes / (pcmwf.wBitsPerSample / 8); i++ )
        {
            /* 指定時間単位でブロックを呼び出す */
            if ( i % (pcmwf.nSamplesPerSec / resolution) == 0 )
            {
                vf = rb_yield( obj );
                if( TYPE( vf ) != T_ARRAY )
                {
                    soundeffect->pDSBuffer->lpVtbl->Unlock( soundeffect->pDSBuffer, pointer, size, pointer2, size2 );
                    rb_raise(rb_eTypeError, "invalid value - SoundEffect_initialize");
                    break;
                }
                f = NUM2DBL( rb_ary_entry(vf, 0) );
                vol = NUM2DBL(rb_ary_entry(vf, 1));
                if( RARRAY_LEN( vf ) > 2 )
                {
                    duty = NUM2DBL(rb_ary_entry(vf, 2));
                }
                /* 最大/最低周波数と最大ボリュームの制限 */
                f = f > pcmwf.nSamplesPerSec / 2.0f ? pcmwf.nSamplesPerSec / 2.0f : f;
                f = f < 20 ? 20 : f;
                vol = vol > 255 ? 255 : vol;
                vol = vol < 0 ? 0 : vol;
            }
            count = count + f;
            if( count >= pcmwf.nSamplesPerSec )
            {
                count = count - pcmwf.nSamplesPerSec;
            }
            pointer[i] = calcwave(type, vol, count, pcmwf.nSamplesPerSec, duty);
        }
    }

    /* アンロック */
    hr = soundeffect->pDSBuffer->lpVtbl->Unlock( soundeffect->pDSBuffer, pointer, size, pointer2, size2 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    return obj;

}


/*--------------------------------------------------------------------
   波形を合成する。
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_add( int argc, VALUE *argv, VALUE obj )
{
    HRESULT hr;
    struct DXRubySoundEffect *soundeffect;
	DSBUFFERDESC desc;
	WAVEFORMATEX pcmwf;
	int i;
	short *pointer, *pointer2;
	DWORD size, size2;
	VALUE vf, vtype, vresolution;
	double count, duty = 0.5;
	double vol;
    double f;
	int type, resolution;
	int data;

	rb_scan_args( argc, argv, "02", &vtype, &vresolution );

	type       = vtype       == Qnil ? 0    : NUM2INT( vtype );
	resolution = vresolution == Qnil ? 1000 : (NUM2INT( vresolution ) > 44100 ? 44100 : NUM2INT( vresolution ));

    /* サウンドオブジェクト取得 */
    soundeffect = DXRUBY_GET_STRUCT( SoundEffect, obj );
    DXRUBY_CHECK_DISPOSE( soundeffect, pDSBuffer );

	/* ロック */
	hr = soundeffect->pDSBuffer->lpVtbl->Lock( soundeffect->pDSBuffer, 0, 0, (LPVOID *)&pointer, &size, (LPVOID *)&pointer2, &size2, DSBLOCK_ENTIREBUFFER );
    if( FAILED( hr ) || size2 != 0 )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

	pcmwf.nChannels = 1;
	pcmwf.nSamplesPerSec = 44100;
	pcmwf.wBitsPerSample = 16;
	pcmwf.nBlockAlign = pcmwf.nChannels * pcmwf.wBitsPerSample / 8;
	pcmwf.nAvgBytesPerSec = pcmwf.nSamplesPerSec * pcmwf.nBlockAlign;
	desc.dwBufferBytes = size;

	count = 0;

	/* 波形生成 */
	for( i = 0; i < desc.dwBufferBytes / (pcmwf.wBitsPerSample / 8); i++ )
	{
		/* 指定時間単位でブロックを呼び出す */
		if ( i % (pcmwf.nSamplesPerSec / resolution) == 0 )
		{
			vf = rb_yield( obj );
			if( TYPE( vf ) != T_ARRAY )
			{
				soundeffect->pDSBuffer->lpVtbl->Unlock( soundeffect->pDSBuffer, pointer, size, pointer2, size2 );
			    rb_raise(rb_eTypeError, "invalid value - SoundEffect_add");
				break;
			}
			f = NUM2DBL( rb_ary_entry(vf, 0) );
			vol = NUM2DBL(rb_ary_entry(vf, 1));
            if( RARRAY_LEN( vf ) > 2 )
            {
                duty = NUM2DBL(rb_ary_entry(vf, 2));
            }
			/* 最大/最低周波数と最大ボリュームの制限 */
			f = f > pcmwf.nSamplesPerSec / 2.0f ? pcmwf.nSamplesPerSec / 2.0f : f;
			f = f < 20 ? 20 : f;
			vol = vol > 255 ? 255 : vol;
			vol = vol < 0 ? 0 : vol;
		}
		count = count + f;
		if( count >= pcmwf.nSamplesPerSec )
		{
			count = count - pcmwf.nSamplesPerSec;
		}

		data = calcwave(type, vol, count, pcmwf.nSamplesPerSec, duty);

		if( data + (int)pointer[i] > 32767 )
		{
			pointer[i] = 32767;
		}
		else if( data + (int)pointer[i] < -32768 )
		{
			pointer[i] = -32768;
		}
		else
		{
			pointer[i] += data;
		}
	}

	/* アンロック */
	hr = soundeffect->pDSBuffer->lpVtbl->Unlock( soundeffect->pDSBuffer, pointer, size, pointer2, size2 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    return obj;

}


/*--------------------------------------------------------------------
   音を鳴らす
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_play( int argc, VALUE *argv, VALUE self )
{
    HRESULT hr;
    struct DXRubySoundEffect *se = DXRUBY_GET_STRUCT( SoundEffect, self );
    VALUE vflag;
    DXRUBY_CHECK_DISPOSE( se, pDSBuffer );

    rb_scan_args( argc, argv, "01", &vflag );

    /* とめる */
    hr = se->pDSBuffer->lpVtbl->Stop( se->pDSBuffer );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Sound play failed - SoundEffect_play" );
    }

    hr = se->pDSBuffer->lpVtbl->SetCurrentPosition( se->pDSBuffer, 0 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Sound play failed - SoundEffect_play" );
    }

    /* 再生 */
    hr = se->pDSBuffer->lpVtbl->Play( se->pDSBuffer, 0, 0, RTEST(vflag) ? DSBPLAY_LOOPING : 0 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Sound play failed - SoundEffect_play" );
    }
    return self;
}


/*--------------------------------------------------------------------
   音を止める
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_stop( VALUE obj )
{
    HRESULT hr;
    struct DXRubySoundEffect *soundeffect;

    soundeffect = DXRUBY_GET_STRUCT( SoundEffect, obj );
    DXRUBY_CHECK_DISPOSE( soundeffect, pDSBuffer );

    /* とめる */
    hr = soundeffect->pDSBuffer->lpVtbl->Stop( soundeffect->pDSBuffer );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "音の停止に失敗しました - Stop" );
    }

    return obj;
}


/*--------------------------------------------------------------------
   wavファイル出力
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_save( VALUE self, VALUE vfilename )
{
    HRESULT hr;
    HANDLE hfile;
    short *pointer, *pointer2;
    DWORD size, size2;
    DWORD tmpl;
    WORD tmps;
    DWORD wsize;
    struct DXRubySoundEffect *se = DXRUBY_GET_STRUCT( SoundEffect, self );

    DXRUBY_CHECK_DISPOSE( se, pDSBuffer );

    /* ロック */
    hr = se->pDSBuffer->lpVtbl->Lock( se->pDSBuffer, 0, 0, (LPVOID *)&pointer, &size, (LPVOID *)&pointer2, &size2, DSBLOCK_ENTIREBUFFER );
    if( FAILED( hr ) || size2 != 0 )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    hfile = CreateFile( RSTRING_PTR( vfilename ), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if( hfile == INVALID_HANDLE_VALUE )
    {
        rb_raise( eDXRubyError, "Write failure - open" );
    }

    if( !WriteFile( hfile, "RIFF", 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmpl = size + 44 - 8;
    if( !WriteFile( hfile, &tmpl, 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    if( !WriteFile( hfile, "WAVE", 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    if( !WriteFile( hfile, "fmt ", 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmpl = 16;
    if( !WriteFile( hfile, &tmpl, 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmps = 1;
    if( !WriteFile( hfile, &tmps, 2, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmps = 1;
    if( !WriteFile( hfile, &tmps, 2, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmpl = 44100;
    if( !WriteFile( hfile, &tmpl, 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmpl = 44100*2;
    if( !WriteFile( hfile, &tmpl, 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmps = 2;
    if( !WriteFile( hfile, &tmps, 2, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmps = 16;
    if( !WriteFile( hfile, &tmps, 2, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );

    if( !WriteFile( hfile, "data", 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    tmpl = size;
    if( !WriteFile( hfile, &tmpl, 4, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );
    if( !WriteFile( hfile, pointer, size, &wsize, NULL ) ) rb_raise( eDXRubyError, "Write failure - write" );

    CloseHandle( hfile );

    /* アンロック */
    hr = se->pDSBuffer->lpVtbl->Unlock( se->pDSBuffer, pointer, size, pointer2, size2 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    return self;
}


/*--------------------------------------------------------------------
   配列化
 ---------------------------------------------------------------------*/
static VALUE SoundEffect_to_a( VALUE self )
{
    HRESULT hr;
    short *pointer, *pointer2;
    DWORD size, size2;
    struct DXRubySoundEffect *se = DXRUBY_GET_STRUCT( SoundEffect, self );
    VALUE ary;
    int i;

    DXRUBY_CHECK_DISPOSE( se, pDSBuffer );

    /* ロック */
    hr = se->pDSBuffer->lpVtbl->Lock( se->pDSBuffer, 0, 0, (LPVOID *)&pointer, &size, (LPVOID *)&pointer2, &size2, DSBLOCK_ENTIREBUFFER );
    if( FAILED( hr ) || size2 != 0 )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    ary = rb_ary_new2( size / 2 );
    for( i = 0; i < size / 2; i++ )
    {
        double tmp;
        if( pointer[i] < 0 )
        {
            tmp = pointer[i] / 32768.0;
        }
        else
        {
            tmp = pointer[i] / 32767.0;
        }
        rb_ary_push( ary, rb_float_new( tmp ) );
    }

    /* アンロック */
    hr = se->pDSBuffer->lpVtbl->Unlock( se->pDSBuffer, pointer, size, pointer2, size2 );
    if( FAILED( hr ) )
    {
        rb_raise( eDXRubyError, "Failure to lock the SoundBuffer - Lock" );
    }

    return ary;
}


void Init_dxruby_Sound( void )
{
    /* 音量変換テーブルの初期化 */
    DXRuby_InitSoundVolumeTables();

    /* Soundクラス定義 */
    cSound = rb_define_class_under( mDXRuby, "Sound", rb_cObject );
    rb_define_singleton_method( cSound, "load_from_memory", Sound_load_from_memory, 2 );
    rb_define_singleton_method( cSound, "loadFromMemory", Sound_load_from_memory, 2 );

    /* Soundクラスにメソッド登録*/
    rb_define_private_method( cSound, "initialize"   , Sound_initialize   , 1 );
    rb_define_method( cSound, "dispose"      , Sound_dispose   , 0 );
    rb_define_method( cSound, "disposed?"    , Sound_check_disposed, 0 );
    rb_define_method( cSound, "play"         , Sound_play      , 0 );
    rb_define_method( cSound, "stop"         , Sound_stop         , 0 );
    rb_define_method( cSound, "set_volume"   , Sound_setVolume    , -1 );
    rb_define_method( cSound, "setVolume"    , Sound_setVolume    , -1 );
    rb_define_method( cSound, "pan"          , Sound_getPan       , 0 );
    rb_define_method( cSound, "pan="         , Sound_setPan       , 1 );
    rb_define_method( cSound, "frequency"    , Sound_getFrequency , 0 );
    rb_define_method( cSound, "frequency="   , Sound_setFrequency , 1 );
    rb_define_method( cSound, "start="       , Sound_setStart     , 1 );
    rb_define_method( cSound, "loop_start="  , Sound_setLoopStart , 1 );
    rb_define_method( cSound, "loopStart="   , Sound_setLoopStart , 1 );
    rb_define_method( cSound, "loop_end="    , Sound_setLoopEnd   , 1 );
    rb_define_method( cSound, "loopEnd="     , Sound_setLoopEnd   , 1 );
    rb_define_method( cSound, "loop_count="  , Sound_setLoopCount , 1 );
    rb_define_method( cSound, "loopCount="   , Sound_setLoopCount , 1 );

    /* Soundオブジェクトを生成した時にinitializeの前に呼ばれるメモリ割り当て関数登録 */
    rb_define_alloc_func( cSound, Sound_allocate );


    /* SoundEffectクラス定義 */
    cSoundEffect = rb_define_class_under( mDXRuby, "SoundEffect", rb_cObject );

    /* SoundEffectクラスにメソッド登録*/
    rb_define_private_method( cSoundEffect, "initialize", SoundEffect_initialize, -1 );
    rb_define_method( cSoundEffect, "dispose"   , SoundEffect_dispose   , 0 );
    rb_define_method( cSoundEffect, "disposed?" , SoundEffect_check_disposed, 0 );
    rb_define_method( cSoundEffect, "play"      , SoundEffect_play      , -1 );
    rb_define_method( cSoundEffect, "stop"      , SoundEffect_stop      , 0 );
    rb_define_method( cSoundEffect, "add"       , SoundEffect_add       , -1 );
    rb_define_method( cSoundEffect, "save"      , SoundEffect_save      , 1 );
#ifdef DXRUBY15
    rb_define_method( cSoundEffect, "to_a"      , SoundEffect_to_a      , 0 );
#endif
    /* SoundEffectオブジェクトを生成した時にinitializeの前に呼ばれるメモリ割り当て関数登録 */
    rb_define_alloc_func( cSoundEffect, SoundEffect_allocate );

    rb_define_const( mDXRuby, "WAVE_SIN"     , INT2FIX(WAVE_SIN) );
    rb_define_const( mDXRuby, "WAVE_SAW"     , INT2FIX(WAVE_SAW) );
    rb_define_const( mDXRuby, "WAVE_TRI"     , INT2FIX(WAVE_TRI) );
    rb_define_const( mDXRuby, "WAVE_RECT"    , INT2FIX(WAVE_RECT) );

    rb_define_const( mDXRuby, "TYPE_MIDI"    , INT2FIX(0) );
    rb_define_const( mDXRuby, "TYPE_WAV"     , INT2FIX(1) );
}