#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "jne_demo_ioctl.h"

#define DEVICE_NAME "jne_demo"
#define BUFFER_SIZE 256
#define QUEUE_CAPACITY 16

//--------------------------------------------------------------------------------


struct jne_demo_file_state {
    unsigned long messages_read;
    unsigned long messages_written;
    unsigned long bytes_read;
    unsigned long bytes_written;
};

struct jne_demo_message {
    size_t length;
    char data[BUFFER_SIZE];
};

struct jne_demo_work {
    struct work_struct work;
    struct jne_demo_message message;
};

static struct workqueue_struct *message_work_queue;

static DEFINE_KFIFO(message_queue, struct jne_demo_message, QUEUE_CAPACITY);
static DEFINE_MUTEX(message_queue_mutex);
static atomic_t message_count = ATOMIC_INIT(0);

static DECLARE_WAIT_QUEUE_HEAD(read_wait_queue);
static DECLARE_WAIT_QUEUE_HEAD(write_wait_queue);

//--------------------------------------------------------------------------------

static ssize_t jne_demo_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_message message;
    ssize_t result;

    if (*offset > 0)
        return 0;

wait_for_message:
    if (atomic_read(&message_count) == 0) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        pr_info("jne_demo: waiting for a message\n");

        result = wait_event_interruptible(
            read_wait_queue,
            atomic_read(&message_count) > 0);

        if (result)
            return result;
    }

    if (mutex_lock_interruptible(&message_queue_mutex))
        return -ERESTARTSYS;

    if (!kfifo_peek(&message_queue, &message)) {
        mutex_unlock(&message_queue_mutex);
        goto wait_for_message;
    }

    if (count < message.length) {
        result = -EMSGSIZE;
        goto unlock;
    }

    if (copy_to_user(buffer, message.data, message.length)) {
        result = -EFAULT;
        goto unlock;
    }

    kfifo_skip(&message_queue);
    atomic_dec(&message_count);

    *offset += message.length;
    result = message.length;

    if (state) {
        state->messages_read++;
        state->bytes_read += message.length;
    }

    pr_info(
        "jne_demo: read %zu bytes, %d messages remain\n",
        message.length,
        atomic_read(&message_count));

unlock:
    mutex_unlock(&message_queue_mutex);

    if (result >= 0)
        wake_up_interruptible(&write_wait_queue);

    return result;
}

//--------------------------------------------------------------------------------

static void jne_demo_process_message(struct work_struct *work)
{
    struct jne_demo_work *message_work = container_of(work, struct jne_demo_work, work);
    unsigned int checksum = 0;
    size_t index;

    for (index = 0; index < message_work->message.length; index++)
        checksum += (unsigned char)message_work->message.data[index];

    pr_info(
        "jne_demo: asynchronously processed %zu bytes, checksum=%u\n",
        message_work->message.length,
        checksum);

    kfree(message_work);
}

//--------------------------------------------------------------------------------

static ssize_t jne_demo_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_work *message_work;
    size_t bytes_to_copy;
    ssize_t result;

    if (count == 0)
        return 0;

    bytes_to_copy = min(count, (size_t)BUFFER_SIZE - 1);

    message_work = kzalloc(sizeof(*message_work), GFP_KERNEL);
    if (!message_work)
        return -ENOMEM;

    if (copy_from_user(message_work->message.data, buffer, bytes_to_copy)) {
        kfree(message_work);
        return -EFAULT;
    }

    message_work->message.data[bytes_to_copy] = '\0';
    message_work->message.length = bytes_to_copy;

wait_for_space:
    if (atomic_read(&message_count) >= QUEUE_CAPACITY) {
        if (file->f_flags & O_NONBLOCK) {
            result = -EAGAIN;
            goto free_work;
        }

        pr_info("jne_demo: waiting for queue space\n");

        result = wait_event_interruptible(
            write_wait_queue,
            atomic_read(&message_count) < QUEUE_CAPACITY);

        if (result)
            goto free_work;
    }

    if (mutex_lock_interruptible(&message_queue_mutex)) {
        result = -ERESTARTSYS;
        goto free_work;
    }

    if (kfifo_is_full(&message_queue)) {
        mutex_unlock(&message_queue_mutex);
        goto wait_for_space;
    }

    if (!kfifo_put(&message_queue, message_work->message)) {
        mutex_unlock(&message_queue_mutex);
        goto wait_for_space;
    }

    atomic_inc(&message_count);
    result = bytes_to_copy;

    if (state) {
        state->messages_written++;
        state->bytes_written += bytes_to_copy;
    }

    pr_info(
        "jne_demo: queued %zu bytes, %d messages available\n",
        bytes_to_copy,
        atomic_read(&message_count));

    mutex_unlock(&message_queue_mutex);

    INIT_WORK(&message_work->work, jne_demo_process_message);
    queue_work(message_work_queue, &message_work->work);

    wake_up_interruptible(&read_wait_queue);
    return result;

