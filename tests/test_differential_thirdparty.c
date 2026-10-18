// SPDX-License-Identifier: GPL-3.0-or-later
//
// Differential test: the ham-cwkeyer Curtis 8044 type B core against eight
// third-party iambic keyer variants (from seven upstream projects; dttsp is
// tested in two configurations).  This test has no
// external dependency (unlike test_differential_libcw): every third-party
// algorithm is transcribed here in C, line-faithful to its published source.
//
//   ours     ../../cwkeyer.c                                (type B latch)
//   vk6ph    recri/keyer dspmath/iambic_vk6ph.h              (GPL-3)
//   ad5dz    recri/keyer dspmath/iambic_ad5dz.h              (GPL-3)
//   dttsp    recri/keyer dspmath/iambic_dttsp.h              (GPL-3)
//   k1el     recri/keyer dspmath/iambic_k1el.h               (GPL-3)
//   nd7pa    recri/keyer dspmath/iambic_nd7pa.h              (GPL-3)
//   cwdojo   DavidClawson/cw-dojo keyer.py                   (MIT)
//   flipb    yo3gnd/morse-flipper src/firmware/keyer.c       (GPL-3)
//
// recri/keyer: https://github.com/recri/keyer
// cw-dojo:     https://github.com/DavidClawson/cw-dojo
// morse-flipper: https://github.com/yo3gnd/morse-flipper
//
// Every keyer is driven with a 1 ms tick.  The squeeze scenarios are
// *adaptive*: the release point is "the middle of the Nth element", found
// by observing each keyer's own keying, because keyers disagree on which
// element a squeeze starts with (dit-first vs dah-first).
//
// Run with no arguments for the regression check; pass
// `wpm seg_min seg_max trials quiet` to sweep speed and input timing.
// See docs/validation.md for the full write-up.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

/* 1 ms tick.  Defaults are 15 wpm: dit 80, dash 240, gap 80.  Both are
 * runtime globals so the harness can sweep keying speeds. */
static int DIT_T = 80;
static int DASH_T = 240;

/* From the morse-flipper keyer.h, needed by the flipb transcription. */
#define MORSE_KEYER_QUEUE_CAPACITY 8

#define IAMBIC_OFF 0
#define IAMBIC_DIT 3
#define IAMBIC_DAH 5

typedef int (*clock_fn)(void *self, int dit, int dah, int ticks);
typedef void *(*make_fn)(void);

struct impl {
    const char *name;
    clock_fn clock;
    make_fn make;
};

/* =====================================================================
 * 1. vk6ph -- Phil Harman VK6PH state machine (openHPSDR Hermes
 *    iambic.v, as ported to C by N1GP and again by recri/keyer).
 *    Source: recri/keyer dspmath/iambic_vk6ph.h
 *    https://github.com/recri/keyer
 * ===================================================================== */
struct vk6ph {
    int dot_memory, dash_memory;
    int key_state;
    int kdelay;
    int dot_delay, dash_delay;
    int mode; /* 0 straight, 1 A, 2 B */
    int keyer_out;
};
enum {
    VK_CHECK = 0, VK_PREDOT, VK_PREDASH, VK_SENDDOT, VK_SENDDASH,
    VK_DOTDELAY, VK_DASHDELAY, VK_DOTHELD, VK_DASHHELD
};

static int vk6ph_clock(void *p_, int raw_dit, int raw_dah, int ticks)
{
    struct vk6ph *k = p_;
    int kdot = raw_dit, kdash = raw_dah;

    switch (k->key_state) {
    default:
        break;
    case VK_CHECK:
        if (kdot)
            k->key_state = VK_PREDOT;
        else if (kdash)
            k->key_state = VK_PREDASH;
        else
            k->keyer_out = IAMBIC_OFF;
        break;
    case VK_PREDOT:
        k->key_state = VK_SENDDOT;
        k->dot_memory = k->dash_memory = 0;
        break;
    case VK_PREDASH:
        k->key_state = VK_SENDDASH;
        k->dot_memory = k->dash_memory = 0;
        break;
    case VK_SENDDOT:
        k->keyer_out = IAMBIC_DIT;
        if (k->kdelay >= k->dot_delay) {
            k->kdelay = 0;
            k->keyer_out = IAMBIC_OFF;
            k->key_state = VK_DOTDELAY;
        } else
            k->kdelay += ticks;
        if (!kdot && !kdash) {
            if (k->mode == 1)
                k->dash_memory = 0;
        } else
            k->dash_memory |= kdash;
        break;
    case VK_SENDDASH:
        k->keyer_out = IAMBIC_DAH;
        if (k->kdelay >= k->dash_delay) {
            k->kdelay = 0;
            k->keyer_out = IAMBIC_OFF;
            k->key_state = VK_DASHDELAY;
        } else
            k->kdelay += ticks;
        if (!kdot && !kdash) {
            if (k->mode == 1)
                k->dot_memory = 0;
        } else
            k->dot_memory |= kdot;
        break;
    case VK_DOTDELAY:
        if (k->kdelay >= k->dot_delay) {
            k->kdelay = 0;
            if (k->dash_memory)
                k->key_state = VK_PREDASH;
            else
                k->key_state = VK_DOTHELD;
        } else
            k->kdelay += ticks;
        k->dash_memory = kdash;
        break;
    case VK_DASHDELAY:
        if (k->kdelay >= k->dot_delay) {
            k->kdelay = 0;
            if (k->dot_memory)
                k->key_state = VK_PREDOT;
            else
                k->key_state = VK_DASHHELD;
        } else
            k->kdelay += ticks;
        k->dot_memory = kdot;
        break;
    case VK_DOTHELD:
        if (kdot)
            k->key_state = VK_PREDOT;
        else if (kdash)
            k->key_state = VK_PREDASH;
        else
            k->key_state = VK_CHECK;
        break;
    case VK_DASHHELD:
        if (kdash)
            k->key_state = VK_PREDASH;
        else if (kdot)
            k->key_state = VK_PREDOT;
        else
            k->key_state = VK_CHECK;
        break;
    }
    return k->keyer_out;
}

