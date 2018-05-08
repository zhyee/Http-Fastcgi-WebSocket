#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>

#include "httpserver.h"

#define HDSIZE  16

#define BUF_SIZE 1
#define IP_LEN 16

#define MAXEVENTS 32

int epfd;
const char *webroot = "/opt/www";

// 从字符串中拷贝长度n的子串
char *strndup(const char *src, size_t n) {
    char *dst = malloc(n+1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
}

char *stringcat(const char *str1, int len1, const char *str2, int len2) {
    char *str = malloc(len1 + len2 + 1);
    memcpy(str, str1, len1);
    memcpy(str + len1, str2, len2);
    str[len1+len2] = '\0';
    return str;
}

// 追加字符串，必要时扩展缓冲区
char *strappend(char *buf, int *datalen, int *bufsize, const char *str) {
    int len = strlen(str);

    if (*bufsize - *datalen < len) {
        do {
            *bufsize *= 2;
        } while (*bufsize - *datalen < len);
        buf = realloc(buf, *bufsize);
    }

    memcpy(buf + (*datalen), str, len);
    *datalen += len;
    return buf;
}

// 释放 Http
void release_http(Http *http) {
    if (http == NULL) {
        return;
    }

    if (http->ev) {
        free(http->ev);
    }

    if (http->databuf) {
        free(http->databuf);
    }

    if (http->request) {
        http->request->release(http->request);
    }

    if (http->response) {
        http->response->release(http->response);
    }

    free(http);
}

// 释放 Request 对象
void release_request(Request *request) {
    if (request == NULL) {
        return;
    }

    if (request->request_uri) {
        free(request->request_uri);
    }

    if (request->request_filename) {
        free(request->request_filename);
    }

    int i;
    if (request->headers) {
        if (request->headers->hdused > 0) {
            for(i = 0; i < request->headers->hdused; i++) {
                free(request->headers->entries[i]);
            }
        }

        free(request->headers);
    }

    free(request);
}

void release_response(Response *response) {
    if (response == NULL) {
        return;
    }

    if (response->databuf) {
        free(response->databuf);
    }

    free(response);
}


void accept_callback(Http *http) {
    int cfd, sfd = http->fd;
    struct sockaddr_in caddr;
    int addrlen = sizeof(caddr);
       
    cfd = accept(sfd, (struct sockaddr *)&caddr, &addrlen);
    
    if (cfd == -1){
        fprintf(stderr, "accept error:%s\n", strerror(errno));
        return;
    }
    char ip[IP_LEN] = {'\0'};
    inet_ntop(AF_INET, &caddr.sin_addr.s_addr, ip, IP_LEN);
    printf("client connected, fd = %d, ip:%s, port: %d\n", cfd, ip, ntohs(caddr.sin_port));

    // 创建一个新的http对象处理请求
    Http *newhttp = malloc(sizeof(Http));
    bzero(newhttp, sizeof(*newhttp));
    newhttp->step = CONNECTED;
    newhttp->release = release_http;
    newhttp->databuf = malloc(BUF_SIZE);
    newhttp->bufsize = BUF_SIZE;

    event_add(epfd, cfd, EPOLLIN, rcve_callback, newhttp);

}

// 解析请求行
int parse_request_line(Http *http) {
    if (http->request == NULL) {
        http->request = malloc(sizeof(Request));
        bzero(http->request, sizeof(*http->request));
        http->request->release = release_request;
    }
    int i, lastpos = 0, endpos = 0;

    if (http->parsepos == 0) {
        fprintf(stderr, "parse request line error\n");
        http->release(http);
        event_del(epfd, http->fd);
        close(http->fd);
        return -1;
    }

    if (http->databuf[http->parsepos-1] == '\r') {  // \r\n
        endpos = http->parsepos - 2;
    } else {
        endpos = http->parsepos - 1;    // \n
    }

    for(i = 0; i <= endpos; i++) {
         if (http->databuf[i] == ' ') {
            if (lastpos == 0) {
                memcpy(http->request->method, http->databuf, i - lastpos);
                lastpos = i + 1;
            } else {
                http->request->request_uri = strndup(http->databuf+lastpos, i - lastpos);
                lastpos = i + 1;

                memcpy(http->request->protocol, http->databuf+lastpos, endpos-lastpos+1);
                break;
            }
         }
    }

    for(i = 0; i < strlen(http->request->request_uri); i++) {
        if (http->request->request_uri[i] == '?') {
            http->request->request_filename = stringcat(webroot, strlen(webroot), http->request->request_uri, i);
            goto REQUEST_LINE_END;
        }
    }

    http->request->request_filename = stringcat(webroot, strlen(webroot), http->request->request_uri, strlen(http->request->request_uri));

REQUEST_LINE_END:
    //移除已解析的数据
    http->rdlen = http->rdlen - 1 - http->parsepos;
    http->lastrdlen = 0; 
    memmove(http->databuf, http->databuf+http->parsepos+1, http->rdlen);

    printf("method = %s, protocol = %s, request_uri = %s, request_filename = %s\n", http->request->method, http->request->protocol, http->request->request_uri, http->request->request_filename);
    return 0;
}

header_entry *create_header_entry(const char *line, size_t len) {
    header_entry *entry = malloc(sizeof(header_entry) + len + 2);
    bzero(entry, sizeof(header_entry) + len + 2);

    int i, j;

    for(i = 0; i < len; i++) {
        if (line[i] == ':') {
            entry->keylen = i;
            memcpy(entry->keyvalue, line, i);
            entry->keyvalue[i] = '=';
            break;
        }
    }

    for(j = i+1; j < len; j++) {
        if (line[j] != ' ') {
            memcpy(entry->keyvalue+i+1, line+j, len - j);
            break;
        }
    }

    return entry;
}

int parse_header(Http *http) {
     if (http->request->headers == NULL) {
        http->request->headers = malloc(sizeof(http_headers) + HDSIZE * sizeof(header_entry *));
        http->request->headers->hdsize = HDSIZE;
        http->request->headers->hdused = 0;
     }

    int i;
    int lastpos = 0;

    printf("header: %.*s, parsepos = %d\n", http->parsepos+1, http->databuf, http->parsepos);

    for(i = 0; i < http->parsepos; i++) {
        if (http->request->headers->hdsize == http->request->headers->hdused) {
            expand_headers(http->request->headers);
        }
        if (http->databuf[i] == '\n') {
              if (http->databuf[i-1] == '\r') {
                      printf("i = %d,lastpos = %d, http->request->headers->hdused = %d\n", i, lastpos, http->request->headers->hdused);
                http->request->headers->entries[http->request->headers->hdused++] = create_header_entry(http->databuf+lastpos, i - 1 - lastpos);
                    printf("create_header_end\n");
              } else {
                http->request->headers->entries[http->request->headers->hdused++] = create_header_entry(http->databuf+lastpos, i - lastpos);
              }
              lastpos = i+1;
        }
    }

    //移除已解析的数据
    http->rdlen = http->rdlen - http->parsepos - 1;
    if (http->rdlen > 0) {
        memmove(http->databuf, http->databuf+ http->parsepos + 1, http->rdlen);
    }
    http->parsepos = 0;

    for(i = 0; i < http->request->headers->hdused; i++) {
        printf("kayvalue = %s, keylen = %d\n", http->request->headers->entries[i]->keyvalue, http->request->headers->entries[i]->keylen);
    }

    return 0;
}

const char *get_header(Http *http, const char *header_name) {
    int i;
    http_headers *headers = http->request->headers;
    header_entry *entry;
    for(i = 0; i < headers->hdused; i++) {
        entry = headers->entries[i];
        if (strlen(header_name) == entry->keylen && strncmp(entry->keyvalue, header_name, entry->keylen) == 0) {
            return entry->keyvalue + entry->keylen + 1;
        }
    }

    return NULL;
}

void expand_databuf(Http *http) {
    http->databuf = realloc(http->databuf, http->bufsize * 2);
    http->bufsize *= 2;  // assume not over flow int
}

void expand_headers(http_headers *headers) {
    headers = realloc(headers, sizeof(*headers) + headers->hdsize  * sizeof(header_entry *));
    headers->hdsize *= 2;
}

void rcve_callback(Http *http) {
    if (http->step < CONNECTED) {
        fprintf(stderr, "not connected\n");
        return;
    } else if (http->step == CONNECTED) {
            //建立连接后的第一个请求
        http->step = REQUEST_BEGIN;
    }

    if (http->step == REQUEST_BEGIN) {
           // 初始化缓冲区  
            http->rdlen = 0;
            http->step = READ_REQUEST_LINE;
    }

    //缓冲区读满 扩充缓冲区
    if (http->rdlen == http->bufsize) {
        expand_databuf(http);
    }

    int i;
    int fd = http->fd;
    size_t rdlen;
    int parse_ret = -9;
    const char *content_length;
    int length;

    char buf[BUF_SIZE];
    
    rdlen = recv(fd, http->databuf+http->rdlen, http->bufsize - http->rdlen, MSG_DONTWAIT);


    if (rdlen == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "recv error:%s\n", strerror(errno));
                    event_del(epfd, fd);
                    http->release(http);
                    close(fd);
                    return;
            }
    } else if (rdlen == 0) {
            //客户端关闭
            fprintf(stderr, "客户端断开连接:fd = %d\n", fd);
            event_del(epfd, fd);
            //http->release(http);
            close(fd);
            return;
    }

    http->lastrdlen = http->rdlen;
    http->rdlen += rdlen;
    printf("######## read data: %.*s, fd = %d #######\n", http->rdlen, http->databuf, fd);

