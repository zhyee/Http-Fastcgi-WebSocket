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
    int32_t request_head_size = BUF_SIZE, curheadlen = 0;
    char *request_header = malloc(request_head_size);  //初始开辟1024字节保存请求头
    char *http_body, *response_header, *response;
	char ip[16];
	int i;
    pid_t pid;

	while(1)
	{	
		int cfd = accept(sfd, (struct sockaddr *)&cliaddr, &addrlen);

		if (cfd > 0)
		{
            pid = fork(); //fork 子进程处理请求
            
            if (pid == 0) {
                close(sfd);
			    inet_ntop(AF_INET, &cliaddr.sin_addr.s_addr, ip, sizeof(ip));			
    			printf("client ip: %s, client port: %d\n", ip, ntohs(cliaddr.sin_port));

	    		while(1) {
                    //------请求开始--------
                    curheadlen = 0;
                    //读取请求头
                    while(1) {
                            rdlen = read(cfd, buf, sizeof(buf));
                            if (curheadlen == 0) {
                                signal(SIGVTALRM, SIG_IGN); //一个新请求开始忽略闹钟信号
                            }
                            if (rdlen > 0) {
                                    if (request_head_size < (curheadlen + rdlen)) {
                                            request_head_size *= 2;
                                            request_header = realloc(request_header, request_head_size);  //请求头总长度超过request_header长度 扩展request_header长度为当前的两倍
                                    }
                                    memcpy(request_header + curheadlen, buf, rdlen); 
                                    curheadlen += rdlen;
                                    write(STDOUT_FILENO, request_header, curheadlen);
                                    //判断请求头是否已经读取完毕
                                    for (i = (curheadlen - rdlen - 4); i <= curheadlen - 4; i++) {  //判断此次读到的数据以及上次最后四字节是否包含\r\n\r\n
                                            if (i < 0) {
                                                    continue;
                                            }
                                            if (strncmp(request_header+i, "\r\n\r\n", 4) == 0) {
                                                    printf("i = %d\n", i);
                                                    goto head_end;   //i为真正的请求头长度
                                            }
                                    }

                            } else if (rdlen == 0) {
                                goto end; //客户端关闭连接
                            } else {
                                //其他错误...
                            }
                    }
                    head_end:write(STDOUT_FILENO, request_header, i);
                    //解析请求头 ...
                    //如果是post请求解析Content-Length继续读取请求体 ...

                    //返回响应
                    response_header = "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nServer: somehttpserver\r\nContent-Type: text/html\r\nTransfer-Encoding:chunked\r\n\r\n";
			    	write(cfd, response_header, strlen(response_header));
                    response = "10\r\nhello world!<br>\r\n0\r\n\r\n";
                    write(cfd, response, strlen(response));
                    //请求结束
                    signal(SIGVTALRM, alarmHandler);
                    alarm(10); //设置一个闹钟
			    }
		    }
            end:close(cfd); // 父进程或fork子进程失败关闭请求
        }
	}
}
