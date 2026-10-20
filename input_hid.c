// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * input_hid.c - macOS input backend.
 *
 * Included at the end of cwkeyer.c; see platform.h for the contract.
 *
 * macOS has no evdev, so there is no /dev/input node, no capability bitmap
 * and no SYN_DROPPED.  A paddle adapter is just a USB HID keyboard: the HRS
 * PaddleAdapter reports nothing but the eight modifier usages (0xE0..0xE7),
 * which is why LEFTCTRL/RIGHTCTRL work as paddles here exactly as they do on
 * Linux.  We therefore talk to IOKit's HID manager and treat key usages the
 * way evdev treats key codes.
 *
 * Differences from the Linux backend, all of them forced:
 *
 *   - Exclusive access is IOHIDDeviceOpen(kIOHIDOptionsTypeSeizeDevice).  It
 *     is only granted to root for keyboards, so a normal user gets the
 *     non-exclusive behaviour: the levers also reach the foreground app.  We
 *     try, warn, and carry on.
 *   - Reading keyboard HID values needs the Input Monitoring permission, and
 *     macOS attributes it to the program that launched us (the terminal).
 *   - A device that goes away is reported by a removal callback instead of
 *     EOF, and there is no queued state to resynchronise, because every HID
 *     value is absolute.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "platform.h"

/*
 * Core Foundation collections and IOKit disagree about constness: CF hands
 * out `const void *` and the HID API then wants the non-const type back.
 * Going through uintptr_t says "this is not a qualifier change I can express"
 * without tripping -Wcast-qual, which is on for the whole program.
 */
#define CF_UNCONST(type, p) ((type)(uintptr_t)(const void *)(p))

/* One manager for the whole process; discovery, setup, and the thread share
 * it, because IOHIDDeviceRef objects are only meaningful while it lives. */
static IOHIDManagerRef hid_manager;

static IOHIDManagerRef manager(void)
{
    if (hid_manager)
        return hid_manager;

    hid_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!hid_manager)
        return NULL;

    /* Match keyboard devices: generic desktop page, keyboard usage.  The HRS
     * adapter and Apple's internal keyboard both qualify. */
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (match) {
        int page = kHIDPage_GenericDesktop;
        int usage = kHIDUsage_GD_Keyboard;
        CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
        CFNumberRef un = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);

        if (pn && un) {
            CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsagePageKey), pn);
            CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsageKey), un);
            IOHIDManagerSetDeviceMatching(hid_manager, match);
        }
        if (pn)
            CFRelease(pn);
        if (un)
            CFRelease(un);
        CFRelease(match);
    }

    if (IOHIDManagerOpen(hid_manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        fprintf(stderr,
                "warning: cannot open the HID manager; grant this program "
                "(or the terminal running it) Input Monitoring permission in\n"
                "         System Settings > Privacy & Security > Input "
                "Monitoring.\n");
        CFRelease(hid_manager);
        hid_manager = NULL;
    }

    return hid_manager;
}

/* ---- device properties ---- */

static int dev_int(IOHIDDeviceRef dev, CFStringRef key, int fallback)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    int out = fallback;

    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        (void)CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &out);

    return out;
}

static void dev_str(IOHIDDeviceRef dev, CFStringRef key, char *out, size_t size)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);

    out[0] = '\0';
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        (void)CFStringGetCString((CFStringRef)v, out, (CFIndex)size,
                                 kCFStringEncodingUTF8);
}

/* The identity the core passes around: stable while the device stays on the
 * same port, and readable in --list-devices. */
static void device_id(IOHIDDeviceRef dev, char *out, size_t size)
{
    (void)snprintf(out, size, "%04x:%04x:%08x",
                   (unsigned)dev_int(dev, CFSTR(kIOHIDVendorIDKey), 0),
                   (unsigned)dev_int(dev, CFSTR(kIOHIDProductIDKey), 0),
                   (unsigned)dev_int(dev, CFSTR(kIOHIDLocationIDKey), 0));
}

