#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <netinet/in.h>
// #include <stdbool.h>

#define IDLE_CHILDREN 5

#define MAX_POOL_SIZE 1024
#define TCP_BACKLOG_LEN 5
#define TCP_LISTENING_PORT 12345

// struct msgbuf {
//     long mtype;
//     char mtext[1];
// };

void error_and_exit(char *err_msg) {
    perror(err_msg);
    exit(EXIT_FAILURE);
}

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
    const int conn_fd = recv_fd(c_sock_fd);
    char buffer[10] = { 0 };
    const int buflen = snprintf(buffer, 10, "%d\n", getpid());
    send(conn_fd, buffer, buflen, 0);

    shutdown(conn_fd, SHUT_WR);
    close(conn_fd);


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

    int socket_fd;
    {
        const struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(TCP_LISTENING_PORT) };
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        int retval = bind(socket_fd, (const struct sockaddr *) &addr, sizeof addr);

        if (retval == -1)
            error_and_exit("bind failure");
    }
    listen(socket_fd, TCP_BACKLOG_LEN);

    int temp = 5;
    while (temp--) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof client_addr;
        const int conn_fd = accept(socket_fd, (struct sockaddr *) &client_addr, &client_addr_len);

        send_fd(p_sock_fd, conn_fd);
    }

    for (int i = 0; i < IDLE_CHILDREN; i++)
        wait(NULL);

    int rc = msgctl(mq_id[0], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[0]");

    rc = msgctl(mq_id[1], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[1]");

    close(socket_fd);
    close(p_sock_fd);

    return 0;
}
