# Validation of the Curtis 8044 type B iambic core

This note explains why the keyer core in `cwkeyer.c` is believed to be a
faithful implementation of the Curtis/Accu-keyer "type B" rule, and how
that belief is tested.  It also records how the core compares with eight
third-party keyer variants from seven upstream projects.

`make test-differential` runs the differential test; `make test` and
`make test-libcw` cover the rest.

## 1. The claim

The core implements the **type B** iambic ("Accu-keyer") rule that John
Curtis named type B on the 8044ABM.  It is a deliberately narrow claim:
type A is not implemented, and this project does not claim type A.  The
one 8044ABM feature that is orthogonal to A/B, positive/negative
weighting, is available as an option (`CW_KEYER_WEIGHT_MS`) but defaults
to zero, i.e. strict ITU timing.

## 2. The primary source

The specification is Curtis Electro Devices, *8044 Series Keyer-on-a-Chip:
Application Note* (22 January 1992).  The passages that define the core's
behaviour are:

> **Type "B" iambic** — The procedure described above is typical of what
> we call iambic type "A" or the original Curtis method.  In type "B"
> iambic, a squeeze released during an element (dot or dash) will cause
> another alternate element to follow the one being produced.

> For example, if you squeeze to produce a "period", then you must
> release during the third dit (or the space following).  The last dah
> will automatically transmit.  For another example, to make a "C", you
> must release during the second dah (or the space following) and the
> last dit will automatically transmit.

> **Dot and dash memories** — Dot memory is absolutely essential …
> Of less importance in ordinary sending is the dash memory … For true
> iambic operation, both dot and dash memories are required for reliable
> dot or dash insertion.

The note also fixes the surrounding behaviour that the core models:

| 8044 property | app-note wording / value | where in `cwkeyer.c` |
|---|---|---|
| debounce RC on the paddles | "1 megohm and 0.01uF … about 10 ms" | `debounce_update()`, `CW_KEYER_DEBOUNCE_MS` default 10 |
| triggered clock (starts on paddle closure, not free-running) | "the code oscillator runs only when started by a paddle closure" | element begins the moment `keyer_core_paddles()` leaves `KY_IDLE` |
| element timing | "1:1 dot-space and 3:1 dash-space" | dit, 3×dit mark, one-dit space |
| no automatic character/word spacing | not provided on the 8044 | not provided here |
| weighting is a constant time, not a ratio | "stretches each dot and dash by a constant amount regardless of what speed is being used" | `keyer_space_ns()` + `weight_ns`, default 0 |
| separate manual-key input | the `MAN. KEY` pin | `straight` mode |

## 3. Type A vs type B, and why the code is structurally B

Karl Fischer (DJ5IL), *All About Squeeze-Keying*, gives the causal
distinction that a reader needs to tell the two apart:

> The behaviour of the Curtis-keyer can be described by the basic iambic
> set of instructions together with the following dot-memory rule: if
> anytime during generation of a dash-element the dot-lever changed its
> state from unpressed to pressed, generate an extra dot-element.
> […]
> The behaviour of the Accu-keyer can be described by … the following
> dot/dash-memory rule: if anytime during generation of an element the
> alternate lever was pressed, generate an extra alternate element.
> […]
> in type "A" (Curtis-keyer) it checks for the transient from unpressed
> to pressed whereas in type "B" (Accu-keyer) it checks just for the
> state pressed.

So the two rules differ in **what is remembered**: a type A keyer
remembers an *edge* (lever changed from open to closed during an
element); a type B keyer remembers the *level* (lever is closed during an
element).  The code is unambiguously a level memory:

```c
/* keyer_core_paddles(): set a latch whenever its lever is closed */
if (dot_down)  keyer->dot_latch  = 1;
if (dash_down) keyer->dash_latch = 1;

/* keyer_core_step(): clear it at the end of its element if the lever is open */
if (!keyer->dot_paddle)  keyer->dot_latch  = 0;   /* in KY_AFTER_DOT  */
if (!keyer->dash_paddle) keyer->dash_latch = 0;   /* in KY_AFTER_DASH */
```

Because a latch survives a release, releasing a squeeze mid-element leaves
the *opposite* latch set and one extra alternating element follows — the
defining type B behaviour.  Because only a *level* is needed, a transient
press that has already been released is still remembered.  Type A would
require putting an edge detector on the opposite lever and is a different
program; it is out of scope.

Chuck Olson (WB9KZY) states the same identification independently, and
gives the operational test used below:

