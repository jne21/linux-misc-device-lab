MODULE_NAME := jne_demo

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)
BIN_DIR := $(PWD)/bin

USER_PROGRAMS := nonblock_read nonblock_write poll_read ioctl_test

obj-m := $(MODULE_NAME).o

.PHONY: all module userspace clean load unload reload

all: module userspace

module:
	mkdir -p $(BIN_DIR)
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	cp $(MODULE_NAME).ko $(BIN_DIR)/

userspace: $(addprefix $(BIN_DIR)/,$(USER_PROGRAMS))

$(BIN_DIR)/%: %.c jne_demo_ioctl.h
	mkdir -p $(BIN_DIR)
	$(CC) -Wall -Wextra -O2 $< -o $@

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf $(BIN_DIR)

load: module
	sudo insmod $(BIN_DIR)/$(MODULE_NAME).ko

unload:
	sudo rmmod $(MODULE_NAME)

reload:
	-sudo rmmod $(MODULE_NAME)
	$(MAKE) load
