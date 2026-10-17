// SPDX-License-Identifier: GPL-3.0-or-later
/* Signal handling and terminal-cleanup tests. */
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
    raise(SIGTERM);
    if (!stop_requested)
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

    restore_terminal();
    if (termios_modified)
        return fail("termios_modified not cleared");
    if (tcgetattr(STDIN_FILENO, &after) != 0)
        return fail("tcgetattr restored");
    if (!(after.c_lflag & ECHO))
        return fail("ECHO not restored");
    if (after.c_lflag != before.c_lflag)
        return fail("c_lflag differs from original after restore");

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

    /*
     * SIGTSTP must actually stop the process, and SIGCONT must resume it
     * without treating the resume as a shutdown.
     */
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0) {
        stop_requested = 0;
        ASTORE(running, 1);
        if (install_signal_handlers() != 0)
            _exit(2);

        /*
         * Detach from the controlling terminal before suspending: on resume
         * the SIGCONT handler would re-disable echo on whatever stdin points
         * at, and the child would otherwise leave the shared terminal broken.
         */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        raise(SIGTSTP);
        restore_terminal();   /* belt and braces */
        if (stop_requested)
            _exit(3);
        _exit(0);
    }

    int status;
    if (waitpid(pid, &status, WUNTRACED) != pid)
        return fail("waitpid stop");
    if (!WIFSTOPPED(status))
        failures += fail("child did not stop on SIGTSTP");
    if (kill(pid, SIGCONT) != 0)
        return fail("kill SIGCONT");
    if (waitpid(pid, &status, 0) != pid)
        return fail("waitpid resume");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures += fail("child failed after resume");

    if (failures == 0) {
        printf("signal/terminal tests passed\n");
    } else {
        fprintf(stderr, "signals: %d test(s) failed\n", failures);
    }

    return failures ? 1 : 0;
}