static void *vk6ph_make(void)
{
    struct vk6ph *k = calloc(1, sizeof(*k));
    k->mode = 2; /* B */
    k->dot_delay = DIT_T;
    k->dash_delay = DASH_T; /* 50% weight */
    k->keyer_out = IAMBIC_OFF;
    return k;
}

/* =====================================================================
 * 2. ad5dz -- reduced-logic keyer, modes A/B/S/G/U.
 *    Source: recri/keyer dspmath/iambic_ad5dz.h
 *    https://github.com/recri/keyer
 * ===================================================================== */
struct ad5dz {
    char mode;
    int ticks_per_dit, ticks_per_dah, ticks_per_ies;
    int key_out;
    int key;   /* 0 off, 1 dah, 2 dit, 3 both */
    int mem_key;
    int state; /* 0 off, 1 dit, 2 dit_space, 3 dah, 4 dah_space */
    int duration;
};

static int ad5dz_transition(struct ad5dz *k, int new_state, int dur)
{
    switch (new_state) {
    default:
        break;
    case 0:
        break;
    case 1:
        k->key_out = IAMBIC_DIT;
        k->mem_key = 0;
        break;
    case 3:
        k->key_out = IAMBIC_DAH;
        k->mem_key = 0;
        break;
    case 2:
    case 4:
        k->key_out = IAMBIC_OFF;
        if (k->key == 0 && k->mode == 'A')
            k->mem_key = 0;
        break;
    }
    k->state = new_state;
    k->duration += dur;
    return 1;
}

static int ad5dz_clock(void *p_, int raw_dit, int raw_dah, int ticks)
{
    struct ad5dz *k = p_;

    k->key = (raw_dit << 1) | raw_dah;
    k->mem_key |= k->key;

    if ((k->duration -= ticks) > 0)
        return k->key_out;

    switch (k->state) {
    default:
        break;
    case 0: /* OFF */
        k->duration = 0;
        k->mem_key = 0;
        if ((k->key | k->mem_key) & 2)
            ad5dz_transition(k, 1, k->ticks_per_dit);
        else if ((k->key | k->mem_key) & 1)
            ad5dz_transition(k, 3, k->ticks_per_dah);
        break;
    case 1: /* DIT */
        ad5dz_transition(k, 2, k->ticks_per_ies);
        break;
    case 3: /* DAH */
        ad5dz_transition(k, 4, k->ticks_per_ies);
        break;
    case 2: { /* DIT_SPACE */
        k->mem_key &= 1;
        if ((k->key | k->mem_key) & 1)
            ad5dz_transition(k, 3, k->ticks_per_dah);
        else if ((k->key | k->mem_key) & 2)
            ad5dz_transition(k, 1, k->ticks_per_dit);
        else
            ad5dz_transition(k, 0, 0);
        break;
    }
    case 4: { /* DAH_SPACE */
        k->mem_key &= 2;
        if ((k->key | k->mem_key) & 2)
            ad5dz_transition(k, 1, k->ticks_per_dit);
        else if ((k->key | k->mem_key) & 1)
            ad5dz_transition(k, 3, k->ticks_per_dah);
        else
            ad5dz_transition(k, 0, 0);
        break;
    }
    }
    return k->key_out;
}

static void *ad5dz_make(void)
{
    struct ad5dz *k = calloc(1, sizeof(*k));
    k->mode = 'B';
    k->ticks_per_dit = DIT_T;
    k->ticks_per_dah = DASH_T;
    k->ticks_per_ies = DIT_T;
    k->key_out = IAMBIC_OFF;
    return k;
}

