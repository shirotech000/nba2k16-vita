#pragma once
// minimal OpenSL ES 1.0.1 (+ the android bits) written from the public spec.
// slot order is everything here: the game indexes these vtables by offset.
#include <stddef.h>
#include <stdint.h>

typedef int8_t   SLint8;
typedef uint8_t  SLuint8;
typedef int16_t  SLint16;
typedef uint16_t SLuint16;
typedef int      SLint32;
typedef unsigned SLuint32;
typedef SLuint32 SLboolean;
typedef SLuint32 SLresult;
typedef SLint16  SLmillibel;
typedef SLuint32 SLmillisecond;
typedef SLuint32 SLmilliHertz;
typedef SLint16  SLpermille;
typedef SLuint8  SLchar;

#define SL_BOOLEAN_FALSE ((SLboolean)0)
#define SL_BOOLEAN_TRUE  ((SLboolean)1)

#define SL_RESULT_SUCCESS                ((SLresult)0x00)
#define SL_RESULT_PRECONDITIONS_VIOLATED ((SLresult)0x01)
#define SL_RESULT_PARAMETER_INVALID      ((SLresult)0x02)
#define SL_RESULT_MEMORY_FAILURE         ((SLresult)0x03)
#define SL_RESULT_RESOURCE_ERROR         ((SLresult)0x04)
#define SL_RESULT_RESOURCE_LOST          ((SLresult)0x05)
#define SL_RESULT_IO_ERROR               ((SLresult)0x06)
#define SL_RESULT_BUFFER_INSUFFICIENT    ((SLresult)0x07)
#define SL_RESULT_CONTENT_CORRUPTED      ((SLresult)0x08)
#define SL_RESULT_CONTENT_UNSUPPORTED    ((SLresult)0x09)
#define SL_RESULT_CONTENT_NOT_FOUND      ((SLresult)0x0A)
#define SL_RESULT_PERMISSION_DENIED      ((SLresult)0x0B)
#define SL_RESULT_FEATURE_UNSUPPORTED    ((SLresult)0x0C)
#define SL_RESULT_INTERNAL_ERROR         ((SLresult)0x0D)
#define SL_RESULT_UNKNOWN_ERROR          ((SLresult)0x0E)
#define SL_RESULT_OPERATION_ABORTED      ((SLresult)0x0F)
#define SL_RESULT_CONTROL_LOST           ((SLresult)0x10)

#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED   2
#define SL_OBJECT_STATE_SUSPENDED  3

#define SL_OBJECTID_ENGINE            0x1001
#define SL_OBJECTID_LEDDEVICE         0x1002
#define SL_OBJECTID_VIBRADEVICE       0x1003
#define SL_OBJECTID_AUDIOPLAYER       0x1004
#define SL_OBJECTID_AUDIORECORDER     0x1005
#define SL_OBJECTID_MIDIPLAYER        0x1006
#define SL_OBJECTID_LISTENER          0x1007
#define SL_OBJECTID_3DGROUP           0x1008
#define SL_OBJECTID_OUTPUTMIX         0x1009
#define SL_OBJECTID_METADATAEXTRACTOR 0x100A

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_PLAYEVENT_HEADATEND    ((SLuint32)0x01)
#define SL_PLAYEVENT_HEADATMARKER ((SLuint32)0x02)
#define SL_PLAYEVENT_HEADATNEWPOS ((SLuint32)0x04)
#define SL_PLAYEVENT_HEADMOVING   ((SLuint32)0x08)
#define SL_PLAYEVENT_HEADSTALLED  ((SLuint32)0x10)

#define SL_TIME_UNKNOWN ((SLuint32)0xFFFFFFFF)

#define SL_MILLIBEL_MAX ((SLmillibel)0x7FFF)
#define SL_MILLIBEL_MIN ((SLmillibel)(-SL_MILLIBEL_MAX - 1))

#define SL_DATALOCATOR_URI             0x01
#define SL_DATALOCATOR_ADDRESS         0x02
#define SL_DATALOCATOR_IODEVICE        0x03
#define SL_DATALOCATOR_OUTPUTMIX       0x04
#define SL_DATALOCATOR_BUFFERQUEUE     0x06
#define SL_DATALOCATOR_MIDIBUFFERQUEUE 0x07
// android
#define SL_DATALOCATOR_ANDROIDFD                0x800007BC
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BD
#define SL_DATALOCATOR_ANDROIDBUFFERQUEUE       0x800007BE