> The original Curtis chip is mode A - the WB4VVF Accu-keyer is mode B.
> […] In mode A you could squeeze both paddles (dah before dit) and you
> would let go of both after hearing the last dit.  With mode B, you start
> the same BUT let go of both paddles after hearing the second *dah*.

DJ5IL's own hardware keyer, *Keyrama*, states the same distinction again
in implementation terms.  Its front-panel LEDs separate a
memory-triggered element from a directly-triggered one, and the manual
defines the memory like this:

> a MEM-LED lights up when the associated MEMory is set because that
> lever changed its state from "unpressed" to "pressed" (modes IA / U /
> S / E) or because it just was in the state "pressed" (mode IB)
> sometime during generation of an element.

The first half is the edge rule (type A); the second half is the level
rule (type B) — the same split, stated from the implementation side.  The
same manual bounds it: in the iambic modes "one single opposite element
can be stored", which is exactly what the two latches hold and no more.

One consequence Keyrama records is worth noting: contact bounce on
*opening* a lever corrupts type A, ultimatic and single-dot keying, but
not type B, because in type B the level memory already emits the extra
element the bounce would otherwise spuriously trigger.  This core
inherits that robustness; bounce on *closing* is the side the RC filter
exists to reject.

## 4. How it is tested

### 4.1 The core's test surface

The decision logic is small and pure: `keyer_core_paddles()` samples the
two levers, and `keyer_core_step()` advances the graph at the end of a
mark and again at the end of the inter-element space.  Both the
differential test and the libcw test drive exactly these two functions,
so they exercise the real decision logic, not a copy.

### 4.2 Reference scenarios

Six deterministic schedules encode the published behaviour.  `N`, `A` and
`C` are the "send it fast" tests from the app note and the WB9KZY/DJ5IL
articles; `S3` and `S2` are simultaneous squeezes released mid-element;
`D` is a held paddle.

| case | schedule | type B result |
|---|---|---|
| `N` | dash, dot added at 60 ms, both released at 160 ms (inside the dash) | `-.` |
| `A` | dot, dash added at 20 ms, both released at 60 ms (inside the dit) | `.-` |
| `C` | dash, dot added at 60 ms, both released at 650 ms (inside the 2nd dash) | `-.-.` |
| `S3` | squeeze from idle, release in the middle of the 3rd element | extra element |
| `S2` | squeeze from idle, release in the middle of the 2nd element | extra element |
| `D` | dit held 0–200 ms | `..` |

The core reproduces all six exactly; `make test-differential` asserts this.

### 4.3 Randomized schedules

The test then feeds identical pseudo-random paddle schedules (20 s of 1 ms
ticks, ~20 000 trials, fixed seed) to the core and to eight third-party
keyer variants, and compares the resulting element strings.  Two numbers
are reported per implementation: the fraction of trials whose element
string differs at all, and the Levenshtein distance per element, which
shows whether a divergence is one element or many.

The comparison implementations are line-faithful C transcriptions of:

| tag | source | licence |
|---|---|---|
| `vk6ph` | recri/keyer `dspmath/iambic_vk6ph.h` (VK6PH / openHPSDR Hermes) | GPL-3 |
| `ad5dz` | recri/keyer `dspmath/iambic_ad5dz.h` | GPL-3 |
| `dttsp`, `dttsp*` | recri/keyer `dspmath/iambic_dttsp.h` (DT4SP / CWirc scheduler) | GPL-3 |
| `k1el` | recri/keyer `dspmath/iambic_k1el.h` (the k1el state machine also shipped in usdx, The-5-kiB and SimpleCWKeyer) | GPL-3 |
| `nd7pa` | recri/keyer `dspmath/iambic_nd7pa.h` | GPL-3 |
| `cwdojo` | DavidClawson/cw-dojo `keyer.py` | MIT |
| `flipb` | yo3gnd/morse-flipper `src/firmware/keyer.c` | GPL-3 |

The transcriptions live in `tests/test_differential_thirdparty.c`, so the
test has no external checkout to build.

### 4.4 Differential test against UnixCW libcw

`make test-libcw` (needs a built unixcw) drives libcw's iambic graph and
this core with the same 200 000 random paddle schedules and compares the
element sequences.  libcw is a mature, independent implementation of the
same model.

## 5. Results

### 5.1 Reference scenarios

The core matches all six exactly.  Of the others, `ad5dz`, `dttsp`,
`dttsp*`, `k1el`, `cwdojo` and `flipb` match on `N`, `A` and `C`; `vk6ph`
and `nd7pa` do not (see below).

