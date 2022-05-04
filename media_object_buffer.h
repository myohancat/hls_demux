#ifndef __MEDIA_OBJECT_BUFFER_H_
#define __MEDIA_OBJECT_BUFFER_H_

#include "media_object.h"
#include <stdbool.h>

#define BUFFER_SUCCESS        (0)
#define BUFFER_ERROR          (-1)
#define BUFFER_ERROR_EMPTY    (-2)
#define BUFFER_ERROR_FULL     (-3)
#define BUFFER_ERROR_TIMEOUT  (-4)
#define BUFFER_ERROR_EOS      (-5)

typedef struct MediaObjectBuffer_s* MediaObjectBuffer;

MediaObjectBuffer MediaObjectBuffer_Create(int capacity);
void              MediaObjectBuffer_Delete(MediaObjectBuffer buffer);


int MediaObjectBuffer_Put(MediaObjectBuffer buffer, const MediaObject obj, int timeOut);
int MediaObjectBuffer_Get(MediaObjectBuffer buffer, MediaObject* obj, int timeOut); 

int MediaObjectBuffer_GetStatus(MediaObjectBuffer buffer, int* capacity, int* free);
bool MediaObjectBuffer_IsEmpty(MediaObjectBuffer buffer);
bool MediaObjectBuffer_IsFull(MediaObjectBuffer buffer);

void MediaObjectBuffer_SetEOS(MediaObjectBuffer buffer, bool isEOS);
bool MediaObjectBuffer_GetEOS(MediaObjectBuffer buffer);

void MediaObjectBuffer_Flush(MediaObjectBuffer buffer);

#endif // __MEDIA_OBJECT_BUFFER_H_
