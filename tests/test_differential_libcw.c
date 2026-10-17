// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Differential test: drive the real UnixCW libcw iambic keyer and our own
 * mode B core with the same random paddle schedules, and compare the element
 * sequences.
 *
 * UnixCW's default (curtis_mode_b = false) is, despite the name, the true
 * Accu-keyer/type B latch logic, so it is a useful independent reference for
 * our mode B core.
 *
 * UnixCW is GPL-2.0-or-later, which is compatible with this project's
 * GPL-3.0-or-later.
 *
 * Build and run with `make test-libcw`; needs a built unixcw checkout
 * (./configure && make). See the Makefile.
 */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

#include "libcw.h"
#include "libcw2.h"
#include "libcw_key.h"

#define OPEN   CW_KEY_VALUE_OPEN
#define CLOSED CW_KEY_VALUE_CLOSED

static unsigned rng_state = 987654321u;

static unsigned rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 16) & 0x7fff;
}

/* The mark libcw is currently sending, or 0 between elements. */
static char ref_symbol(const cw_key_t *key)
{
    switch (key->ik.graph_state) {
    case KS_IN_DOT_A:
    case KS_IN_DOT_B:
        return '.';
    case KS_IN_DASH_A:
    case KS_IN_DASH_B:
        return '-';
    default:
        return 0;
    }
}

int main(void)
{
    const int trials = 200000;
    cw_gen_config_t conf;
    cw_gen_t *gen;
    long mismatches = 0;

    memset(&conf, 0, sizeof(conf));
    conf.sound_system = CW_AUDIO_NULL;

    gen = cw_gen_new(&conf);
    if (!gen) {
        fprintf(stderr, "differential: cw_gen_new failed\n");
        return 2;
    }
    cw_gen_set_speed(gen, 20);

    for (int trial = 0; trial < trials; trial++) {
        cw_key_t *key = cw_key_new();
        keyer_t mine = {0};
        int d = 0, h = 0;
        int ref_d = -1, ref_h = -1;
        char ours[64] = "";
        char theirs[64] = "";
        int on = 0, tn = 0;

        if (!key) {
            fprintf(stderr, "differential: cw_key_new failed\n");
            cw_gen_delete(&gen);
            return 2;
        }

        cw_key_register_generator(key, gen);
        /* libcw's default, which is the true type B latch model. */
        cw_key_ik_disable_curtis_mode_b(key);

        for (int step = 0; step < 16; step++) {
            if (rng() & 1) {
                d = (int)(rng() & 1);
                h = (int)(rng() & 1);
            }

            keyer_core_paddles(&mine, d, h);      /* continuous sampling */
            if (d != ref_d || h != ref_h) {       /* libcw is event-driven */
                cw_key_ik_notify_paddle_event(key,
                                              d ? CLOSED : OPEN,
                                              h ? CLOSED : OPEN);
                ref_d = d;
                ref_h = h;
            }
            keyer_core_step(&mine);
            cw_key_ik_update_graph_state_internal(key);

            int msym = keyer_symbol(&mine);
            char rsym = ref_symbol(key);
            if (msym >= 0 && on < (int)sizeof(ours) - 1)
                ours[on++] = msym ? '.' : '-';
            if (rsym && tn < (int)sizeof(theirs) - 1)
                theirs[tn++] = rsym;
            ours[on] = '\0';
            theirs[tn] = '\0';

            int mine_idle = (mine.state == KY_IDLE);
            int ref_idle = (key->ik.graph_state == KS_IDLE);

            if ((msym >= 0) != (rsym != 0) || mine_idle != ref_idle ||
                strcmp(ours, theirs) != 0) {
                if (mismatches < 5) {
                    fprintf(stderr,
                            "trial %d step %d paddles %d/%d: "
                            "ours='%s' idle=%d, libcw='%s' idle=%d\n",
                            trial, step, d, h,
                            ours, mine_idle, theirs, ref_idle);
                }
                mismatches++;
                break;
            }
        }

        cw_key_delete(&key);
    }

    cw_gen_delete(&gen);

    printf("differential vs libcw: %d trials, %ld mismatches\n",
           trials, mismatches);
    return mismatches ? 1 : 0;
}