/* Which keyboard usages the device can report. */
static void collect_usages(IOHIDDeviceRef dev, unsigned char usages[256])
{
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int page = kHIDPage_KeyboardOrKeypad;
    CFNumberRef pn;
    CFArrayRef elements;

    memset(usages, 0, 256);

    if (!m)
        return;
    pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    if (!pn) {
        CFRelease(m);
        return;
    }
    CFDictionarySetValue(m, CFSTR(kIOHIDElementUsagePageKey), pn);

    elements = IOHIDDeviceCopyMatchingElements(dev, m, 0);
    if (elements) {
        CFIndex n = CFArrayGetCount(elements);

        for (CFIndex i = 0; i < n; i++) {
            IOHIDElementRef el =
                CF_UNCONST(IOHIDElementRef, CFArrayGetValueAtIndex(elements, i));
            uint32_t usage;

            if (!el || IOHIDElementGetUsagePage(el) != (uint32_t)kHIDPage_KeyboardOrKeypad)
                continue;
            usage = IOHIDElementGetUsage(el);
            if (usage < 256)
                usages[usage] = 1;
        }
        CFRelease(elements);
    }

    CFRelease(pn);
    CFRelease(m);
}

static int has_usage(const unsigned char usages[256], int usage)
{
    return usage >= 0 && usage < 256 && usages[usage];
}

static int str_contains_ci(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);

    if (nlen == 0)
        return 1;

    for (const char *p = haystack; *p; p++) {
        size_t i = 0;

        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return 1;
    }

    return 0;
}

/*
 * Same policy as the evdev backend: a USB ID or a name that says what the
 * device is wins over the capability check, because the HRS adapter reports a
 * full keyboard keyset.  A device that can type is a keyboard; one that
 * reports both default paddle keys and nothing else is a paddle.
 */
static device_kind_t classify_hid(IOHIDDeviceRef dev, const char *name,
                                  unsigned short vendor, unsigned short product)
{
    unsigned char usages[256];
    int typing;

    if (known_paddle_label(vendor, product) != NULL)
        return DEVICE_KIND_PADDLE;

    if (name && (str_contains_ci(name, "keyer") || str_contains_ci(name, "paddle") ||
                 str_contains_ci(name, "vband") || str_contains_ci(name, "morse")))
        return DEVICE_KIND_PADDLE;

    if (name && (str_contains_ci(name, "keyboard") || str_contains_ci(name, "kbd")))
        return DEVICE_KIND_KEYBOARD;

    collect_usages(dev, usages);
    typing = has_usage(usages, kHIDUsage_KeyboardA) &&
             has_usage(usages, kHIDUsage_KeyboardSpacebar) &&
             has_usage(usages, kHIDUsage_KeyboardReturnOrEnter);
    if (typing)
        return DEVICE_KIND_KEYBOARD;

    if (has_usage(usages, CWKEY_LEFTCTRL) && has_usage(usages, CWKEY_RIGHTCTRL))
        return DEVICE_KIND_PADDLE;

    return DEVICE_KIND_UNKNOWN;
}

/* ---- discovery ---- */

static void run_loop_briefly(double seconds)
{
    (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
}

int input_backend_discover(input_device_t **out, size_t *out_count)
{
    IOHIDManagerRef mgr = manager();
    CFSetRef set;
    CFIndex n;
    input_device_t *devices;
    size_t count = 0;

    *out = NULL;
    *out_count = 0;

    if (!mgr)
        return 0;

    /*
     * The manager fills its device set from callbacks, so give the run loop a
     * moment before asking for the list.
     */
    run_loop_briefly(0.05);

    set = IOHIDManagerCopyDevices(mgr);
    if (!set)
        return 0;

    n = CFSetGetCount(set);
    devices = calloc(n > 0 ? (size_t)n : 1, sizeof(*devices));
    if (!devices) {
        CFRelease(set);
        fprintf(stderr, "out of memory listing input devices\n");
        return -1;
    }

    const void **refs = calloc(n > 0 ? (size_t)n : 1, sizeof(*refs));
    if (!refs) {
        free(devices);
        CFRelease(set);
        fprintf(stderr, "out of memory listing input devices\n");
        return -1;
    }
    CFSetGetValues(set, refs);

    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef dev = CF_UNCONST(IOHIDDeviceRef, refs[i]);
        input_device_t *d = &devices[count];
        unsigned char usages[256];

        if (!dev)
            continue;

        dev_str(dev, CFSTR(kIOHIDProductKey), d->name, sizeof(d->name));
        if (d->name[0] == '\0')
            (void)snprintf(d->name, sizeof(d->name), "(unnamed)");

        d->vendor = (unsigned short)dev_int(dev, CFSTR(kIOHIDVendorIDKey), 0);
        d->product = (unsigned short)dev_int(dev, CFSTR(kIOHIDProductIDKey), 0);
        d->order = dev_int(dev, CFSTR(kIOHIDLocationIDKey), 0);
        device_id(dev, d->id, sizeof(d->id));

        d->kind = classify_hid(dev, d->name, d->vendor, d->product);
        collect_usages(dev, usages);
        d->default_paddles = has_usage(usages, CWKEY_LEFTCTRL) &&
                             has_usage(usages, CWKEY_RIGHTCTRL);

        /* Holding the device keeps it valid for the caller. */
        d->handle = CF_UNCONST(void *, CFRetain(dev));
        d->readable = 1;
        count++;
    }

    free(CF_UNCONST(void *, refs));
    CFRelease(set);
    *out = devices;
    *out_count = count;
    return 0;
}

