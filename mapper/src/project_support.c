#include "project_support.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* mapper_folder_name(void)
{
    const char* name = getenv("MAPPER_FOLDER_NAME");
    if (name && name[0]) {
        return name;
    }
    return "raspberryPi-video-mapper-main";
}

static const char* mapper_videos_path(void)
{
    const char* path = getenv("MAPPER_VIDEOS_PATH");
    if (path && path[0]) {
        return path;
    }
    return "videos";
}

const char* mapper_path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

int mapper_path_is_dir(const char* path)
{
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int mapper_find_videos_under_root(const char* root, char* out, size_t out_sz)
{
    char candidate[1024];
    const char* folder_name = mapper_folder_name();
    const char* videos_path = mapper_videos_path();

    snprintf(candidate, sizeof(candidate), "%s/%s", root, videos_path);
    if (mapper_path_is_dir(candidate)) {
        snprintf(out, out_sz, "%s", candidate);
        return 1;
    }

    snprintf(candidate, sizeof(candidate), "%s/%s/%s", root, folder_name, videos_path);
    if (mapper_path_is_dir(candidate)) {
        snprintf(out, out_sz, "%s", candidate);
        return 1;
    }

    return 0;
}

static int resolve_usb_label_device(const char* label, char* out_dev, size_t out_dev_sz)
{
    char by_label[1024];
    char resolved[PATH_MAX];

    if (!label || !label[0]) {
        return 0;
    }

    snprintf(by_label, sizeof(by_label), "/dev/disk/by-label/%s", label);
    if (!realpath(by_label, resolved)) {
        return 0;
    }

    snprintf(out_dev, out_dev_sz, "%s", resolved);
    return 1;
}

static int fs_source_matches_device(const char* fs_source, const char* wanted_dev)
{
    char resolved[PATH_MAX];

    if (!wanted_dev || !wanted_dev[0]) {
        return 1;
    }

    if (strcmp(fs_source, wanted_dev) == 0) {
        return 1;
    }

    if (realpath(fs_source, resolved) && strcmp(resolved, wanted_dev) == 0) {
        return 1;
    }

    return 0;
}

static int run_cmd_wait(char* const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return 0;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int find_mount_for_device(const char* wanted_dev, char* out_mount, size_t out_mount_sz)
{
    FILE* mounts = setmntent("/proc/mounts", "r");
    if (!mounts) {
        return 0;
    }

    int found = 0;
    struct mntent* ent;
    while ((ent = getmntent(mounts)) != NULL) {
        if (fs_source_matches_device(ent->mnt_fsname, wanted_dev)) {
            snprintf(out_mount, out_mount_sz, "%s", ent->mnt_dir);
            found = 1;
            break;
        }
    }

    endmntent(mounts);
    return found;
}

static int ensure_device_mounted(const char* wanted_dev, char* out_mount, size_t out_mount_sz)
{
    if (!wanted_dev || !wanted_dev[0]) {
        return 0;
    }

    if (find_mount_for_device(wanted_dev, out_mount, out_mount_sz)) {
        return 1;
    }

    if (mkdir("/run/mapper-usb", 0755) != 0 && errno != EEXIST) {
        return 0;
    }

    const char* base = strrchr(wanted_dev, '/');
    base = base ? (base + 1) : wanted_dev;

    char mount_point[1024];
    snprintf(mount_point, sizeof(mount_point), "/run/mapper-usb/%s", base);
    if (mkdir(mount_point, 0755) != 0 && errno != EEXIST) {
        return 0;
    }

    char* cmd1[] = { "mount", (char*)wanted_dev, mount_point, NULL };
    if (!run_cmd_wait(cmd1)) {
        char* cmd2[] = { "mount", "-o", "ro", (char*)wanted_dev, mount_point, NULL };
        if (!run_cmd_wait(cmd2)) {
            return 0;
        }
    }

    return find_mount_for_device(wanted_dev, out_mount, out_mount_sz);
}

int mapper_detect_media_root(char* out, size_t out_sz)
{
    const char* env_root = getenv("MAPPER_MEDIA_ROOT");
    const char* usb_label = getenv("MAPPER_USB_LABEL");
    char wanted_usb_dev[PATH_MAX];
    char mounted_dir[1024];

    wanted_usb_dev[0] = '\0';
    mounted_dir[0] = '\0';

    if (env_root && env_root[0] && mapper_path_is_dir(env_root)) {
        snprintf(out, out_sz, "%s", env_root);
        return 1;
    }

    if (usb_label && usb_label[0]) {
        if (resolve_usb_label_device(usb_label, wanted_usb_dev, sizeof(wanted_usb_dev))) {
            printf("[MEDIA] USB label filter: %s -> %s\n", usb_label, wanted_usb_dev);
            if (ensure_device_mounted(wanted_usb_dev, mounted_dir, sizeof(mounted_dir))) {
                printf("[MEDIA] USB mounted at: %s\n", mounted_dir);
                if (mapper_find_videos_under_root(mounted_dir, out, out_sz)) {
                    return 1;
                }
            } else {
                printf("[MEDIA] Failed to auto-mount %s (need root privileges?)\n", wanted_usb_dev);
            }
        } else {
            printf("[MEDIA] USB label '%s' is not currently resolvable\n", usb_label);
        }
    }

    FILE* mounts = setmntent("/proc/mounts", "r");
    if (mounts) {
        struct mntent* ent;
        while ((ent = getmntent(mounts)) != NULL) {
            if (wanted_usb_dev[0] && !fs_source_matches_device(ent->mnt_fsname, wanted_usb_dev)) {
                continue;
            }
            if (mapper_find_videos_under_root(ent->mnt_dir, out, out_sz)) {
                endmntent(mounts);
                return 1;
            }
        }
        endmntent(mounts);
    }

    const char* home = getenv("HOME");
    if (!home) {
        home = "/home/pi";
    }

    char candidate[1024];

    snprintf(candidate, sizeof(candidate), "%s", home);
    if (mapper_find_videos_under_root(candidate, out, out_sz)) {
        return 1;
    }

    snprintf(candidate, sizeof(candidate), "%s", "/opt/raspberryPi-video-mapper");
    if (mapper_find_videos_under_root(candidate, out, out_sz)) {
        return 1;
    }

    out[0] = '\0';
    return 0;
}

int mapper_load_playlist_from_subdir(Playlist* p, const char* media_root, const char* subdir)
{
    char full[1024];

    snprintf(full, sizeof(full), "%s/%s", media_root, subdir);
    return playlist_load_from_dir(p, full);
}
