ifneq ($(KERNELRELEASE),)
obj-m := pebs_config.o

else
KDIR := /lib/modules/$(shell uname -r)/build

all:
		make -C ${KDIR} M=$(PWD) modules

clean:
		make -C ${KDIR} M=$(PWD) clean
endif
