#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>

#define DEVICE_NAME "jne_demo"
#define BUFFER_SIZE 256

//--------------------------------------------------------------------------------

static char device_buffer[BUFFER_SIZE];
static size_t device_buffer_length;
static bool data_available;

static DEFINE_MUTEX(device_buffer_mutex);
static DECLARE_WAIT_QUEUE_HEAD(data_wait_queue);

//--------------------------------------------------------------------------------

static ssize_t jne_demo_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
    size_t bytes_to_copy;
    ssize_t result;

    if (*offset > 0)
        return 0;

wait_for_data:
    if (!READ_ONCE(data_available)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        pr_info("jne_demo: waiting for data\n");

        result = wait_event_interruptible(data_wait_queue, READ_ONCE(data_available));
        if (result)
            return result;
    }

    if (mutex_lock_interruptible(&device_buffer_mutex))
        return -ERESTARTSYS;

    if (!data_available) {
        mutex_unlock(&device_buffer_mutex);
        goto wait_for_data;
    }

    bytes_to_copy = min(count, device_buffer_length);

    if (copy_to_user(buffer, device_buffer, bytes_to_copy)) {
        result = -EFAULT;
        goto unlock;
    }

    *offset += bytes_to_copy;
    data_available = false;

    pr_info("jne_demo: read %zu bytes\n", bytes_to_copy);

    result = bytes_to_copy;

unlock:
    mutex_unlock(&device_buffer_mutex);
    return result;
}

//--------------------------------------------------------------------------------

static ssize_t jne_demo_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    size_t bytes_to_copy = min(count, (size_t)BUFFER_SIZE - 1);
    ssize_t result;

    if (mutex_lock_interruptible(&device_buffer_mutex))
        return -ERESTARTSYS;

    if (copy_from_user(device_buffer, buffer, bytes_to_copy)) {
        result = -EFAULT;
        goto unlock;
    }

    device_buffer[bytes_to_copy] = '\0';
    device_buffer_length = bytes_to_copy;
    WRITE_ONCE(data_available, true);

    pr_info("jne_demo: wrote %zu bytes\n", bytes_to_copy);

    result = bytes_to_copy;

unlock:
    mutex_unlock(&device_buffer_mutex);

    if (result >= 0)
        wake_up_interruptible(&data_wait_queue);

    return result;
}


//--------------------------------------------------------------------------------

static __poll_t jne_demo_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(file, &data_wait_queue, wait);

    if (READ_ONCE(data_available))
        mask |= EPOLLIN | EPOLLRDNORM;

    mask |= EPOLLOUT | EPOLLWRNORM;

    return mask;
}

//--------------------------------------------------------------------------------

static int jne_demo_open(struct inode *inode, struct file *file)
{
    pr_info("jne_demo: device opened\n");
    return 0;
}

static int jne_demo_release(struct inode *inode, struct file *file)
{
    pr_info("jne_demo: device closed\n");
    return 0;
}

//--------------------------------------------------------------------------------

static const struct file_operations jne_demo_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_open,
    .read = jne_demo_read,
    .write = jne_demo_write,
    .poll = jne_demo_poll,
    .release = jne_demo_release,
};

//--------------------------------------------------------------------------------

static struct miscdevice jne_demo_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &jne_demo_fops,
    .mode = 0666,
};

//--------------------------------------------------------------------------------

static int __init jne_demo_init(void)
{
    int result = misc_register(&jne_demo_device);

    if (result) {
        pr_err("jne_demo: failed to register device: %d\n", result);
        return result;
    }

    pr_info("jne_demo: module loaded, device /dev/%s registered\n", DEVICE_NAME);
    return 0;
}
//--------------------------------------------------------------------------------

static void __exit jne_demo_exit(void)
{
    misc_deregister(&jne_demo_device);
    pr_info("jne_demo: module unloaded\n");
}

//--------------------------------------------------------------------------------

module_init(jne_demo_init);
module_exit(jne_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eugene Kutuzov");
MODULE_DESCRIPTION("Linux blocking misc character device code example");
MODULE_VERSION("1.2");
