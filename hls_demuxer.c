#include <string.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libavformat/internal.h"
#include "libavformat/avio_internal.h"
#include "libavutil/avstring.h"

#ifdef __cplusplus
}
#endif

#include "hls_common.h"
#include "hls_receiver.h"
#include "m3u8_parser.h"
#include "util.h"
#include "hls_log.h"

static AVRational g_Rational = {1, AV_TIME_BASE};

typedef struct StreamInfo_s {
    int         mId;
    int64_t     mOrignStartPts;
    int64_t     mSegmentStartPts;
} StreamInfo_t;

typedef struct SessionContext_s {
    int               mEOF;

    HLSReceiver       mReceiver;

    AVFormatContext*  mContext;
    AVIOContext       mIO;
    unsigned char*    mBuffer;
    int               mBufferSize; 
    
    StreamInfo_t**    mStreamInfos;
    int               mStreamInfoCnt;

    int64_t           mSeekTimestamp;
    int64_t           mSeekStreamIndex;
    AVPacket          mPkt; 

#ifdef ENABLE_DEBUG_DROP_COUNT
    int               mDropCnt;
#endif
}SessionContext_t;

typedef struct HLSContext_s {
    AVClass*           mClass;

    AVIOInterruptCB*   mIntCB;

    HLSInfo_t          mInfo;
    int                mVariantIndex;
    int                mManualVariantIndex;  /* -1 means auto, otherwise manual */

    SessionContext_t** mSessions;
    int                mSessionCnt;

    int                mStreamCnt;

    int                mCodecBufLevel;
    int                mCodecVideoBufSize;
    int                mCodecAudioBufSize;
    int                mCodecVideoDataSize;
    int                mCodecAudioDataSize;

    pthread_mutex_t    mLock;

	int                mProbe; // During probing media, No need to change adaptive.
 
	bool               mIsSegmentChanged;
} HLSContext_t;

static void avio_reset2(AVIOContext* io)
{
    io->eof_reached = 0;
    io->buf_end = io->buf_ptr = io->buffer;
    io->pos = 0;
    io->error = 0;
    io->seekable = 0;
	io->is_segment_media = 1;
}

static void reset_packet(AVPacket* pkt)
{
    av_init_packet(pkt);
    pkt->data = NULL;
}

static void add_metadata_from_renditions(AVStream* st, Playlist_t* pls)
{
    int rend_idx = 0;
    int ii;

    for (ii = 0; ii < pls->mRenditionCnt; ii++)
    {
        Rendition_t* rend = pls->mRenditions[rend_idx];
        if (st->codecpar->codec_type != rend->mType)
            continue;

        if (rend->mLanguage[0])
            av_dict_set(&st->metadata, "language", rend->mLanguage, 0);
        if (rend->mName[0])
            av_dict_set(&st->metadata, "comment", rend->mName, 0);

        st->disposition |= rend->mDisposition;
    }
}

static int hls_switch_variant(HLSContext_t* c, HLSReceiver receiver, Playlist_t* pls, int bandwidth)
{
    Variant_t* curVar = c->mInfo.mVariants[c->mVariantIndex];

    int ii, diff;
    int newVariantIndex   = -1;

	if (c->mProbe == 1)
		return 0;

    for (ii = 0; ii < c->mInfo.mVariantCnt; ii++)
    {
		if (c->mInfo.mVariants[ii]->mBandwidth < bandwidth)
		{
			if (newVariantIndex == -1 || diff > bandwidth - c->mInfo.mVariants[ii]->mBandwidth)
			{
				diff = bandwidth - c->mInfo.mVariants[ii]->mBandwidth;
				newVariantIndex = ii;
			}
		}
	}

    if (newVariantIndex >= 0 && c->mVariantIndex != newVariantIndex)
    {
        Variant_t* nextVar = c->mInfo.mVariants[newVariantIndex];
        c->mVariantIndex = newVariantIndex;
       
#ifdef ENABLE_DEBUG_ADAPTIVE_INFO
        LOG_ERROR("Change Adaptive - BandWidth: %d ->%d\n", curVar->mBandwidth, nextVar->mBandwidth);
#endif
        HLS_Receiver_SetPlaylist(receiver, nextVar->mPlaylists[0]); /* Main Stream  */
       
        return 0;
    }

    return -1;    
}

