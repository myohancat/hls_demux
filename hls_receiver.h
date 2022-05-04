#ifndef __HLS_RECEIVER_H_
#define __HLS_RECEIVER_H_

#include "m3u8_parser.h"
#include <stdbool.h>

typedef struct HLSReceiver_s*  HLSReceiver;

typedef void (*OnDonwloadComplete_fn)(const HLSReceiver receiver, Playlist_t* pls, int bandwidth, void* opaque);

HLSReceiver HLS_Receiver_Create(Playlist_t* playlist, AVIOInterruptCB* int_cb, OnDonwloadComplete_fn callback, void* opaque);
int         HLS_Receiver_Start(HLSReceiver receiver);
int         HLS_Receiver_Stop(HLSReceiver receiver);
void        HLS_Receiver_Delete(HLSReceiver receiver);

int HLS_Receiver_Read(HLSReceiver receiver, unsigned char* buf, int bufLen);
int HLS_Receiver_Seek(HLSReceiver receiver, int64_t timestamp);

int HLS_Receiver_SetPlaylist(HLSReceiver receiver, Playlist_t* pls);

int64_t HLS_Receiver_GetCurrentSegmentPts(HLSReceiver receiver);
bool    HLS_Receiver_CheckEOS(HLSReceiver receiver);

#endif /* __HLS_RECEIVER_H_ */
