#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
#include "timer.h"

/* the length of the struct epoll_events array pointed to by *events */
#define MAXEVENTS 1024

#define LISTENQ 1024

#define MAXWORKER 32

static const char short_options[] = "p:r:h";
static const struct option long_options[] = {{"port", 1, NULL, 'p'},
                                             {"root", 1, NULL, 'r'},
                                             {"help", 0, NULL, 'h'}};

typedef struct {
    int listenfd;
    char *root;
} worker_param;

static int open_listenfd(int port)
{
    int listenfd, optval = 1;

    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminate "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval,
                   sizeof(int)) < 0)
        return -1;

    /* Listenfd will be an endpoint for all requests to given port. */
    struct sockaddr_in serveraddr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((unsigned short) port),
        .sin_zero = {0},
    };
    if (bind(listenfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;

    return listenfd;
}

/* set a socket non-blocking. If a listen socket is a blocking socket, after
 * it comes out from epoll and accepts the last connection, the next accpet
 * will block unexpectedly.
 */
static int sock_set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_err("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    int s = fcntl(fd, F_SETFL, flags);
    if (s == -1) {
        log_err("fcntl");
        return -1;
    }
    return 0;
}

static void print_usage()
{
    printf(
        "Usage: sehttpd [options]\n"
        "Options:\n"
        "   -p, --port       port number to be specified\n"
        "   -r, --root       web page root to be specified\n"
        "   -h, --help       display this message\n");
    exit(0);
}

void server_loop(worker_param param)
{
    /* copy parameters */
    int listenfd = param.listenfd;
    char *root = param.root;

    /* create epoll and add listenfd */
    int epfd = epoll_create1(0 /* flags */);
    assert(epfd > 0 && "epoll_create1");

    struct epoll_event events[MAXEVENTS];
    assert(events && "epoll_event: malloc");

    http_request_t *request = malloc(sizeof(http_request_t));
    init_http_request(request, listenfd, epfd, root);

    struct epoll_event event = {
        .data.ptr = request,
        .events = EPOLLIN | EPOLLET,
    };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &event);

    timer_init();

    /* epoll_wait loop */
    while (1) {
        int time = find_timer();
        debug("wait time = %d", time);
        int n = epoll_wait(epfd, events, MAXEVENTS, time);
        handle_expired_timers();

        for (int i = 0; i < n; i++) {
            http_request_t *r = events[i].data.ptr;
            int fd = r->fd;
            if (listenfd == fd) {
                /* we hava one or more incoming connections */
                while (1) {
                    socklen_t inlen = 1;
                    struct sockaddr_in clientaddr;
                    int infd = accept(listenfd, (struct sockaddr *) &clientaddr,
                                      &inlen);
                    if (infd < 0) {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            /* we have processed all incoming connections */
                            break;
                        }
                        log_err("accept");
                        break;
                    }

                    int rc UNUSED = sock_set_non_blocking(infd);
                    assert(rc == 0 && "sock_set_non_blocking");

                    request = malloc(sizeof(http_request_t));
                    if (!request) {
                        log_err("malloc");
                        break;
                    }

                    init_http_request(request, infd, epfd, root);
                    event.data.ptr = request;
                    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, infd, &event);

                    add_timer(request, TIMEOUT_DEFAULT, http_close_conn);
                }
            } else {
                if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (!(events[i].events & EPOLLIN))) {
                    log_err("epoll error fd: %d", r->fd);
                    close(fd);
                    continue;
                }

                do_request(events[i].data.ptr);
            }
        }
    }
}

pid_t create_worker(worker_param param)
{
    pid_t pid = fork();
    if (pid < 0) {
        puts("fork() fail");
        return -1;
    } else if (pid > 0) {
        return pid;
    }

    server_loop(param);
    return 0;
}

int destroy_worker(pid_t pid)
{
    int status;
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    if (status < 0)
        return -1;
    return 0;
}

void sighandler(int sig)
{
    (void) sig;
    printf("Terminating web server.\n");
}

#define PORT 8081
#define WEBROOT "./www"

int main(int argc, char *argv[])
{
    /* when a fd is closed by remote, writing to this fd will cause system
     * send SIGPIPE to this process, which exit the program
     */
    if (sigaction(SIGPIPE,
                  &(struct sigaction){.sa_handler = SIG_IGN, .sa_flags = 0},
                  NULL)) {
        log_err("Failed to install sigal handler for SIGPIPE");
        return 0;
    }

    /* parsing the arguments */
    int port = PORT;
    char *root = WEBROOT;
    int next_option;
    do {
        next_option =
            getopt_long(argc, argv, short_options, long_options, NULL);
        switch (next_option) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'r':
            root = optarg;
            break;
        case 'h':
            print_usage();
            break;
        case -1:
            break;
        }
    } while (next_option != -1);

    int listenfd = open_listenfd(port);
    int rc UNUSED = sock_set_non_blocking(listenfd);
    assert(rc == 0 && "sock_set_non_blocking");


    /* copy the parameters */
    worker_param param = {.listenfd = listenfd, .root = root};
    /* create the childrend process */
    pid_t workers[MAXWORKER];
    for (int i = 0; i < MAXWORKER; i++) {
        pid_t pid;
        if ((pid = create_worker(param)))
            break;
        workers[i] = pid;
    }

    printf("Web server started.\n");

    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGKILL, sighandler);

    /* main process idle */
    pause();

    /* release the child process */
    for (int i = 0; i < MAXWORKER; i++)
        destroy_worker(workers[i]);

    return 0;
}
