#ifndef __LOG_H_
#define __LOG_H_

#include <stdio.h>
#include <string.h>

#define BASENAME(str)       (strrchr(str, '/') ? strrchr(str, '/') + 1 : str)   
#define __BASE_FILE_NAME__   BASENAME(__FILE__)

#define ANSI_COLOR_NONE    NULL
#define ANSI_COLOR_BLACK   "\x1b[30;1m"
#define ANSI_COLOR_RED     "\x1b[31;1m"
#define ANSI_COLOR_GREEN   "\x1b[32;1m"
#define ANSI_COLOR_YELLOW  "\x1b[33;1m"
#define ANSI_COLOR_BLUE    "\x1b[34;1m"
#define ANSI_COLOR_MAGENTA "\x1b[35;1m"
#define ANSI_COLOR_CYAN    "\x1b[36;1m"
#define ANSI_COLOR_RESET   "\x1b[0m"

typedef enum
{
    LOG_LEVEL_NONE,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE,

    MAX_LOG_LEVEL    
}LOG_LEVEL_e;

extern void HLS_LOG_SetLevel(LOG_LEVEL_e eLevel);
extern void HLS_LOG_Print(int priority, const char* color, const char *fmt, ...);
extern void HLS_LOG_Dump(int priority, const void* ptr, int size);

#define LOG_TMP(fmt, args...)      do { \
                                         HLS_LOG_Print(LOG_LEVEL_INFO, ANSI_COLOR_NONE, "[%s:%d]%s() " fmt, __BASE_FILE_NAME__, __LINE__, __FUNCTION__, ##args); \
                                     } while(0)

#define LOG_TRACE(fmt, args...)      do { \
                                         HLS_LOG_Print(LOG_LEVEL_TRACE, ANSI_COLOR_CYAN, fmt, ##args); \
                                     } while(0)

#define LOG_DEBUG(fmt, args...)      do { \
                                         HLS_LOG_Print(LOG_LEVEL_DEBUG, ANSI_COLOR_NONE, fmt, ##args); \
                                     } while(0)

#define LOG_INFO(fmt, args...)       do { \
                                         HLS_LOG_Print(LOG_LEVEL_INFO, ANSI_COLOR_YELLOW, "[%s:%d] %s() " fmt, __BASE_FILE_NAME__, __LINE__, __FUNCTION__, ##args); \
                                     } while(0)

#define LOG_WARN(fmt, args...)      do { \
                                         HLS_LOG_Print(LOG_LEVEL_WARN, ANSI_COLOR_MAGENTA, "[%s:%d] %s() " fmt, __BASE_FILE_NAME__, __LINE__, __FUNCTION__, ##args); \
                                     } while(0)

#define LOG_ERROR(fmt, args...)      do { \
                                         HLS_LOG_Print(LOG_LEVEL_ERROR, ANSI_COLOR_RED, "[%s:%d] %s() " fmt, __BASE_FILE_NAME__, __LINE__, __FUNCTION__, ##args); \
                                     } while(0)


#define __TRACE_ENTER__       LOG_TRACE("------ [%s:%d] %s() Enter ----\n", __BASE_FILE_NAME__, __LINE__, __FUNCTION__)
#define __TRACE_LEAVE__       LOG_TRACE("------ [%s:%d] %s() Leave ----\n", __BASE_FILE_NAME__, __LINE__, __FUNCTION__)

#endif /* __LOG_H_ */