/* =====================================================================
 * 3. dttsp -- CWirc-derived scheduler (Pierre-Philippe Coupard, as
 *    ported to C by DT4SP and again by recri/keyer).
 *    Source: recri/keyer dspmath/iambic_dttsp.h
 *    https://github.com/recri/keyer
 *    Configured for "full mode B": dit/dah memory on, mid-element
 *    squeeze required (need_midelemodeB), 50% weight, no auto spacing.
 *    (The C++ constructor leaves those parameters unset.)
 * ===================================================================== */
struct dttsp {
    int prev_dit, prev_dah;
    int altrn, psqam;
    int curr, iamb, last;
    int t_beep, t_dlay, t_elem, t_midl;
    int ditlen;
    int weight;
    int need_mid; /* need_midelemodeB: extra element only if the squeeze
                    persisted past mid-element before release */
};

static int dttsp_clock(void *p_, int dit, int dah, int ticklen)
{
    struct dttsp *k = p_;
    int set_which = -2; /* NO_TIME_LEFTS_SCHED */

    k->t_dlay -= k->t_dlay > 0 ? ticklen : 0;
    if (k->t_dlay <= 0) {
        k->t_midl -= k->t_midl > 0 ? ticklen : 0;
        k->t_beep -= k->t_beep > 0 ? ticklen : 0;
        k->t_elem -= k->t_elem > 0 ? ticklen : 0;
    }

    if (dit && dah) {
        k->iamb = 1; /* squeezed */
        if (k->t_midl <= 0)
            k->psqam = 1;
    } else if (!dit && !dah && k->iamb == 1)
        k->iamb = 2; /* released */

    if (k->t_elem <= 0 && k->curr != -1) {
        k->last = k->curr;
        if ((dit && dah) ||
            (k->altrn && k->iamb != 2) ||
            (k->iamb == 2 && (!k->need_mid || k->psqam))) {
            if (k->last == 1)
                set_which = k->curr = 0;
            else
                set_which = k->curr = 1;
        } else {
            k->curr = -1;
        }
        k->altrn = 0;
        k->iamb = 0;
        k->psqam = 0;
    }

    if (k->curr == -1) {
        if (dah)
            set_which = k->curr = 1; /* dah wins from idle */
        else if (dit)
            set_which = k->curr = 0;
    }

    if (k->curr == 0 && !k->prev_dah && dah)
        k->altrn = 1;
    if (k->curr == 1 && !k->prev_dit && dit)
        k->altrn = 1;

    if (k->t_dlay > 0 && !dit && !dah && k->curr != -1)
        set_which = k->curr = -1;

    switch (set_which) {
    default:
        break;
    case -1:
        k->t_beep = 0;
        k->t_midl = 0;
        k->t_elem = 0;
        break;
    case 0:
        k->t_beep = k->ditlen * k->weight / 50;
        k->t_midl = k->t_beep / 2;
        k->t_elem = k->ditlen * 2;
        break;
    case 1:
        k->t_beep = k->ditlen * k->weight / 50 + k->ditlen * 2;
        k->t_midl = k->t_beep / 2;
        k->t_elem = k->ditlen * 4;
        break;
    }

    k->prev_dit = dit;
    k->prev_dah = dah;

    if (k->t_beep > 0 && k->t_dlay <= 0)
        return k->curr == 0 ? IAMBIC_DIT : IAMBIC_DAH;
    return IAMBIC_OFF;
}

static void *dttsp_make(void)
{
    struct dttsp *k = calloc(1, sizeof(*k));
    k->curr = -1;
    k->last = -1;
    k->ditlen = DIT_T;
    k->weight = 50;
    k->need_mid = 1;
    return k;
}

static void *dttsp_nomid_make(void)
{
    struct dttsp *k = dttsp_make();
    ((struct dttsp *)k)->need_mid = 0;
    return k;
}

/* =====================================================================
 * 4. k1el -- DIT_L/DAH_L latch keyer (openqrp lineage; the same state
 *    machine ships in usdx, The-5-kiB, SimpleCWKeyer).
 *    Source: recri/keyer dspmath/iambic_k1el.h
 *    https://github.com/recri/keyer
 * ===================================================================== */
struct k1el {
    int key_out;
    int duration;
    int ticks_dit, ticks_dah, ticks_ies;
    unsigned char control; /* 1 DIT_L, 2 DAH_L, 4 DIT_PROC */
    int state;        /* 0 IDLE 1 CHK_DIT 2 CHK_DAH 3 PREP 4 KEYED 5 IE */
};

static void k1el_latch(struct k1el *k, int raw_dit, int raw_dah)
{
    if (raw_dit)
        k->control |= 1;
    if (raw_dah)
        k->control |= 2;
}

