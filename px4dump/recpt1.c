/* -*- tab-width: 4; indent-tabs-mode: nil -*- */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <ctype.h>
#include <libgen.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/msg.h>

#include <sys/ioctl.h>
#include <sys/uio.h>
#include "pt1_ioctl.h"

#include "config.h"
#include "recpt1core.h"
#include "recpt1.h"

/* maximum write length at once */
#define SIZE_CHANK 1316

/* globals */
extern boolean f_exit;

QUEUE_T *
create_queue(size_t size)
{
    QUEUE_T *p_queue;
    int memsize = sizeof(QUEUE_T) + size * sizeof(BUFSZ*);

    p_queue = (QUEUE_T*)calloc(memsize, sizeof(char));

    if(p_queue != NULL) {
        p_queue->size = size;
        p_queue->num_avail = size;
        p_queue->num_used = 0;
        pthread_mutex_init(&p_queue->mutex, NULL);
        pthread_cond_init(&p_queue->cond_avail, NULL);
        pthread_cond_init(&p_queue->cond_used, NULL);
    }

    return p_queue;
}

void
destroy_queue(QUEUE_T *p_queue)
{
    if(!p_queue)
        return;

    pthread_mutex_destroy(&p_queue->mutex);
    pthread_cond_destroy(&p_queue->cond_avail);
    pthread_cond_destroy(&p_queue->cond_used);
    free(p_queue);
}

/* enqueue data. this function will block if queue is full. */
void
enqueue(QUEUE_T *p_queue, BUFSZ *data)
{
    struct timeval now;
    struct timespec spec;
    int retry_count = 0;

    pthread_mutex_lock(&p_queue->mutex);
    /* entered critical section */

    /* wait while queue is full */
    while(p_queue->num_avail == 0) {

        gettimeofday(&now, NULL);
        spec.tv_sec = now.tv_sec + 1;
        spec.tv_nsec = now.tv_usec * 1000;

        pthread_cond_timedwait(&p_queue->cond_avail,
                               &p_queue->mutex, &spec);
        retry_count++;
        if(retry_count > 60) {
            f_exit = TRUE;
        }
        if(f_exit) {
            pthread_mutex_unlock(&p_queue->mutex);
            return;
        }
    }

    p_queue->buffer[p_queue->in] = data;

    /* move position marker for input to next position */
    p_queue->in++;
    p_queue->in %= p_queue->size;

    /* update counters */
    p_queue->num_avail--;
    p_queue->num_used++;

    /* leaving critical section */
    pthread_mutex_unlock(&p_queue->mutex);
    pthread_cond_signal(&p_queue->cond_used);
}

/* dequeue data. this function will block if queue is empty. */
BUFSZ *
dequeue(QUEUE_T *p_queue)
{
    struct timeval now;
    struct timespec spec;
    BUFSZ *buffer;
    int retry_count = 0;

    pthread_mutex_lock(&p_queue->mutex);
    /* entered the critical section*/

    /* wait while queue is empty */
    while(p_queue->num_used == 0) {

        gettimeofday(&now, NULL);
        spec.tv_sec = now.tv_sec + 1;
        spec.tv_nsec = now.tv_usec * 1000;

        pthread_cond_timedwait(&p_queue->cond_used,
                               &p_queue->mutex, &spec);
        retry_count++;
        if(retry_count > 60) {
            f_exit = TRUE;
        }
        if(f_exit) {
            pthread_mutex_unlock(&p_queue->mutex);
            return NULL;
        }
    }

    /* take buffer address */
    buffer = p_queue->buffer[p_queue->out];

    /* move position marker for output to next position */
    p_queue->out++;
    p_queue->out %= p_queue->size;

    /* update counters */
    p_queue->num_avail++;
    p_queue->num_used--;

    /* leaving the critical section */
    pthread_mutex_unlock(&p_queue->mutex);
    pthread_cond_signal(&p_queue->cond_avail);

    return buffer;
}

