#include "media_object_buffer.h"

#include "hls_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct MediaObjectBuffer_s
{
    int          mCapacity;

    MediaObject* mBuffer;
    int          mFront;
    int          mRear;
    int          mCount;

    bool         mEOS;

    pthread_mutex_t mLock;
    pthread_cond_t  mCondVarFull;
    pthread_cond_t  mCondVarEmpty;

}MediaObjectBuffer_t;

#define IS_EMPTY(buffer)  (buffer->mCount == 0)
#define IS_FULL(buffer)   (buffer->mCapacity == buffer->mCount)

MediaObjectBuffer MediaObjectBuffer_Create(int capacity)
{
    int rc;
    pthread_condattr_t attr;

    MediaObjectBuffer buffer = (MediaObjectBuffer)malloc(sizeof(MediaObjectBuffer_t) + capacity * sizeof(MediaObject));
    if (!buffer)
    {
        LOG_ERROR("Cannot allocate buffer !!\n");
        goto ERROR;
    }
    
    memset(buffer, 0x00, sizeof(MediaObjectBuffer_t));

    if ((rc = pthread_mutex_init(&buffer->mLock, NULL)) != 0)
    {
        LOG_ERROR("Cannot init mutext !!\n");
        goto ERROR;
    }

    if ((rc = pthread_condattr_init(&attr)) != 0)
    {
        LOG_ERROR("Cannot init cond attribute !!\n");
        pthread_mutex_destroy(&buffer->mLock);
         goto ERROR;
    }

    if ((rc = pthread_cond_init(&buffer->mCondVarFull, &attr)) != 0)
    {
        LOG_ERROR("Cannot init condvariable Full !!\n");
        pthread_mutex_destroy(&buffer->mLock);
        goto ERROR;
    }

    if ((rc = pthread_cond_init(&buffer->mCondVarEmpty, &attr)) != 0)
    {
        LOG_ERROR("Cannot init condvariable Empty !!\n");
        pthread_mutex_destroy(&buffer->mLock);
        goto ERROR;
    }

    buffer->mBuffer   = (MediaObject*)(buffer + 1);
    buffer->mCapacity = capacity;
    buffer->mFront    = 0;
    buffer->mRear     = 0;
    buffer->mCount    = 0;
    buffer->mEOS      = false;

    goto EXIT;
ERROR:
    if (!buffer)
    {
        free(buffer);
        buffer = NULL;
    }
EXIT:

    return buffer;
}

void MediaObjectBuffer_Delete(MediaObjectBuffer buffer)
{
    if (!buffer)
        return;

    MediaObjectBuffer_Flush(buffer);

    pthread_mutex_destroy(&buffer->mLock);
    pthread_cond_destroy(&buffer->mCondVarFull);
    pthread_cond_destroy(&buffer->mCondVarEmpty);

    free(buffer);
}

int MediaObjectBuffer_Get(MediaObjectBuffer buffer, MediaObject* obj, int timeout)
{
    int rc;
    struct timespec target;

    if (!buffer)
        return BUFFER_ERROR;

    if (timeout > 0)
    {
        struct timeval now;
        rc = gettimeofday(&now, NULL);
        target.tv_nsec = now.tv_usec * 1000 + (timeout%1000)*1000000;
        target.tv_sec = now.tv_sec + (timeout/1000);

        if(target.tv_nsec > 1000000000) 
        {
            target.tv_nsec -=  1000000000;
            target.tv_sec ++;
        }
    }

    pthread_mutex_lock(&buffer->mLock);

    if (IS_EMPTY(buffer) && !buffer->mEOS)
    {
        if (timeout == 0)
        {
            pthread_mutex_unlock(&buffer->mLock);
            return BUFFER_ERROR_EMPTY;
        }

        if (timeout == -1)
        {
            rc = pthread_cond_wait(&buffer->mCondVarEmpty, &buffer->mLock);
        }
        else
        {
            rc = pthread_cond_timedwait(&buffer->mCondVarEmpty, &buffer->mLock, &target);
            if (rc == ETIMEDOUT) 
            {
                pthread_mutex_unlock(&buffer->mLock);
                return BUFFER_ERROR_TIMEOUT;
            }
        }
    }

    if (buffer->mEOS)
    {
        pthread_mutex_unlock(&buffer->mLock);
        return BUFFER_ERROR_EOS;
    }

    *obj = buffer->mBuffer[buffer->mFront];
    buffer->mFront = (buffer->mFront + 1) % buffer->mCapacity;
    buffer->mCount --;

    pthread_cond_signal(&buffer->mCondVarFull);
    pthread_mutex_unlock(&buffer->mLock);

    return BUFFER_SUCCESS;    
}

