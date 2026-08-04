#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char buffer[256];
    ssize_t bytes_read;
    int fd;

    fd = open("/dev/jne_demo", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    bytes_read = read(fd, buffer, sizeof(buffer) - 1);

    if (bytes_read < 0) {
        if (errno == EAGAIN)
            printf("No data available\n");
        else
            fprintf(stderr, "read: %s\n", strerror(errno));

        close(fd);
        return errno == EAGAIN ? 0 : 1;
    }

    buffer[bytes_read] = '\0';
    printf("Read %zd bytes: %s", bytes_read, buffer);

    close(fd);
    return 0;
}
