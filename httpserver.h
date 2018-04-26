#ifndef HTTPSERVER_H
#define HTTPSERVER_H


#define MAX(m, n)  m > n ? m : n
#define MIN(m, n)  m < n ? m : n

typedef struct Http Http;
typedef struct Request Request;
typedef struct Response Response;
typedef enum parse_step parse_step;

typedef void (*event_callback) (Http *);

enum parse_step {
    WAIT_CONNECT = 0,
    CONNECTED = 1,
    REQUEST_BEGIN = 2,
    READ_REQUEST_LINE = 3,
    READ_REQUEST_HEADER = 4,
    READ_REQUEST_BODY = 5,
    SEND_STATUS_LINE = 6,
    SEND_REPONSE_HEADER = 7,
    SEND_RESPONSE_BODY = 8,
    REQUEST_END = 9
};

struct Http {
        int fd;
        parse_step step;
        char *databuf;
        int bufsize;
        int rdlen;      // 当前databuf中读到的数据长度
        int lastrdlen;  //上一次读到的数据长度
        int parsepos;   // 解析到数据的位置
        Request *request;
        Response *response;
        event_callback callback;
        struct epoll_event *ev;
        void (*release)(Http *);
};

typedef struct header_entry {
    int keylen;
    char keyvalue[];
} header_entry;

typedef struct http_headers {
    int hdsize;
    int hdused;   
    header_entry *entries[];
} http_headers;

struct Request {
    char method[16];
    char *request_uri;
    char protocol[16];
    int content_length;
    http_headers *headers;
    int fd;
    void (*release)(Request *);
};

struct Response {
    char protocol[16];
    int  status_code;
    char *reason_phrase;
    header_entry **headers;
    char *body;
    void (*release)(Response *);
};


// 添加事件到 eventloop
int event_add(int epfd, int fd, uint32_t events, event_callback, Http *http);

// 删除指定的fd关联的事件
int event_del(int epfd, int fd);

//建立链接事件
void accept_callback(Http *http);

// 读取数据事件
void rcve_callback(Http *http);

// 释放http
void release_http(Http *http);

// 扩展headers容量
void expand_headers(http_headers *headers);

int status_codes[] = {
    200,
    403,
    404
};

char *reason_phrases[] = {
    "OK",
    "Forbidden",
    "Not Found"
};


#endif
