obj-m += jne_demo.o

KERNEL_DIR := /lib/modules/$(shell uname -r)/build
CURRENT_DIR := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURRENT_DIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURRENT_DIR) clean