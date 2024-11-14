#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/socket.h>
// #include <stdbool.h>

#define IDLE_CHILDREN 5

#define MAX_POOL_SIZE 1024

// struct msgbuf {
//     long mtype;
//     char mtext[1];
// };

void send_fd(int socket_fd, int payload_fd) {
    char dummy_data = '\0';
    char control_buffer[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = &dummy_data, .iov_len = sizeof dummy_data } ;
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control_buffer, .msg_controllen = CMSG_SPACE(sizeof(int))
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    *cmsg = (struct cmsghdr) { .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS, .cmsg_len = CMSG_LEN(sizeof(int)) };
    memcpy(CMSG_DATA(cmsg), &payload_fd, sizeof payload_fd); // no direct assignment due to alignment issues

    msg.msg_controllen = CMSG_SPACE(sizeof(int)); // why do this again?

    sendmsg(socket_fd, &msg, 0);
    
    close(payload_fd);

    return;
}

int recv_fd(int socket_fd) {
    char m_buffer[1];
    struct iovec io = { .iov_base = m_buffer, .iov_len = sizeof(m_buffer) };
    char c_buffer[256];

    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = c_buffer, .msg_controllen = sizeof(c_buffer) };

    if (recvmsg(socket_fd, &msg, 0) < 0)
        ; // error

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    unsigned char *data = CMSG_DATA(cmsg);
    int recvd_fd = *((int *) data);

    return recvd_fd;
}

void execute_child(int mq_id[2], int c_sock_fd) {
    long i;
    msgrcv(mq_id[0], &i, 0, 0, 0);
    printf("child: %ld\n", i);
    msgsnd(mq_id[1], &i, 0, 0);

    int recvd_int;
    recv(c_sock_fd, &recvd_int, sizeof recvd_int, 0);
    printf("(%d): %d\n", getpid(), recvd_int);

    close(c_sock_fd);

    return;
}

int main(void) {
    int mq_id[2] = { msgget(IPC_PRIVATE, 0660), msgget(IPC_PRIVATE, 0660) };

    int p_sock_fd, c_sock_fd;

    {
        int temp_fd[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp_fd);
        p_sock_fd = temp_fd[0];
        c_sock_fd = temp_fd[1];
    }

    pid_t *children_array = calloc(MAX_POOL_SIZE, sizeof *children_array);
    int children_count = 0;

    for (long i = 0; i < IDLE_CHILDREN; i++) {
        pid_t temp = fork();
        if (!temp) {
            close(p_sock_fd);
            execute_child(mq_id, c_sock_fd);
            exit(EXIT_SUCCESS);
        } else
            children_array[children_count++] = temp;
    }

    close(c_sock_fd);

    for (long i = 1; i <= IDLE_CHILDREN; i++)
        msgsnd(mq_id[0], &i, 0, 0);

    for (int i = 0; i < IDLE_CHILDREN; i++) {
        long j;
        if (msgrcv(mq_id[1], &j, 0, 0, 0) == -1)
            perror("msgrcv");
        printf("parent: rcv %ld\n", j);
    }

    for (int i = 0; i < IDLE_CHILDREN; i++)
        send(p_sock_fd, &i, sizeof i, 0);

    for (int i = 0; i < IDLE_CHILDREN; i++)
        wait(NULL);

    int rc = msgctl(mq_id[0], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[0]");

    rc = msgctl(mq_id[1], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[1]");

    close(p_sock_fd);

    return 0;
}
