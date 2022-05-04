#include "media_object.h"

#include "hls_common.h"
#include "buffered_stream.h"

#include <pthread.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "libavformat/url.h"
#include "libavformat/internal.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"

#ifdef __cplusplus
}
#endif

//#define ENABLE_TRACE_LOG

typedef enum {
    STATE_NOT_STARTED,
    STATE_STARTED,
    STATE_IN_PROGRESS,
    STATE_REQUEST_ABORT,
    STATE_ABORTED,
    STATE_COMPLETED,

    MAX_STATE
} STATE_e;

#if 0
static const char* gStrSTATE[MAX_STATE] =
{
    "NOT_STARTED",
    "STARTED",
    "IN_PROGRESS",
    "REQUEST_ABORT",
    "ABORTED",
    "COMPLETED",
};
#endif

typedef struct MediaObject_s {
    char             mURL[MAX_URL_SIZE];
	Segment_t*       mSegment;

    URLContext*      mHttpHandle;
    AVDictionary*    mOpts;
    AVIOInterruptCB* mParentIntCB;
    AVIOInterruptCB  mIntCB;

    int              mAbortFlag;
    int              mDownloadSize;
    int              mLastError;
    int64_t          mSegmentStartPts;

    BufferedStream   mStream;

    STATE_e          mState;
    pthread_t        mThread;

    pthread_mutex_t  mLock;
    pthread_cond_t   mCond;

    int64_t          mStartTime;
    int              mBandwidth;
    
    bool             mWaitForEnd;
} MediaObject_t;

#define _LOCK(obj)      pthread_mutex_lock(&obj->mLock);
#define _UNLOCK(obj)    pthread_mutex_unlock(&obj->mLock);

typedef struct KeyStore_s {
    char    mKeyURL[MAX_URL_SIZE];
    uint8_t mKey[16];
} KeyStore_t;

#define MAX_KEY_STORE_SIZE   3
static KeyStore_t gKeyStores[MAX_KEY_STORE_SIZE];
static int        gLastKeyStoreIndex = -1;

static int key_download_and_store(const char* url, AVIOInterruptCB* int_cb)
{
    uint8_t     key[16];
    KeyStore_t* store;
    int         ret;
    AVIOContext* in;

    if (!url)
    {
        LOG_ERROR("url is null\n");
        return -1;
    }

    ret = avio_open2(&in, url, AVIO_FLAG_READ, int_cb, NULL);
    if (ret < 0)
    {
        LOG_ERROR("open url is failed : %s\n", url);
        in = NULL;
        goto EXIT;
    }

    ret = avio_read(in, key, sizeof(key));
    if (ret != sizeof(key))
    {
        LOG_ERROR("Unable Download Key !!!\n");
        ret = -2;
        goto EXIT;
    }

    gLastKeyStoreIndex = (gLastKeyStoreIndex + 1) % MAX_KEY_STORE_SIZE;
    store = &gKeyStores[gLastKeyStoreIndex];

    strcpy(store->mKeyURL, url);
    memcpy(store->mKey, key, 16);
    ret = 0;

EXIT:
    if (in)
        avio_close(in);

    return ret;
}

static int get_key(const char* url, unsigned char* key, AVIOInterruptCB* int_cb)
{
    static pthread_mutex_t _CS = PTHREAD_MUTEX_INITIALIZER;
    int ii;
    KeyStore_t* store = NULL;

    pthread_mutex_lock(&_CS);

    for (ii = 0; ii < MAX_KEY_STORE_SIZE; ii++)
    {
        if (strcmp(gKeyStores[ii].mKeyURL, url) == 0)
        {
            store = &gKeyStores[ii];
            break;
        }
    }
    
    if (!store)
    {
        if (key_download_and_store(url, int_cb) != 0)
        {
            pthread_mutex_unlock(&_CS);
            return -1;
        }

        store = &gKeyStores[gLastKeyStoreIndex];
    }

    memcpy(key, store->mKey, 16);
    pthread_mutex_unlock(&_CS);

    return 0;
}

