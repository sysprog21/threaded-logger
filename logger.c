#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

#define futex_wait(addr, val) _futex((addr), FUTEX_WAIT_PRIVATE, (val), NULL)
#define futex_timed_wait(addr, val, ts) \
    _futex((addr), FUTEX_WAIT_PRIVATE, (val), (ts))
#define futex_wake(addr, val) _futex((addr), FUTEX_WAKE_PRIVATE, (val), NULL)
static inline int _futex(atomic_int *uaddr,
                         int futex_op,
                         int val,
                         struct timespec *tv)
{
    /* Note: not using the last 2 parameters uaddr2 & val2 (see futex(2)) */
    return syscall(SYS_futex, uaddr, futex_op, val, tv);
}

#define NTOM(v) ((v) / 1000000) /* ns -> ms */
#define NTOU(v) ((v) / 1000)    /* ns -> sec */

/* Global logger context */
static logger_t logger;

static void *logger_reader_func(void);

static _Thread_local logger_write_queue_t *_own_wrq = NULL;

static void set_thread_name(logger_write_queue_t *wrq)
{
    wrq->thread = pthread_self();
    pthread_getname_np(wrq->thread, wrq->thread_name, sizeof(wrq->thread_name));
    if (!wrq->thread_name[0])
        snprintf(wrq->thread_name, LOGGER_MAX_THREAD_NAME_SZ, "%lu",
                 wrq->thread);
    wrq->thread_name_len = strlen(wrq->thread_name);
}

static logger_write_queue_t *alloc_write_queue(int lines_max,
                                               logger_opts_t opts)
{
    if (logger.queues_nr == logger.queues_max)
        return errno = ENOBUFS, NULL;

    logger_write_queue_t *wrq = calloc(1, sizeof(logger_write_queue_t));
    wrq->lines = calloc(lines_max, sizeof(logger_line_t));
    wrq->lines_nr = lines_max;
    wrq->opts = opts;
    set_thread_name(wrq);

    if (opts & LOGGER_OPT_PREALLOC) {
        /* Pre-fill the queue with something so that Linux really allocate
         * the pages.  This is due to the 'copy-on-write' logic where the
         * page is really allocated (or copied) when the process try to
         * write something.
         */
        for (int i = 0; i < lines_max; i++) {
            wrq->lines[i].ts.tv_nsec = ~0;
            for (int j = 0, k = 0; j < sizeof(wrq->lines[i].str) / 64; j++)
                wrq->lines[i].str[j] = k++;
        }
    }

    /* Ensure this is done atomically between writers. Reader is safe. */
    pthread_mutex_lock(&logger.queues_mx);
    wrq->queue_idx = logger.queues_nr;
    logger.queues[logger.queues_nr++] = wrq;
    pthread_mutex_unlock(&logger.queues_mx);

    /* Let the logger thread take this change into account when possible */
    atomic_compare_exchange_strong(&logger.reload, &(atomic_int){0}, 1);
    return wrq;
}

int logger_init(int queues_max, int lines_max, logger_opts_t opts)
{
    memset(&logger, 0, sizeof(logger_t));

    pthread_mutex_init(&logger.queues_mx, NULL);

    logger.queues = calloc(queues_max, sizeof(logger_write_queue_t *));
    logger.queues_max = queues_max;
    logger.opts = opts;
    logger.default_lines_nr = lines_max;
    logger.running = true;

    _own_wrq = NULL;

    /* Reader thread */
    pthread_create(&logger.reader_thread, NULL, (void *) logger_reader_func,
                   NULL);
    pthread_setname_np(logger.reader_thread, "logger-reader");
    return 0;
}

void logger_deinit(void)
{
    /* Sync with the logger & force him to double-check the queues */
    while (!logger.waiting)
        usleep(100);

    logger.running = false;
    atomic_store(&logger.waiting, 0);
    int r = futex_wake(&logger.waiting, 1);

    pthread_join(logger.reader_thread, NULL);

    for (int i = 0; i < logger.queues_nr; i++) {
        free(logger.queues[i]->lines);
        free(logger.queues[i]);
    }
    free(logger.queues);
    memset(&logger, 0, sizeof(logger_t));
}

