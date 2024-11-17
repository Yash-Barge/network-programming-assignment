#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/errno.h>
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
#define CLIENTS_PER_PROCESS 3

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

int assemble_fdset(fd_set *fdset, int *fdarr, int len) {
    FD_ZERO(fdset);
    int max_fd = fdarr[0];

    for (int i = 0; i < len; i++) {
        FD_SET(fdarr[i], fdset);
        max_fd = max_fd > fdarr[i] ? max_fd : fdarr[i];
    }

    return max_fd + 1;
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
        return -1; // error

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    unsigned char *data = CMSG_DATA(cmsg);
    int recvd_fd = *((int *) data);

    return recvd_fd;
}

void execute_child(int c_sock_fd) {
    bool continue_execution = true, is_active = false;
    int connections[CLIENTS_PER_PROCESS] = { 0 };
    int connection_count = 0;

    while (continue_execution) {
        fd_set fdset;
        int nfds = assemble_fdset(&fdset, connections, connection_count);
        if (connection_count < CLIENTS_PER_PROCESS) {
            FD_SET(c_sock_fd, &fdset);
            nfds = nfds > c_sock_fd + 1 ? nfds : c_sock_fd + 1;
        }

        int event_count = select(nfds, &fdset, NULL, NULL, NULL);
        
        // new connection event, recv immediately to mitigate thundering herd
        int new_conn_fd = -1;
        if (event_count > 0 && FD_ISSET(c_sock_fd, &fdset)) {
            new_conn_fd = recv_fd(c_sock_fd);
            event_count--;
        }

        // client connection event
        for (int i = 0; event_count > 0 && i < connection_count; i++) {
            if (FD_ISSET(connections[i], &fdset)) {
                char buffer[1024] = { 0 };
                int bytes_recvd = recv(connections[i], buffer, 1024, 0);
                if (bytes_recvd)
                    send(connections[i], buffer, bytes_recvd, 0);
                else {
                    shutdown(connections[i], SHUT_WR);
                    close(connections[i]);
                    connections[i--] = connections[--connection_count]; // remove ith file descriptor, rerun iteration for new descriptor at index i
                }
                event_count--;
            }
        }

        // handle new connection
        if (new_conn_fd != -1) {
            connections[connection_count++] = new_conn_fd;

            if (connection_count == 1) { // process went from inactive to active
                is_active = true;
                // is condition variable needed here?
                pthread_mutex_lock(&shm->access_lock);
                shm->p_inactive--;
                if (shm->p_inactive < IDLE_CHILDREN && !shm->parent_informed) {
                    send(c_sock_fd, NULL, 0, 0);
                    shm->parent_informed = true;
                }
                pthread_mutex_unlock(&shm->access_lock);
            }
        }

        if (is_active && connection_count == 0) {
            is_active = false;
            pthread_mutex_lock(&shm->access_lock);
            if (shm->p_inactive < IDLE_CHILDREN)
                shm->p_inactive++;
            else
                continue_execution = false;
            pthread_mutex_unlock(&shm->access_lock);
        }
    }

    printf("too many idle children, removing 1 process...\n");

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

    int p_sock_fd, c_sock_fd;
    {
        int temp_fd[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp_fd);
        p_sock_fd = temp_fd[0];
        c_sock_fd = temp_fd[1];
    }
    fcntl(c_sock_fd, F_SETFL, fcntl(c_sock_fd, F_GETFL) | O_NONBLOCK);

    for (long i = 0; i < IDLE_CHILDREN; i++) {
        pid_t temp = fork();
        if (!temp) {
            close(p_sock_fd);
            execute_child(c_sock_fd);
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

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof client_addr;

        fd_set fdset;
        const int nfds = assemble_fdset(&fdset, (int []) { socket_fd, p_sock_fd }, 2);

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
                    execute_child(c_sock_fd);
                    exit(EXIT_SUCCESS);
                }
            }
            shm->p_inactive += new_process_count;
            shm->parent_informed = false;
            pthread_mutex_unlock(&shm->access_lock);

            printf("created %d new process(es)...\n", new_process_count);
        }

        if (FD_ISSET(socket_fd, &fdset)) {
            const int conn_fd = accept(socket_fd, (struct sockaddr *) &client_addr, &client_addr_len);
            send_fd(p_sock_fd, conn_fd);
        }
    }

    munmap(shm, sizeof *shm);

    close(c_sock_fd); // left open as need to pass to new children

    close(socket_fd);
    close(p_sock_fd);

    return 0;
}
