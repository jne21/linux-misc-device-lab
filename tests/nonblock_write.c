#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/jne_demo"

int main(void)
{
    static const char message[] = "Non-blocking writer message\n";
    ssize_t bytes_written;
    int fd;

    fd = open(DEVICE_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    bytes_written = write(fd, message, sizeof(message) - 1);

    if (bytes_written < 0) {
        if (errno == EAGAIN)
            printf("Queue is full\n");
        else
            fprintf(stderr, "write: %s\n", strerror(errno));

        close(fd);
        return errno == EAGAIN ? 0 : 1;
    }

    printf("Written %zd bytes\n", bytes_written);

    close(fd);
    return 0;
}