NEXT_SWITCH:
    switch (http->step) {
        //解析请求行
        case READ_REQUEST_LINE:   
                for(i = http->lastrdlen; i < http->rdlen; i++) {
                        if (strncmp(http->databuf + i, "\n", 1) == 0) {
                                // 读到请求行结束标计， 解析请求行
                                http->parsepos = i;
                                if (parse_request_line(http) ==  0) {
                                        http->step = READ_REQUEST_HEADER;
                                        goto NEXT_SWITCH;
                                } else {
                                    // parse error
                                    fprintf(stderr, "parse request line error\n");
                                }
                        }
                }

                // 没读到请求行结束标记，继续下一次读取
                break;

        case READ_REQUEST_HEADER:
                printf("bufsize: %d--------rdlen:%d\n", http->bufsize, http->rdlen);
            if (http->lastrdlen >= 3) {
                i = http->lastrdlen - 3;
            } else {
                i = 0;
            }


            for (; i < http->rdlen - 1; i++) {
                if (strncmp(http->databuf + i, "\r\n", 2) == 0) {
                    if (i < http->rdlen - 3 && strncmp(http->databuf + i + 2, "\r\n", 2) == 0) {
                        http->parsepos = i+3;
                        parse_ret = parse_header(http);
                    }
                } else if (strncmp(http->databuf + i, "\n\n", 2) == 0) {
                    http->parsepos = i+1;
                    parse_ret = parse_header(http);
                }
            }


            if (parse_ret == 0) {
                    // POST | PUT
                if (strcmp(http->request->method, "POST") == 0 || strcmp(http->request->method, "PUT") == 0) {
                    content_length = get_header(http, "Content-Length");
                    if (content_length != NULL) {
                        length = atoi(content_length);
                        printf("content-length: %d\n", length);
                        if (length > 0) {
                            http->request->content_length = length;
                            http->step = READ_REQUEST_BODY;
                            goto NEXT_SWITCH;
                        }  
                    }
                }

                http->step = SEND_STATUS_LINE;
                goto NEXT_SWITCH;
            }
            
            break;

         case READ_REQUEST_BODY:
            // body读完
            if (http->rdlen >= http->request->content_length) {
                printf("body:%.*s\n", http->rdlen, http->databuf);

                http->step = SEND_STATUS_LINE;
                goto NEXT_SWITCH;    
            }

            break;

         case SEND_STATUS_LINE:
            
            while(recv(fd, buf, BUF_SIZE, MSG_DONTWAIT) > 0); //读完该请求的多余数据
            
            //  write response
            event_modify(epfd, EPOLLOUT, send_callback, http);

            break;
    }

}

