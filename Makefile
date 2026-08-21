# Cross-platform Linux Makefile
# Works on glibc (Debian, Ubuntu, Fedora, Arch, openSUSE)
# and musl (Alpine Linux)

CC ?= gcc
CFLAGS ?= -O2

# Common flags for all Linux targets
COMMON_CFLAGS = -Iinclude -Ithird_party/mongoose \
	-std=c11 \
	-D_POSIX_C_SOURCE=200809L \
	-Wall -Wextra -Wpedantic

# _DEFAULT_SOURCE is needed for glibc systems to expose
# additional declarations (e.g. usleep, open with O_RDWR).
# On musl it is a no-op, so we add it unconditionally.
COMMON_CFLAGS += -D_DEFAULT_SOURCE

# Append user-supplied CFLAGS after our defaults
FINAL_CFLAGS = $(COMMON_CFLAGS) $(CFLAGS)

LDFLAGS ?=
FINAL_LDFLAGS = -pthread $(LDFLAGS)

BUILD_DIR = build

HTTP_SOURCES = \
	src/http_main.c \
	src/http_server.c \
	src/frame_stream.c \
	src/frame_queue.c \
	src/camera_v4l2.c \
	src/camera_worker.c

HTTP_OBJECTS = \
	$(BUILD_DIR)/src/http_main.o \
	$(BUILD_DIR)/src/http_server.o \
	$(BUILD_DIR)/src/frame_stream.o \
	$(BUILD_DIR)/src/frame_queue.o \
	$(BUILD_DIR)/src/camera_v4l2.o \
	$(BUILD_DIR)/src/camera_worker.o \
	$(BUILD_DIR)/third_party/mongoose/mongoose.o


.PHONY: all http clean info test

all: http

http: $(BUILD_DIR)/http_server

# Print build configuration (useful for troubleshooting across distros)
info:
	@echo "CC       = $(CC)"
	@echo "CFLAGS   = $(FINAL_CFLAGS)"
	@echo "LDFLAGS  = $(FINAL_LDFLAGS)"
	@echo "SOURCES  = $(HTTP_SOURCES)"
	@$(CC) --version | head -1


$(BUILD_DIR)/http_server: $(HTTP_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(FINAL_CFLAGS) $(HTTP_OBJECTS) -o $@ $(FINAL_LDFLAGS)


$(BUILD_DIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(FINAL_CFLAGS) -c $< -o $@


# Compile the vendored Mongoose library.
# Mongoose already includes <alloca.h> internally,
# so we do not need -include alloca.h here.
$(BUILD_DIR)/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(FINAL_CFLAGS) -c $< -o $@


clean:
	rm -rf $(BUILD_DIR)


# Multi-receiver integration test.
#
# Builds a throwaway server that uses a synthetic (V4L2-free) camera and
# opens several WebSocket receivers at once to confirm that multiple
# viewers can stream simultaneously and that a new viewer does not kick
# an existing one off. Requires gcc and python3.
test:
	python3 test/test_multiple_receivers.py