static int k1el_clock(void *p_, int raw_dit, int raw_dah, int ticks)
{
    struct k1el *k = p_;

    for (;;) {
        switch (k->state) {
        default:
            break;
        case 0: /* IDLE */
            if (raw_dit || raw_dah || (k->control & 3)) {
                k1el_latch(k, raw_dit, raw_dah);
                k->state = 1;
                continue;
            }
            return k->key_out;
        case 1: /* CHK_DIT */
            if (k->control & 1) {
                k->control |= 4;
                k->duration = k->ticks_dit;
                k->state = 3;
                k->key_out = IAMBIC_DIT;
                continue;
            }
            k->state = 2;
            continue;
        case 2: /* CHK_DAH */
            if (k->control & 2) {
                k->duration = k->ticks_dah;
                k->state = 3;
                k->key_out = IAMBIC_DAH;
                continue;
            }
            k->state = 0;
            return k->key_out;
        case 3: /* KEYED_PREP */
            k->control &= (unsigned char)~(1 + 2);
            k->state = 4;
            return k->key_out;
        case 4: /* KEYED */
            if ((k->duration -= ticks) > 0) {
                k1el_latch(k, raw_dit, raw_dah); /* mode B early latch */
                return k->key_out;
            }
            k->key_out = IAMBIC_OFF;
            k->duration = k->ticks_ies;
            k->state = 5;
            return k->key_out;
        case 5: /* INTER_ELEMENT */
            k1el_latch(k, raw_dit, raw_dah);
            if ((k->duration -= ticks) > 0)
                return k->key_out;
            if (k->control & 4) {
                k->control &= (unsigned char)~(1 + 4);
                k->state = 2;
                continue;
            }
            k->control &= (unsigned char)~2;
            k->state = 0;
            continue;
        }
    }
}

static void *k1el_make(void)
{
    struct k1el *k = calloc(1, sizeof(*k));
    k->ticks_dit = DIT_T;
    k->ticks_dah = DASH_T;
    k->ticks_ies = DIT_T;
    k->key_out = IAMBIC_OFF;
    return k;
}

/* =====================================================================
 * 5. nd7pa -- R. Traylor pending-memory keyer.
 *    Source: recri/keyer dspmath/iambic_nd7pa.h
 *    https://github.com/recri/keyer
 * ===================================================================== */
struct nd7pa {
    int state; /* 0 IDLE 1 DIT 2 DAH 3 DIT_DLY 4 DAH_DLY */
    int dit_pending, dah_pending;
    int timer;
    int ticks_dit, ticks_dah, ticks_ies;
};

static int nd7pa_clock(void *p_, int raw_dit, int raw_dah, int ticks)
{
    struct nd7pa *k = p_;
    int dit_on = raw_dit, dah_on = raw_dah;
    int key_out = IAMBIC_OFF;
    int expired;

    k->timer -= ticks;
    expired = k->timer <= 0;

    if (k->state == 0) {
        if (dit_on) {
            k->timer = k->ticks_dit;
            k->state = 1;
        } else if (dah_on) {
            k->timer = k->ticks_dah;
            k->state = 2;
        }
    } else if (k->state == 1) {
        key_out = IAMBIC_DIT;
        if (expired) {
            k->timer = k->ticks_ies;
            k->state = 3;
        }
    } else if (k->state == 2) {
        key_out = IAMBIC_DAH;
        if (expired) {
            k->timer = k->ticks_ies;
            k->state = 4;
        }
    } else if (k->state == 3) {
        if (expired) {
            if (k->dah_pending) {
                k->timer = k->ticks_dah;
                k->state = 2;
            } else
                k->state = 0;
        }
    } else if (k->state == 4) {
        if (expired) {
            if (k->dit_pending) {
                k->timer = k->ticks_dit;
                k->state = 1;
            } else
                k->state = 0;
        }
    }

    k->dit_pending = k->dit_pending ?
        k->state != 1 :
        (dit_on && ((k->state == 2 && k->timer < k->ticks_dah / 3) ||
                    (k->state == 4 && k->timer > k->ticks_ies / 2)));

    k->dah_pending = k->dah_pending ?
        k->state != 2 :
        (dah_on && ((k->state == 1 && k->timer < k->ticks_dit / 2) ||
                    (k->state == 3 && k->timer > k->ticks_ies / 2)));

    return key_out;
}

static void *nd7pa_make(void)
{
    struct nd7pa *k = calloc(1, sizeof(*k));
    k->ticks_dit = DIT_T;
    k->ticks_dah = DASH_T;
    k->ticks_ies = DIT_T;
    return k;
}