int MediaObjectBuffer_Put(MediaObjectBuffer buffer, const MediaObject obj, int timeout)
{
    int rc;
    struct timespec target;

    if (!buffer)
        return BUFFER_ERROR;
    
    rc = pthread_mutex_lock(&buffer->mLock);
    if (rc != 0) 
    {
        LOG_ERROR("pthread_mutex_lock failed !! rc=%d", rc);
        return BUFFER_ERROR;
    }

    if (timeout > 0)
    {
        struct timeval now;
        rc = gettimeofday(&now, NULL);
        target.tv_nsec = now.tv_usec * 1000 + (timeout%1000)*1000000;
        target.tv_sec = now.tv_sec + (timeout/1000);

        if (target.tv_nsec > 1000000000) 
        {
            target.tv_nsec -=  1000000000;
            target.tv_sec ++;
        }
    }

    if (IS_FULL(buffer) && !buffer->mEOS)
    {
        if (timeout == 0)
        {
            pthread_mutex_unlock(&buffer->mLock);
            return BUFFER_ERROR_FULL;
        }

        if (timeout == -1)
        {
            rc = pthread_cond_wait(&buffer->mCondVarFull, &buffer->mLock);
        }
        else
        {
            rc = pthread_cond_timedwait(&buffer->mCondVarFull, &buffer->mLock, &target);
            if (rc == ETIMEDOUT) 
            {
                pthread_mutex_unlock(&buffer->mLock);
                return BUFFER_ERROR_TIMEOUT;
            }
        }
    }

    if (buffer->mEOS)
    {
        pthread_mutex_unlock(&buffer->mLock);
        return BUFFER_ERROR_EOS;
    }

    if (IS_FULL(buffer))
    {
        pthread_mutex_unlock(&buffer->mLock);
        return BUFFER_ERROR_FULL;
    }

    buffer->mBuffer[buffer->mRear] =  obj;
    buffer->mRear = (buffer->mRear + 1) % buffer->mCapacity;
    buffer->mCount ++;

    pthread_cond_signal(&buffer->mCondVarEmpty);
    pthread_mutex_unlock(&buffer->mLock);

    return 0;
}

void MediaObjectBuffer_SetEOS(MediaObjectBuffer buffer, bool isEOS)
{
    if (!buffer)
        return;

    pthread_mutex_lock(&buffer->mLock);

    buffer->mEOS = isEOS;    
    
    pthread_cond_signal(&buffer->mCondVarFull);
    pthread_cond_signal(&buffer->mCondVarEmpty);

    pthread_mutex_unlock(&buffer->mLock);
}

bool MediaObjectBuffer_GetEOS(MediaObjectBuffer buffer)
{
    bool isEOS = false;

    if (!buffer)
        return true;


    pthread_mutex_lock(&buffer->mLock);
    isEOS = buffer->mEOS;
    pthread_mutex_unlock(&buffer->mLock);

    return isEOS;
}

void MediaObjectBuffer_Flush(MediaObjectBuffer buffer)
{
    if (!buffer)
        return;

    pthread_mutex_lock(&buffer->mLock);

    while (!IS_EMPTY(buffer))
    {
        MediaObject obj = buffer->mBuffer[buffer->mFront];

        MediaObject_Delete(obj);
        buffer->mFront = (buffer->mFront + 1) % buffer->mCapacity;
        buffer->mCount --;
    }

    buffer->mFront = 0;
    buffer->mRear  = 0;

    pthread_mutex_unlock(&buffer->mLock);
}

bool MediaObjectBuffer_IsEmpty(MediaObjectBuffer buffer)
{
    bool ret;

    pthread_mutex_lock(&buffer->mLock);
    ret = IS_EMPTY(buffer);
    pthread_mutex_unlock(&buffer->mLock);
    
    return ret;    
}

bool MediaObjectBuffer_IsFull(MediaObjectBuffer buffer)
{
    bool ret;

    pthread_mutex_lock(&buffer->mLock);
    ret = IS_FULL(buffer);
    pthread_mutex_unlock(&buffer->mLock);
    
    return ret;    
}

int MediaObjectBuffer_GetStatus(MediaObjectBuffer buffer, int* capacity, int* free)
{
    if (!buffer)
        return BUFFER_ERROR;

    pthread_mutex_lock(&buffer->mLock);

    *capacity = buffer->mCapacity;
    *free     = buffer->mCapacity - buffer->mCount;

    pthread_mutex_unlock(&buffer->mLock);

    return 0;
}

