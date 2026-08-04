#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "jne_demo_ioctl.h"

#define DEVICE_NAME "jne_demo"
#define BUFFER_SIZE 256
#define QUEUE_CAPACITY 16
#define MIN_STATUS_INTERVAL_MS 100

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

struct jne_demo_global_stats {
    atomic64_t opens;
    atomic64_t closes;
    atomic64_t messages_read;
    atomic64_t messages_written;
    atomic64_t bytes_read;
    atomic64_t bytes_written;
    atomic64_t async_jobs;
};

static bool status_reporting = true;
static unsigned int status_interval_ms = 5000;
static bool module_stopping;

static DEFINE_KFIFO(message_queue, struct jne_demo_message, QUEUE_CAPACITY);
static DEFINE_MUTEX(message_queue_mutex);
static atomic_t message_count = ATOMIC_INIT(0);

static DECLARE_WAIT_QUEUE_HEAD(read_wait_queue);
static DECLARE_WAIT_QUEUE_HEAD(write_wait_queue);

static struct workqueue_struct *message_work_queue;
static struct delayed_work status_work;
static struct dentry *debugfs_directory;
static struct jne_demo_global_stats global_stats;

static int jne_demo_set_status_interval(const char *value, const struct kernel_param *parameter);

static int jne_demo_debugfs_status_show(struct seq_file *file, void *data);
static int jne_demo_debugfs_status_open(struct inode *inode, struct file *file);
static int jne_demo_debugfs_messages_show(struct seq_file *file, void *data);
static int jne_demo_debugfs_messages_open(struct inode *inode, struct file *file);
static int jne_demo_debugfs_stats_show(struct seq_file *file, void *data);
static int jne_demo_debugfs_stats_open(struct inode *inode, struct file *file);

static int jne_demo_open(struct inode *inode, struct file *file);
static int jne_demo_release(struct inode *inode, struct file *file);
static ssize_t jne_demo_read(struct file *file, char __user *buffer, size_t count, loff_t *offset);
static ssize_t jne_demo_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static long jne_demo_ioctl(struct file *file, unsigned int command, unsigned long argument);
static __poll_t jne_demo_poll(struct file *file, poll_table *wait);

static void jne_demo_process_message(struct work_struct *work);
static void jne_demo_status_work(struct work_struct *work);

static int jne_demo_create_debugfs(void);
static void jne_demo_destroy_debugfs(void);
static void jne_demo_reset_global_stats(void);

static int __init jne_demo_init(void);
static void __exit jne_demo_exit(void);

static const struct kernel_param_ops status_interval_ops = {
    .set = jne_demo_set_status_interval,
    .get = param_get_uint,
};

module_param(status_reporting, bool, 0444);
MODULE_PARM_DESC(status_reporting, "Enable periodic queue status reporting");

module_param_cb(status_interval_ms, &status_interval_ops, &status_interval_ms, 0644);
MODULE_PARM_DESC(status_interval_ms, "Queue status reporting interval in milliseconds, minimum 100");

static const struct file_operations jne_demo_debugfs_status_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_debugfs_status_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static const struct file_operations jne_demo_debugfs_messages_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_debugfs_messages_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static const struct file_operations jne_demo_debugfs_stats_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_debugfs_stats_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static const struct file_operations jne_demo_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_open,
    .read = jne_demo_read,
    .write = jne_demo_write,
    .poll = jne_demo_poll,
    .release = jne_demo_release,
    .unlocked_ioctl = jne_demo_ioctl,
};

static struct miscdevice jne_demo_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &jne_demo_fops,
    .mode = 0666,
};

static int jne_demo_set_status_interval(const char *value, const struct kernel_param *parameter)
{
    unsigned int interval;
    int result;

    result = kstrtouint(value, 0, &interval);
    if (result) {
        return result;
    }

    if (interval < MIN_STATUS_INTERVAL_MS) {
        return -EINVAL;
    }

    *(unsigned int *)parameter->arg = interval;
    return 0;
}

static int jne_demo_debugfs_status_show(struct seq_file *file, void *data)
{
    int count;

    (void)data;

    count = atomic_read(&message_count);

    seq_printf(file, "messages: %d\n", count);
    seq_printf(file, "capacity: %d\n", QUEUE_CAPACITY);
    seq_printf(file, "available: %d\n", QUEUE_CAPACITY - count);
    seq_printf(file, "status_reporting: %s\n", status_reporting ? "enabled" : "disabled");
    seq_printf(file, "status_interval_ms: %u\n", status_interval_ms);

    return 0;
}

static int jne_demo_debugfs_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, jne_demo_debugfs_status_show, inode->i_private);
}

static int jne_demo_debugfs_messages_show(struct seq_file *file, void *data)
{
    struct jne_demo_message *messages;
    unsigned int message_count_snapshot;
    unsigned int copied;
    unsigned int index;

    (void)data;

    messages = kcalloc(QUEUE_CAPACITY, sizeof(*messages), GFP_KERNEL);
    if (!messages) {
        return -ENOMEM;
    }

    mutex_lock(&message_queue_mutex);

    message_count_snapshot = atomic_read(&message_count);
    copied = kfifo_out_peek(&message_queue, messages, message_count_snapshot);

    mutex_unlock(&message_queue_mutex);

    seq_printf(file, "messages: %u\n", copied);

    for (index = 0; index < copied; index++) {
        seq_printf(
            file,
            "[%u] length=%zu data=\"%.*s\"\n",
            index,
            messages[index].length,
            (int)messages[index].length,
            messages[index].data);
    }

    kfree(messages);
    return 0;
}