/* =====================================================================
 * 6. cwdojo -- IambicKeyer from DavidClawson/cw-dojo (Python).
 *    Type-B state machine: explicit per-element memory flags set on the
 *    paddle-down edge, consumed by the next gap.
 *    Source: DavidClawson/cw-dojo keyer.py (MIT)
 *    https://github.com/DavidClawson/cw-dojo
 * ===================================================================== */
struct cwdojo {
    int state; /* 0 IDLE 1 DIT 2 DAH 3 DIT_GAP 4 DAH_GAP */
    int dit_mem, dah_mem;
    int timer;
};

static int cwdojo_clock(void *p_, int d, int h, int ticks)
{
    struct cwdojo *m = p_;
    int out = IAMBIC_OFF;

    for (int i = 0; i < ticks; i++) {
        if (d) m->dit_mem = 1;
        if (h) m->dah_mem = 1;

        if (m->state == 0) {
            if (d) { m->state = 1; m->timer = DIT_T; }
            else if (h) { m->state = 2; m->timer = DASH_T; }
        } else if (m->state == 1) {
            out = IAMBIC_DIT;
            if (--m->timer <= 0) { m->state = 3; m->timer = DIT_T; }
        } else if (m->state == 2) {
            out = IAMBIC_DAH;
            if (--m->timer <= 0) { m->state = 4; m->timer = DIT_T; }
        } else if (m->state == 3) {
            if (--m->timer <= 0) {
                if (h || m->dah_mem) { m->state = 2; m->timer = DASH_T; }
                else if (d) { m->state = 1; m->timer = DIT_T; }
                else m->state = 0;
                m->dit_mem = m->dah_mem = 0;
            }
        } else {
            if (--m->timer <= 0) {
                if (d || m->dit_mem) { m->state = 1; m->timer = DIT_T; }
                else if (h) { m->state = 2; m->timer = DASH_T; }
                else m->state = 0;
                m->dit_mem = m->dah_mem = 0;
            }
        }
    }
    return out;
}

static void *cwdojo_make(void) { return calloc(1, sizeof(struct cwdojo)); }

/* =====================================================================
 * 7. flipb -- MorseKeyerModeIambicB from
 *    yo3gnd/morse-flipper (Flipper Zero, C).  A deduplicating set-queue
 *    latch: any paddle press is queued once, and at each element end the
 *    currently-down paddles are (re)queued before the next is shifted.
 *    Source: yo3gnd/morse-flipper src/firmware/keyer.c (GPL-3)
 *    https://github.com/yo3gnd/morse-flipper
 * ===================================================================== */
struct flipb {
    int paddle_down[2];
    int q[MORSE_KEYER_QUEUE_CAPACITY];
    int qlen;
    int pulse_active;
    int next_pulse_at;
    int cur_pulse; /* -1 none */
    int now;
};

static void flipb_qadd(struct flipb *k, int paddle)
{
    if (k->qlen >= MORSE_KEYER_QUEUE_CAPACITY)
        return;
    for (int i = 0; i < k->qlen; i++)
        if (k->q[i] == paddle)
            return;
    k->q[k->qlen++] = paddle;
}

static int flipb_qshift(struct flipb *k)
{
    int paddle;

    if (k->qlen == 0)
        return -1;
    paddle = k->q[0];
    k->qlen--;
    for (int i = 0; i < k->qlen; i++)
        k->q[i] = k->q[i + 1];
    return paddle;
}

static int flipb_first_down(const struct flipb *k)
{
    if (k->paddle_down[0]) return 0;
    if (k->paddle_down[1]) return 1;
    return -1;
}

static void flipb_tick(struct flipb *k, int now)
{
    if (!k->pulse_active || now < k->next_pulse_at)
        return;

    if (k->cur_pulse >= 0) {
        k->cur_pulse = -1;
        k->next_pulse_at = now + DIT_T;
        return;
    }

    for (int p = 0; p < 2; p++)
        if (k->paddle_down[p])
            flipb_qadd(k, p);

    int next = flipb_qshift(k);
    if (next < 0) {
        k->pulse_active = 0;
        k->next_pulse_at = 0;
        return;
    }
    k->cur_pulse = next;
    k->next_pulse_at = now + (next == 1 ? DASH_T : DIT_T);
}

static int flipb_clock(void *p_, int d, int h, int ticks)
{
    struct flipb *k = p_;
    int out = IAMBIC_OFF;
    int now[2] = { d, h };

    for (int i = 0; i < ticks; i++) {
        /* Reached the harness's fixed 1 ms tick. */
        for (int p = 0; p < 2; p++) {
            if (now[p] && !k->paddle_down[p]) {
                k->paddle_down[p] = 1;
                flipb_qadd(k, p);
                k->pulse_active = 1;
            } else if (!now[p]) {
                k->paddle_down[p] = 0;
            }
        }
        flipb_tick(k, k->now++);
        if (k->cur_pulse == 0)
            out = IAMBIC_DIT;
        else if (k->cur_pulse == 1)
            out = IAMBIC_DAH;
    }
    (void)flipb_first_down;
    return out;
}