int logger_assign_write_queue(unsigned int lines_max, logger_opts_t opts)
{
    /* If this is already set, nothing else to do */
    if (_own_wrq)
        return 0;

    /* Caller don't want a specific size */
    if (!lines_max)
        lines_max = logger.default_lines_nr;

    logger_write_queue_t **queue = logger.queues;
    logger_write_queue_t *fwrq;
    int last_lines_nr;

retry:
    /* Searching first for a free queue previously allocated */
    last_lines_nr = INT_MAX;
    fwrq = NULL;
    for (int i = 0; i < logger.queues_nr; i++) {
        if (!atomic_load(&queue[i]->free))
            continue;

        /* Find the best free queue ... */
        int lines_nr = queue[i]->lines_nr;
        if (lines_max <= lines_nr && lines_nr < last_lines_nr) {
            last_lines_nr = lines_nr;
            fwrq = queue[i];
        }
    }
    if (fwrq) {
        if (!atomic_compare_exchange_strong(&fwrq->free, &(atomic_int){1}, 0)) {
            /* Race condition, another thread took it right before us.
             * Trying another one */
            goto retry;
        }
        set_thread_name(fwrq);
        fwrq->opts = opts ?: logger.opts;
    } else {
        /* No free queue that fits our needs... Adding a new one. */
        fwrq = alloc_write_queue(lines_max, opts ?: logger.opts);
        if (!fwrq)
            return -1;
    }
    _own_wrq = fwrq;
    return 0;
}

static inline int wakeup_reader_if_needed(void)
{
    if (atomic_compare_exchange_strong(&logger.waiting, &(atomic_int){1}, 0)) {
        /* (the only) 1 waiter to wakeup  */
        return futex_wake(&logger.waiting, 1) < 0 ? -1 : 1;
    }
    return 0;
}

int logger_free_write_queue(void)
{
    while (_own_wrq->rd_seq != _own_wrq->wr_seq) {
        if (wakeup_reader_if_needed() < 0)
            return -1;

        /* Wait for the queue to be empty before leaving */
        usleep(100);
    }
    atomic_store(&_own_wrq->free, 1);
    _own_wrq = NULL;
    return 0;
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    int max_lines;
    logger_opts_t opts;
    char thread_name[LOGGER_MAX_THREAD_NAME_SZ];
} thread_params_t;

static void thread_wrapper(thread_params_t *params)
{
    pthread_cleanup_push((void *) free, (void *) params);

    pthread_setname_np(pthread_self(), params->thread_name);
    /* The name of the thread is fixed at allocation time so, the
     * pthread_setname_np() call must occur before the assignation bellow.
     * If there is no name specified, thread_id is used instead.
     */
    if (logger_assign_write_queue(params->max_lines, params->opts) < 0) {
        /* If it happens, it means the limit of queues to alloc is too low. */
        pthread_exit(NULL);
    }

    /* Must be called when the thread don't need it anymore.  Otherwise it
     * will stay allocated for an unexistant thread for ever !  This is also
     * true for the local threads forked by the domains themself.
     *
     * Note also that if this thread never print something, you should NOT
     * use this wrapper but better use the native pthread_create(3) instead
     * as all this is useless and reserve a log queue for nothing ...
     */
    pthread_cleanup_push((void *) logger_free_write_queue, NULL);

    /* Let run the main thread function */
    pthread_exit(params->start_routine(params->arg));

    /* a push expects a pop */
    pthread_cleanup_pop(true);
    pthread_cleanup_pop(true);
}

