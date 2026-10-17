// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * evdev_thread() tests. A FIFO stands in for the input device: the thread's
 * capability ioctls fail harmlessly, but its poll/read loop and the
 * resulting paddle state are exercised for real.
 */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

#include <sys/stat.h>

static void send_event(int fd, unsigned short type, unsigned short code,
                       int value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    (void)!write(fd, &ev, sizeof(ev));
}

int main(void)
{
    int failures = 0;
    char path[] = "/tmp/cwkeyer-evdev-XXXXXX";
    int tmp = mkstemp(path);

    if (tmp < 0) {
        perror("evdev: mkstemp");
        return 1;
    }
    close(tmp);
    unlink(path);
    if (mkfifo(path, 0600) != 0) {
        perror("evdev: mkfifo");
        return 1;
    }
    int wfd = open(path, O_RDWR);
    if (wfd < 0) {
        perror("evdev: open fifo");
        unlink(path);
        return 1;
    }

    dit_code = KEY_LEFTCTRL;
    dah_code = KEY_RIGHTCTRL;
    do_grab = 0;                  /* the FIFO has no EVIOCGRAB */
    clear_input_state();
    ASTORE(running, 1);
    stop_requested = 0;

    /* Not a real evdev device, so keep the ioctl warnings off the console. */
    int saved_err = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, evdev_thread, path) != 0) {
        fprintf(stderr, "evdev: pthread_create failed\n");
        return 1;
    }

    struct timespec pause = {0, 50000000}; /* 50 ms */
    nanosleep(&pause, NULL);

    send_event(wfd, EV_KEY, KEY_LEFTCTRL, 1);
    nanosleep(&pause, NULL);
    int pressed = ALOAD(dit_paddle);
    int latched = ALOAD(dit_edge);

    send_event(wfd, EV_KEY, KEY_LEFTCTRL, 0);
    nanosleep(&pause, NULL);
    int released = (ALOAD(dit_paddle) == 0);

    ASTORE(running, 0);
    pthread_join(tid, NULL);

    dup2(saved_err, STDERR_FILENO);
    close(saved_err);
    close(wfd);
    unlink(path);

    if (!pressed) {
        fprintf(stderr, "evdev: press not observed\n");
        failures++;
    }
    if (!latched) {
        fprintf(stderr, "evdev: press edge not latched\n");
        failures++;
    }
    if (!released) {
        fprintf(stderr, "evdev: release not observed\n");
        failures++;
    }
    if (ALOAD(dit_paddle) != 0 || ALOAD(dit_edge) != 0) {
        fprintf(stderr, "evdev: state not cleared when the thread exits\n");
        failures++;
    }

    if (failures == 0)
        printf("evdev-thread tests passed\n");
    else
        fprintf(stderr, "evdev: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
