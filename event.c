//
// Created by root on 1/5/18.
//

#include <event2/event.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

struct event_base *base;

void accept_cb(evutil_socket_t, short, void *);
void read_cb(evutil_socket_t, short, void *);
void write_cb(evutil_socket_t fd, short events, void *arg);



void accept_cb(evutil_socket_t fd, short events, void *arg)
{

    if (events & EV_READ)
    {
        int connfd;
        struct sockaddr_in caddr;
        socklen_t slen = sizeof(caddr);
        connfd = accept(fd, (struct sockaddr *)&caddr, &slen);
        printf("客户端连接服务器 fd : %d\n", connfd);
        struct event **ev = malloc(sizeof(struct event *));
        *ev = event_new(base, connfd, EV_READ | EV_PERSIST, read_cb, (void *)ev);
        event_add(*ev, NULL);
    }

}

void read_cb(evutil_socket_t fd, short events, void *arg)
{
    char *buf = calloc(1024, sizeof(char));
    ssize_t rdlen;
    int i, ret;
    if (events & EV_READ)
    {
        bzero(buf, sizeof(buf));

        rdlen = recv(fd, buf, 1024, MSG_DONTWAIT);

        if (rdlen == -1 && errno == EAGAIN)
        {

        }
        else if (rdlen == 0)
        {
            close(fd);
            struct event **ev = (struct event **)arg;
            ret = event_del(*ev);
            if (ret == -1)
            {
                printf("event_del fail\n");
            }
            event_free(*ev);
            free(ev);
            printf("fd = %d 关闭 \n", fd);
        }
        else if (rdlen > 0)
        {
            printf("read from fd: %d, %s\n", fd, buf);

            for(i = 0; i < rdlen; i++)
            {
                if(buf[i] >= 'a' && buf[i] <= 'z')
                {
                    buf[i] -= 32;
                }
            }

            struct event *ev = event_new(base, fd, EV_WRITE, write_cb, (void *)buf);
        }

    }
}

void write_cb(evutil_socket_t fd, short events, void *arg)
{

}




int main()
{
    int servfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in saddr;

    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(8888);
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bind(servfd, (struct sockaddr *)&saddr, sizeof(saddr));

    listen(servfd, 1024);


    struct event *ev;

    base = event_base_new();
    ev = event_new(base, servfd, EV_READ | EV_PERSIST, accept_cb, NULL);

    event_add(ev, NULL);

    event_base_dispatch(base);

}
