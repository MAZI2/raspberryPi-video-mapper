```bash
sudo apt update
sudo apt install -y \
  build-essential pkg-config \
  libsdl2-dev \
  libgpiod-dev \
  libgles2-mesa-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav
```

```bash
make -j
```

```bash
gcc test3.c -o mapping_video_keystone `pkg-config --cflags --libs sdl2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libgpiod` -lGLESv2
```

```bash
sudo systemctl stop getty@tty1

SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone videos/vid12.mp4 > log.txt 2>&1

tail -f log.txt
```
