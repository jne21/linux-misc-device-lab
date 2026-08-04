#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "./../jne_demo_ioctl.h"

#define DEVICE_PATH "/dev/jne_demo"

static int get_length(int fd)
{
    unsigned int length;

    if (ioctl(fd, JNE_DEMO_IOC_GET_LENGTH, &length) < 0) {
        fprintf(stderr, "GET_LENGTH: %s\n", strerror(errno));
        return -1;
    }

    printf("Buffer length: %u bytes\n", length);
    return 0;
}

int main(void)
{
    int fd = open(DEVICE_PATH, O_RDWR);

    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (get_length(fd) < 0) {
        close(fd);
        return 1;
    }

    if (ioctl(fd, JNE_DEMO_IOC_CLEAR) < 0) {
        fprintf(stderr, "CLEAR: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("Buffer cleared\n");

    if (get_length(fd) < 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