static int _abort_interrupt_callback(void* opaque)
{
    MediaObject_t* obj = (MediaObject_t*)opaque;

    if (obj->mParentIntCB && obj->mParentIntCB->callback && obj->mParentIntCB->callback(obj->mParentIntCB->opaque))
        return 1;

    return obj->mAbortFlag;
}

#if ENABLE_TRACE_LOG
static const char* strrchr2(const char* str, char c, int level)
{
	const char* p;
	const char* end = str + strlen(str) - 1;

	for (p = end -1; p > str; p--)
	{
		if (*p == c) level --;
		if (level == 0) break;
	}
	
	if (p == str)
		return NULL;

	return p;
}

#define RELURL(url)	  (strrchr2(url, '/', 1) ? strrchr2(url, '/', 1) + 1 : url) 
#define RELURL2(url)  (strrchr2(url, '/', 2) ? strrchr2(url, '/', 2) + 1 : url) 
#endif

#define BUFFER_SIZE        (32 * 1024)
static void* _http_download_proc(void* param)
{
    MediaObject_t* obj = (MediaObject_t*)param;
    int   ret = 0;
    unsigned char* buf = (unsigned char*)av_malloc(BUFFER_SIZE);

#ifdef ENABLE_TRACE_LOG
if (obj->mSegment->mSize > 0)
LOG_TRACE("[XXX] Download segment : %s, offset : %lld, size : %lld\n",RELURL2(obj->mURL), obj->mSegment->mUrlOffset, obj->mSegment->mSize);
else
LOG_TRACE("[XXX] Download segment : %s\n", RELURL2(obj->mURL));
#endif
    if (!buf)
    {
        obj->mLastError = AVERROR(ENOMEM);
        goto EXIT;
    }

    _LOCK(obj);
    if (obj->mState == STATE_REQUEST_ABORT)
    {
        _UNLOCK(obj);
        goto EXIT;
    }
    obj->mState = STATE_IN_PROGRESS;
    _UNLOCK(obj);

    do
    {
        while (1)
        {
            ret = ffurl_read(obj->mHttpHandle, buf, BUFFER_SIZE);
            if(ret != AVERROR(EAGAIN))
                break;

            usleep(20 * 1000);
        }

        _LOCK(obj);
        if (obj->mState == STATE_REQUEST_ABORT)
        {
            _UNLOCK(obj);
            goto EXIT;
        }
        _UNLOCK(obj);

        if (ret > 0)
        {
            int ret2 = BufferedStream_Write(obj->mStream, buf, ret);
            if (ret2)
            {
                LOG_ERROR("Failed to write block !\n");
                ret = 0; /* TBD. Check this code */
            }

            obj->mDownloadSize += ret;
        }
        else 
        {
            if (ret == 0)
                ret = AVERROR_EOF;

            if (ret != AVERROR_EXIT)
                obj->mLastError = ret;

            ret = 0;
        }

    }while(ret);

    obj->mBandwidth = (int64_t)obj->mDownloadSize * 8 * 1000 * 1000 / (get_tick() - obj->mStartTime);

EXIT:
    _LOCK(obj);
    if (obj->mState == STATE_REQUEST_ABORT)
        obj->mState = STATE_ABORTED;
    else
        obj->mState = STATE_COMPLETED;

    pthread_cond_signal(&obj->mCond);
    _UNLOCK(obj);

    if (buf)
        av_free(buf);

    BufferedStream_SetEOS(obj->mStream, true);

    return NULL;
}

