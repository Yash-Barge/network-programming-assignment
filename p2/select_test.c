#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/signal.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <stdbool.h>
#include <errno.h>

int execute_child(int c_sock_fd) {
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(c_sock_fd, &fdset);

    select(c_sock_fd+1, &fdset, NULL, NULL, NULL);
    printf("passed select\n");

    // char temp;
    // recv(c_sock_fd, &temp, sizeof temp, 0);
    sleep(2);
    int rec_int;
    if (recv(c_sock_fd, &rec_int, sizeof rec_int, 0) < 0)
        perror("recv");
    else
        printf("%d\n", rec_int);

    return 0;
}

int main(void) {
    int p_sock_fd, c_sock_fd;
    {
        int temp_fd[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp_fd);
        p_sock_fd = temp_fd[0];
        c_sock_fd = temp_fd[1];
    }

    fcntl(c_sock_fd, F_SETFL, fcntl(c_sock_fd, F_GETFL) | O_NONBLOCK);

    for (int i = 0; i < 2; i++) {
        if (!fork()) {
            close(p_sock_fd);
            return execute_child(c_sock_fd);
        }
    }

    close(c_sock_fd);

    char temp = 4;
    send(p_sock_fd, &temp, sizeof temp, 0);

    // temp = 8;
    // send(p_sock_fd, &temp, sizeof temp, 0);
    sleep(2);

    return 0;
}