static int IORead(void *opaque, uint8_t *buf, int buf_size)
{
    int ret;
    SessionContext_t* session = (SessionContext_t*)opaque;
    if (!session)
        return 0;

    ret =  HLS_Receiver_Read(session->mReceiver, buf, buf_size);
#ifdef ENABLE_READ_DATA_DUMP
	LOG_INFO("--- ret : %d\n", ret);
	if (ret >= 0)
		HLS_LOG_Dump(LOG_LEVEL_NONE, buf, buf_size > 32 ? 32:buf_size);
#endif

    return ret;
}

static void download_complete_callback(HLSReceiver receiver, Playlist_t* pls, int bandwidth, void* opaque)
{
    HLSContext_t* c = (HLSContext_t*)opaque;

    Variant_t* curVar = c->mInfo.mVariants[c->mVariantIndex];
    LOG_TRACE("@@@@@@@ Downlaod Completed : Variant : %d, Bandwidth : %d, MeasureBandwidth %d @@@@@@@\n", c->mVariantIndex, curVar->mBandwidth, bandwidth);

    hls_switch_variant(c, receiver, pls, bandwidth);
}

static SessionContext_t* hls_session_open(AVFormatContext* s, Playlist_t* pls, int isMainStream)
{
    int ret = 0;
    int ii;
	ff_const59 AVInputFormat *in_fmt = NULL;
    HLSContext_t* c = (HLSContext_t*)s->priv_data;
    SessionContext_t* session = (SessionContext_t*)av_mallocz(sizeof(SessionContext_t));
    if (!session)
    {
        LOG_ERROR("failed to alloc session !\n");
        goto ERROR;
    }

    if (isMainStream)
        session->mReceiver = HLS_Receiver_Create(pls, c->mIntCB, download_complete_callback, c);
    else
        session->mReceiver = HLS_Receiver_Create(pls, c->mIntCB, NULL, c);

    if (!session->mReceiver)
    {
        LOG_ERROR("failed to create receiver !\n");
        goto ERROR;
    }

    HLS_Receiver_Start(session->mReceiver);

    session->mBuffer = (unsigned char*)av_malloc(INITIAL_BUFFER_SIZE);
    if (!session->mBuffer)
    {
        LOG_ERROR("failed to alloc buffer !\n");
        goto ERROR;
    }
    session->mBufferSize = INITIAL_BUFFER_SIZE;

	session->mContext = avformat_alloc_context();

    ffio_init_context(&session->mIO, session->mBuffer, session->mBufferSize, 0, session, IORead, NULL, NULL); 
    session->mIO.seekable = 0;
	session->mIO.is_segment_media = 1;

	ff_copy_whiteblacklists(session->mContext, s);

	session->mContext->flags = AVFMT_FLAG_CUSTOM_IO;
	session->mContext->probesize = s->probesize > 0 ? s->probesize : 1024 * 4;
    session->mContext->max_analyze_duration = s->max_analyze_duration > 0 ? s->max_analyze_duration : 4 * AV_TIME_BASE;
    ret = av_probe_input_buffer(&session->mIO, &in_fmt, "", NULL, 0, 0);
	if (ret < 0)
	{
		LOG_ERROR("failed to probe input buffer !\n");
	}

    session->mSeekTimestamp = AV_NOPTS_VALUE;
    session->mSeekStreamIndex = -1;
    reset_packet(&session->mPkt);

    dynarray_add(&c->mSessions, &c->mSessionCnt, session);

    session->mContext->pb = &session->mIO;

    ret = avformat_open_input(&session->mContext, "", in_fmt, NULL);
    if (ret < 0)
    {
        LOG_ERROR("Open avformat input failed !!\n");

        if (session->mContext)
            avformat_close_input(&session->mContext);
        goto ERROR;
    }

    ret = avformat_find_stream_info(session->mContext, NULL);
    for (ii = 0; ii < session->mContext->nb_streams; ii++)
    {
        StreamInfo_t* streamInfo = NULL;
        AVStream*     st = avformat_new_stream(s, NULL);
        AVStream*     ist = session->mContext->streams[ii];

		st->id = c->mStreamCnt;
        st->time_base.num = g_Rational.num;
        st->time_base.den = g_Rational.den;
        st->r_frame_rate.num = ist->r_frame_rate.num;
        st->r_frame_rate.den = ist->r_frame_rate.den;

        avcodec_parameters_copy(st->codecpar, ist->codecpar);

        streamInfo = (StreamInfo_t*)av_malloc(sizeof(StreamInfo_t));
        streamInfo->mId              = st->index;
        streamInfo->mOrignStartPts   = -1;
        streamInfo->mSegmentStartPts = HLS_Receiver_GetCurrentSegmentPts(session->mReceiver);

        dynarray_add(&session->mStreamInfos,  &session->mStreamInfoCnt, streamInfo);

        add_metadata_from_renditions(st, pls);

        c->mStreamCnt++;
    }

    goto EXIT;
ERROR:
    if (session)
    {
        if (!session->mReceiver)
            HLS_Receiver_Delete(session->mReceiver);

        if (session->mContext)
            av_free(session->mContext);

        free(session);
        session = NULL;
    }

EXIT:
    return session;
}

