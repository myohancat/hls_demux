#include "hls_receiver.h"

#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>

#include "hls_common.h"
#include "media_object.h"
#include "media_object_buffer.h"

#define ENABLE_DEBUG_STOP_PERFORMANCE

#define VOD_SEGMENT_BUFFER_SIZE  (3)
#define LIVE_SEGMENT_BUFFER_SIZE (2)
#define LIVE_START_INDEX         (-2)

#define MAX_INIT_SEGMENTS        (16)

typedef struct HLSReceiver_s {
    MediaObjectBuffer     mBuffer;

    AVIOInterruptCB*      mParentIntCB;
    AVIOInterruptCB       mIntCB;

    bool                  mExitBuffering;
    bool                  mIsRunning;
    pthread_t             mThread;
    pthread_mutex_t       mLock;

    Playlist_t*           mPlaylist;
    int                   mCurrentSeqNo;

	MediaObject           mCurrentInitMedia;
	int                   mCurrentInitMediaOffset;

    MediaObject           mCurrentMedia;
    int64_t               mCurrentStartPts;

	MediaObject           mCachedInitSegments[MAX_INIT_SEGMENTS];
	int                   mCachedInitSegmentCnt;

    OnDonwloadComplete_fn mCompleteCB;
    void*                 mOpaque;

    AVIOContext*          mM3u8IO;
} HLSReceiver_t;

#define _LOCK(receiver)    pthread_mutex_lock(&receiver->mLock)
#define _UNLOCK(receiver)  pthread_mutex_unlock(&receiver->mLock)

#define _INTERRUPTED(receiver) receiver->mIntCB.callback(receiver->mIntCB.opaque)

static MediaObject find_cached_init_segment(HLSReceiver_t* receiver, Segment_t* initSeg)
{
	int ii;
	for (ii = 0; ii < MAX_INIT_SEGMENTS; ii++)
	{
		MediaObject obj = receiver->mCachedInitSegments[ii];
		if (obj != NULL && MediaObject_GetSegment(obj) == initSeg)
			return obj;
	}

	return NULL;
}

static void cache_init_segment(HLSReceiver_t* receiver, Segment_t* initSeg, MediaObject obj)
{
	MediaObject cachedObj = NULL;

	cachedObj = receiver->mCachedInitSegments[receiver->mCachedInitSegmentCnt];
	if (cachedObj != NULL)
	{
		MediaObject_Delete(cachedObj);
		receiver->mCachedInitSegments[receiver->mCachedInitSegmentCnt] = NULL;
	}

	receiver->mCachedInitSegments[receiver->mCachedInitSegmentCnt++] = obj;
}

static void clear_cached_init_segment(HLSReceiver_t* receiver)
{
	int ii;

	for (ii = 0; ii < MAX_INIT_SEGMENTS; ii++)
	{
		MediaObject obj = receiver->mCachedInitSegments[ii];
		MediaObject_Delete(obj);
		receiver->mCachedInitSegments[ii] = NULL;
	}

	receiver->mCachedInitSegmentCnt = 0;
}

static int64_t default_reload_interval(Playlist_t* pls)
{
    return pls->mSegmentCnt > 0 ? pls->mSegments[pls->mSegmentCnt - 1]->mDuration :
                                  pls->mTargetDuration;
}

static int _abort_interrupt_callback(void* opaque)
{
    HLSReceiver_t* receiver = (HLSReceiver_t*)opaque;

    if (receiver->mParentIntCB && receiver->mParentIntCB->callback && receiver->mParentIntCB->callback(receiver->mParentIntCB->opaque))
        return 1;

    return receiver->mExitBuffering;
}

