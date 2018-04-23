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

#define BUF_SIZE 1024
#define IP_LEN 16

#define MAXEVENTS 32


int epfd;

typedef struct header_entry {
    uint32_t len;
    char kv[];
} header_entry;

typedef struct header_array {
    uint32_t size;
    uint32_t len;
    header_entry *entry[];
} header_array;

header_array *extend_header_array(header_array *arr) {
    arr = realloc(arr, sizeof(header_array) + sizeof(header_entry *) * arr->size * 2);
    arr->size *= 2;
    return arr;
}

void header_free(header_array *harr) {
    int i = 0;
    for(; i < harr->len; i++) {
        free(harr->entry[i]);
    }
    free(harr);
}

// 解析请求头 并放入数组
header_array *renderHeader(const char *request_header, int32_t headlen) {
    header_array *arr = malloc(sizeof(header_array) + sizeof(header_entry *) * 16);
    arr->size = 16;
    arr->len = 0;
    int pos, prepos = 0, elen;
    for (pos = 0; pos < headlen; pos++) {
        if (pos <= prepos) {
            continue;
        }
        if (strncmp(request_header + pos, "\r\n", 2) == 0 || pos == (headlen - 1)) {
            elen = pos - prepos;
            header_entry *entry = malloc(sizeof(header_entry) + elen);
            entry->len = elen;
            memcpy(entry->kv, request_header + prepos, elen);
            if (arr->size == arr->len) {
                arr = extend_header_array(arr);
            }
            arr->entry[arr->len++] = entry;
            prepos = pos + 2;
        }
    }
    return arr;
}

//判断是否是get请求
int is_get(header_array *harr) {
    return strncmp(harr->entry[0]->kv, "GET", 3) == 0 ? 1 : 0;
}

void var_dump(header_array *arr) {
    int i;
    for(i = 0; i < arr->len; i++) {
        write(STDOUT_FILENO, arr->entry[i]->kv, arr->entry[i]->len);
        write(STDOUT_FILENO, "\n\0", 2);
    }
}

// 从字符串中拷贝长度n的子串
char *strndup(const char *src, size_t n) {
    char *dst = malloc(n+1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
}

// 释放 epoll_event_data
void release_epdata(epoll_event_data *epdata) {
    if (epdata->ev) {
        free(epdata->ev);
    }

    if (epdata->databuf) {
        free(epdata->databuf);
    }

    if (epdata->request) {
        epdata->request->release(epdata->request);
    }

    if (epdata->response) {
        epdata->response->release(epdata->response);
    }

    free(epdata);
}


void accept_callback(epoll_event_data *epdata) {
    int cfd, sfd = epdata->fd;
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

    // 创建一个新的epdata
    epoll_event_data *newepdata = malloc(sizeof(epoll_event_data));
    bzero(newepdata, sizeof(*newepdata));
    newepdata->step = CONNECTED;
    newepdata->release = release_epdata;

    event_add(epfd, cfd, EPOLLIN, rcve_callback, newepdata);

}

// 解析请求行
void parse_request_line(epoll_event_data *epdata) {
    if (epdata->request == NULL) {
        epdata->request = malloc(sizeof(Request));
        bzero(epdata->request, sizeof(*epdata->request));
    }
    int i, j = 0, lastpos = 0, endpos = 0;
    for(i = 1; i < epdata->rdlen; i++) {
            if (strncmp(epdata->databuf+i, "\r\n", 2) == 0) {
                endpos = i;
                epdata->parselen = i + 2;
                goto parse_success;
            } else if (strncmp(epdata->databuf+i, "\n", 1) == 0) {
                endpos = i;
                epdata->parselen = i + 1;
                goto parse_success;
            }

   }

    fprintf(stderr, "parse request line error\n");
    epdata->release(epdata);
    event_del(epfd, epdata->fd);
    return;

parse_success:
    for(i = 0; i < endpos; i++) {
         if (epdata->databuf[i] == ' ') {
            if (j == 0) {
                memcpy(epdata->request->method, epdata->databuf + lastpos, i - lastpos);
                lastpos = i + 1;
                j++;
            } else if (j == 1) {
                epdata->request->request_uri = strndup(epdata->databuf+lastpos, i - lastpos);
                lastpos = i + 1;
                memcpy(epdata->request->protocol, epdata->databuf+lastpos, endpos-lastpos);
                break;
            }
         }
    }
    epdata->step = READ_REQUEST_HEADER;
    printf("method = %s, protocol = %s, request_uri = %s\n", epdata->request->method, epdata->request->protocol, epdata->request->request_uri);

}

void parse_header(epoll_event_data *epdata) {
    
}

void expand_databuf(epoll_event_data *epdata) {
    epdata->databuf = realloc(epdata->databuf, epdata->bufsize * 2);
    epdata->bufsize *= 2;  // assume not over flow int
}

void rcve_callback(epoll_event_data *epdata) {
    if (epdata->step < CONNECTED) {
        fprintf(stderr, "not connected\n");
        return;
    } else if (epdata->step == CONNECTED) {
            //建立连接后的第一个请求
        epdata->step = REQUEST_BEGIN;
    }

    if (epdata->step == REQUEST_BEGIN) {
           // 初始化缓冲区  
            epdata->databuf = malloc(BUF_SIZE);
            epdata->bufsize = BUF_SIZE;
            epdata->rdlen = 0;
            epdata->parselen = 0;
    }

    if (epdata->rdlen == epdata->bufsize) {
        expand_databuf(epdata);
    }

    int fd = epdata->fd, i;
    size_t rdlen;
    
    rdlen = recv(fd, epdata->databuf+epdata->rdlen, epdata->bufsize - epdata->rdlen, MSG_DONTWAIT);

    if (rdlen == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "recv error:%s\n", strerror(errno));
                    epdata->release(epdata);
                    event_del(epfd, fd);
                    close(fd);
            }
    } else if (rdlen == 0) {
            //客户端关闭
            fprintf(stderr, "客户端异常断开:fd = %d\n", fd);
            epdata->release(epdata);
            event_del(epfd, fd);
            close(fd);
    }

    epdata->rdlen += rdlen;

    switch (epdata->step) {
        // 请求的第一次读数据
        case REQUEST_BEGIN:   
            
                   for(i = epdata->parselen; i < rdlen - 1; i++) {
                        if (strncmp(epdata->databuf + i, "\r\n", 2) == 0 || strncmp(epdata->databuf + i, "\n", 1) == 0) {
                            // 读到请求行结束标计， 解析请求行
                            parse_request_line(epdata);
                            goto parse_header;
                        }
                   }

                   if (strncmp(epdata->databuf + i, "\n", 1) == 0) {
                        parse_request_line(epdata);
                        goto parse_header;
                   }

                   // 没读到完整的请求行，继续读取
                   epdata->step = READ_REQUEST_LINE;
                   return;


parse_header:
                   for(i = epdata->parselen; i < rdlen - 3; i++) {
                        if (strncmp(epdata->databuf + i, "\r\n\r\n", 2) == 0 || strncmp(epdata->databuf + i, "\n\n", 2) == 0) {
                                //解析请求头
                        }
                   }
            
            break;

        case READ_REQUEST_LINE:

            break;
            
                
    
    }

}

