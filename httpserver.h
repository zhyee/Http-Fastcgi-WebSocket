#ifndef HTTPSERVER_H
#define HTTPSERVER_H

typedef struct epoll_event_data epoll_event_data;
typedef struct Request Request;
typedef struct Response Response;
typedef enum parse_step parse_step;

typedef void (*event_callback) (epoll_event_data *);

enum parse_step {
    WAIT_CONNECT = 0,
    CONNECTED = 1,
    REQUEST_BEGIN = 2,
    READ_REQUEST_LINE = 3,
    PARSE_REQUEST_LINE = 4,
    READ_REQUEST_HEADER = 5,
    PARSE_REQUEST_HEADER  = 6,
    READ_REQUEST_BODY = 7,
    PARSE_REQUEST_BODY = 8,
    REQUEST_END = 9
};

struct epoll_event_data {
        int fd;
        parse_step step;
        char *databuf;
        int bufsize;
        int rdlen;
        int parselen;
        Request *request;
        Response *response;
        event_callback callback;
        struct epoll_event *ev;
        void (*release)(epoll_event_data *);
};

typedef struct HeaderEntry {
    int keylen;
    char keyvalue[];
} HeaderEntry;

struct Request {
    char method[16];
    char *request_uri;
    char protocol[16];
    HeaderEntry **headers;
    int connfd;
    void (*release)(Request *);
};

struct Response {
    int status_code;
    HeaderEntry **headers;
    char *body;
    void (*release)(Response *);
};


// 添加事件到 eventloop
int event_add(int epfd, int fd, uint32_t events, event_callback, epoll_event_data *epdata);

// 删除指定的fd关联的事件
int event_del(int epfd, int fd);

//建立链接事件
void accept_callback(epoll_event_data *epdata);

// 读取数据事件
void rcve_callback(epoll_event_data *epdata);

// 释放epdata
void release_epdata(epoll_event_data *epdata);

#endif
