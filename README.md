### Install Dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential pkg-config \
  libsdl2-dev \
  libgpiod-dev \
  gpiod \
  libgles2-mesa-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  libgstreamer1.0-dev \
  gstreamer1.0-libav
```

### Add videos to playlist

Add videos to `/raspberryPi-video-mapper/videos` in mp4 format.

### Build

From `/raspberryPi-video-mapper` run:

```bash
make -j
```

### Start

From `/raspberryPi-video-mapper` run:

```bash
SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone videos/vid1.mp4
```