int logger_pthread_create(const char *thread_name,
                          unsigned int max_lines,
                          logger_opts_t opts,
                          pthread_t *thread,
                          const pthread_attr_t *attr,
                          void *(*start_routine)(void *),
                          void *arg)
{
    thread_params_t *params = malloc(sizeof(thread_params_t));
    if (!params)
        return -1;

    strncpy(params->thread_name, thread_name, sizeof(params->thread_name) - 1);
    params->thread_name[sizeof(params->thread_name) - 1] = 0;
    params->max_lines = max_lines;
    params->opts = opts;
    params->start_routine = start_routine;
    params->arg = arg;

    return pthread_create(thread, attr, (void *) thread_wrapper,
                          (void *) params);
}

int logger_printf(logger_line_level_t level,
                  const char *src,
                  const char *func,
                  unsigned int line,
                  const char *format,
                  ...)
{
    if (!logger.running)
        return errno = ENOTCONN, -1;

    if (!_own_wrq && logger_assign_write_queue(0, LOGGER_OPT_NONE) < 0)
        return -1;

    va_list ap;
    int index;
    logger_line_t *l;
reindex:
    index = _own_wrq->wr_seq % _own_wrq->lines_nr;
    l = &_own_wrq->lines[index];

    while (l->ready) {
        int ret = wakeup_reader_if_needed();
        if (ret < 0)
            return -1;

        if (ret > 0) {
            /* Last chance to empty at least a cell before giving up */
            usleep(1);
            continue;
        }
        if (_own_wrq->opts & LOGGER_OPT_NONBLOCK) {
            _own_wrq->lost++;
            return errno = EAGAIN, -1;
        }
        usleep(50);
    }
    if (_own_wrq->lost && _own_wrq->opts & LOGGER_OPT_PRINTLOST) {
        int lost = _own_wrq->lost;
        _own_wrq->lost_total += lost;
        _own_wrq->lost = 0;
        goto reindex;
    }
    va_start(ap, format);

    clock_gettime(CLOCK_REALTIME, &l->ts);
    l->level = level;
    l->file = src;
    l->func = func;
    l->line = line;
    vsnprintf(l->str, sizeof(l->str), format, ap);

    l->ready = true;
    _own_wrq->wr_seq++;

    return wakeup_reader_if_needed() < 0 ? -1 : 0;
}

static const char *const _logger_level_label[LOGGER_LEVEL_COUNT] = {
    [LOGGER_LEVEL_ALERT] = "ALERT",  [LOGGER_LEVEL_CRITICAL] = "CRIT ",
    [LOGGER_LEVEL_ERROR] = "ERROR",  [LOGGER_LEVEL_WARNING] = "WARN",
    [LOGGER_LEVEL_NOTICE] = "NOTCE", [LOGGER_LEVEL_INFO] = "INFO",
    [LOGGER_LEVEL_DEBUG] = "DEBUG",
};

typedef struct {
    unsigned long ts;          /* Key to sort on (ts of current line) */
    logger_write_queue_t *wrq; /* Related write queue */
} fuse_entry_t;

static const char *get_date(unsigned long sec, const logger_line_colors_t *c)
{
    static char date[64];
    static unsigned long prev_day = 0;
    unsigned long day = (sec - timezone) / (60 * 60 * 24); /* 1 day in sec */
    if (day == prev_day)
        return "";

    char tmp[16];
    struct tm tm;
    localtime_r((const time_t *) &sec, &tm);
    strftime(tmp, sizeof(tmp), "%Y-%m-%d", &tm);
    sprintf(date, "%s-- %s%s%s --%s\n", c->date_lines, c->date, tmp,
            c->date_lines, c->reset);
    prev_day = day;
    return date;
}

static const char *get_time(unsigned long sec, const logger_line_colors_t *c)
{
    static char time[32];
    static unsigned long prev_min = 0;
    unsigned long min = sec / 60;
    if (min != prev_min) {
        char tmp[8];
        struct tm tm;
        localtime_r((const time_t *) &sec, &tm);
        strftime(tmp, sizeof(tmp), "%H:%M", &tm);
        sprintf(time, "%s%s%s", c->time, tmp, c->reset);
        prev_min = min;
    }
    return time;
}

