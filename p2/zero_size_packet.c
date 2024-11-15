#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>

void execute_child(int c_sock_fd) {
    // for (int i = 0; i < 5; i++)
        send(c_sock_fd, NULL, 0, 0);

    return;
}

int main(void) {
    int p_sock_fd, c_sock_fd;

    {
        int temp_fd[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp_fd);
        p_sock_fd = temp_fd[0];
        c_sock_fd = temp_fd[1];
    }

    if (!fork()) {
        close(p_sock_fd);
        execute_child(c_sock_fd);
        exit(EXIT_SUCCESS);
    }

    sleep(5);
    char buffer[512] = { 0 };
    for (int i = 0; i < 6; i++) {
        int bytes_recvd = recv(p_sock_fd, buffer, 512, 0);
        printf("%d\n", bytes_recvd);
    }

    close(c_sock_fd);

    return 0;
}
