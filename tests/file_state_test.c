#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "jne_demo_ioctl.h"

#define DEVICE_PATH "/dev/jne_demo"

static void print_stats(int fd)
{
    struct jne_demo_stats stats;

    if (ioctl(fd, JNE_DEMO_IOC_GET_STATS, &stats) < 0) {
        perror("JNE_DEMO_IOC_GET_STATS");
        return;
    }

    printf("Messages read:    %lu\n", stats.messages_read);
    printf("Messages written: %lu\n", stats.messages_written);
    printf("Bytes read:       %lu\n", stats.bytes_read);
    printf("Bytes written:    %lu\n", stats.bytes_written);
}

int main(void)
{
    static const char first_message[] = "First message\n";
    static const char second_message[] = "Second message\n";

    char buffer[256];
    ssize_t bytes_read;
    int fd;

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (write(fd, first_message, sizeof(first_message) - 1) < 0 ||
        write(fd, second_message, sizeof(second_message) - 1) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    buffer[bytes_read] = '\0';
    printf("Read: %s", buffer);

    print_stats(fd);

    close(fd);
    return 0;
}