free_work:
    kfree(message_work);
    return result;
}

//--------------------------------------------------------------------------------

static long jne_demo_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_message message;
    struct jne_demo_stats stats;
    unsigned int length = 0;
    long result = 0;

    if (_IOC_TYPE(command) != JNE_DEMO_IOC_MAGIC)
        return -ENOTTY;

    switch (command) {
        case JNE_DEMO_IOC_CLEAR:
            if (mutex_lock_interruptible(&message_queue_mutex))
                return -ERESTARTSYS;

            kfifo_reset(&message_queue);
            atomic_set(&message_count, 0);

            mutex_unlock(&message_queue_mutex);

            wake_up_interruptible(&write_wait_queue);

            pr_info("jne_demo: message queue cleared\n");
            break;

        case JNE_DEMO_IOC_GET_LENGTH:
            if (mutex_lock_interruptible(&message_queue_mutex))
                return -ERESTARTSYS;

            if (kfifo_peek(&message_queue, &message))
                length = message.length;

            mutex_unlock(&message_queue_mutex);

            if (copy_to_user((unsigned int __user *)argument, &length, sizeof(length)))
                result = -EFAULT;

            break;

        case JNE_DEMO_IOC_GET_STATS:
            if (!state)
                return -EINVAL;

            stats.messages_read = state->messages_read;
            stats.messages_written = state->messages_written;
            stats.bytes_read = state->bytes_read;
            stats.bytes_written = state->bytes_written;

            if (copy_to_user(
                    (struct jne_demo_stats __user *)argument,
                    &stats,
                    sizeof(stats)))
                result = -EFAULT;

            break;

        default:
            result = -ENOTTY;
            break;
    }

    return result;
}

//--------------------------------------------------------------------------------

static __poll_t jne_demo_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    int count;

    poll_wait(file, &read_wait_queue, wait);
    poll_wait(file, &write_wait_queue, wait);

    count = atomic_read(&message_count);

    if (count > 0)
        mask |= EPOLLIN | EPOLLRDNORM;

    if (count < QUEUE_CAPACITY)
        mask |= EPOLLOUT | EPOLLWRNORM;

    return mask;
}

//--------------------------------------------------------------------------------

static int jne_demo_open(struct inode *inode, struct file *file)
{
    struct jne_demo_file_state *state;

    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;

    file->private_data = state;

    pr_info("jne_demo: device opened\n");
    return 0;
}

//--------------------------------------------------------------------------------

static int jne_demo_release(struct inode *inode, struct file *file)
{
    struct jne_demo_file_state *state = file->private_data;

    if (state) {
        pr_info(
            "jne_demo: device closed, read=%lu messages/%lu bytes, written=%lu messages/%lu bytes\n",
            state->messages_read,
            state->bytes_read,
            state->messages_written,
            state->bytes_written);

        kfree(state);
        file->private_data = NULL;
    }

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
    .unlocked_ioctl = jne_demo_ioctl,
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
    int result;

    kfifo_reset(&message_queue);
    atomic_set(&message_count, 0);

    message_work_queue = alloc_ordered_workqueue("jne_demo_wq", 0);
    if (!message_work_queue)
        return -ENOMEM;

    result = misc_register(&jne_demo_device);
    if (result) {
        destroy_workqueue(message_work_queue);
        message_work_queue = NULL;

        pr_err("jne_demo: failed to register device: %d\n", result);
        return result;
    }

    pr_info("jne_demo: module loaded\n");
    return 0;
}

//--------------------------------------------------------------------------------

static void __exit jne_demo_exit(void)
{
    misc_deregister(&jne_demo_device);

    destroy_workqueue(message_work_queue);
    message_work_queue = NULL;

    pr_info("jne_demo: module unloaded\n");
}

//--------------------------------------------------------------------------------

module_init(jne_demo_init);
module_exit(jne_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eugene Kutuzov");
MODULE_DESCRIPTION("Linux blocking misc character device code example");
MODULE_VERSION("1.2");
