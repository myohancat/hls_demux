#ifndef __MEDIA_OBJECT_H_
#define __MEDIA_OBJECT_H_

#include "hls_common.h"
#include "m3u8_parser.h"
#include "libavformat/avio.h"

typedef struct MediaObject_s* MediaObject;

MediaObject MediaObject_Create(Segment_t* seg, AVIOInterruptCB* int_cb);
int         MediaObject_StartDownload(MediaObject obj);
int         MediaObject_StopDownload(MediaObject obj);
void        MediaObject_WaitForEnd(MediaObject obj);
void        MediaObject_Delete(MediaObject obj);

int  MediaObject_Read(MediaObject obj, unsigned char* buf, int bufLen);
int  MediaObject_Peek(MediaObject obj, unsigned char* buf, int bufLen, int offset);

int MediaObject_GetBandwidth(MediaObject obj);
Segment_t* MediaObject_GetSegment(MediaObject obj);
int64_t MediaObject_GetSegmentStartPts(MediaObject obj); /* TBD. Change Name */

#endif /* __MEDIA_OBJECT_H_ */
