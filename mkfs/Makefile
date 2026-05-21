BIN ?= mkfs.ouichefs
IMG ?= test.img
IMGSIZE ?= 50

SHARE_DIR ?= $(shell pwd)/../../share/projet

all: ${BIN}

${BIN}: mkfs-ouichefs.c
	gcc -Wall -o $@ $<

${BIN}-static: mkfs-ouichefs.c
	gcc -Wall -static -o $@ $<

img: ${BIN}
	rm -rf ${IMG}
	dd if=/dev/zero of=${IMG} bs=1M count=${IMGSIZE}
	./${BIN} ${IMG}
	sync

deploy: all img
	@mkdir -p ${SHARE_DIR}/bin
	@mkdir -p ${SHARE_DIR}/images
	cp ${BIN} ${SHARE_DIR}/bin/
	cp ${IMG} ${SHARE_DIR}/images/
	@echo "========================================================"
	@echo " -> ${BIN} déployé dans ${SHARE_DIR}/bin/"
	@echo " -> ${IMG} déployé dans ${SHARE_DIR}/images/"
	@echo "========================================================"

clean:
	rm -rf *~

mrproper: clean
	rm -rf ${BIN}

.PHONY: all clean mrproper img deploy
