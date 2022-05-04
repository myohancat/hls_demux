// Reference parse code from ffmpeg hls.c

#include "m3u8_parser.h"

#include "hls_common.h"

#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libavformat/internal.h"
#include "libavformat/http.h"

#ifdef __cplusplus
}
#endif

static Playlist_t* find_playlist(HLSInfo_t* info, const char* absURL)
{
	int ii;

	for (ii = 0; ii < info->mPlaylistCnt; ii++)
	{
		if (strcmp(info->mPlaylists[ii]->mURL, absURL) == 0)
			return info->mPlaylists[ii];
	}

	return NULL;
}

static Playlist_t* new_playlist(HLSInfo_t* info, const char* url, const char* base)
{
    char absURL[MAX_URL_SIZE];
    Playlist_t* pls;
	
	ff_make_absolute_url(absURL, sizeof(absURL), base, url);

	pls = find_playlist(info, absURL);
	if (pls != NULL)
		return pls;
	
	pls = (Playlist_t*)av_mallocz(sizeof(Playlist_t));
    if (!pls)
        return NULL;

    av_strlcpy(pls->mURL, absURL, sizeof(pls->mURL));
   
    dynarray_add(&info->mPlaylists, &info->mPlaylistCnt, pls);
 
    return pls;
}

static void free_segment_from_playlist(Playlist_t* pls)
{
    int ii;
    for (ii = 0; ii < pls->mSegmentCnt; ii++)
    {
        Segment_t* seg = pls->mSegments[ii];
        av_freep(&seg->mKeyURL);
        av_freep(&seg->mURL);
        av_freep(&seg);
    }

    av_freep(&pls->mSegments);
    pls->mSegmentCnt = 0;
}


typedef struct VariantInfo_s {
    char mBandwidth[20];
    /* variant group ids: */
    char mAudioGroup[MAX_FIELD_LEN];
    char mVideoGroup[MAX_FIELD_LEN];
    char mSubtitleGroup[MAX_FIELD_LEN];
} VariantInfo_t;

static void handle_variant_args(VariantInfo_t* info, const char *key, int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "BANDWIDTH=", key_len))
    {
        *dest     =        info->mBandwidth;
        *dest_len = sizeof(info->mBandwidth);
    }
    else if (!strncmp(key, "AUDIO=", key_len))
    {
        *dest     =        info->mAudioGroup;
        *dest_len = sizeof(info->mAudioGroup);
    }
    else if (!strncmp(key, "VIDEO=", key_len))
    {
        *dest     =        info->mVideoGroup;
        *dest_len = sizeof(info->mVideoGroup);
    }
    else if (!strncmp(key, "SUBTITLES=", key_len))
    {
        *dest     =        info->mSubtitleGroup;
        *dest_len = sizeof(info->mSubtitleGroup);
    }
}

static Variant_t* new_variant(HLSInfo_t* info, const VariantInfo_t* variantInfo, const char *url, const char *base)
{
    Variant_t* variant = NULL;

    Playlist_t* pls = NULL;

    pls = new_playlist(info, url, base);
    if (!pls)
        return NULL;

    variant = (Variant_t*)av_mallocz(sizeof(Variant_t));
    if (!variant)
        return NULL;

    if (variantInfo)
    {
        variant->mBandwidth = atoi(variantInfo->mBandwidth);
        strcpy(variant->mAudioGroup,    variantInfo->mAudioGroup);
        strcpy(variant->mVideoGroup,    variantInfo->mVideoGroup);
        strcpy(variant->mSubtitleGroup, variantInfo->mSubtitleGroup);
    }

    dynarray_add(&info->mVariants, &info->mVariantCnt, variant);
    dynarray_add(&variant->mPlaylists, &variant->mPlaylistCnt, pls);

    return variant;
}

typedef struct KeyInfo_s {
    char       mURI[MAX_URL_SIZE];
    char       mMethod[11];
    char       mIV[35];
} KeyInfo_t;