static int hls_session_next_segment(AVFormatContext* s, SessionContext_t* session)
{
    int ret;
    int ii;
    AVFormatContext* newContext = NULL;

__TRACE_ENTER__;
    avio_reset2(&session->mIO);
    reset_packet(&session->mPkt);

    if (!(newContext = avformat_alloc_context()))
    {
        return AVERROR(ENOMEM);
    }
    newContext->pb = &session->mIO;
    ret = avformat_open_input(&newContext, "", NULL, NULL);
    if (ret < 0)
    {
        if (newContext)
            avformat_close_input(&newContext);

        LOG_ERROR("failed to open input ! ret = %d\n", ret);
        return ret;
    }

    /* Change context */
    if (session->mContext)
    {
        avformat_close_input(&session->mContext);
        session->mContext = NULL;
    }
    session->mContext = newContext;

    ret = avformat_find_stream_info(session->mContext, NULL);
    for (ii = 0; ii < session->mContext->nb_streams; ii++)
    {
        AVStream* st = s->streams[session->mStreamInfos[ii]->mId];
        AVStream* ist = session->mContext->streams[ii];

        if (ist->codecpar->extradata_size > 0 && ist->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            uint8_t *e;

            if (st->codecpar->extradata)
                av_free(st->codecpar->extradata);

            e = (uint8_t *)av_malloc(ist->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!e)
            {
                LOG_ERROR("Alloc extra data is failed !\n");
                return AVERROR(ENOMEM);
            }
            memcpy(e, ist->codecpar->extradata, ist->codecpar->extradata_size);
            st->codecpar->extradata = e;
            st->codecpar->extradata_size = ist->codecpar->extradata_size;
        }
        session->mStreamInfos[ii]->mSegmentStartPts = HLS_Receiver_GetCurrentSegmentPts(session->mReceiver);
        session->mStreamInfos[ii]->mOrignStartPts = -1;
    }

    return 0;
}

static void hls_session_close(AVFormatContext* s, SessionContext_t* session)
{
    if (session->mReceiver)
    {
        HLS_Receiver_Delete(session->mReceiver);
        session->mReceiver = NULL;
    }

    if (session->mContext)
    {
        avformat_close_input(&session->mContext);
        session->mContext = NULL;

#if 0 // TBD. Crash, check it 
        av_free(session->mBuffer);
#endif
        session->mBuffer = NULL;
        session->mBufferSize = 0;
    }

    // TBD. Check it !!
    // avio_close(&session->mIO);
}

static int hls_close(AVFormatContext *s)
{
    HLSContext_t* c = (HLSContext_t*)s->priv_data;
    int ii;

    for (ii = 0; ii < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];
        hls_session_close(s, session);
    }

    // TBD. IMPLEMENTS HERE

    HLS_M3U8_Delete(&c->mInfo);
    pthread_mutex_destroy(&c->mLock);

    return 0;
}

