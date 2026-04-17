# Raspberry Pi Video Mapper

This repository contains a Raspberry Pi video mapper for Raspberry Pi OS Lite / Raspbian Lite.

The system has two main parts:
- `mapper/`: the core mapper engine (rendering, mapping, video playback)
- `patches/`: project-specific patches (`freund`, `omahen`)

The Raspberry Pi only needs to be set up once. After that, updates are normally done through a USB drive.

## 1. Requirements

Recommended environment:
- Raspberry Pi 5
- Raspberry Pi OS Lite
- default user: `pi`
- internet connection for the first installation

Typical default login:
- username: `pi`
- password: `raspberry`

Notes:
- Change the password after first login if needed.
- The first installation installs all required packages automatically.

## 2. First Installation on Raspberry Pi

Run this only once directly on the Raspberry Pi:

```bash
git clone https://github.com/MAZI2/raspberryPi-video-mapper.git
cd raspberryPi-video-mapper
chmod +x ./scripts/install_boot_mapper.sh
sudo ./scripts/install_boot_mapper.sh
```

What the installer does:
- installs all required dependencies
- creates a `systemd` service for boot startup
- creates a local project copy in `/opt/raspberryPi-video-mapper`
- creates a boot runner that checks the USB drive on startup, updates the code, and launches the mapper

Main service after installation:
- `mapper_boot_mapper.service`

## 3. Normal Workflow After Installation

After the first installation, the system is usually updated through a USB drive.

The USB drive should contain two main folders:
- one folder for videos
- one folder for the playback code

With the current default configuration, the folder names should be:
- video folder: `videos`
- code folder: `raspberryPi-video-mapper-main`

Expected USB structure:

```text
<USB_ROOT>/
  videos/
  raspberryPi-video-mapper-main/
    configure.conf
    mapper/
    patches/
    run.sh
    stop.sh
    scripts/
```

Important:
- the code folder name on the USB is separate from the folder name you get from `git clone`
- after `git clone`, the local folder is usually `raspberryPi-video-mapper`
- on the USB drive, with the current config, it should be renamed to `raspberryPi-video-mapper-main`
- if you want a different name, change it in `configure.conf`

## 4. Required Configuration

All main settings are controlled through:
- [configure.conf](configure.conf)

You normally edit this file directly on the USB drive inside the project folder.

Current example configuration:

```ini
start_on_boot=True
usb_label=INTENSO
mapper_folder_name=raspberryPi-video-mapper-main
videos_path=videos
project=freund
```

Configuration options:
- `start_on_boot=True/False`
  - decides whether playback starts automatically when Raspberry Pi boots
- `usb_label=<usb_label_name>`
  - should be set to the label of the USB drive so Raspberry Pi can reliably find it
- `mapper_folder_name=<code_folder_name>`
  - folder name on the USB drive that contains the project code
  - default: `raspberryPi-video-mapper-main`
- `videos_path=<videos_folder_name>`
  - folder name on the USB drive that contains videos
  - default: `videos`
- `project=freund` or `project=omahen`
  - selects which project patch will be applied

Important:
- `project` is not a patch file path
- the system automatically resolves the patch as `patches/<project>.patch`
- currently supported projects are:
  - `freund`
  - `omahen`

## 5. USB Label

`usb_label` must match the actual label of the USB drive.

You can check it on Raspberry Pi with:

```bash
lsblk -o NAME,LABEL,MOUNTPOINT
```

Example:

```text
sda1  INTENSO  /run/mapper-usb/sda1
```

In that case, `configure.conf` should contain:

```ini
usb_label=INTENSO
```

If the label is wrong, the boot service may not find the correct USB drive.

## 6. Updating Code and Videos Through USB

### Updating Videos

Update videos directly inside:
- `videos/`

### Updating Code

Update the code by replacing the whole folder on the USB drive:
- `raspberryPi-video-mapper-main`

Typical flow:
- download the new repository version from GitHub
- rename the folder to `raspberryPi-video-mapper-main` if needed
- replace the old project folder on the USB drive with the new one

Repository:
- https://github.com/MAZI2/raspberryPi-video-mapper

Important:
- the USB drive must contain the full project, not only `mapper/`
- at minimum it should contain:
  - `configure.conf`
  - `mapper/`
  - `patches/`

## 7. What Happens on Boot

When you insert the USB drive and reboot Raspberry Pi:
- the boot service finds the USB drive by its label
- it looks for the project folder `<mapper_folder_name>/mapper`
- it reads `configure.conf`
- it updates the local code copy if needed
- it applies the selected project patch
- it builds the project with `make -j`
- it launches `mapping_video_keystone`

The first boot after a larger update may take longer.

A realistic expectation for first boot after an update:
- up to several minutes for copying code and building
- in practice, up to about 5 minutes depending on the amount of changes and system speed

## 8. Important Note About Removing the USB Drive

With the current boot setup:
- the code is copied locally to Raspberry Pi
- the videos are still used from the USB drive during normal boot playback

That means:
- the USB drive should stay connected during normal operation
- removing the USB drive after boot is not recommended
- if you remove it, the next video, transition, or loop may fail

Note:
- `run.sh` caches videos locally for manual runs
- the normal boot service currently does not do the same for videos

## 9. Video Folder Structure by Project

### Project `freund`

Expected folders:

```text
/videos/freund/BACKGROUND
/videos/freund/LOOP
/videos/freund/TRANSITION
```

