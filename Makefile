obj-m += ouichefs.o
ouichefs-objs := fs.o super.o inode.o file.o dir.o sysfs.o

KERNELDIR ?= $(shell pwd)/../OS/linux-6.5.7
PWD := $(shell pwd)
SHARE_DIR := $(PWD)/../share/projet

all:
	make -C $(KERNELDIR) M=$(PWD) modules
	
debug:
	make -C $(KERNELDIR) M=$(PWD) ccflags-y+="-DDEBUG -g" modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean
	rm -rf *~

deploy: all
	@mkdir -p $(SHARE_DIR)/modules
	cp ouichefs.ko $(SHARE_DIR)/modules/
	@echo "========================================================"
	@echo " -> ouichefs.ko déployé dans $(SHARE_DIR)/modules/"
	@echo "========================================================"

.PHONY: all clean deploy
