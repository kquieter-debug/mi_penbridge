/*
 * penbridge - Lenovo Tab Pen Plus -> HyperOS stylus framework bridge
 *
 * Creates a persistent uinput virtual pen recognized by the MIUI stylus
 * stack (VID/PID 0x1915:0x4D81 -> InputDevice.isXiaomiStylus() == 2,
 * name "Redmi Smart Pen" so the Settings page name-based detection also
 * matches) and translates the physical pen's BLE HID button
 * (Consumer Control interface, /dev/input/eventN) into the two shortcut
 * keys the MIUI stylus shortcut manager listens for:
 *
 *   pen firmware reports the click pattern directly via MSC_SCAN:
 *     0x000c0600  single click  -> synthetic 450ms KEY_PAGEDOWN hold
 *                                  (>=380ms framework timer -> quick note)
 *     0x000c0601  double click  -> synthetic 450ms KEY_PAGEUP hold
 *                                  (>=380ms framework timer -> partial
 *                                   screenshot)
 *
 * The physical down/up events are consumed by the bridge; only the
 * synthesized press is emitted, so the framework's 380ms long-press
 * logic fires exactly once per click gesture.
 *
 * The virtual device stays registered for the lifetime of this process,
 * keeping the stylus settings page in "connected" state. All state is
 * kernel-side: exiting closes the uinput fd and removes the device, so
 * killing this daemon is a complete rollback.
 *
 * Build (NDK r28):  make
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define LOG_TAG "penbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* physical pen: Lenovo Tab Pen Plus, BLE HID */
#define PEN_VENDOR   0x17ef
#define PEN_PRODUCT  0x617f
#define PEN_NAME_TAG "Consumer Control"
#define KEY_UNKNOWN_LNX 240

/* click patterns reported by pen firmware via MSC_SCAN */
#define PEN_SCAN_SINGLE 0x000c0600
#define PEN_SCAN_DOUBLE 0x000c0601

/* framework triggers shortcuts at >=380ms hold; 450 leaves margin */
#define SYNTH_HOLD_MS 450

/* virtual pen: Redmi Smart Pen -> isXiaomiStylus() == 2 */
#define VIRT_VENDOR  0x1915
#define VIRT_PRODUCT 0x4d81
#define VIRT_NAME    "Redmi Smart Pen"

#define INPUT_DIR    "/dev/input"
#define UINPUT_NODE  "/dev/uinput"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig) { (void)sig; g_run = 0; }

static void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0)
        LOGE("uinput write failed (%d/%d/%d): %s", type, code, val, strerror(errno));
}

