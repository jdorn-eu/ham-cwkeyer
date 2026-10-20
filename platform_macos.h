// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * platform_macos.h - macOS key codes for the portable core.
 *
 * On macOS a key is a HID usage id on the Keyboard/Keypad usage page (0x07),
 * which is what IOHID reports and what the core stores in dit_code/dah_code.
 * The names are the same as on Linux, so the command line is unchanged.
 */
#ifndef CWKEYER_PLATFORM_MACOS_H
#define CWKEYER_PLATFORM_MACOS_H

#include <IOKit/hid/IOHIDUsageTables.h>

#define CWKEY_LEFTCTRL   kHIDUsage_KeyboardLeftControl
#define CWKEY_RIGHTCTRL  kHIDUsage_KeyboardRightControl
#define CWKEY_LEFTSHIFT  kHIDUsage_KeyboardLeftShift
#define CWKEY_RIGHTSHIFT kHIDUsage_KeyboardRightShift
#define CWKEY_LEFTALT    kHIDUsage_KeyboardLeftAlt
#define CWKEY_RIGHTALT   kHIDUsage_KeyboardRightAlt
#define CWKEY_COMMA      kHIDUsage_KeyboardComma
#define CWKEY_DOT        kHIDUsage_KeyboardPeriod
#define CWKEY_LEFTBRACE  kHIDUsage_KeyboardOpenBracket
#define CWKEY_RIGHTBRACE kHIDUsage_KeyboardCloseBracket
#define CWKEY_LEFT       kHIDUsage_KeyboardLeftArrow
#define CWKEY_RIGHT      kHIDUsage_KeyboardRightArrow
#define CWKEY_INSERT     kHIDUsage_KeyboardInsert
#define CWKEY_DELETE     kHIDUsage_KeyboardDeleteForward
#define CWKEY_Z          kHIDUsage_KeyboardZ
#define CWKEY_X          kHIDUsage_KeyboardX

/* Highest key code the command line accepts as a raw number: one HID byte. */
#define CWKEY_MAX        0xFF

#endif /* CWKEYER_PLATFORM_MACOS_H */
