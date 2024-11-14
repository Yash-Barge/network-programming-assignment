#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

int main(void) {
    int p_fd, c_fd;
    {
        int temp[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, temp);
        p_fd = temp[0];
        c_fd = temp[1];
    }

    bool is_parent = !fork();

    if (is_parent) {
        close(c_fd);

        int file_fd = open("test.c", O_RDONLY);

        char dummy_data = '\0';
        char control_buffer[CMSG_SPACE(sizeof(int))] = { 0 };
        struct iovec iov = { .iov_base = &dummy_data, .iov_len = sizeof dummy_data } ;
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control_buffer, .msg_controllen = CMSG_SPACE(sizeof(int))
        };

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        *cmsg = (struct cmsghdr) { .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS, .cmsg_len = CMSG_LEN(sizeof(int)) };
        memcpy(CMSG_DATA(cmsg), &file_fd, sizeof file_fd); // no direct assignment due to alignment issues

        msg.msg_controllen = CMSG_SPACE(sizeof(int)); // why do this again?

        sendmsg(p_fd, &msg, 0);
        
        close(file_fd);
        close(p_fd);
    } else {
        close(p_fd);

        struct msghdr msg = {0};

        char m_buffer[1];
        struct iovec io = { .iov_base = m_buffer, .iov_len = sizeof(m_buffer) };
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        char c_buffer[256];
        msg.msg_control = c_buffer;
        msg.msg_controllen = sizeof(c_buffer);

        if (recvmsg(c_fd, &msg, 0) < 0)
            printf("err\n"); // error

        struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msg);

        unsigned char * data = CMSG_DATA(cmsg);

        int file_fd = *((int*) data);

        close(c_fd);

        char char_buf[512] = { 0 };
        int bytes_read;
        while ((bytes_read = read(file_fd, char_buf, 511)) > 0) {
            char_buf[bytes_read] = '\0';
            printf("%s", char_buf);
        }
        close(file_fd);
    }

    return 0;
}