static void handle_key_args(KeyInfo_t* info, const char* key, int key_len, char** dest, int* dest_len)
{
    if (!strncmp(key, "METHOD=", key_len))
    {
        *dest     =        info->mMethod;
        *dest_len = sizeof(info->mMethod);
    }
    else if (!strncmp(key, "URI=", key_len))
    {
        *dest     =        info->mURI;
        *dest_len = sizeof(info->mURI);
    }
    else if (!strncmp(key, "IV=", key_len))
    {
        *dest     =        info->mIV;
        *dest_len = sizeof(info->mIV);
    }
}

typedef struct RenditionInfo_s {
    char mType[16]; 
    char mURI[MAX_URL_SIZE]; 
    char mGroupId[MAX_FIELD_LEN]; 
    char mLanguage[MAX_FIELD_LEN]; 
    char mAssocLanguage[MAX_FIELD_LEN]; 
    char mName[MAX_FIELD_LEN]; 
    char mDefaultr[4]; 
    char mForced[4]; 
    char mCharacteristics[MAX_CHARACTERISTICS_LEN]; 
} RenditionInfo_t;

static void handle_rendition_args(RenditionInfo_t* info, const char* key, int key_len, char** dest, int* dest_len) 
{ 
    if (!strncmp(key, "TYPE=", key_len))
    { 
        *dest     =        info->mType; 
        *dest_len = sizeof(info->mType); 
    }
    else if (!strncmp(key, "URI=", key_len))
    { 
        *dest     =        info->mURI; 
        *dest_len = sizeof(info->mURI); 
    }
    else if (!strncmp(key, "GROUP-ID=", key_len))
    { 
        *dest     =        info->mGroupId; 
        *dest_len = sizeof(info->mGroupId); 
    }
    else if (!strncmp(key, "LANGUAGE=", key_len))
    { 
        *dest     =        info->mLanguage; 
        *dest_len = sizeof(info->mLanguage); 
    }
    else if (!strncmp(key, "ASSOC-LANGUAGE=", key_len))
    { 
        *dest     =        info->mAssocLanguage; 
        *dest_len = sizeof(info->mAssocLanguage); 
    }
    else if (!strncmp(key, "NAME=", key_len))
    { 
        *dest     =        info->mName; 
        *dest_len = sizeof(info->mName); 
    }
    else if (!strncmp(key, "DEFAULT=", key_len))
    { 
        *dest     =        info->mDefaultr; 
        *dest_len = sizeof(info->mDefaultr); 
    }
    else if (!strncmp(key, "FORCED=", key_len))
    { 
        *dest     =        info->mForced; 
        *dest_len = sizeof(info->mForced); 
    }
    else if (!strncmp(key, "CHARACTERISTICS=", key_len))
    { 
        *dest     =        info->mCharacteristics; 
        *dest_len = sizeof(info->mCharacteristics); 
    } 
    /* 
     * ignored: 
     * - AUTOSELECT: client may autoselect based on e.g. system language 
     * - INSTREAM-ID: EIA-608 closed caption number ("CC1".."CC4") 
     */ 
}