void input_backend_close_devices(input_device_t *devices, size_t count)
{
    if (!devices)
        return;

    for (size_t i = 0; i < count; i++) {
        if (devices[i].handle)
            CFRelease((CFTypeRef)devices[i].handle);
    }

    free(devices);
}

/* Nothing is queued: HID values are only delivered to a callback, and no
 * callback is registered outside a wait. */
void input_backend_drain(const input_device_t *devices, size_t count)
{
    (void)devices;
    (void)count;
}

/* ---- waiting for a tap (the shared setup wizard) ---- */

typedef struct {
    size_t index;        /* index of the device this callback belongs to */
    struct hid_wait *wait;
} watch_t;

typedef struct hid_wait {
    size_t only;         /* SIZE_MAX for any device, else the exact index */
    int want_release;    /* 0: next press, 1: release of this code */
    int want_code;
    int fired;
    size_t which;
    int code;
} hid_wait_t;

static void watch_cb(void *refcon, IOReturn result, void *sender, IOHIDValueRef value)
{
    watch_t *w = refcon;
    hid_wait_t *wait = w->wait;
    IOHIDElementRef el;
    uint32_t usage;
    CFIndex v;

    (void)sender;

    if (result != kIOReturnSuccess || wait->fired)
        return;
    if (wait->only != SIZE_MAX && wait->only != w->index)
        return;

    el = IOHIDValueGetElement(value);
    if (!el || IOHIDElementGetUsagePage(el) != (uint32_t)kHIDPage_KeyboardOrKeypad)
        return;

    usage = IOHIDElementGetUsage(el);
    v = IOHIDValueGetIntegerValue(value);

    if (wait->want_release) {
        if (v == 0 && (int)usage == wait->want_code) {
            wait->fired = 1;
            wait->which = w->index;
            wait->code = (int)usage;
        }
        return;
    }

    /* 1 = press.  HID reports no autorepeat; 2 does not occur. */
    if (v == 1) {
        wait->fired = 1;
        wait->which = w->index;
        wait->code = (int)usage;
    }
}

static int wait_for(const input_device_t *devices, size_t count, size_t only,
                    int want_release, int want_code, int timeout_ms,
                    size_t *which, int *code)
{
    watch_t *watches;
    hid_wait_t wait;
    CFRunLoopRef loop;
    size_t n = 0;
    int64_t deadline;
    int rc = -1;

    if (only != SIZE_MAX && only >= count)
        return -1;

    watches = calloc(count ? count : 1, sizeof(*watches));
    if (!watches)
        return -1;

    memset(&wait, 0, sizeof(wait));
    wait.only = only;
    wait.want_release = want_release;
    wait.want_code = want_code;
    wait.which = SIZE_MAX;

    loop = CFRunLoopGetCurrent();

    for (size_t i = 0; i < count; i++) {
        IOHIDDeviceRef dev;

        if (!devices[i].readable || (only != SIZE_MAX && i != only))
            continue;

        dev = (IOHIDDeviceRef)devices[i].handle;
        watches[n].index = i;
        watches[n].wait = &wait;

        if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess)
            continue;

        IOHIDDeviceRegisterInputValueCallback(dev, watch_cb, &watches[n]);
        IOHIDDeviceScheduleWithRunLoop(dev, loop, kCFRunLoopDefaultMode);
        n++;
    }

    if (n == 0) {
        free(watches);
        return -1;
    }

    deadline = mono_ns() + (int64_t)timeout_ms * 1000000LL;
    while (!wait.fired) {
        int64_t remaining = deadline - mono_ns();

        if (remaining <= 0) {
            rc = 0;
            break;
        }
        if (!keep_running()) {
            rc = 0;
            break;
        }
        run_loop_briefly(0.05);
    }

    if (wait.fired) {
        *which = wait.which;
        *code = wait.code;
        rc = 1;
    }

    for (size_t i = 0; i < n; i++) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)devices[watches[i].index].handle;

        IOHIDDeviceUnscheduleFromRunLoop(dev, loop, kCFRunLoopDefaultMode);
        IOHIDDeviceRegisterInputValueCallback(dev, NULL, NULL);
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    }

    free(watches);
    return rc;
}