static void *flipb_make(void)
{
    struct flipb *k = calloc(1, sizeof(*k));
    k->cur_pulse = -1;
    return k;
}

/* =====================================================================
 * Ours: drive the cwkeyer.c core at 1 ms ticks.
 * ===================================================================== */
struct ours {
    keyer_t keyer;
    int ticks_in_phase;
    int phase_len;
};

static void ours_reset(struct ours *m)
{
    memset(&m->keyer, 0, sizeof(m->keyer));
    m->ticks_in_phase = 0;
    m->phase_len = 0;
}

static int ours_clock(void *p_, int d, int h, int ticks)
{
    struct ours *m = p_;
    int out = IAMBIC_OFF;

    for (int i = 0; i < ticks; i++) {
        keyer_core_paddles(&m->keyer, d, h);
        if (m->keyer.state == KY_IN_DOT)
            out = IAMBIC_DIT;
        else if (m->keyer.state == KY_IN_DASH)
            out = IAMBIC_DAH;
        if (m->keyer.state == KY_IN_DOT)
            m->phase_len = DIT_T;
        else if (m->keyer.state == KY_IN_DASH)
            m->phase_len = DASH_T;
        else if (m->keyer.state == KY_AFTER_DOT ||
                 m->keyer.state == KY_AFTER_DASH)
            m->phase_len = DIT_T;
        if (m->phase_len > 0 && ++m->ticks_in_phase >= m->phase_len) {
            m->ticks_in_phase = 0;
            m->phase_len = 0;
            keyer_core_step(&m->keyer);
        }
    }
    return out;
}

/* =====================================================================
 * Harness
 * ===================================================================== */
static void append_run(char *seq, size_t seqlen, int run)
{
    size_t L = strlen(seq);
    if (L + 1 < seqlen) {
        seq[L] = run > DIT_T * 3 / 2 ? '-' : '.';
        seq[L + 1] = '\0';
    }
}

/* Levenshtein distance between two element strings, capped at 511 elements. */
static int seq_distance(const char *a, const char *b)
{
    static int prev[520], cur[520];
    size_t n = strlen(a), m = strlen(b);

    if (n > 511) n = 511;
    if (m > 511) m = 511;
    for (size_t j = 0; j <= m; j++)
        prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int best = del < ins ? del : ins;
            cur[j] = best < sub ? best : sub;
        }
        memcpy(prev, cur, (m + 1) * sizeof(int));
    }
    return prev[m];
}

/*
 * Scenarios.  The first three mirror the reference cases documented in
 * tests/test_keyer.c (Curtis 8044 app note / DJ5IL squeeze-keying notes);
 * the last two are true simultaneous squeezes released mid-element, with
 * the release point found adaptively ("middle of element N") because the
 * keyers disagree on which element a simultaneous squeeze starts with.
 * Paddle bits: (d << 1) | h.
 */
enum { SC_N, SC_A, SC_C, SC_S3, SC_S2, SC_END };

static int fixed_paddles(int scenario, int t)
{
    int d = 0, h = 0;

    switch (scenario) {
    default:
        break;
    case SC_N: /* dash, add dot at 60, release both at 160 (in dash 1) */
        h = t < 160;
        d = t >= 60 && t < 160;
        break;
    case SC_A: /* dot, add dash at 20, release both at 60 (in dit 1) */
        d = t < 60;
        h = t >= 20 && t < 60;
        break;
    case SC_C: /* dash, add dot at 60, release both at 650 (in dash 2) */
        h = t < 650;
        d = t >= 60 && t < 650;
        break;
    }
    return (d << 1) | h;
}

/* Adaptive simultaneous-squeeze script. */
struct sscript {
    int was_on;
    int elem_count;
    int release_at;
};

static int squeezer_paddles(struct sscript *s, int t)
{
    return s->release_at && t >= s->release_at ? 0 : 3;
}

static void squeezer_observe(struct sscript *s, int target, int t, int v)
{
    int on = v != IAMBIC_OFF;

    if (on && !s->was_on) {
        s->elem_count++;
        if (s->elem_count == target)
            s->release_at = t + (v == IAMBIC_DAH ? DASH_T / 2 : DIT_T / 2);
    }
    s->was_on = on;
}