void render_status_line(Http *http) {
        strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, http->request->protocol);
        strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, " ");

        if (access(http->request->request_filename, F_OK) != 0) {
                strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "404 Not Found\r\n");
                http->response->status_code = 404;
        } else if (access(http->request->request_filename, R_OK) != 0) {
                strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "403 Forbidden\r\n");
                http->response->status_code = 403;
        } else {
                strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "200 OK\r\n");
                http->response->status_code = 200;
        }

        http->step = SEND_REPONSE_HEADER;
}

void render_response_header(Http *http) {
        struct stat stbuf;
        char header_buf[64] = {'\0'};
        int i, lastcommapos = -1;

        strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "Server: httpserver/0.1\r\n");
        if (strncmp(http->request->protocol, "HTTP/1.1", strlen("HTTP/1.1")) == 0 
                        && get_header(http, "Connection") 
                        && strncmp(get_header(http, "Connection"), "keep-alive", strlen("keep-alive")) == 0) {
                http->is_keepalive = 1;
                strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "Connection: keep-alive\r\n");
        }

        if (http->response->status_code == 200) {

            lstat(http->request->request_filename, &stbuf);
            http->response->content_length = stbuf.st_size;
            sprintf(header_buf, "Content-Length: %lu\r\n", stbuf.st_size);
            strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, header_buf);
            http->response->req_fd = open(http->request->request_filename, O_RDONLY);
           
            http->step = SEND_RESPONSE_BODY;

            for(i = 0; i < strlen(http->request->request_filename); i++) {
                    if (http->request->request_filename[i] == '.') {
                            lastcommapos = i;
                    }
            }

            if (lastcommapos != -1) {
                    for(i=0; i< (sizeof(ext_names) / sizeof(char *)); i++) {
                            if (strcmp(http->request->request_filename + lastcommapos + 1, ext_names[i]) == 0) {
                                    strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "Content-Type: ");               
                                    strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, mimes[i]);               
                                    strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "\r\n");
                                    goto RENDER_END;  
                            }
                    }
            }

            strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "Content-Type: text/plain\r\n");
        } else { 
            strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "Content-Length: 0\r\n");
            http->step = REQUEST_END;
        }