static void* _buffering_task_proc(void* param)
{
    HLSReceiver_t* receiver = (HLSReceiver_t*)param;
    int64_t reload_duration = 0;

    _LOCK(receiver);
    reload_duration = default_reload_interval(receiver->mPlaylist);
    _UNLOCK(receiver);

    while (!receiver->mExitBuffering)
    {
        Segment_t*   seg = NULL;
        MediaObject  obj = NULL;
        int          index = 0;

        if (_INTERRUPTED(receiver))
            break;

        _LOCK(receiver);
        if (!receiver->mPlaylist->mFinished && /* LIVE */
             get_tick() - receiver->mPlaylist->mLastLoadTime >= reload_duration)
        {
            if (HLS_M3U8_Update(receiver->mPlaylist, &receiver->mIntCB, &receiver->mM3u8IO))
            {
                LOG_ERROR("!!!!! Failed to update !!!!\n");
                if (_INTERRUPTED(receiver))
                {
                    _UNLOCK(receiver);
                    break;
                }
            }
        }

        index = receiver->mCurrentSeqNo - receiver->mPlaylist->mStartSeqNo;
        if (index < receiver->mPlaylist->mSegmentCnt)
        {    
            seg = receiver->mPlaylist->mSegments[index];
        }
        else
        {
            reload_duration = default_reload_interval(receiver->mPlaylist) / 2;
            if (receiver->mPlaylist->mFinished)
            {
//              LOG_INFO("playlist is finished and all segment is ended !!!!\n");
                _UNLOCK(receiver);
                if (MediaObjectBuffer_IsEmpty(receiver->mBuffer))
                    break;
    
                usleep(10*1000);
                continue;
            }

            _UNLOCK(receiver);
            usleep(10* 1000);
            continue;
        }
        _UNLOCK(receiver);

        if (!seg)
        {
            usleep(10 * 1000);
            continue;
        }

		/* Save Init segment to cache */
		if (seg->mInitSection)
		{
			MediaObject initObj = find_cached_init_segment(receiver, seg->mInitSection);
			if (initObj == NULL)
			{
				initObj = MediaObject_Create(seg->mInitSection, &receiver->mIntCB);
				cache_init_segment(receiver, seg->mInitSection, initObj);
				MediaObject_StartDownload(initObj);
			}
		}

        obj = MediaObject_Create(seg, &receiver->mIntCB);
        if (!obj)
        {
            LOG_ERROR("Media Object create faield !!\n");
            if (_INTERRUPTED(receiver))
                break;

            usleep(100*1000);
            continue;
        }
       
        if (MediaObject_StartDownload(obj))
        {
            LOG_ERROR("Failed to download file !!!\n");
            MediaObject_Delete(obj);
            if (_INTERRUPTED(receiver))
                break;

            usleep(100*1000);
            continue;
        }

        if (MediaObjectBuffer_Put(receiver->mBuffer, obj, -1))
        {
            LOG_ERROR("MediaObjectBuffer_Put failed !\n");
            MediaObject_Delete(obj);
            break;
        }
       
        MediaObject_WaitForEnd(obj);

        if (_INTERRUPTED(receiver))
            break;

        if (receiver->mCompleteCB)
            receiver->mCompleteCB(receiver, receiver->mPlaylist, MediaObject_GetBandwidth(obj), receiver->mOpaque);

        receiver->mCurrentSeqNo ++;
        
        _LOCK(receiver);
        reload_duration = default_reload_interval(receiver->mPlaylist);
        _UNLOCK(receiver);
    }

    MediaObjectBuffer_SetEOS(receiver->mBuffer, true);

    return NULL;
}

HLSReceiver HLS_Receiver_Create(Playlist_t* pls, AVIOInterruptCB* int_cb, OnDonwloadComplete_fn callback, void* opaque)
{
    HLSReceiver_t* receiver = (HLSReceiver_t*)malloc(sizeof(HLSReceiver_t));
    if (!receiver)
        goto ERROR;

    memset(receiver, 0x00, sizeof(HLSReceiver_t));

    if (pls->mFinished)
        receiver->mBuffer = MediaObjectBuffer_Create(VOD_SEGMENT_BUFFER_SIZE);
    else
        receiver->mBuffer = MediaObjectBuffer_Create(LIVE_SEGMENT_BUFFER_SIZE);

    if (!receiver->mBuffer)
        goto ERROR;

    receiver->mPlaylist = pls;
    if (!pls->mFinished)
        receiver->mCurrentSeqNo = pls->mStartSeqNo + FFMAX(pls->mSegmentCnt + LIVE_START_INDEX, 0);
    else
        receiver->mCurrentSeqNo = pls->mStartSeqNo;

    receiver->mParentIntCB    = int_cb;
    receiver->mIntCB.callback = _abort_interrupt_callback;
    receiver->mIntCB.opaque   = receiver;
 
    receiver->mCompleteCB = callback;
    receiver->mOpaque = opaque;

    return receiver;

ERROR:
    if (!receiver)
    {
        if (!receiver->mBuffer)
            MediaObjectBuffer_Delete(receiver->mBuffer);

        free(receiver);
    }

    return NULL;
}

int HLS_Receiver_Start(HLSReceiver receiver)
{
    int ret = 0;

    if (!receiver)
        return -1;

    receiver->mExitBuffering = false;
    MediaObjectBuffer_SetEOS(receiver->mBuffer, false);

    ret = pthread_create(&receiver->mThread, NULL, _buffering_task_proc, receiver);
    if (ret)
    {
        LOG_ERROR("pthread_create() fault.\n");
        return -1;
    }
    receiver->mIsRunning = true;

    return 0;
}

int HLS_Receiver_Stop(HLSReceiver receiver)
{
#ifdef ENABLE_DEBUG_STOP_PERFORMANCE
    int64_t startTime = get_tick();
    int64_t diff = 0;
#endif
    if (!receiver)
        return -1;

    receiver->mExitBuffering = true;
    MediaObjectBuffer_SetEOS(receiver->mBuffer, true);
    MediaObjectBuffer_Flush(receiver->mBuffer);

    if(receiver->mIsRunning)
        pthread_join(receiver->mThread, NULL);

    _LOCK(receiver);
    if(receiver->mCurrentMedia)
    {
        MediaObject_Delete(receiver->mCurrentMedia);
        receiver->mCurrentMedia = NULL;
    }
	receiver->mCurrentInitMedia = NULL;
	receiver->mCurrentInitMediaOffset = 0;
    _UNLOCK(receiver);

    receiver->mIsRunning = false;

#ifdef ENABLE_DEBUG_STOP_PERFORMANCE
    diff = get_tick() - startTime;
    LOG_TRACE("###### Stop time : [%lld] \n", diff);
#endif
    return 0;
}