static int hls_change_playlist_manually(HLSContext_t* c)
{
    Variant_t* var;
    int ii, jj;
    int ret;
    
    if (c->mManualVariantIndex < 0)
        return 0;

    if (c->mManualVariantIndex >= c->mInfo.mVariantCnt)
        c->mManualVariantIndex = c->mInfo.mVariantCnt -1;

    if (c->mManualVariantIndex == c->mVariantIndex)
        return 0;

    var = c->mInfo.mVariants[c->mManualVariantIndex];

    for (ii = 0; ii < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];
        for (jj = 0; jj < var->mPlaylistCnt; jj++)
        {            
            Playlist_t* pls = var->mPlaylists[ii];
            if ((ret = HLS_Receiver_SetPlaylist(session->mReceiver, pls)) != 0)
            {
                LOG_ERROR("Cannot Set playlist ! session : %d, playlist : %d, ret : %d\n",  jj, jj, ret);
            }
        }
    }

    return 0;
}

static int hls_read_header(AVFormatContext* s)
{
    HLSContext_t* c = (HLSContext_t*)s->priv_data;
    int ii;
    int ret = 0;
    pthread_mutexattr_t attr;

    c->mIntCB = &s->interrupt_callback;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->mLock, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Download and Parse HLS M3U8 file */
    do {
        ret = HLS_M3U8_Parse(&c->mInfo, s->url, c->mIntCB, NULL);
        if (ret < 0)
        {
            LOG_ERROR("cannot parse M3U8 !\n");
            return ret;
        }
        
        HLS_M3U8_Dump(&c->mInfo);
    } while (0);

    /* Calculate total duration */
    do {
        int64_t duration = 0;
        Playlist_t* pls = c->mInfo.mVariants[0]->mPlaylists[0];
        if(pls && pls->mFinished)
        {
            for (ii = 0; ii < pls->mSegmentCnt; ii++)
                duration += pls->mSegments[ii]->mDuration;

            s->duration = duration;
        }
    } while (0);

    do {
        Variant_t* var = NULL;
        
        if (c->mManualVariantIndex < 0 )
            c->mVariantIndex = 0;
            
        else if (c->mManualVariantIndex >= c->mInfo.mVariantCnt)
        {
            c->mManualVariantIndex = c->mInfo.mVariantCnt -1;
            c->mVariantIndex = c->mManualVariantIndex;
        }

        var = c->mInfo.mVariants[c->mVariantIndex];

		c->mProbe = 1;
        for (ii = 0; ii < var->mPlaylistCnt; ii++)
        {
            Playlist_t* playlist = var->mPlaylists[ii];
            hls_session_open(s, playlist, ii == 0);
        }
		c->mProbe = 0;

#ifdef ENABLE_DEBUG_ADAPTIVE_INFO
        LOG_ERROR("Set Adaptive - BandWidth: %d\n", var->mBandwidth);
#endif
    } while (0);
__TRACE_LEAVE__;
    return 0;
}