static Rendition_t* new_rendition(HLSInfo_t* info, RenditionInfo_t* rendInfo, const char* url_base)
{
    Rendition_t* rend;
    enum AVMediaType eType = AVMEDIA_TYPE_UNKNOWN;
    char *characteristic;
    char *chr_ptr;
    char *saveptr;

printf("--- new_rendition : %s\n", rendInfo->mURI);
    if (!strcmp(rendInfo->mType, "AUDIO"))
        eType = AVMEDIA_TYPE_AUDIO;
    else if (!strcmp(rendInfo->mType, "VIDEO"))
        eType = AVMEDIA_TYPE_VIDEO;
    else if (!strcmp(rendInfo->mType, "SUBTITLES"))
        eType = AVMEDIA_TYPE_SUBTITLE;
    else if (!strcmp(rendInfo->mType, "CLOSED-CAPTIONS"))
	{
        /* CLOSED-CAPTIONS is ignored since we do not support CEA-608 CC in
         * AVC SEI RBSP anyway */
	}

    if (eType == AVMEDIA_TYPE_UNKNOWN)
    {
        LOG_ERROR("Can't support the type: %s\n", rendInfo->mType);
        return NULL;
    }

    /* URI is mandatory for subtitles as per spec */
    if (eType == AVMEDIA_TYPE_SUBTITLE && !rendInfo->mURI[0]) {
        LOG_ERROR("The URI tag is REQUIRED for subtitle.\n");
        return NULL;
    }

	if (eType == AVMEDIA_TYPE_SUBTITLE) {
		LOG_ERROR("Can't support the subtitle(uri: %s)\n", rendInfo->mURI);
		return NULL;
	}

    rend = (Rendition_t*)av_mallocz(sizeof(Rendition_t));
    if (!rend)
        return NULL;

    dynarray_add(&info->mRenditions, &info->mRenditionCnt, rend);

    rend->mType = eType;
    strcpy(rend->mGroupId, rendInfo->mGroupId);
    strcpy(rend->mLanguage, rendInfo->mLanguage);
    strcpy(rend->mName, rendInfo->mName);

    /* add the playlist if this is an external rendition */
    if (rendInfo->mURI[0])
    {
        rend->mPlaylist = new_playlist(info, rendInfo->mURI, url_base);
        if (rend->mPlaylist)
            dynarray_add(&rend->mPlaylist->mRenditions, &rend->mPlaylist->mRenditionCnt, rend);
    }

    if (rendInfo->mAssocLanguage[0])
    {
        int langlen = strlen(rend->mLanguage);
        if (langlen < sizeof(rend->mLanguage) - 3) {
            rend->mLanguage[langlen] = ',';
            av_strlcpy(rend->mLanguage + langlen + 1, rendInfo->mAssocLanguage, sizeof(rend->mLanguage) - langlen - 2);
        }
    }

#if 1 /* TBD. Check It */
    if (!strcmp(rendInfo->mDefaultr, "YES"))
        rend->mDisposition |= AV_DISPOSITION_DEFAULT;
    if (!strcmp(rendInfo->mForced, "YES"))
        rend->mDisposition |= AV_DISPOSITION_FORCED;

    chr_ptr = rendInfo->mCharacteristics;
    while ((characteristic = strtok_r(chr_ptr, ",", &saveptr)))
    {
        if (!strcmp(characteristic, "public.accessibility.describes-music-and-sound"))
            rend->mDisposition |= AV_DISPOSITION_HEARING_IMPAIRED;
        else if (!strcmp(characteristic, "public.accessibility.describes-video"))
            rend->mDisposition |= AV_DISPOSITION_VISUAL_IMPAIRED;

        chr_ptr = NULL;
    }
#endif

    return rend;
}

typedef struct InitSectionInfo_s {
    char    mURI[MAX_URL_SIZE];
    char    mByterange[32];
} InitSectionInfo_t;

static void handle_init_section_args(InitSectionInfo_t* info, const char *key, int key_len, char **dest, int *dest_len)
{
    if (!strncmp(key, "URI=", key_len))
    {
        *dest     =        info->mURI;
        *dest_len = sizeof(info->mURI);
    } else if (!strncmp(key, "BYTERANGE=", key_len)) {
        *dest     =        info->mByterange;
        *dest_len = sizeof(info->mByterange);
    }
}

static Segment_t* new_init_section(Playlist_t* pls, InitSectionInfo_t* info, const char* url_base) 
{ 
    Segment_t* sec; 
    char *ptr; 
    char tmp_str[MAX_URL_SIZE]; 
 
    if (!info->mURI[0]) 
        return NULL; 
 
    sec = (Segment_t*)av_mallocz(sizeof(*sec)); 
    if (!sec) 
        return NULL; 
 
    ff_make_absolute_url(tmp_str, sizeof(tmp_str), url_base, info->mURI); 
    sec->mURL = av_strdup(tmp_str); 

    if (!sec->mURL)
    { 
        av_free(sec); 
        return NULL; 
    } 
 
    if (info->mByterange[0])
    { 
        sec->mSize = strtoll(info->mByterange, NULL, 10); 
        ptr = strchr(info->mByterange, '@'); 
        if (ptr) 
            sec->mUrlOffset = strtoll(ptr+1, NULL, 10); 
    } 
    else
    { 
        /* the entire file is the init section */ 
        sec->mSize = -1; 
    } 
 
    dynarray_add(&pls->mInitSections, &pls->mInitSectionCnt, sec); 
 
    return sec; 
}