#define SL_DATAFORMAT_MIME 1
#define SL_DATAFORMAT_PCM  2

#define SL_CONTAINERTYPE_UNSPECIFIED 0x01
#define SL_CONTAINERTYPE_RAW         0x02

#define SL_BYTEORDER_BIGENDIAN    1
#define SL_BYTEORDER_LITTLEENDIAN 2

// interface ids
struct SLInterfaceID_ {
  SLuint32 time_low;
  SLuint16 time_mid;
  SLuint16 time_hi_and_version;
  SLuint16 clock_seq;
  SLuint8 node[6];
};
typedef const struct SLInterfaceID_ *SLInterfaceID;

// data source / sink
typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

// same layout, android just gave it its own name + locator id
typedef SLDataLocator_BufferQueue SLDataLocator_AndroidSimpleBufferQueue;
typedef SLDataLocator_BufferQueue SLDataLocator_AndroidBufferQueue;

typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec; // milliHz!
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 formatType;
  SLchar *mimeType;
  SLuint32 containerType;
} SLDataFormat_MIME;

typedef struct {
  SLuint32 feature;
  SLuint32 data;
} SLEngineOption;

// SLObjectItf
struct SLObjectItf_;
typedef const struct SLObjectItf_ *const *SLObjectItf;

typedef struct {
  SLuint32 locatorType;
  SLObjectItf outputMix;
} SLDataLocator_OutputMix;

typedef void (*slObjectCallback)(SLObjectItf caller, const void *pContext, SLuint32 event,
                                 SLresult result, SLuint32 param, void *pInterface);

struct SLObjectItf_ {
  SLresult (*Realize)(SLObjectItf self, SLboolean async);
  SLresult (*Resume)(SLObjectItf self, SLboolean async);
  SLresult (*GetState)(SLObjectItf self, SLuint32 *pState);
  SLresult (*GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(SLObjectItf self, slObjectCallback callback, void *pContext);
  void (*AbortAsyncOperation)(SLObjectItf self);
  void (*Destroy)(SLObjectItf self);
  SLresult (*SetPriority)(SLObjectItf self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable);
  SLresult (*SetLossOfControlInterfaces)(SLObjectItf self, SLint16 numInterfaces,
                                         SLInterfaceID *pInterfaceIDs, SLboolean enabled);
};

// SLEngineItf
struct SLEngineItf_;
typedef const struct SLEngineItf_ *const *SLEngineItf;

struct SLEngineItf_ {
  SLresult (*CreateLEDDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                              SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                              const SLboolean *pInterfaceRequired);
  SLresult (*CreateVibraDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                                SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                                const SLboolean *pInterfaceRequired);
  SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pAudioSrc,
                                SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                                const SLInterfaceID *pInterfaceIds,
                                const SLboolean *pInterfaceRequired);
  SLresult (*CreateAudioRecorder)(SLEngineItf self, SLObjectItf *pRecorder,
                                  SLDataSource *pAudioSrc, SLDataSink *pAudioSnk,
                                  SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                                  const SLboolean *pInterfaceRequired);
  SLresult (*CreateMidiPlayer)(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pMIDISrc,
                               SLDataSource *pBankSrc, SLDataSink *pAudioOutput,
                               SLDataSink *pVibra, SLDataSink *pLEDArray, SLuint32 numInterfaces,
                               const SLInterfaceID *pInterfaceIds,
                               const SLboolean *pInterfaceRequired);
  SLresult (*CreateListener)(SLEngineItf self, SLObjectItf *pListener, SLuint32 numInterfaces,
                             const SLInterfaceID *pInterfaceIds,
                             const SLboolean *pInterfaceRequired);
  SLresult (*Create3DGroup)(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces,
                            const SLInterfaceID *pInterfaceIds,
                            const SLboolean *pInterfaceRequired);
  SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLboolean *pInterfaceRequired);
  SLresult (*CreateMetadataExtractor)(SLEngineItf self, SLObjectItf *pMetadataExtractor,
                                      SLDataSource *pDataSource, SLuint32 numInterfaces,
                                      const SLInterfaceID *pInterfaceIds,
                                      const SLboolean *pInterfaceRequired);
  SLresult (*CreateExtensionObject)(SLEngineItf self, SLObjectItf *pObject, void *pParameters,
                                    SLuint32 objectID, SLuint32 numInterfaces,
                                    const SLInterfaceID *pInterfaceIds,
                                    const SLboolean *pInterfaceRequired);
  SLresult (*QueryNumSupportedInterfaces)(SLEngineItf self, SLuint32 objectID,
                                          SLuint32 *pNumSupportedInterfaces);
  SLresult (*QuerySupportedInterfaces)(SLEngineItf self, SLuint32 objectID, SLuint32 index,
                                       SLInterfaceID *pInterfaceId);
  SLresult (*QueryNumSupportedExtensions)(SLEngineItf self, SLuint32 *pNumExtensions);
  SLresult (*QuerySupportedExtension)(SLEngineItf self, SLuint32 index, SLchar *pExtensionName,
                                      SLint16 *pNameLength);
  SLresult (*IsExtensionSupported)(SLEngineItf self, const SLchar *pExtensionName,
                                   SLboolean *pSupported);
};

