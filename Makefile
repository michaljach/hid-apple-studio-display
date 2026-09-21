# Out-of-tree build for the running kernel:   make
# Against another kernel:                     make KVER=6.12.0-1-generic
# Install without DKMS (this kernel only):    make install / make uninstall
# Install with DKMS (rebuilt on updates):     make dkms-install / make dkms-remove
ifneq ($(KERNELRELEASE),)
obj-m := hid-apple-studio-display.o
else
MODNAME  := hid-apple-studio-display
VERSION  := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
KVER     ?= $(shell uname -r)
KDIR     ?= /lib/modules/$(KVER)/build
DKMS_SRC := /usr/src/$(MODNAME)-$(VERSION)

.PHONY: default clean install uninstall dkms-install dkms-remove

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: default
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a $(KVER)

uninstall:
	rm -f /lib/modules/$(KVER)/extra/$(MODNAME).ko*
	depmod -a $(KVER)

dkms-install:
	mkdir -p $(DKMS_SRC)
	cp $(MODNAME).c Makefile dkms.conf $(DKMS_SRC)/
	dkms install $(MODNAME)/$(VERSION)

dkms-remove:
	dkms remove $(MODNAME)/$(VERSION) --all
	rm -rf $(DKMS_SRC)
endif