/* this function will be reader thread */
void *
reader_func(void *p)
{
    thread_data *tdata = (thread_data *)p;
    QUEUE_T *p_queue = tdata->queue;
    int wfd = tdata->wfd;
    pthread_t signal_thread = tdata->signal_thread;
    BUFSZ *qbuf;

    struct {
        uint8_t *data;
        int32_t  size;
    } sbuf, buf;

    buf.size = 0;
    buf.data = NULL;

    while(1) {
        ssize_t wc = 0;
        int file_err = 0;
        qbuf = dequeue(p_queue);
        /* no entry in the queue */
        if(qbuf == NULL) {
            break;
        }

        sbuf.data = qbuf->buffer;
        sbuf.size = qbuf->size;

        buf = sbuf; /* default */

        /* write data to output file */
        int size_remain = buf.size;
        int offset = 0;

        while(size_remain > 0) {
            int ws = size_remain < SIZE_CHANK ? size_remain : SIZE_CHANK;

            wc = write(wfd, buf.data + offset, ws);
            if(wc < 0) {
                perror("write");
                file_err = 1;
                pthread_kill(signal_thread,
                             errno == EPIPE ? SIGPIPE : SIGUSR2);
                break;
            }
            size_remain -= wc;
            offset += wc;
        }

        free(qbuf);
        qbuf = NULL;

        /* normal exit */
        if((f_exit && !p_queue->num_used) || file_err) {

            buf = sbuf; /* default */

            if(!file_err) {
                wc = write(wfd, buf.data, buf.size);
                if(wc < 0) {
                    perror("write");
                    file_err = 1;
                    pthread_kill(signal_thread,
                                 errno == EPIPE ? SIGPIPE : SIGUSR2);
                }
            }

            break;
        }
    }

    time_t cur_time;
    time(&cur_time);
    fprintf(stderr, "Recorded %dsec\n",
            (int)(cur_time - tdata->start_time));

    return NULL;
}

void
show_usage(char *cmd)
{
    fprintf(stderr, "Usage: \n%s [--device devicefile] [--lnb voltage] channel\n", cmd);
    fprintf(stderr, "\n");
}

void
show_options(void)
{
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "--device devicefile: Specify devicefile to use\n");
    fprintf(stderr, "--lnb voltage:       Specify LNB voltage (0, 11, 15)\n");
    fprintf(stderr, "--help:              Show this help\n");
    fprintf(stderr, "--version:           Show version\n");
    fprintf(stderr, "--list:              Show channel list\n");
}

void
cleanup(thread_data *tdata)
{
    /* stop recording */
    ioctl(tdata->tfd, STOP_REC, 0);

    f_exit = TRUE;

    pthread_cond_signal(&tdata->queue->cond_avail);
    pthread_cond_signal(&tdata->queue->cond_used);
}

