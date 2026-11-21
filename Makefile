#
# OS-9 RBF filesystem handler for AmigaOS (OS-9/68k and OS-9000 RBF),
# and host test tools
#
#   make            build the Amiga handler and the host tools
#   make handler    build objs/os9-rbf-handler (m68k-amigaos-gcc)
#   make host       build host/rbftool and host/rbfcheck
#   make test       run the host test suite
#

AMIGA_GCC  ?= /opt/amiga162rc25
AMIGA_BIN  := $(AMIGA_GCC)/bin
CC_AMIGA   := $(AMIGA_BIN)/m68k-amigaos-gcc
STRIP      := $(AMIGA_BIN)/m68k-amigaos-strip
LIBNIX     := $(AMIGA_GCC)/m68k-amigaos/libnix/lib

HOST_CC    ?= cc

# core9: filesystem API, cache, block mapping and the OS-9/68k backend
# core9k: the OS-9000 backend (big- and little-endian)
CORE9_SRCS := core9/fs_api.c core9/cache.c core9/blockmap.c \
              core9/rbf68k_vol.c core9/rbf68k_node.c core9/rbf68k_file.c \
              core9/rbf68k_dir.c
CORE9K_SRCS := core9k/rbf9k_vol.c core9k/rbf9k_node.c core9k/rbf9k_file.c \
              core9k/rbf9k_dir.c
CORE_SRCS  := $(CORE9_SRCS) $(CORE9K_SRCS)
CORE_HDRS  := core9/fs_api.h core9/blockdev.h core9/cache.h \
              core9/blockmap.h core9/rbf68k.h core9k/rbf9k.h

# start.c must be first: LoadSeg() enters the handler at its first byte
AMIGA_SRCS := amiga/start.c amiga/handler.c amiga/packets.c \
              amiga/volume.c amiga/amigadev.c amiga/support.c
AMIGA_HDRS := amiga/handler.h amiga/amigadev.h amiga/printf.h

OBJDIR     := objs
HANDLER    := $(OBJDIR)/os9-rbf-handler

CFLAGS_AMIGA := -m68000 -Os -Wall -Wextra -Wno-pointer-sign \
                -Wno-sign-compare -fomit-frame-pointer -fno-builtin-printf \
                -fno-toplevel-reorder -Icore9 -Icore9k -Iamiga
ifdef DEBUG
CFLAGS_AMIGA += -DDEBUG
AMIGA_SRCS   += amiga/printf.c     # serial debug output
endif
LDFLAGS_AMIGA := -nostdlib -nostartfiles -Wl,-Map=$(OBJDIR)/os9-rbf-handler.map

CFLAGS_HOST := -O2 -g -Wall -Wextra -Wno-sign-compare -Icore9 -Icore9k -Ihost

HOST_TOOLS := host/rbftool host/rbfcheck

all: handler host

handler: $(HANDLER)

host: $(HOST_TOOLS)

AMIGA_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(AMIGA_SRCS) $(CORE_SRCS))

$(OBJDIR)/%.o: %.c $(CORE_HDRS) $(AMIGA_HDRS)
	@mkdir -p $(dir $@)
	$(CC_AMIGA) $(CFLAGS_AMIGA) -c $< -o $@

$(HANDLER).debug: $(AMIGA_OBJS)
	$(CC_AMIGA) $(CFLAGS_AMIGA) $(LDFLAGS_AMIGA) -o $@ $(AMIGA_OBJS) \
		-lgcc $(LIBNIX)/libnix13.a
	@$(AMIGA_BIN)/m68k-amigaos-nm $@ | grep -q '^00000000 T _handler_entry$$' || \
		{ echo "error: handler_entry is not at the start of the hunk"; \
		  rm -f $@; exit 1; }

$(HANDLER): $(HANDLER).debug
	$(STRIP) -o $@ $<

host/rbftool: host/rbftool.c host/hostdev.c host/hostdev.h $(CORE_SRCS) \
              $(CORE_HDRS)
	$(HOST_CC) $(CFLAGS_HOST) -o $@ host/rbftool.c host/hostdev.c $(CORE_SRCS)

host/rbfcheck: host/rbfcheck.c host/rbfcheck9k.c
	$(HOST_CC) $(CFLAGS_HOST) -o $@ host/rbfcheck.c host/rbfcheck9k.c

test: host
	tests/run_tests.sh

clean:
	rm -rf objs objs.debug $(HOST_TOOLS) tests/work

.PHONY: all handler host test clean
