#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/jne_demo"
#define BUFFER_SIZE 256

int main(void)
{
    struct pollfd descriptor;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    int result;
    int fd;

    fd = open(DEVICE_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    descriptor.fd = fd;
    descriptor.events = POLLIN;

    printf("Waiting for data...\n");

    result = poll(&descriptor, 1, -1);
    if (result < 0) {
        perror("poll");
        close(fd);
        return 1;
    }

    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fprintf(stderr, "poll returned error flags: 0x%x\n", descriptor.revents);
        close(fd);
        return 1;
    }

    if (!(descriptor.revents & POLLIN)) {
        fprintf(stderr, "poll returned without POLLIN\n");
        close(fd);
        return 1;
    }

    bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        fprintf(stderr, "read: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    buffer[bytes_read] = '\0';
    printf("Read %zd bytes: %s", bytes_read, buffer);

    close(fd);
    return 0;
}
