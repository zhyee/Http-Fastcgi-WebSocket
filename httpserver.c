#include <stdio.h>
#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>

#define BUF_SIZE 1024

void alarmHandler(int signum) {
    if (signum == SIGVTALRM) {
        exit(0);
    }
}

void childHandler(int signum){
        if (signum == SIGCHLD)
            while(waitpid(-1, NULL, WNOHANG) > 0); // 回收子进程
}

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
            header_entry *entry = malloc(elen);
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
        write(STDOUT_FILENO, "\n", 1);
    }
}


int main()
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
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
    char ip[16];
    int i;
    pid_t pid;

    signal(SIGCHLD, childHandler);

    while(1) {	
        int cfd = accept(sfd, (struct sockaddr *)&cliaddr, &addrlen);

        if (cfd > 0) {
            pid = fork(); //fork 子进程处理请求

            if (pid == 0) {

                close(sfd);
                inet_ntop(AF_INET, &cliaddr.sin_addr.s_addr, ip, sizeof(ip));			
                printf("--------------client ip: %s, client port: %d----------------\n", ip, ntohs(cliaddr.sin_port));

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
                                    printf("curheadlen = %d\n", curheadlen); // trueheadlen为此次请求真正的请求头长度
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
    }
}
