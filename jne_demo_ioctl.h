#ifndef JNE_DEMO_IOCTL_H
#define JNE_DEMO_IOCTL_H

#include <linux/ioctl.h>

#define JNE_DEMO_IOC_MAGIC 'J'

#define JNE_DEMO_IOC_CLEAR _IO(JNE_DEMO_IOC_MAGIC, 1)
#define JNE_DEMO_IOC_GET_LENGTH _IOR(JNE_DEMO_IOC_MAGIC, 2, unsigned int)
#define JNE_DEMO_IOC_GET_STATS _IOR(JNE_DEMO_IOC_MAGIC, 3, struct jne_demo_stats)

#endif

struct jne_demo_stats {
    unsigned long messages_read;
    unsigned long messages_written;
    unsigned long bytes_read;
    unsigned long bytes_written;
};