static int ensure_playlist(HLSInfo_t* info, Playlist_t** pls, const char* url)
{
    Variant_t* variant = NULL;

    if (*pls)
        return 0;

    variant = new_variant(info, NULL, url, NULL);
    if (!variant)
        return AVERROR(ENOMEM);

    *pls = variant->mPlaylists[info->mPlaylistCnt - 1];

    return 0;
}

static int is_same_server(const char* url1, const char* url2)
{
    char proto1[32];
    char proto2[32];
    char hostname1[1024];
    char hostname2[1024];
    int  port1, port2;

    av_url_split(proto1, sizeof(proto1), NULL, 0, hostname1, sizeof(hostname1), &port1, NULL, 0, url1);
    av_url_split(proto2, sizeof(proto2), NULL, 0, hostname2, sizeof(hostname2), &port2, NULL, 0, url2);

    if (strcmp(proto1, proto2) == 0 && strcmp(hostname1, hostname2) == 0 && port1 == port2)
        return 1;

    return 0;
}

static int parse_playlist(HLSInfo_t* info, const char* url, Playlist_t* pls, const AVIOInterruptCB* int_cb, AVIOContext** io)
{
    int ret;
    char tmp_str[MAX_URL_SIZE];
    char line[MAX_URL_SIZE];
    
    int is_variant = 0;
    VariantInfo_t variantInfo;

    KeyType_e eKeyType = KEY_TYPE_NONE;
    char      keyURI[MAX_URL_SIZE];
    int       has_iv = 0;
    uint8_t   iv[16] = { 0, };

    int is_segment = 0;
    int64_t segmentDuration = 0;
    int64_t segmentSize = -1;
    int64_t segmentOffset = 0;

    Segment_t* curInitSection = NULL;

    AVIOContext* in = NULL;
    URLContext* h = NULL;

    if (io && *io) /* KEEP ALIVE OPEN */
    {
        in = *io;
        h = (URLContext*)in->opaque;
        if(!h)
        {
            LOG_ERROR("url context is null \n");
            return -1;
        }

        if (is_same_server(h->filename, url))
        {
// TBD Check it !!!
//          avio_reset(in, AVIO_FLAG_READ);
            in->eof_reached = 0;
            ret = ff_http_do_new_request(h, url);
            if (ret < 0)
            {
                avio_close(in);
                if (ret == AVERROR_EXIT)
                {
                    LOG_ERROR("AVERROR_EXIT !!!\n");
                    return ret;
                }
                LOG_WARN("keepalive open failed, clear connection and retry new connection !\n");
                in = NULL;
            }
        }
        else
        {
            LOG_WARN("It's difference server, new open !\n");
            avio_close(in);
            in = NULL;
        }
    }

    if (in == NULL)
    {
		uint8_t *new_url = NULL;
		AVDictionary *opts = NULL;
		av_dict_set(&opts, "multiple_requests", "1", 0);

        ret = avio_open2(&in, url, AVIO_FLAG_READ, int_cb, &opts);
        if (ret < 0)
        {
            LOG_ERROR("Cannot open url : %s\n", url);
            in = NULL;
            return ret;
        }

        // Save redirect URL 
	    if (av_opt_get(in, "location", AV_OPT_SEARCH_CHILDREN, &new_url) >= 0)
			url = av_strdup((const char*)new_url); // TBD Free This

    }

    if (ff_get_line(in, line, sizeof(line)) == 0 && in->error)
    {
        LOG_ERROR("ff_get_line is failed !\n");
        ret = in->error;
        goto EXIT;
    }

    rtrim(line);

    if (strcmp(line, "#EXTM3U"))
    {
        ret = AVERROR_INVALIDDATA;
        LOG_ERROR("Invalid Data\n");
        goto EXIT;
    }

    while (!avio_feof(in))
    {
        const char* ptr;

        if (ff_get_line(in, line, sizeof(line)) == 0 && in->error)
        {
            LOG_ERROR("ff_get_line is failed !\n");
            ret = in->error;
            goto EXIT;
        }
        rtrim(line);

        if (av_strstart(line, "#EXT-X-STREAM-INF:", &ptr))
        {
            is_variant = 1;
            memset(&variantInfo, 0, sizeof(Variant_t));
            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_variant_args, &variantInfo);
        }
        else if (av_strstart(line, "#EXT-X-MEDIA:", &ptr))
        {
            RenditionInfo_t renditionInfo = {{0}};
            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_rendition_args, &renditionInfo);
            new_rendition(info, &renditionInfo, url);
        }
        else if (av_strstart(line, "#EXTINF:", &ptr))
        {
            is_segment = 1;
            segmentDuration = atof(ptr) * AV_TIME_BASE;        
        }
        else if (av_strstart(line, "#EXT-X-KEY:", &ptr))
        {
            KeyInfo_t keyInfo;
			memset(&keyInfo, 0x00, sizeof(keyInfo));

            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_key_args, &keyInfo);
            
            eKeyType = KEY_TYPE_NONE;
            if (!strcmp(keyInfo.mMethod, "AES-128"))
                eKeyType = KEY_TYPE_AES128;
            
            has_iv = 0;
            if (!strncmp(keyInfo.mIV, "0x", 2) || !strncmp(keyInfo.mIV, "0X", 2))
            {
                ff_hex_to_data(iv, keyInfo.mIV + 2);
                has_iv = 1;
            }
            av_strlcpy(keyURI, keyInfo.mURI, sizeof(keyURI));
        }
        else if (av_strstart(line, "#EXT-X-TARGETDURATION:", &ptr))
        {
            ret = ensure_playlist(info, &pls, url);
            if (ret < 0)
                goto EXIT;

            pls->mTargetDuration = strtoll(ptr, NULL, 10) * AV_TIME_BASE;
        }
        else if (av_strstart(line, "#EXT-X-MEDIA-SEQUENCE:", &ptr))
        {
            ret = ensure_playlist(info, &pls, url);
            if (ret < 0)
                goto EXIT;

            pls->mStartSeqNo = atoi(ptr);
        }
        else if (av_strstart(line, "#EXT-X-PLAYLIST-TYPE:", &ptr))
        {
            ret = ensure_playlist(info, &pls, url);
            if (ret < 0)
                goto EXIT;

            if (!strcmp(ptr, "EVENT"))
                pls->mType = PLS_TYPE_EVENT;
            else if (!strcmp(ptr, "VOD"))
                pls->mType = PLS_TYPE_VOD;
        }
        else if (av_strstart(line, "#EXT-X-ENDLIST", &ptr))
        {
            if (pls)
                pls->mFinished = 1;
        }
        else if (av_strstart(line, "#EXT-X-MAP:", &ptr))
        {
            InitSectionInfo_t initSecInfo = {{0}};
printf("---- pls : %p url : %s\n", pls, pls?pls->mURL:"NONE");
            ret = ensure_playlist(info, &pls, url);
            if (ret < 0)
                goto EXIT;

            ff_parse_key_value(ptr, (ff_parse_key_val_cb) handle_init_section_args, &initSecInfo);
            curInitSection = new_init_section(pls, &initSecInfo, url);
            curInitSection->mKeyType = eKeyType;
            if (has_iv)
            {
                memcpy(curInitSection->mIV, iv, sizeof(iv));
            }
            else
            {
                int seq = pls->mStartSeqNo + pls->mSegmentCnt;
                memset(curInitSection->mIV, 0, sizeof(curInitSection->mIV));
                AV_WB32(curInitSection->mIV + 12, seq);
            }

            if (eKeyType != KEY_TYPE_NONE)
            {
                ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, keyURI);
                curInitSection->mKeyURL = av_strdup(tmp_str);
                if (!curInitSection->mKeyURL)
                {
                    av_free(curInitSection);
                    ret = AVERROR(ENOMEM);
                    goto EXIT;
                }
            }
            else
            {
                curInitSection->mKeyURL = NULL;
            }
        }
        else if (av_strstart(line, "#EXT-X-BYTERANGE:", &ptr))
        {
            segmentSize = strtoll(ptr, NULL, 10);
            ptr = strchr(ptr, '@');
            if (ptr)
                segmentOffset = strtoll(ptr+1, NULL, 10);
        }
        else if (av_strstart(line, "#", NULL))
        {
            //LOG_WARN("Skip ('%s')\n", line);
        }
        else if (line[0])
        {
            if (is_variant)
            {
                if (!new_variant(info, &variantInfo, line, url))
                {
                    ret = AVERROR(ENOMEM);
                    goto EXIT;
                }
                is_variant = 0;    
            }
            else if (is_segment)
            {
                Segment_t* seg;
                ret = ensure_playlist(info, &pls, url);
                if (ret < 0)
                    goto EXIT;

                seg = (Segment_t*)av_malloc(sizeof(Segment_t));
                if (!seg)
                {
                    ret = AVERROR(ENOMEM);
                    goto EXIT;
                }
                seg->mDuration = segmentDuration;
                seg->mKeyType = eKeyType;
                if (has_iv)
                    memcpy(seg->mIV, iv, sizeof(iv));
                else
                {
                    int seq = pls->mStartSeqNo + pls->mSegmentCnt;
                    memset(seg->mIV, 0, sizeof(seg->mIV));
                    AV_WB32(seg->mIV + 12, seq);
                }

                if (eKeyType != KEY_TYPE_NONE)
                {
                    ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, keyURI);
                    seg->mKeyURL = av_strdup(tmp_str);
                    if (!seg->mKeyURL)
                    {
                        av_free(seg);
                        ret = AVERROR(ENOMEM);
                        goto EXIT;
                    }
                }
                else
                {
                    seg->mKeyURL = NULL;
                }

                ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, line);
                seg->mURL = av_strdup(tmp_str);

                if (!seg->mURL)
                {
                    av_free(seg->mKeyURL);
                    av_free(seg);
                    ret = AVERROR(ENOMEM);
                    goto EXIT;
                }

                dynarray_add(&pls->mSegments, &pls->mSegmentCnt, seg);
                is_segment = 0;

                seg->mSize = segmentSize;
                if (segmentSize >= 0) {
                    seg->mUrlOffset = segmentOffset;
                    segmentOffset += segmentSize;
                    segmentSize = -1;
                } else {
                    seg->mUrlOffset = 0;
                    segmentOffset = 0;
                }

                seg->mInitSection = curInitSection;
            }
        }
    }

    if (pls)
        pls->mLastLoadTime = get_tick();

