# Linux Misc Character Device Lab

![Language](https://img.shields.io/badge/language-C-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Kernel Module](https://img.shields.io/badge/type-kernel%20module-orange)
![Status](https://img.shields.io/badge/status-learning%20project-green)

An educational Linux kernel module that implements a message-oriented miscellaneous character device:

```text
/dev/jne_demo
```

The project demonstrates communication between user space and kernel space through standard Linux file operations, blocking and non-blocking I/O, wait queues, polling, synchronization, `ioctl()` commands, and a typed kernel FIFO.

## Features

| Feature                 | Implementation                  |
| ----------------------- | ------------------------------- |
| Character device        | Linux `miscdevice` API          |
| Device path             | `/dev/jne_demo`                 |
| Message storage         | Typed `kfifo`                   |
| Queue capacity          | 16 messages                     |
| Maximum message size    | 255 bytes                       |
| Reading                 | `read()` callback               |
| Writing                 | `write()` callback              |
| User-to-kernel transfer | `copy_from_user()`              |
| Kernel-to-user transfer | `copy_to_user()`                |
| Synchronization         | Kernel `mutex`                  |
| Message counter         | `atomic_t`                      |
| Blocking reads          | Reader wait queue               |
| Blocking writes         | Writer wait queue               |
| Non-blocking I/O        | `O_NONBLOCK` and `-EAGAIN`      |
| Readiness notification  | `poll()`, `select()`, `epoll()` |
| Control commands        | `ioctl()`                       |
| Diagnostics             | `pr_info()` and `dmesg`         |

## Architecture

```text
┌─────────────────────────────────────────────┐
│                User space                   │
│                                             │
│  cat / echo / poll_read / ioctl_test        │
│  nonblock_read / nonblock_write             │
└──────────────────────┬──────────────────────┘
                       │
          open / read / write / poll / ioctl
                       │
┌──────────────────────▼──────────────────────┐
│                 Linux VFS                   │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│             jne_demo kernel module          │
│                                             │
│  ┌───────────────────────────────────────┐  │
│  │ Typed kfifo message queue             │  │
│  │ Capacity: 16 × jne_demo_message       │  │
│  └───────────────────────────────────────┘  │
│                                             │
│  mutex                                     │
│  atomic message counter                    │
│  reader wait queue                         │
│  writer wait queue                         │
└─────────────────────────────────────────────┘
```

## Message Queue

Each call to `write()` creates one independent message:

```c
struct jne_demo_message {
    size_t length;
    char data[BUFFER_SIZE];
};
```

The messages are stored in a typed kernel FIFO:

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

The maximum payload is therefore 255 bytes because the last byte is reserved for the terminating null character.

Messages are returned in FIFO order:

```text
write "Message one"
write "Message two"
write "Message three"

read → "Message one"
read → "Message two"
read → "Message three"
```

## Project Structure

```text
.
├── jne_demo.c           # Linux kernel module
├── jne_demo_ioctl.h     # Shared ioctl command definitions
├── nonblock_read.c      # Non-blocking read example
├── nonblock_write.c     # Non-blocking write example
├── poll_read.c          # poll()-based reader
├── ioctl_test.c         # ioctl command test
├── Makefile             # Kernel module build rules
├── .gitignore
└── README.md
```

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

Build the kernel module:

```bash
make
```

The build produces:

```text
jne_demo.ko
```

Remove generated files:

```bash
make clean
```

## Load the Module

```bash
sudo insmod ./jne_demo.ko
```

Verify that it is loaded:

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

## Kernel Log

Open a separate terminal:

```bash
sudo dmesg -w
```

Example messages:

```text
jne_demo: module loaded
jne_demo: device opened
jne_demo: waiting for a message
jne_demo: queued 12 bytes, 1 messages available
jne_demo: read 12 bytes, 0 messages remain
jne_demo: waiting for queue space
jne_demo: device closed
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

Each `write()` is treated as one message. Each successful `read()` consumes one complete message.

## Blocking Read

Start a reader while the queue is empty:

```bash
cat /dev/jne_demo
```

The process blocks without consuming CPU time.

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

In another terminal, read one message:

```bash
cat /dev/jne_demo
```

Expected output:

```text
Message 01
```

The blocked writer then wakes and adds `Message 17` to the queue.

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

Build the test program:

```bash
gcc -Wall -Wextra -O2 nonblock_read.c -o nonblock_read
```

Run it while the queue is empty:

```bash
./nonblock_read
```

Expected result:

```text
No data available
```

Write a message:

```bash
echo "Non-blocking read message" > /dev/jne_demo
```

Run the program again:

```bash
./nonblock_read
```

Expected result:

```text
Read 26 bytes: Non-blocking read message
```

The device is opened with:

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

Build the test program:

```bash
gcc -Wall -Wextra -O2 nonblock_write.c -o nonblock_write
```

When the queue has free space:

```bash
./nonblock_write
```

Expected result:

```text
Written 28 bytes
```

When all 16 queue positions are occupied:

```bash
./nonblock_write
```

Expected result:

```text
Queue is full
```

The device is opened with:

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

Read readiness means that at least one message is available:

```c
EPOLLIN | EPOLLRDNORM
```

Write readiness means that at least one queue position is free:

```c
EPOLLOUT | EPOLLWRNORM
```

### Test Program

Build:

```bash
gcc -Wall -Wextra -O2 poll_read.c -o poll_read
```

Run:

```bash
./poll_read
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
```

Available commands:

| Command                   | Description                         |
| ------------------------- | ----------------------------------- |
| `JNE_DEMO_IOC_CLEAR`      | Remove every message from the FIFO  |
| `JNE_DEMO_IOC_GET_LENGTH` | Return the size of the next message |

Build the test:

```bash
gcc -Wall -Wextra -O2 ioctl_test.c -o ioctl_test
```

Add a message:

```bash
echo "Message for ioctl test" > /dev/jne_demo
```

Run:

```bash
./ioctl_test
```

Example output:

```text
Buffer length: 23 bytes
Buffer cleared
Buffer length: 0 bytes
```

After `CLEAR`, blocked writers are woken because the queue has free space again.

## Data Transfer

The `write()` callback receives a user-space pointer:

```c
static ssize_t jne_demo_write(
    struct file *file,
    const char __user *buffer,
    size_t count,
    loff_t *offset)
```

Kernel code must not directly dereference user-space pointers.

User-to-kernel transfer:

```c
copy_from_user(
    message.data,
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

This prevents losing the message when copying to user space fails.

## Synchronization

The typed FIFO is protected by a kernel mutex:

```c
static DEFINE_MUTEX(message_queue_mutex);
```

The current number of messages is also available through an atomic counter:

```c
static atomic_t message_count = ATOMIC_INIT(0);
```

The mutex protects compound operations on the FIFO. The atomic counter provides a lightweight condition for wait queues and `poll()`.

Typical locking:

```c
if (mutex_lock_interruptible(&message_queue_mutex))
    return -ERESTARTSYS;

/* Access or modify the queue. */

mutex_unlock(&message_queue_mutex);
```

The code rechecks queue state after acquiring the mutex because another reader or writer may have changed the state between the initial test and lock acquisition.

## File Operations

The callbacks are connected through `struct file_operations`:

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

## Unload the Module

```bash
sudo rmmod jne_demo
```

Verify that the module is gone:

```bash
lsmod | grep jne_demo
```

Verify that the device node was removed:

```bash
ls -l /dev/jne_demo
```

## Typical Development Cycle

```bash
sudo rmmod jne_demo 2>/dev/null || true

make clean
make

sudo insmod ./jne_demo.ko
sudo dmesg | tail -n 30
```

After testing:

```bash
sudo rmmod jne_demo
```

## Safety

> [!WARNING]
> Linux kernel modules execute with full kernel privileges. A programming error may freeze the system or cause a kernel panic.

This module is loaded manually and is not installed into the system or configured for automatic startup.

Recommended precautions:

* save open files before loading experimental modules;
* do not add unfinished modules to automatic startup;
* do not copy experimental modules into `/lib/modules`;
* do not use forced module unloading;
* use a virtual machine for intentionally unsafe experiments.

## Planned Improvements

* [x] Basic miscellaneous character device
* [x] `read()` and `write()`
* [x] Safe user-space memory transfer
* [x] Mutex synchronization
* [x] Blocking reads
* [x] Non-blocking reads
* [x] `poll()` support
* [x] `ioctl()` commands
* [x] Typed message FIFO
* [x] Blocking writes
* [x] Non-blocking writes
* [ ] Per-open file state
* [ ] Kernel timer
* [ ] Workqueue-based processing
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
```

## License

This project is intended for educational and demonstration purposes.
