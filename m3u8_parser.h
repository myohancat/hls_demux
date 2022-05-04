#ifndef __M3U8_PARSER_H_
#define __M3U8_PARSER_H_

#include "hls_common.h"
#include "libavformat/avio.h"

#define MAX_FIELD_LEN    64
#define MAX_CHARACTERISTICS_LEN 512

typedef enum {
    KEY_TYPE_NONE,
    KEY_TYPE_AES128,
} KeyType_e;

typedef enum {
    PLS_TYPE_UNSPECIFIED,
    PLS_TYPE_EVENT,
    PLS_TYPE_VOD,
} PlaylistType_e;

typedef struct Segment_s {
    char*             mURL;

    int64_t           mStartPts;
    int64_t           mDuration;
    int64_t           mUrlOffset;
    int64_t           mSize;

    KeyType_e         mKeyType;
    char*             mKeyURL;
    uint8_t           mIV[16];

    struct Segment_s* mInitSection;
} Segment_t;

typedef struct Playlist_s {
    char                mURL[MAX_URL_SIZE];

    int                 mFinished;
    PlaylistType_e      mType;
    
    int64_t             mTargetDuration; /* #EXT-X-TARGETDURATION */
    int                 mStartSeqNo;     /* #EXT-X-MEDIA-SEQUENCE */

    Segment_t**         mSegments;
    int                 mSegmentCnt;

    Segment_t**         mInitSections;
    int                 mInitSectionCnt;

    struct Rendition_s** mRenditions;
    int                  mRenditionCnt;

    int64_t             mLastLoadTime;  /* for live update */
} Playlist_t;

typedef struct Rendition_s {
    enum AVMediaType mType;
    Playlist_t*      mPlaylist;
    char             mGroupId[MAX_FIELD_LEN];
    char             mLanguage[MAX_FIELD_LEN];
    char             mName[MAX_FIELD_LEN];
    int              mDisposition; /* TBD. Check it  */
} Rendition_t;

typedef struct Variant_s {
    int              mBandwidth;
    
    Playlist_t**     mPlaylists;
    int              mPlaylistCnt;
    
    char             mAudioGroup[MAX_FIELD_LEN];
    char             mVideoGroup[MAX_FIELD_LEN];
    char             mSubtitleGroup[MAX_FIELD_LEN];
} Variant_t;

typedef struct HLSInfo_s {
    Playlist_t**     mPlaylists;
    int              mPlaylistCnt;

    Variant_t**      mVariants;
    int              mVariantCnt;

    Rendition_t**    mRenditions;
    int              mRenditionCnt;
} HLSInfo_t;

int HLS_M3U8_Parse(HLSInfo_t* info, const char* url, const AVIOInterruptCB* int_cb, AVIOContext** io);
int HLS_M3U8_Update(Playlist_t* pls, const AVIOInterruptCB* int_cb, AVIOContext** io);
void HLS_M3U8_Delete(HLSInfo_t* info);
void HLS_M3U8_Dump(HLSInfo_t* info);

#endif /* __M3U8_PARSER_H_ */