static int jne_demo_debugfs_messages_open(struct inode *inode, struct file *file)
{
    return single_open(file, jne_demo_debugfs_messages_show, inode->i_private);
}

static int jne_demo_debugfs_stats_show(struct seq_file *file, void *data)
{
    (void)data;

    seq_printf(file, "opens: %lld\n", atomic64_read(&global_stats.opens));
    seq_printf(file, "closes: %lld\n", atomic64_read(&global_stats.closes));
    seq_printf(file, "messages_read: %lld\n", atomic64_read(&global_stats.messages_read));
    seq_printf(file, "messages_written: %lld\n", atomic64_read(&global_stats.messages_written));
    seq_printf(file, "bytes_read: %lld\n", atomic64_read(&global_stats.bytes_read));
    seq_printf(file, "bytes_written: %lld\n", atomic64_read(&global_stats.bytes_written));
    seq_printf(file, "async_jobs: %lld\n", atomic64_read(&global_stats.async_jobs));

    return 0;
}

static int jne_demo_debugfs_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, jne_demo_debugfs_stats_show, inode->i_private);
}

static int jne_demo_open(struct inode *inode, struct file *file)
{
    struct jne_demo_file_state *state;

    (void)inode;

    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state) {
        return -ENOMEM;
    }

    file->private_data = state;
    atomic64_inc(&global_stats.opens);

    pr_info("jne_demo: device opened\n");
    return 0;
}

static int jne_demo_release(struct inode *inode, struct file *file)
{
    struct jne_demo_file_state *state = file->private_data;

    (void)inode;

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

    atomic64_inc(&global_stats.closes);
    return 0;
}

static ssize_t jne_demo_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_message message;
    ssize_t result;

    if (*offset > 0) {
        return 0;
    }

wait_for_message:
    if (atomic_read(&message_count) == 0) {
        if (file->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }

        pr_info("jne_demo: waiting for a message\n");

        result = wait_event_interruptible(
            read_wait_queue,
            atomic_read(&message_count) > 0);

        if (result) {
            return result;
        }
    }

    if (mutex_lock_interruptible(&message_queue_mutex)) {
        return -ERESTARTSYS;
    }

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

    atomic64_inc(&global_stats.messages_read);
    atomic64_add(message.length, &global_stats.bytes_read);

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

    if (result >= 0) {
        wake_up_interruptible(&write_wait_queue);
    }

    return result;
}

static ssize_t jne_demo_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_work *message_work;
    size_t bytes_to_copy;
    ssize_t result;

    (void)offset;

    if (count == 0) {
        return 0;
    }

    bytes_to_copy = min(count, (size_t)BUFFER_SIZE - 1);

    message_work = kzalloc(sizeof(*message_work), GFP_KERNEL);
    if (!message_work) {
        return -ENOMEM;
    }

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

        if (result) {
            goto free_work;
        }
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
    atomic64_inc(&global_stats.messages_written);
    atomic64_add(bytes_to_copy, &global_stats.bytes_written);

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

static long jne_demo_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
    struct jne_demo_file_state *state = file->private_data;
    struct jne_demo_message message;
    struct jne_demo_stats stats;
    unsigned int length = 0;
    long result = 0;

    if (_IOC_TYPE(command) != JNE_DEMO_IOC_MAGIC) {
        return -ENOTTY;
    }

    switch (command) {
    case JNE_DEMO_IOC_CLEAR:
        if (mutex_lock_interruptible(&message_queue_mutex)) {
            return -ERESTARTSYS;
        }

        kfifo_reset(&message_queue);
        atomic_set(&message_count, 0);

        mutex_unlock(&message_queue_mutex);
        wake_up_interruptible(&write_wait_queue);

        pr_info("jne_demo: message queue cleared\n");
        break;

    case JNE_DEMO_IOC_GET_LENGTH:
        if (mutex_lock_interruptible(&message_queue_mutex)) {
            return -ERESTARTSYS;
        }

        if (kfifo_peek(&message_queue, &message)) {
            length = message.length;
        }

        mutex_unlock(&message_queue_mutex);

        if (copy_to_user((unsigned int __user *)argument, &length, sizeof(length))) {
            result = -EFAULT;
        }

        break;

    case JNE_DEMO_IOC_GET_STATS:
        if (!state) {
            return -EINVAL;
        }

        stats.messages_read = state->messages_read;
        stats.messages_written = state->messages_written;
        stats.bytes_read = state->bytes_read;
        stats.bytes_written = state->bytes_written;

        if (copy_to_user(
                (struct jne_demo_stats __user *)argument,
                &stats,
                sizeof(stats))) {
            result = -EFAULT;
        }

        break;

    default:
        result = -ENOTTY;
        break;
    }

    return result;
}