// SLPlayItf
struct SLPlayItf_;
typedef const struct SLPlayItf_ *const *SLPlayItf;
typedef void (*slPlayCallback)(SLPlayItf caller, void *pContext, SLuint32 event);

struct SLPlayItf_ {
  SLresult (*SetPlayState)(SLPlayItf self, SLuint32 state);
  SLresult (*GetPlayState)(SLPlayItf self, SLuint32 *pState);
  SLresult (*GetDuration)(SLPlayItf self, SLmillisecond *pMsec);
  SLresult (*GetPosition)(SLPlayItf self, SLmillisecond *pMsec);
  SLresult (*RegisterCallback)(SLPlayItf self, slPlayCallback callback, void *pContext);
  SLresult (*SetCallbackEventsMask)(SLPlayItf self, SLuint32 eventFlags);
  SLresult (*GetCallbackEventsMask)(SLPlayItf self, SLuint32 *pEventFlags);
  SLresult (*SetMarkerPosition)(SLPlayItf self, SLmillisecond mSec);
  SLresult (*ClearMarkerPosition)(SLPlayItf self);
  SLresult (*GetMarkerPosition)(SLPlayItf self, SLmillisecond *pMsec);
  SLresult (*SetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond mSec);
  SLresult (*GetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond *pMsec);
};

// SLBufferQueueItf. android's SLAndroidSimpleBufferQueueItf is the exact
// same shape (Enqueue, Clear, GetState, RegisterCallback, state = {count, index})
struct SLBufferQueueItf_;
typedef const struct SLBufferQueueItf_ *const *SLBufferQueueItf;
typedef SLBufferQueueItf SLAndroidSimpleBufferQueueItf;
typedef void (*slBufferQueueCallback)(SLBufferQueueItf caller, void *pContext);

typedef struct {
  SLuint32 count;
  SLuint32 playIndex;
} SLBufferQueueState;

