#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>

#include "httpserver.h"

#define HDSIZE  16

#define BUF_SIZE 1
#define IP_LEN 16

#define MAXEVENTS 32

int epfd;

// 从字符串中拷贝长度n的子串
char *strndup(const char *src, size_t n) {
    char *dst = malloc(n+1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
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
    printf("client connected, ip:%s, port: %d\n", ip, ntohs(caddr.sin_port));

    // 创建一个新的http
    Http *newhttp = malloc(sizeof(Http));
    bzero(newhttp, sizeof(*newhttp));
    newhttp->step = CONNECTED;
    newhttp->release = release_http;

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

    //移除已解析的数据
    http->rdlen = http->rdlen - 1 - http->parsepos;
    http->lastrdlen = 0; 
    memmove(http->databuf, http->databuf+http->parsepos+1, http->rdlen);

    printf("method = %s, protocol = %s, request_uri = %s\n", http->request->method, http->request->protocol, http->request->request_uri);
    return 0;
}

header_entry *create_header_entry(const char *line, size_t len) {
    header_entry *entry = malloc(sizeof(header_entry) + len + 2);
    bzero(entry, sizeof(*entry));
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

    for(i = 0; i < http->parsepos; i++) {
        if (http->request->headers->hdsize == http->request->headers->hdused) {
            expand_headers(http->request->headers);
        }
        if (http->databuf[i] == '\n') {
              if (http->databuf[i-1] == '\r') {
                http->request->headers->entries[http->request->headers->hdused++] = create_header_entry(http->databuf+lastpos, i - 1 - lastpos);
              } else {
                http->request->headers->entries[http->request->headers->hdused++] = create_header_entry(http->databuf+lastpos, i - lastpos);
              }
              lastpos = i+1;
        }
    }

    //移除已解析的数据
    http->rdlen = http->rdlen - http->parsepos - 1;
    memmove(http->databuf, http->databuf+ http->parsepos + 1, http->rdlen);
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
            http->databuf = malloc(BUF_SIZE);
            http->bufsize = BUF_SIZE;
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
    
    rdlen = recv(fd, http->databuf+http->rdlen, http->bufsize - http->rdlen, MSG_DONTWAIT);

    if (rdlen == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "recv error:%s\n", strerror(errno));
                    http->release(http);
                    event_del(epfd, fd);
                    close(fd);
            }
    } else if (rdlen == 0) {
            //客户端关闭
            fprintf(stderr, "客户端异常断开:fd = %d\n", fd);
            http->release(http);
            event_del(epfd, fd);
            close(fd);
    }

    http->lastrdlen = http->rdlen;
    http->rdlen += rdlen;

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

int event_modify(int epfd, int fd, uint32_t events, event_callback callback, Http *http) {
    
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
	
/**
        if (cfd > 0) {
            pid = fork(); //fork 子进程处理请求

            if (pid == 0) {

                close(sfd);
                inet_ntop(AF_INET, &cliaddr.sin_addr.s_addr, ip, sizeof(ip));			
                printf("建立一个新连接********************** client ip: %s, client port: %d *********************\n\n", ip, ntohs(cliaddr.sin_port));

                signal(SIGVTALRM, alarmHandler);  //安装一个闹钟信号处理器
                alarm(20); //保持连接keepalive = 20秒
                
                while(1) {
                    //------请求开始--------
                    curheadlen = nextheadlen;
                    //读取请求头
                    while(1) {
                        rdlen = read(cfd, buf, sizeof(buf));
                        if (rdlen > 0) {
                            if (curheadlen == nextheadlen) {
                                signal(SIGVTALRM, SIG_IGN); //一个新请求开始时忽略闹钟信号
                            }
                            if (request_head_size < (curheadlen + rdlen)) {
                                request_head_size *= 2;
                                request_header = realloc(request_header, request_head_size);  //请求头总长度超过request_header长度 扩展request_header长度为当前的两倍
                            }
                            memcpy(request_header + curheadlen, buf, rdlen); 
                            curheadlen += rdlen;
                            //判断请求头是否已经读取完毕
                            for (trueheadlen = (curheadlen - rdlen - 4); trueheadlen <= curheadlen - 4; trueheadlen++) {  //判断此次读到的数据以及上次最后四字节是否包含\r\n\r\n
                                if (trueheadlen < 0) {
                                    continue;
                                }
                                if (strncmp(request_header + trueheadlen, "\r\n\r\n", 4) == 0) {
                                    printf("trueheadlen = %d\n", trueheadlen); // trueheadlen为此次请求真正的请求头长度
                                    printf("curheadlen = %d\n", curheadlen); // curheadlen 已经读取到的长度
                                    goto head_end;
                                }
                            }

                        } else if (rdlen == 0) {
                            goto end; //客户端关闭连接
                        } else {
                            //其他错误...
                        }
                    }
head_end:
                    //解析请求头 ...
                    harr = renderHeader(request_header, trueheadlen);
                    var_dump(harr);
                    printf("---------------------request header end-----------------\n\n");
                    header_free(harr);
                    //如果是post请求解析Content-Length继续读取请求体 如果是get请求则解析下一个请求...
                    if (is_get(harr)) {
                        nextheadlen = curheadlen - trueheadlen - 4;
                        if (nextheadlen > 0) { 
                            memmove(request_header, request_header + trueheadlen + 4, nextheadlen); //保存当前读到的下一次请求的请求头
                        }
                    } else {
                        // 读取请求体
                    }

                    //响应
                    response_header = "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nServer: somehttpserver\r\nContent-Type: text/html\r\nTransfer-Encoding:chunked\r\n\r\n";
                    write(cfd, response_header, strlen(response_header));
                    response = "10\r\nhello world!<br>\r\n35\r\n<script src=\"http://127.0.0.1:8899/demo.js\"></script>\r\n0\r\n\r\n";
                    write(cfd, response, strlen(response));

                    //本次请求结束
                    signal(SIGVTALRM, alarmHandler);  //安装一个闹钟信号处理器
                    alarm(20); //保持连接keepalive = 20秒
                }
            }

end:
            close(cfd); // 父进程或fork子进程失败关闭连接
        }
*/
    
    }
}
