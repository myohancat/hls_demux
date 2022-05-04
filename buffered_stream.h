#ifndef __BUFFERED_STREAM_H_
#define __BUFFERED_STREAM_H_

#include <stdbool.h>

typedef struct BufferedStream_s* BufferedStream;

BufferedStream  BufferedStream_Create(void);
void            BufferedStream_Delete(BufferedStream stream);

int BufferedStream_Peek(BufferedStream stream, unsigned char* buf, int len, int offset); 
int BufferedStream_Read(BufferedStream stream, unsigned char* buf, int len); 

int BufferedStream_Write(BufferedStream stream, unsigned char* buf, int len);

void BufferedStream_SetEOS(BufferedStream stream, bool isEOS);
void BufferedStream_Flush(BufferedStream stream);

#endif // __BUFFERED_STREAM_H_