struct SLBufferQueueItf_ {
  SLresult (*Enqueue)(SLBufferQueueItf self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(SLBufferQueueItf self);
  SLresult (*GetState)(SLBufferQueueItf self, SLBufferQueueState *pState);
  SLresult (*RegisterCallback)(SLBufferQueueItf self, slBufferQueueCallback callback,
                               void *pContext);
};

// SLAndroidBufferQueueItf: the source side of a decoder player. NOT the
// simple BQ: own vtable, buffers carry a context + optional items (EOS etc)
struct SLAndroidBufferQueueItf_;
typedef const struct SLAndroidBufferQueueItf_ *const *SLAndroidBufferQueueItf;

#define SL_ANDROID_ITEMKEY_NONE             ((SLuint32)0x00)
#define SL_ANDROID_ITEMKEY_EOS              ((SLuint32)0x01)
#define SL_ANDROID_ITEMKEY_DISCONTINUITY    ((SLuint32)0x02)
#define SL_ANDROID_ITEMKEY_BUFFERQUEUEEVENT ((SLuint32)0x03)
#define SL_ANDROID_ITEMKEY_FORMAT_CHANGE    ((SLuint32)0x04)

#define SL_ANDROIDBUFFERQUEUEEVENT_NONE        ((SLuint32)0x00)
#define SL_ANDROIDBUFFERQUEUEEVENT_PROCESSED   ((SLuint32)0x01)
#define SL_ANDROIDBUFFERQUEUEEVENT_UNREALIZED  ((SLuint32)0x02)
#define SL_ANDROIDBUFFERQUEUEEVENT_CLEARED     ((SLuint32)0x04)
#define SL_ANDROIDBUFFERQUEUEEVENT_STOPPED     ((SLuint32)0x08)
#define SL_ANDROIDBUFFERQUEUEEVENT_ERROR       ((SLuint32)0x10)
#define SL_ANDROIDBUFFERQUEUEEVENT_CONTENT_END ((SLuint32)0x20)

// items are packed back to back: {key, size, size bytes of data}
typedef struct {
  SLuint32 itemKey;
  SLuint32 itemSize;
  SLuint8 itemData[];
} SLAndroidBufferItem;

typedef struct {
  SLuint32 count;
  SLuint32 index;
} SLAndroidBufferQueueState;

typedef SLresult (*slAndroidBufferQueueCallback)(SLAndroidBufferQueueItf caller, void *pCallbackContext,
                                                 void *pBufferContext, void *pBufferData,
                                                 SLuint32 dataSize, SLuint32 dataUsed,
                                                 const SLAndroidBufferItem *pItems,
                                                 SLuint32 itemsLength);

struct SLAndroidBufferQueueItf_ {
  SLresult (*RegisterCallback)(SLAndroidBufferQueueItf self, slAndroidBufferQueueCallback callback,
                               void *pCallbackContext);
  SLresult (*Clear)(SLAndroidBufferQueueItf self);
  SLresult (*Enqueue)(SLAndroidBufferQueueItf self, void *pBufferContext, void *pData,
                      SLuint32 dataLength, const SLAndroidBufferItem *pItems, SLuint32 itemsLength);
  SLresult (*GetState)(SLAndroidBufferQueueItf self, SLAndroidBufferQueueState *pState);
  SLresult (*SetCallbackEventsMask)(SLAndroidBufferQueueItf self, SLuint32 eventFlags);
  SLresult (*GetCallbackEventsMask)(SLAndroidBufferQueueItf self, SLuint32 *pEventFlags);
};

// SLVolumeItf
struct SLVolumeItf_;
typedef const struct SLVolumeItf_ *const *SLVolumeItf;

struct SLVolumeItf_ {
  SLresult (*SetVolumeLevel)(SLVolumeItf self, SLmillibel level);
  SLresult (*GetVolumeLevel)(SLVolumeItf self, SLmillibel *pLevel);
  SLresult (*GetMaxVolumeLevel)(SLVolumeItf self, SLmillibel *pMaxLevel);
  SLresult (*SetMute)(SLVolumeItf self, SLboolean mute);
  SLresult (*GetMute)(SLVolumeItf self, SLboolean *pMute);
  SLresult (*EnableStereoPosition)(SLVolumeItf self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(SLVolumeItf self, SLboolean *pEnable);
  SLresult (*SetStereoPosition)(SLVolumeItf self, SLpermille stereoPosition);
  SLresult (*GetStereoPosition)(SLVolumeItf self, SLpermille *pStereoPosition);
};

// the one function the game links against
SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions,
                        const SLEngineOption *pEngineOptions, SLuint32 numInterfaces,
                        const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);

// the IIDs it imports (data symbols)
extern const SLInterfaceID SL_IID_ENGINE;
extern const SLInterfaceID SL_IID_PLAY;
extern const SLInterfaceID SL_IID_BUFFERQUEUE;
extern const SLInterfaceID SL_IID_ANDROIDBUFFERQUEUESOURCE;
extern const SLInterfaceID SL_IID_VOLUME;
extern const SLInterfaceID SL_IID_3DLOCATION;
extern const SLInterfaceID SL_IID_3DSOURCE;
extern const SLInterfaceID SL_IID_ENVIRONMENTALREVERB;
