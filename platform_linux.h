// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * platform_linux.h - Linux key codes for the portable core.
 *
 * The core and the command-line key table only ever name keys through
 * CWKEY_*, so another platform can supply the same names from its own
 * namespace without touching the core.  The numeric values stay opaque to
 * the core: the only requirement is that distinct names stay distinct.
 */
#ifndef CWKEYER_PLATFORM_LINUX_H
#define CWKEYER_PLATFORM_LINUX_H

#include <linux/input.h>

#define CWKEY_LEFTCTRL   KEY_LEFTCTRL
#define CWKEY_RIGHTCTRL  KEY_RIGHTCTRL
#define CWKEY_LEFTSHIFT  KEY_LEFTSHIFT
#define CWKEY_RIGHTSHIFT KEY_RIGHTSHIFT
#define CWKEY_LEFTALT    KEY_LEFTALT
#define CWKEY_RIGHTALT   KEY_RIGHTALT
#define CWKEY_COMMA      KEY_COMMA
#define CWKEY_DOT        KEY_DOT
#define CWKEY_LEFTBRACE  KEY_LEFTBRACE
#define CWKEY_RIGHTBRACE KEY_RIGHTBRACE
#define CWKEY_LEFT       KEY_LEFT
#define CWKEY_RIGHT      KEY_RIGHT
#define CWKEY_INSERT     KEY_INSERT
#define CWKEY_DELETE     KEY_DELETE
#define CWKEY_Z          KEY_Z
#define CWKEY_X          KEY_X

/* Highest key code the command line accepts as a raw number. */
#define CWKEY_MAX        KEY_MAX

#endif /* CWKEYER_PLATFORM_LINUX_H */