### 5.2 libcw

```
differential vs libcw: 200000 trials, 0 mismatches
```

### 5.3 The eight third-party variants vs this core

Canonical run (`make test-differential`): 15 wpm, 20 000 trials, random
element-length input.

| tag | differing bursts | element error rate | ≈1 element error per |
|---|---|---|---|
| `cwdojo` | 9.0% | 0.80% | ~125 elements |
| `k1el` | 10.5% | 0.91% | ~110 elements |
| `flipb` | 11.3% | 1.03% | ~97 elements |
| `ad5dz` | 33.1% | 3.63% | ~28 elements |
| `nd7pa` | 36.5% | 4.02% | ~25 elements |
| `dttsp*` | 59.7% | 5.29% | ~19 elements |
| `dttsp` | 64.7% | 6.10% | ~16 elements |
| `vk6ph` | 72.0% | 7.84% | ~13 elements |

The element error rate is a harsher measure than a human comparison: the
random schedules deliberately include very short (20–40 ms) taps that
stress the memory windows far beyond normal keying.

With human-realistic input timings (50–300 ms holds/taps) at 12–20 wpm,
the three closest implementations converge on the core essentially
exactly: `cwdojo`, `flipb` and `k1el` all fall to **0.1–0.4 % element
error** (roughly one element in 250–700), while `ad5dz` drops to 0.7–0.8 %
and the others barely move.  The residual divergence for the closest three
is a single element at a sub-element tap/release boundary — never a
different policy.

## 6. What the divergences are

They fall into three families:

1. **Policy differences — real and operator-noticeable.**
   - `vk6ph`: its "mode B" does not implement the extra-element rule.  Its
     memory is only (re)written from the live paddles during the
     inter-element delay, so a tap during an element is lost (`C` → `-.-`,
     `N` → `-`).  This does not improve with slower speed.
   - `dttsp` / `dttsp*`: different extra-element condition and dash-first
     priority on a simultaneous squeeze.
   - `nd7pa`: recognises the opposite paddle only inside a sub-window of
     the element (last third of a dash, first half of a dit); taps outside
     are dropped.

2. **Boundary race — visible only at very fast taps.**
   - `ad5dz`: reproduces every reference case, but its latch-clear timing
     differs at the element boundary, so very short taps can land on the
     wrong side.  It improves sharply with realistic timing.

3. **Sub-element race — effectively identical in practice.**
   - `cwdojo`, `flipb`, `k1el`: the same level-latch model.  They differ
     only at the exact sample where a tap or release falls relative to the
     element edge, by one element, and rarely.

In every case the difference is this core *having* the documented type B
behaviour (extra element on squeeze release, mid-element tap registration)
where the other omits it, or a boundary timing choice — never this core
exhibiting behaviour the type B model lacks.

## 7. What is and is not proven

**Proven against the published specification:** the type B decision rule
(extra element on squeeze release, dot and dash level memory, held-paddle
repeat, mid-element tap registration), and the surrounding timing model
(RC debounce, triggered clock, 1:1:3 ratio, constant-time weighting, no
automatic spacing).

**Corroborated by independent software:** identical element sequences to
UnixCW libcw over 200 000 random schedules, and to the three independent
level-memory implementations on all documented cases.

**Not provable from documents:** sample-level equivalence to the physical
8044ABM die.  The reference cases fix the policy, not the behaviour at a
single internal clock tick; that corner is under-determined by the
available sources.  What can be said is that the core agrees with the
published rule and with a mature independent implementation of it.

## 8. Reproducing

```
make test-differential                                  # canonical run
make test-differential ARGS="20 50 300 5000 1"          # 20 wpm, human timings
./difftest <wpm> <seg_min> <seg_max> <trials> <quiet>   # explore directly
```

References:

1. Curtis Electro Devices, Inc. (John G. Curtis, K6KU), *8044 Series
   Keyer-on-a-Chip: Application Note*, 22 January 1992.
2. Karl Fischer (DJ5IL), *All About Squeeze-Keying*, 3 October 2016,
   <http://cq-cq.eu/DJ5IL_rt007.pdf>.
3. Chuck Olson (WB9KZY), *What's all this iambic keyer mode A and B stuff,
   anyhow?*, Jackson Harbor Press.
4. Karl Fischer (DJ5IL), *Keyrama: A unique PIC-based multi-mode Morse
   code keyer*, 4 October 2016, <http://cq-cq.eu/DJ5IL_rt008.pdf>.