EXIT:
    if (io)
    {
        *io = in;
    }
    else
    {
        if (in)
            avio_close(in);
    }

    return ret;
}

static void add_renditions_to_variant(HLSInfo_t* info, Variant_t* var, enum AVMediaType type, const char* group_id)
{
    int ii;

    for (ii = 0; ii < info->mRenditionCnt; ii++)
    {
        Rendition_t* rend = info->mRenditions[ii];

        if (rend->mType == type && !strcmp(rend->mGroupId, group_id))
        {
            if (rend->mPlaylist)
                dynarray_add(&var->mPlaylists, &var->mPlaylistCnt, rend->mPlaylist);
            else
            {
                dynarray_add(&var->mPlaylists[0]->mRenditions, &var->mPlaylists[0]->mRenditionCnt, rend);
            }
        }
    }
}

int HLS_M3U8_Parse(HLSInfo_t* info, const char* url, const AVIOInterruptCB* int_cb, AVIOContext** io)
{
    AVIOContext* in = NULL;
    int ret = 0;
    int ii, jj;

    memset(info, 0x00, sizeof(HLSInfo_t));

    if (io && *io)
        in = *io;

    if ((ret = parse_playlist(info, url, NULL, int_cb, &in)) != 0)
        goto ERROR;

    /* m3u8 contains other m3u8 for variant */
    if (info->mPlaylistCnt > 1 || info->mPlaylists[0]->mSegmentCnt == 0)
    {
        for (ii = 0; ii < info->mPlaylistCnt; ii++)
        {
            Playlist_t* pls = info->mPlaylists[ii];

            if ((ret = parse_playlist(info, pls->mURL, pls, int_cb, &in)) != 0)
                goto ERROR;
        }
    }

#if 1 /* Make Start PTS */
    for (ii = 0; ii < info->mPlaylistCnt; ii++)
    {
        int64_t pts = 0;
        Playlist_t* pls = info->mPlaylists[ii];

        for (jj = 0; jj < pls->mSegmentCnt; jj++)
        {
            Segment_t* seg = pls->mSegments[jj];
            if (pls->mFinished)
            {
                seg->mStartPts = pts;
                pts += seg->mDuration;
            }
            else
            {
                seg->mStartPts = AV_NOPTS_VALUE;
            }
        }
    }
#endif

    /* Register renditions to playlist */
    for (ii = 0; ii < info->mVariantCnt; ii++)
    {
        Variant_t* var = info->mVariants[ii];
        if(var->mAudioGroup[0])
        {
            add_renditions_to_variant(info, var, AVMEDIA_TYPE_AUDIO, var->mAudioGroup);
        }
        if(var->mVideoGroup[0])
        {
            add_renditions_to_variant(info, var, AVMEDIA_TYPE_VIDEO, var->mVideoGroup);
        }
        if(var->mSubtitleGroup[0])
        {
            add_renditions_to_variant(info, var, AVMEDIA_TYPE_SUBTITLE, var->mSubtitleGroup);
        }
    }

    goto EXIT;
ERROR:
    /* TBD. Free Here */

EXIT:
    if (io)
    {
        *io = in;
    }
    else
    {
        if (in)
            avio_close(in);
    }
    return ret;
}