int input_backend_wait_press(const input_device_t *devices, size_t count,
                             size_t only, int timeout_ms,
                             size_t *which, int *code)
{
    return wait_for(devices, count, only, 0, 0, timeout_ms, which, code);
}

int input_backend_wait_release(const input_device_t *device, int code,
                               int timeout_ms)
{
    size_t which = SIZE_MAX;
    int got = 0;

    if (!device)
        return -1;
    return wait_for(device, 1, 0, 1, code, timeout_ms, &which, &got);
}

/* ---- the running keyer ---- */

static int device_gone;

static void removal_cb(void *refcon, IOReturn result, void *sender)
{
    (void)result;
    (void)sender;
    (void)refcon;

    clear_input_state();
    device_gone = 1;
}

static void value_cb(void *refcon, IOReturn result, void *sender, IOHIDValueRef value)
{
    IOHIDElementRef el;
    CFIndex v;

    (void)refcon;
    (void)sender;

    if (result != kIOReturnSuccess)
        return;

    el = IOHIDValueGetElement(value);
    if (!el || IOHIDElementGetUsagePage(el) != (uint32_t)kHIDPage_KeyboardOrKeypad)
        return;

    v = IOHIDValueGetIntegerValue(value);
    if (v == 0 || v == 1)
        set_key_state((int)IOHIDElementGetUsage(el), (int)v);
}

/* A lever already held when we start: read the element's current value, the
 * way the evdev backend uses EVIOCGKEY. */
static void initialize_from_device(IOHIDDeviceRef dev)
{
    unsigned char usages[256];
    const int codes[2] = {dit_code, dah_code};

    collect_usages(dev, usages);

    for (int i = 0; i < 2; i++) {
        CFMutableDictionaryRef m;
        CFNumberRef pn, un;
        CFArrayRef elements;
        int page = kHIDPage_KeyboardOrKeypad;
        int usage = codes[i];
        int down = 0;

        if (usage < 0 || usage > 255 || !usages[usage]) {
            fprintf(stderr,
                    "warning: selected %s key %d does not appear to be "
                    "reported by this device\n",
                    i == 0 ? "dit" : "dah", usage);
            continue;
        }

        m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
        un = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
        if (m && pn && un) {
            CFDictionarySetValue(m, CFSTR(kIOHIDElementUsagePageKey), pn);
            CFDictionarySetValue(m, CFSTR(kIOHIDElementUsageKey), un);
            elements = IOHIDDeviceCopyMatchingElements(dev, m, 0);
            if (elements) {
                if (CFArrayGetCount(elements) > 0) {
                    IOHIDValueRef v = NULL;

                    if (IOHIDDeviceGetValue(
                            dev,
                            CF_UNCONST(IOHIDElementRef,
                                       CFArrayGetValueAtIndex(elements, 0)),
                            &v) == kIOReturnSuccess && v)
                        down = IOHIDValueGetIntegerValue(v) != 0;
                }
                CFRelease(elements);
            }
        }
        if (pn)
            CFRelease(pn);
        if (un)
            CFRelease(un);
        if (m)
            CFRelease(m);

        if (i == 0)
            ASTORE(dit_paddle, down);
        else
            ASTORE(dah_paddle, down);
    }
}

