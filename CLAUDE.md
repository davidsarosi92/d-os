# d-os — orientation for Claude

> **Purpose of this file:** give the assistant enough state to start
> working without re-reading every doc.  Keep it tight — it loads
> automatically into every session.  Update only when status moves
> milestones or when a hard convention changes.

## What this is

Hobby / teaching i386 OS kernel.  Boots from GRUB → installs its own
GDT / IDT / paging → talks to a 1280×800 framebuffer with an embedded
8×8 bitmap font → IRQ-driven PS/2 keyboard + xHCI USB host with
boot-HID keyboards → ramfs + devfs + procfs mounted at
`/`, `/dev`, `/proc` → preemptive round-robin scheduler with ring-3
syscalls via `int 0x80` → virtio-blk block device exposed as
`/dev/vda` → exFAT mountable as `/mnt` → a TCP/IP stack with a real
connection table, a server role and DHCP over virtio-net + a loopback
(`/proc/net/*`) → screen split into multiple shell panes (Alt-N to
focus, `pane split h|v` to split).

## Status (update when a milestone ships)

◐ **§M33 TIER 0 — A DRIVER FAULT IS NO LONGER A DEAD MACHINE (2026-08-27, DOCS
§4.82, all 3 arches).**  Stages 1-2 of §M33: the DECLARED placement capability
and fault containment.  **`.domains` IS A CAPABILITY OF THE CODE**, `driver.
<name>.domain` is a DEPLOYMENT DECISION that picks among the declared set and
**cannot widen it** — without that split, config could ask for something the
code cannot do and the failure would land at runtime on somebody else's
machine.  Every driver here declares KERNEL only and *that default is doing real
work*: they all call `outb`/`kmalloc`/`irq_install` directly, so none can run in
ring 3 as written.  **THE HONESTY GATE: `user`/`isolated` are REFUSED WITH THE
REASON**, never accepted and quietly run in the kernel — *a boundary you believe
in and do not have is worse than one you know you lack* (§M33's own "isolation
theatre", §M23's argument for three sound icons rather than two).  ONE function
knows what is real (`domain_enforceable`), so Tier 1 is one edit and every
caller inherits it; three copies would be three chances for one to keep refusing
after the thing became possible.  **"ALLOWED" AND "ISOLATED" ARE SEPARATE
QUESTIONS** because the DMA case is where they diverge — a DMA driver in ring 3
is allowed and is NOT isolated until an IOMMU exists, and a single boolean would
have to pick one of those to report.  **TIER 0 IS §M46's UACCESS FIXUP WITH A
BIGGER UNIT OF RECOVERY:** instead of "resume at the next instruction", *abandon
this whole call and return an error to whoever made it*.  A per-CPU saved
context, checked in the ring-0 fault handler NEXT TO the uaccess fixup and
BEFORE any policy; on a hit the trap frame is rewritten to resume in an assembly
landing pad that restores the stack and returns a failure up the ordinary C
path.  **A LANDING PAD RATHER THAN JUST EDITING THE FRAME** because a
same-privilege `iret` on i386 pops only EIP/CS/EFLAGS and the `pusha` ESP slot
is the one `popa` discards — the stack cannot be restored through the frame; all
three arches use the same shape so one reading serves for all.  All NINE entry
points go through three wrappers — *a guard applied to eight of nine is a guard
nobody can rely on*.  **THE LOCK CHECK IS LOAD-BEARING:** recovery is REFUSED
when the preempt count moved since arming (this tree's spinlocks disable
preemption, so a changed count IS "the driver holds a lock"), and the fault
falls through to the old policy — *a deadlocked machine is worse than a panicked
one, because a panic says what happened.*  **WHAT IT IS NOT, and not as a
footnote: NOT MEMORY ISOLATION.**  Ring 0, one address space, and the wild write
has already happened by the time the trap fires; what is contained is the
CONSEQUENCE of the trap-style failures.  *A mechanism that catches faults LOOKS
like isolation from outside, which is why it is written down three times.*  IRQ
handlers are deliberately UNGUARDED — a fault in interrupt context has no caller
to unwind to, and pretending otherwise returns control to a random stack.
**TWO BUGS FOUND ON THE WAY:** `/proc/drivers` indexed `__start_drivers`
directly, so **a module-loaded driver was missing from it entirely** — it was
reporting a subset and calling it the whole, and the missing ones are exactly
those most likely to be under investigation; and `lsdrv` said `QUARANTINED`
while `drv start` said **"already running"** — §M66's `driver_fault` can only
clear INITED via a shutdown hook, so for a driver with none the documented way
to clear a quarantine did not work on precisely the drivers most likely to need
it.  **VERIFIED BY MAKING DRIVERS ACTUALLY FAULT:** new `drv crash <name>`
faults INSIDE a guarded call (*a safety net nobody has fallen into is one nobody
has tested* — §M31's `hardlock`), writing to `0xDEAD0000` and **not** a low
address, because §M62 found `*(int*)0x4 =` does not fault here (low memory is
identity-mapped) and its test's pass and fail therefore looked identical.  On
all three arches the whole chain runs: the report names the driver AND the entry
point, §M66 quarantines, §M47 records, the call unwinds, `drv start` revives it,
and the shell answers afterwards — **on aarch64 the victim was a LOADED MODULE
(`loopback.ko`), so §M67 put the code there and §M33 caught its fault.**  **AND
§M67's AUTOMATIC ABI CHECK EARNED ITS KEEP UNPROMPTED:** `struct driver` grew
two fields here, `sizeof` went 20 → 28 on i386, and the stale fixture is refused
with *"struct driver is 28 bytes here, 32 in the module"* — **a number nobody
updated by hand**, catching a real change made a milestone later.  **OPEN:**
Tier 1 (the driver-runtime API + its user-mode backend + a first non-DMA driver
in ring 3) — what `domain_enforceable` is waiting for; stage 5 (an IOMMU, without
which a DMA driver outside the kernel is placement not isolation); Tier 2 (DMA
drivers in ring 3 with CLIENT RECONNECTION, the genuinely pervasive part).
`driver.profile` is deliberately absent — with one reachable domain it would be
a key with one legal value.  **NEXT: §M32 multi-user.**

✅ **§M67 — A DRIVER THAT IS NOT IN THE KERNEL IMAGE (2026-08-27, DOCS §4.81,
all 3 arches).**  §M66 left `driver_attach()` as an entry point with no caller;
this fills it — a relocatable ELF object on disk becomes a `struct driver` the
registry cannot tell apart from a built-in one.  **THE SYMBOL TABLE IS A
REGISTRY, NOT A SCRAPE:** the plan said generate it with `nm` (kallsyms's
shape), which needs a MULTI-PASS LINK and makes the export surface ACCIDENTAL —
every non-static function in the tree joins a contract nobody decided on.
`EXPORT_SYMBOL()` into a linker section makes *"what may a module call"* a LIST
SOMEBODY WROTE, and deleting a line from it a breaking change that looks like
one (41 symbols on i386, 22 on aarch64 — the difference is real: x86 registers
IRQs through `irq_install`, ARM through `gic_register_handler`, and inventing a
portable shim so the list could be arch-free would be an interface with one
caller).  **THE PART NOBODY DESIGNS FOR IS THE COMPILER'S RUNTIME:** the first
module failed to load on `__udivdi3` — a 64-bit divide on i386 is a libgcc
CALL, linked into the kernel and not into the module, *so `insmod` names a
symbol that occurs nowhere in the driver's source*.  **THE VERSION CHECK IS
TWO CHECKS AND NEITHER IS SUFFICIENT:** a STRUCTURAL FINGERPRINT (every
module-visible struct's size, computed by the compiler on BOTH sides from the
real headers — nobody can forget to update it, and a mismatch names WHICH
struct moved) plus **`DOS_MODULE_ABI`, bumped by hand, because a fingerprint
cannot see SEMANTICS** — which earned its place the same day when
`driver_ops.shutdown` changed from `void` to `int` **without changing any
struct's size at all**.  The descriptor's SCALARS precede its pointers on
purpose, so a module is refused straight out of the FILE before anything is
allocated.  **AN UNKNOWN RELOCATION TYPE IS REFUSED, NEVER SKIPPED** — a
skipped relocation is a pointer that stays zero, failing at first use
arbitrarily far from the load.  **THE BUG §M66 LEFT AND §M67 MADE FATAL:**
`audio_unregister` could already REFUSE to release a device in use, but
`shutdown` returned `void` so the refusal reached nobody and `driver_stop`
cleared INITED anyway — survivable for exactly one reason, *a built-in driver's
code cannot be freed*.  `rmmod` frees it, so a refusal nobody propagates became
a use-after-free.  Now it returns `int`, and **a module with NO shutdown hook is
refused at LOAD** (code that can never be stopped is a module that can never be
removed — a leak by construction; the loopback driver had `.shutdown = NULL`
and this is what made it grow one, plus `net_unregister`).  **THE LOADER BUG
THAT LOOKED LIKE ANYTHING BUT ONE:** the measuring pass laid sections out at
OFFSETS from 0 and the placing pass at ADDRESSES from a 16-aligned base — equal
only while no section wants more, *true on i386 and FALSE on x86_64 where GCC
aligns `.data`/`.bss` to 32* — so the last section ran off the end of the
allocation.  **It did not crash: the module loaded, the codec answered, every
bring-up value printed identically to the working arch, and only the AUDIO was
wrong** (peak 32620 for 8000, 1030 Hz for 443, L≠R) because `.bss` overlapped
whatever the heap handed out next.  Fixed by computing the layout ONCE in
offsets — two passes that are the same arithmetic instead of two that must be
kept in step.  **SHIPS `hda` ON x86 AND `loopback` ON ALL THREE**, both from the
SAME SOURCE as their built-in form (only the registration differs); the arch
split is the point, *because without a portable module the loader would be an
x86 feature with untested relocation code on the third arch* — §4.63's
`setconf` shape.  Module CFLAGS are DERIVED from the kernel's — not a
convenience but the ABI (x86_64 is `-mcmodel=large`; a module without it lands
out of range of what it calls).  `modules.autoload` (default on) means
behaviour is unchanged for anyone who never types `insmod` **and the loader runs
on every boot on every arch**; the deliberately-broken fixtures live in
`/modules/test`, which autoload does not descend into — *a test fixture that
pollutes normal operation stops being run.*  **WHAT IT IS NOT:** ring 0, one
address space, no W^X (the kernel heap is executable on all three arches today
— stated as a fact about the tree, not a goal).  A module is AS TRUSTED AS THE
KERNEL; the version check stops a STALE module, not a hostile one.  That is why
shipping before §M33 is defensible — *the first customer is our own code,
differently packaged* — and why a THIRD-PARTY driver still needs §M33 first.
**VERIFIED BY MEASURING, NOT BY LOADING:** i386 `insmod` = 7384 bytes / 214
relocations / 55 kernel symbols, then `play dsptest` gives **300.0 ms, peak
8000, 443.3 Hz, L == R, zero internal silence — byte-identical to the built-in
driver**; copied to exFAT, REBOOTED, loaded from `/mnt` with the same numbers;
x86_64 295 relocations (and `struct driver` correctly reported as 40 bytes
against the fixture's 44); aarch64's loopback module 101 relocations with `ping
127.0.0.1` replying, and after `rmmod` the next ping answers **"no route to
host"** — *which is what proves the device left the registry rather than merely
being marked down.*  New: `insmod`/`rmmod`/`lsmod`/`ksyms`/**`cp`** (the shell
could `rm` and could not copy, which is what made "put a module on the
persistent volume" impossible).  **FOUND, NOT CAUSED HERE — an intermittent
§M23 HDA defect:** leaving a machine idle after playback for the first time
showed the sound REPLAYING every **682.7 ms = exactly one revolution of the
32-entry BDL** (an HDA stream is cyclic and stops only when the driver stops
it).  **ISOLATED BY BUILDING THE SAME DRIVER BOTH WAYS** — with `hda` BUILT IN
the identical signature appears at the identical rate on both x86 arches, so it
is §M23's, not this work's.  Two faces, ~1 run in 4: `play` never returns and
the ring replays forever; or the capture carries an **882-sample (20.0 ms =
`HDA_SETTLE_MS`) silence mid-sound** with the remainder at near-full scale and
L≠R — the drain firing in the middle of the stream.  One arithmetic error was
fixed on the way and **did not fix the hang, which is said plainly**:
`hda_drain`'s budget was 4 buffers + 500 ms = 585 ms while its condition can
need a FULL revolution (682.7 ms) — *a deadline shorter than the time its own
condition takes*.  AC97 is unaffected (one clean 300.0 ms burst then ten
seconds of silence), which is the control that makes the finding specific.
**NEXT: §M33 execution domains, then §M32 multi-user.**

✅ **§M66 — DRIVER AGILITY: LIFECYCLE, HOT-PLUG, QUARANTINE (2026-08-25, DOCS
§4.80, all 3 arches).**  Asked from use: *"is driver loading plug and play?  can
we load one the moment it is needed, swap one, stop a broken one?"*  No on every
count — and **one part was worse than no: `driver_ops.shutdown` was declared in
§M8, documented as "called on power-off / reboot", and NOTHING HAD EVER CALLED
IT.**  §M52's shape exactly.  **THE BLOCKER WAS NEVER THE REGISTRY — IT WAS
LIFETIMES:** a device was handed out as a RAW POINTER and held across sleeps
(the mixer for a period, the WAV player for a whole file) while the block layer,
devfs, the IDT and the PMM each kept their own reference into driver-owned
statics, so *"stop a driver" meant dangling pointers in four registries at
once* — §M54/§M57's bug class.  Now: `audio_get`/`audio_put` count users INSIDE
a call, `audio_unregister` marks-waits-unlinks and **REFUSES rather than
unlinking under a live user**; the pump holds its reference for ONE PERIOD, not
its lifetime, *so a device stays removable exactly when removal is easiest*.
**Shutdown runs in REVERSE INIT ORDER** (init order is dependency order) through
ONE route (`system_power_off`/`system_reboot`), not five call sites (§M63's
shape) — and **the fault paths are deliberately excluded**, because a watchdog
reboot runs from an interrupt with the machine in an unknown state and *calling
driver code there turns a crash report into a second crash*.  **`drv
stop|start|swap|rescan|fault`**; a driver with NO shutdown hook is **refused**,
not forced (it cannot withdraw its registrations).  **HOT-PLUG NEEDED SOMETHING
x86 NEVER HAD:** firmware programs BARs AT BOOT, so a device added later arrives
with none and *the first symptom is a driver complaining about a BAR rather than
about hot-plug* (aarch64 always assigned them — booting raw there is no
firmware).  `pci_assign_bars` seeds its bump allocator **ABOVE the firmware's
high-water mark**: with no PCI resource manager, handing out a window an
existing device already decodes must be impossible BY CONSTRUCTION, not by luck.
Detection is POLLED (`drivers.rescan_ms`, 0 = off) and the claim is *"usable
within one interval"*, not "on the instant" — the ACPI hot-plug GPE is large
machinery for one event.  **THREE BUGS THE OUTPUT SHOWED, ALL MINE:** `%02x`
printed literally (*this printf has no width specifiers — §M65 wrote that down
the same day*); `completion IRQ on line 0` — **a hot-added device has no routed
interrupt and line 0 is the TIMER**, so installing there *is not a degraded
driver, it is a stopped machine* (refused; the polled path carries it, §M55);
and after `drv swap ac97 hda` **the poller RESTARTED ac97 two seconds later**,
silently undoing the swap — hence `DRV_S_ADMIN_DOWN`, kept distinct from
quarantine because *"the user turned it off" and "it misbehaved" are different
facts* though both mean the same to the automatic paths.  **THE REGISTRY IS A
SLOT TABLE NOW**, not an index into the linker section — which is what §M67
needs.  **VERIFIED WITH NO TYPING UNTIL THE LAST STEP:** boot with no audio
device → `device_add AC97` on the monitor → BARs assigned, unrouted IRQ
declined, `drv: 'ac97' appeared — started` → a typed `play dsptest` gives
**300.0 ms / peak 8000 / 443.3 Hz**; the swap is two separate 300.0 ms sounds
through two controllers with a live swap between them; quarantine makes `drv
rescan` start **0** drivers until an explicit start clears it.  **WHAT THIS IS
NOT:** it contains the CONSEQUENCES of a driver that fails — *not* one that
corrupts memory, because in one address space the damage is done before anything
notices.  That is §M33, and calling this isolation would be the "isolation
theatre" that plan refuses by name.  **NEXT: §M67 loadable modules — planned,
not started, and deliberately so: loading foreign code into ring 0 wants §M33
first or alongside.**

✅ **§M23 STAGE 4 + THE TASKBAR SOUND INDICATOR (2026-08-24, DOCS §4.26.1, all
3 arches).**  **A MIXER**, because a sound USED TO OWN THE DEVICE for its whole
duration — `play` and a second program could not both be heard.  Every source
opens a STREAM now; ONE pump task owns the device and mixes what is active into
each period, running exactly while somebody has audio and fully blocked
otherwise (§M55's shape).  Streams are single-producer/single-consumer so a ring
needs no lock.  **MIXING SATURATES** — *a wrap turns the loudest instant into
white noise, and only when two things overlap: maximally audible, maximally
confusing.*  **THE BUG I WROTE, AND WHERE IT WAS ALREADY WRITTEN DOWN:**
`waitq.h` states the discipline in its own header — hold the lock, LOOP on the
condition, unlock after, and **`waitq_block` RE-ACQUIRES the lock before
returning**.  I treated it as returning unlocked and `continue`d back into
`waitq_lock` = *a task deadlocking against itself with interrupts masked*; I
also called `waitq_wake_all` unlocked in three places, which the same header
forbids in capitals.  It surfaced as an **NMI HARD LOCKUP in `hal_cpu_pause`**
and §4.67.1's watchdog **named the wedged CPU correctly — the diagnostic built
for exactly this paid for itself.**  **THE PERIOD SIZE IS MEASURED, NOT CHOSEN:**
the device stops between periods (one BDL entry per call), so each boundary is a
gap the capture fills by holding the last sample — at 2048 frames *300 ms came
back as 323 ms at 411 Hz instead of 443*; 4096 measures clean.  **MASTER VOLUME
+ MUTE** applied ONCE to the finished mix (*the system volume must not change
the balance between two things playing*), and **MUTE IS NOT VOLUME 0** — stored
separately so unmuting restores the level the user chose; a muted mix still
produces frames so the device keeps its timing.  A §M63 setting
(`audio.volume`/`audio.muted`), so the Control Panel gets it with no per-key UI
code.  **THE TASKBAR INDICATOR, asked for from use: THREE ICONS, NOT TWO** — a
missing/failed device shows a DIFFERENT glyph from a user-silenced one,
*otherwise "I turned it off" and "it is broken" are the same picture and the
user goes looking for the wrong problem* — and the button is **ALWAYS DRAWN**,
because *a control that disappears when its subsystem fails leaves nothing to
point at, and "there is no icon" is not a diagnosis* (§M46's argument for chrome
that survives a wedged app).  Icons are DRAWN from primitives (§M64).  The
flyout obeys §M65's popup rule, **both chrome popups publish through ONE
function** (*a second publisher is a second thing that can forget to clear the
extent, and a stale extent swallows clicks over a window*), and the slider's
track geometry lives in ONE place that draw and hit-test both read.  Dragging
UNMUTES.  `run_qemu.sh` gained **`--no-audio`** — *the third indicator state IS
that machine, and a state nobody can boot into is a state nobody has tested.*
**VERIFIED BY MEASURING AUDIO AND READING PIXELS:** two streams from two TASKS
at 6000+4000 peak at **exactly 10000** (*an amplitude is the one thing a mix
cannot fake; a test that played two sounds and listened for "both" would pass
with one silently dropped*); playback length exact at 200.0/300.0/600.0 ms; with
`volume 50`+mute the muted run gives **no non-silent stretch at all** and the
unmuted one peaks at **4000 = 8000×128/256**; no device → grey button
(0x5A6478) + "No audio device"; muted → blue (0x3D6FB8) + cross, logging
`master 256/256 (muted)` (**the level PRESERVED, not zeroed**).  **STAGE 5 — BOTH DRIVERS QUEUE; MIXER LATENCY 85 ms → 21 ms.**  `play` returns
on QUEUEING and blocks when the device's queue is full (still real-time paced —
a slot frees only when the hardware consumes one); new `drain` waits for the
sound to actually end; and **the DEVICE advertises `period_frames`** so a
queueing driver is not held to a non-queueing one's latency.  **THREE BUGS,
EACH FOUND BY MEASURING:** (1) the ring used 4 of the BDL's 32 entries so **LVI
wrapped BACKWARDS**, which the engine reads as "already past the end" — it
halts and skips queued buffers (*300 ms came back as 129, 600 as 88*); the ring
must wrap over the WHOLE list.  (2) 3–10 ms missing off the end, variably —
`audio_stream_close` called `drain` while the pump could still be queueing the
last period, *stopping the engine out from under it*; **this file's own header
says one pump owns the device and I broke that rule three functions later.**
(3) A few ms still gone: **`DCH` means the DMA halted, not that the codec
emptied** — a settle before clearing the run bit, MEASURED not guessed (at
30 ms the capture held the last sample and 200 ms read as 231; at 4 ms exact).
*`BDL_BUP` was the obvious suspect and was measured INNOCENT* — removed anyway
because it means "this is the last buffer" and that is wrong in a ring.
**MEASURED: x86 exact at 200.0/600.0/1000.0 ms (0.9 % short at 300), aarch64
exact at 200.0/300.0/600.0, zero silent samples, two streams still peak at
exactly 10000, WAV unchanged.**  **STAGE 6 — THE COMPLETION INTERRUPT, AND THE LAST 0.9 %.**  x86 was 0.9 %
short at one length while ARM measured exact, and *the difference was the
SIGNAL, not the arithmetic*: ARM knew exactly which buffers the device had
finished (its used ring), while AC97 inferred it from `CIV` plus a settle
constant tuned against one emulator.  The `IOC` interrupt supplies the same
exact fact — ISR does TWO things and no third (ack + count; §M49's xHCI lesson
was a drain inside an ISR reaching code that blocks), `outstanding` becomes
`submitted - completed`, and **the driver learns its interrupt works by
RECEIVING one** (§M55) so a never-firing IRQ costs latency, not silence.
*`irq_install` does not CHAIN in this tree, so the line is logged and a
collision is visible rather than mysterious.*  **THAT ALONE DID NOT FINISH IT:**
300 ms became exact and repeatable but 200 and 1000 lost a few ms — **the drain
was clearing the run bit the instant the last completion arrived**, cutting
whatever the codec had not yet emitted.  It does not: every queued buffer has
completed, so the engine halts by itself, and the next sound resets the PCM box
anyway.  **NOW EXACT AND REPEATABLE AT 200.0 / 300.0 / 600.0 / 1000.0 ms on
x86, matching aarch64.**  The settle constant survives only on the polled path
and is documented as the crutch it is.  **STAGE 7 — CAPTURE, ON BOTH ARCHES, WITH AN HONEST ACCOUNT OF WHAT IS
PROVEN.**  `rec <path.wav> [ms]` + a readable `/dev/dsp`.  AC97's PCM-IN box and
virtio-sound's RX queue, each with **their own** ring and buffers — *recording
while playing is two independent flows and sharing either would have one
silently corrupt the other.*  **A RECORDING STARTS WHEN YOU ASK FOR IT:** a
capture engine keeps filling its ring, so the first version returned stale
audio — **`rec 250` completed in 0 ms** — which is not "record 250 ms" but
"give me the last 250 ms that went past"; a new `record_start` op re-arms, and
`/dev/dsp` arms once per open.  **WHAT IS *NOT* VERIFIED, SAID PLAINLY: no QEMU
backend here can inject a known signal** (`wav` is output-only and rejects
`in.path`, `none` is silence, `coreaudio` is a real microphone), so the frame
counts, the file's shape and the start-on-request are testable and **the
CONTENT is not.**  **THE TWO ARCHES THEN NARROWED THE ONE UNEXPLAINED NUMBER:**
same core, same null backend — **virtio-sound paces to 991 ms of 1000 (99 %)
while AC97 runs 804 ms (80 %)** — so the null input DOES pace on one path, which
points the AC97 discrepancy at the AC97 side (its ADC rate, or our reading of
`CIV`) rather than at the core.  *A narrowing, not a diagnosis, and written down
as such.*  Round trip verified on both: `rec` then `play` reproduces
48000 frames / 1000 ms.  **OPEN:** that AC97 capture rate, Intel HDA, an
interrupt for virtio-sound, and whether `/dev/vda` should be published on ARM.

✅ **§M23 STAGE 2 — A WAV PLAYER, AND THE TWO SILENT TRUNCATIONS UNDER IT
(2026-08-24, DOCS §4.26.1, i386 + x86_64 measured, all 3 arches build).**
`play <path.wav>` — and before it could exist, two things in the stage-1 path
had to stop lying.  **THE DRIVER WAITED BY SPINNING:** `ac97_play` sat in
`hal_cpu_pause()` for the ENTIRE duration of the sound — three seconds of audio
burned three seconds of a CPU, and on a UP box the only one.  *§M49's, §M55's
and §M56's lesson for the THIRD time — waiting must not cost the same as
computing* — and it survived because the only caller was a beep short enough
that nobody noticed.  It sleeps now against a REAL-TIME deadline from the
buffer's own duration (a wedged device costs a bounded wait, not a hung shell);
**deliberately not an interrupt yet** — AC97's IOC would replace the poll with a
waitq exactly as §M55 did for the NIC, and that is written down as the next step
*because unlike the spin, a sleeping poll is already CORRECT, merely coarse*.
**TWO SILENT TRUNCATIONS, THE SAME SHAPE ONE LAYER APART:** `ac97_play` clamped
to its DMA buffer and returned SUCCESS, and `audio_play_tone` clamped to its
render buffer and returned success — so **`tone 440 3000` played 666 ms and said
nothing about the missing two and a third seconds.**  Invisible while the only
caller is a beep that fits; silent data loss the moment anything streams.
`play` returns the frames it ACTUALLY consumed (a short count is the normal
case), `audio_play_pcm` is the loop every caller would otherwise write and get
wrong its own way, and the tone renders in chunks **carrying phase and level
across them** — restarting per buffer is an audible click twice a second plus a
dropped partial half-period each time.  **THE PLAYER STREAMS, NEVER LOADS** (one
input block, one output block, both static: a four-minute song is ~40 MB of PCM
— §M60's wallpaper argument, worse); RIFF chunks are skipped by DECLARED LENGTH
so LIST/INFO/fact are not parse errors, and a non-PCM format is **REFUSED BY
NAME** — *feeding compressed bytes to a DAC is exactly what noise sounds like.*
**THREE CONVERSIONS, EACH A WAY TO BE SILENTLY WRONG:** 8-bit WAV samples are
UNSIGNED (biased around 128 — treating them as signed is a classic, very audible
bug); **mono is DUPLICATED** (played into one side it sounds like a broken
speaker and every listener blames the hardware); and rate conversion is
NEAREST-SAMPLE **and says so** — not band-limited, aliases on a big ratio, but
the alternative is refusing every file that is not already 48 kHz *and most WAV
files in the world are 44.1*.  The fixed-point accumulator **carries across
blocks and is never re-derived from a block index** — §M53's periodic-timer
lesson in a different costume, where re-deriving accumulates one rounding error
per block into audible drift.  **VERIFIED BY MEASURING THE CAPTURED AUDIO, NOT
BY LISTENING** — the test file is written BY THE GUEST (`play testwav`, §M60's
rule) and is deliberately NOT the device's format (22050 Hz, mono, 8-bit),
*because a file that already matched the DAC would exercise none of the three
conversions*: through `-audiodev wav`, **498.9 ms for 498 asked, amplitude
exactly ±18432 (= (200-128)<<8, so the 8-bit bias is right), 440.0 Hz by zero
crossing (the resampler preserves pitch), L == R on all 22001 frames (mono
really was duplicated)** — and the spin fix has its own number from the playing
task's own `cpu_ms`: **3000 ms of audio costs 204 ms of CPU (1 → 204); spinning
it would have been ~3000**, with the full three seconds now arriving (3000.0 ms
captured where the old clamp gave 666) at an unchanged 444 Hz, *which is what
proves the phase carried across all five chunk boundaries.*  `play`/`lsaudio`
live in `audio.c` and are wired into the aarch64 serial REPL too (§M24's rule):
**that arch has the audio CORE and no audio DEVICE**, so they answer `no audio
devices` — the honest failure, better than "unknown command" telling the user
the feature does not exist.  **AND THE ARCH ASYMMETRY IS CLOSED: aarch64 HAS A SOUND DEVICE NOW**
(`virtio_snd.c`) — AC97 is a PCI card and `-M virt` has no slot for one, so ARM
had the core, the commands and NO DEVICE, which reads as *"sound is an x86
feature here"*.  virtio-sound over virtio-MMIO, the same way this machine gets
every other device.  **TWO THINGS WORTH CARRYING:** a TX message is a THREE-part
descriptor chain (device-READABLE header + payload, device-WRITABLE status) —
merge them or drop `VRING_DESC_F_WRITE` and the device REJECTS the buffer, *which
is silence with no error anywhere*; and **the stream count comes from CONFIG
SPACE, do not assume one** — the first version asked for a fixed four and got
`BAD_MSG` (0x8001), because per spec a query past the available items is
MALFORMED, not merely optimistic.  *It failed cleanly only because the status was
CHECKED — an unchecked request would have left the info buffer zeroed and picked
"stream 0, direction 0": silently right on this device and wrong on the next.*
**MEASURED ON ARM THROUGH THE SAME ANALYSIS AS x86** — 399.1 ms for 399 asked,
±18432, 439.7 Hz, L == R — *three arches, one file, the same audio.*  **PLUS THE
FIFTH APPEARANCE OF THE HARNESS SHAPE:** `run_qemu.sh` attached NO audio device,
so an everyday boot had no sound card while every audio TEST passed its own
`-device` (§M48 NIC, §M49 -smp, §4.66 disk, §4.67.1 watchdog/VGA — now this).
Both arches get one, `DOS_AUDIO=none` is the deliberate escape (matching
`DOS_DISK=none`); and `dos-shell-test.py` gained **`--no-display`**, because
aarch64 has TWO boot paths (framebuffer → full `shell.c` on a VC; none →
`serial_shell.c` on the PL011) and with the GPU permanently attached *the serial
one could not be driven or read at all.*  **STAGE 3 — `/dev/dsp`, AND THE PACING BUG ITS OWN MEASUREMENT CAUGHT.**  Raw
PCM as a file (§M59's `/dev/clipboard` argument applied to the speaker: no new
syscall, and it works for BOTH personalities because a Linux-ABI binary has no
d-os syscall numbers to call).  **THE NODE BELONGS TO THE SUBSYSTEM, NOT A
CARD** — registered even with no driver up, so *"this machine cannot play
audio" and "no such device file" are different answers to a program rather than
the same one*.  No `SNDCTL_DSP_SPEED`: changing the format is **REFUSED**, not
accepted-and-ignored, *because silently taking a rate you do not honour plays
everything at the wrong pitch — the most confusing way an audio device can
fail.*  **THE BUG:** the first version played whatever each `write()` held, so a
1000-byte-chunk writer (what a real one is) got a 250-frame playback per call —
the DMA engine started and halted every 5 ms and the gaps stretched the stream:
**300 ms written came back as 256.4 ms, 444 Hz as 403.7.**  *The amplitude and
the L/R pairing were perfect in BOTH runs, and that is what named the fault: the
framing was right and the PACING was wrong.*  With period buffering: **300.0 ms,
443.3 Hz, ZERO silent samples inside.**  Hence **devfs grew a `close` HOOK** — a
write plays only whole periods and the remainder waits, and without a drain
point the last partial period would be dropped from every sound (*a loss that
sounds like a truncated file*).  Partial FRAMES carry across writes for a
sharper reason: *dropping four stray bytes does not lose a sample, it shifts
every later sample by one channel and swaps left with right for the rest of the
stream.*  **AND AARCH64 HAD NO `/dev` AT ALL** — `devfs_init()` is called from
x86's `kernel_main` and this arch runs its OWN entry path, which never called
it: `ls /dev` answered `(empty)`, so §M59's `/dev/clipboard`, §M39's
`/dev/urandom` and `/dev/null` were **effectively x86-only, silently** (the
drivers register into a list; the list was never published as files).  *The
exFAT mount is why nobody noticed: `/dev/vda` is a devfs node, but a mount
resolves its volume through the block layer BY NAME, so storage worked without a
`/dev` to look in.*  Verified on i386 AND aarch64 by `play dsptest`, which
writes through the VFS in deliberately awkward 1000-byte chunks.  **OPEN:**
mixer/multi-stream, PCM input, Intel HDA, the AC97 completion interrupt, and
whether `/dev/vda` should be published on ARM (its block driver initialises
after `devfs_init` there — not chased).

✅ **§M64 TAIL — A DESKTOP YOU CAN ARRANGE, AND THE PERSISTENCE IT ONLY CLAIMED
(2026-08-23, DOCS §4.79, all 3 arches build, i386 driven).**  §M64's three open
items closed — and the second thing closed was not on the list.  **A POSITION IS
A GRID SLOT, NOT A PIXEL**, and everything follows from that: §M61 made the
resolution a runtime choice, so an icon positioned in pixels goes off the screen
at the next smaller mode — *silently, because nothing draws outside the box, and
what the user sees is a shortcut that was deleted.*  The `.lnk` has carried
`x`/`y` since §M64 and nothing had ever read them.  **TWO OPTIONAL POINTS
APPENDED to the item-view interface** (§M58's positional-initialiser scar, now a
rule): `item_model.pos` says where the owner put an item, `item_view.slot_at`
turns a drop point into a slot — and **`slot_at` is optional ON PURPOSE, because
a layout decides whether its items can be positioned at all**: the list and the
table say NO by leaving it NULL, since dropping row 3 onto row 7 means REORDER,
a different feature with different persistence.  *A caller can tell "this view
cannot be arranged" from "the drop missed".*  Models without `pos` (Control
Panel, file manager) are untouched.  **PLACEMENT IS THE MODEL'S JOB:** an
unplaced item takes the first slot no PLACED item claimed — flow order knows
nothing about the cells somebody dragged into, and *two icons in one cell is not
a layout, it is a lost shortcut*.  With slots in play `grid_hit` stops doing
arithmetic and asks `grid_rect` — one source of truth, *because a view that
draws correctly and hit-tests wrongly is invisible in a screenshot*.  **A DROP
ONTO AN OCCUPIED SLOT SWAPS** (stacking hides one and the hit test can only
return one of them; refusing springs the icon back for a reason nothing on
screen explains), the live preview is MEMORY-ONLY and the file is written once
on release (*a drag crosses a dozen cells; each would be a `.lnk` rewrite, i.e.
VFS traffic proportional to hand tremor*), and **the gesture's ORIGIN is
captured at press** — the preview overwrites the stored slot, so the swap must
hand over the slot the user STARTED from, not one from mid-gesture.
**TRANSPORT:** `desktop_pointer(x,y,phase)` carrying §M58's WPTR_* (the same
vocabulary, not a second one) **plus a GRAB** — without it the gesture ends at
the first window it crosses.  **THE KEYBOARD: THE DESKTOP IS THE FOCUS OF LAST
RESORT** — and the keycodes were not being dropped where it looked
(`dispatch_keycodes` skips events with no focused window, but nothing reached
it: `gui_raw_key` only ENQUEUES when there is a focused `WIN_APP`).  **Enter and
Escape are gated on the desktop having a selection, and that gate is
load-bearing:** the GUI suppresses the console but keys still reach its VC —
that is how a command is typed with the desktop up and **how this project's own
harness drives every GUI build**, so consuming Enter unconditionally would have
made the test that proves the feature its first casualty.  The neighbour is
GEOMETRIC (item 5 may sit left of item 2), weighted 4× perpendicular so "right"
prefers the current row; a search that finds nothing leaves the selection alone
— *at the edge of the field the honest answer to "move right" is "stay".*
**AND THE BUG UNDER ALL OF IT: SHORTCUTS DID NOT SURVIVE A REBOOT.**
`SHORTCUT_DIR` was a constant pointing at `/desktop` = **ramfs**, while the
persistent volume is the exFAT mount — so *"a shortcut is a FILE so that it
survives a reboot" was true about the format and false about the outcome*, and
every document here claimed otherwise.  **§M63 stage 0's bug exactly, one layer
over**, hidden for the same reason: the write SUCCEEDS.
`shortcut_attach_persistent("/mnt")` on **BOTH** entry paths (miss one and that
arch keeps losing them), creating the directory IS the write test, and it names
which mode it is in.  **VERIFIED IN THE ORDER THAT MAKES EACH STEP FALSIFIABLE:**
`shortcut move` is the drop WITHOUT a mouse (so slot + swap + both rewrites are
testable with no display), `shortcut check` now prints each item's slot, its
pixels AND the view's own hit test there with a `MISMATCH` flag; then a real
reboot (both shortcuts and slot (2,1) return); then a DRIVEN MOUSE DRAG
(`desktop: shortcut 0 moved (-1,-1) -> (1,0)`, and the next boot reports slot
(1,0)); then `sendkey` alone walking two icons in different rows/columns, End,
Escape, and **Enter opening one** (`app-host 'app:Task Manager' up`).  Every
pointer hop under 90 px (§4.60's signed-byte lesson, re-paid).  **The drop LOGS
itself** — *a screenshot cannot distinguish an icon that moved from one that
moved and will be back in its old slot at the next boot, which is exactly the
bug this work contained.*  **Also closed:** file manager **Send to desktop**
(one row in §M65's declared menu model, `file:<path>` — a pointer, not a copy),
and **`gui.mode` is confirmed applied at `gui_start`** (the status text listed it
as open; the code reads it and a reboot comes up at 1024×768).  **STILL OPEN:**
`run:` targets (want a terminal window accepting an initial command — a shell
change, and `shortcut_launch` SAYS so rather than doing nothing); exFAT
`rename`; the Send-to-desktop menu ROW is not pointer-verified (only the code it
calls), because the harness cannot type once a GUI window has focus.  **NOTICED,
NOT CAUSED HERE:** `gui stats` prints nothing on serial while the GUI is up
though `shortcut list` does — likely writing to the suppressed console.

✅ **§M65 TAIL — THE OPEN ITEMS, AND THE BUILD TRAP CLOSED FOR GOOD
(2026-08-23, DOCS §4.78).**  **Tab / Shift+Tab cycles focus**, handled at the
WINDOW level and wrapping — no control can know what comes after it, and a
toolkit whose third field is reachable only by mouse is one half the people
cannot use.  **A SCROLLING CONTAINER** (`UI_SCROLL`): children are laid out at
natural height and the column is offset, rather than drawn into an offscreen
surface — the widgets already carry coordinates and the layout already places
them.  The settings panel's grid now lives in one, so a long group no longer
needs a tall window (`320 px of content in 280 px (scrolls)`, logged at build
time because *whether the content is taller than its viewport is a NUMBER, and
without it "the page looks cut off" and "the page scrolls" are the same
picture*).  **THE FILE MANAGER IS A MODEL + THE TABLE VIEW NOW** — its header
used to be a string with spaces in it (`"NAME          SIZE"`) and its rows
pre-padded text, with a SECOND array of raw names because path arithmetic must
not see the padding: two representations of one directory kept in step by hand.
`fileman.view` = table|list|grid.  **TWO CLIPPING BUGS, BOTH ABOUT ONE
INVARIANT.**  First: the clip did not DESCEND — a scrolling container set it on
its direct children, and a grid inside a viewport has none, so its labels
scrolled out over the panel's title.  *A clip that does not descend is not a
clip.*  Second, and the one worth remembering: `ui_text_clipped` ALSO set a
clip, to the widget's own box — and `gfx_set_clip` REPLACES, so the inner one
threw the viewport away.  **Two mechanisms for one invariant, and the narrower
one lost.**  The clipping now happens in exactly one place (`widget_draw_all`,
which clips every widget to its box or to its inherited viewport).  **AND THE
BUILD CONVENTION IS NOW A BUILD FEATURE:** `-MMD -MP` + `-include`, so editing a
shared header rebuilds what includes it.  The old rule ("run make clean after
editing a header") was forgotten TWICE TODAY — §M63's descriptor gained two
fields and the settings panel reported "no settings declared"; `struct widget`
gained four and the Control Panel jumped to 0x53f000ff — *a documented
convention that must be remembered is a bug generator.*  Verified by touching
`widget.h` and watching exactly its dependents recompile.  *Also: the kernel
printf has no width specifiers, so `%-3d` prints literally — it turned the first
version of the layout dump into garbage precisely when it was needed.*

✅ **§M65 — A WIDGET TOOLKIT WITH A SEAM (2026-08-23, DOCS §4.78, all 3 arches).**
Asked for from use: *"components behind one API, so anyone can add their own or
swap the units — and responsive, ARM machines have small screens"*, then *"yes,
usable from ring 3 too"*.  **THE MISSING PIECE WAS NOT THE CONTROL LIST:** M22's
five controls all took ABSOLUTE PIXELS, which is exactly why the settings panel
rendered a boolean as a text box with a "Cycle" button — there was no checkbox
and nowhere to put one.  **FOUR DECISIONS:** a two-pass LAYOUT (measure
bottom-up, arrange top-down; no constraint solver — *a layout that needs
iteration to settle is one whose result nobody can predict*); **identity by
NAME** (`WIDGET_CLASS()`, the `ITEM_VIEW()` shape); **the description is DATA**
(`struct ui_spec` = ints + strings); and **ONE event sink per window**
(id, type, value).  The last two because **a name and an integer cross a process
boundary and a function pointer does not** — the ring-3 answer decided the API's
shape from the first line.  **NEW CAPABILITIES GO ON THE CLASS, NOT INTO
`widget_ops`** — measure/get_value/set_value/get_text/popup_pick cost zero edits
to existing tables (§M58's positional-initialiser scar).  Nine classes; checkbox,
radio, slider, combo, menubar are new.  **RESPONSIVE IS MEASURED IN CELLS, not
pixels** (one fixed 8×8 font, no DPI): three size classes, and the behaviour is
one rule — `UI_WRAP_COMPACT` turns a row into a column.  *A 12-column grid with
six breakpoints answers a browser's problem, not ours.*  **THE PROOF IS THE
SETTINGS PANEL: zero per-key UI code** — the DESCRIPTOR picks the control
(bool→checkbox, enum→radio ≤3 / combo more, ranged int→slider) — and
`config_apply` now LOGS the change, so a panel that applied a value and one that
silently did nothing no longer produce the same empty log.  **THE WINDOW POPUP**
(one slot; *an open popup owns the next click* wherever it lands, and it must
NOT also reach the window underneath; Escape closes it and the owner still hears
row -1) serves both the **menu bar** — a declared `(menu, item, id)` model, in
the file manager, *because a row number describes the menu's shape and an id
describes the command* — and the **combo**.  **THE TABLE IS THE ITEM MODEL ASKED
A SECOND QUESTION** (columns/col_title/col_weight/cell APPENDED; a model that
leaves them NULL still renders): `controlpanel.view = table` is a config change,
not an app change.  **TWO THINGS REPORTED FROM USE MID-BUILD, BOTH REAL:** the
menu left *"colouring along the mouse path"* — hover set `need_frame` and
claimed NO AREA, and a frame with an empty damage list repaints only the
CURSOR's rectangle (§4.61), so the highlight was painted into the cursor's
footprint; and *"the System page all runs together"* — a box per row gave every
row its own label width, so nothing lined up.  `UI_GRID` shares ONE label column
(and the table now sizes columns from CONTENT, not weights: *weights are a
preference, the longest cell is a fact*).  **RING 3 USES THE SAME TOOLKIT:**
`dosgui_ui_build` takes a header + fixed-size int32 records + a string pool
addressed by OFFSET (copied in first, every offset bounds-checked, pool
force-terminated), events come back on the queue the client already drains
(type 5), the op went in through §M50's engine AND the native dispatcher under
ONE number — *a program's window code should not depend on which libc it was
linked against.*  **VERIFIED by `user/uidemo.c`**: widgets drawn, and clicking
prints `widget id 10, type 2, value 1`.  **TWO BUGS ONLY A RING-3 CLIENT COULD
EXPOSE:** nothing DREW them (a client-managed window has `host_task` cleared by
design, and §M40 had closed only the INPUT half of that hole) and nothing was
CLICKABLE (the raw pointer stream was forwarded to the client — correct for a
client that draws its own pixels, wrong for one that asked the kernel to run its
interface).  **OPEN:** the file manager's list is still the M22 listview (24
direct field accesses — the table wants the model refactor first); no scrolling
container; `ui_build` is build-once (a resize re-LAYOUTS — stated in ui.h
because the first attempt built a second set of controls and the panel came back
empty); no Tab focus cycling.

✅ **§M58 SCROLLBACK, §M59's REAL BUG, AND §M61 FINISHED ON aarch64 (2026-08-22,
DOCS §4.75–§4.77, all 3 arches verified).**  The three open items, closed.

**§M58 — A TERMINAL YOU CANNOT SCROLL BACK IS ONE WHOSE OUTPUT YOU CANNOT
SELECT** once one more line has arrived.  History is a ring (`gui.scrollback`,
500 lines ≈ 120 KB at 1920 px); a scroll pushes the evicted row into it, so does
the shrink half of a resize.  **THE PART TO REMEMBER: the selection is addressed
in ABSOLUTE LINE NUMBERS.**  A grid row is a position on the SCREEN and one line
of output renumbers every one of them — a selection held in grid rows silently
slides onto text the user never pointed at.  `gterm_row(abs)` answers "where
does that line live now" for the renderer, the hit test and the copy alike, so
the three cannot disagree; a line aged out of the ring yields NOTHING rather
than the wrong text (*copying whatever occupies that slot today is worse than a
short copy*).  Drawing respects the view — otherwise the live shell paints over
the history being read, the one thing scrollback exists to prevent — and while
scrolled back a scroll moves only the MODEL, the compositor re-renders (§M22.7's
split).  Wheel = 3 lines/notch, **Shift**+PgUp/PgDn pages (plain PgUp/PgDn
belong to whatever runs IN the terminal; stealing them breaks those programs
invisibly), typing snaps to the live bottom, and a `[N lines back]` tag says so
— *a terminal that silently stops showing new text is indistinguishable from one
that has hung.*  **VERIFIED BY `termcheck`, WHICH ASKS THE MODEL:** it writes
numbered lines until they scroll off, selects one BY ABSOLUTE NUMBER and prints
what the copy path returns (`"SBLINE 3"` — PASS).  *A screenshot can show a
window looks scrolled; it cannot show the selection still names the text the
user pointed at.*  Screendump confirms the rest: 5 wheel notches → exactly
`[15 lines back]`.

**§M59 — THE "CLIPBOARD WON'T TAKE A RING-3 WRITE" REPORT WAS NOT ABOUT THE
CLIPBOARD.**  `fd_lookup` rejected anything below 3: **fds 0/1/2 were not table
entries at all**, the console was reached by NUMBER inside read/write, and
`dup2(fd, 1)` — how every shell implements `>` — returned -1.  So NO program
here could redirect anything, in either direction, *silently, with a successful
exit status*.  They are ordinary slots now and **the console is the DEFAULT
(a NULL entry), not a special case**, which is what keeps every non-redirecting
program working unchanged; five call sites follow (read/write fallbacks, close
releasing a redirected stream, dup2 accepting 0-2, poll readiness).  `F_DUPFD`
still clamps to 3 on purpose — fd 0 is *free* here, so honouring `F_DUPFD(0)`
would let a library silently take over stdin.  **VERIFIED FROM RING 3 ON ALL
THREE ARCHES** by `redirtest` (open → dup2 onto stdout → write → read the file
back → then the same onto `/dev/clipboard`), written because the musl coreutils
are NOT in the tree (`pkgrun sh` said *'sh' is not installed* throughout — the
weak blob symbols are absent).  Plus **typed offers**: both slots carry a
MIME-shaped type (default `text/plain`, `clip type`, ioctl on the DEVICE so both
personalities can reach it — a Linux-ABI binary has no d-os syscall numbers and
no Linux clipboard call to borrow).  **NOT DONE, WITH THE REASON:** Wayland
`wl_data_device` — four interfaces plus fd-passing, and *nothing in the tree
would exercise it*; shipping a protocol surface with no client to falsify it
against is how a feature "works" until the first real user.

**§M61 — aarch64 MODE SETTING, the decline reversed.**  Same rule as x86: build
the NEW framebuffer + resource + backing, switch the scanout in ONE command,
take the old apart only afterwards — a failure before the switch leaves the
display exactly as it was.  **The resource id ALTERNATES** (the old one is still
bound while the new is built, and a device may refuse a live id), and **detach
before unref** (between those commands the device still holds a pointer into RAM
about to be freed).  **`pmm_free_contiguous(addr, n)` had to be written**: a
contiguous run is ONE buddy block of order ceil_log2(n), freeing it as n frames
corrupts the accounting, and the order cannot be recovered from the address —
so the free takes the count the alloc used.  Nothing had ever freed one, which
is why the gap survived.  **VERIFIED BY THE SCREENDUMP'S OWN SIZE:** 1280×800 →
`mode 1024x768 --force` → 1024×768; `mode 800x600` left alone → 800×600 → back
to **1280×800** with `gui: reverted to 1280x800`.  **AND THE HARNESS NOW GIVES
ARM A DISPLAY** — it passed no virtio-gpu, so every ARM test booted serial-only
and nothing that draws could be tested there at all: *"aarch64 declines" stayed
true partly by accident.*

🔧 **THE LOCKUP REPORT NAMED THE WRONG CPU (2026-08-22, DOCS §4.67.1).**
Reported as a GUI fault — *"the Start menu comes apart, the cursor sticks, half
the taskbar disappears, it flickers"* — and **none of it was a drawing bug**:
`/tmp/dos-serial.log` (kept automatically by `run_qemu.sh`) held two
`!! NMI HARD-LOCKUP … rebooting` records.  *A screen that stops mid-frame with
the pointer where it was IS a frozen machine; the flicker is the reboot.*  **AND
THE REPORT POINTED AT THE HEALTHIEST CPU IN THE BOX** — the address sat in
`hal_cpu_halt`, §4.67's signature: the alarm interrupts ONE core and on a 4-CPU
box that is most likely an idle one.  Fixed by naming the CPU that STOPPED:
layer 2 already keeps every CPU's tick counter plus its value at the last sweep,
so the NMI handler prints each CPU's progress and points at the least-advanced
one (`watchdog_cpu_tick_state`, plain reads — no lock, no alloc, NMI-safe).
**Verified with `hardlock`: the alarm lands on CPU 1, the report names CPU 0,
which is where the test pins itself.**  *The signal is smallest PROGRESS, not
equality* — the sweep snapshot is up to WD_SWEEP_MS old, so the first version's
`ticks == snapshot` test flagged nobody.  **THE HARNESS COULD NOT HAVE CAUGHT
IT — FOURTH TIME IN THIS SHAPE:** `dos-shell-test.py` passed no `ib700`, no
`-vga none -device VGA,vgamem_mb=32` (hence **no page flip** — std-VGA has no
room for a second 1920×1200 frame), a different RTC and half the RAM, so every
GUI test ran on a different machine from the one a person boots (§M48 NIC, §M49
-smp, §4.66 disk, now this).  All four are passed now.  **NOT ROOT-CAUSED, said
plainly:** the lockup is intermittent and host-load dependent (twice in one
session, absent in five later runs incl. three clean boots in the exact
configuration) — what changed is that the next occurrence names the wedged CPU.
**Also a real regression from the same day:** `gui stop` was dispatched on the
`gui ` PREFIX above the existing exact `gui stats` arm, so `gui stats` answered
"already running" — *a generic prefix arm added above an exact one silently
swallows it.*  `gui stats` now also prints the DESKTOP task's counters (loop
iterations, panel repaints, chrome events, half-second ticks): *"the taskbar is
not updating" has three causes that look identical from outside* — the loop is
not running, it never marks itself dirty, or its damage never reaches the
compositor.  *Method note at my own expense: three rounds were spent chasing a
"Start menu broken at -smp 4" that was MY test dropping mouse packets (a new
monitor connection per command), and a "stuck cursor" that was my detector
counting the wallpaper's white highlights.  An instrument that silently drops
input produces confident, wrong conclusions.*

✅ **THE DESKTOP IS WHERE BOOT ENDS — AND LEAVING IT LANDS ON A SHELL
(2026-08-22, DOCS §4.74, i386 + x86_64 mouse-verified, all 3 arches build).**
Three requests that turned out to be one mechanism: a Start-menu button to close
the GUI, automatic desktop start, and *"if we exit, we should land back in the
shell."*  **THE SHELL IS SPAWNED FIRST AND STAYS BEHIND THE DESKTOP** —
`gui_start` only SUPPRESSES the console, so "back to the shell" is not something
to re-create, it is something to stop hiding.  `gui.autostart` (default on) is
read by ONE function called from BOTH boot paths (x86 `kernel_main`, aarch64
`main_entry`), because a feature that exists on one entry path only is a shape
this project has already paid for (§4.63's `setconf` on ARM).  **Exit GUI is a
third tail item above Reboot/Shut Down** — session, kernel, machine, and the
session one is the only REVERSIBLE one (`gui` brings it straight back, verified
on a fresh compositor pid).  **THE TEARDOWN RUNS ON ITS OWN DETACHED TASK, and
that is structural:** the click is dispatched ON THE COMPOSITOR and the teardown
KILLS the compositor — *a task cannot free the surfaces it is still composing
from, nor outlive its own kill_tree to tidy up.*  **THE ORDER IS THE DESIGN:**
unhook input FIRST (an event delivered into a dying compositor is the classic
teardown crash) → hand every app-host's REAP back to init (§M27's reaper skips
owned tasks, so a host owned by a dead compositor is a corpse nobody may
collect) → `kill_tree` the session → **WAIT for it to actually be gone** (*"we
asked it to die" is not "it is dead"*, and freeing a backbuffer mid-compose is a
multi-megabyte use-after-free; poll for DISAPPEARANCE, never `task_wait` — init
may reap it first, §M57) → windows through `destroy_window` (so a dosgui bridge
still gets its dispose callback) → surfaces → **the SCANOUT back to buffer 0**
(the console writes into the BASE framebuffer; a display left panned to the
flip's second buffer gives a black screen produced by a working console) →
un-suppress + push an empty line, because the shell prints its prompt only after
it reads one.  `gui stop` exists too: *a way out that only works while
everything works is not a way out.*  **VERIFIED BY DRIVING THE MOUSE** (launch
Task Manager → Start → Exit GUI): serial shows `reaped 'app:Task Manager'`,
`reaped 'desktop'`, `session ended`, `reaped 'compositor'` — **an app started
inside the session is closed by leaving it** — and the screendump shows a live
`d-os>`.  *Two test lessons: every pointer hop must be ≤90 px INCLUDING the one
that homes it (the PS/2 delta is a signed byte — a single `mouse_move -2000`
is clamped and the click lands elsewhere, which the first run did), and once a
GUI window has focus the harness can no longer type shell commands at all.*

✅ **FOUR SPLASH BUGS, ALL REPORTED FROM USE (2026-08-22, DOCS §4.71.1).**
(1) **The splash now goes up on the DEFAULT, immediately** — only the OVERRIDE
needs the disk, so the first version's blank screen until the mount was the
wrong trade: *a blank screen is not a neutral state to somebody watching a
machine boot; it is what a hung machine shows.*  Cancelling later wipes the logo
and replays the missed lines.  (2) **Raised right after `module_init_all`**, the
call that ACTIVATES the framebuffer console — everything printed between the two
was drawn, which was the *"few lines before the boot screen"*; what remains is
SeaBIOS's own banner (GRUB's share silenced with `timeout=0` + `clear`).
(3) **ESC — AND EVERY KEY — NEVER WORKED DURING BOOT.**  Rule 4 lived in
`vc_kbd_push`, which looks like where keys arrive and is not: the drivers call
it only when a VC is FOCUSED, and `vc_init` runs at the END of boot, so for the
whole period the splash is up the escape hatch was unreachable.  It hooks
`vc_raw_kbd_dispatch` now — the one call ALL THREE input drivers (PS/2, USB HID,
virtio-input) already make BEFORE deciding where a key goes, pre-translation, so
Escape and function keys work too.  Evidence is a serial line, which makes it a
headless test rather than a claim.  (4) **`splash_end` cleared nothing** — its
`console_clear()` broadcasts to ACTIVE sinks and `vc_init` had deactivated the
framebuffer one, so the last progress bar sat under the first prompt.  (5) **And
the sink restore RESURRECTED a sink `vc_init` had killed**, so every kprintf
from a task with no VC painted across the framebuffer — `meminfo` output over
the wallpaper.  *A remembered flag is only valid while nobody else is allowed to
change what it describes.*  Instrument note: the logo timing now says "too early
to time" instead of printing 0 — neither clock is running that early.

✅ **exFAT CAN CREATE AND REMOVE NOW — THE §M12 GAP (2026-08-22, DOCS §4.73,
i386 + x86_64 verified, all 3 arches build).**  Asked for directly, and the one
thing §4.72's persistent package store was blocked on: a store is a DIRECTORY
PER PACKAGE and `.mkdir` was `NULL`.  **A directory is a file with three
differences** — `ATTR_DIRECTORY`; a **ZEROED** first cluster (in exFAT "end of
directory" is an entry whose type byte is 0x00, so an unzeroed cluster is a
directory full of whatever the disk held before); and a `DataLength` of one
CLUSTER, never zero (a zero-length directory reads as having no entries, to us
and to every other driver).  **Removal is not erasure**: exFAT clears **bit 7**
of each entry's type byte (0x85 → 0x05), and the whole SET must be cleared by
walking SecondaryCount — clearing only the File entry leaves orphan Stream/Name
entries that `fsck` reports and another driver may believe.  Freeing the
clusters needs BOTH allocation shapes (a `NoFatChain` contiguous run sized from
`DataLength` vs a FAT walk); getting that wrong corrupts nothing visible and
**silently leaks free space** — the bug that surfaces months later as "the disk
is full and nothing is on it".  A non-empty directory is REFUSED (-2);
recursion is policy and already lives in the VFS.  **THE BUG THAT MADE "IT
WORKS" A LIE:** `exfat_make` never checked whether the name existed, so boot 2
created a SECOND `/mnt/store` and the scan found the new empty one first —
every package rebuilt itself while `ls` showed a store full of packages.  Two
entry sets for one name is valid structure meaning something impossible: *a
filesystem that can create the same name twice does not have a namespace.*
**AND THE x86 SHELL HAD NO `rm`** — not an oversight: exFAT could not delete,
so there was nothing for it to do.  `rm [-r] <path>` now, because *a filesystem
you can only add to is not one you can use.*  **VERIFIED BY `fsck.exfat -n`
REPORTING `clean`**, on both x86 arches, after create → write → refuse-non-empty
→ delete file → delete directory, plus survival across a REBOOT — our own
reader agreeing with our own writer would only prove they share the same
misunderstanding.  **THREE STORAGE MODES ON EVERY RUN SCRIPT** (asked for
alongside it; same subject — what state a boot starts from): `--empty` (a disk,
freshly formatted: a first boot with nothing carried over) and `--no-disk` (no
storage; "will NOT survive a reboot" is the truth), default unchanged.  Parsed
BEFORE the arch branch so all three `run-<arch>.sh` wrappers and aarch64 behave
identically — ARM previously attached a disk only if one happened to exist and
nothing ever created one, so the arch where persistence is hardest to reach was
the one whose everyday run never had it.  `dos-shell-test.py` takes `--empty`
too, because *a test that reuses whatever the last run left behind depends on
the order the tests ran in.*  Each mode verified by what the GUEST saw, not by
what the script printed.  *One `set -e` trap: `[ cond ] && cmd` as a statement
is a FAILING command when the condition is false, and the script exits there.*
**OPEN:** `rename` (the last `NULL` in the ops table); `dir_is_empty` scans a
bounded 4096 entries; `pkg.store = disk` is genuinely usable now but stays
non-default on the §4.72 measurement (82 ms vs 7823 ms).

✅ **§M62 — THE BOOT SCREEN, AND THE FAULT THAT MUST REMOVE IT (2026-08-21, DOCS
§4.71).  THE §M58–§M64 DESKTOP-UX CLUSTER IS COMPLETE.**  `boot.splash` =
off/on/quiet.  The splash is **DRAWN, not loaded** (gradient + the 8×8 console
font expanded by an integer scale + the milestone label + a progress bar): no
file to be missing, no decoder to refuse, no allocation to lose — *the boot
screen is the one thing that must not be able to fail while the machine is
still deciding whether it works.*  **The log is SUPPRESSED, never discarded**
(klog + serial keep everything; `dmesg` verified).  **It starts after the disk
is mounted on purpose:** `boot.splash` lives in the PERSISTENT store and that
store IS the mount (§4.63), so an earlier call could only ever read the default
— and the long phase a splash exists to cover begins there anyway.  Any key
drops to the log.  **AND THE RULE THE FEATURE IS JUDGED BY: ANY FAULT TEARS IT
DOWN**, through ONE hook in **`crash_dump_begin()`** — the function every ring-0
dump, NMI report and panic already passes through, so a NEW fault path inherits
the behaviour instead of having to remember it.  **DEMONSTRATED, NOT ASSERTED:**
`splash faultkernel` raises the splash and dereferences an unmapped kernel
address in the SAME command (typing anything would have dismissed it first);
with `kernel.fault_policy=kill` the box survives and the screendump either
shows the report or shows a logo — **there is no third outcome, which is what
makes it a test.**  **FOUR BUGS, EACH FOUND BY THAT TEST:** (1) suppressing the
console SINK is not suppressing output — the per-task VC hook is the live path
after `vc_init`; (2) there is more than one screen sink (fb + VGA fallback,
both category "screen") and the first match was the INACTIVE one while the
other kept printing; (3) **`*(int*)0x4 = …` does not fault** — low memory is
identity-mapped — so the deliberate fault succeeded silently and the splash
looked "still up" for the innocent reason that nothing had crashed (*a test
whose success and failure look identical is not a test*); (4) handing the sinks
back is not clearing the screen — the first working report printed ON TOP of the
gradient, so `splash_abort` wipes the framebuffer itself.

🎨 **THE BOOT LOGO IS VECTOR ARTWORK (2026-08-22, DOCS §4.71).**  Asked from
use: *"could it be SVG rather than BMP — it has to look good at every
resolution?"*  It has to, and a bitmap cannot: sharp at exactly ONE size, and
§M61 made the resolution a runtime choice.  **`scripts/svg2paths.py` flattens
the SVG to POLYGONS at BUILD time; `kernel/gui/vpath.c` rasterises them at
whatever size the screen is** — 2.5 KB of points instead of a 786 KB bitmap,
size derived from the screen (measured: 600×600 at 1920×1200, 384×384 at
1024×768, same table).  **The split is the point:** a general SVG renderer in
ring 0 means XML, a path grammar, transforms, styles and a cascade — everything
that makes SVG general and none of what a logo needs.  The kernel side has NO
PARSER (it cannot fail on malformed input because there is no input), is
integer-only (§A2: no FP in kernel context), bounded in memory, even-odd filled
(so the "d" keeps its hole without winding data) and anti-aliased on purpose —
a logo IS its edges.  **TWO TRAPS, BOTH CAUGHT BEFORE BOOT:** the paths carry
`transform="translate(…) scale(1,-1)"` — a Y FLIP — and ignoring it does not
fail, it silently mirrors the logo (the converter parses transforms and REFUSES
unknown ones); and the first in-kernel render looked cut in half, which read as
a rasteriser bug but was a screendump catching it MID-DRAW (102 ms under
emulation).  The fix was not a cleverer rasteriser but not redrawing the whole
screen for a progress update — the logo is rasterised ONCE per boot now, and
its cost is logged, *because a boot screen whose own cost is unmeasured is one
nobody can defend when boot gets slower.*  `boot.splash` now defaults to **on**
and is listed in the Control Panel's **System** panel.  **TWO THINGS THE USER
NOTICED, BOTH REAL:** *"a few lines print before the splash"* — the setting is
on the DISK, so the decision waits for the mount while the console prints;
inverted now (**the screen goes quiet as soon as a framebuffer exists**, and if
the splash is declined the missed lines are **REPLAYED from klog**).  *That
replay took two attempts, both about who owns the screen:* at the decision
point it was drawn and then wiped by `vc_init` (which paints over the boot log
by design — *restoring output before the final owner exists is restoring it to
nobody*), and after `vc_init` it still showed nothing because the framebuffer
sink is deliberately inactive by then and the boot task has no console bound —
so it names its destination now (`vc_putchar` into the root VC).  And *"it
loads in visibly, top to bottom"* — the filler wrote rows straight into the
scanned-out framebuffer; it rasterises into a private surface and blits once
now, so the logo APPEARS rather than arrives (M22.6's page-flip argument, one
layer down).

⚡ **MOUSE WHEEL (both arches, same day).**  Reported from use: scrolling did not
work in the resolution list — and it could not, because the PS/2 driver decoded
the default 3-byte packet, which has no wheel.  The **IntelliMouse knock**
(200/100/80 sample rates) switches the device to 4-byte packets, and **the
device ID is READ BACK**: assuming the switch would shift every packet by one
byte and turn the pointer into noise.  ARM's virtio-input already reports
`REL_WHEEL` and needed only the plumbing (and the symbol, or the shared GUI
fails to link — *a link error on one arch is how an interface says it was half
implemented*).  The wheel is reported SEPARATELY from motion (routing it as
`dy` would move the cursor instead of scrolling what is under it) and the sign
was MEASURED, not assumed.

◐ **§M61 — CHANGING THE RESOLUTION WHILE THE DESKTOP RUNS (2026-08-21, DOCS
§4.70, x86; ARM declines).**  Block 5.  The resolution was **a constant in
assembly** (`dd 1920 / dd 1200` in the multiboot header, `FB_WIDTH 1280` on
ARM).  `mode 1280x800` now re-lays the running desktop.  The seam is the one
M21 already carved (`fb_present.h`), extended with
`fb_mode_count/get/current/set`: x86 drives the same Bochs-VBE DISPI registers
the page flip uses, so the BAR does not move.  **aarch64 DECLINES and says
why** — virtio-gpu needs a fresh CONTIGUOUS framebuffer (the buddy-order
ceiling M22.6 met), a new resource and a new scanout, each able to fail with the
display half-configured; **`fb_mode_count() == 1` is the interface's own way of
saying "this display cannot be asked to change"**, which beats an
implementation that works on the sizes someone happened to try.  Two x86 rules,
both about not ending at a black screen: **map the new frame BEFORE switching**,
and **read the geometry back** (the device CLAMPS what it cannot do, so
believing the write leaves the kernel drawing at a size the display is not
showing).  **THE MODE SET IS ONE CALL; THE WORK IS EVERYTHING ABOVE IT** —
backbuffer, wallpaper, panel strip, chrome layout, window clamping,
client-managed windows told through §4.60's resize event — done **on the
compositor between frames**, allocating before freeing so an OOM leaves a
working desktop.  **THE CONFIRM-OR-REVERT DIALOG IS NOT OPTIONAL:** a mode the
display cannot show is a black screen and nobody clicks "revert" on one — so
apply first, open a dialog IN the new mode with a **ktimer** countdown (never a
frame counter: a mode that shows nothing produces no frames), and make the
no-input outcome the safe one.  `gui.mode_confirm_s` (default 15, 0 = no
dialog); the shell has the same contract (`mode <w>x<h>`, `--force`, `mode
confirm|revert`) so **both** outcomes are drivable headlessly — *a revert
nothing can trigger on purpose is a revert nobody has tested.*  **VERIFIED BY
SCREENDUMP, WHOSE OWN SIZE IS THE EVIDENCE:** forced → a 1280×800 dump; queued →
dialog centred on the new screen; left alone → `reverted to 1920x1200` and a
1920×1200 dump; `mode confirm` → stays.  **THREE BUGS, EACH WORTH A SENTENCE:**
a dialog centred on the screen that no longer existed (the change is QUEUED, so
the requester sees stale state); **a window built on a task with no APP-HOST
loop never lays out and never ticks** (`gui_app_window_create` binds to
`task_current()` — new `gui_queue_open(fn)` is `gui_queue_launch` minus the
launcher entry); and **a save guard that was the exact inverse of its rule**, so
the geometry snapshot was taken in every case EXCEPT a provisional change — the
dialog counted down, said all the right things, and undid nothing.  *A guard
whose condition is backwards fails only in the case it was written for.*
**TWO FOLLOW-UP BUGS REPORTED FROM USE, BOTH REAL:** *"the countdown is very
slow, those aren't seconds"* — it was a VARIABLE decremented by a re-arming
timer and drawn by a separate ~2 Hz tick, so the number showed **how many tick
events had happened, not how much time had passed**, and under emulation
missed/doubled firings land straight in what the user reads.  It is a DEADLINE
now (`remaining = deadline - now` from §M53's clock; the timer only wakes the
repaint).  *A counter counts events; a clock measures time.*  And *"scroll
doesn't work in the resolution list"* — it did not: `scroll` existed in the
widget and in every view signature and **nothing ever changed it**, so items
below the fold were unreachable (no wheel to fall back on — this PS/2 driver
decodes the 3-byte packet).  The selection now carries the viewport, and a drag
past the edge scrolls.  **THE ROOT CAUSE UNDER IT IS THE ONE TO REMEMBER:**
keyboard navigation had NEVER worked in an item view because
`w_itemview_create` — hand-written instead of widget.c's shared `widget_init` —
never set `widget.win`, so `gui_window_focus_widget(w->win, w)` focused a NULL
window and every keycode was dropped.  The symptom was precise and misleading:
**the mouse worked and the keyboard did nothing.**  *A constructor written by
hand skips exactly the line nothing else needed.*  **OPEN:** aarch64 mode
setting; `gui.mode` is written on OK but not yet read at `gui_start`; the
multiboot header still asks for 1920×1200.  **NEXT: block 6 — §M62 boot
splash.**

◐ **§M58 + §M59 KERNEL HALF — TEXT SELECTION AND TWO CLIPBOARDS (2026-08-21,
DOCS §4.69).**  Block 4.  **NOTHING IN THIS SYSTEM COULD BE SELECTED WITH A
MOUSE, and the reason was structural:** `widget_ops.mouse` carried
`kind: 0 = click, 1 = double` and NOTHING ELSE — a drag is press/move/release
and had **no transport**, whatever a widget did.  Now: a `pointer` op
(PRESS/DRAG/RELEASE) plus a real **pointer GRAB** (from press to release the
stream goes to the pressing widget even after the pointer leaves it — without
which a selection stops exactly where a user drags to), with the grabbed widget
resolved on the HOST task and never carried through the IRQ ring (§M54's
lifetime class).  **TERMINAL SELECTION IS A RANGE OVER THE MODEL** (the cell
backing store), **linear in reading order, not rectangular** — from the middle
of one line to the middle of the next means the end of the first and the start
of the second; a rectangular selection is a different feature, not this one
with the wrong maths — with trailing blanks trimmed per row.  **Content clicks
had been gated on `kind == WIN_APP`, so a press inside a terminal reached
NOTHING AT ALL** — which is why every command's output, the text people most
want to copy, was the one thing that could not be selected.  **THE IRQ ONLY
RECORDS; THE COMPOSITOR WORKS:** a grid re-render is thousands of glyph blits
and `clipboard_set` allocates, so the mouse handler moves a cell range and sets
a flag (§M22.7's split).  **TWO CLIPBOARD SLOTS, because they are two
INTENTIONS:** a selection fills PRIMARY, Ctrl+C fills the clipboard — one slot
means every drag destroys what you deliberately copied.  **Middle-click pastes
the primary** into a terminal (raising it first, so text lands in the window
that was CLICKED, not in whichever had focus); `clip show|paste [primary]|copy|
promote` on both shells.  **Verified on i386 by screendump + serial:** drag →
cells highlighted + `gui: selected 27 byte(s)`; `clip paste primary` prints
exactly those bytes; middle-click types them at the prompt.  *And the pointer op
went into the MIDDLE of `widget_ops` first — every ops struct in the tree is a
POSITIONAL initialiser, so each silently re-bound by one slot; new optional ops
go at the END, now written in the header.*  **REPORTED FROM USE THE SAME DAY — *"I can't manage with the
clipboard, the selection doesn't work either"* — AND BOTH WORKED IN THE TEST:**
the test pasted with the **MIDDLE BUTTON, which a trackpad does not have.**
*A feature whose only trigger is hardware the user does not own is, from where
they sit, a feature that does not exist.*  Added the keyboard route people
actually reach for: **Ctrl+Shift+C / Ctrl+Insert copies, Ctrl+Shift+V /
Shift+Insert pastes** (middle-click still pastes the primary).  **SHIFT is
load-bearing** — plain Ctrl+C stays the INTERRUPT; making it "copy when
something happens to be selected" would put the most important key on a terminal
at the mercy of invisible state, which is why every terminal emulator chose this
binding.  A paste prefers the explicit clipboard and falls back to the primary.
**And the bindings are now DISCOVERABLE** (the confirmation line and `clip` both
name them) — *a binding nobody can find is a binding nobody has.*  **The editor
selects with the mouse too** now, through the same `ed_move_to(off,
keep_anchor)` Shift+arrow uses, so the two cannot drift.  **OPEN:**
scrollback-anchored selection, and §M59's larger half — typed offers, the ring-3
ABI surface and Wayland `wl_data_device`.  **NEXT: block 5 — §M61 resolution switching.**

⚡ **BOOT TIME, MEASURED (2026-08-21, DOCS §4.68).**  Asked from use: *"what is
this self-test at boot, it looks slow?"*  It was — and the measurement and the
intuition disagree about where the rest goes.  i386 `-smp 4`, QEMU start →
`[pane 1 ready]`: **7.46 s → 5.81 s**.  The two self-tests were **1.64 s (22 %)
and every millisecond of it a FIXED SLEEP, not work** (each ran its hogs a flat
500 ms, then waited a flat 100 ms for them to exit).  They STAY — they are the
only thing that would notice preemption or SMP breaking on a machine nobody
tests, and *a check nobody runs is a comment* (§M52) — but the window is
`kernel.selftest_ms` now (default **150 ms** = three full quanta at
`SCHED_QUANTUM_TICKS`=50 @1000 Hz, so a hog still cannot fail to be preempted
inside it; 0 skips them).  **preempt 0.88 → 0.32 s, parallel 0.76 → 0.19 s.**
*The fix had a bug that only measuring again found:* the flat drain first became
a `task_wait()`, and the parallel test barely moved (0.76 → 0.70) because **init
is an always-on universal reaper** (§M27) — it often collects these tasks first,
and waiting on a child somebody else reaped never completes, it just burns the
bound.  Polling for the task to be GONE: 0.19 s.  **THE HONEST HEADLINE: the
self-tests are no longer where boot goes — ~80 % is GRUB reading a 61 MB kernel
image** (NetSurf + Mesa + musl + coreutils + libstdc++ + the 3.6 MB wallpaper).
That is the next target if boot time matters, and it is a PACKAGING question,
not a kernel one.

🔧 **§M31 L3 FIX — THE SAFETY NET REBOOTED A HEALTHY MACHINE (2026-08-21, DOCS
§4.67).**  Reported from use: *"x64 crashes, fix it now, this should not
happen."*  It should not — and it was worse than a program crash: the box
rebooted itself while working correctly.  **THE EVIDENCE WAS ALREADY ON DISK**
(`run_qemu.sh` keeps COM1 at `/tmp/dos-serial.log`): `!! NMI HARD-LOCKUP
rip=0x1795af` right after `pkg: installed musl`, then §M47's `PREVIOUS BOOT
ENDED UNCLEANLY`.  `dos-sym.sh` maps it to **`hal_cpu_halt+0x9`** — the NMI
landed on an IDLE CPU, so nothing was wedged.  **THE BUG IS THE WATCHDOG'S
PREMISE, NOT THE CODE IT WATCHED:** the ib700 was petted ONLY by the watchdog
TASK, whose argument is *"if I cannot run, the scheduler is wedged"* — FALSE
during boot, where `pkg_init` copies megabytes and every console line at
1920×1200 scrolls the framebuffer under §M57's preempt-disabled print lock
(~9 MB memmove per line at §4.61's measured ~43 MB/s).  On a loaded host one
stretch passes four seconds while the system is making perfectly good progress,
and the net kills the healthy patient.  **FIX A (symptom): pet from the TICK on
the BSP** — a real hard lockup is interrupts dying, so the tick stops, the pet
stops and the NMI still fires; BSP-only on purpose, since healthy APs must not
mask a wedged one (that case is the softlockup sweep's, and it REPORTS rather
than reboots).  **FIX B (cause): provisioning yields** (`write_file`/`copy_file`
in pkg.c) — nothing that runs for seconds should hold a CPU without offering it
up.  **AND THE NET IS RE-TESTED, because a loosened safety net that nobody
falsified is one nobody can trust:** new **`hardlock`** (both shells) pins to
CPU 0, disables interrupts and spins — verified on i386 AND x86_64 at -smp 1
and -smp 4 that the NMI fires and the kernel-mode path reboots, and that a
boot under deliberate host load now reaches the desktop.  *The test found its
own bug first*: it yielded once after setting affinity and assumed the
migration had happened, wedged some other CPU at -smp 4, and read as "the net
is gone" when it was the test that had missed.  **Also: the NMI report is now
bracketed by §M54's `crash_dump_begin/end`** — several CPUs taking the alarm
interleaved character by character, and the unreadable output cost real time
diagnosing the very bug this path exists to report.

✅ **REPORTED FROM USE, FIXED (2026-08-21, DOCS §4.66).**  (1) **A DEFAULT
PICTURE SHIPS WITH THE SYSTEM** — `assets/wallpaper-default.bmp` embedded,
written to `/usr/share/wallpapers/default.bmp` on first `gui_start`, and
`gui.wallpaper` defaults to that PATH (not a magic value: the shipped picture
is an ordinary file the user can replace or delete — *an embedded default that
cannot be replaced is a hardcoded background wearing a config key*).  Blob
symbols are WEAK, so a tree without `assets/` still links.  (2) **THE EVERYDAY
RUN HAD NO DISK — THE THIRD TIME THIS SHAPE HAS APPEARED.**  A user reported
that the keyboard layout "only saves to RAM"; it did, because `run_qemu.sh`
attached no disk, so the path a PERSON uses had no writable volume while every
test supplied its own (§M48's missing NIC, §M49's missing `-smp`, now this).
The script creates a 64 MiB exFAT image on first use and attaches it with
`-boot d`.  **So there are two modes and the system says which it is in:** with
storage, `persistent store /mnt/d-os.conf created|loaded` + *"saved … (survives
reboot)"*; without it everything works, nothing persists, and every save says
so — `DOS_DISK=none` selects that mode deliberately, because *a path nobody can
run is a path nobody tests*.  (3) **HUNGARIAN LETTERS EXIST.**  The font was
`font8x8[128]` and `layouts.c` mapped the accented vowels to **0** rather than
lie about what it produced (accurate since M16).  The table is 256 entries now
and the upper half is **ISO-8859-2 (Latin-2), NOT Latin-1** — forced, not
stylistic: **ő and ű do not exist in Latin-1**, so a Latin-1 font would render
seven of nine vowels and silently drop the two most characteristic of the
language.  The 18 glyphs are DERIVED from their base letters (mark in the free
top row; capitals shifted down one row, free because row 7 is empty for every
capital here), which is what keeps them the same typeface.  Verified: `setlayout
hu` + the `;'[]\\` key positions → `e9 e1 f5 fa fb` on the wire and é á ő ú ű
on screen.  **Also:** `widget_ops` gained §M58's `pointer` op, and the first
attempt put it in the MIDDLE of the struct — every `widget_ops` in the tree is
a POSITIONAL initialiser, so each silently re-bound by one slot.  *New optional
ops go at the END*, now written in the header.

✅ **§M63 — THE CONTROL PANEL: TWO REGISTRIES, AND SETTINGS WITH NO UI CODE
(2026-08-21, DOCS §4.65; stage 0 in §4.63).**  Block 3.  §M60/§M61/§M62 each
ended with *"…and a UI for it"*, and the launcher could not have taken three
more entries — **`SM_MAX_APPS` was 10 with 10 apps registered, and the cap
SILENTLY DROPS the overflow**, so an eleventh app reads as a broken
registration rather than a full menu (12 now, failure mode written down).  So
the container is not a nicety that comes after the settings: *it decides
whether adding a setting is a LINE or an APP.*  **TWO REGISTRIES:**
`SETTINGS_PANEL()` for pages that need real UI (they ship next to the code they
configure — `controlpanel.c` NAMES NO SETTING, it walks the registry and hands
it to an item view), and **`CONFIG_KEY()` descriptors** (key, group, type,
values, default, help) so ONE generic panel renders every plain setting and
**most settings need no UI code at all**.  Twelve keys declared today, **eleven
of which had been discoverable only by reading source** (`kernel.fault_policy`,
`crash.report`, `gui.close_grace_ms`, `bus.allow-adaptation`…).  **THE
DECLARATION LIVES NEXT TO THE CODE THAT READS THE KEY** — `gui.wallpaper` is
declared in `wallpaper.c`, three lines below its reader; otherwise the panel
accumulates knowledge of every subsystem, which is the thing being avoided.
**DESCRIPTORS BUY VALIDATION, NOT JUST RENDERING:** `conf set
gui.wallpaper_fit nonsense` → *"not a valid enum (fill stretch center tile)"*,
while **`setconf` stays deliberately unvalidated** because it must still reach
undeclared keys.  Panels open as their OWN §M22.7 windows, not pages in one —
the app that changes display modes and keyboard layouts is the one most able to
wedge.  Everything a panel writes goes through `config_apply`, so stage 0's
watchers fire and the subsystem re-reads immediately.  New `w_itemview` widget =
the window-side half of §M64's item view (layouts stay stateless; the widget
owns selection + scroll + arrow/Enter navigation).  **Verified by screendump on
i386** (ONE Start-menu settings entry → three categories from the registry →
double-click Personalisation → a window listing exactly its five declared keys
→ **Cycle changes `gui.wallpaper_fit` from `fill` to `stretch` live**) **and
headlessly on all three arches** (`conf list` = 3 panels / 12 keys, invalid
enum refused, valid one accepted).  **OPEN:** the Display panel (that is §M61,
which owns the mode-setting code), Packages (§M45 should REGISTER rather than
become a separate app), a path picker for `CFG_PATH`.  **NEXT: block 4 — §M58
text selection + §M59 clipboard.**

✅ **§M64 — ICONS, A SWAPPABLE ITEM VIEW, AND DESKTOP SHORTCUTS (2026-08-21,
DOCS §4.64).**  Block 2 of the desktop-UX schedule.  Three things at once
because each is useless without the one below.  **THERE WAS NO ICON ANYWHERE IN
THIS SYSTEM** (the only graphic with a shape in it was the 8×8 font), so
`icons.c` **DRAWS** sixteen glyphs from gfx primitives rather than shipping
bitmaps: one definition serves 24/48/64 px, there is no per-arch objcopy blob
plumbing (§M47.5's shape), and an icon cannot fail at runtime — no file to be
missing, no decoder to refuse.  The trade (geometry, not artwork) is in the
header with the seam that replaces it.  **THE LAYOUT IS NOT THE WIDGET** — the
piece the user asked for: a **MODEL** (count/get/activate, knows nothing about
pixels) plus a stateless **VIEW** registered with `ITEM_VIEW()` and chosen BY
NAME from config (`desktop.view` = grid | list), because the desktop, §M63's
control panel and the file manager are three views of one idea and writing it
three times makes "I want a list instead of icons" a rewrite in each.
Selection/scroll live in the viewer, not the view, so views stay stateless; the
draw call takes a SURFACE + ORIGIN, not a window, because the desktop paints on
the compositor's back buffer.  **A SHORTCUT IS A FILE** (`/desktop/*.lnk`, the
config format — one text format in this system, not two), so `ls`/`rm`/fileman
already work on it and it survives a reboot on the exFAT mount; four target
kinds behind ONE resolver (`app:` by NAME so it survives a rebuild, matched
loosely so `app:Task-Manager` finds "Task Manager"), and the unimplemented kinds
SAY so instead of doing nothing quietly.  **THE COMPOSITOR GAINED A BACKGROUND
LAYER, NOT A CHROME HOOK** — `desktop_shell.draw_under`, painted after the
wallpaper and under every window, because the one-line alternative (the existing
`draw`) puts a shortcut on top of every application; `desktop_click` is
dispatched **WITHOUT the WM lock** since activating a shortcut spawns an
app-host task, and double-click detection stays in gui.c next to the title
bar's so the two cannot drift.  Selection damages TWO CELLS, not the screen
(§4.61).  **Auto-population from the GUI_APP registry is deliberately absent** —
a desktop that re-creates icons the user deleted is the most-complained-about
behaviour of every system that has tried it.  **Verified by screendump on i386**
(five labelled icons; click selects; **double-click opens NetSurf in a real
window**; `setconf desktop.view list` gives the same items as rows) **and
headlessly by `shortcut check`**, which prints a layout checksum AND the
hit-test answer — *a view that draws correctly and hit-tests wrongly is
invisible in a screenshot*.  **THE BUG WORTH KEEPING:** the reload loop tested
`vfs_readdir(...) == 0`, but this VFS returns **>0 per entry, 0 at end** — so
`shortcut add` succeeded, `shortcut list` showed an empty desktop, and both
halves looked right in isolation.  *Copy an existing caller's convention
(`cmd_ls`, ten lines away) instead of assuming one.*  **OPEN:** drag-to-move
(the file format has x/y and `shortcut_set_pos` writes them, but a drag needs
§M58's press/motion/release), keyboard navigation, "Send to desktop" in the file
manager, `run:` targets (want a terminal window that accepts an initial
command).  **NEXT: block 3 — §M63 Control Panel proper.**

✅ **§M63 STAGE 0 — SETTINGS THAT SURVIVE A REBOOT (2026-08-21, DOCS §4.63).**
The bug was invisible and total: `config_save()` wrote `/etc/d-os.conf` and
**`/` is ramfs** (the persistent volume is exFAT at `/mnt`), while the ordering
forbade anything else anyway — `config_init()` runs at `kernel.c:144` because
half of boot reads config, and the mount is ~125 lines later at
`kernel.c:269`.  So **every setting anybody ever saved was written into memory
and lost**, while the shell printed `config saved.`  Now
`config_attach_persistent("/mnt")` runs right after the mount **on BOTH entry
paths** (x86 `kernel_main` AND aarch64's own `main_entry.c` — miss one and that
arch keeps losing settings while the other keeps them), it returns success only
when the file was actually READ or CREATED (*creating it is the only honest
test that the volume is writable; a path we merely HOPE is writable turns every
later save into a silent failure*), and `saveconf` names the file — or says
`will NOT survive a reboot (no writable volume)` in those words.  **THE FILE
COPY WAS THE EASY HALF: `CONFIG_WATCH()`** (a linker-section registry, the
`GUI_APP()`/`SERVICE()`/`CRASH_SINK()` shape) is what makes it a feature — a key
read at BOOT has already been acted on by the time the store is overlaid, so a
saved `keyboard.layout = hu` would apply **one boot LATE**, the file saying `hu`
while the machine types `us` and nothing anywhere explaining it.  `config_set`
(fill the cache) and **`config_apply`** (record a decision → notify) are
deliberately different calls, and watchers fire only on a REAL change —
otherwise the defaults table would fire a callback per key at every boot before
any subsystem exists.  First two watchers: keymap and wallpaper.  **Verified
across a real reboot on i386** (`created` → `loaded`, both keys intact, the
layout applied at boot), and with no disk attached the honest-failure path.
*The clearest evidence was an accident:* on the boot that loaded `hu`, the
harness's next command arrived as `lslazout` — QEMU sends key POSITIONS and the
guest really was typing Hungarian.  Which is also a standing warning: **a
persistent store makes test runs STATEFUL** (re-make the image between runs).
**TWO ONE-ARCH-ONLY FEATURES FOUND ON ARM, same shape:**
`setconf`/`getconf`/`saveconf` lived in `shell.c`, so aarch64 (own
`serial_shell.c`) could create a persistent store and then had **no command
able to write to it** — all three moved into `config.c` (`config_cmd_*`), both
shells call one copy (§M24's rule, applied to the code that was breaking it);
and the **harness honoured `--disk` on x86 ONLY, silently** — the flag was
accepted, the ARM guest never saw a disk (on `-M virt` the drive must be
attached to a virtio-MMIO slot explicitly), so anything needing storage
"failed" there for no visible reason.  ARM round-trip now verified end to end.
Harness also fixed to pass `-boot d` with `--disk` — a formatted image
otherwise makes SeaBIOS boot the disk, and the guest produces NO serial output
at all.  **NEXT: block 2 — the icon primitive + an abstract item view (model +
swappable grid/list, per the user's request) + §M64 desktop shortcuts.**

✅ **§M60 — THE DESKTOP BACKGROUND BECOMES A SOURCE (2026-08-21, DOCS §4.62, all
3 arches).**  Block 1 of the §M58–§M64 desktop-UX schedule (PLAN "Execution
order").  The background was ONE `gfx_vgradient` call in `gui_start` — not
configurable, not changeable, not a thing anybody could point at.  Now
`gui.wallpaper` = `gradient` | `solid:RRGGBB` | a BMP path and
`gui.wallpaper_fit` = fill/stretch/center/tile, changed at runtime by
`wallpaper` — a command that lives in **`kernel/gui/wallpaper.c`, not in a
shell**, so the ARM serial REPL runs the same implementation (§M24's rule).
**TWO CONTRACTS FIXED UP FRONT because both are painful to retrofit:** a render
**always leaves the surface fully painted** (missing file, bad format,
truncated read, malformed hex → gradient + the REASON, in the status line and
in klog: *a desktop that will not start because an image moved is worse than a
gradient, and a silent black screen is worse than either*), and **decoding
never holds the image in memory** — rows stream from the file straight into the
destination, so a 1920×1200 wallpaper costs ONE source row (~7.7 KB) instead of
9 MB of pixels plus a 6.9 MB file buffer.  ONE format on purpose (uncompressed
BMP, 24/32 bpp, both row orders); richer codecs belong in ring 3 where §M42
already ships `nsgif`/`nsbmp` — *a kernel-resident image decoder is an attack
surface with a mouse attached to it*.  Three BMP traps handled, each producing
a plausible-looking wrong image if missed (a POSITIVE height means BOTTOM-UP
rows; rows are PADDED to 4 bytes; a 32 bpp alpha byte is usually ZERO), and
`BI_BITFIELDS` is REFUSED rather than assumed — it *usually* carries the same
channel order, and "usually" is not a format.  **THE VERIFICATION IS THE PART
WORTH KEEPING: `wallpaper check` renders the current config into an OFF-SCREEN
surface and prints corner pixels + an order-sensitive checksum**, because the
aarch64 harness passes NO display device and a screendump there is not
awkward but impossible — i386, x86_64 and aarch64 return **byte-identical
checksums** for fill/center/tile, which no screenshot could establish.  The
test image is generated BY THE GUEST (`wallpaper testimg`) so it travels through
the real VFS and decoder, patterned to make failure obvious (two-axis ramp,
1-px border, red block in the TOP-LEFT ONLY — which is what proves row order).
Screendump-verified too on both x86 arches, including `wallpaper solid:…` typed
**while the GUI runs** (the live path §M63's panel will call).  **TWO PROCESS
FINDINGS:** a status line that answered "gradient" while the config held a
picture (*a lie with a straight face* — it now says "not rendered yet —
configured: …"), and **the aarch64 harness had silently stopped typing**: its
default boot marker still matched `serial shell ready`, which `serial_shell.c`
has not printed in a long time, so every ARM run failed the boot wait, typed
NOTHING and produced a log that reads like a healthy boot.  *A harness that
quietly stops driving the guest is worse than one that crashes.*  **NEXT (block
1b): §M63 stage 0 — settings do NOT survive a reboot** (`config_save()` writes
`/etc/d-os.conf` on ramfs; `config_init()` at kernel.c:144 runs 125 lines before
the exFAT mount at kernel.c:269).

✅ **A DRAG IS A COPY NOW, NOT A REPAINT (2026-08-21, DOCS §4.61).**  Reported
from use: *"a little lag when I drag a big window."*  The compositor had
damage-rect counters but no TIME, and lag is a claim about duration — so
`compose()` now accumulates its own nanoseconds and a MOVE drag prints a
summary when it ends (`gui.drag_stats`).  **MEASURED FIRST:** the same 40-step
drag costs 338 ms of compose on a 240×130 window and **1630 ms on a 921×721
one — 36 ms per composite against a 30 ms frame budget**, i.e. one composite
takes longer than the interval between frames and the window necessarily trails
the pointer.  Not a stall, a fill-rate wall (~43 MB/s of software compositing).
**TWO CANDIDATE SAVINGS WERE MEASURED, NOT ASSUMED** — skipping the dragged
window's shadow is worth 17%, skipping the wallpaper under an opaque window is
worth 2% (during a drag the damage rect is the window UNION the vacated strip,
so it is never fully covered: a real optimisation that does not apply to the
case that hurts) — and both were reverted in favour of the structural fix.
**A MOVING WINDOW'S PIXELS DO NOT CHANGE**, so the composited image is COPIED
inside the back buffer and only the leftovers are painted: **1630 → 986 ms, 22
ms per composite, under budget.**  The move is passed OUT OF BAND (a
`move_hint`) rather than as damage, which is what makes it safe: the copy path
runs only when the damage list is otherwise EMPTY, so anything else that
changed falls back to the painter — inspecting merged damage rects could not
tell a window that MOVED from one that moved AND redrew, and the failure mode
of guessing is a stale image nobody can explain.  **FOUR RULES, EACH A WAY TO
GET IT WRONG:** copy FIRST and paint second (the vacated strip and the cursor's
footprint are INSIDE the source, and the painter draws the window already
moved); the window must be TOPMOST (anything above would be dragged along);
**the cursor comes along for the ride** (`draw_cursor` paints into the back
buffer, so the copy deposits a second cursor at old+delta — one small rect
repainted after); and DIRECTION matters (`gfx_move_within` picks row/column
order from the sign of the move, where a plain blit would read pixels it had
already overwritten).  **THE REPORT COUNTS BOTH PATHS ON PURPOSE:** `40 copied,
0 repainted` would mean the fallback is never exercised and therefore never
tested — dragging the self-refreshing Task Manager gives `37 copied, 3
repainted`, both paths in one drag, image correct.  Verified by SCREENDUMP on
i386 + x86_64: a window dragged off another reveals it cleanly, a window
dragged partly off-screen survives the clipping, NetSurf keeps its page and
chrome.  *And the instrument's own first version reported 15 µs per frame for
megabytes of blitting, because the accumulation sat BEFORE the draw and present
passes — a measurement placed on the wrong side of the work does not understate
it, it reports the work as free.*

✅ **NETSURF RESIZES ITS CONTENTS NOW (2026-08-16, DOCS §4.60).**  Reported from
use: *the window grows, the page inside stays small.*  Only client-managed
(dosgui) windows were affected, and the reason is structural: a resize allocates
a bigger content surface and then needs SOMEBODY to refill it — a terminal
re-renders its grid, an app window's host consumes `layout_pending` — but a
dosgui window has `host_task` cleared by design (§M54), so nothing consumed it
and **the bridge had no event that could carry a size at all**.  Fixed with
`dosgui_event` type 4 = RESIZE, reported the way `close` already is: by
comparing window state in `dosgui_poll` rather than queueing, because a resize
is a LEVEL not an edge — a drag produces fifty of them and only the last is
true.  `libnsfb_dos.c` turns it into `NSFB_EVENT_RESIZE`, after which
**everything already existed upstream** (fbtk → `gui_resize()` → realloc +
re-layout): the vendored tree needed no patch, because a framebuffer frontend
normally runs on a screen and nobody had ever sent it the event.  **AND
`dos_set_geometry` HAD TO START REALLOCATING** — it changed the dimensions and
left the buffer alone, harmless only while nothing could change them; the first
real resize would have plotted past the end of the heap block.  *A latent bug is
a bug whose trigger has not shipped yet.*  **Verified by driving the mouse**
(`--monitor-cmd` in the test harness: home the pointer, walk to the grip in ≤90
px steps because the PS/2 delta is a signed byte, press, drag, release,
screendump) — and the first attempt missed the grip by 23 px and proved nothing,
so the window's rectangle is now MEASURED out of the before-shot instead of
derived from constants.  Result: 795×579 → 1089×793 with the page text
REFLOWING, which a scaled image would not do.

✅ **§M24 COMPLETE — A NETWORK THAT CAN HOLD MORE THAN ONE CONVERSATION
(2026-08-15, DOCS §4.59, all 3 arches).**  §M24's first stages shipped a NIC and
a TCP/IP stack in July; §M55 made waiting free; §M56 built poll and epoll —
**machinery for watching many descriptors at once, over a transport that held
ONE TCP connection in one file-scope struct**, could not accept an incoming one
at all, and forgot every byte the moment it left.  NOW: a bounded **connection
table** (32 entries, four-tuple demux with the LISTEN entry as the FALLBACK
match — the order is load-bearing, since a listener and its accepted children
share a local port), per-connection **receive RING** (so the free space it
implies IS the advertised window, instead of a constant that stopped being true
at 16 KiB) + **send buffer** with segmentation, RFC 793 states through
TIME_WAIT, **listen/accept** with a bounded backlog, and an **RST for a segment
belonging to no connection** — which is why a closed port now fails fast instead
of at the timeout.  The table is STATIC because connections are created on the
RX path under the stack lock with interrupts off, where an allocator call would
nest the heap's lock inside the network's on the one path that must not fail.
**A LOOPBACK DEVICE MADE ANY OF THIS TESTABLE** — the server half cannot be
reached through SLIRP without a hostfwd rule the automated runs do not have, and
a test that needs the host's network reports the host's network.  It is a real
device (full IPv4/TCP output path → queue → `net_rx`; the queue is what stops
transmit from re-entering the stack with a lock on every frame), and `lo drop
<permille>` makes it lose frames ON PURPOSE: *the retransmit timer was written
AFTER the link learned to lose things, because a timer nothing can falsify is
not a feature.*  `tcptest` was likewise checked against a deliberately shrunken
table (2 entries → 0/4 clients, FAIL) before being believed about the real one.
**THREE BUGS, EACH PRESENTING AS "THE NETWORK IS SLOW":** (1) an ACK that only
WIDENS the window carries the same acknowledgement number, so reacting to it
only inside the "new data acked" branch left the sender stopped; (2) a
zero-window probe that OCCUPIES a sequence number digs a hole exactly when the
receiver has no room to fill it, and only the RTO repairs it; (3) **a FIN whose
sequence number was inferred from `snd_nxt`** — a retransmission rolls
`snd_nxt` BACK, after which an ACK covering only the data satisfied "have they
acknowledged my FIN?": sender in FIN_WAIT_2, receiver still ESTABLISHED, every
byte delivered, and the reader blocked until its timeout.  *Do not derive a
fact from a value that moves — record the FIN's own sequence number.*  **WHAT
FOUND ALL THREE WAS INSTRUMENTATION, NOT INSPECTION:** splitting the test's wall
clock into time-in-recv / asleep / in-send turned 8.5 s of mystery into "the
33rd read waited 8 s", and printing the connection table AT THE MOMENT OF THE
STALL named the last one in a single line — after two theories the measurements
had already excluded.  Also: **a counter that lives on an object cannot measure
a period longer than the object** (the loss test summed per-connection retransmit
counts and read 0 while 7 timeouts had fired).  **THE SOCKET ABI IS §M50
OPERATIONS NOW** — 15 canonical ops, ONE `sockaddr_in` marshaller for three
arches (and unlike `epoll_event` it really is the same everywhere, which is
worth stating so nobody hunts for the per-arch case), i386's `socketcall(102)`
reduced to a demultiplexer into those same handlers, the per-arch copies DELETED
(§M56.1's rule), and `shutdown` promoted from `return 0` to a real half-close.
**i386's socket numbers are not sequential by name** (360 is `socketpair`,
between `socket` and `bind`) and reciting them put `connect`'s handler on bind's
number — the symptom was a bind failing with ECONNREFUSED, the one errno that
names the handler that actually ran: *syscall numbers are data; copy them, do
not recall them.*  Plus **DHCP** (address/mask/router/nameserver, T1 renewal via
a §M49 worker because a ktimer callback may not send and wait), **/proc/net/**
{dev,arp,route,tcp,stat} (procfs learned one-level subdirectories), and **a
virtio-mmio NIC for aarch64** — the open item that had let every network feature
ship untested on a third of the targets.  **MEASURED on i386 + x86_64 +
aarch64: 8 concurrent connections; 32 KiB through a 10 % loss link intact and in
order; bind/listen/accept/getpeername/shutdown through an UNMODIFIED musl
binary.**  New: `tcptest`, `tcploss`, `netstat`, `lo drop`, `dhcp` (in BOTH
shells — they live in `kernel/core/net_cmds.c`, not in a shell, because a test
that lives in one shell can only run on the arches that build it), and
`scripts/dos-shell-test.py` (boot headless, type commands, capture serial — the
harness every earlier milestone rebuilt by hand).  **AND IT FOUND A LIVE REGRESSION IN THE TWO MILESTONES BEFORE IT** — the one
the user actually reported ("web pages do not come up"): **§M55** made netd run
only while somebody waits for the network; **§M56** made a finite `poll()`
timeout a REAL wait.  Neither knew about the other, so **a task polling a
SOCKET was a waiter nobody counted** — the poller stayed parked, no frame was
collected, and the wait ran to its deadline with the answer sitting in the NIC.
musl's resolver sends and then polls, so **nothing in ring 3 could resolve a
name** (`wget`, NetSurf's fetcher) while `nettest` passed throughout, because
the KERNEL resolver waits through `net_wait_cond` and counts itself.  *Two
changes, each correct, each verified, composing into a bug that only the path a
PROGRAM takes could reveal.*  Fixed by two contracts: a readiness wait that
touched a socket registers as a network waiter (`net_waiter_enter/leave`), and
a pump that delivered frames wakes the readiness queue too — **after** dropping
the stack lock, since a poll waiter holds the readiness queue while its scan
takes the stack lock and the other order deadlocks.  Verified end to end: HTTP
and HTTPS (TLS 1.3) through musl `wget`, and NetSurf fetching example.com
(`HTTP 200, 571 body bytes`).  **OPEN:** no reassembly queue
(out-of-order segments are dropped + dup-ACKed), no congestion control (the
peer's window is the only limit), a fixed 200 ms RTO (an estimator would measure
the emulator), the ARM NIC is polled, and `sendmsg`/`recvmsg` stay per-arch
(their `msghdr` is guest-width words, not a fixed address).

✅ **§M57 — A TASK IS NOT DEAD WHILE IT IS STILL RUNNING (2026-08-12, DOCS
§4.58, all 3 arches).**  §M54's three open items, closed.  Its own notes ended
with *"the reap sweep still reports a queued task roughly once in several
hundred kills"* — **a confession, not a measurement**, and it stayed one for two
milestones because the only evidence was a log line arriving long after the
event, from the subsystem that merely NOTICED first.  **THE INVARIANT IS NOW
STATED IN CODE** (`task_rq_audit`, six rules) and driven by **`rqcheck`** +
**`schedstorm`** — with rules 5 (ready-but-unqueued) and 6 (`rq_load`) counted
SEPARATELY, because both have legitimate transients and folding a briefly stale
estimate into "the runqueue is corrupt" is exactly how a checker gets ignored.
**`cpu_home` WAS A HINT BEING USED AS A FACT:** documented since M18.6.1 as
"which CPU's rq this task lives on", it was assigned by CALLERS at moments when
they merely INTENDED to place a task, under whichever lock they happened to
hold — so it could name a queue that did not hold the task, and four sites then
mutated a ring while holding the WRONG CPU's lock (`rq_rotate_to_tail_locked`;
`task_set_affinity` and `task_set_nice`, **both reachable from the shell**; and
the steal window).  It is an **OWNERSHIP TOKEN** now: claimed by CAS from -1
inside `rq_insert_tail_locked`, released at the END of `rq_remove_locked`, both
under the owning queue's lock, with one sentence covering every reader —
*holding queue N's lock while `cpu_home == N` is exclusive permission to touch
that task's links*.  §M54 tried the RELEASE half alone and it HUNG TASKS: a
queued task carried -1 forever so every detach silently did nothing — **half of
a two-sided invariant is worse than neither half.**  Migration is one transition
under BOTH rq locks, ascending `cpu_index` (the only nesting in the file).
**THE RESIDUAL WAS NOT A CORRUPTED RING AT ALL.**  The first diagnostic printed
the link state AFTER the sweep had removed it — identical for every possible
cause, hence saying nothing, and it cost a round of wrong theories; captured
under the lock and BEFORE the removal it read `home 3, next=set prev=set
head=self state=2`: properly linked, cpu_home agreeing, **state 2 = DEAD**.
`task_exit_code` marked itself DEAD at the top and then did a great deal of
PREEMPTIBLE work, but `pick_next_local_locked` picks only RUNNABLE tasks — so
**from that store the task was unschedulable while still executing with
interrupts on.**  One timer preemption and it was switched away forever: never
reaching its own `rq_purge_all`, left in a runqueue as a corpse, and freed by
the reaper with a live frame on its stack.  *A task is not dead while it is
still running* — DEAD, the joiner wake, the sweep and the final switch are now
ONE indivisible step with interrupts off, and `this_cpu()` is re-read there
(the value from function entry can name a CPU the task has since migrated off,
and scheduling on another CPU's runqueue puts two CPUs on one stack).
**MEASURED: 2880 kills → 0 `STILL QUEUED` (was ~1 per 200–300); ~420k churn ops
→ 0 structural violations.**  **THE FALSIFICATION MATTERS MORE THAN THE PASS:**
the first `schedstorm` drove affinity from ONE task and reported `ok` even
against the pre-fix code — the only thing it could race was the 100 ms balancer,
so a 240 ms run offered ~2 chances at a window measured in instructions.  *A
test that cannot fail is not evidence.*  With four concurrent churners it takes
the pre-fix kernel down with an **NMI hard-lockup** and passes on the fixed one.
**AND THE LOG ITSELF WAS BROKEN:** `printf.c` still said *"Not reentrant; fine
because the kernel is single-threaded today"* — true when written, false since
§M18, **the §M52 shape exactly** (a comment cannot fail a test).  Two CPUs
interleave CHARACTER BY CHARACTER, and every automated check in this project is
a grep over the serial log, so **one PASSING `killstorm` was read as a frozen
shell on that evidence alone** — *a harness that silently loses output is worse
than no harness, because it is trusted.*  Output is serialised now with
**PREEMPTION disabled, not interrupts** (a framebuffer scroll moves megabytes;
IRQ-off across it drops ticks and trips the softlockup watchdog — a logging path
that makes the machine look wedged is a poor trade for tidy output) and with
same-CPU re-entry detected BEFORE the acquire rather than waited on (asking "do
I already hold this?" after `spin_lock` has blocked is a question whose only
answer is a hung machine).  Also: **`usock_set_owner` had no prototype** since
§M56.2 — the compiler assumed `int f()`, correct by LUCK on today's arches;
stale `§M57` comment labels that belonged to §M56.1 relabelled (one label
meaning two unrelated things is a comment that misleads later); two warnings
cleared so the build is silent and a real one cannot hide in the noise.
**OPEN:** `AARCH64_MAX_CPUS` ships at 2 (ARM verification is real SMP at two
cores); `load_balance_pull`'s migration counter counts an attempt whose insert
can still be refused (diagnostic only).

✅ **§M56 — A WAIT THAT IS REALLY A WAIT (2026-08-11, DOCS §4.57, all 3
arches).**  **`poll(2)` with a positive timeout was treated as a SNAPSHOT** —
documented as such, which made it sound conservative.  It is not: a program
asking to wait 200 ms got an immediate 0, so **every correct event loop written
against it became a busy loop** (do the right thing, see nothing, ask again,
forever).  *A timeout that returns early is not a safe approximation of one that
waits; it is a different function.*  It now arms a `ktimer` and blocks like
every other wait §M53 and §M55 built.  **ONE definition of readiness**
(`fd_readiness`, shared by poll and epoll) replaced two, and extracting it
immediately exposed two bugs in `poll_snapshot`'s fall-through: **`FD_NETSOCK`
was reported PERMANENTLY READY** (so any loop polling an AF_INET socket spun —
unnoticed because nothing polled one until epoll made it obvious), and **stdin
was never ready at all**.  stdin is now readable when a whole LINE is buffered,
not a byte — cooked reads block until Enter, and *a poll that lies about which
reads will not block is the one thing a poll must never do*; `vc_kbd_push`
signals on the newline only.  **NEW: `epoll`** (create/ctl/wait,
level-triggered, kernel-resident set, caller cookie returned verbatim).  Stated
plainly in its own header: **our `epoll_wait` still SCANS**, so the asymptotics
are poll's — the win is the INTERFACE (the watch list stops crossing the syscall
boundary every iteration); genuine O(ready) needs per-fd waitqs with callback
registration, a change to every fd kind, and bundling it in silently would claim
an efficiency the code does not have.  **`EPOLLET` is REFUSED, not downgraded**
— serving it level-triggered would "work" (level is a superset) but a program
written for it drains each fd once per report and would be handed the same fd
forever, spinning while appearing correct; a loud `-EINVAL` points at the one
line to change.  poll and epoll share ONE blocking loop (`fd_readiness_wait`):
two copies would be two chances to get the lost-wakeup rule wrong.  **THE
SHARPEST ABI TRAP YET:** `struct epoll_event` is 12 bytes on i386, **12 on
amd64** (Linux packs it on x86_64 *specifically* so the 32- and 64-bit layouts
agree) and **16 on arm64** — the size does NOT follow the word size, so deriving
one from the other passes on two arches and fails on the third.
`abi_map.epoll_event_bytes` carries it (a sibling of §M53's `word_bytes`); and
the x86 layout leaves `data` at offset 4, **unaligned for a u64 on a 64-bit
host**, so it is marshalled BYTEWISE rather than relying on x86's tolerance —
*writing the kernel so it only works on forgiving hardware is how an arch port
later fails for no visible reason.*  Proven with an UNMODIFIED musl binary on
all three arches (`epollmusltest`): sizes 12/12/16, cookie `0x1122334455667788`
intact everywhere — bits in BOTH halves on purpose, because a four-byte offset
error would look plausible with a small integer and invisible with zero.  Shell:
`epolltest`, `epollmusltest`.

**§M56.1 — FINISHED THE OPEN ITEMS (same day).**  Four of the five were things
an event loop cannot work without.  **Hangup is visible WITHOUT reading**:
readiness is a POLL* mask now, `POLLERR`/`POLLHUP`/`POLLNVAL` reported
UNREQUESTED (POSIX — the only other way to find EOF is the read the loop exists
to avoid), and **`POLLRDHUP` kept SEPARATE from `POLLHUP`**: a closed writer
with data still buffered reports `RDHUP|IN` (`2001`), only once drained
`RDHUP|HUP` (`2010`) — collapsing them would throw away the tail of every
conversation whose writer closed promptly, which is most of them.
**`O_NONBLOCK` is GENERIC**: it lived inside `struct netsock`, so setting it on
a pipe did nothing AND said nothing (the worst failure an event loop can meet);
it now lives on `struct ofile` where POSIX puts it — a property of the open file
DESCRIPTION, shared by dup, not by a second open — and an empty pipe with a live
writer returns **EAGAIN, not 0** (zero means EOF, and a drain loop told EOF by a
live pipe stops for good).  **An epoll set is itself pollable** so loops nest —
which immediately created a way to hang the kernel (two sets watching each other
recurse until the stack is gone, with a lock held on every frame), closed with a
bounded `task->epoll_depth`; refusing to descend reports "not ready", so a
too-deep chain never fires instead of killing the box.  **`sigprocmask` IS REAL**
(it was `return 0` — accept and forget): a blocked signal stays PENDING and is
delivered when unblocked (*dropping it would make sigprocmask a way to LOSE
signals rather than DEFER them*), SIGKILL stays unblockable (§M46's guarantee,
not politeness), `rt_sigpending` added because without it "defer" and "discard"
are indistinguishable from inside the process — and `epoll_pwait` now swaps the
mask around the wait, which is the ENTIRE reason that call exists (unblock-then-
wait as two steps loses a signal landing between them).  **TWO BUGS THE NEW
TESTS FOUND:** (1) **`ABI_SIGPROCMASK` had a working handler registered in the
arm64 map ONLY** — §M50's engine DECLINES numbers absent from a guest's map and
lets the old per-arch `switch` answer, where a stub returned "success, did
nothing".  So it worked on arm64 and was unreachable on both x86 guests, with no
error anywhere: *a fallback is only a fallback while nothing better exists; when
something better arrives it must be REMOVED IN THE SAME CHANGE* (§M52's lesson,
arriving through the mechanism §M50 built).  (2) **A Linux `sigset_t` stores
signal N at bit N-1; this kernel stores it at bit N** — both self-consistent,
and copying the word across without shifting fails SILENTLY: SIGALRM (14) lands
on bit 14, which is SIGCHLD's slot here, so the mask looks set and never
matches.  One number in one test (`pending=0`) was the whole symptom;
`abi_sigset_to_kernel`/`to_guest` are the whole fix.  (The first run also
"found" a POLLRDHUP bug that was the TEST's: it closed a pipe fd without
`EPOLL_CTL_DEL`, the next pipe reused the number, `ADD` failed `-EEXIST`, and
the stale entry's narrower mask hid the bit.)  **THE ONE ITEM NOT BUILT, WITH
THE NUMBER BEHIND IT:** `epoll_wait` still scans — `epolltest` now MEASURES it
at **~16 µs for 26 registered fds** (≈620 ns each, emulated), so
`EPOLL_MAX_ITEMS = 64` bounds the worst case at ~40 µs BY CONSTRUCTION and a
realistic loop (<10 fds) pays ~6 µs.  Per-fd wakeups would buy none of that back
at this scale and would cost a new lifetime relationship between epoll items and
open file descriptions — §M54's defect class.  *A measured decision with a
measured trigger, not a deferral.*

**§M56.2 — NO BUGS LEFT BEHIND (2026-08-12).**  **`EPOLLERR` has a producer:**
a TCP **RST is not a FIN** — both end the connection, but one is an orderly EOF
and the other is a BROKEN connection, and without the distinction a refused or
dropped connection looks exactly like a server that answered with nothing
(`g_tcp.reset` → POLLERR, reported unrequested like POLLHUP).  **Signal masks
are handled at their REAL width** (8 bytes, the `sigsetsize` every libc passes):
this kernel has 32 signals and no real-time signals, so writing ZERO for bits
32–63 is the TRUTH rather than a loss, and what the guest keeps beyond
`sigsetsize` is left untouched.  **AND `epoll_wait`'s SCAN WAS FINISHED — BY
BUILDING THE CACHE, MEASURING IT, AND REMOVING IT.**  Each item remembered
(description, generation, answer); the lifetime problem was solved cleanly (the
fd table is consulted every time, so a remembered pointer is only ever
COMPARED, never dereferenced; generations come from ONE global sequence so a new
object at a reused address cannot present a number the cache has seen).  **It
was still wrong, and the test written alongside it said so on the first run:**
`memo check — 20/20 ready, 0/20 idle`.  A cache like this is correct only if
EVERY readiness-affecting state change bumps the generation, and the sites are
NOT where intuition puts them — **a pipe's readability changes when its OWNER
reads; its writability changes when its PEER reads**.  Two sites were missing
on the first attempt.  *A cache whose invalidation must be remembered at every
mutation site is a bug generator, and the bug it generates is an event that
never arrives* — surfacing long after the change that caused it.  **It also
bought nothing:** 15.5 µs vs 16.4 µs for 26 descriptors, inside the noise,
because the per-item cost is the LOOKUP and the LOOP, not the readiness
evaluation it was skipping — *the optimisation was aimed at the wrong thing.*
Replaced by `fd_readiness_of()`, which takes the ofile the scan already
resolved instead of looking it up twice: plainly redundant work removed, and
labelled in the source as the right SHAPE rather than a proven win, because at
this scale the benchmark is noise-dominated (16–25 µs across runs).  **LESSON
FOR ANY FUTURE OPTIMISATION HERE: write the test that can falsify it BEFORE
writing it, and measure both sides.**  **STILL OPEN:** genuine per-fd wakeups
(declined on the measurement, trigger written down); `EPOLLEXCLUSIVE` and
edge-triggered mode (the latter deliberately refused).

✅ **§M55 — WAITING FOR THE NETWORK WITHOUT SPENDING A CPU ON IT (2026-08-11,
DOCS §4.56, i386 + x86_64 at -smp 4).**  §M24 drove RX by *polling from the
calling task*, and net.c's header said so — *"everything runs in one task
context → no locking"*, true when written.  Three defects follow and they
compound: (1) **the waiter burned a CPU** — a task waiting for DNS spun with
`hal_cpu_pause` and never left the runqueue, so *waiting for the network cost
exactly as much as computing flat out* (§M49 removed the last such polls from
the console and the reaper; the net stack was the one that got away); (2) **N
waiters meant N pollers** — `dev->poll` is not reentrant (it advances
`last_used_idx` and recycles RX buffers), so two waiters were two tasks mutating
one ring, surviving only because nothing ever waited on two sockets at once —
*a statement about the workload, not the code*; (3) **a spin count is not a
timeout** — `20000000u` means a different duration on every machine and arch,
and is exactly how musl's resolver hung "for minutes" on emulated i386.  NOW:
ONE poller task (**`netd`**) is the only caller of `dev->poll`, everyone else
BLOCKS on the stack waitq until their condition holds or a REAL millisecond
deadline (§M53's clock + a per-wait `ktimer`) passes; **netd runs exactly while
somebody is waiting and is fully blocked otherwise** — on a box that never
touches the network the task does not exist (`lsnic`: `netd: not started`).
The waitq's lock IS the stack lock (waitq's own contract), so `net_rx` and
everything below runs with it HELD — **whence the rule that reshaped the file:
the RX path may never resolve an ARP entry.**  Replies now go back to the MAC
the frame CAME FROM (the correct next hop by construction, on-link or routed,
and cheaper — *a TCP ACK has no business doing an address lookup*), via a
`via_mac` argument and `_locked` (assemble+transmit) vs unlocked (resolve, then
emit) send helpers; a TCP connection resolves its peer's MAC ONCE at connect.
**NEW `netstorm [n]`** — n tasks ARP for different unanswerable addresses, so
each really waits: **8 and 16 concurrent waiters both finish in ~3.4 s at 2–3%
of 4 CPUs**, and the elapsed time is falsifiable (serialised would be 3 s × n).
**A NUMBER THAT DID NOT ADD UP FOUND A REAL BUG:** first storm on a fresh boot
143115 pumps, second (identical work) 4463 — `net_wait_cond`'s
poller-not-up-yet fallback looped **with the stack lock held and interrupts
off**, so it could not be preempted and starved the very task whose arrival
would end it.  Dropping the lock + yielding each round: **4047 pumps, 2 inline**.
The inline count is now separate in `lsnic` — *a rising figure there means the
poller is not doing its job, and that should be visible rather than inferred*.
**Two process lessons:** I nearly skipped that measurement because the number
merely looked large; and the build behind the first explanation had **FAILED** —
an `error:` slipped past my output filter and I read a stale ISO (§M51 again:
*check the exit status, not the output*).  **PART 2 — THE NIC INTERRUPT**, safe to wire only
now (before part 1 it would have been the ISR PLUS every spinning waiter on one
virtqueue).  The ISR does two things and no third: **acks the device** (reading
the legacy ISR status register is what deasserts the level-triggered line —
skipping it is an interrupt storm, and a zero read means it was not ours) and
**wakes the poller**.  It does NOT drain the ring: draining runs `net_rx`, which
can generate a TCP ACK, which spins on the TX virtqueue — §M49's xHCI lesson
verbatim.  **The stack learns interrupts work by RECEIVING one** (no config key;
a driver that wires an interrupt which never fires degrades to polling instead
of blocking forever on a promise), and **netd never blocks indefinitely** — a
10 ms backstop makes a missed interrupt cost LATENCY, not LIVENESS, while an
interrupt SEQUENCE COUNTER sampled before the pump and re-compared under the
queue lock makes one arriving DURING that pump impossible to lose (a bare "is
there work" flag would lose exactly those).  TX completion interrupts are
suppressed (`VRING_AVAIL_F_NO_INTERRUPT` — we wait for them synchronously, so
they could only wake a task with nothing to do), and `vnet_poll` notifies only
when it actually recycled a buffer.  **Measured: `nettest` 4982 → 26/39 pumps;
`netstorm 8` 4047 → 286 pumps at 0% of 4 CPUs; `missed 0` throughout.**  The
~290 backstops `netstorm` reports are the DESIGN, not a defect (3 s ÷ 10 ms =
300 with nothing coming) — which is why `missed` is counted separately: only a
backstop whose next pump FINDS FRAMES is a fault.  **AND THE SAME RAKE, AGAIN:**
the first part-2 run reported `peak waiters 0` + FAIL while 8 probes
demonstrably finished in parallel in 2912 ms — I had added a struct field to
`net.h` and rebuilt **without `make clean`**, so net.c and shell.c used two
layouts of one struct.  §M51's lesson, which CLAUDE.md states outright, and it
still caught me; what saved it is that the numbers CONTRADICTED EACH OTHER —
*a self-contradictory measurement is evidence about the measuring apparatus.*
**OPEN:** the stack is still single-instance above the transport (one ping, one
DNS query, one TCP connection) — §M55 makes concurrency SAFE, not
multi-connection; `epoll` and non-blocking file I/O are next.

✅ **§M54 — A TASK THE SCHEDULER WAS STILL STANDING ON (2026-08-10, DOCS §4.54,
all 3 arches).**  Reported as *"open NetSurf, a crash report comes up, close it,
open it again — the machine dies"*; §M47's NVRAM breadcrumb had already written
the answer down from the previous boot — `kernel-fault in 'idle-3' at
pc=0x120ab5` = **`pick_next_local_locked+0x54`, reading `t->state` out of a task
that was no longer a task**.  ROOT CAUSE: `schedule_locked` publishes the
incoming task as `current` **BEFORE** it swaps stacks, so between those two
points the outgoing task is current NOWHERE while a CPU is still executing on
its kernel stack and has not written back its saved `esp`.  Both the guard that
stops two CPUs picking one task (`task_running_elsewhere`) and the reaper's
"is it current anywhere" check asked exactly that question — so one CPU resumed
a task **from a stale esp while another was still on it** (one task, two CPUs,
one stack), and the reaper **freed a kernel stack that was in use**.  Fixed with
`task->on_cpu`, a flag that spans the WHOLE switch, released by the task that
takes the CPU over — the outgoing task cannot do it, because by the time it
would be safe it is no longer running.  **Missing the second arrival point (the
brand-new-task trampoline, `task_finish_first_switch`) was a bug in the first
version of the fix** and left tasks unreapable.  FOUR MORE on the same path:
(1) a **DEAD task could be enqueued** (the wake paths decide under one lock and
enqueue after dropping it) — the check now lives inside `rq_insert_tail_locked`,
under the destination queue's lock, one instruction before the insert; (2) the
exit path's `if (self->cpu_home == this_cpu_id()) rq_remove_locked(...)` in FOUR
places is not a removal but a removal ATTEMPT — **a guard that silently skips
when its premise fails is not a guard** — replaced by `rq_purge_all` over every
queue, airtight BECAUSE it runs after DEAD is published; (3) *"I marked myself
asleep, then took myself off the queue — but by then someone had already woken
me"* — `task_msleep`/`waitq_block` left a task **awake, ready and on no
runqueue**, never scheduled again, with no trace (this is what "the shell just
stopped" was); (4) the runqueue walks trusted the ring absolutely — now bounded,
and a broken ring is repaired + reported instead of faulting in the scheduler
with the lock held.  **NEW `killstorm [rounds] [tasks]` (both shells)
reproduces the whole family in seconds** — 480 spawn+kill cycles over 4 CPUs;
killed the box on the first run, now three clean runs on x86_64 and 300 clean on
aarch64.  *A bug that needs a browser, a crash and a reboot is a bug nobody can
work on.*  **GUI half (what the user saw FIRST):** a crashed dosgui client
leaked its bridge handle forever (`dosgui_destroy` is a call a crashed client
never makes), so NetSurf refused to open after four crashes — disposal is now
NOTIFIED from `destroy_window` on every route (`gui_window_set_dispose_cb`): *a
handle whose lifetime is INFERRED is a handle that leaks.*  **Diagnostics that
were missing exactly when needed:** x86_64 kernel-fault records hard-coded 0 for
the fault address (CR2 now recorded), ring-0 dumps name the faulting task/pid/
CPU, two CPUs faulting at once no longer interleave their dumps
(`crash_dump_begin/end`, bounded + lock-free — never a real lock in fault
context), and `crash_report` claims its ring slot atomically (two simultaneous
faults used to blend into ONE record).  **OPEN:** the reap sweep still reports a
queued task roughly once in several hundred kills (caught, repaired, logged —
box unaffected); an i386 `killstorm` run once hung a shell task (box stayed up);
`load_steal_one` leaves `cpu_home` naming the stealer while the task is briefly
on no queue — setting it to -1 is more honest and HANGS tasks, because the block
paths use `cpu_home` to find the queue to detach from.

✅ **§M53 STAGE 3 — A DEADLINE YOU CAN WAIT ON (2026-08-10, DOCS §4.55, all 3
arches).**  `timerfd` + `setitimer`.  Stage 2 let a program WAIT for a deadline
but only by doing nothing else meanwhile; a real event loop is already blocked
in `poll` on its sockets, so "wake me in 20 ms" has to arrive through the SAME
wait or the loop chooses between being responsive and being punctual.  A timer
behind a DESCRIPTOR makes the two commensurable — and it is exactly what an
`epoll`-shaped loop needs, which is why it landed before the async work.
`setitimer` is the other delivery (SIGALRM) for the program with nothing to
poll.  **A read yields the EXPIRATION COUNT and resets it**: a timer that
silently dropped uncollected ticks would let a program drift with nothing to
notice — the §M53 stage-1 lesson one layer up.  **Periodic timers re-arm from
the stored DEADLINE, never from `now`** — re-arming from now adds each expiry's
lateness to every later period, and lateness is bounded by the tick while the
drift is not.  `timerfdtest` measures error against the ORIGINAL start, not the
previous tick (*a drifting timer looks perfect tick-to-tick*): it oscillates
around the tick floor (~1 ms x86, ~10 ms aarch64 at 100 Hz) and does not grow.
**§M50's engine paid off exactly as advertised — four canonical ops, four
handlers, four table rows per guest, and all three arches got `timerfd` at
once** — with one addition: **`abi_map.word_bytes`**, because `struct
itimerspec` is four `long`s (16 bytes on a 32-bit guest, 32 on a 64-bit one)
and the width is a property of the GUEST, not something a shared handler may
infer from the host's own `sizeof(long)`.  Traps worth keeping: `it_interval`
comes FIRST in `itimerspec` (getting it backwards yields a timer that works
exactly once, which reads like a different bug), and `itimerval` is
MICROseconds — the one place POSIX uses a different unit for the same idea.
Interval timers live in a **pid-keyed table, not in `struct task`**: an embedded
timer must be cancelled at exactly the right point in teardown, and getting that
wrong fires a callback into freed memory (§M54's failure one layer up).
Delivery is an atomic pending-signal BIT set from interrupt context, never
`sys_kill` (which takes the scheduler lock + applies a ring-3 credential rule).
Shell: `timerfdtest [ms]`, `alarmtest [ms]`.  **Open:** `timer_create`
(per-process timer IDs / `sigev_notify`) and ITIMER_VIRTUAL/PROF (need per-task
CPU-time hooks — declined rather than faked); `poll(2)` still treats a positive
timeout as a snapshot, and now that a deadline is a first-class object a finite
`poll` timeout should just BE a timerfd internally — the next thing the async
work wants.

✅ **§M53 STAGES 1–2 — TIME, IN NANOSECONDS (2026-08-09, DOCS §4.53, all 3
arches).**  `timer_now_ns()` — ONE monotonic nanosecond clock from whatever the
machine has, callers never learning which: `CNTPCT_EL0` on aarch64 (62.5 MHz,
16 ns; the architecture DEFINES the rate, nothing to calibrate) and the TSC on
x86 (~1.2 GHz, 1 ns) but **only once established to be constant-rate**
(`CPUID.80000007:EDX[8]`, or `CPUID.1:ECX[31]` — a hypervisor virtualises the
TSC so it cannot track a guest core's frequency scaling).  Neither → keep the
tick: *a coarse clock that is right beats a fine one that is wrong.*  Plus
**deadline timers** (`ktimer_arm`/`ktimer_cancel`, one sorted list, callbacks in
IRQ context with the lock dropped), `task_sleep_until_ns`, and
`sys_clock_nanosleep_ns` (the ABSOLUTE form matters: a relative sleep restarted
after a signal DRIFTS, so every non-drifting periodic loop is written against
the absolute one).  **THE CLOCK FOUND A REAL BUG IN ITS FIRST MINUTES:** `ktime`
reported a 100 ms sleep on aarch64 as **57 ms** — `timer_ticks_ms` divided a
tick counter that EVERY CPU increments by a PER-CPU rate, so the millisecond
clock ran N× too fast on an N-CPU box and every timeout/watchdog deadline/sleep
on that arch was wrong by the CPU count, silently, **for want of a second
opinion**.  Now read from `CNTPCT` directly (100.0/105/106 ms at -smp 1/2/4);
x86 was never affected (the PIT delivers to the BSP only).  **AND THE ACCURACY
FLOOR IS MEASURED, NOT ASSUMED:** expiry first went into `schedule_check`, which
LOOKS like the tick and runs at the QUANTUM rate — every timer up to 10 ms late
regardless of its deadline (a 500 µs sleep measured 9.7 ms).  Moved to the tick
ISR: worst lateness **9037 µs → 840–953 µs on i386**, i.e. the tick period
itself; aarch64's floor is 10 ms because it ticks at 100 Hz, and its `ktimer`
says so.  New **`ktime`** + **`ktimer`** commands on both shells report source,
resolution and the error on a spread of sleeps.  **Open:** one-shot hardware
deadlines (TSC-deadline / `CNTP_CVAL`) to remove the tick floor; raising the
aarch64 tick needs a quantum divider like x86's `SCHED_QUANTUM_TICKS` first;
`timerfd` + `timer_create`/`setitimer` (stage 3, and what `epoll`-shaped event
loops need); a clock read costs a 64-bit division (~2–4 µs under emulation) —
Linux precomputes a multiply-and-shift.

✅ **§M52 — THE NOTE THAT OUTLIVED ITS PREMISE (2026-08-09, DOCS §4.52; x86_64
clean at -smp 1/2/4).**  x86_64's SYSCALL entry stub kept the kernel stack and
the stashed user `rsp` in **two GLOBALS**, so two CPUs inside `syscall` at once
overwrote each other's stash **and ran the kernel on the SAME stack** — each
then returned to ring 3 with the other's stack pointer (a child executing its
own argv strings; the kernel returning to address 3).  **`syscall_entry.s` had
said so in its own header since §M20.6.1** — *"UP-correct only … ring-3 tasks
only run on the BSP today … **Noted, not built**"* — and every word was TRUE
when written.  §M35 then gave x86_64 a per-CPU TSS and ring-3 tasks began
running on APs; nothing went back to the note.  **A COMMENT CANNOT FAIL A
TEST.**  Fixed with **`swapgs`**, the instruction that exists for exactly this:
it swaps `IA32_KERNEL_GS_BASE` into `GS.base` atomically so the stub reaches a
per-CPU slot (`[gs:0]` kernel rsp, `[gs:8]` user-rsp stash) **without needing a
spare register** — and `syscall` leaves none (`rcx`/`r11` clobbered by the
instruction, everything else holds an argument).  GS is free because x86_64 musl
keeps TLS in FS.  Swaps back BEFORE the shared `isr_common` tail, so nothing
else in the kernel knows.  **WHY IT HID — THREE INDEPENDENT IMMUNITIES:** (1)
i386 enters via `int 0x80`, and an interrupt gate switches stacks through the
TSS, per-CPU since §M35; (2) native d-os programs (`forktest`/`forkexec`/
`pipetest`) use `int 0x80` too and passed at -smp 2 throughout — only **musl**
binaries issue `syscall` (musl hard-codes it, and not patching musl is the whole
point of the personality); (3) ONE musl process rarely collides with itself, so
`musltest` passed — it took TWO (a shell and the coreutil it forks).  **GENERAL
LESSON: a deferred note is a DEPENDENCY ON A PREMISE, and nothing in the build
checks that the premise still holds.**  Both milestones that invalidated it were
green.

✅ **§M51 — THE BROADCAST x86 DOES NOT HAVE (2026-08-08, DOCS §4.51, i386
verified at -smp 1/2/4).**  x86 does NOT broadcast TLB invalidation — `invlpg`
and a CR3 reload are strictly local — and **nothing in the tree ever sent an
invalidation IPI**.  COW's whole safety argument is "the next write faults", so
a parent runnable on a second core in the window between `fork` and the child's
`execve` kept a WRITABLE entry for pages `fork` had just protected: its next
write did NOT fault, it landed in the frame the child was sharing, and the two
processes corrupted each other **with no fault and no log**.  That is why
`pkgrun sh -c "echo A; echo B"` failed ~2 runs in 3 at `-smp 2` and passed every
time at `-smp 1`.  New **`kernel/hal/x86/tlb.c`** (shared by both x86 arches):
IPI vector 0x42 + a per-CPU **ticket pair** (`percpu.tlb_req`/`tlb_ack`) — no
lock and no request slot, because the remote action is always a **FULL flush**,
which makes overlapping requests harmless; the ack is published only AFTER the
flush; and **the wait loop services its own slot**, without which two
simultaneous shootdowns from interrupt-disabled contexts wait on each other
forever.  aarch64 needs none of it (`tlbi ...is` IS the hardware broadcast) —
its `hal_tlb_shootdown` is an empty function with the reason written down.
**Only WEAKENING edits pay:** remap-over-present, unmap, mprotect and the COW
resolution broadcast; a FRESH map does not, and that distinction is what keeps
`map_in_pd` (once per page of every ELF load) affordable.  `vmm_space_clone`
suppresses the per-page broadcast entirely and issues ONE whole-space shootdown
at the end — the first version broadcast per page and turned a fork into
thousands of IPI round trips that never finished.  **LESSON — THE HARNESS LIED
FOR AN HOUR:** after adding fields to `struct percpu` the shootdown *appeared*
to hang; the target CPU was demonstrably alive (tick/switch counters advancing)
yet never took the vector, and its `apic_id` read back as 3 on a box whose boot
log said 1.  **This project has no header dependencies** — CLAUDE.md says to
`make clean ARCH=<arch>` after editing a shared header, and I had not, so half
the tree used the OLD `struct percpu` layout.  Every measurement in that window
was fiction; the first run after `make clean` passed.  *A documented build
convention is a CORRECTNESS convention, and an impossible measurement — a field
holding a value it cannot hold — is evidence about the BUILD, not the code.*
The x86_64 SMP failure left open here turned out to be a
DIFFERENT bug entirely and is fixed in §M52 above — and the pointer that led
there was exactly "i386 and x86_64 share the VMM shape but NOT the syscall entry
path".

✅ **AARCH64 A3 — A SHELL THAT FORKS AND EXECS, AND THE REGISTER NOBODY SAVED
(2026-08-08, DOCS §4.50).**  `pkgrun sh -c "echo A; echo B; echo C"` prints all
three on ARM — a musl shell forking musl coreutils out of the store, third arch,
`-smp 1` and `-smp 4`.  The predicted work (cross toolchain, arch-parametric
recipes) was an afternoon; the stage was held up by **per-task state kept where
nothing saves it, twice**: `TPIDR_EL0` (EL0 writes it with one `msr`, so
`has_tls` is never set and a forked musl child dies at `TP-0xc8`), then
**`SP_EL0`** — banked, absent from the trapframe, written by the kernel exactly
once at the `eret` into EL0, and **never saved by `context_switch`**.  A task
that blocked at EL0 resumed with whichever task ran there next, so `sh -c "echo
one"` printed `one` and THEN the shell died at a wild PC (a shell's first
schedule-out at EL0 is its `waitpid`).  **x86 cannot have this bug — the user SP
is a field in the frame the CPU pushes — which is why no design review caught
it.**  RULE FOR A4 AND ANY NEW ARCH: *enumerate the registers the kernel writes
once and the user owns thereafter; every one belongs in the switch* (here:
TPIDR_EL0, SP_EL0, FP/SIMD — all three found one crash at a time).  Found by
NARROWING, not guessing: a never-free experiment, a `-smp 1` run, a **canary in
the trapframe — which proved the frame INTACT and killed every theory about the
return path at once**, `ESR` in the fault print, then a dump of the user stack
showing kernel frame data at user addresses.  `ESR`+`SP_EL0`+task name are now
permanent in that print.  Three pre-existing defects fixed on the way:
`cow_release` decremented the LAST reference without freeing (every fork-shared
page leaked); the COW paths used CPU-local `tlbi vmalle1` where a sibling core
keeps a stale writable entry (→ `vmalle1is`); `ABI_WAIT` returned the raw exit
code instead of `(code & 0xFF) << 8`, unvalidated.  **Build lesson:** the
per-arch artifact cache filed `user/*.muslelf` by `build/.last_arch` — a HINT,
wrong whenever `make` runs directly — and had parked an AArch64 `sh.muslelf` in
the x86_64 slot, whence it was linked into the x86_64 kernel and surfaced as
`rc=-7` (ELF_EBADARCH); **27 cached artifacts were mis-filed**.  The cache now
keys on the file's own `e_machine`: *ask the file, not the stamp.*  **OPEN, NARROWED, NOT ROOT-CAUSED —
TOP OF THE LIST (full state of the hunt in DOCS §4.50): a multi-command `sh -c`
fails on BOTH x86 arches and ONLY UNDER SMP.**  `-smp 1` passes every time;
`-smp 2` fails ~2 runs in 3; adding a `kprintf` to the syscall path makes it pass
— a RACE, not a logic error.  The parent's `esp`/`ebp` at the fault are IDENTICAL
every run (inside `run_command`, its local `argv[]` intact) while the faulting
`eip` is DIFFERENT every run and always garbage — so a corrupted **code pointer**,
not a corrupted stack pointer (which is what distinguishes it from the ARM bug).
Ruled out by measurement: the artifact cache (i386 never affected), `task_reap`
freeing a still-queued task (probe never fired), stale cross-CPU TLB on migration
(forced an unconditional CR3 reload — **still fails**), `signal_deliver` on a
ring-0 frame (correctly guarded).  **Still standing and where to look first:
THERE IS NO TLB SHOOTDOWN IPI IN THE TREE** — every `invlpg` on both x86 arches
is CPU-local and `lapic_send_ipi` serves only the preempt IPI.  A CR3 reload
closes the MIGRATION window but not the one where two tasks share an address
space on two cores concurrently — exactly what a `fork` in flight is before the
child's `execve` lands.  Also seen twice: an SMP kernel fault in `load_steal_one`
(§M49 balancer) taking the runqueue lock down; probably the same corruption, not
yet shown.  Also open: `ls` needs `openat`+`getdents64`, which need a place in
the §M50 pipeline for per-guest FLAG translation (a second table, a design step).

✅ **AARCH64 A2 — UNMODIFIED musl RUNS ON ARM, IN ~80 LINES (2026-08-07, DOCS
§4.49).**  `hal/aarch64/linux_abi.c` is **~80 lines** vs 1211 (x86) and 1064
(x86_64) — not less capability but §M50's engine landing FIRST, so all that
remains is the genuinely arch-specific part: **x8 = number, x0..x5 = args,
result in x0**.  `musltest` passes with **ZERO unhandled syscalls** — the
vocabulary grown for x86 was already enough for an ARM musl startup, which is
the strongest evidence the engine's split is right.  Pointer gate armed from
line one (§M47.2's lesson).  **THE TRAP PLAN_AARCH64 PREDICTED FOR A6 ARRIVED AT
A2:** musl's `memset` opens with `dup v0.16b, w1` (NEON), so libc startup
trapped — presenting as an EL0 fault with **`FAR_EL1 = 0`, which reads exactly
like a null dereference and is nothing of the kind**.  *When a fault address
looks impossible, DISASSEMBLE THE FAULTING INSTRUCTION before theorising about
the address* — it took one step.  `fpu.c` had described this failure AND both
halves of the fix in advance; both now implemented: `CPACR_EL1.FPEN` per CPU
(**BSP and AP** — enable one core only and FP works there and traps on the
other) + Q0..Q31/FPCR/FPSR saved on context switch (`stp` of 64-bit regs tops
out at 504, so the control words at 512 need plain `str`; a zeroed image IS
valid here, unlike x86 FXSAVE).  A1's forktest/sigtest/pipetest still pass with
FP live on the switch path.  **This unblocks A5/A6 early** — the FP unit is what
every ported library needs.  Also: A2's proof needed the toolchain half of A3
(plan ordering was wrong), cheap because `fetch-musl-cross-prebuilt.sh` was
already arch-parametric.

▶️ **§M50 STARTED — ONE GUEST-ABI TRANSLATION ENGINE (2026-08-07, DOCS §4.48,
PLAN §M50).**  `hal/x86/linux_abi.c` + `hal/x86_64/linux_abi.c` = 2275 lines,
~160 `case`s, **two copies of one idea**; aarch64 (A2) would have been a third.
Linux numbers `read` 3/0/63 on i386/amd64/arm64 — same meaning, different DATA.
Pipeline: **arch shim (frame→args) → per-guest number map → canonical op →
shared handler** (`includes/abi.h`, `core/abi_engine.c`, `core/abi_linux.c`).
A new ARCH = ~6 lines; a new GUEST ABI = a table; a new SYSCALL = one handler
every arch gets at once.  **The engine may DECLINE** and the old switch stays as
fallback — that is what makes the 2275 lines migrate ONE OPERATION AT A TIME
with both paths side by side.  Vocabulary is named after MEANINGS not Linux
spellings (`ABI_SEEK`, not `ABI_LSEEK`) — otherwise the interlingua quietly
becomes "Linux with different numbers".  Both x86 arches now serve read/write/
close/seek/mprotect/munmap/getpid/getppid through it, musl userland unchanged
(`musltest`+`solibtest`+`crypttest` pass on both); `abi` prints the three number
spaces side by side.  **Windows analysis in PLAN §M50:** the pipeline
generalises, the CUT POINT does not — NT syscall numbers are not a contract
(they change between builds), which is why Wine cuts at the DLL boundary and why
WSL1 was replaced by a real kernel; the hard part is SEMANTICS (HANDLEs vs fds,
CreateProcess vs fork, SEH vs signals, reserve/commit vs mmap), not numbering.
Next: migrate more ops, then the aarch64 shim (PLAN_AARCH64 A2) — which also
needs `make musl` made arch-parametric (today hardwired `--target=i386`).

✅ **AARCH64 A1 — A POSIX PROCESS MODEL ON ARM (2026-08-07, DOCS §4.47,
PLAN_AARCH64 stage A1).**  M21's "full x86 parity" was true when written; §M34's
fork + signals landed on x86 afterwards and were never carried across.
PLAN_AARCH64 scoped this as "mirror `hal/x86_64/fork.c`" — **that file was about
a quarter of the job**: the port also had NO aarch64 `struct user_regs` (it fell
through to the **i386** one — `eax`/`ebx` names at 64-bit width), no
`enter_user_mode_regs`, and no data-abort decode into a COW resolution.
Shipped: `vmm_space_clone` (both sides read-only + `PTE_SW_COW`, bit 56) +
`vmm_cow_fault`, refcount table sized from `pmm_nr_frames` (§M48 lesson),
`fork.c` + `signal.c`, and SYS_FORK/WAITPID/EXECVE/PIPE/DUP2/KILL/SIGACTION/
SIGRETURN.  **Three ARM traps worth carrying into A2+:** (1) **SP_EL0 is NOT in
the trapframe** — an EL0 exception switches to SP_EL1 and leaves it banked, so
fork and signal delivery read/write it with `mrs`/`msr`; (2) **the signal return
address is a REGISTER** (x30), not a stack slot, so nothing is pushed and
`SYS_SIGRETURN` finds the saved context exactly at the user SP; (3) **COW must
be resolved BEFORE the uaccess fixup** — a kernel write into a forked child's
buffer must copy the page, not unwind as `-EFAULT`.  Verified over the ARM
SERIAL shell (`serial_shell.c` is its own small REPL — the full `shell.c` needs
a VC behind virtio-input, undrivable headless, which is why PLAN_AARCH64 scopes
the proof there): `forktest` prints `secret still=111` (real COW isolation),
`pipetest` + `sigtest` pass.  Also fixed a pre-existing truncation: teardown cast
physical addresses to `uint32_t` before freeing them.  Next on this arch: **A2,
the Linux-ABI personality** — without it no musl binary runs at all, which is
why ARM embeds 3 in-tree-libc programs where x86 embeds ~60.

✅ **§M49 — LOAD DISTRIBUTION, MEASURED (2026-08-06, DOCS §4.46, i386 +
x86_64, aarch64 builds).**  §M18.6.1's balancer ran **only when a runqueue went
empty** — work stealing, not load distribution — so with every queue non-empty
an arbitrarily bad split never corrected itself; the file's own comment
described a periodic pass and named a `LOAD_BALANCE_INTERVAL_MS` **that existed
nowhere in the tree**.  Nothing caught it because **`run_qemu.sh` passed no
`-smp`**: the everyday run was uniprocessor, so the balancer never executed on
the path a person uses (the §M48 missing-NIC shape again — the measured path and
the used path were different paths).  New **`sched [ms]`** samples twice and
reports the delta (per-CPU rq/load/busy%/switches/migrations, per-task
demand-vs-actual); it showed five hogs on CPU0 at 15-20% of a core while
singletons got 66% — **and every CPU read 100% busy, so aggregate utilisation is
blind to this whole class of bug.**  Four fixes: (1) a **periodic threshold
pass** (a migration swings the difference by TWICE the moved task, hence a
minimum delta — at 1 it ping-pongs forever); (2) load = **demand** (share of
time spent RUNNABLE), because queue length scores four hogs and four sleepers
alike, and *which* task moves matters — the balancer asks for one of roughly
half the imbalance; (3) **`task_msleep` really blocks** — it was a spin-yield
loop that kept every service queued AND `hlt`ed the CPU with work behind it, so
`cron`/`watchdog` measured as CPU hogs (**a metric is only as honest as the
state it observes**); `task_kill` now wakes a timed sleeper so §M46 teardown
latency is unchanged; (4) **priority** — `nice <pid> <-20..19>` → a weight that
is both quantum budget and load share (measured 65/16/13/4% for -10/0/0/+10),
degenerating to the old behaviour at the default.  Result: queue spread 2..6 →
2..3, x86_64 seven of eight hogs at the ideal 49-50%.  **Lesson:** `struct task`
is constructed in FOUR places and only one is `spawn_common` — the new weight
field stayed zero in the other three and the first boot took a #DE in the
scheduler; one `task_sched_defaults()` plus a guard at the division.
**Then the two remaining polls:** `vc_getchar` and init's reaper now BLOCK (per-VC waitq woken from the keyboard IRQ; the ring
write stays lock-free but the WAKE takes the lock — that is what closes the
lost-wakeup window), so an idle 4-CPU box went from **one core pegged at 100% to
all four at 0-2%**.  That needed `task_kill` to wake waitq-parked tasks (new
`task->wq` back-pointer — `wq_next` alone says "in some queue", not WHICH), and
it exposed a **latent SMP race**: the boot shell's VC was bound AFTER spawn
under `preempt_disable`, which has been PER-CPU since §M18.6.2 while
`task_enqueue` puts the task on another core and IPIs it — the shell reached its
entry point first and exited with "no VC bound".  Four sites had it; fixed with
`task_spawn_console`, binding it inside the spawn exactly as `start_arg` already
was.  And a **deferred-work pool** (`kernel/core/workqueue.c`,
`work_submit`/`work_flush`/`workqueue_stats`, one `kworker` per CPU, blocking so
an idle pool costs nothing; pending list and waitq share ONE lock by waitq's own
contract; `work_flush` counts callbacks that RETURNED, not items dequeued; NO
NMI submission — §M47's crash capture keeps its lock-free ring, an NMI-safe path
needs `irq_work`'s shape).  `wqtest [n]`: 16 items over 4 CPUs in **26 ms vs
~80 ms serial**.  **Lesson:** that test first reported FAIL on duplicate runs —
the TEST was wrong, not the queue; re-queueing an item that is already RUNNING
is the intended semantic (how a driver says "more arrived while you drained").
**First production consumer: the xHCI event-ring drain** — `xhci_poll()` runs
from the TIMER IRQ on both arches and used to drain the whole ring there (MMIO
walk + HID decode + a `vc_kbd_push` that now wakes a task and may IPI a core);
the ISR now only `work_submit`s.  One change in `xhci.c` covers both arches
because both timers call the same function.  It also closed a LATENT bug:
`evt_drain` is not reentrant and its other caller (`cmd_submit_wait`, task
context, enumeration) could be interrupted mid-drain — and `evt_drain(NULL)`
from the ISR SWALLOWS the command completion the enumerating task waits for,
turning success into a 200 ms timeout.  New `spin_trylock` (deferred drain skips
if someone is already draining; the enumeration path holds the lock across its
whole wait).  **Verified with `-device qemu-xhci -device usb-kbd`:
`usb-hid: first key delivered over USB` — a one-shot marker added because every
PC target ALSO has PS/2, so "typing still works" is not evidence the USB path
works.**  Still open: `keyboard_getchar` + the GUI compositor/app-host loops are
still `hlt`+`yield` polls (the GUI ones need waitqs on the compositor's event
queues — a compositor change), and NIC RX is the next workqueue consumer but
needs `net.c` LOCKED first: the stack is single-task by construction ("everything
runs in one task context → no locking") and every blocking helper spins calling
`dev->poll`.

✅ **§M48 — THE MEMORY CEILING IS DISCOVERED, NOT COMPILED IN + A USABLE
BROWSER (2026-08-04, DOCS §4.42–§4.44).**  `pmm_init` sizes its metadata from the
firmware map instead of a per-arch `#define`, so ONE image boots on 128 MiB and
on 128 GiB (x86_64 verified at 1G/2G/3G/4G/8G/128G, userland running, zero
faults).  `pmm_phys_t` widens physical addresses to arch width; seeding emits
maximal ALIGNED runs instead of releasing 33.8 M frames one at a time; new
`ZONE_DMA32` — past 4 GiB, "any frame" and "a frame a 32-bit device can reach"
stop being the same thing, and a device handed a truncated address does not fail
loudly.  **Raising the ceiling exposed that x86_64 userland was broken on ANY
machine with >1 GiB RAM**: the identity map's 1 GiB page landed exactly on
`vmm_user_base()` and every `exec` returned `ELF_ENOMEM` — invisible because
every x86_64 test used `-m 1024M`.  User programs cannot move (small code model
⇒ symbols below 2 GiB), so the KERNEL's physical window moved: a direct map in
the canonical upper half (`KERNEL_DIRECT_MAP_BASE`, `phys_to_virt`), which
compiles to nothing where the base is 0.  **Four more latent bugs, all silent:**
slab's `page_of` masked with a 32-bit `~(4096u-1)`; the COW refcount table
covered 1 GiB and a frame outside it became a DOUBLE FREE (fork shares it, both
spaces free it — same shape on i386, safe only by coincidence); ACPI
identity-mapped its tables "far below the user base", true on i386 and false on
x86_64 where they sit at the top of low RAM, so `fork()` read ACPI memory; and
i386 ring 3 could not execute SSE (`CR4.OSFXSR` deliberately clear — fine while
every binary was ours, fatal for a ported library).  **i386's identity map now
runs to 1 GiB** (it stopped at 256 MiB while user space starts at 1 GiB — three
quarters of the window unused): 234 → 473 MiB on a 512 MiB box.  Past that the
limit is REAL: 32-bit paging = 32-bit physical addresses, and **64 GiB on i386
is exactly the PAE maximum — a different page-table format, not a bigger
constant.**  **NetSurf is now usable**: the compositor had NO mouse-button event
at all (`{KEY, MOTION}`), so a click arrived as motion and nothing in any client
was ever clickable; typing forwarded raw scancodes, which libnsfb reads as ASCII;
and there was no http fetcher compiled in.  `user/netsurf/fetch_dos.c` attaches
through `fetcher_init`'s own `WITH_CURL` hook (vendored tree untouched) over
ring-3 sockets + Mbed TLS with CA + hostname verification.  Transport lessons:
**`send`/`recv` do not work on a connected TCP socket** (the Linux-ABI layer
wires `connect`/`read`/`write`), **never wait for EOF** (the FIN is not surfaced
as `read()==0` — take the length from `Content-Length`), and **the read must stay
BLOCKING** (RX is polled from the calling task, so the blocking read is what
drives the NIC).  **`run_qemu.sh` had no NIC** — every network test passed its
own `-netdev`, so the automated path and the path a person uses were not the same
path.  **Mesa/EGL runs on i386** (`egltri win`, softpipe, GLES 3.1); the blob
block was x86_64-only by where ten Makefile lines lived, not by anything in the
code.  New: `scripts/build-mesa.sh` (arch-parametric), `PLAN_AARCH64.md`.
Open: i386 kmap/PAE, a non-blocking fetcher `poll`.

✅ **§M47 — CRASH RECORDS & REPORTING (2026-08-02, DOCS §4.38, all 3 arches).**
M46 stopped the box from dying; M47 makes sure that when something *does* go
wrong the system SAYS SO.  Two phases on purpose: **capture** (`crash_report`)
runs in fault/NMI context so it only copies a fixed record into a static ring —
no locks, no alloc, no I/O; **delivery** (`crash_drain`, watchdog task) runs
ordinary so a sink may allocate, block or draw.  New destinations register with
`CRASH_SINK()` — **a reporting mechanism can be armed at any time WITHOUT
touching a fault path again.**  Sinks: `klog` (always) + `gui-report` (the Crash
Reports window, `gui/apps/crashapp.c`, gated by `crash.report`, opens itself when
a record is delivered).  Surfaces: `crash`, **`/proc/crash`**, klog, the GUI
window — one record, several views; the window is never the storage.  The one
event nothing in the guest can log (triple fault / reset / power loss) is
reported on the NEXT boot from a CMOS NVRAM marker + a **40-byte checksummed
breadcrumb** (kind/cpu/pid/pc/addr/code/uptime/comm).  Also: taskbar clock shows
the ISO date + keyboard layout, wallpaper label carries the arch
(`d-os M47  x32`/`x64`/`arm64`).

✅ **§M47.2 — THE RING-3 POINTER GATE WAS NEVER ARMED FOR THE LINUX ABI
(2026-08-03, DOCS §4.41).**  `linux_syscall_dispatch` never set
`task->in_user_syscall` on EITHER arch, so §M46's first boundary layer (the
per-syscall pointer gate) was off for **every** musl program — coreutils, sh,
TLS, NetSurf, Wayland.  Nothing failed visibly, which is why it survived two
milestones.  Now armed on both; the `_k`/`_u` discipline covers the places that
legitimately pass kernel buffers (new `sys_recv_u`).

✅ **§M40 STAGE 9 — REAL musl PTHREADS (2026-08-03, DOCS §4.40).**  `clone` was
the last hard `-ENOSYS` in the Linux ABI and it blocked every toolkit AND Mesa.
`proc_clone_thread()` = `proc_fork` but the child SHARES the address space,
resumes on the caller's stack at the SAME instruction with rax/eax = 0 (musl's
`__clone` pre-lays fn+arg there), and installs the caller's thread pointer.
**`pthread_join` needs the kernel**: `CLONE_CHILD_CLEARTID` — `task_exit_code`
zeroes the tid word and futex-wakes before marking DEAD, else a program prints
everything and hangs in join.  **i386 traps:** arg order is (flags, stack, ptid,
TLS, ctid) — TLS BEFORE ctid, opposite of amd64; TLS is a per-CPU GDT descriptor
so the thread must be CPU-pinned and entered with the TLS selector in `%gs`
(else musl faults at `%gs:0x10`); and musl passes a `struct user_desc*`, not a
raw base.  Shell: `pthreadtest`.

✅ **§M40 STAGES 7–8 (2026-08-03, DOCS §4.40).**  **Focus**: `wl_pointer.enter`
/ `wl_keyboard.enter` + `modifiers` + `wl_pointer.frame` — a real client IGNORES
input that arrives without a preceding enter.  (libwayland caught a malformed
`modifiers` we had sized 24 bytes instead of 28 — "message too short" — the kind
of thing a hand-written test client never notices.)  **Keymap**: an xkb keymap
GENERATED from d-os's live layout (`keymap_active()`), passed as a memfd over
SCM_RIGHTS; keycodes = d-os scancode + 8, text self-contained (no `include`).
Verified with a REAL xkb compiler (`xkbcli compile-keymap --from-xkb`: 297 lines,
zero diagnostics; exit codes are inverted in 1.4.0, so the output is the signal —
establish it with a known-bad control).  Chain: scancode 4 → key 4 → xkb 12 →
keysym a/A.  Shell: `waykeymap`.

✅ **§M40 STAGE 6 — AN UNMODIFIED UPSTREAM WAYLAND APP RUNS (2026-08-03, DOCS
§4.40).**  `weston-simple-shm` (weston's own reference client, compiled exactly
as it sits in its tree — nothing patched) animates continuously in a real d-os
window on both x86 arches; 716 frames in one i386 run.  `config.h` sets
HAVE_MEMFD_CREATE because d-os has no writable XDG_RUNTIME_DIR for the mkostemp
fallback.  Accepted two more requests: `xdg_toplevel.set_app_id` and
`wl_shm_pool.destroy` (**buffers outlive the pool** — simple-shm destroys its
pool right after creating its buffers, so releasing the frames there would pull
the pixels out from under a live window).  **Build trap:** never put
`-I<weston>/shared` on the include path — weston has its own `shared/signal.h`
and it shadows the C library's.  Shell: `simpleshm [win]`.

✅ **§M40 STAGE 5 (2026-08-03, DOCS §4.40).**  The two globals a real toolkit
REFUSES to start without: **`wl_output`** (SDL/GTK/Qt all need the size+scale
before laying out; full geometry/mode/scale/done burst, real framebuffer size)
and **`wl_surface.frame`** (a render loop BLOCKS on the callback — ignoring it
stops the app drawing entirely; answered on the next commit, then the callback
object is deleted).  Verified on both arches; the client's frame loop is
self-sustaining (~130 commits/run).

✅ **§M40 STAGE 4 (2026-08-03, DOCS §4.40).**  REAL desktop input reaches the
upstream client's `wl_seat` (pointer motion + keys, both arches).  Three gaps,
all hidden by §M26's demo synthesising input instead of taking it from the
desktop: (1) nobody drained a hook-backed window's queue — a Wayland window's
"host" is the server task (blocked on its socket) and a dosgui window has
`host_task` cleared, so the COMPOSITOR now pumps any window with an
`input_hook`; **this also fixed NetSurf's input, broken the same way**; (2)
pointer motion was only delivered on click (fine for widgets, useless for a
client); (3) only nav/Ctrl-letter keycodes were forwarded.  Client-side lesson:
fill listener structs COMPLETELY — libwayland calls whatever arrives and a NULL
slot is a jump to zero.

✅ **§M40 STAGE 3 (2026-08-03, DOCS §4.40).**  `wayupstream win` → the upstream
client's `xdg_toplevel` IS a desktop window (title bar + taskbar button + its
pixels as contents), verified by screenshot on both arches.  The window is now
created at the FIRST COMMIT WITH CONTENT (our configure says 0×0 = "you pick",
so the size is unknown at `get_toplevel`) and sized via the new
`gui_window_outer_for_content()`; the title arrives before any content, so
`wl_conn.title` holds it until the window exists.

✅ **§M40 STAGE 2 (2026-08-03, DOCS §4.40, i386 + x86_64).**  Upstream libwayland
drives a REAL `xdg_toplevel` + shm buffer; the server reads the client's pixels
(`top-left=ff102040`).  Added: SCM_RIGHTS in both ABI control paths,
`memfd_create`/`ftruncate` (`sys_memfd_resize`/`shm_grow`), memfd mapping from
the Linux `mmap` path, `wl_surface.damage`, real `F_DUPFD`/`F_DUPFD_CLOEXEC`.
**Lesson:** libwayland DUPS every fd it sends; our `fcntl` "succeeded" with 0,
and 0 IS a valid descriptor — the pool silently carried fd 0.  An unimplemented
command that should yield a descriptor must fail loudly.  `sys_dupfd` must skip
0–2 (console-reserved, absent from the fd table).

✅ **§M40 STAGE 1 — UPSTREAM libwayland-client RUNS (2026-08-03, DOCS §4.40,
i386 + x86_64).**  The REAL library (not §M26's mini `user/libwl`) does
connect + get_registry + listener + roundtrip against the d-os server; all 4
globals arrive through libwayland's own libffi closure dispatch.  `make [ARCH=…]
wayland` cross-builds libffi + libwayland-client; `wayland-scanner` runs on the
HOST (in the image) and nothing generated is committed; the vendored tree stays
pristine (build runs in a /tmp copy because wayland `#include "../config.h"`).
Client connects via **`WAYLAND_SOCKET`** (upstream's already-connected-fd
mechanism — no named unix socket needed), which added `proc_set_exec_env()` /
`task.exec_extra_env`: ONE `KEY=VALUE` for the next exec, consumed by it.
**Bug found:** both Linux-ABI layers did recvmsg/sendmsg for AF_INET only, so
libwayland's first read on the UNIX socket returned a bare -1 → musl reported
EPERM; both now route by `sys_fd_kind()`.  Shell: `wayupstream`.  Open: SCM_RIGHTS
in the ABI control path (wl_shm pools), an upstream-driven xdg_toplevel, then a
real toolkit.

✅ **CLOSING A WINDOW IS NOT A CRASH (2026-08-03, DOCS §4.38.1).**  The X button
force-killed a client-managed window's client on the FIRST compositor pass, so
closing a healthy NetSurf was recorded as "unresponsive task reclaimed by force"
and popped the Crash Reports window.  The kill is the FALLBACK for a wedged
client, and the escalation is the USER's: **1st X click = ask, 2nd X click =
force immediately**.  `gui.close_grace_ms` (10000 ms) is only the unattended
backstop.
New `wedgewin` cmd + `user/wedgewin.c` (a client that opens a window then
freezes) is the automated test M46's "chrome works when the app is frozen"
guarantee never had.  **Harness lesson:** step the QEMU mouse in <=100 px hops —
the PS/2 delta is a signed byte, so one big `mouse_move` is clamped and the click
lands elsewhere (the first repro attempt clicked the page and wrongly concluded
there was no bug).

✅ **x86_64 USERLAND PARITY (2026-08-02, DOCS §4.39).**  x86_64 now runs the same
userland i386 does: musl coreutils + `sh`, ring-3 sockets, threads/TLS/signals,
Mbed TLS (crypto + TLSv1.3 + HTTPS w/ CA verify + `wget`), and on-device TinyCC.
The gap was **duplication, not missing kernel support**: objcopy blob symbols
carried the arch in their NAME (`_binary_user_X_i386_elf_start`) so every in-tree
program was i386-only by construction; the program lists were written twice; the
x86_64 dispatcher stopped at M25.  Fixed with one blob pattern rule +
`--redefine-sym`, shared lists above the arch branches, the missing dispatcher
cases and `hal/x86_64/signal.c`.  **Four 32-bit assumptions** surfaced: crt0
never read argc/argv; `thread_create` passed its arg the cdecl way (amd64 wants
RDI); `tls_load4` hard-coded `%gs` AND a literal offset 4 (x86_64 = FS.base, and
the field sits at 8); virtio-net + AC97 were absent from the x86_64 source list.
`make mbedtls` / `make tcc` are arch-aware (`third_party/{mbedtls,tinycc}-<arch>`).
**Pre-existing i386 bug fixed on the way:** `linux_socketcall` validated its
argument array as a USER pointer, but the direct socket syscalls (359+) hand it a
KERNEL array → every direct call -EFAULT and musl doesn't fall back on -EFAULT,
so `socket()` failed.  Split into `linux_socketcall_k` + a gated wrapper.
**musl `getaddrinfo` FIXED (2026-08-03, both arches, was pre-existing).**  Two
defects: (1) `SOCK_NONBLOCK` was discarded — musl drains its resolver socket with
`while (recvmsg(...) >= 0)` and needs the EAGAIN only a non-blocking socket
gives, so we sat in a 40M-iteration spin (minutes on emulated i386 → looked
arch-specific); sockets now carry `nonblock`, honoured by the recv paths and by
`SOCK_NONBLOCK` + `fcntl(F_SETFL)` in both Linux-ABI layers.  (2)
`hostorder_to_sockaddr` validated a KERNEL word (`msg_namelen` from an
already-checked msghdr) as a ring-3 pointer → returned without writing
`msg_name`, and **musl drops any DNS reply whose source doesn't match a queried
nameserver**, so every answer was silently discarded.  **Third instance of the
same lesson** (after `sys_*_k` and `linux_sendmsg`): the user-pointer check
belongs where the pointer's ORIGIN is known, never in a shared helper.

✅ **§M46 — RESILIENCE / freeze-freeness (2026-08-01, DOCS §4.37, i386 + x86_64,
aarch64 parity).**  The rule now enforced: *nothing a user program does can take
the machine down.*  Ring-3 fault ⇒ kill only that process (all 3 arches; ring-0
follows `kernel.fault_policy` halt|reboot|kill).  **Force-kill of a WEDGED ring-3
task** at the timer-preemption safe point (`fkill`, Task Manager "Force kill",
opt-in `package[.<name>].auto_fkill_ms` runaway auto-kill).  **NMI hard-lockup**
recovery via the ib700 HW watchdog (logs the stuck EIP lock-free to COM1, kills
a ring-3 lockup, reboots a kernel one) + spinlock-deadlock reporting +
`scripts/dos-dump.sh` for a frozen guest.  **Chrome works while an app is
frozen:** Ctrl+Alt+Del / Ctrl+Alt+X trapped in the keyboard IRQ, window X
force-kills an unresponsive client.  **Ring-3 pointer boundary in THREE layers:**
a per-syscall gate (`task->in_user_syscall` + `vmm_user_access_ok`), a real
exception table (`.ex_table` + `uaccess_*` — a fault DURING a copy returns
-EFAULT instead of panicking), and **bounce buffers** (2026-08-01) so the bulk
payloads never reach the VFS/socket/console layers as ring-3 pointers — those
dereference deep inside their own call chains where no fixup entry covers them
(`sys_*_k` cores + a gated staging wrapper); `faulttest` proves all three.
Also: dosgui handles
owner-bound + blits range-checked, sigreturn EFLAGS sanitised, `sys_kill`
restricted to the caller's subtree, **x86_64 real COW**, ACPI tables above the
identity map mapped on demand (i386 boots with `-m 512M` again).
**Two lessons worth keeping:** (1) the `sys_*` layer is DUAL-USE (ring-3
dispatchers *and* in-kernel callers) — putting the user-pointer check inside it
broke every kernel caller, so ld.so's `fstat` of each `.so` failed and NetSurf
stopped starting on both x86 arches; a check belongs where the pointer's ORIGIN
is known (`in_user_syscall` + `sys_*_k` cores).  (2) A validity CHECK is not a
guarantee — only the exception table survives a range going bad mid-copy.
Build: `scripts/build.sh` now keeps a **per-arch `user/` artifact cache**
(`build/.userartifacts/<arch>/`), so an ARCH flip no longer re-compiles the
NetSurf + freetype stack (~25 min → instant).

✅ **M1 – M20 + M18.5 + M20.5 + M18.6 + M19.5 + M21 (full ARM parity) +
M22 – M22.7 + M27 + M28 + M25 (incl. Tier B tail) + Tier A + M29 + M30 +
M31 + M24 (net, stages 1–3, i386) + M23 (audio, stage 1, i386) + M34 (POSIX
process model, i386)** shipped
(10/11 polish sub-items; the lone outstanding one is §M20.6.1
SYSCALL/SYSRET).  **M34** (2026-07-11, DOCS §4.27): POSIX process model (i386)
— SysV argv/env/auxv initial stack; **copy-on-write fork** (`vmm_space_clone` +
`vmm_cow_fault` on #PF + `enter_user_mode_regs`); `waitpid` (Tier-A); `execve`
loading `/bin/*` from the VFS; `pipe`+`dup2`; **signals** (sigaction/kill/raise,
return-to-user delivery + `__sig_trampoline`→SYS_SIGRETURN).  Syscalls 14–21;
shell runargs/forktest/forkexec/pipetest/sigtest.  Open: EINTR, sigprocmask,
user #PF→SIGSEGV, x86_64/aarch64.  Next: net socket syscall API → §M35 threads.
**M23** (2026-07-11, DOCS §4.26): audio (i386) — `audio_dev`
registry + AC97 codec driver (BDL bus-master DMA, 48 kHz 16-bit stereo out) +
square-wave tone generator; shell `lsaudio`/`beep`/`tone`; boot-tested via QEMU
`-audiodev wav` (440 Hz ±8000 square wave captured).  Open: `play <path>` WAV
player, `/dev/dsp`, mixer/multi-stream, input, Intel HDA, x86_64/aarch64.
**M24** (2026-07-11, DOCS §4.25): network stack (i386) —
virtio-net driver + `net_device` registry + arch-independent
Ethernet/ARP/IPv4/ICMP/UDP/TCP + DNS stub resolver; shell
`lsnic`/`ping`/`arp`/`nslookup`/`wget`/`nettest`; boot-tested through QEMU
SLIRP (ICMP 3/3, DNS example.com, TCP `HTTP/1.1 200 OK`).  RX polled from the
calling task (no IRQ/lock yet); TCP client-only, no retransmit/congestion.
**Stage 6 (2026-07-11): BSD socket API to userland** — `FD_NETSOCK` +
`socket`/`bind`/`connect`/`sendto`/`recvfrom` (syscalls 22–26), ring-3 UDP+TCP;
`dnstest`/`httptest` resolve + fetch a page from ring 3.  Open: sockaddr,
multiple TCP conns, IRQ RX, DHCP, IPv6.  **M35** (2026-07-11, DOCS §4.28):
threads + futex + TLS (i386) — `proc_clone` (shared address space, `mm_shared`)
+ `futex` (SYS_CLONE/SYS_FUTEX) + libc `thread_create`/`thread_join` + `%gs`
thread-local storage (SYS_SET_TLS, per-CPU GDT TLS descriptors); tested
20000/20000 (threadtest) + tlstest 0-mismatch on **UP and `-smp 2`**.  Also
fixed a pre-existing gap it exposed (ring-3 tasks didn't run on APs) with a
**per-CPU TSS** (array in tss.c + one GDT descriptor per CPU + each CPU LTRs its
own) — unblocks all ring-3-on-AP.  **Tier A** (2026-07-10, DOCS §4.20): blocking
primitives — `waitq` (block/wake, lost-wakeup-free, SMP cross-CPU wake;
`TASK_SLEEPING` now real), `task_wait(pid,&code)`, blocking socket
read + `poll(timeout<0)`, `task_msleep`.  **M29** (DOCS §4.21):
services — supervisor (`SERVICE()` + `task_wait` restart w/ backoff +
config gate + `service` cmd + `/proc/services`) + service bus
(endpoint/contract\@ver/transport, strict bind + opt-in `BUS_ADAPTER`
gated by `bus.allow-adaptation` + `/proc/bus`).  **M31** (DOCS §4.22):
watchdog — L1 per-task heartbeat (`watchdog_register/kick` → detect +
kill-tree + M29 restart) + L2 per-CPU softlockup (`percpu.ticks`);
`/proc/watchdog` + `wdtest`; L3 HW watchdog deferred.  **M30** (DOCS
§4.23): cron — itself an M29 service; `CRON_JOB()` registry + interval
schedules (`/etc/crontab` / config) + `/proc/cron`.  **M25 Tier B tail**
(DOCS §4.24): concurrent preemptible user processes (`proc_spawn`,
per-task TSS.esp0/rsp0 via `hal_set_kernel_stack`; SP_EL1 auto on ARM;
one-way `enter_user_mode`; SYS_EXIT→task_exit; `user_task` flag) +
**full-arch libc** (arch-cond `syscall3` + per-arch crt0 + Makefile
USER_* knobs; `hello`/`spin` build on all 3; `SYS_GETPID`); tests
`procspawn`/`libctest` green on i386/x86_64/aarch64.  M28 (2026-07-10):
system log — klog static ring
+ `kprintf` auto-tee + `klog(level,tag,…)` + `dmesg [-l level]` +
`/proc/kmsg` (DOCS §4.18).  M25 (2026-07-10): userland foundation
stages 1–7 (DOCS §4.19) — per-process address spaces (`vmm_space` +
`task.mm`, scheduler CR3/TTBR0 switch), ELF loader (`elf.c`) + run
(`proc_exec_elf`, ring3/EL0 excursion), fd table + `write/read/open/
close/lseek/mmap/memfd/socketpair/send/recv/poll` (generic `struct
ofile`), memfd shared memory (`VMM_SHARED` PTE bit), unix socketpair +
SCM_RIGHTS fd passing (`usock.c`), poll, in-tree libc (`user/`,
compiled-C runs in ring 3).  All on 3 arches (libc now all 3 via Tier B).
**Ring model: ring 0/3 (EL1/EL0) only — and the reasoning is now
UNDER REVIEW rather than closed (§M68).**  The standing answer is
unchanged: paging carries ONE privilege bit, so rings 0/1/2 are all
supervisor to the MMU and a ring-1 driver can write every kernel page.
Rings 1/2 have ever bought real isolation only through SEGMENT LIMITS —
which **i386 enforces and x86_64 ignores, and aarch64 has no analogue
of**, so the ring axis would give its strongest isolation on the oldest
target and none on the two that matter.  §M68 is the investigation that
must produce that verdict WITH A MEASUREMENT (and the capability report
that is worth building either way); the modern instrument for the same
goal is protection keys / IOMMU / virtualization, not ring count.  The former deferred tail (concurrent
preemptible user processes + x86_64/aarch64 libc) SHIPPED as Tier B
(DOCS §4.24) — `proc_spawn` runs many at once; the synchronous excursion
(`proc_exec_elf`) is kept for the self-tests.  Self-tests: `userrun/
fdtest/shmtest/socktest/polltest/libctest/waittest/procspawn`.  Still
open: force-kill of a wedged pure-ring3 task (needs M25/§M33 isolation),
argv/env, fork/COW.  M22 + M22.1 + M22.2 (2026-07-04): GUI — gfx
surfaces + compositor + WM core + widget toolkit + file manager,
PS/2 mouse (IRQ12), CMOS RTC, `vfs_unlink`, 1280×800 FB; desktop
shells + apps + command shells are REGISTRY-swappable
(`DESKTOP_SHELL()` / `GUI_APP()` / `SHELL_PROVIDER()` linker
sections; `gui.shell` + `shell.provider` config keys; vista + bare
desktops, d-os + rescue shells, apps under `kernel/gui/apps/`,
`launch` command); GUI dev guide in DOCS §4.14.  M22.3: task
manager app, cooperative task_kill/reap (kthread contract) +
cpu_ms, terminal-window close, minimize, Alt-Tab, dirty-rect
composition (`gui stats`).  M22.4 (2026-07-04): compositor
smoothness — cursor-damage race fix (compositor-side bookkeeping),
rect-bounded drag damage, tearing notes; instant Task Manager
(task_set_change_hook + DEAD reaping via vc_task_bound).  M22.5
(2026-07-04): desktop apps — nav keys end-to-end (PS/2 E0 → HID →
widget keycode events), multiline editor widget + kernel clipboard,
Editor app, Tiny-BASIC (`core/basic.c`, BASIC window via
gui_window_create_task, `run <path>` cmd), file manager 2.0 (path
bar, sorting, Ren/Copy/recursive-Del, GUI_APP_ASSOC extension
associations, vfs_rename/vfs_copy/vfs_unlink_recursive),
maximize/restore.  M22.6 (2026-07-04): tear-free presentation —
Bochs-VBE hardware page flip (DISPI VIRT_HEIGHT double buffer +
Y_OFFSET pan; buffer-age-2 dirty∪prev copy; graceful fallback to
single-buffer blit), plus the QEMU display-scaling fix
(zoom-to-fit=off); corrects M22.4's "not fixable" tearing note.
Same session: 1920×1200 desktop (needs `-device VGA,vgamem_mb=32`
for the double buffer + `BUDDY_MAX_ORDER` 10→12 for 9.2 MiB
contiguous surfaces + `-m 256M`), and terminal-window auto-close
when its hosted task dies (flagged at TASK_DEAD → reused close
teardown → also leaves the Task Manager list).  M27 (2026-07-04):
process model — `struct task` gains ppid/exit_code/reap_owned; an
always-on **init** task universally reaps DEAD non-owned tasks
(closes the zombie-leak gap) + re-parents orphans; `task_kill_tree`
takes a subtree down (GUI window close uses it); `task_spawn_detached`
(parent=init) for daemons; ps + /proc/tasks grow PPID, Task Manager
shows a process tree; pid 0 + init reap-guarded.  M22.7-A (2026-07-05):
per-task GUI apps — every WIN_APP window runs on its own `app:<name>`
task (`app_host_main` + `task_spawn_arg`); compositor = surface-
compositor + input router (per-window `aq` queue, host does widget
dispatch + render + tick); host↔compositor teardown dance; apps now
visible/killable in the Task Manager, a slow app no longer freezes the
GUI.  M22.7-B: the desktop shell/taskbar runs on its own `desktop` task
too (full-screen `panelsurf`; compositor composites taskbar strip +
launcher popup on top; input via `pevq`).  **Net: the compositor is now
a pure surface-compositor + input router; windows, apps AND the panel
are each their own task (the M26 Wayland shape, internal API).**
M22.7 refinements (2026-07-05): idle loops halt only when idle (was:
every iteration → cursor lag with menu/taskman open); vista_motion is
chrome-only repaint not full recompose; **app launches moved to the
desktop task → launched apps are children of `desktop`, not the
compositor**; `panelsurf` is a bottom strip not full-screen (~5 MiB
saved); bare shell reserves a hint strip.  Session vs detached GUI
shells: `task_spawn_under(name,entry,ppid)` parents a launched terminal
to the desktop ("New Shell" = session, dies with the desktop) or to
init ("Detached Shell" = outlives the session — nohup/tmux-detach in a
GUI).  GUI session root: `gui_start` spawns `desktop` first, parents
compositor + windows under it (`boot-shell → desktop → {compositor,
apps}`); no auto-started shells — the GUI boots as a bare desktop
(wallpaper + taskbar), user launches from Start.  Damage is now a LIST
of disjoint rects (was a single bounding box) — `compose()` paints +
presents each rect separately, so a Task Manager refresh + a far-away
cursor stay two small blits instead of one huge union (fixed the
cursor stutter: ~630 KB/frame vs ~2.4–5.3 MB).  Plus: a window click
damages only the two affected windows (was a full 9 MB frame), and the
Task Manager repaints only its listview (`gui_window_request_redraw_rect`).
All on both archs.
Highlights so far: VFS + ramfs + exFAT on virtio-blk, devfs +
procfs, preemptive scheduler, multi-pane shell, xHCI USB + HID,
keyboard layouts, HAL cut (`hal_api.h`), **SMP on i386 + x86_64**
with per-CPU runqueue + load balancer + per-CPU preempt_count + task
affinity (`taskset`) + cross-CPU preempt IPI + MSI/MSI-X allocator,
memory at scale (per-zone buddy PMM + slab + per-CPU magazines +
empty-slab caching + x86_64 HIGHMEM via 1 GiB-page identity-map
extension + ACPI SRAT-derived per-CPU NUMA nodes), APs scheduling,
**x86_64 (long mode) — full parity with i386 INCLUDING xHCI USB +
virtio-blk + exFAT**.  `m20_stubs.c` is empty.

▶️ **DECIDED NEXT (2026-07-12): REAL musl RUNS → coreutils → §M35.5 store next.**
§M36 IN PROGRESS.  **Stage 1** (syscall breadth: stat/fstat/getdents/uname/
clock_gettime/nanosleep + errno; DOCS §4.30).  **Stage 2 = "two brothers"**
(design settled; parked own-libc debate in `NATIVE_LIBC.md`): Role B (ecosystem
libc) via TWO peers onto the SAME kernel primitives — (a) **Linux-ABI peer**:
PRISTINE vendored musl → Linux numbers → isolated `kernel/hal/x86/linux_abi.c` +
`task->linux_abi` personality (doubles as §M41); (b) **native musl-fork peer**:
a light `arch/dos/` musl fork → d-os numbers → native `syscall.c` (store
default).  **Linux-ABI peer GOAL ACHIEVED (DOCS §4.31): an unmodified static
musl binary runs on d-os** — `make musl` builds static i386 musl
(`third_party/musl-i386/`), `user/muslhello.c` (stdio/printf) links against
musl crt1/libc.a into a stock Linux ELF (`-Ttext-segment=0x40000000` + libgcc),
run by the **`musltest`** cmd under the personality; prints via real musl
`printf`, rc=0, ZERO unhandled syscalls.  Startup welds: `set_thread_area`
(→§M35 `%gs` GDT-TLS), `auxv` (`AT_PAGESZ/CLKTCK/RANDOM/SECURE` in
`build_initial_stack`), `set_tid_address`, `ioctl`→ENOTTY.  **musl COREUTILS in
the store — DONE (§4.31):** `echo`+`cat` (generic `user/%.muslelf` pattern) are
`pkg install`ed into the §M35.5 store + run FROM `/store` by `pkgrun <name>
[args]` — real argv + musl file I/O.  **The ABI is DATA-DRIVEN (the swappable
seam the user demanded): a package declares `.abi` (`pkg_recipe.abi`), `pkg_run`
maps it → personality in ONE place (`abi_to_personality`) — no hardcoded
"musl"/"linux"** (see memory [[feedback-dos-swappable-layers]] + `NATIVE_LIBC.md`).
`linux_abi.c` grew open-flag xlat/openat/readv/mprotect/munmap + an `mmap2`
decode fix.  **Coreutils `echo`/`cat`/`ls`/`env` + a real (non-interactive)
`sh` DONE:** `pkgrun sh -c "echo a; echo b; ls /store"` forks 3 children, each
execve's a coreutil from `/bin` (fork/execve/waitpid/rt_sigprocmask in
linux_abi; `pkg install` exposes `/bin/<name>` + `PATH=/bin`).  Forced two
fixes: **TLS-after-fork** (proc_fork inherits has_tls/tls_base; child %gs=TLS
selector via g_entry_gs) + a **pre-existing COW double-fork bug** in
vmm_space_clone (already-COW page misclassified as RO code → fixed by routing
VMM_COW through the COW branch).  **Two-brothers SEAM PROVEN with a native
backend:** `pkg_run` logs the backend; `pkgrun hello` (in-tree d-os libc,
`abi=native`) → native syscall path, `pkgrun echo` (musl, `abi=linux`) →
linux_abi — same store, two real backends by data.  The minimal 2nd brother =
the in-tree native libc; the **full native musl (`arch/dos` fork) is PARKED**
(`NATIVE_LIBC.md`) — it needs musl `src/` shape patches (bare-base SYS_SET_TLS,
`(len,fd)` mmap, `kstat`), not a clean `arch/` add → a separate project.
**Checklist in `third_party/MUSL.md`.**  **§M26 Wayland STARTED — stage 1+2 (DOCS
§4.32, i386+x86_64, `kernel/gui/wayland.c`, shell `waytest`; hand-marshalled
client à la linuxhello): stage 1 = real wire protocol + wl_display/wl_registry/
wl_callback handshake; stage 2 = the SHM BUFFER PATH — bind + wl_shm(formats) +
create_surface + create_pool (client memfd passed OUT-OF-BAND via SCM_RIGHTS) +
create_buffer + attach + commit → the server reads the client's pixels back
(4×4 0x3366CCFF → top-left+checksum verified); stage 3 = xdg_shell top-level
(bind xdg_wm_base → get_xdg_surface → get_toplevel → configure pair → set_title
→ ack_configure).**  Also this session: **interactive `sh`** (cooked stdin via
`vc_focused`/`vc_getchar` — `pkgrun sh` → `d-os$` REPL) and **x86_64 build parity
restored** (trampoline stubs + net/audio/futex/pkg cores; Wayland runs on x86_64
too).  **§M26 CORE COMPLETE (i386+x86_64): wire handshake + shm buffers
(SCM_RIGHTS) + xdg_shell + framebuffer bridge (`waydemo` VISIBLE OK) + a
WM-managed `gui_window` target (`gui_window_blit`; `waywin` IN-WINDOW OK) +
`wl_seat` input (`wl_send_key`/`wl_send_motion`; `wayinput`) + a REAL ring-3
client (`user/wlclient.c` speaks the wire protocol over an inherited fd 3, server
runs on its own `wl_conn_serve` task; `wayclient` parses 4 globals from user
space) + **server-per-surface** (`wl_conn.wm_mode`: `xdg get_toplevel` spawns a
`gui_window`, commits fill it, input routed to the client's wl_seat via
`gui_window_set_input_hook`; `waycomp` = SURFACE-IN-WINDOW OK + key/motion) + a
**mini-libwayland client library** (`user/libwl` + `user/wlapp.c`; `wayapp`).
The UPSTREAM libwayland port landed as **§M40, now COMPLETE** (DOCS §4.40 +
§4.40.1, x86_64): upstream libwayland-client cross-built for musl runs
`weston-simple-shm` UNMODIFIED in a d-os window, and **Mesa EGL + GLES2 on
gallium softpipe** spins a shader-drawn triangle presented through `wl_shm`
(`egltri win`).  Two lessons: libwayland must be a SHARED object (libEGL had
absorbed it statically — two protocol object tables in one process crash the
first event dispatch), and **`mincore` must answer truthfully** — stubbed as
"succeed and ignore" alongside `madvise`, it told Mesa every address was mapped,
so Mesa dereferenced the literal 3 stored in a version-3 `wl_egl_window`.**  The
desktop label is now dynamic (`kernel/includes/version.h` `DOS_MILESTONE` — bump
it when a milestone ships).  Also open: more coreutils, tty line-editing/`isatty`.
§M35 (threads/futex/TLS/per-CPU TSS) COMPLETE (UP+SMP, §4.28); also: §M34 POSIX
(§4.27), §M24 sockets (§4.25), §M35.5 store (§4.29).  **§M26 Wayland deferred
until POSIX + libc exist.**

🔲 **Other options** (was "pick one"; superseded by the decision above):

- **§M58–§M62 — desktop UX cluster** (design only, PLAN.md; added 2026-08-21 from
  use): **§M58 text selection** (no transport today — `widget_ops.mouse` has
  click + double-click, so a drag cannot even be expressed; needs a pointer grab
  + a selection MODEL, byte range for text and cell rectangle for the terminal
  grid), **§M59 system-wide clipboard** (M22.5's kernel clipboard is
  in-kernel-widgets only — its header says the userland protocol arrives with
  §M25/§M26, both shipped; wants typed offers + §M50 ops + `/dev/clipboard` +
  Wayland `wl_data_device` + a primary selection), **§M60 wallpaper** (a
  `gfx_vgradient` call today; image source + fit mode, decode in ring 3, gradient
  fallback on a bad file), **§M61 runtime resolution change** (the mode is an
  assembler literal in `boot.s` / `FB_WIDTH` on ARM — needs `fb_mode_set` behind
  the `fb_present.h` seam and then a full scene resize; the hard half, telling a
  client its canvas moved, already exists as §4.60's dosgui RESIZE), **§M62
  switchable boot splash** (`boot.splash`, embedded image, log suppressed not
  discarded, **torn down by any fault** — a splash over a panic turns a
  diagnosable crash into "it froze at the logo").  **§M63 Vezérlőpult / Control
  Panel** — a `SETTINGS_PANEL()` linker registry so §M60/§M61/§M62 each ship a
  panel instead of an app, panels open as their own §M22.7 windows, ONE Start-menu
  entry (`SM_MAX_APPS` is 10 and 8 `GUI_APP`s are registered — one entry per
  setting does not fit); hosts Display / Personalisation / System / Packages
  (§M35.5+§M45 as a panel, not an app) / Region.  **Its stage 0 is a bug in
  disguise: settings do NOT survive a reboot** — `config_save()` writes
  `/etc/d-os.conf` on ramfs while the persistent volume is exFAT at `/mnt`, and
  `config_init()` (kernel.c:144) runs 125 lines BEFORE that mount (kernel.c:269).
  **§M64 desktop shortcuts** — icons on the wallpaper, shortcuts only; a shortcut
  is a FILE (`/desktop/*.lnk`) so fileman/`ls`/`rm` already work on it; the real
  cost is that **no icon exists anywhere in this system and `GUI_APP` has no icon
  field**; never auto-populate.
- **M21** — aarch64 port.  Third arch, real torture test of HAL
  portability (no port I/O, GIC instead of APIC, EL1/EL0 instead
  of rings).  ✅ **Phase A–M shipped — FULL x86 parity** (2026-07-07..10,
  DOCS §4.17) — boot + SMP + virtio-blk + exFAT + DTB + framebuffer +
  EL0 userspace + **full shell.c + M22 GUI** (kbd/mouse) + **USB (xHCI+HID
  over PCIe ECAM)** on ARM64:
  A = raw-ELF boot on QEMU `-M virt` (no GRUB/multiboot), EL2→EL1 drop,
  PL011 UART, EL1 exception vectors, MMU identity map on;
  B = GICv2 (GICD 0x08000000 / GICC 0x08010000) + ARM generic timer
  (CNTP, INTID 30) + IRQ dispatch API;
  C = context switch (switch.S over x19–x30) + full hal_arch.c
  (DAIF/wfi) + PMM/kmalloc (stock pmm/slab/kmalloc; synthesised RAM map;
  BUDDY_MAX_FRAMES 4 GiB cap) + PL011 console sink + the stock
  preemptive scheduler (task/percpu/lock with UP stubs);
  D = interactive serial shell (`serial_shell.c` REPL on a scheduler
  task, PL011 RX poll+yield) + VFS + ramfs (stock vfs/ramfs/block/module)
  — ls/cat/mkdir/write/rm/ps/meminfo work over the UART;
  E = SMP via PSCI (`smp.c` + `smp_entry.S`) — secondary cores join the
  STOCK per-CPU runqueue + load balancer (percpu topology hook =
  MPIDR.Aff0; per-CPU mmu/gic/timer bring-up); verified two hogs running
  on two cores in parallel (`AARCH64_MAX_CPUS`+`-smp`, shipped at 2);
  F = virtio-MMIO block driver (`virtio_mmio_blk.c`, modern/version-2
  transport) → `/dev/vda` on the stock block layer; write→read self-test
  + shell `blk` command (needs `-global virtio-mmio.force-legacy=false`);
  G = exFAT at /mnt off /dev/vda — the STOCK block_cache.c + exfat.c link
  unchanged (arch-independent); shell ls/cat/write/rm hit persistent disk,
  writes survive a reboot;
  H = device-tree (FDT/DTB) parsing (`dtb.c`) — discovers RAM size + CPU
  count, sizes the PMM to the actual `-m` (DTB loaded at 0x48000000 via
  `-device loader`; falls back to defaults);
  I = virtio-gpu framebuffer (`virtio_gpu.c`) — QEMU `virt` has no
  VGA/Bochs-VBE, so the display is a virtio-gpu on a virtio-MMIO slot; a
  1280×800 2D scanout backed by a contiguous RAM framebuffer runs the *same*
  portable `fb_terminal.c` x86 uses (boot log + shell render graphically).
  The one x86-only bit of fb_terminal (Bochs-VBE port I/O + vmm map) was
  hoisted behind `fb_present.h` — `fb_present_map` + `fb_present_flush`
  (x86: no-op, linear FB is the scanout; ARM: virtio-gpu transfer+flush) —
  and the M22.6 page flip moved to `kernel/hal/x86/fb_present.c` (gui.c
  unchanged); i386 GUI re-verified regression-free;
  L = EL0 userspace substrate (`vmm.c` per-process TTBR0 spaces + EL0-page
  mappings; `usermode.S` `eret`-to-EL0 + SYS_EXIT teleport; `syscall.c` SVC
  dispatcher, x8=num/x0..x5=args, shared `syscall.h`; ESR.EC==0x15 decode in
  `exceptions.c`).  `usertest` runs a program at EL0 → SYS_PRINT/SYS_EXIT.
  This is the ARM analogue of x86 M6/M20.5 ring-3+`int 0x80` → **all 3
  arches are now M25-ready** (each can enter user mode + service a syscall);
  J/K = the *same* full `shell.c` on a VC + the **M22 GUI** (compositor +
  taskbar + PL031 clock + windows) driven by **virtio-input** kbd/mouse over
  the virtio-gpu framebuffer.  Portability shims: `arch_ringtest()`, PSCI
  `hal_shutdown/reboot`, `pl031_rtc.c`, `fb_present_flush()` in gui.c's present
  path, `virtio_input.c`.  **Scheduler lesson:** pid 0's idle loop must
  `hal_intr_enable()` each pass (like `cpu_idle_entry`) — a bare `for(;;)
  hal_cpu_halt()` wedges the CPU if DAIF masks IRQs (wfi wakes but won't take a
  masked IRQ) → its timer stops → it stops scheduling → CPU-homed tasks starve.
  aarch64 runs its OWN `main_entry.c` (NOT the x86-coupled kernel_main), builds
  via a separate `Dockerfile.aarch64`.  M = USB: a new PCIe-ECAM layer
  (`kernel/hal/aarch64/pci.c` — config via MMIO at 0x40_1000_0000 + BAR
  assignment, no firmware) lets the stock `xhci.c` + `usb_hid.c` link + run
  (MMIO, polled from the timer ISR); a USB HID keyboard drives the shell.
  **aarch64 now has full x86 parity** — M21 complete.
- **M23** — Audio — ✅ stage 1 shipped (i386, DOCS §4.26): AC97 PCM output +
  tone (`lsaudio`/`beep`/`tone`).  Open: WAV player, /dev/dsp, mixer, input,
  HDA, x86_64/aarch64.
- **M24** — Network — ✅ **COMPLETE** (DOCS §4.25 + §4.59, all 3 arches):
  virtio-net (PCI + virtio-mmio) + loopback + routing, ARP/IPv4/ICMP/UDP/TCP
  with a real connection table, listen/accept, retransmission, DHCP, /proc/net,
  and the socket ABI as shared §M50 operations.  Open: reassembly queue,
  congestion control, RTT-estimated RTO, an interrupt for the ARM NIC, IPv6.
- **§M19.5.1 i386 kmap** — the deferred half of HIGHMEM: real
  kmap-style temp mappings so i386 can manage > 256 MiB of RAM.
- **§M19.5.3 per-NUMA-node PMM zones** — the deferred deeper half
  of SRAT integration; today the parser populates per-CPU node IDs
  but PMM still has a single zone set.
- **§M20.6.1** — SYSCALL/SYSRET instruction path (needs GDT slot
  reorg to satisfy SYSRET's selector arithmetic).

🔲 **PLAN extensions (placeholders, design only):**
- §M23 — Audio subsystem (AC97 → HDA → I2S).
- §M24 — Network stack — ✅ stages 1–3 shipped (i386, DOCS §4.25); socket
  syscall API + DHCP + IPv6 + x86_64/aarch64 still open.
- §M25 — ✅ SHIPPED stages 1–7 (DOCS §4.19) + Tier B tail (DOCS §4.24,
  concurrent preemptible user processes + full-arch libc): per-process
  VMM, ELF loader + exec, fd table, mmap + memfd shm, unix sockets + fd
  passing, poll, in-tree libc (all 3 arches), `proc_spawn`.  Wayland
  prerequisites in place.  Blocking primitives (Tier A, DOCS §4.20:
  waitq / task_wait / blocking read+poll / task_msleep) also shipped.
- §M26 — Wayland server (wire protocol over M22 compositor +
  M25 substrate; depends on both).  **Now the next natural target — its
  M25 + M22.7 prerequisites are all in place.**
- **Workload-management cluster** (order M27→M30 — ✅ ALL SHIPPED):
  - §M27 — ✅ SHIPPED (DOCS §4.15): init + parent/child hierarchy +
    universal reaper + kill-tree + task_spawn_detached + ps/procfs
    PPID + Task Manager tree.
  - §M28 — ✅ SHIPPED (DOCS §4.18): klog static ring (seq + ms + printk
    severity + tag + msg); `kprintf` auto-tees via `emit`→`klog_feed_char`;
    `klog(level,tag,fmt,…)` structured entry; `dmesg [-l level]` +
    `/proc/kmsg`.  (Pitfall: `va_list` is an array type on x86_64 — forward
    it by `va_copy`, never `&`-a-parameter; see the §M28 lesson.)
  - §M29 — ✅ SHIPPED (DOCS §4.21): `SERVICE()` registry + supervisor
    (autostart + restart policy w/ crash-loop backoff, `task_wait`-driven,
    config gate, `/proc/services`) — systemd-lite — PLUS the **service
    bus** (endpoint / contract\@version / transport; strict bind + opt-in
    `BUS_ADAPTER` gated by `bus.allow-adaptation`; `/proc/bus`).  Contracts
    marshalling-shaped so a `LocalCall` service can later move to
    IPC/SharedMemory.  The bus makes §M33 execution domains a config
    (not code) decision.  Non-local transports still reserved for real
    isolation.
  - §M30 — ✅ SHIPPED (DOCS §4.23): cron — itself an M29 service;
    `CRON_JOB()` registry + interval schedules (`/etc/crontab` / config,
    run-once-no-backfill) + `crontab -l` / `/proc/cron`.
  - §M31 — ✅ SHIPPED L1+L2 (DOCS §4.22): watchdog — per-task heartbeat
    (detect + kill-tree + M29 restart) + per-CPU softlockup; `/proc/
    watchdog` + `wdtest`.  L3 (HW watchdog device) deferred.  Blocking
    substrate = Tier A (DOCS §4.20).
- **§M32 — Multi-user** (design only): credentials (uid/gid) on tasks,
  `/etc/passwd`-style user DB, login/sessions, VFS file ownership +
  rwx perms, privilege gating, per-user process isolation.  Hard-depends
  on §M25 (real isolation needs per-process address spaces; today's
  ring-0 kthreads share one, so users would be advisory until then).
- **§M33 — Execution domains** (design only): a service's run location
  (`DOMAIN_KERNEL` / `USER` / `ISOLATED`) is a *declared capability*
  (`.domains` field), config *chooses* among the declared set; the §M29
  broker resolves domain → transport at bind.  Domain constrains
  transport (KERNEL→LocalCall, USER/ISOLATED→IPC/SharedMemory).  Only
  `KERNEL`+LocalCall is real today; `USER`/`ISOLATED` reserved until
  §M25 (no isolation theatre).  Flagship case = switchable **driver
  placement** (Tier 0 fault-tolerant in-kernel hosting → Tier 1
  user-mode non-DMA → Tier 2 DMA+IOMMU); the driver-runtime "narrow
  waist, two backends" IS the M29 transport abstraction.  Hybrid kernel
  (NT/XNU), not a micro-vs-monolith flip.
- **§M35.5 — Package manager & isolation** — ✅ **store slice shipped** (i386,
  DOCS §4.29): content-addressed `/store/<hash>-name-ver/` + profiles + GC
  (`pkg …`/`pkgtest`).  Design (rest still open) — a hard **gate before any
  porting** — a **content-addressed store** (Nix/Guix-
  shaped, NOT dpkg/apt; convention #6): immutable `/store/<hash>-name-ver/`
  paths, pinned dependency closures (versions coexist, no global `/lib`
  soup), hermetic §M33-sandboxed builds, symlink-profile + GC (no cruft,
  rollback), text recipes.  Two-level isolation: §M37 RPATH (load) +
  §M25/§M33/§M32 FS-view (run).  Gates §M36–§M42 (every port installs into
  the store, never the global FS).  Satisfies: isolate ports, no clutter,
  minimal version coupling.
- **Userland maturation §M34–§M42** (design only, PLAN.md): **the goal is
  the POSIX platform, NOT a browser** — each milestone is independently
  necessary and valuable (unblocks shells, build tools, servers, native
  apps, language runtimes); §M42 (browser) is only the *completeness
  proof / bonus*, not the driver.  §M34 POSIX process & signals
  (fork/execve-argv/waitpid/pipes/job-control/signals — the general POSIX
  abstraction layer) → §M35 threads & futex (clone/TLS/pthreads/futex on
  the SMP scheduler) → §M35.5 pkg store → §M36 POSIX syscall breadth +
  native libc (musl port) → §M37 dynamic linking (ld.so/`.so`/dlopen) →
  §M38 C++ runtime + support libs (libc++/unwind, zlib, freetype, ICU,
  harfbuzz, Skia…); side-branches §M39 crypto+entropy+TLS+DNS (`/dev/
  urandom`, mbedTLS/BoringSSL, getaddrinfo — needs §M24) and §M40 client
  graphics (libwayland-client + Mesa `llvmpipe` EGL/GL + Skia — needs
  §M26); §M41 optional Linux syscall ABI shim (binary-compat accelerator,
  useful on its own).  §M42 validation target only: NetSurf (realistic
  first) → WPE-WebKit → Firefox/Chromium (multi-year north star).
  Hard-depends on §M25; target x86_64/aarch64 (i386 out of scope for the
  heavy ports).
- **§M-registry** — Windows-style registry PARKED (accidental history;
  /etc + procfs already covers it).

## Hard conventions (do NOT deviate without asking)

1. **Heavy English comments in code.**  Conversation with the user is
   in **Hungarian** — reply in Hungarian.
2. **Drivers self-register** via `MODULE()` (legacy) or `DRIVER()`
   (probe/init/shutdown lifecycle) — never edit `kernel_main` to wire
   a driver in.
3. **Arch portability:** everything x86-specific lives under
   `kernel/hal/x86/`.  Core code (`kernel/core/`, `kernel/mem/`,
   `kernel/fs/`, portable drivers) must NOT do `__asm__`, port I/O,
   reference descriptor tables, or assume page-table layout.  Target
   arches: x86 (now), x86_64, aarch64.
4. **SMP-ready on UP:** lock + per-CPU APIs in place even when no-op
   today.  Don't ship code that would have to be hunted down later
   for a second core.
5. **Stable interfaces from day one.**  Define the final API shape
   even when the first implementation is a stub.  Don't ship "we'll
   wrap it later."
6. **Linux-inspired, not Linux-bound.**  Adopt the patterns that
   solve a concrete problem we have; reject what's accidental
   history (`kobject`, sysfs, RCU until needed, namespaces, cgroups).

## Where to read (on demand, not eagerly)

- **DOCS.md** — current state, per-component reference.  Has a TOC at
  the top — use `Read` with `offset`/`limit` to land in a specific
  section.
- **PLAN.md** — roadmap + design sketches for upcoming milestones.
  Same TOC pattern.
- **README.md** — public-facing intro.
- Source: arch-independent under `kernel/{core,mem,fs,drivers}/`;
  x86 specifics under `kernel/hal/x86/`.

## Build / run

```sh
./scripts/build.sh                    # default ARCH=i386 → build/i386/d-os.iso
./scripts/run_qemu.sh                 # i386 GUI window, NO disk attached

ARCH=x86_64 ./scripts/build.sh        # → build/x86_64/d-os.iso
ARCH=x86_64 ./scripts/run_qemu.sh     # x86_64 in qemu-system-x86_64

# Per-arch convenience wrappers (thin shims over the ARCH= scripts above):
./scripts/build-i386.sh   ./scripts/run-i386.sh
./scripts/build-x86_64.sh ./scripts/run-x86_64.sh
./scripts/build-aarch64.sh ./scripts/run-aarch64.sh   # ARM64 (raw ELF on -M virt)
```

`make clean` wipes the current ARCH only; `make clean-all` wipes all
builds.  **Header dependencies are tracked** (`-MMD -MP`, §M65): editing a
shared header rebuilds exactly the objects that include it, so the old
"remember to `make clean` after touching a header" rule is gone — it was
forgotten three times in one day, and the failure it produces is not a link
error but a wrong struct offset (a jump through a garbage pointer).

For block-layer / future-fs testing, the disk image must be attached
manually (the script intentionally doesn't add `-drive`):

```sh
dd if=/dev/zero of=build/test.img bs=1M count=4   # once
qemu-system-i386 -cdrom build/i386/d-os.iso \
    -drive if=virtio,file=build/test.img,format=raw
```

For headless / automated testing (capture serial log):

```sh
qemu-system-i386 -display none -no-reboot \
    -serial file:/tmp/serial.log -monitor stdio \
    -m 256M -cdrom build/i386/d-os.iso \
    -drive if=virtio,file=build/test.img,format=raw
```

NB: a FORMATTED image (e.g. `build/exfat.img`, mkfs.exfat'd in the
Docker container) carries a boot signature — add `-boot d` or SeaBIOS
boots the empty disk instead of the CD and hangs with no serial
output at all.

Block / USB drivers are i386-only today; x86_64 boots without them
(virtio-blk + xhci need a 64-bit DMA-path revisit — M20.5+).

## Session etiquette

- Use **TodoWrite** for any multi-step work (every milestone qualifies).
- When a milestone ships:
  - Add a component section to **DOCS.md** (under `## 4. Components`).
  - Add a change-log entry to DOCS.md (`## 8. Change log`).
  - Flip the PLAN.md status table row to ✅ and condense the design
    section to a one-paragraph "Shipped, see DOCS.md §…" pointer.
  - Bump `DOS_MILESTONE` in `kernel/includes/version.h` to the new M number
    (the desktop wallpaper draws it — always show the latest shipped milestone)
    **and clear `DOS_MILESTONE_NOTE`**, whose whole job is to name an OLDER
    section finished after that number shipped (`M57 (updated M24)`); once a
    newer number exists the note is advertising old work as fresh.  A shared
    header, so `make clean ARCH=<arch>` — the label is drawn from a constant
    compiled into gui.c, and a rebuilt kernel with a stale gui.o shows the old
    string while every source file says otherwise.
- **Boot-test in QEMU** before claiming done.  For most milestones a
  sendkey-driven script + `-serial file:` capture is enough; for the
  framebuffer text path, `pmemsave 0xb8000` + a small Python script
  renders the cells to ASCII.
- **Pitfalls hit during bring-up** go into BOTH the source comment
  (so future readers see why) AND the PLAN.md milestone as a "Lesson
  learned" note (so the design-time intuition is preserved).

## Where things live (when you're modifying)

| Concern                              | File                                  |
|--------------------------------------|---------------------------------------|
| Boot order, new milestone wiring     | `kernel/core/kernel.c`                |
| Adding a shell command               | `kernel/core/shell.c`                 |
| Adding a new .c to the build         | `Makefile` (C_SRCS or ASM_SRCS)       |
| New linker section                   | `linker.ld`                           |
| New driver class                     | `kernel/includes/<class>.h` + impl    |
| x86 arch primitives                  | `kernel/hal/x86/`                     |

## Memory pointers (cross-project user preferences)

These live under `~/.claude/.../memory/` and are auto-indexed in
MEMORY.md:
- `feedback_dos_style.md` — heavy English comments, Hungarian
  conversation, keep DOCS.md current.
- `project_dos_arch_goals.md` — modular driver registry, multi-arch
  HAL, multi-session shell, PLAN.md is the roadmap.
