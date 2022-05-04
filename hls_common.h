#ifndef __HLS_COMMON_H_
#define __HLS_COMMON_H_

#include "hls_log.h"
#include <stdint.h>

#define _MAX(x, y)     ((x) > (y)) ? (x):(y)
#define _MIN(x, y)     ((x) < (y)) ? (x):(y)

#define AV_PKT_FLAG_SEGMENT_CHANGED    0x8000

/* max url size */
#ifndef MAX_URL_SIZE
#define MAX_URL_SIZE    4096
#endif

#define HLS_SESSION_EOF  (-99)
#define HLS_SESSION_EXIT (-88)

#define INITIAL_BUFFER_SIZE 32768

#define ENABLE_SEGMENT_SEEK
//#define ENABLE_ADJUST_PTS

#define ENABLE_BANDWIDTH_POPUP
#define ENABLE_DEBUG_ADAPTIVE_INFO

/* DEBUG */
//#define ENABLE_DEBUG_DROP_COUNT
//#define ENABLE_DEBUG_STOP_PERFORMANCE

char* ltrim(char *s);
char* rtrim(char* s);
#define trim(s)  rtrim(ltrim(s))

int64_t get_tick(void);

#endif /* __HLS_COMMON_H_ */
