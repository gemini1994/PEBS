ifneq ($(KERNELRELEASE),)
obj-m := config.o

else
KDIR := /lib/modules/$(shell uname -r)/build

all:
		make -C ${KDIR} M=$(PWD) modules

clean:
		make -C ${KDIR} M=$(PWD) clean
endif