int HLS_M3U8_Update(Playlist_t* pls, const AVIOInterruptCB* int_cb, AVIOContext** io)
{
    int ret;
    int ii, pts;
    Playlist_t newpls;
    memset(&newpls, 0x00, sizeof(Playlist_t));
    strcpy(newpls.mURL, pls->mURL);

    ret = parse_playlist(NULL, newpls.mURL, &newpls, int_cb, io);
    if (ret)
    {
        LOG_ERROR("parse_playlist is failed : ret %d\n", ret);
        return ret;
    }

    LOG_INFO("SegmentCnt : %d, StartSeqNo : %d, EndSeqNo : %d\n", newpls.mSegmentCnt, newpls.mStartSeqNo, newpls.mStartSeqNo + newpls.mSegmentCnt);
#if 0 /* IS NEED TO CHECK */
    if (newpls.mStartSeqNo > pls->mStartSeqNo)
    {
        int diff = newpls.mStartSeqNo - pls->mStartSeqNo;
        
    }
#endif 

    free_segment_from_playlist(pls);
    *pls = newpls;

    for (ii = 0; ii < pls->mSegmentCnt; ii++)
    {
        Segment_t* seg = pls->mSegments[ii];
        if (pls->mFinished)
        {
            seg->mStartPts = pts;
            pts += seg->mDuration;
        }
        else
        {
            seg->mStartPts = AV_NOPTS_VALUE;
        }

    }

    return 0;
}