RENDER_END:
        strappend(http->response->databuf, &http->response->datalen, &http->response->bufsize, "\r\n");
        return;
}


void send_callback(Http *http) {
    ssize_t sndlen;

    switch(http->step) {
            case SEND_STATUS_LINE:
                    if (http->response == NULL) {
                        http->response = malloc(sizeof(Response));
                        bzero(http->response, sizeof(*http->response));
                        http->response->databuf = malloc(BUF_SIZE);
                        http->response->bufsize = BUF_SIZE;
                        http->response->datalen = 0;
                        http->response->release = release_response;
                    }
                    render_status_line(http);
                    break;

            case SEND_REPONSE_HEADER:
                render_response_header(http);
                break;

            case SEND_RESPONSE_BODY:
                // 响应头发送完毕 才能发送响应体
                if (http->response->datalen == 0) {
                    sndlen = sendfile(http->fd, http->response->req_fd, &http->response->offset, http->response->content_length - http->response->offset);

                    if (sndlen == -1) {
                        fprintf(stderr, "send fail: %s\n", strerror(errno));
                    }

                    http->response->sendlen += sndlen;

                    printf("sendlen = %d, offset = %d\n", http->response->sendlen, http->response->offset);

                    if (http->response->sendlen >= http->response->content_length) {
                        close(http->response->req_fd);
                        http->step = REQUEST_END;
                    }
                }
                break;

            case REQUEST_END:
                    // 监听下一个请求
                    http->step = REQUEST_BEGIN;
                    if (http->request) {
                        http->request->release(http->request);
                    }

                    if (http->response) {
                        http->response->release(http->response);
                    }

                    http->request = NULL;
                    http->response = NULL;

                    event_modify(epfd, EPOLLIN, rcve_callback, http);

                break;

    }

    if (http->response != NULL && http->response->datalen > 0) {
        sndlen = send(http->fd, http->response->databuf, http->response->datalen, MSG_DONTWAIT);
        if (sndlen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "send error:%s\n", strerror(errno));
            event_del(epfd, http->fd);
            http->release(http);
            close(http->fd);
            return;
        }
        
        // 清除已发送的数据
        if (sndlen > 0) {
            http->response->datalen -= sndlen;
            memmove(http->response->databuf, http->response->databuf+sndlen, http->response->datalen);
        }
    }
}

int event_add(int epfd, int fd, uint32_t events, event_callback callback, Http *http) {    
    struct epoll_event *epev = malloc(sizeof(struct epoll_event));

    http->fd = fd;
    http->callback = callback;
    http->ev = epev;
    
    epev->events = events;
    epev->data.ptr = http;

    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, epev);
}

int event_modify(int epfd, uint32_t events, event_callback callback, Http *http) {
    http->callback = callback;
    http->ev->events = events;
    http->ev->data.ptr = http;
    
    return epoll_ctl(epfd, EPOLL_CTL_MOD, http->fd, http->ev);
}

int event_del(int epfd, int fd) {
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}



int main()
{
    int sfd, reuse = 1;
    int i;
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (int *)&reuse, sizeof(int));

    struct sockaddr_in servaddr, cliaddr;
    int addrlen = sizeof(cliaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8899);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(sfd, (struct sockaddr *)&servaddr, sizeof(servaddr));	
    listen(sfd, 511);

    epfd = epoll_create(1024);
    
    Http *http = malloc(sizeof(Http));
    bzero(http, sizeof(*http));
    http->step = WAIT_CONNECT;
    event_add(epfd, sfd, EPOLLIN, accept_callback, http);
    
    struct epoll_event *events = malloc(MAXEVENTS * sizeof(struct epoll_event));
    struct epoll_event readyev;
    Http *readyhttp;

    while(1) {

            int evcount = epoll_wait(epfd, events, MAXEVENTS, -1); // wait infinit

            if (-1 == evcount) {
                    //被信号打断
                if (errno == EINTR)  {
                    continue;
                }
                fprintf(stderr, "epoll_wait error:%s\n", strerror(errno));
                continue;
            }

            for (i = 0; i < evcount; i++) {
                readyev = events[i];
                if (readyev.data.ptr != NULL) {
                    readyhttp = (Http *)readyev.data.ptr;
                    readyhttp->callback(readyhttp);
                }
            }
   
    }
}