/* Default theme */
extern const logger_line_colors_t logger_colors_default;

static int write_line(const logger_write_queue_t *wrq, const logger_line_t *l)
{
    char linestr[LOGGER_LINE_SZ + LOGGER_MAX_PREFIX_SZ];
    const logger_line_colors_t *c = &logger_colors_default;

    /* File/Function/Line */
    char src_str[128], *start_of_src_str = src_str;
    int len = snprintf(src_str, sizeof(src_str), "%24s %20s %4d", l->file,
                       l->func, l->line);
    if (len > LOGGER_MAX_SOURCE_LEN)
        start_of_src_str += len - LOGGER_MAX_SOURCE_LEN;

    /* Time stamp calculations */
    unsigned long usec = NTOU(l->ts.tv_nsec) % 1000;
    unsigned long msec = NTOM(l->ts.tv_nsec) % 1000;
    int sec = l->ts.tv_sec % 60;

    /* Format all together */
    static int biggest_thread_name = 0;
    if (wrq->thread_name_len > biggest_thread_name)
        biggest_thread_name = wrq->thread_name_len;

    len = sprintf(linestr, "%s%s:%02d.%03lu,%03lu [%s%s%s] %*s <%s%*s%s> %s\n",
                  get_date(l->ts.tv_sec, c), get_time(l->ts.tv_sec, c), sec,
                  msec, usec, c->level[l->level], _logger_level_label[l->level],
                  c->reset, LOGGER_MAX_SOURCE_LEN, start_of_src_str,
                  c->thread_name, biggest_thread_name, wrq->thread_name,
                  c->reset, l->str);
    return write(1, linestr, len);
}

static inline void bubble_fuse_up(fuse_entry_t *fuse, int fuse_nr)
{
    if (fuse_nr > 1 && fuse[0].ts > fuse[1].ts) {
        /* Strictly bigger move up (stack empty ones at the end of smallers) */
        fuse_entry_t newentry = fuse[0];
        int i = 0;
        do {
            fuse[i] = fuse[i + 1];
            i++;
        } while (i < fuse_nr - 1 && newentry.ts > fuse[i + 1].ts);
        fuse[i] = newentry;
    }
}

static inline void bubble_fuse_down(fuse_entry_t *fuse, int fuse_nr)
{
    if (fuse_nr > 1 && fuse[fuse_nr - 1].ts <= fuse[fuse_nr - 2].ts) {
        int i = fuse_nr - 1;
        /* smaller-n-same move down (so, stack empty ones at top of smallers) */
        fuse_entry_t newentry = fuse[i];
        do {
            fuse[i] = fuse[i - 1];
            i--;
        } while (i > 0 && newentry.ts <= fuse[i - 1].ts);
        fuse[i] = newentry;
    }
}

static inline int set_queue_entry(const logger_write_queue_t *wrq,
                                  fuse_entry_t *fuse)
{
    unsigned int index = wrq->rd_idx;
    if (!wrq->lines[index].ready) {
        fuse->ts = ~0; /* it is empty */
        return 1;
    }

    struct timespec ts = wrq->lines[index].ts;
    fuse->ts = timespec_to_ns(ts);
    return 0;
}

static int enqueue_next_lines(fuse_entry_t *fuse, int fuse_nr, int empty_nr)
{
    logger_write_queue_t *wrq;

    /* This one should have been processed. Freeing it */
    if (fuse[0].ts != ~0) {
        wrq = fuse[0].wrq;

        /* Free this line for the writer thread */
        wrq->lines[wrq->rd_idx].ready = false;
        wrq->rd_idx = ++wrq->rd_seq % wrq->lines_nr;
        /* Enqueue the next line */
        empty_nr += set_queue_entry(wrq, &fuse[0]);

        bubble_fuse_up(fuse, fuse_nr); /* Find its place */
    }

    /* Let see if there is something new in the empty queues. */
    int rv = 0;
    for (int i = 0, last = fuse_nr - 1; i < empty_nr; i++) {
        rv += set_queue_entry(fuse[last].wrq, &fuse[last]);
        bubble_fuse_down(fuse, fuse_nr);
    }
    /* return the number of remaining empty queues */
    return rv;
}