static void free_init_section_list(Playlist_t* pls)
{
    int ii;
    for (ii = 0; ii < pls->mInitSectionCnt; ii++)
    {
        av_freep(&pls->mInitSections[ii]->mURL);
        av_freep(&pls->mInitSections[ii]);
    }
    av_freep(&pls->mInitSections);
    pls->mInitSectionCnt = 0;
}

void HLS_M3U8_Delete(HLSInfo_t* info)
{
    int ii;

    // 1. free playlist 
    for (ii = 0; ii < info->mPlaylistCnt; ii++)
    {
        Playlist_t* pls = info->mPlaylists[ii];

        free_segment_from_playlist(pls);
        free_init_section_list(pls);
        av_freep(&pls->mRenditions);
        av_free(pls);
        /* TBD. IMPLEMENTS HERE .... */
    }
    av_freep(&info->mPlaylists);
    info->mPlaylistCnt = 0;

    // 2. free_variant_list
    for (ii = 0; ii < info->mVariantCnt; ii++)
    {
        Variant_t* var = info->mVariants[ii];

        av_freep(&var->mPlaylists);
        av_free(var);
    }
    av_freep(&info->mVariants);
    info->mVariantCnt = 0;

    // 3. free_rendition_list
    for (ii = 0; ii < info->mRenditionCnt; ii++)
    {
        av_freep(&info->mRenditions[ii]);
    }
    av_freep(&info->mRenditions);
    info->mRenditionCnt = 0;
}

