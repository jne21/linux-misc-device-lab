#ifndef JNE_DEMO_IOCTL_H
#define JNE_DEMO_IOCTL_H

#include <linux/ioctl.h>

#define JNE_DEMO_IOC_MAGIC 'J'

#define JNE_DEMO_IOC_CLEAR _IO(JNE_DEMO_IOC_MAGIC, 1)
#define JNE_DEMO_IOC_GET_LENGTH _IOR(JNE_DEMO_IOC_MAGIC, 2, unsigned int)

#endif
