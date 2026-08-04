# Linux Misc Character Device Lab

![Language](https://img.shields.io/badge/language-C-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Kernel Module](https://img.shields.io/badge/type-kernel%20module-orange)
![Status](https://img.shields.io/badge/status-learning%20project-green)

An educational Linux kernel module that implements a miscellaneous character device:

```text
/dev/jne_demo
```

The project demonstrates communication between user space and kernel space through standard Linux file operations.

## Features

| Feature                 | Implementation               |
| ----------------------- | ---------------------------- |
| Character device        | Linux `miscdevice` API       |
| Device path             | `/dev/jne_demo`              |
| Reading                 | `read()` callback            |
| Writing                 | `write()` callback           |
| User-to-kernel transfer | `copy_from_user()`           |
| Kernel-to-user transfer | `copy_to_user()`             |
| Synchronization         | Kernel `mutex`               |
| Blocking I/O            | Wait queue                   |
| Interrupted waiting     | `wait_event_interruptible()` |
| Non-blocking I/O        | `O_NONBLOCK` and `-EAGAIN`   |
| Device diagnostics      | `pr_info()` and `dmesg`      |

## Architecture

```text
┌─────────────────────────────┐
│      User-space program     │
│  cat, echo, nonblock_read   │
└──────────────┬──────────────┘
               │
        open / read / write
               │
┌──────────────▼──────────────┐
│      Linux VFS layer        │
└──────────────┬──────────────┘
               │
┌──────────────▼──────────────┐
│   jne_demo kernel module    │
│                             │
│  miscdevice                 │
│  shared kernel buffer       │
│  mutex                      │
│  wait queue                 │
└─────────────────────────────┘
```

## Project Structure

```text
.
├── jne_demo.c          # Linux kernel module
├── nonblock_read.c     # Non-blocking user-space test program
├── Makefile            # Kernel module build rules
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

Open a separate terminal and watch kernel messages:

```bash
sudo dmesg -w
```

The module logs events such as:

```text
jne_demo: module loaded
jne_demo: device opened
jne_demo: waiting for data
jne_demo: wrote 22 bytes
jne_demo: read 22 bytes
jne_demo: device closed
jne_demo: module unloaded
```

## Blocking Read

Start a reader:

```bash
cat /dev/jne_demo
```

The process blocks until data becomes available.

In another terminal, write a message:

```bash
echo "Hello from user space" > /dev/jne_demo
```

The blocked reader wakes up and prints:

```text
Hello from user space
```

A blocked read can be interrupted with `Ctrl+C`.

## Non-Blocking Read

Build the user-space test program:

```bash
gcc -Wall -Wextra -O2 nonblock_read.c -o nonblock_read
```

Run it when no data is available:

```bash
./nonblock_read
```

Expected result:

```text
No data available
```

Write new data:

```bash
echo "Non-blocking message" > /dev/jne_demo
```

Run the test again:

```bash
./nonblock_read
```

Expected result:

```text
Read 21 bytes: Non-blocking message
```

The test program opens the device with:

```c
fd = open("/dev/jne_demo", O_RDONLY | O_NONBLOCK);
```

When no data is available, the driver returns:

```c
return -EAGAIN;
```

In user space this becomes:

```c
read(...) == -1;
errno == EAGAIN;
```

## Data Transfer

The `write()` callback receives a user-space pointer:

```c
static ssize_t jne_demo_write(
    struct file *file,
    const char __user *buffer,
    size_t count,
    loff_t *offset)
```

User-space memory must not be accessed directly from kernel code.

Data is copied into the kernel buffer with:

```c
copy_from_user(device_buffer, buffer, bytes_to_copy);
```

Reading uses the opposite operation:

```c
copy_to_user(buffer, device_buffer, bytes_to_copy);
```

## Synchronization

The shared buffer is protected by a kernel mutex:

```c
static DEFINE_MUTEX(device_buffer_mutex);
```

A writer locks the buffer before changing it:

```c
if (mutex_lock_interruptible(&device_buffer_mutex))
    return -ERESTARTSYS;

/* Update the shared buffer. */

mutex_unlock(&device_buffer_mutex);
```

This prevents a reader from accessing the buffer while another process is modifying it.

## Wait Queue

The driver uses a wait queue for blocking reads:

```c
static DECLARE_WAIT_QUEUE_HEAD(data_wait_queue);
```

A reader waits until data becomes available:

```c
result = wait_event_interruptible(
    data_wait_queue,
    READ_ONCE(data_available));
```

After writing new data, the writer wakes waiting readers:

```c
WRITE_ONCE(data_available, true);
wake_up_interruptible(&data_wait_queue);
```

This is event-driven waiting. The blocked process sleeps and does not continuously consume CPU time.

## File Operations

The driver connects its callbacks through `struct file_operations`:

```c
static const struct file_operations jne_demo_fops = {
    .owner = THIS_MODULE,
    .open = jne_demo_open,
    .read = jne_demo_read,
    .write = jne_demo_write,
    .release = jne_demo_release,
};
```

The miscellaneous device is registered as:

```c
static struct miscdevice jne_demo_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "jne_demo",
    .fops = &jne_demo_fops,
    .mode = 0666,
};
```

`MISC_DYNAMIC_MINOR` allows the kernel to assign the minor device number automatically.

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
sudo dmesg | tail -n 20
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

* [ ] `poll()` support
* [ ] `select()` and `epoll()` compatibility
* [ ] `ioctl()` commands
* [ ] per-open file state
* [ ] multiple-message queue
* [ ] kernel timer
* [ ] workqueue-based processing
* [ ] automated test scripts
* [ ] CI build verification

## Learning Goals

This project covers the basic Linux driver-development path:

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
wait queues
    ↓
blocking and non-blocking I/O
```

## License

This project is intended for educational and demonstration purposes.