static __poll_t jne_demo_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    int count;

    poll_wait(file, &read_wait_queue, wait);
    poll_wait(file, &write_wait_queue, wait);

    count = atomic_read(&message_count);

    if (count > 0) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }

    if (count < QUEUE_CAPACITY) {
        mask |= EPOLLOUT | EPOLLWRNORM;
    }

    return mask;
}

static void jne_demo_process_message(struct work_struct *work)
{
    struct jne_demo_work *message_work = container_of(work, struct jne_demo_work, work);
    unsigned int checksum = 0;
    size_t index;

    for (index = 0; index < message_work->message.length; index++) {
        checksum += (unsigned char)message_work->message.data[index];
    }

    pr_info(
        "jne_demo: asynchronously processed %zu bytes, checksum=%u\n",
        message_work->message.length,
        checksum);

    atomic64_inc(&global_stats.async_jobs);
    kfree(message_work);
}

static void jne_demo_status_work(struct work_struct *work)
{
    int count;

    (void)work;

    count = atomic_read(&message_count);

    pr_info(
        "jne_demo: queue status: %d/%d messages, %d slots available\n",
        count,
        QUEUE_CAPACITY,
        QUEUE_CAPACITY - count);

    if (!READ_ONCE(module_stopping)) {
        queue_delayed_work(
            message_work_queue,
            &status_work,
            msecs_to_jiffies(status_interval_ms));
    }
}

static int jne_demo_create_debugfs(void)
{
    struct dentry *entry;

    debugfs_directory = debugfs_create_dir(DEVICE_NAME, NULL);
    if (IS_ERR_OR_NULL(debugfs_directory)) {
        return debugfs_directory ? PTR_ERR(debugfs_directory) : -ENOMEM;
    }

    entry = debugfs_create_file(
        "status",
        0444,
        debugfs_directory,
        NULL,
        &jne_demo_debugfs_status_fops);
    if (IS_ERR_OR_NULL(entry)) {
        goto error;
    }

    entry = debugfs_create_file(
        "messages",
        0444,
        debugfs_directory,
        NULL,
        &jne_demo_debugfs_messages_fops);
    if (IS_ERR_OR_NULL(entry)) {
        goto error;
    }

    entry = debugfs_create_file(
        "stats",
        0444,
        debugfs_directory,
        NULL,
        &jne_demo_debugfs_stats_fops);
    if (IS_ERR_OR_NULL(entry)) {
        goto error;
    }

    return 0;

error:
    jne_demo_destroy_debugfs();
    return entry ? PTR_ERR(entry) : -ENOMEM;
}

static void jne_demo_destroy_debugfs(void)
{
    debugfs_remove_recursive(debugfs_directory);
    debugfs_directory = NULL;
}

static void jne_demo_reset_global_stats(void)
{
    atomic64_set(&global_stats.opens, 0);
    atomic64_set(&global_stats.closes, 0);
    atomic64_set(&global_stats.messages_read, 0);
    atomic64_set(&global_stats.messages_written, 0);
    atomic64_set(&global_stats.bytes_read, 0);
    atomic64_set(&global_stats.bytes_written, 0);
    atomic64_set(&global_stats.async_jobs, 0);
}

static int __init jne_demo_init(void)
{
    int result;

    WRITE_ONCE(module_stopping, false);

    kfifo_reset(&message_queue);
    atomic_set(&message_count, 0);
    jne_demo_reset_global_stats();

    message_work_queue = alloc_ordered_workqueue("jne_demo_wq", 0);
    if (!message_work_queue) {
        return -ENOMEM;
    }

    INIT_DELAYED_WORK(&status_work, jne_demo_status_work);

    result = misc_register(&jne_demo_device);
    if (result) {
        pr_err("jne_demo: failed to register device: %d\n", result);
        goto destroy_workqueue;
    }

    result = jne_demo_create_debugfs();
    if (result) {
        pr_err("jne_demo: failed to create debugfs entries: %d\n", result);
        goto deregister_device;
    }

    if (status_reporting) {
        queue_delayed_work(
            message_work_queue,
            &status_work,
            msecs_to_jiffies(status_interval_ms));
    }

    pr_info(
        "jne_demo: status reporting %s, interval=%u ms\n",
        status_reporting ? "enabled" : "disabled",
        status_interval_ms);
    pr_info("jne_demo: module loaded\n");

    return 0;

deregister_device:
    misc_deregister(&jne_demo_device);

destroy_workqueue:
    destroy_workqueue(message_work_queue);
    message_work_queue = NULL;

    return result;
}

static void __exit jne_demo_exit(void)
{
    jne_demo_destroy_debugfs();
    misc_deregister(&jne_demo_device);

    WRITE_ONCE(module_stopping, true);
    cancel_delayed_work_sync(&status_work);

    destroy_workqueue(message_work_queue);
    message_work_queue = NULL;

    pr_info("jne_demo: module unloaded\n");
}

module_init(jne_demo_init);
module_exit(jne_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eugene Kutuzov");
MODULE_DESCRIPTION("Linux blocking misc character device code example");
MODULE_VERSION("1.2");
