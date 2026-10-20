// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * macOS input backend tests.
 *
 * These drive hid_dispatch(), which is where every decision the backend makes
 * lives: which mode is active, whether a value is a press or a release, and
 * which device an event came from.  The IOKit glue around it (IOHIDManager,
 * element matching, the run loop) cannot be exercised without hardware, which
 * is exactly why the decisions were separated out.
 */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

static int failures;

static void reset_session(void)
{
    memset(&hid, 0, sizeof(hid));
    hid.mode = HID_MODE_IDLE;
    hid.only = SIZE_MAX;
    clear_input_state();
    dit_code = CWKEY_COMMA;
    dah_code = CWKEY_DOT;
}

static void expect(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "hid: %s\n", what);
        failures++;
    }
}

static void test_run_mode(void)
{
    int dit = (int)CWKEY_COMMA;
    int dah = (int)CWKEY_DOT;

    reset_session();
    hid.mode = HID_MODE_RUN;

    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dit, 1);
    expect(ALOAD(dit_paddle) == 1, "dit press did not set the paddle");
    expect(ALOAD(dit_edge) == 1, "dit press did not latch the edge");
    expect(ALOAD(dah_paddle) == 0, "dit press touched the dah paddle");

    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dit, 0);
    expect(ALOAD(dit_paddle) == 0, "dit release did not clear the paddle");

    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dah, 1);
    expect(ALOAD(dah_paddle) == 1, "dah press did not set the paddle");
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dah, 0);
    expect(ALOAD(dah_paddle) == 0, "dah release did not clear the paddle");

    /* Autorepeat does not exist in HID, but a stray 2 must not be a press. */
    clear_input_state();
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dit, 2);
    expect(ALOAD(dit_paddle) == 0 && ALOAD(dit_edge) == 0,
           "value 2 was treated as a press");

    /* Another key on the same keyboard is not a paddle. */
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardA, 1);
    expect(ALOAD(dit_paddle) == 0 && ALOAD(dah_paddle) == 0,
           "an unrelated key changed a paddle");

    /* Nor is a value from another usage page. */
    hid_dispatch(NULL, kHIDPage_Consumer, (unsigned int)dit, 1);
    expect(ALOAD(dit_paddle) == 0, "a consumer-page value changed a paddle");

    /* Press order is still stamped, so a squeeze starts with the first lever. */
    clear_input_state();
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dah, 1);
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)dit, 1);
    expect(ALOAD(dah_press_seq) != ALOAD(dit_press_seq),
           "press order was not stamped");
    expect(ALOAD(dah_press_seq) < ALOAD(dit_press_seq),
           "press order does not follow the event order");
}

static void test_wait_press(void)
{
    size_t which = SIZE_MAX;
    int code = -1;
    int dev_a;
    int dev_b;

    reset_session();
    hid.mode = HID_MODE_WAIT_PRESS;
    hid.ndev = 2;
    hid.dev[0] = (IOHIDDeviceRef)(void *)&dev_a;
    hid.dev[1] = (IOHIDDeviceRef)(void *)&dev_b;
    hid.index[0] = 4;
    hid.index[1] = 9;

    /* A release is not a tap. */
    hid_dispatch((void *)&dev_b, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 0);
    expect(!hid.fired, "a release satisfied the press wait");

    /* A press on the second device reports that device's caller index. */
    hid_dispatch((void *)&dev_b, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardPeriod, 1);
    expect(hid.fired, "a press did not satisfy the press wait");
    which = hid.which;
    code = hid.code;
    expect(which == 9, "the press wait reported the wrong device");
    expect(code == (int)kHIDUsage_KeyboardPeriod, "wrong key code reported");

    /* Later presses do not overwrite the first one. */
    hid_dispatch((void *)&dev_a, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardA, 1);
    expect(hid.which == 9 && hid.code == (int)kHIDUsage_KeyboardPeriod,
           "a later press overwrote the reported tap");

    /* `only` restricts which device may answer. */
    reset_session();
    hid.mode = HID_MODE_WAIT_PRESS;
    hid.ndev = 2;
    hid.dev[0] = (IOHIDDeviceRef)(void *)&dev_a;
    hid.dev[1] = (IOHIDDeviceRef)(void *)&dev_b;
    hid.index[0] = 4;
    hid.index[1] = 9;
    hid.only = 4;
    hid_dispatch((void *)&dev_b, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 1);
    expect(!hid.fired, "a device outside `only` answered the wait");
    hid_dispatch((void *)&dev_a, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 1);
    expect(hid.fired && hid.which == 4, "the selected device did not answer");

    /* An unknown sender is ignored rather than dereferenced. */
    reset_session();
    hid.mode = HID_MODE_WAIT_PRESS;
    hid.ndev = 1;
    hid.dev[0] = (IOHIDDeviceRef)(void *)&dev_a;
    hid.index[0] = 4;
    hid_dispatch((void *)&dev_b, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 1);
    expect(!hid.fired, "an unknown device answered the wait");
}

static void test_wait_release(void)
{
    int dev_a;

    reset_session();
    hid.mode = HID_MODE_WAIT_RELEASE;
    hid.ndev = 1;
    hid.dev[0] = (IOHIDDeviceRef)(void *)&dev_a;
    hid.index[0] = 2;
    hid.want_code = (int)kHIDUsage_KeyboardComma;

    /* A press is not the release we are waiting for. */
    hid_dispatch((void *)&dev_a, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 1);
    expect(!hid.fired, "a press satisfied the release wait");

    /* Nor is the release of a different key. */
    hid_dispatch((void *)&dev_a, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardPeriod, 0);
    expect(!hid.fired, "the wrong key's release satisfied the wait");

    hid_dispatch((void *)&dev_a, kHIDPage_KeyboardOrKeypad,
                 (unsigned int)kHIDUsage_KeyboardComma, 0);
    expect(hid.fired && hid.which == 2, "the release was not reported");
}

static void test_idle_mode(void)
{
    reset_session();
    hid.mode = HID_MODE_IDLE;

    /* A callback that arrives after its phase ended must be harmless. */
    hid_dispatch(NULL, kHIDPage_KeyboardOrKeypad, (unsigned int)CWKEY_COMMA, 1);
    expect(ALOAD(dit_paddle) == 0 && ALOAD(dit_edge) == 0,
           "an idle callback changed a paddle");
    expect(!hid.fired, "an idle callback satisfied a wait");
}

/* The device argument may stop at a field boundary, because the last field is
 * the USB port and changes when the adapter is moved. */
static void test_device_id_matching(void)
{
    const char *id = "04d8:01a6:00110000";

    expect(device_id_matches(id, id), "an exact id did not match");
    expect(device_id_matches(id, "04d8:01a6"), "vendor:product did not match");
    expect(device_id_matches(id, "04d8"), "a vendor alone did not match");
    expect(!device_id_matches(id, "04d8:01"), "a half field matched");
    expect(!device_id_matches(id, "04d8:01a6:001100000"),
           "a selector longer than the id matched");
    expect(!device_id_matches(id, "04d9"), "the wrong vendor matched");
    expect(!device_id_matches(id, "04d8:01a6:"), "a trailing colon matched");
    expect(!device_id_matches(id, ""), "an empty selector matched");
    expect(!device_id_matches("", "04d8"), "an empty id matched");
}

int main(void)
{
    test_device_id_matching();
    test_run_mode();
    test_wait_press();
    test_wait_release();
    test_idle_mode();

    if (failures == 0)
        printf("hid tests passed\n");
    else
        fprintf(stderr, "hid: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