void HLS_M3U8_Dump(HLSInfo_t* info)
{
    int ii, jj;
    if (!info)
        LOG_ERROR("-- info is NULL \n");

    LOG_INFO("######## DUMP M3U8 :\n");
    for (ii = 0; ii  < info->mVariantCnt; ii++)
    {
        Variant_t* var = info->mVariants[ii];
        LOG_INFO("Variant [%d] - Bandwidth : %d\n", ii, var->mBandwidth);
        if (var->mAudioGroup[0])
            LOG_INFO("       AudioGroup : %s\n", var->mAudioGroup);
        if (var->mVideoGroup[0])
            LOG_INFO("       VideoGroup : %s\n", var->mVideoGroup);
        if (var->mSubtitleGroup[0])
            LOG_INFO("       SubtitleGroup : %s\n", var->mSubtitleGroup);
        
        for (jj = 0; jj < var->mPlaylistCnt; jj++)
        {
            Playlist_t* pls = var->mPlaylists[jj];
            if (jj == 0)
                LOG_INFO("       Playlist [%d] - type : MainStream, start : %d, segment : %d\n", jj, pls->mStartSeqNo, pls->mSegmentCnt);
            else
            {
                const char* strType = "Unknown";
                switch(pls->mRenditions[0]->mType)
                {
                    case AVMEDIA_TYPE_VIDEO:    strType = "VIDEO";    break;
                    case AVMEDIA_TYPE_AUDIO:    strType = "AUDIO";    break;
                    case AVMEDIA_TYPE_SUBTITLE: strType = "SUBTITLE"; break;
                    case AVMEDIA_TYPE_DATA:     strType = "DATA";     break;
                    default: break;
                }

                LOG_INFO("       Playlist [%d] - type : Rendition, %s, start : %d, segment : %d\n", jj, strType, pls->mStartSeqNo, pls->mSegmentCnt);
            }

        }
    }

    for (ii = 0; ii < info->mRenditionCnt; ii++)
    {
        Rendition_t* rend = info->mRenditions[ii];
        const char* strType = "Unknown";
        switch(rend->mType)
        {
            case AVMEDIA_TYPE_VIDEO:    strType = "VIDEO";    break;
            case AVMEDIA_TYPE_AUDIO:    strType = "AUDIO";    break;
            case AVMEDIA_TYPE_SUBTITLE: strType = "SUBTITLE"; break;
            case AVMEDIA_TYPE_DATA:     strType = "DATA";     break;
            default: break;
        }
        LOG_INFO("Rendition [%d] - %s GroupId: %s, Name: %s, LANG: %s\n", ii, strType, rend->mGroupId, rend->mName, rend->mLanguage);
        if (rend->mPlaylist)
            LOG_INFO("       Playlist - SegmentCnt : %d\n", rend->mPlaylist->mSegmentCnt);
    }
}