static int create_virtual_pen(void) {
    struct uinput_setup us;
    int fd = open(UINPUT_NODE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open %s failed: %s", UINPUT_NODE, strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    ioctl(fd, UI_SET_KEYBIT, KEY_PAGEDOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_PAGEUP);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2);

    memset(&us, 0, sizeof(us));
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%s", VIRT_NAME);
    us.id.bustype = BUS_BLUETOOTH;
    us.id.vendor  = VIRT_VENDOR;
    us.id.product = VIRT_PRODUCT;
    us.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        LOGE("uinput setup failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    LOGI("virtual pen \"%s\" (%04x:%04x) created", VIRT_NAME, VIRT_VENDOR, VIRT_PRODUCT);
    return fd;
}

/* open the pen's Consumer Control event node, -1 if not present */
static int find_and_open_pen(char *path_out, size_t outsz) {
    DIR *d = opendir(INPUT_DIR);
    struct dirent *e;
    int fd = -1;

    if (!d) {
        LOGE("opendir %s failed: %s", INPUT_DIR, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char path[128], name[128];
        struct input_id id;
        int evfd;

        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, e->d_name);
        evfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (evfd < 0)
            continue;
        memset(&id, 0, sizeof(id));
        memset(name, 0, sizeof(name));
        if (ioctl(evfd, EVIOCGID, &id) < 0 ||
            ioctl(evfd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            close(evfd);
            continue;
        }
        if (id.vendor == PEN_VENDOR && id.product == PEN_PRODUCT &&
            strstr(name, PEN_NAME_TAG) != NULL) {
            LOGI("pen button device found: %s (\"%s\")", path, name);
            snprintf(path_out, outsz, "%s", path);
            fd = evfd;
            break;
        }
        close(evfd);
    }
    closedir(d);
    return fd;
}

/* synthesize a >=380ms hold of key so the framework shortcut timer fires */
static void synth_press(int ufd, int key) {
    emit(ufd, EV_KEY, key, 1);
    emit(ufd, EV_SYN, SYN_REPORT, 0);
    usleep(SYNTH_HOLD_MS * 1000);
    emit(ufd, EV_KEY, key, 0);
    emit(ufd, EV_SYN, SYN_REPORT, 0);
}

/* returns 0 ok, -1 if the device is gone */
static int forward_events(int ufd, int penfd) {
    struct input_event ev[64];
    unsigned int last_scan = 0;
    ssize_t n;

    while (g_run) {
        n = read(penfd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return 0;
            LOGW("pen device read failed: %s", strerror(errno));
            return -1;  /* ENODEV etc: caller rescans */
        }
        if (n == 0)
            return -1;
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            if (ev[i].type == EV_MSC && ev[i].code == MSC_SCAN) {
                last_scan = (unsigned int)ev[i].value;
            } else if (ev[i].type == EV_KEY && ev[i].code == KEY_UNKNOWN_LNX &&
                       ev[i].value == 1) {
                if (last_scan == PEN_SCAN_SINGLE) {
                    LOGI("single click -> PAGE_DOWN hold (quick note)");
                    synth_press(ufd, KEY_PAGEDOWN);
                } else if (last_scan == PEN_SCAN_DOUBLE) {
                    LOGI("double click -> PAGE_UP hold (partial screenshot)");
                    synth_press(ufd, KEY_PAGEUP);
                }
                last_scan = 0;
            } else if (ev[i].type == EV_SYN) {
                last_scan = 0;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    struct pollfd pfds[2];
    char penpath[128] = "";
    int ufd, penfd = -1, ifd, wd = -1;

    if (argc > 1) {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    ufd = create_virtual_pen();
    if (ufd < 0)
        return 1;

    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        LOGE("inotify_init failed: %s", strerror(errno));
    } else if ((wd = inotify_add_watch(ifd, INPUT_DIR, IN_CREATE | IN_DELETE)) < 0) {
        LOGE("inotify_add_watch failed: %s", strerror(errno));
        close(ifd);
        ifd = -1;
    }

    LOGI("penbridge started: single click -> PAGE_DOWN (quick note), "
         "double click -> PAGE_UP (partial screenshot)");

    while (g_run) {
        if (penfd < 0) {
            penfd = find_and_open_pen(penpath, sizeof(penpath));
            if (penfd < 0 && ifd < 0) {
                /* no inotify to signal replug: poll slowly */
                struct timespec ts = { 2, 0 };
                nanosleep(&ts, NULL);
                continue;
            }
        }

        pfds[0].fd = ifd;
        pfds[0].events = POLLIN;
        pfds[1].fd = penfd;
        pfds[1].events = POLLIN;
        int r = poll(pfds, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOGE("poll failed: %s", strerror(errno));
            break;
        }

        if (penfd >= 0 && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            if ((pfds[1].revents & POLLIN) && forward_events(ufd, penfd) < 0) {
                LOGW("pen device lost, waiting for reconnection");
                close(penfd);
                penfd = -1;
            }
            if (penfd >= 0 && (pfds[1].revents & (POLLERR | POLLHUP))) {
                LOGW("pen device hung up, waiting for reconnection");
                close(penfd);
                penfd = -1;
            }
            if (penfd < 0)
                continue;   /* rescan immediately */
        }

        if (ifd >= 0 && (pfds[0].revents & POLLIN)) {
            char buf[1024];
            while (read(ifd, buf, sizeof(buf)) > 0)
                ;  /* drain; device-add/remove is handled by the rescan above */
        }
    }

    LOGI("penbridge exiting, removing virtual pen");
    if (penfd >= 0)
        close(penfd);
    if (wd >= 0)
        inotify_rm_watch(ifd, wd);
    if (ifd >= 0)
        close(ifd);
    if (ufd >= 0) {
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
    }
    return 0;
}
