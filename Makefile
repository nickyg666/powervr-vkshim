# PowerVR BXM Vulkan compat shim (Minecraft Java on Orange Pi Zero 3W)
#
# all: builds the shim layer and the reproducer tests
# layer: builds just the layer   install: installs it (needs root)
# clean, test targets for the primtest suite

CC      ?= gcc
CFLAGS  ?= -O2 -g -fPIC -shared -Wall -rdynamic -DAS_LAYER
LDLIBS  ?= -ldl -lpthread
TESTCFLAGS ?= -O1
TESTLIBS   ?= -lvulkan

SHIM_SRCS  := vkimg_compat.c pvr_texelbuf.c pvr_lod.c pvr_copysec.c
SHIM_HDRS  := pvr_texelbuf.h pvr_spirv_descriptors.h pvr_copysec.h minimal_fs.h

.PHONY: all layer tests install clean

all: layer primtest primtest2 primtest3 primtest4 primtest5 formats offtest

layer: libVkLayer_powervr_compat.so

libVkLayer_powervr_compat.so: $(SHIM_SRCS) $(SHIM_HDRS)
	$(CC) $(CFLAGS) -o $@ $(SHIM_SRCS) $(LDLIBS)

%: %.c
	$(CC) $(TESTCFLAGS) -o $@ $< $(TESTLIBS)

install: libVkLayer_powervr_compat.so
	install -m 0755 libVkLayer_powervr_compat.so /usr/lib/
	@echo "layer installed; manifests live in manifests/"

uninstall:
	rm -f /usr/lib/libVkLayer_powervr_compat.so

clean:
	rm -f libVkLayer_powervr_compat.so primtest primtest2 primtest3 \
	      primtest4 primtest5 offtest formats
