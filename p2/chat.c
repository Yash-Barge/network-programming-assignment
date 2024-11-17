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
#define USER_NAME_LEN_LIMIT 32
#define USER_COUNT_LIMIT 300

struct user {
    bool is_online;
    int msg_queue_fd; // TODO: fix
    char name[USER_NAME_LEN_LIMIT];
};

// user data and message queues will have to go in here
struct shared_mem {
    pthread_mutex_t access_lock;
    int p_inactive;
    bool parent_informed;
    int user_count;
    struct user user_list[USER_COUNT_LIMIT];
};

struct shared_mem *shm;
bool is_parent = true;

void interrupt_handler(int signo) {
    if (!is_parent)
        exit(EXIT_SUCCESS);

    pthread_mutex_lock(&shm->access_lock);
    for (int i = 0; i < shm->user_count; i++)
        msgctl(shm->user_list[i].msg_queue_fd, IPC_RMID, NULL);
    pthread_mutex_unlock(&shm->access_lock);

    munmap(shm, sizeof *shm);

    exit(EXIT_SUCCESS);
}

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
        return -1; // error

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    unsigned char *data = CMSG_DATA(cmsg);
    int recvd_fd = *((int *) data);

    return recvd_fd;
}

struct connection {
    int conn_fd;
    int user_index;
};

int assemble_fdset(fd_set *fdset, struct connection *connections, int len) {
    FD_ZERO(fdset);
    int max_fd = connections[0].conn_fd;

    for (int i = 0; i < len; i++) {
        FD_SET(connections[i].conn_fd, fdset);
        max_fd = max_fd > connections[i].conn_fd ? max_fd : connections[i].conn_fd;

        // TODO: fix
        // if (connections[i].user_index != -1) {
        //     FD_SET(shm->user_list[connections[i].user_index].msg_queue_fd, fdset);
        //     max_fd = max_fd > shm->user_list[connections[i].user_index].msg_queue_fd ? max_fd : shm->user_list[connections[i].user_index].msg_queue_fd;
        // }
    }

    return max_fd + 1;
}

struct msgbuf {
    long mtype;
    char mtext[1024];
};

// returns false if connection has closed
bool recv_from_connection(struct connection *conn) {
    char buffer[1024] = { 0 };
    int bytes_recvd = recv(conn->conn_fd, buffer, 1024, 0);

    if (buffer[bytes_recvd - 1] == '\n')
        buffer[--bytes_recvd] = '\0';
    if (buffer[bytes_recvd - 1] == '\r')
        buffer[--bytes_recvd] = '\0';

    if (!bytes_recvd) {
        shutdown(conn->conn_fd, SHUT_WR);
        close(conn->conn_fd);

        if (conn->user_index != -1)
            shm->user_list[conn->user_index].is_online = false;

        return false;
    }

    char reply[1024] = { 0 };
    int size = 0;

    if (conn->user_index == -1) {
        pthread_mutex_lock(&shm->access_lock);
        for (int i = 0; i < shm->user_count; i++) {
            if (!strcmp(buffer, shm->user_list[i].name)) {
                if (shm->user_list[i].is_online)
                    size = snprintf(reply, 1024, "User %s is already online! Enter a different username: ", buffer);
                else {
                    shm->user_list[i].is_online = true;
                    conn->user_index = i;
                    size = snprintf(reply, 1024, "Welcome back %s!\n", buffer);
                }
                break;
            }
        }
        if (!size) { // user not in list, make a new one
            shm->user_list[shm->user_count] = (struct user) { .is_online = true, .msg_queue_fd = msgget(IPC_PRIVATE, 0660) };
            memcpy(shm->user_list[shm->user_count].name, buffer, bytes_recvd);
            conn->user_index = shm->user_count++;
            size = snprintf(reply, 1024, "New user %s created\n", buffer);
        }
        pthread_mutex_unlock(&shm->access_lock);
    } else {
        if (!strcmp(buffer, "help")) {
            const char msg_help[] = "help         Display this message.\n"
                                    "listu        List all users.\n";
            
            size = strlen(msg_help);
            memcpy(reply, msg_help, size);
        } else if (!strcmp(buffer, "listu")) {
            pthread_mutex_lock(&shm->access_lock);
            for (int i = 0; i < shm->user_count; i++)
                size += snprintf(reply + size, 1024 - size, "%3d: %10s (%s)\n", i+1, shm->user_list[i].name, shm->user_list[i].is_online ? "online" : "offline");
            pthread_mutex_unlock(&shm->access_lock);
        }
        // TODO: fix
        /* else if (!strncmp(buffer, "msgu ", 5)) {
            int r;
            for (r = 5; r < strlen(buffer); r++)
                if (buffer[r] == ' ')
                    break;
            if (r == 5 || r == strlen(buffer))
                goto unknown_cmd;

            char target_user[r - 5 + 1];
            memcpy(target_user, buffer + 5, r - 5);
            target_user[r - 5] = '\0';
            
            const int user_count = shm->user_count;
            int target_user_index;
            for (int target_user_index = 0; target_user_index < user_count; target_user_index++) {
                if (!strcmp(shm->user_list[target_user_index].name, target_user))
                    break;
            }
            if (target_user_index == user_count)
                goto unknown_cmd;

            struct msgbuf msg = { .mtype = 0, .mtext = "\r" };
            int mtext_len = 1;
            mtext_len += snprintf(msg.mtext + mtext_len, 1024 - mtext_len, "FROM %s: %s\n", shm->user_list[target_user_index].name, buffer + r + 1);

            msgsnd(shm->user_list[target_user_index].msg_queue_fd, &msg, mtext_len+1, 0);
        } */
        else {
        unknown_cmd:;
            const char msg_unknown[] = "Unknown command. Enter `help` to see a list of supported commands.\n";

            size = strlen(msg_unknown);
            memcpy(reply, msg_unknown, size);
        }
    }

    if (conn->user_index != -1) {
        strcat(reply, shm->user_list[conn->user_index].name);
        strcat(reply, "> ");
        size += strlen(shm->user_list[conn->user_index].name) + 2;
    }

    send(conn->conn_fd, reply, size, 0);

    return true;
}