MediaObject MediaObject_Create(Segment_t* seg, AVIOInterruptCB* int_cb)
{
    MediaObject_t* obj = (MediaObject_t*)av_mallocz(sizeof(MediaObject_t));
    if (!obj)
    {
        LOG_ERROR("obj malloc is failed !\n");
        goto ERROR;
    }
	obj->mSegment        = seg;
    obj->mParentIntCB    = int_cb;
    obj->mIntCB.callback = _abort_interrupt_callback;
    obj->mIntCB.opaque   = obj;

    if (seg->mKeyType == KEY_TYPE_AES128)
    {
        uint8_t key[16];
        char strKey[33], strIV[33];

        if (get_key(seg->mKeyURL, key, &obj->mIntCB) != 0)
            goto ERROR;

        if (strstr(seg->mURL, "://"))
            snprintf(obj->mURL, sizeof(obj->mURL), "crypto+%s", seg->mURL);
        else
            snprintf(obj->mURL, sizeof(obj->mURL), "crypto:%s", seg->mURL);

        memset(strKey, 0x00, sizeof(strKey)); 
        memset(strIV, 0x00, sizeof(strIV));

        ff_data_to_hex(strKey, key, 16, 0);

        if (seg->mIV[0] != 0)
            ff_data_to_hex(strIV, seg->mIV, 16, 0);
        
        av_dict_set(&obj->mOpts, "key", strKey, 0);
        av_dict_set(&obj->mOpts, "iv", strIV, 0);
    }
    else
    {
        av_strlcpy(obj->mURL, (const char*)seg->mURL, MAX_URL_SIZE);
        obj->mURL[MAX_URL_SIZE - 1] = 0;
    }

    if (seg->mSize >= 0)
    {
        LOG_INFO("--- setting offset : %lld,  size : %lld\n", seg->mUrlOffset, seg->mSize);
        av_dict_set_int(&obj->mOpts, "offset", seg->mUrlOffset, 0);
        av_dict_set_int(&obj->mOpts, "end_offset", seg->mUrlOffset + seg->mSize, 0);
    }

    obj->mStream = BufferedStream_Create();
    if (!obj->mStream)
        goto ERROR;

    obj->mSegmentStartPts   = seg->mStartPts;

    pthread_mutex_init(&obj->mLock, NULL);
    pthread_cond_init(&obj->mCond, NULL);

    obj->mState = STATE_NOT_STARTED;
    
    return obj;
ERROR:
    if (obj)
    {
        if (obj->mStream)
        {
            BufferedStream_Delete(obj->mStream);
            obj->mStream = NULL;
        }

        if (obj->mHttpHandle)
            ffurl_close(obj->mHttpHandle);

        av_free(obj);
    }

    return NULL;
}

static int _http_url_open(MediaObject obj)
{
    int ret = 0;
    URLContext* h = NULL;

    if(obj->mHttpHandle)
    {
        LOG_ERROR("http is already opened !!!!\n");
        return 0;
    }

    ret = ffurl_alloc(&h, obj->mURL, AVIO_FLAG_READ, &obj->mIntCB); 
    if (ret)
        goto ERROR;

    av_opt_set_dict(h->priv_data, &obj->mOpts);

    ret = ffurl_connect(h, NULL);
    if(ret < 0)
    {
        LOG_ERROR("ffurl connection failed !\n");
        goto ERROR;
    }

    /* save redirect url */
    {
        uint8_t *new_url = NULL;
        if (av_opt_get(h, "location", AV_OPT_SEARCH_CHILDREN, &new_url) >= 0)
        {
            av_strlcpy(obj->mURL, (const char*)new_url, MAX_URL_SIZE);
            obj->mURL[MAX_URL_SIZE -1] = 0;
        }
    }

    obj->mHttpHandle = h;

    return 0;

ERROR:
    if (h)
        ffurl_close(h);

    return ret;
}

int MediaObject_StartDownload(MediaObject obj)
{
    int ret = 0;

    if (!obj)
    {
        LOG_ERROR("object is null !\n");
        return -1;
    }

    _LOCK(obj);
    obj->mStartTime = get_tick();
    
    if (_http_url_open(obj))
    {
        _UNLOCK(obj);
        goto ERROR;
    }

    obj->mAbortFlag = 0;
    obj->mState = STATE_STARTED;
    BufferedStream_SetEOS(obj->mStream, false);
    _UNLOCK(obj);

    ret = pthread_create(&obj->mThread, NULL, _http_download_proc, obj);
    if(ret)
    {
        LOG_ERROR("pthread_create() fault.\n");
        goto ERROR;
    }

    return 0;
ERROR:
    _LOCK(obj);
    obj->mState = STATE_NOT_STARTED;
    _UNLOCK(obj);
    return -1;
}