static int hls_read_packet(AVFormatContext *s, AVPacket *out_pkt)
{
    HLSContext_t* c = (HLSContext_t*)s->priv_data;
    int ii, ret;
    int minPtsIndex = -1;

//    hls_change_playlist_manually(c);

    pthread_mutex_lock(&c->mLock);

    for (ii = 0; ii < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];
       
        if (session->mEOF)
            continue;

        if (!session->mPkt.data)
        {
            while (1)
            {
                ret = av_read_frame(session->mContext, &session->mPkt);
                if (ret < 0) /* failed to read frame */
                {
                    if (ret == AVERROR(EAGAIN))
                    {
                        LOG_INFO("EAGAIN - retry read frame !\n");
                        continue;
                    }
                    else if (ret == AVERROR_EOF)
                    {
                        LOG_INFO("AVERROR_EOF - session index : %d\n", ii);
                        if ((ret = hls_session_next_segment(s, session)) == 0)
                        {
							c->mIsSegmentChanged = true;	
                            LOG_TRACE("####### session : %d SegmentPts : %lld\n", ii, session->mStreamInfos[0]->mSegmentStartPts);
                        }
                        else
                        {
                            LOG_INFO("hls_session_next_segment failed (%d)! end session : index %d\n", ret, ii);
                            session->mEOF = 1;
                            break;
                        }
                        continue;
                    }
                    else if (ret == HLS_SESSION_EOF)
                    {
                        LOG_INFO("HLS_SESSION_EOF - end session : index %d\n", ii);
                        session->mEOF = 1;
                        break;
                    }
                    else if (ret == AVERROR_EXIT)
                    {
                        LOG_INFO("AVERROR_EXIT - end play\n");
                        goto EXIT;
                    }

                    LOG_ERROR("session terminated with error : %d\n", ret);
                    goto EXIT;
                }
                else /* Successed to read frame */
                {
                    AVPacket* pkt = &session->mPkt;
                    AVStream* ist = session->mContext->streams[pkt->stream_index];

					if (c->mIsSegmentChanged)
					{
						pkt->flags = AV_PKT_FLAG_SEGMENT_CHANGED;
						c->mIsSegmentChanged = false;
					}

                    if (pkt->pts != AV_NOPTS_VALUE)
                        pkt->pts = av_rescale_q(pkt->pts, ist->time_base, g_Rational);
                    if (pkt->dts != AV_NOPTS_VALUE)
                        pkt->dts = av_rescale_q(pkt->dts, ist->time_base, g_Rational);
                }

                if (session->mSeekTimestamp == AV_NOPTS_VALUE)
                {
#ifdef ENABLE_DEBUG_DROP_COUNT
                    if (session->mDropCnt > 0)
                    {
                        LOG_TRACE("@@@@@@@@@ DROP PKT : Session : %d, Cnt : %d\n", ii, session->mDropCnt);
                        session->mDropCnt = 0;
                    }
#endif
                    break;
                }

                if (session->mSeekStreamIndex < 0  || session->mSeekStreamIndex == session->mPkt.stream_index)
                {
                    if (session->mPkt.dts == AV_NOPTS_VALUE)
                    {
                        session->mSeekTimestamp = AV_NOPTS_VALUE;
                        break;
                    }

                    if (session->mPkt.dts >= session->mSeekTimestamp 
                        /* && (session->mPkt.flags & AV_PKT_FLAG_KEY) */) // TBD. Check it !!!!
                    {
                        session->mSeekTimestamp = AV_NOPTS_VALUE;
                        break;
                    }
                }
#ifdef ENABLE_DEBUG_DROP_COUNT
                session->mDropCnt ++;
#endif
                av_packet_unref(&session->mPkt);
            }

            if (!session->mPkt.data)
                continue;
        }
    }

    for (ii = 0; ii  < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];
        if (session->mEOF == 1)
            continue;

        if (!session->mPkt.data)
            continue;

        if (minPtsIndex == -1)
            minPtsIndex = ii;
        else
        {
            int64_t dts    = session->mPkt.dts;
            int64_t mindts = c->mSessions[minPtsIndex]->mPkt.dts;

            if (dts == AV_NOPTS_VALUE || av_compare_mod(dts, mindts, 1LL << 33) < 0)
                minPtsIndex = ii;
        }
    }
    if (minPtsIndex == -1)
    {
        LOG_INFO("minPtsIndex is -1\n");
        ret = AVERROR_EOF;
        goto EXIT;
    }

    *out_pkt = c->mSessions[minPtsIndex]->mPkt;
    out_pkt->stream_index = c->mSessions[minPtsIndex]->mStreamInfos[out_pkt->stream_index]->mId;
    reset_packet(&c->mSessions[minPtsIndex]->mPkt);
    ret = 0;

EXIT:
    pthread_mutex_unlock(&c->mLock);

    return ret;
}

#ifdef ENABLE_SEGMENT_SEEK
static int64_t get_seek_timestamp_of_main_stream(HLSContext_t* c, int64_t timestamp)
{
	int ii;
	Segment_t* segToSeek = NULL;
	Variant_t* var = c->mInfo.mVariants[c->mVariantIndex];
	
	Playlist_t* pls = var->mPlaylists[0]; // Main Stream
	segToSeek = pls->mSegments[0];

	for (ii = 0; ii < pls->mSegmentCnt; ii++)
	{
		segToSeek = pls->mSegments[ii];

		if (segToSeek->mStartPts + segToSeek->mDuration > timestamp)
			break;
	}

	return segToSeek->mStartPts;
}
#endif

