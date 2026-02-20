CC      := gcc
CFLAGS  := -O2 -pipe -Wall -Wextra -Wno-unused-parameter -flto
LDFLAGS := -flto

PKGS := sdl2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libgpiod

SRC := \
  src/shaders.c \
  src/homography.c \
  src/app_state.c \
  src/gpio_helpers.c \
  src/playlist.c \
  src/video.c \
  src/video_engine.c \
  src/input_actions.c \
  src/main.c

OBJ := $(SRC:.c=.o)

TARGET := mapping_video_keystone

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ `pkg-config --libs $(PKGS)` -lGLESv2

src/%.o: src/%.c
	$(CC) $(CFLAGS) `pkg-config --cflags $(PKGS)` -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