void *input_backend_thread(void *arg)
{
    const char *wanted = (const char *)arg;
    input_device_t *devices = NULL;
    size_t count = 0;
    IOHIDDeviceRef dev = NULL;
    CFRunLoopRef loop;
    int seized = 0;
    int fatal = 0;

    if (input_backend_discover(&devices, &count) < 0) {
        request_stop(1);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(devices[i].id, wanted ? wanted : "") == 0) {
            dev = (IOHIDDeviceRef)devices[i].handle;
            break;
        }
    }

    if (!dev) {
        fprintf(stderr, "no input device with id '%s' is attached\n",
                wanted ? wanted : "");
        fprintf(stderr, "run --list-devices to see what is available\n");
        input_backend_close_devices(devices, count);
        request_stop(1);
        return NULL;
    }

    /* Seizing a keyboard needs root; when it is refused, keep listening
     * non-exclusively so the levers still work. */
    if (do_grab) {
        if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice) == kIOReturnSuccess) {
            seized = 1;
        } else {
            fprintf(stderr,
                    "warning: cannot take exclusive control of this device "
                    "(needs root on macOS);\n"
                    "         paddle presses will also reach the foreground "
                    "application\n");
            if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
                fprintf(stderr, "cannot open input device '%s'\n", wanted);
                input_backend_close_devices(devices, count);
                request_stop(1);
                return NULL;
            }
        }
    } else if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        fprintf(stderr, "cannot open input device '%s'\n", wanted);
        input_backend_close_devices(devices, count);
        request_stop(1);
        return NULL;
    }

    initialize_from_device(dev);

    loop = CFRunLoopGetCurrent();
    IOHIDDeviceRegisterInputValueCallback(dev, value_cb, NULL);
    IOHIDDeviceRegisterRemovalCallback(dev, removal_cb, NULL);
    IOHIDDeviceScheduleWithRunLoop(dev, loop, kCFRunLoopDefaultMode);

    while (keep_running()) {
        if (device_gone) {
            fprintf(stderr, "input device '%s' disappeared\n", wanted);
            fatal = 1;
            break;
        }
        run_loop_briefly(0.1);
    }

    IOHIDDeviceUnscheduleFromRunLoop(dev, loop, kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputValueCallback(dev, NULL, NULL);
    IOHIDDeviceRegisterRemovalCallback(dev, NULL, NULL);
    IOHIDDeviceClose(dev, seized ? kIOHIDOptionsTypeSeizeDevice
                                 : kIOHIDOptionsTypeNone);

    clear_input_state();
    input_backend_close_devices(devices, count);

    if (fatal)
        request_stop(1);

    return NULL;
}

/* ---- signalling and help text ---- */

void input_backend_suspend(void)
{
    /* Nothing to release: an exclusive open ends with the process's handle,
     * and macOS has no separate grab to drop for a suspend. */
}

void input_backend_resume(void)
{
}

const char *input_backend_grab_description(void)
{
    return do_grab ? "exclusive access requested" : "nograb";
}

const char *input_backend_devices_noun(void)
{
    return "HID keyboard";
}

const char *input_backend_unreadable_hint(void)
{
    return "grant Input Monitoring permission in System Settings > "
           "Privacy & Security";
}

void input_backend_usage(FILE *out, usage_part_t part, const char *prog)
{
    switch (part) {
    case USAGE_DEVICE_ARG:
        fputs("<vendor:product:location>", out);
        break;

    case USAGE_EXAMPLES:
        fprintf(out,
            "  %s --setup                 learn the device by tapping\n"
            "  %s --list-devices          show HID keyboards and their ids\n"
            "  %s 04d8:01a6:01110000      HRS PaddleAdapter\n"
            "  %s 04d8:01a6:01110000 25 700 COMMA DOT\n"
            "  %s 0000:0000:000000a7 12 800 COMMA DOT nograb\n",
            prog, prog, prog, prog, prog);
        break;

    case USAGE_KEYNOTE:
        fputs("You may also use raw numeric HID usages on the keyboard page,\n"
              "e.g. 224 for left control.\n", out);
        break;

    case USAGE_DEVICE_OPTS:
        fputs("            List HID keyboards, grouped as unknown, keyboard,\n"
              "            or paddle, and exit. Colors are used only on a\n"
              "            terminal; set NO_COLOR to disable them.\n"
              "  --setup   Choose the paddle device by tapping the levers, then\n"
              "            start with the default speed, pitch, level, and\n"
              "            weighting. Known paddle adapters skip the tap test.\n"
              "            Terminal.app needs Input Monitoring permission to\n"
              "            see the levers; exclusive access needs root.\n",
              out);
        break;

    default:
        break;
    }
}
