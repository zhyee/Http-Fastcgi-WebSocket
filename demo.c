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

#include "http_parser.h"

#define BUF_SIZE 1024

int main(){

        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (int *)&reuse, sizeof(int));

        struct sockaddr_in servaddr, cliaddr;
        int addrlen = sizeof(cliaddr);
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8899);
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

        bind(sfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
        listen(sfd, 511);

        char buf[BUF_SIZE];
        size_t nparsed;
        ssize_t recved;

        while(1) {
           int cfd = accept(sfd, (struct sockaddr *)&cliaddr, &addrlen); 

           recved = recv(cfd, buf, BUF_SIZE, 0);

            

        }

}