Rules:
- `BACKGROUND` contains 1 background video
- `LOOP` contains loop videos
- `TRANSITION` contains transition videos
- `LOOP` and `TRANSITION` must be numbered with prefixes like `1-`, `2-`, `3-`, ...
- transition videos and loop videos are paired by the same numeric prefix

Examples:
- `1-loop_Raznolikost_1.mov`
- `2-something_else.mov`
- `8-transition_Raznolikost_1.mov`

`freund` behavior:
- the background loops continuously in the back
- it starts with transition 1
- when the transition finishes, loop 1 starts
- on click, the next transition is triggered
- after that transition, the matching loop starts
- after the last step, it starts again from the beginning

Video note:
- foreground transition and loop videos may use `.mov` files with alpha
- the background can be a normal `.mp4`

### Project `omahen`

Expected folders:

```text
/videos/omahen/IDLE
/videos/omahen/TRANSITION
/videos/omahen/ANSWER
```

Rules:
- `IDLE` contains 1 idle video
- `TRANSITION` contains transition videos
- `ANSWER` contains answer videos
- `TRANSITION` and `ANSWER` do not need numbering, because selection is random

`omahen` behavior:
- the system starts in idle
- on click, a random transition is selected
- then a random answer is selected
- when the answer finishes, the system returns to idle
- during one full cycle, new clicks are ignored until the system is fully back in idle

## 10. Mapping Controls and GPIO Buttons

Current controls:
- `BTN3`: toggle edit mode ON/OFF
- `BTN1`: cycle selected corner when edit mode is ON
- `UP/DOWN/LEFT/RIGHT`: move the currently selected corner when edit mode is ON
- `BTN1`: when edit mode is OFF, trigger the project action
- `BTN2`: currently unused

GPIO pins use BCM numbering:
- `BTN1` -> `GPIO17` -> physical pin `11`
- `BTN2` -> `GPIO18` -> physical pin `12`
- `BTN3` -> `GPIO27` -> physical pin `13`
- `UP` -> `GPIO24` -> physical pin `18`
- `DOWN` -> `GPIO22` -> physical pin `15`
- `LEFT` -> `GPIO25` -> physical pin `22`
- `RIGHT` -> `GPIO23` -> physical pin `16`

Recommended button wiring:
- one side of the button to the GPIO pin
- the other side to GND
- do not connect GPIO input buttons to 5V

## 11. Manual Run and Stop

For manual testing from the repository:

```bash
./run.sh
```

This script:
- reads `configure.conf`
- finds the USB drive by label
- finds the video folder
- caches video content locally into `.cache/videos`
- applies the selected patch
- builds the mapper
- launches the mapper

To stop the boot service and the running mapper:

```bash
./stop.sh
```

Script location:
- [stop.sh](stop.sh)

## 12. If the Terminal Looks Broken After Stopping

Sometimes after a forced stop, the terminal may stay in a broken state.

Try:

```bash
reset
```

If that is not enough:

```bash
stty sane
tput sgr0
clear
```

Or all at once:

```bash
stty sane && tput sgr0 && reset
```

## 13. Useful Service Commands

Service status:

```bash
sudo systemctl status mapper_boot_mapper.service --no-pager -l
```

Current boot logs:

```bash
sudo journalctl -u mapper_boot_mapper.service -b --no-pager
```

Restart the service:

```bash
sudo systemctl restart mapper_boot_mapper.service
```

Check if the service is enabled:

```bash
sudo systemctl is-enabled mapper_boot_mapper.service
```

## 14. Common Problems

### USB drive is not found

Check:
- whether `usb_label` is correct
- whether the USB drive really has that label
- whether the correct project folder exists on the USB drive
- whether the correct video folder exists on the USB drive

Useful commands:

```bash
lsblk -o NAME,LABEL,MOUNTPOINT
sudo journalctl -u mapper_boot_mapper.service -b --no-pager
```

### Installer reports an `rsync change_dir` error

This usually means the installer was not started from the correct project checkout.

Correct usage:

```bash
cd ~/raspberryPi-video-mapper
sudo ./scripts/install_boot_mapper.sh
```

For USB updates, the full project should be inside:
- `raspberryPi-video-mapper-main`

### Nothing starts on boot

Check:
- `start_on_boot=True`
- service status
- service logs
- whether the selected project patch exists in `patches/`

### Mapper finds the USB drive but not the videos

Check:
- `videos_path=videos`
- whether the `videos/` folder exists
- whether the selected project structure is correct (`freund` or `omahen`)

### Updated code does not seem to apply

Check:
- that the entire project folder was replaced on the USB drive
- that its name matches `mapper_folder_name`
- that you reboot or restart the service after the update

## 15. Short Hand-Off Instructions

The Raspberry Pi only needs to be set up once:

```bash
git clone https://github.com/MAZI2/raspberryPi-video-mapper.git
cd raspberryPi-video-mapper
chmod +x ./scripts/install_boot_mapper.sh
sudo ./scripts/install_boot_mapper.sh
```

After that, use only the USB drive:
- video folder: `videos`
- code folder: `raspberryPi-video-mapper-main`

Required `configure.conf` settings on the USB drive:
- `start_on_boot=True` or `False`
- `usb_label=<usb_label>`
- `mapper_folder_name=raspberryPi-video-mapper-main`
- `videos_path=videos`
- `project=freund` or `project=omahen`

After updating code or videos:
- insert the USB drive into Raspberry Pi
- reboot Raspberry Pi
- wait for the build and startup to finish

For reliable operation, keep the USB drive connected.