int MediaObject_StopDownload(MediaObject obj)
{
    _LOCK(obj);
    if (obj->mState == STATE_NOT_STARTED)
    {
        _UNLOCK(obj);
        return -1;
    }

    obj->mAbortFlag = 1;
    BufferedStream_SetEOS(obj->mStream, true);
    if (obj->mState == STATE_STARTED || obj->mState == STATE_IN_PROGRESS)
    {
        obj->mState = STATE_REQUEST_ABORT;
    }
    _UNLOCK(obj);

    pthread_join(obj->mThread, NULL);

    _LOCK(obj);
    pthread_cond_signal(&obj->mCond);
    _UNLOCK(obj);

    BufferedStream_Flush(obj->mStream);
    
    if (obj->mHttpHandle)
    {
        ffurl_close(obj->mHttpHandle);
        obj->mHttpHandle = NULL;
    }
    return 0;
}

void MediaObject_Delete(MediaObject obj)
{
    if (!obj )
        return;

    MediaObject_StopDownload(obj);

    _LOCK(obj);
    BufferedStream_Delete(obj->mStream);
    av_dict_free(&obj->mOpts);
    _UNLOCK(obj);

    while(obj->mWaitForEnd == true)
    {
        LOG_INFO("!!!!! [TODO] W/A : wait for end issue !!!!!\n");
        usleep(10*1000);
    }
    pthread_cond_destroy(&obj->mCond);
    pthread_mutex_destroy(&obj->mLock);

    free(obj);   
}

void MediaObject_WaitForEnd(MediaObject obj)
{
    if (!obj)
        return;

    _LOCK(obj);

    if (obj->mState == STATE_STARTED || obj->mState == STATE_IN_PROGRESS)
    {
        obj->mWaitForEnd = true;
        pthread_cond_wait(&obj->mCond, &obj->mLock);
        obj->mWaitForEnd = false;
    }
    _UNLOCK(obj);
}

int MediaObject_Read(MediaObject obj, unsigned char* buf, int bufLen)
{
    int ret;
    if (!obj )
        return -1;

#ifdef ENABLE_TRACE_LOG
LOG_TRACE("[XXX] Read from %s, size : %d\n", RELURL2(obj->mURL), bufLen);
#endif

    ret = BufferedStream_Read(obj->mStream, buf, bufLen);
    if (ret == 0)
        ret = obj->mLastError;

    return ret;
}

int MediaObject_Peek(MediaObject obj, unsigned char* buf, int bufLen, int offset)
{
    if (!obj )
    {
        LOG_ERROR("obj is null !\n");
        return -1;
    }
#ifdef ENABLE_TRACE_LOG
LOG_TRACE("[XXX] Peek from %s, offset : %d, size : %d\n", RELURL2(obj->mURL), offset, bufLen);
#endif
    return BufferedStream_Peek(obj->mStream, buf, bufLen, offset);
}

int MediaObject_GetBandwidth(MediaObject obj)
{
    if (!obj )
    {
        LOG_ERROR("obj is null !\n");
        return -1;
    }

    return obj->mBandwidth;
}

Segment_t* MediaObject_GetSegment(MediaObject obj)
{
    if (!obj )
    {
        LOG_ERROR("obj is null !\n");
        return NULL;
    }

    return obj->mSegment;
}
int64_t MediaObject_GetSegmentStartPts(MediaObject obj)
{
    if (!obj )
    {
        LOG_ERROR("obj is null !\n");
        return -1;
    }

    return obj->mSegmentStartPts;
}