static int hls_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    HLSContext_t* c = (HLSContext_t*)s->priv_data;
    int ii, jj, ret;
    int64_t seek_timestamp;
    SessionContext_t* session_to_seek = NULL;
    int stream_subdemuxer_index = 0;

    pthread_mutex_lock(&c->mLock);

    seek_timestamp = av_rescale_rnd(timestamp, AV_TIME_BASE, s->streams[stream_index]->time_base.den, 
                                    flags & AVSEEK_FLAG_BACKWARD ? AV_ROUND_DOWN : AV_ROUND_UP);

    LOG_INFO("stream_index : %d, seek_timestamp : %lld\n", stream_index, seek_timestamp);

#ifdef ENABLE_SEGMENT_SEEK
	seek_timestamp = get_seek_timestamp_of_main_stream(c, seek_timestamp);
#endif

    for (ii = 0; ii < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];
        LOG_INFO("ii : %d, session : %p\n", ii, session);

        for (jj = 0; jj < session->mContext->nb_streams; jj++)
        {
            StreamInfo_t* streamInfo = session->mStreamInfos[jj];
            if (streamInfo->mId == stream_index)
            {
                session_to_seek = session;
                stream_subdemuxer_index = jj;
                break;
            }
        }
    }

    if (!session_to_seek)
    {
        LOG_ERROR("Cannot found seek session !!!!\n");
        ret = AVERROR(EIO);
        goto EXIT;
    }

    for (ii = 0; ii < c->mSessionCnt; ii++)
    {
        SessionContext_t* session = c->mSessions[ii];

        av_packet_unref(&session->mPkt);

        avio_reset2(&session->mIO);
        session->mEOF = 0;
#ifdef ENABLE_DEBUG_DROP_COUNT
        session->mDropCnt = 0;
#endif

        ff_read_frame_flush(session->mContext);
        session->mSeekTimestamp = seek_timestamp;

        if (session_to_seek == session)
            session->mSeekStreamIndex = stream_subdemuxer_index;
        else
            session->mSeekStreamIndex = -1;

        HLS_Receiver_Seek(session->mReceiver, seek_timestamp);

        hls_session_next_segment(s, session);
    }
    ret = 0;

EXIT:
    pthread_mutex_unlock(&c->mLock);
    return ret;
}

static int hls_probe(const AVProbeData *p)
{
   const char* buf = (const char*)p->buf;

   if (strncmp(buf, "#EXTM3U", 7))
        return 0;

    if (strstr(buf, "#EXT-X-STREAM-INF:")     ||
        strstr(buf, "#EXT-X-TARGETDURATION:") ||
        strstr(buf, "#EXT-X-MEDIA-SEQUENCE:"))
        return AVPROBE_SCORE_MAX;

    return 0;
}

#define OFFSET(x) offsetof(HLSContext_t, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption hls_options[] = {
    {"manual_index", "manual index to select variant index, -1 mean auto", OFFSET(mManualVariantIndex), AV_OPT_TYPE_INT, {.i64 = 3}, 0, INT_MAX, FLAGS},
    {"codec_buf_level",       "setting codec buffer level",    OFFSET(mCodecBufLevel),      AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"codec_video_buf_size",  "setting codec video buf size",  OFFSET(mCodecVideoBufSize),  AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"codec_audio_buf_size",  "setting codec audio buf size",  OFFSET(mCodecAudioBufSize),  AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"codec_video_data_size", "setting codec video data size", OFFSET(mCodecVideoDataSize), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"codec_audio_data_size", "setting codec audio data size", OFFSET(mCodecAudioDataSize), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {NULL}
};

static const AVClass hls_class = {
    .class_name = "hls demuxer",
    .item_name  = av_default_item_name,
    .option     = hls_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_hls_demuxer = 
{
    .name           = "hls",
    .long_name      = NULL_IF_CONFIG_SMALL("Apple HTTP Live Streaming"),
    .priv_class     = &hls_class,
    .priv_data_size = sizeof(HLSContext_t),
	.flags          = AVFMT_NOGENSEARCH | AVFMT_TS_DISCONT | AVFMT_NO_BYTE_SEEK,
    .read_probe     = hls_probe,
    .read_header    = hls_read_header,
    .read_packet    = hls_read_packet,
    .read_close     = hls_close,
    .read_seek      = hls_read_seek,
};
