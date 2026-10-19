// SPDX-License-Identifier: GPL-3.0-or-later
/* Signal handling and terminal-cleanup tests. */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

#include <sys/wait.h>

static int fail(const char *msg)
{
    fprintf(stderr, "signals: %s\n", msg);
    return 1;
}

int main(void)
{
    int failures = 0;

    if (install_signal_handlers() != 0)
        return fail("install_signal_handlers");

    /* SIGPIPE must be ignored, not fatal. */
    raise(SIGPIPE);
    if (!keep_running())
        failures += fail("SIGPIPE terminated the process");

    /* A stop signal sets the flag and stops the loops. */
    kill(getpid(), SIGTERM);
    for (int i = 0; i < 100 && keep_running(); i++) {
        struct timespec tick = {0, 1000000};
        nanosleep(&tick, NULL);
    }
    if (!ALOAD(stop_requested))
        failures += fail("SIGTERM did not set stop_requested");
    if (keep_running() != 0)
        failures += fail("SIGTERM did not stop keep_running()");

    /* Terminal disable/restore must be idempotent on a real tty. */
    stop_requested = 0;
    ASTORE(running, 1);

    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return fail("posix_openpt");
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0)
        return fail("grantpt/unlockpt");
    const char *slave_name = ptsname(mfd);
    if (!slave_name)
        return fail("ptsname");
    int sfd = open(slave_name, O_RDWR | O_NOCTTY);
    if (sfd < 0)
        return fail("open pty slave");

    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0 || dup2(sfd, STDIN_FILENO) < 0)
        return fail("redirect stdin");

    struct termios before, after;
    if (tcgetattr(STDIN_FILENO, &before) != 0)
        return fail("tcgetattr before");
    if (!(before.c_lflag & ECHO))
        return fail("pty did not start with ECHO set");

    disable_terminal_echo();
    if (!termios_modified)
        return fail("disable_terminal_echo did not take effect");

    /* A second disable must not re-save the already-disabled state. */
    disable_terminal_echo();
    if (tcgetattr(STDIN_FILENO, &after) != 0)
        return fail("tcgetattr disabled");
    if (after.c_lflag & ECHO)
        return fail("ECHO still enabled after disable");

    /* Input queued while the keyer owns the terminal must be discarded. */
    const char queued[] = "x\n";
    if (write(mfd, queued, sizeof queued - 1) !=
        (ssize_t)(sizeof queued - 1))
        return fail("write pty master");
    struct timespec settle = {0, 10000000}; /* 10 ms */
    nanosleep(&settle, NULL);

    restore_terminal();
    if (termios_modified)
        return fail("termios_modified not cleared");
    if (tcgetattr(STDIN_FILENO, &after) != 0)
        return fail("tcgetattr restored");
    if (!(after.c_lflag & ECHO))
        return fail("ECHO not restored");
    if (after.c_lflag != before.c_lflag)
        return fail("c_lflag differs from original after restore");

    int pty_flags = fcntl(sfd, F_GETFL);
    if (pty_flags < 0 || fcntl(sfd, F_SETFL, pty_flags | O_NONBLOCK) < 0)
        return fail("set pty nonblocking");
    char queued_input[8];
    ssize_t queued_len = read(sfd, queued_input, sizeof queued_input);
    if (queued_len > 0)
        failures += fail("tty input survived restore_terminal");
    else if (queued_len < 0 && errno != EAGAIN)
        failures += fail("read pty slave after restore");

    /* A SIGCONT that did not follow our SIGTSTP must not change the tty. */
    raise(SIGCONT);
    if (termios_modified)
        failures += fail("spurious SIGCONT re-enabled terminal changes");
    if (tcgetattr(STDIN_FILENO, &after) != 0)
        return fail("tcgetattr after SIGCONT");
    if (!(after.c_lflag & ECHO))
        failures += fail("spurious SIGCONT disabled echo");

    /* A full second cycle must restore the exact original flags. */
    disable_terminal_echo();
    restore_terminal();
    if (tcgetattr(STDIN_FILENO, &after) != 0)
        return fail("tcgetattr cycle2");
    if (after.c_lflag != before.c_lflag)
        return fail("second disable/restore cycle changed c_lflag");

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    close(sfd);
    close(mfd);

    /* Exercise stop, resume and final shutdown against a real pty. */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        return fail("second pty setup");
    const char *name = ptsname(master);
    if (!name)
        return fail("second ptsname");
    int slave = open(name, O_RDWR | O_NOCTTY);
    int ready[2];
    if (slave < 0 || pipe(ready) != 0)
        return fail("second pty/pipe open");

    pid_t pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0) {
        close(ready[0]);
        close(master);
        if (dup2(slave, STDIN_FILENO) < 0)
            _exit(2);
        close(slave);
        ASTORE(stop_requested, 0);
        ASTORE(running, 1);
        if (install_signal_handlers() != 0)
            _exit(2);
        disable_terminal_echo();
        if (write(ready[1], "x", 1) != 1)
            _exit(2);
        close(ready[1]);
        while (keep_running()) {
            struct timespec tick = {0, 1000000};
            nanosleep(&tick, NULL);
        }
        ASTORE(shutdown_wanted, 1);
        stop_signal_thread();
        restore_terminal();
        _exit(0);
    }
    close(ready[1]);
    char ready_byte;
    if (read(ready[0], &ready_byte, 1) != 1)
        return fail("child startup");
    close(ready[0]);
    if (tcgetattr(slave, &after) != 0 || (after.c_lflag & ECHO))
        failures += fail("child did not disable echo");

    if (kill(pid, SIGTSTP) != 0)
        return fail("kill SIGTSTP");
    int status;
    if (waitpid(pid, &status, WUNTRACED) != pid)
        return fail("waitpid stop");
    if (!WIFSTOPPED(status))
        failures += fail("child did not stop on SIGTSTP");
    if (tcgetattr(slave, &after) != 0 || !(after.c_lflag & ECHO))
        failures += fail("echo not restored while stopped");
    if (kill(pid, SIGCONT) != 0)
        return fail("kill SIGCONT");
    for (int i = 0; i < 100; i++) {
        struct timespec tick = {0, 1000000};
        if (tcgetattr(slave, &after) == 0 && !(after.c_lflag & ECHO))
            break;
        nanosleep(&tick, NULL);
    }
    if (after.c_lflag & ECHO)
        failures += fail("echo not disabled on resume");
    if (kill(pid, SIGTERM) != 0)
        return fail("kill SIGTERM");
    if (waitpid(pid, &status, 0) != pid)
        return fail("waitpid shutdown");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures += fail("child failed after resume");
    if (tcgetattr(slave, &after) != 0 || !(after.c_lflag & ECHO))
        failures += fail("echo not restored at exit");
    close(slave);
    close(master);

    stop_signal_thread();
    if (failures == 0) {
        printf("signal/terminal tests passed\n");
    } else {
        fprintf(stderr, "signals: %d test(s) failed\n", failures);
    }

    return failures ? 1 : 0;
}
