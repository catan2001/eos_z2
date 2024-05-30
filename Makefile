CC		= gcc
CFLAGS		= -g
RM		= rm -f
RUN 		= ./
RMMOD 		= rmmod
# If KERNELRELEASE is defined, we've been invoked from
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	obj-m := timer_driver.o
# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
# Ako KERNELDIR nije definisan postavi ga na ovu vrednost
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
# Podesi promenljivu PWD na vrednost trenutnog direktorijuma
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	$(CC) $(CFLAGS) -o alarm alarm.c
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	$(RM) *~
	$(RM) alarm
run:
	$(RUN)alarm
endif
