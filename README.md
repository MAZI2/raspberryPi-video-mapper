```bash
gcc test3.c -o mapping_video_keystone `pkg-config --cflags --libs sdl2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libgpiod` -lGLESv2
```

```bash
sudo systemctl stop getty@tty1

SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone videos/vid12.mp4 > log.txt 2>&1

tail -f log.txt
```
