CC = gcc
CFLAGS = -Wall

SHARE_DIR := $(shell pwd)/../../share/projet
TEST_DIR := $(SHARE_DIR)/test

PROGS = test_ioctl

all: $(PROGS)

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) -o test_ioctl test_ioctl.c

deploy: all
	@mkdir -p $(TEST_DIR)
	cp $(PROGS) $(TEST_DIR)/
	@echo "========================================================"
	@echo " -> programmes déployés dans $(TEST_DIR)/"
	@echo "========================================================"

clean:
	rm -rf $(PROGS) *~

.PHONY: all deploy clean