static inline int init_lines_queue(fuse_entry_t *fuse, int fuse_nr)
{
    memset(fuse, 0, fuse_nr * sizeof(fuse_entry_t));

    for (int i = 0; i < fuse_nr; i++) {
        fuse[i].ts = ~0; /* Init all the queues as if they were empty */
        fuse[i].wrq = logger.queues[i];
    }
    return fuse_nr; /* Number of empty queues (all) */
}

static void *logger_reader_func(void)
{
    bool running = logger.running;
    fprintf(stderr, "<logger-thd-read> Starting...\n");

    while (running) {
        int empty_nr = 0;
        int really_empty = 0;
        int fuse_nr = logger.queues_nr;

        if (!fuse_nr) {
            fprintf(stderr,
                    "<logger-thd-read> Wake me up when there is something... "
                    "Zzz\n");
            atomic_store(&logger.waiting, 1);
            if (futex_wait(&logger.waiting, 1) < 0 && errno != EAGAIN) {
                fprintf(stderr, "<logger-thd-read> ERROR: %m !\n");
                break;
            }
            if (!logger.running)
                break;

            atomic_store(&logger.reload, 0);
            continue;
        }
        fuse_entry_t fuse_queue[fuse_nr];

        fprintf(stderr,
                "<logger-thd-read> (Re)Loading... fuse_entry_t = %d x "
                "%lu bytes (%lu bytes total)\n",
                fuse_nr, sizeof(fuse_entry_t), sizeof(fuse_queue));

        empty_nr = init_lines_queue(fuse_queue, fuse_nr);

        while (1) {
            empty_nr = enqueue_next_lines(fuse_queue, fuse_nr, empty_nr);

            if (atomic_compare_exchange_strong(&logger.reload, &(atomic_int){1},
                                               0)) {
                break;
            }
            if (fuse_queue[0].ts == ~0) {
                logger.empty = true;
                if (!logger.running) {
                    /* We want to terminate when all the queues are empty ! */
                    running = false;
                    break;
                }
                if (really_empty < 5) {
                    int wait = 1 << really_empty++;
                    fprintf(stderr,
                            "<logger-thd-read> Print queue empty. Double check "
                            "in %d us ...\n",
                            wait);
                    usleep(wait);
                    /* Double-check multiple times if the queue is really empty.
                     * This is avoid the writers to wakeup too frequently the
                     * reader in case of burst. Waking it up through the futex
                     * also takes time and the goal is to lower the time spent
                     * in logger_printf() as much as possible.
                     */
                    continue;
                }
                really_empty = 0;
                fprintf(stderr,
                        "<logger-thd-read> Print queue REALLY empty ... Zzz\n");
                atomic_store(&logger.waiting, 1);
                if (futex_wait(&logger.waiting, 1) < 0 && errno != EAGAIN) {
                    fprintf(stderr, "<logger-thd-read> ERROR: %m !\n");
                    running = false;
                    break;
                }
                continue;
            }
            logger.empty = false;
            really_empty = 0;

            logger_write_queue_t *wrq = fuse_queue[0].wrq;
            int rv = write_line(wrq, &wrq->lines[wrq->rd_idx]);
            if (rv < 0) {
                fprintf(stderr, "<logger-thd-read> logger_write_line(): %m\n");
                /* In this case we loose the line but we continue to empty the
                 * queues ... otherwise all the queues gets full and all the
                 * threads are stuck on it (this can happen if disk is full.)
                 */
            }
        }
    }
    fprintf(stderr, "<logger-thd-read> Exit\n");
    return NULL;
}
