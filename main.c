#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

typedef struct {
    unsigned long print_max;
    int thread_max;
    int lines_min, lines_max;
    int lines_total;
    int uwait;
    int chances;
    int opts;
} thread_params_t;

/* Tester thread */
static void *writer(const thread_params_t *thp)
{
    char th[LOGGER_MAX_THREAD_NAME_SZ];

    pthread_getname_np(pthread_self(), th, sizeof(th));

    for (int seq = 0; seq < thp->print_max; seq++) {
        if (!(rand() % thp->chances)) {
            fprintf(stderr, "<%s> Bad luck, waiting for %d us\n", th,
                    thp->uwait);
            usleep(thp->uwait);
        }
        struct timespec before, after;
        int level = rand() % LOGGER_LEVEL_COUNT;

        clock_gettime(CLOCK_MONOTONIC, &before);
        if (LOG_LEVEL(level, "<%s> %d", th, seq) < 0) {
            clock_gettime(CLOCK_MONOTONIC, &after);
            fprintf(stderr, "<%s> %d **LOST** (%m)\n", th, seq);
        } else {
            clock_gettime(CLOCK_MONOTONIC, &after);
        }

        fprintf(stderr, "<%s> %lu logger_printf took %lu ns\n", th,
                timespec_to_ns(after), elapsed_ns(before, after));
    }

    return NULL;
}

int main(int argc, char **argv)
{
    int start_wait = 0;

    if (argc < 7) {
        printf(
            "%s <threads> <min q lines> <max q lines> <total lines> <print "
            "max/thd> <us wait> <wait chances> [blocking (0)] [printlost (0)] "
            "[delay sec]\n",
            argv[0]);
        return 1;
    }

    thread_params_t thp = {
        .thread_max = atoi(argv[1]),
        .lines_min = atoi(argv[2]),
        .lines_max = atoi(argv[3]),
        .lines_total = atoi(argv[4]),
        .print_max = atoi(argv[5]),
        .uwait = atoi(argv[6]),
        .chances = atoi(argv[7]),
        .opts = LOGGER_OPT_PREALLOC,
    };
    if (argc > 8 && atoi(argv[8]))
        thp.opts |= LOGGER_OPT_NONBLOCK;
    if (argc > 9 && atoi(argv[9]) && (thp.opts & LOGGER_OPT_NONBLOCK))
        thp.opts |= LOGGER_OPT_PRINTLOST;
    if (argc > 10)
        start_wait = atoi(argv[10]);
    srand(time(NULL));

    fprintf(stderr, "cmdline: ");
    for (int i = 0; i < argc; i++)
        fprintf(stderr, "%s ", argv[i]);
    fprintf(stderr,
            "\nthreads[%d] q_min[%d] q_max[%d] lines_total[%d] "
            "max_lines/thr[%lu] (1/%d chances to wait %d us) %s%s\n",
            thp.thread_max, thp.lines_min, thp.lines_max, thp.lines_total,
            thp.print_max, thp.chances, thp.uwait,
            thp.opts & LOGGER_OPT_NONBLOCK ? "non-blocking" : "",
            thp.opts & LOGGER_OPT_PRINTLOST ? "+printlost" : "");
    fprintf(
        stderr,
        "Waiting for %d seconds after the logger-reader thread is started\n\n",
        start_wait);

    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    fprintf(stderr,
            "For reference, the call to fprintf(stderr,...) to print this line "
            "took: ");
    clock_gettime(CLOCK_MONOTONIC, &after);
    fprintf(stderr, "%lu ns\n\n", elapsed_ns(before, after));

    logger_init(thp.thread_max * 1.5, 50, 0);
    sleep(start_wait);

    /* Writer threads */
    pthread_t tid[thp.thread_max];
    char tnm[thp.thread_max][LOGGER_MAX_THREAD_NAME_SZ];
    unsigned long printed_lines = 0;

    for (int i = 0; i < thp.thread_max; i++) {
        int queue_size =
            thp.lines_min + rand() % (thp.lines_max - thp.lines_min + 1);

        snprintf(tnm[i], LOGGER_MAX_THREAD_NAME_SZ, "writer-thd-%04d", i);
        logger_pthread_create(tnm[i], queue_size, thp.opts, &tid[i], NULL,
                              (void *) writer, (void *) &thp);

        printed_lines += thp.print_max;
    }

    while (printed_lines < thp.lines_total) {
        for (int i = 0; i < thp.thread_max; i++) {
            if (tid[i] && pthread_tryjoin_np(tid[i], NULL))
                continue;

            if (printed_lines < thp.lines_total) {
                /* Not the right amount... Restart the exited thread */
                int queue_size = thp.lines_min +
                                 rand() % (thp.lines_max - thp.lines_min + 1);
                logger_pthread_create(tnm[i], queue_size, LOGGER_OPT_NONE,
                                      &tid[i], NULL, (void *) writer,
                                      (void *) &thp);
                printed_lines += thp.print_max;
                fprintf(stderr, "Restarting thread %02d ...\n", i);
                continue;
            }
            tid[i] = 0;
        }
        usleep(100);
    }

    for (int i = 0; i < thp.thread_max; i++) {
        if (tid[i]) /* If not yet terminated, waiting for it */
            pthread_join(tid[i], NULL);
    }
    logger_deinit();

    fprintf(stderr, "%lu total printed lines ...\n", printed_lines);
    return 0;
}
