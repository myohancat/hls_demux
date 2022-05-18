#include "buffered_stream.h"

#include "hls_log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct Block_s {
    struct Block_s* mNext;

    unsigned char*  mData;
    int             mSize;
}Block_t;

typedef struct BufferedStream_s {
    Block_t*        mFront;
    Block_t*        mRear;

    int             mSize;

    bool            mEOS;
    pthread_mutex_t mLock;
    pthread_cond_t  mCondVarFull;

} BufferedStream_t;

BufferedStream  BufferedStream_Create()
{
    BufferedStream stream = (BufferedStream_t*)malloc(sizeof(BufferedStream_t));
    if (!stream)
        return NULL;
    
    memset(stream, 0x00, sizeof(BufferedStream_t));
    
    pthread_mutex_init(&stream->mLock, NULL);
    pthread_cond_init(&stream->mCondVarFull, NULL);

    return stream;
}

void BufferedStream_Delete(BufferedStream stream)
{
    if (!stream)
        return;

    BufferedStream_Flush(stream);

    pthread_mutex_destroy(&stream->mLock);
    pthread_cond_destroy(&stream->mCondVarFull);

    free(stream);
}

int BufferedStream_Peek(BufferedStream stream, unsigned char* buf, int len, int offset)
{
    int pos = 0;
    Block_t* block = NULL;

    if (!stream)
        return -1;

    pthread_mutex_lock(&stream->mLock);

    if (stream->mFront == NULL && !stream->mEOS)
        pthread_cond_wait(&stream->mCondVarFull, &stream->mLock);

    for (block = stream->mFront; block != NULL; block = block->mNext)
    {
        if (block->mSize > offset)
            break;

        offset -= block->mSize;
    }

    while (pos < len)
    {
        if (!block)
            break;

        if ((offset + len - pos) < block->mSize)
        {
            memcpy(buf + pos, block->mData + offset, len - pos);

            pos += (len - pos);
            break;
        }
        else
        {
            memcpy(buf + pos, block->mData + offset, block->mSize - offset);
            
            pos += block->mSize - offset;
            block = block->mNext;
        }
    }
    pthread_mutex_unlock(&stream->mLock);

    return pos;
}

int BufferedStream_Read(BufferedStream stream, unsigned char* buf, int len)
{
    int pos = 0;
    if (!stream)
        return -1;

    pthread_mutex_lock(&stream->mLock);

    if (stream->mFront == NULL && !stream->mEOS)
        pthread_cond_wait(&stream->mCondVarFull, &stream->mLock);

    while (pos < len)
    {
        Block_t* block = stream->mFront;

        if (!block)
            break;

        if ((len - pos) < block->mSize)
        {
            int size = len - pos;

            memcpy(buf + pos, block->mData, size);
            pos += size;

            block->mData += size;
            block->mSize -= size;
            break;
        }
        else
        {
            memcpy(buf + pos, block->mData, block->mSize);
            
            pos += block->mSize;
            stream->mFront = block->mNext;
            if (stream->mFront == NULL)
                stream->mRear = NULL;

            free(block);
        }
    }

    pthread_mutex_unlock(&stream->mLock);

    return pos;
}

int BufferedStream_Write(BufferedStream stream, unsigned char* buf, int len)
{
    Block_t* block = NULL;

    if (!stream)
        return -1;

    block = (Block_t*)malloc(sizeof(Block_t) + len);
    if (!block)
        return -1;

    pthread_mutex_lock(&stream->mLock);

    block->mNext = NULL;
    block->mData = (unsigned char*)block + sizeof(Block_t);
    block->mSize = len;
    memcpy(block->mData, buf, len);

    if (!stream->mRear)
        stream->mFront = block;
    else 
        stream->mRear->mNext = block;

    stream->mRear = block;

    pthread_cond_signal(&stream->mCondVarFull);
    pthread_mutex_unlock(&stream->mLock);

    return 0;
}

void BufferedStream_SetEOS(BufferedStream stream, bool isEOS)
{
    if (!stream)
        return;

    pthread_mutex_lock(&stream->mLock);

    stream->mEOS = isEOS;
    pthread_cond_signal(&stream->mCondVarFull);

    pthread_mutex_unlock(&stream->mLock);
}

void BufferedStream_Flush(BufferedStream stream)
{
    Block_t* block = NULL;

    if (!stream)
        return;
    
    pthread_mutex_lock(&stream->mLock);
    
    while (stream->mFront)
    {
        block = stream->mFront;
        stream->mFront = block->mNext;
        free(block);
    }

    stream->mRear = NULL;
    stream->mSize = 0;

    pthread_mutex_unlock(&stream->mLock);
}