int event_add(int epfd, int fd, uint32_t events, event_callback callback, epoll_event_data *epdata) {    
    struct epoll_event *epev = malloc(sizeof(struct epoll_event));

    epdata->fd = fd;
    epdata->callback = callback;
    epdata->ev = epev;
    
    epev->events = events;
    epev->data.ptr = epdata;

    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, epev);
}

int event_modify(int epfd, int fd, uint32_t events, event_callback callback, epoll_event_data *epdata) {
    
}

int event_del(int epfd, int fd) {
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}



int main()
{
    int sfd, reuse = 1;
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (int *)&reuse, sizeof(int));

    struct sockaddr_in servaddr, cliaddr;
    int addrlen = sizeof(cliaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8899);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(sfd, (struct sockaddr *)&servaddr, sizeof(servaddr));	
    listen(sfd, 511);

    char buf[BUF_SIZE] = {'\0'};
    ssize_t rdlen;
    header_array *harr;
    int32_t request_head_size = BUF_SIZE, curheadlen = 0, nextheadlen = 0, trueheadlen;
    char *request_header = malloc(request_head_size);  //初始开辟1024字节保存请求头
    char *http_body, *response_header, *response;
    int i;
    pid_t pid;

    epfd = epoll_create(1024);
    
    epoll_event_data *epdata = malloc(sizeof(epoll_event_data));
    epdata->step = WAIT_CONNECT;
    epdata->databuf = NULL;
    event_add(epfd, sfd, EPOLLIN, accept_callback, epdata);
    
    struct epoll_event *events = malloc(MAXEVENTS * sizeof(struct epoll_event));
    struct epoll_event readyev;
    epoll_event_data *readyepdata;

    while(1) {

            int evcount = epoll_wait(epfd, events, MAXEVENTS, -1); // wait infinit

            if (-1 == evcount) {
                fprintf(stderr, "epoll_wait error:%s\n", strerror(errno));
                continue;
            }

            for (i = 0; i < evcount; i++) {
                readyev = events[i];
                if (readyev.data.ptr != NULL) {
                    readyepdata = (epoll_event_data *)readyev.data.ptr;
                    readyepdata->callback(readyepdata);
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
