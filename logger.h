#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

/* Maximum size per log msg (\0 included) */
#define LOGGER_LINE_SZ 1024

/* Added to LOGGER_LINE_SZ for the date/time/... */
#define LOGGER_MAX_PREFIX_SZ 256

/* Maximum size for the thread name (if it is set) */
#define LOGGER_MAX_THREAD_NAME_SZ 16

/* Maximum length of "file:src:line" sub string */
#define LOGGER_MAX_SOURCE_LEN 50

/* Follow the notation of Linux kernel printk.
 * See https://www.kernel.org/doc/html/latest/core-api/printk-basics.html
 */
typedef enum {
    /* Alert: Process can not continue working. Manual action must be done. */
    LOGGER_LEVEL_ALERT = 0,
    /* Crit: Process was entered in an unknown state.  */
    LOGGER_LEVEL_CRITICAL,
    /* Error: Error is returned from function, etc. */
    LOGGER_LEVEL_ERROR,
    /* Warning: Message have to be checked further */
    LOGGER_LEVEL_WARNING,
    /* Notice: Message could be important/interresting to know. */
    LOGGER_LEVEL_NOTICE,
    /* Info: Message is symply informational */
    LOGGER_LEVEL_INFO,
    /* Debug: Message is for debugging informations only. */
    LOGGER_LEVEL_DEBUG,

    /* Internal use */
    LOGGER_LEVEL_COUNT,                         /* Number of levels */
    LOGGER_LEVEL_FIRST = LOGGER_LEVEL_ALERT,    /* First level value */
    LOGGER_LEVEL_LAST = LOGGER_LEVEL_COUNT - 1, /* Last level value */
} logger_line_level_t;

#define LOGGER_LEVEL_ALL LOGGER_LEVEL_FIRST... LOGGER_LEVEL_LAST

typedef enum {
    LOGGER_OPT_NONE = 0,     /* No options. Use default values ! */
    LOGGER_OPT_NONBLOCK = 1, /* return -1 and EAGAIN when the queue is full */
    /* Print lost lines soon as there is some free space again */
    LOGGER_OPT_PRINTLOST = 2,
    /* Force the kernel to really allocate the bloc (to bypass copy-on-write
     * mechanism)
     */
    LOGGER_OPT_PREALLOC = 4,
} logger_opts_t;

/* Definition of a log line */
typedef struct {
    bool ready;                /* Line ready to be printed */
    struct timespec ts;        /* Timestamp (key to order on) */
    logger_line_level_t level; /* Level of this line */
    const char *file;          /* File who generated the log */
    const char *func;          /* Function */
    unsigned int line;         /* Line */
    char str[LOGGER_LINE_SZ];  /* Line buffer */
} logger_line_t;

/* Write queue: 1 per thread */
typedef struct {
    logger_line_t *lines; /* Lines buffer */
    int lines_nr;         /* Maximum number of buffered lines for this thread */
    int queue_idx;        /* Index of the queue */
    /* Options for this queue. Set to default if not precised */
    logger_opts_t opts;
    unsigned int rd_idx;      /* Actual read index */
    unsigned long rd_seq;     /* Read sequence */
    unsigned long wr_seq;     /* Write sequence */
    unsigned long lost_total; /* Total number of lost records so far */
    unsigned long lost;       /* Number of lost records since last printed */
    atomic_int free;          /* True (1) if this queue is not used */
    pthread_t thread;         /* Thread owning this queue */
    char thread_name[LOGGER_MAX_THREAD_NAME_SZ]; /* Thread name */
    int thread_name_len;                         /* Length of the thread name */
} logger_write_queue_t;

typedef struct {
    /* Colors definition for the log levels */
    const char *level[LOGGER_LEVEL_COUNT];
    const char *reset;       /* Reset the color to default */
    const char *time;        /* Time string color */
    const char *date;        /* Date string color */
    const char *date_lines;  /* Lines surrounding the date color */
    const char *thread_name; /* Thread name (or id) color */
} logger_line_colors_t;

typedef struct {
    logger_write_queue_t **queues; /* Write queues, 1 per thread */
    int queues_nr;                 /* Number of queues allocated */
    int queues_max;                /* Maximum number of possible queues */
    int default_lines_nr; /* Default number of lines max / buffer to use */
    bool running;         /* Set to true when the reader thread is running */
    bool empty;           /* Set to true when all the queues are empty */
    logger_opts_t opts;   /* Default logger options. Some can be fine tuned by
                             write queue */
    atomic_int reload;    /* True (1) when new queue(s) are added */
    atomic_int waiting;   /* True (1) if the reader-thread is sleeping ... */
    pthread_t reader_thread;   /* TID of the reader thread */
    pthread_mutex_t queues_mx; /* Needed when extending the **queues array... */
} logger_t;

/**
 * Initialize the logger manager.
 * @queues_max Hard limit of queues that can be allocated
 * @lines_max_def Recommanded log lines to allocate by default
 * @options See options above
 */
int logger_reader_create(int queues_max, int lines_max_def, logger_opts_t options);

/**
 * Empty the queues and free all the ressources.
 */
void logger_deinit(void);

/**
 * Assign a queue to the calling thread.
 * @lines_max Max lines buffer (=0 use default)
 * @opts If not precised, use the logger's options
 */
int logger_assign_write_queue(unsigned int lines_max, logger_opts_t opts);

/* Release the write queue for another thread */
int logger_free_write_queue(void);

/**
 * Create a new thread with logger queue assignment.
 * @thread_name Thread name to give
 * @max_lines Lines buffer to allocte for that thread (=0 use default)
 * @opts Options to used for this queue. (=0 use default)
 * @thread See pthread_create(3) for these args
 */
int logger_writer_create(const char *thread_name,
                          unsigned int max_lines,
                          logger_opts_t opts,
                          pthread_t *thread,
                          const pthread_attr_t *attr,
                          void *(*start_routine)(void *),
                          void *arg);

/**
 * Print a message.
 * @level Importance level of this print
 * @src Source/Func/Line this print was issued
 */
int logger_printf(logger_line_level_t level,
                  const char *src,
                  const char *func,
                  unsigned int line,
                  const char *format,
                  ... /* printf() like format & arguments */);

#define STON(v) ((v) *1000000000) /*  sec -> ns */
#define timespec_to_ns(a) ((STON((a).tv_sec) + (a).tv_nsec))
#define elapsed_ns(b, a) (timespec_to_ns(a) - timespec_to_ns(b))

#define LOG_LEVEL(lvl, fmt, ...) \
    logger_printf((lvl), __FILE__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ALERT(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_ALERT, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
#define LOG_CRITICAL(fmt, ...)                                             \
    logger_printf(LOGGER_LEVEL_CRITICAL, __FILE__, __FUNCTION__, __LINE__, \
                  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_ERROR, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_WARNING, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
#define LOG_NOTICE(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_NOTICE, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_INFO, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)                                                  \
    logger_printf(LOGGER_LEVEL_DEBUG, __FILE__, __FUNCTION__, __LINE__, fmt, \
                  ##__VA_ARGS__)