void HLS_Receiver_Delete(HLSReceiver receiver)
{
    if (!receiver)
        return;

    HLS_Receiver_Stop(receiver);

    if (!receiver->mBuffer)
        MediaObjectBuffer_Delete(receiver->mBuffer);

    if (receiver->mM3u8IO)
        avio_close(receiver->mM3u8IO);

	clear_cached_init_segment(receiver);

    free(receiver);
}

int HLS_Receiver_Read(HLSReceiver receiver, unsigned char* buf, int bufLen)
{
    int ret = 0;
	int readSize = 0;

    if (!receiver)
    {
        LOG_ERROR("receiver is null !\n");
        return -1;
    }

    if (!receiver->mCurrentMedia)
    {
		Segment_t* initSegment = NULL;
        if ((ret = MediaObjectBuffer_Get(receiver->mBuffer, &receiver->mCurrentMedia, -1)) != 0)
        {
            LOG_ERROR("Failed to read ! ret = %d\n", ret);
            return HLS_SESSION_EOF;
        }
		
		initSegment = MediaObject_GetSegment(receiver->mCurrentMedia)->mInitSection;
		if (initSegment != NULL)
			receiver->mCurrentInitMedia = find_cached_init_segment(receiver, initSegment);

		receiver->mCurrentInitMediaOffset = 0;
        receiver->mCurrentStartPts = MediaObject_GetSegmentStartPts(receiver->mCurrentMedia);
    }

	if (receiver->mCurrentInitMedia)
	{
		ret = MediaObject_Peek(receiver->mCurrentInitMedia, buf, bufLen, receiver->mCurrentInitMediaOffset);
		if (ret <= 0)
		{
			receiver->mCurrentInitMedia = NULL;
			receiver->mCurrentInitMediaOffset = 0;
		}
		else
		{
			receiver->mCurrentInitMediaOffset += ret;
			readSize += ret;
		}
	}

	if (bufLen - readSize == 0)
		return readSize;
		
    ret = MediaObject_Read(receiver->mCurrentMedia, buf + readSize, bufLen - readSize);
    if (ret <= 0)
    {
        MediaObject obj = NULL;

        _LOCK(receiver);
        if(receiver->mCurrentMedia)
        {
            obj = receiver->mCurrentMedia;
            receiver->mCurrentMedia = NULL;
            receiver->mCurrentInitMedia = NULL;
			receiver->mCurrentInitMediaOffset = 0;
        }
        _UNLOCK(receiver);

        if (obj)
            MediaObject_Delete(obj);

		return ret;
    }

	readSize += ret;
    return readSize;
}

int HLS_Receiver_Seek(HLSReceiver receiver, int64_t timestamp)
{
    int ii;
    int64_t pos = 0;

    if (!receiver)
    {
        LOG_ERROR("receiver is null !\n");
        return -1;
    }

    HLS_Receiver_Stop(receiver);

    /* Calculator Current Sequence Number */
    _LOCK(receiver);
    for (ii = 0; ii < receiver->mPlaylist->mSegmentCnt; ii++)
    {
        int64_t diff = pos + receiver->mPlaylist->mSegments[ii]->mDuration - timestamp;
        if (diff > 0)
            break;

        pos += receiver->mPlaylist->mSegments[ii]->mDuration;
    }

    if (ii == receiver->mPlaylist->mSegmentCnt)
        ii = receiver->mPlaylist->mSegmentCnt - 1;

    receiver->mCurrentSeqNo = receiver->mPlaylist->mStartSeqNo + ii;
    _UNLOCK(receiver);

    HLS_Receiver_Start(receiver);

    _LOCK(receiver);
    {
        int index = receiver->mCurrentSeqNo - receiver->mPlaylist->mStartSeqNo;
        receiver->mCurrentStartPts = receiver->mPlaylist->mSegments[index]->mStartPts;
    }
    _UNLOCK(receiver);

    return 0;
}

int HLS_Receiver_SetPlaylist(HLSReceiver receiver, Playlist_t* pls)
{
    if (!receiver)
        return -1;

    LOG_INFO("!!!!!! Change Playlist !!!!!!!\n");
    _LOCK(receiver);
    receiver->mPlaylist = pls;

    /* TBD. IMPLEMENTS HERE */

    _UNLOCK(receiver);

    return 0;
}

int64_t HLS_Receiver_GetCurrentSegmentPts(HLSReceiver receiver)
{
    if (!receiver)
        return AV_NOPTS_VALUE;

    return receiver->mCurrentStartPts;
}
