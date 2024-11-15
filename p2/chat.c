#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/_types/_fd_def.h>
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

#define IDLE_CHILDREN 5

#define MAX_POOL_SIZE 1024
#define TCP_BACKLOG_LEN 5
#define TCP_LISTENING_PORT 12345

// struct msgbuf {
//     long mtype;
//     char mtext[1];
// };

// user data and message queues will have to go in here
struct shared_mem {
    pthread_mutex_t access_lock;
    int p_inactive;
    bool parent_informed;
};

struct shared_mem *shm;

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
    bool continue_execution = true;

    while (continue_execution) {
        const int conn_fd = recv_fd(c_sock_fd);

        // is condition variable needed here?
        pthread_mutex_lock(&shm->access_lock);
        shm->p_inactive--;
        if (shm->p_inactive < IDLE_CHILDREN && !shm->parent_informed) {
            send(c_sock_fd, NULL, 0, 0);
            shm->parent_informed = true;
        }
        pthread_mutex_unlock(&shm->access_lock);

        char buffer[1024] = { 0 };
        int bytes_recvd;
        while ((bytes_recvd = recv(conn_fd, buffer, 1024, 0)) > 0) {
            send(conn_fd, buffer, bytes_recvd, 0);
            memset(buffer, '\0', 1024);
        }

        pthread_mutex_lock(&shm->access_lock);
        if (shm->p_inactive < IDLE_CHILDREN)
            shm->p_inactive++;
        else
            continue_execution = false;
        pthread_mutex_unlock(&shm->access_lock);

        shutdown(conn_fd, SHUT_WR);
        close(conn_fd);
    }

    close(c_sock_fd);
    return;
}

int main(void) {
    // children do not become zombies when they exit, no need to wait on them
    sigset_t block_sigchld;
    sigemptyset(&block_sigchld);
    sigaddset(&block_sigchld, SIGCHLD);
    struct sigaction sa = { .sa_handler = SIG_DFL, .sa_mask = block_sigchld, .sa_flags = SA_NOCLDWAIT };
    sigaction(SIGCHLD, &sa, NULL);

    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    {
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shm->access_lock, &mattr);
    }

    int mq_id[2] = { msgget(IPC_PRIVATE, 0660), msgget(IPC_PRIVATE, 0660) };

    int p_sock_fd, c_sock_fd;

    {
        int temp_fd[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp_fd);
        p_sock_fd = temp_fd[0];
        c_sock_fd = temp_fd[1];
    }

    for (long i = 0; i < IDLE_CHILDREN; i++) {
        pid_t temp = fork();
        if (!temp) {
            close(p_sock_fd);
            execute_child(mq_id, c_sock_fd);
            exit(EXIT_SUCCESS);
        }
    }

    shm->p_inactive = IDLE_CHILDREN;

    // c_sock_fd cannot be closed yet, as it will be needed when creating new children

    int socket_fd;
    {
        const struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(TCP_LISTENING_PORT) };
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        int retval = bind(socket_fd, (const struct sockaddr *) &addr, sizeof addr);

        if (retval == -1)
            error_and_exit("bind failure");
    }
    listen(socket_fd, TCP_BACKLOG_LEN);

    const int nfds = (socket_fd > p_sock_fd ? socket_fd : p_sock_fd) + 1;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof client_addr;

        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(socket_fd, &fdset);
        FD_SET(p_sock_fd, &fdset);

        select(nfds, &fdset, NULL, NULL, NULL);

        if (FD_ISSET(p_sock_fd, &fdset)) {
            char temp;
            recv(p_sock_fd, &temp, sizeof temp, 0);

            pthread_mutex_lock(&shm->access_lock);
            const int new_process_count = IDLE_CHILDREN - shm->p_inactive;
            for (int i = 0; i < new_process_count; i++) {
                pid_t temp = fork();
                if (!temp) {
                    close(p_sock_fd);
                    execute_child(mq_id, c_sock_fd);
                    exit(EXIT_SUCCESS);
                }
            }
            shm->p_inactive += new_process_count;
            shm->parent_informed = false;
            pthread_mutex_unlock(&shm->access_lock);
        }

        if (FD_ISSET(socket_fd, &fdset)) {
            const int conn_fd = accept(socket_fd, (struct sockaddr *) &client_addr, &client_addr_len);
            send_fd(p_sock_fd, conn_fd);
        }
    }

    int rc = msgctl(mq_id[0], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[0]");

    rc = msgctl(mq_id[1], IPC_RMID, NULL);
    if (rc == -1)
        perror("msgctl[1]");

    munmap(shm, sizeof *shm);

    close(c_sock_fd); // left open as need to pass to new children

    close(socket_fd);
    close(p_sock_fd);

    return 0;
}
