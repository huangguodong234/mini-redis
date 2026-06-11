#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>      //connect () 是什么？ 作用：客户端主动连接服务器。
#include <netinet/in.h>
#include <arpa/inet.h>      //inet_pton()-把人类可以看懂的IP地址，转化为计算机可以看懂的二进制地址


#define SERVER_PORT 6379
#define SERVER_IP "127.0.0.1"

void send_cmd(int fd, const char *cmd) {
    write(fd, cmd, strlen(cmd));
}

void read_resp(int fd) {
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("响应: %s\n", buf);
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    // 测试 SET
    send_cmd(fd, "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhangsan\r\n");
    read_resp(fd);

    // 测试 GET
    send_cmd(fd, "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");
    read_resp(fd);

    // 测试 DEL
    send_cmd(fd, "*2\r\n$3\r\nDEL\r\n$4\r\nname\r\n");
    read_resp(fd);

    // 测试 GET 不存在
    send_cmd(fd, "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");
    read_resp(fd);

    //测试DEL命令的key不存在
    send_cmd(fd,"*2\r\n$3\r\nDEL\r\n$7\r\nunknown\r\n");
    read_resp(fd);

    close(fd);
    return 0;
}