/* will be signal handler thread */
void *
process_signals(void *t)
{
    sigset_t waitset;
    int sig;
    thread_data *tdata = (thread_data *)t;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIGPIPE);
    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGTERM);
    sigaddset(&waitset, SIGUSR1);
    sigaddset(&waitset, SIGUSR2);

    sigwait(&waitset, &sig);

    switch(sig) {
    case SIGPIPE:
        fprintf(stderr, "\nSIGPIPE received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGINT:
        fprintf(stderr, "\nSIGINT received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGTERM:
        fprintf(stderr, "\nSIGTERM received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGUSR1: /* normal exit*/
        cleanup(tdata);
        break;
    case SIGUSR2: /* error */
        fprintf(stderr, "Detected an error. cleaning up...\n");
        cleanup(tdata);
        break;
    }

    return NULL; /* dummy */
}

void
init_signal_handlers(pthread_t *signal_thread, thread_data *tdata)
{
    sigset_t blockset;

    sigemptyset(&blockset);
    sigaddset(&blockset, SIGPIPE);
    sigaddset(&blockset, SIGINT);
    sigaddset(&blockset, SIGTERM);
    sigaddset(&blockset, SIGUSR1);
    sigaddset(&blockset, SIGUSR2);

    if(pthread_sigmask(SIG_BLOCK, &blockset, NULL))
        fprintf(stderr, "pthread_sigmask() failed.\n");

    pthread_create(signal_thread, NULL, process_signals, tdata);
}

int
main(int argc, char **argv)
{
    pthread_t signal_thread;
    pthread_t reader_thread;
    QUEUE_T *p_queue = create_queue(MAX_QUEUE);
    BUFSZ   *bufptr;
    static thread_data tdata;
    tdata.lnb = 0;

    int result;
    int option_index;
    struct option long_options[] = {
        { "LNB",       1, NULL, 'n'},
        { "lnb",       1, NULL, 'n'},
        { "device",    1, NULL, 'd'},
        { "help",      0, NULL, 'h'},
        { "version",   0, NULL, 'v'},
        { "list",      0, NULL, 'l'},
        {0, 0, NULL, 0} /* terminate */
    };

    char *device = NULL;
    int val;
    char *voltage[] = {"0V", "11V", "15V"};

    while((result = getopt_long(argc, argv, "n:d:hvl:",
                                long_options, &option_index)) != -1) {
        switch(result) {
        case 'h':
            fprintf(stderr, "\n");
            show_usage(argv[0]);
            fprintf(stderr, "\n");
            show_options();
            fprintf(stderr, "\n");
            show_channels();
            fprintf(stderr, "\n");
            exit(0);
            break;
        case 'v':
            fprintf(stderr, "%s %s\n", argv[0], version);
            fprintf(stderr, "recorder command for PT1/2/3 digital tuner.\n");
            exit(0);
            break;
        case 'l':
            show_channels();
            exit(0);
            break;
        /* following options require argument */
        case 'n':
            val = atoi(optarg);
            switch(val) {
            case 11:
                tdata.lnb = 1;
                break;
            case 15:
                tdata.lnb = 2;
                break;
            default:
                tdata.lnb = 0;
                break;
            }
            fprintf(stderr, "LNB = %s\n", voltage[tdata.lnb]);
            break;
        case 'd':
            device = optarg;
            fprintf(stderr, "using device: %s\n", device);
            break;
        }
    }

    if(argc - optind < 1) {
        fprintf(stderr, "Arguments are necessary!\n");
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }
    if(argc - optind > 1) {
        fprintf(stderr, "Too many arguments are exists!\n");
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }

    fprintf(stderr, "pid = %d\n", getpid());

    /* tune */
    if(tune(argv[optind], &tdata, device) != 0)
        return 1;

    tdata.wfd = 1; /* stdout */

    while(1){
        /* prepare thread data */
        tdata.queue = p_queue;
        tdata.tune_persistent = FALSE;

        /* spawn signal handler thread */
        init_signal_handlers(&signal_thread, &tdata);

        /* spawn reader thread */
        tdata.signal_thread = signal_thread;
        pthread_create(&reader_thread, NULL, reader_func, &tdata);

        /* start recording */
        if(ioctl(tdata.tfd, START_REC, 0) < 0) {
            fprintf(stderr, "Tuner cannot start recording\n");
            return 1;
        }

        fprintf(stderr, "\nRecording...\n");

        time(&tdata.start_time);

        /* read from tuner */
        while(1) {
            bufptr = malloc(sizeof(BUFSZ));
            if(!bufptr) {
                break;
            }
            bufptr->size = read(tdata.tfd, bufptr->buffer, MAX_READ_SIZE);
            if(bufptr->size <= 0) {
                free(bufptr);
                continue;
            }
            enqueue(p_queue, bufptr);
        }

        pthread_kill(signal_thread, SIGUSR1);

        /* wait for threads */
        pthread_join(reader_thread, NULL);
        pthread_join(signal_thread, NULL);

        /* close tuner */
        if(close_tuner(&tdata) != 0)
            return 1;

        /* release queue */
        destroy_queue(p_queue);

        return 0;
    }
}