static void run_scenario(int scenario, clock_fn clock, void *h, char *seq)
{
    struct sscript s;
    int run = 0;
    int total = scenario >= SC_S3 ? 3000 : 1200;

    memset(&s, 0, sizeof(s));
    seq[0] = '\0';
    for (int t = 0; t < total; t++) {
        int packed;
        int v;

        if (scenario == SC_S3 || scenario == SC_S2)
            packed = squeezer_paddles(&s, t);
        else
            packed = fixed_paddles(scenario, t);
        v = clock(h, packed >> 1, packed & 1, 1);

        if (v != IAMBIC_OFF)
            run++;
        else if (run) {
            append_run(seq, 512, run);
            run = 0;
        }
        if (scenario == SC_S3)
            squeezer_observe(&s, 3, t, v);
        else if (scenario == SC_S2)
            squeezer_observe(&s, 2, t, v);
    }
}

int main(int argc, char **argv)
{
    int explore = argc > 1;
    int wpm_arg = argc > 1 ? atoi(argv[1]) : 15;
    int seg_min = argc > 2 ? atoi(argv[2]) : 20;
    int seg_max = argc > 3 ? atoi(argv[3]) : 299;
    int trials = argc > 4 ? atoi(argv[4]) : 20000;
    int quiet = argc > 5 ? atoi(argv[5]) : (argc > 1 ? 0 : 1);
    int failed = 0;

    static const char *expect_sc[] = { "-.", ".-", "-.-.", ".-.-", ".-." };

    if (wpm_arg < 5 || wpm_arg > 60)
        wpm_arg = 15;
    DIT_T = 1200 / wpm_arg;
    DASH_T = DIT_T * 3;
    if (seg_min < 1) seg_min = 1;
    if (seg_max <= seg_min) seg_max = seg_min + 1;

    enum { N = 9 };
    struct impl impls[N] = {
        { "ours", 0, 0 },
        { "vk6ph", vk6ph_clock, vk6ph_make },
        { "ad5dz", ad5dz_clock, ad5dz_make },
        { "dttsp", dttsp_clock, dttsp_make },
        { "dttsp*", dttsp_clock, dttsp_nomid_make },
        { "k1el", k1el_clock, k1el_make },
        { "nd7pa", nd7pa_clock, nd7pa_make },
        { "cwdojo", cwdojo_clock, cwdojo_make },
        { "flipb", flipb_clock, flipb_make },
    };
    void *h[N];
    int runs_buf[N] = { 0 };
    static struct ours ours_state;
    char seqs[N][512];
    const char *names[SC_END] = {
        "N  dash, +dot@60, release@160      (8044 B: -.)",
        "A  dot,  +dash@20, release@60      (8044 B: .-)",
        "C  dash, +dot@60, release@650      (8044 B: -.-.)",
        "S3 squeeze from idle, rel mid-elem3 (B: extra element)",
        "S2 squeeze from idle, rel mid-elem2 (B: extra element)",
    };

    for (int si = 0; si < SC_END; si++) {
        for (int i = 0; i < N; i++)
            h[i] = i == 0 ? (void *)&ours_state : impls[i].make();
        if (!quiet)
            printf("%s:\n", names[si]);
        for (int i = 0; i < N; i++) {
            if (i == 0) {
                ours_reset(&ours_state);
                run_scenario(si, ours_clock, &ours_state, seqs[i]);
            } else
                run_scenario(si, impls[i].clock, h[i], seqs[i]);
        }
        if (!explore && si < (int)(sizeof(expect_sc) / sizeof(expect_sc[0])) &&
            strcmp(seqs[0], expect_sc[si]) != 0) {
            fprintf(stderr,
                    "reference case %d (%s): ours='%s', expected '%s'\n",
                    si, names[si], seqs[0], expect_sc[si]);
            failed = 1;
        }
        if (quiet)
            continue;
        for (int i = 0; i < N; i++)
            printf("  %-8s %s%s\n", impls[i].name, seqs[i],
                   strcmp(seqs[i], seqs[0]) ? "   << differs from ours" : "");
        printf("\n");
    }

    /* Fixed-schedule scenario: dit paddle held 0..200 -> two dits. */
    if (!quiet)
        printf("D  dit held 0..200 (expect ..):\n");
    for (int i = 0; i < N; i++)
        h[i] = i == 0 ? (void *)&ours_state : impls[i].make();
    for (int i = 0; i < N; i++) {
        int run = 0;
        seqs[i][0] = '\0';
        if (i == 0)
            ours_reset(&ours_state);
        for (int t = 0; t < 500; t++) {
            int d = t < 200, v = (i == 0)
                             ? ours_clock(&ours_state, d, 0, 1)
                             : impls[i].clock(h[i], d, 0, 1);
            if (v != IAMBIC_OFF)
                run++;
            else if (run) {
                append_run(seqs[i], 512, run);
                run = 0;
            }
        }
    }
    if (!explore && strcmp(seqs[0], "..") != 0) {
        fprintf(stderr, "reference case D: ours='%s', expected '..'\n",
                seqs[0]);
        failed = 1;
    }
    if (!quiet) {
        for (int i = 0; i < N; i++)
            printf("  %-8s %s%s\n", impls[i].name, seqs[i],
                   strcmp(seqs[i], seqs[0]) ? "   << differs from ours" : "");
        printf("\n");
    }

    /* Randomized schedules: same input to all, compare element strings. */
    printf("random: %d trials, seg %d..%d ms, wpm %d (dit %d ms)\n",
           trials, seg_min, seg_max, wpm_arg, DIT_T);
    unsigned state = 12345;
    size_t mismatch[N] = { 0 };
    size_t dist_sum[N] = { 0 };
    size_t elems[N] = { 0 };
    int shown = 0;
    for (int trial = 0; trial < trials; trial++) {
        struct seg { int dit, dah, until; } segs[24];
        int t = 0;
        size_t nseg = 0;
        int d = state & 1, hh = (state >> 1) & 1;
        state = state * 1103515245u + 12345u;
        while (t < 4000 && nseg < 24) {
            int dur = seg_min +
                      (int)(state % (unsigned)(seg_max - seg_min + 1));
            state = state * 1103515245u + 12345u;
            if (t + dur > 4000)
                dur = 4000 - t;
            segs[nseg].dit = d;
            segs[nseg].dah = hh;
            segs[nseg].until = t + dur;
            nseg++;
            t += dur;
            if ((state & 3) == 0) {
                d = (state >> 3) & 1;
                hh = (state >> 4) & 1;
            }
            state = state * 1103515245u + 12345u;
        }
        segs[nseg].dit = segs[nseg].dah = 0;
        segs[nseg].until = 4000;
        nseg++;

        for (int i = 0; i < N; i++) {
            h[i] = i == 0 ? (void *)&ours_state : impls[i].make();
            seqs[i][0] = '\0';
            runs_buf[i] = 0;
        }
        ours_reset(&ours_state);
        for (int x = 0; x < 4000; x++) {
            int dd = 0, hh2 = 0;
            for (size_t s2 = 0; s2 < nseg; s2++) {
                if (x < segs[s2].until) {
                    dd = segs[s2].dit;
                    hh2 = segs[s2].dah;
                    break;
                }
            }
            for (int i = 0; i < N; i++) {
                int v = (i == 0) ? ours_clock(&ours_state, dd, hh2, 1)
                                 : impls[i].clock(h[i], dd, hh2, 1);
                if (v != IAMBIC_OFF)
                    runs_buf[i]++;
                else if (runs_buf[i]) {
                    append_run(seqs[i], 512, runs_buf[i]);
                    runs_buf[i] = 0;
                }
            }
        }
        for (int i = 1; i < N; i++) {
            elems[i] += strlen(seqs[0]);
            if (strcmp(seqs[i], seqs[0])) {
                mismatch[i]++;
                dist_sum[i] += (size_t)seq_distance(seqs[0], seqs[i]);
                if (i == 5 && !quiet && shown < 5) {
                    char buf[256] = "";
                    size_t bl = 0;
                    for (size_t s2 = 0; s2 < nseg && bl < 220; s2++) {
                        bl += (size_t)snprintf(buf + bl, 256 - bl,
                                                "%sd%d h%d u%d",
                                                s2 ? " " : "",
                                                segs[s2].dit, segs[s2].dah,
                                                segs[s2].until);
                    }
                    printf("  k1el sample: in:%s\n    ours=%s k1el=%s\n",
                           buf, seqs[0], seqs[i]);
                    shown++;
                }
            }
        }
    }
    for (int i = 1; i < N; i++)
        printf("  %-8s bursts %.1f%%   edit-dist/elem %.2f%%   (mean %.1f elems per differing burst)\n",
               impls[i].name,
               100.0 * (double)mismatch[i] / (double)trials,
               elems[i] ? 100.0 * (double)dist_sum[i] / (double)elems[i] : 0.0,
               mismatch[i] ? (double)dist_sum[i] / (double)mismatch[i] : 0.0);

    /*
     * In test mode, lock the randomized comparison against the recorded
     * baseline.  Every number here moves if the core's element-boundary
     * behaviour changes, so a mismatch means the test needs re-baselining.
     * The values are for the canonical 15 wpm / 20..299 ms / 20000 trial run.
     */
    if (!explore) {
        static const size_t baseline[] = {
            0, 14390, 6613, 12941, 11937, 2099, 7291, 1796, 2259
        };
        for (int i = 1; i < N; i++)
            if (mismatch[i] != baseline[i]) {
                fprintf(stderr,
                        "%s diverges from ours in %zu trials; baseline is %zu\n",
                        impls[i].name, mismatch[i], baseline[i]);
                failed = 1;
            }
    }

    if (explore)
        return 0;
    if (failed) {
        fprintf(stderr, "third-party differential tests failed\n");
        return 1;
    }
    printf("third-party differential tests passed\n");
    return 0;
}

