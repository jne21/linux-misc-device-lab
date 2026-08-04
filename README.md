# Linux Misc Character Device Lab

![Language](https://img.shields.io/badge/language-C-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Kernel Module](https://img.shields.io/badge/type-kernel%20module-orange)
![Status](https://img.shields.io/badge/status-learning%20project-green)

An demonstration Linux kernel module that implements a message-oriented miscellaneous character device:

```text
/dev/jne_demo
```

The project demonstrates communication between user space and kernel space through standard Linux file operations, blocking and non-blocking I/O, wait queues, polling, synchronization, `ioctl()` commands, typed kernel FIFO storage, per-open state, asynchronous workqueue processing, and periodic delayed work.

## Features

| Feature                   | Implementation                  |
| ------------------------- | ------------------------------- |
| Character device          | Linux `miscdevice` API          |
| Device path               | `/dev/jne_demo`                 |
| Message storage           | Typed `kfifo`                   |
| Queue capacity            | 16 messages                     |
| Maximum message size      | 255 bytes                       |
| Reading                   | `read()` callback               |
| Writing                   | `write()` callback              |
| User-to-kernel transfer   | `copy_from_user()`              |
| Kernel-to-user transfer   | `copy_to_user()`                |
| Queue synchronization     | Kernel `mutex`                  |
| Queue state               | `atomic_t` message counter      |
| Blocking reads            | Reader wait queue               |
| Blocking writes           | Writer wait queue               |
| Non-blocking I/O          | `O_NONBLOCK` and `-EAGAIN`      |
| Readiness notification    | `poll()`, `select()`, `epoll()` |
| Control commands          | `ioctl()`                       |
| Per-open statistics       | `file->private_data`            |
| Asynchronous processing   | Ordered kernel workqueue        |
| Periodic status reporting | `delayed_work` every 5 seconds  |
| Diagnostics               | `pr_info()` and `dmesg`         |
| Build output              | `bin/` directory                |

## Architecture

```text
┌───────────────────────────────────────────────┐
│                  User space                   │
│                                               │
│  cat / echo / poll_read / ioctl_test          │
│  nonblock_read / nonblock_write               │
│  file_state_test                              │
└──────────────────────┬────────────────────────┘
                       │
          open / read / write / poll / ioctl
                       │
┌──────────────────────▼────────────────────────┐
│                   Linux VFS                   │
└──────────────────────┬────────────────────────┘
                       │
┌──────────────────────▼────────────────────────┐
│              jne_demo kernel module           │
│                                               │
│  ┌─────────────────────────────────────────┐  │
│  │ Typed kfifo message queue               │  │
│  │ Capacity: 16 × jne_demo_message         │  │
│  └─────────────────────────────────────────┘  │
│                                               │
│  queue mutex                                  │
│  atomic message counter                       │
│  reader wait queue                            │
│  writer wait queue                            │
│  per-open file state                          │
│  ordered workqueue                            │
│  periodic delayed work                        │
└──────────────────────┬────────────────────────┘
                       │
               asynchronous work
                       │
┌──────────────────────▼────────────────────────┐
│             jne_demo_wq workers               │
│                                               │
│  asynchronous checksum processing             │
│  periodic FIFO status reporting               │
└───────────────────────────────────────────────┘
```

## Message Queue

Each call to `write()` creates one independent message:

```c
struct jne_demo_message {
    size_t length;
    char data[BUFFER_SIZE];
};
```

Messages are stored in a typed kernel FIFO:

```c
static DEFINE_KFIFO(
    message_queue,
    struct jne_demo_message,
    QUEUE_CAPACITY);
```

Configuration:

```c
#define BUFFER_SIZE 256
#define QUEUE_CAPACITY 16
```

The maximum payload is 255 bytes because the last byte is reserved for the terminating null character.

Messages are returned in FIFO order:

```text
write "Message one"
write "Message two"
write "Message three"

read → "Message one"
read → "Message two"
read → "Message three"
```

Each successful `read()` removes exactly one complete message.

## Asynchronous Processing

After a message is successfully added to the FIFO, the driver also submits a copy to an ordered kernel workqueue:

```c
struct jne_demo_work {
    struct work_struct work;
    struct jne_demo_message message;
};
```

The workqueue is created during module initialization:

```c
message_work_queue = alloc_ordered_workqueue("jne_demo_wq", 0);
```

The writer schedules asynchronous processing without waiting for it to finish:

```c
INIT_WORK(&message_work->work, jne_demo_process_message);
queue_work(message_work_queue, &message_work->work);
```

The worker calculates a simple checksum:

```c
static void jne_demo_process_message(struct work_struct *work)
{
    struct jne_demo_work *message_work = container_of(
        work,
        struct jne_demo_work,
        work);

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
```

An ordered workqueue executes submitted work items sequentially and preserves their order.

The original message remains in the FIFO and can still be read through `/dev/jne_demo`.

## Periodic Delayed Work

The module periodically reports the current FIFO state through a delayed work item.

The reporting interval is configured as:

```c
#define STATUS_INTERVAL_MS 5000
```

A static delayed work object is used:

```c
static struct delayed_work status_work;
```

The callback reports the number of occupied and available queue positions:

```c
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

    if (!READ_ONCE(module_stopping))
        queue_delayed_work(
            message_work_queue,
            &status_work,
            msecs_to_jiffies(STATUS_INTERVAL_MS));
}
```

The delayed work is initialized during module startup:

```c
INIT_DELAYED_WORK(&status_work, jne_demo_status_work);
```

The first execution is scheduled after the device has been registered:

```c
queue_delayed_work(
    message_work_queue,
    &status_work,
    msecs_to_jiffies(STATUS_INTERVAL_MS));
```

Each callback schedules the next execution, creating periodic status reporting without a busy loop.

The interval is converted from milliseconds to kernel ticks with:

```c
msecs_to_jiffies(STATUS_INTERVAL_MS)
```

Example kernel log output:

```text
jne_demo: queue status: 0/16 messages, 16 slots available
jne_demo: queue status: 3/16 messages, 13 slots available
jne_demo: queue status: 2/16 messages, 14 slots available
```

### Safe Cancellation

The module uses a stopping flag to prevent the callback from scheduling another execution while the module is being unloaded:

```c
static bool module_stopping;
```

The flag is reset during module initialization:

```c
WRITE_ONCE(module_stopping, false);
```

Before unloading, the module sets it to `true`:

```c
WRITE_ONCE(module_stopping, true);
```

The callback checks the flag before scheduling itself again:

```c
if (!READ_ONCE(module_stopping))
    queue_delayed_work(
        message_work_queue,
        &status_work,
        msecs_to_jiffies(STATUS_INTERVAL_MS));
```

The delayed work is cancelled synchronously before the workqueue is destroyed:

```c
WRITE_ONCE(module_stopping, true);
cancel_delayed_work_sync(&status_work);
destroy_workqueue(message_work_queue);
```

`cancel_delayed_work_sync()` cancels a pending delayed execution and waits for a currently running callback to finish.

## Per-Open State

Each successful `open()` allocates a separate state object:

```c
struct jne_demo_file_state {
    unsigned long messages_read;
    unsigned long messages_written;
    unsigned long bytes_read;
    unsigned long bytes_written;
};
```

The state is stored in:

```c
file->private_data
```

Initialization:

```c
state = kzalloc(sizeof(*state), GFP_KERNEL);
if (!state)
    return -ENOMEM;

file->private_data = state;
```

Each open file descriptor therefore has independent statistics.

The memory is released in `release()`:

```c
kfree(state);
file->private_data = NULL;
```

When a file descriptor is closed, its statistics are written to the kernel log.

## Project Structure

```text
.
├── jne_demo.c
├── jne_demo_ioctl.h
├── nonblock_read.c
├── nonblock_write.c
├── poll_read.c
├── ioctl_test.c
├── file_state_test.c
├── Makefile
├── .gitignore
├── README.md
└── bin/
    ├── jne_demo.ko
    ├── nonblock_read
    ├── nonblock_write
    ├── poll_read
    ├── ioctl_test
    └── file_state_test
```

The `bin/` directory contains generated binaries and is excluded from Git.

## Requirements

The module must be built against the headers of the currently running Linux kernel.

### Debian

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

Initially tested with:

```text
Debian 13
Linux kernel 6.12
x86_64
GCC 14
```

## Build

Build the kernel module and user-space test programs:

```bash
make
```

Generated binaries are placed in:

```text
bin/
```

The kernel module is created as:

```text
bin/jne_demo.ko
```

Remove generated files:

```bash
make clean
```

## Load the Module

Using the Makefile:

```bash
make load
```

Or manually:

```bash
sudo insmod bin/jne_demo.ko
```

Verify that the module is loaded:

```bash
lsmod | grep jne_demo
```

Verify that the device exists:

```bash
ls -l /dev/jne_demo
```

Expected device type:

```text
crw-rw-rw- ... /dev/jne_demo
```

The first character, `c`, means that this is a character device.

## Reload the Module

```bash
make reload
```

This unloads the current module, rebuilds it, and loads the new version.

## Kernel Log

Open a separate terminal:

```bash
sudo dmesg -w
```

Example messages:

```text
jne_demo: module loaded
jne_demo: device opened
jne_demo: queued 12 bytes, 1 messages available
jne_demo: asynchronously processed 12 bytes, checksum=...
jne_demo: queue status: 1/16 messages, 15 slots available
jne_demo: read 12 bytes, 0 messages remain
jne_demo: device closed, read=1 messages/12 bytes, written=1 messages/12 bytes
jne_demo: module unloaded
```

## Basic FIFO Test

Write three messages:

```bash
printf 'Message one\n' > /dev/jne_demo
printf 'Message two\n' > /dev/jne_demo
printf 'Message three\n' > /dev/jne_demo
```

Read them:

```bash
cat /dev/jne_demo
cat /dev/jne_demo
cat /dev/jne_demo
```

Expected output:

```text
Message one
Message two
Message three
```

## Blocking Read

Start a reader while the queue is empty:

```bash
cat /dev/jne_demo
```

The process blocks without continuously consuming CPU time.

In another terminal:

```bash
echo "Hello from user space" > /dev/jne_demo
```

The blocked reader wakes and prints:

```text
Hello from user space
```

The wait can be interrupted with `Ctrl+C`.

The driver waits with:

```c
result = wait_event_interruptible(
    read_wait_queue,
    atomic_read(&message_count) > 0);
```

After adding a message, the writer wakes readers:

```c
wake_up_interruptible(&read_wait_queue);
```

## Blocking Write

The FIFO holds 16 messages.

Fill it:

```bash
for i in $(seq 1 16); do
    printf 'Message %02d\n' "$i" > /dev/jne_demo
done
```

Attempt to write the seventeenth message:

```bash
echo "Message 17" > /dev/jne_demo
```

The command blocks because no queue slot is available.

In another terminal:

```bash
cat /dev/jne_demo
```

Expected output:

```text
Message 01
```

The blocked writer then wakes and adds `Message 17`.

The writer waits with:

```c
result = wait_event_interruptible(
    write_wait_queue,
    atomic_read(&message_count) < QUEUE_CAPACITY);
```

After consuming a message, the reader wakes writers:

```c
wake_up_interruptible(&write_wait_queue);
```

## Non-Blocking Read

Run the test while the queue is empty:

```bash
bin/nonblock_read
```

Expected result:

```text
No data available
```

Write a message:

```bash
echo "Non-blocking read message" > /dev/jne_demo
```

Run the test again:

```bash
bin/nonblock_read
```

Expected result:

```text
Read 26 bytes: Non-blocking read message
```

The test opens the device with:

```c
fd = open("/dev/jne_demo", O_RDONLY | O_NONBLOCK);
```

When the queue is empty, the driver returns:

```c
return -EAGAIN;
```

In user space:

```c
read(...) == -1;
errno == EAGAIN;
```

## Non-Blocking Write

When the queue has free space:

```bash
bin/nonblock_write
```

Expected result:

```text
Written 28 bytes
```

When all 16 queue positions are occupied:

```bash
bin/nonblock_write
```

Expected result:

```text
Queue is full
```

The test opens the device with:

```c
fd = open("/dev/jne_demo", O_WRONLY | O_NONBLOCK);
```

When the queue is full, the driver returns:

```c
return -EAGAIN;
```

## `poll()` Support

The driver supports readiness notification through:

* `poll()`;
* `select()`;
* `epoll()`.

The callback registers both wait queues:

```c
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
```

Read readiness:

```c
EPOLLIN | EPOLLRDNORM
```

Write readiness:

```c
EPOLLOUT | EPOLLWRNORM
```

Run the test:

```bash
bin/poll_read
```

Expected initial output:

```text
Waiting for data...
```

In another terminal:

```bash
echo "Message received through poll" > /dev/jne_demo
```

The reader wakes:

```text
Read 30 bytes: Message received through poll
```

## `ioctl()` Commands

The shared header defines the control interface:

```c
#define JNE_DEMO_IOC_MAGIC 'J'

#define JNE_DEMO_IOC_CLEAR \
    _IO(JNE_DEMO_IOC_MAGIC, 1)

#define JNE_DEMO_IOC_GET_LENGTH \
    _IOR(JNE_DEMO_IOC_MAGIC, 2, unsigned int)

#define JNE_DEMO_IOC_GET_STATS \
    _IOR(JNE_DEMO_IOC_MAGIC, 3, struct jne_demo_stats)
```

Available commands:

| Command                   | Description                                            |
| ------------------------- | ------------------------------------------------------ |
| `JNE_DEMO_IOC_CLEAR`      | Remove every message from the FIFO                     |
| `JNE_DEMO_IOC_GET_LENGTH` | Return the length of the next message                  |
| `JNE_DEMO_IOC_GET_STATS`  | Return statistics for the current open file descriptor |

Run the basic control test:

```bash
echo "Message for ioctl test" > /dev/jne_demo
bin/ioctl_test
```

Example output:

```text
Buffer length: 23 bytes
Buffer cleared
Buffer length: 0 bytes
```

## Per-Open Statistics Test

Run:

```bash
bin/file_state_test
```

Example output:

```text
Read: First message
Messages read:    1
Messages written: 2
Bytes read:       14
Bytes written:    29
```

These counters belong only to the file descriptor opened by `file_state_test`.

A different process opening `/dev/jne_demo` receives a new zero-initialized state object.

## Data Transfer

Kernel code must not directly dereference user-space pointers.

User-to-kernel transfer:

```c
copy_from_user(
    message_work->message.data,
    buffer,
    bytes_to_copy);
```

Kernel-to-user transfer:

```c
copy_to_user(
    buffer,
    message.data,
    message.length);
```

The message is removed from the FIFO only after a successful `copy_to_user()`:

```c
kfifo_peek(&message_queue, &message);

/* copy_to_user() */

kfifo_skip(&message_queue);
```

This prevents message loss when copying to user space fails.

## Synchronization

The FIFO is protected by a kernel mutex:

```c
static DEFINE_MUTEX(message_queue_mutex);
```

The current number of queued messages is tracked through:

```c
static atomic_t message_count = ATOMIC_INIT(0);
```

The mutex protects compound FIFO operations. The atomic counter provides a lightweight condition for wait queues, `poll()`, and periodic status reporting.

The code rechecks FIFO state after acquiring the mutex because another reader or writer may have changed the queue between the initial state check and lock acquisition.

## File Operations

Callbacks are connected through `struct file_operations`:

```c
static const struct file_operations jne_demo_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_open,
    .read = jne_demo_read,
    .write = jne_demo_write,
    .poll = jne_demo_poll,
    .unlocked_ioctl = jne_demo_ioctl,
    .release = jne_demo_release,
};
```

The device is registered through `miscdevice`:

```c
static struct miscdevice jne_demo_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "jne_demo",
    .fops = &jne_demo_fops,
    .mode = 0666,
};
```

`MISC_DYNAMIC_MINOR` allows the kernel to assign the minor number automatically.

## Safe Module Unloading

The device is deregistered first so that no new file descriptors can be opened:

```c
misc_deregister(&jne_demo_device);
```

The periodic callback is prevented from scheduling itself again:

```c
WRITE_ONCE(module_stopping, true);
```

Pending or running delayed work is cancelled synchronously:

```c
cancel_delayed_work_sync(&status_work);
```

The ordered workqueue is then destroyed:

```c
destroy_workqueue(message_work_queue);
message_work_queue = NULL;
```

The complete sequence is:

```c
static void __exit jne_demo_exit(void)
{
    misc_deregister(&jne_demo_device);

    WRITE_ONCE(module_stopping, true);
    cancel_delayed_work_sync(&status_work);

    destroy_workqueue(message_work_queue);
    message_work_queue = NULL;

    pr_info("jne_demo: module unloaded\n");
}
```

Unload the module:

```bash
make unload
```

Or manually:

```bash
sudo rmmod jne_demo
```

Verify that it is gone:

```bash
lsmod | grep jne_demo
ls -l /dev/jne_demo
```

## Typical Development Cycle

```bash
make clean
make
make reload
sudo dmesg | tail -n 30
```

After testing:

```bash
make unload
```

## Safety

> [!WARNING]
> Linux kernel modules execute with full kernel privileges. A programming error may freeze the system or cause a kernel panic.

Recommended precautions:

* save open files before loading experimental modules;
* do not add unfinished modules to automatic startup;
* do not copy experimental modules into `/lib/modules`;
* do not use forced module unloading;
* use a virtual machine for intentionally unsafe experiments.

## Progress

* [x] Basic miscellaneous character device
* [x] `open()` and `release()`
* [x] `read()` and `write()`
* [x] Safe user-space memory transfer
* [x] Mutex synchronization
* [x] Blocking reads
* [x] Non-blocking reads
* [x] Blocking writes
* [x] Non-blocking writes
* [x] `poll()` support
* [x] `ioctl()` commands
* [x] Typed `kfifo` message queue
* [x] Per-open file state
* [x] Per-open statistics
* [x] Ordered workqueue
* [x] Asynchronous message processing
* [x] Periodic delayed work
* [x] Safe delayed-work cancellation
* [x] Separate `bin/` build output
* [ ] Automated integration tests
* [ ] CI build verification
* [ ] Device Tree and platform-driver example

## Learning Path

```text
kernel module
    ↓
misc character device
    ↓
open / read / write callbacks
    ↓
copy_to_user / copy_from_user
    ↓
mutex synchronization
    ↓
reader and writer wait queues
    ↓
blocking and non-blocking I/O
    ↓
poll / select / epoll readiness
    ↓
ioctl control interface
    ↓
typed kfifo message queue
    ↓
per-open file state
    ↓
ordered workqueue
    ↓
asynchronous kernel processing
    ↓
delayed work
    ↓
periodic kernel tasks
    ↓
safe asynchronous cancellation
```

## License

This project is intended for demonstration purposes.