// TODO: fix
void send_to_connection(struct connection *conn) {
    struct msgbuf msg = { 0 };
    msgrcv(shm->user_list[conn->user_index].msg_queue_fd, &msg, 1024, 0, 0);
    send(conn->conn_fd, msg.mtext, strlen(msg.mtext), 0);

    return;
}

void execute_child(int c_sock_fd) {
    bool continue_execution = true, is_active = false;
    struct connection connections[CLIENTS_PER_PROCESS] = {{ 0 } };
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
            if (FD_ISSET(connections[i].conn_fd, &fdset)) {
                const bool connection_alive = recv_from_connection(connections + i);

                if (!connection_alive)
                    connections[i--] = connections[--connection_count]; // remove ith file descriptor, rerun iteration for new descriptor at index i

                event_count--;
                continue;
            }

            // TODO: fix
            // if (connections[i].user_index != -1 && FD_ISSET(shm->user_list[connections[i].user_index].msg_queue_fd, &fdset))
            //     send_to_connection(connections + i);
        }

        // handle new connection
        if (new_conn_fd != -1) {
            connections[connection_count++] = (struct connection) { .conn_fd = new_conn_fd, .user_index = -1 };

            const char msg_enter_username[] = "Enter username (limit of %d characters): ";
            const int buffer_capacity = strlen(msg_enter_username) + 8; // just some wiggle room is limit is long
            char buffer[buffer_capacity];
            memset(buffer, '\0', buffer_capacity);
            const int len = snprintf(buffer, buffer_capacity, msg_enter_username, USER_NAME_LEN_LIMIT-3);

            send(new_conn_fd, buffer, len, 0);

            if (connection_count == 1) { // process went from inactive to active
                is_active = true;
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
    signal(SIGINT, interrupt_handler); // cleanup message queues

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
            is_parent = false;
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

    const int nfds = (socket_fd > p_sock_fd ? socket_fd : p_sock_fd) + 1;
    fd_set ref_fdset;
    FD_ZERO(&ref_fdset);
    FD_SET(socket_fd, &ref_fdset);
    FD_SET(p_sock_fd, &ref_fdset);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof client_addr;

        fd_set fdset = ref_fdset;

        select(nfds, &fdset, NULL, NULL, NULL);

        if (FD_ISSET(p_sock_fd, &fdset)) {

            char temp;
            recv(p_sock_fd, &temp, sizeof temp, 0);

            pthread_mutex_lock(&shm->access_lock);
            const int new_process_count = IDLE_CHILDREN - shm->p_inactive;
            for (int i = 0; i < new_process_count; i++) {
                pid_t temp = fork();
                if (!temp) {
                    is_parent = false;
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
