# Where things stand (2026-09-24)

## §M83 is written up in PLAN.md — everything a package, repos split last

Asked for directly: separate repositories where possible, everything modular,
everything (desktop, compositor, GUI, Wayland) manageable from `pkg`.
**Designed, not started.**  The measured starting point: no package can come
from outside the 61 MB kernel image, 21 registries are link-time only, and a
module carries one `struct driver` against 41 exports.  The OPEN DECISION the
plan states and recommends: apps as ring-3 programs (§M65's
`dosgui_ui_build`), gui-core / shells / Wayland as module packages.  Stage 0
is a coupling map with numbers PLUS a review of EVERY ring-0 component (must
stay / could move / unknown cost, with the reason) — asked for explicitly,
because most of the kernel is there by history rather than by requirement.
The repository split is stage 7, gated on a green boundary audit.

## §M82 — the preference boundary FIXED, and the teardown use-after-free under it

"Measure before building" on the per-user wallpaper found three defects in the
boundary and one memory-corruption bug in the GUI teardown.  All fixed; see
DOCS.md (§M32 "Settings that belong to a person", the §M82 correction) and the
PROGRESS note in PLAN §M82.

- `config.c` now keeps the MACHINE value aside when a user overrides a key;
  withdrawal restores it (or unsets), the user store holds only overrides, the
  machine store only machine values.
- the GUI withdraws the previous user on every session replacement and on
  `gui stop` (it never did — only the text `logout` did).
- `gui_teardown` waited for the desktop only; the compositor (its child,
  re-parented to init once the desktop died) kept compositing into the freed
  back buffer.  That was the x86_64 GPF at `rip=0xff0000ffff0000ff` and the NMIs
  inside the font tables, and very probably the i386 NMI seen after `gui stop`.
  `task_kill_tree_pids()` + wait for every member.

Falsifiers (hidden from `help`, need two accounts and a scratch disk):
`sessiontest <a> <b>` and `sessionstorm <a> <b> <n> [mode]` — mode 2 (theme
flip from a foreign task) is the reproducer for the teardown bug.

**The "harness loses keystrokes" was an IRQ storm that starved the clock** —
virtio-blk left its shared level line (IRQ 10) asserted; fixed, plus chained
IRQ handlers and a mouse drain that ate keyboard bytes.  See DOCS.md (§M32
section, the 2026-09-25 paragraph).  Typing across `gui stop` is reliable again.

**§M82 proper (greeter, per-user desktop dir, PATH, program list, sign-out
saving) is NOT started.**  Next in that order.

## Known defects — triaged 2026-09-25

Only DEFECTS (wrong behaviour), not missing features.  Each row says how it was
established; "open" rows were checked against today's tree, not copied forward.

### Fixed in this round

| defect | how it showed | fix |
|---|---|---|
| virtio-blk left its shared level IRQ (10) asserted → interrupt storm on CPU 0 | "harness loses keystrokes" after `gui stop`; `task_msleep`/cron stopped; no NMI | `c75756f` — NO_INTERRUPT + ISR ack; chained `irq_install` |
| `irq_install` REPLACED the previous handler on a shared line | (the mechanism of the above) | `c75756f` — up to 4 chained handlers |
| PS/2 mouse drain read and discarded keyboard bytes, also from its 1 s backstop | occasional lost keystroke | `c75756f` |
| `gui stop` command bypassed the §M82 preference withdrawal | previous user's prefs stayed on the console | `c75756f` — withdrawal inside `gui_teardown` |
| HDA replayed the sound every 682.7 ms / split it (the §M23/§M67 "intermittent HDA defect") | 3/8 runs on the old kernel, 0/8 after | cause was the storm (`c75756f`); ring now plays silence when stalled (`fe9f19f`) |
| x86 ms clock counted PIT interrupts and ran at **80 %** of real time | `uptime` 16.35 s per 20.4 s; LAPIC 80 Hz; TSC 1.26–17.8 GHz | `70f6bd6` — derived from the ACPI PM timer |
| AC97 capture "at 80 % of real time" (§M23 stage 7 open item) | `rec` warned FASTER | not a codec bug — the clock above; `rec 2000` now 2008–2020 ms |
| GUI teardown freed surfaces while the compositor still drew | x86_64 GPF at rip=`0xff0000ffff0000ff`, NMIs in font tables | `3fce70f` |
| config had one layer (user overwrote machine; leaked across sessions) | `sessiontest` FAIL×3 | `3fce70f` |
| harness could not issue a command once a GUI window held focus (§4.74) | every GUI-state test needed a pre-set config key (`gui.autorun`, `gui.stats_ms`…) | COM1 command channel (`serial_cmd.c`) + `--via serial` / `--monitor-cmd "serial …"`; `shell_current_vc()` made per-task |

Instruments added: `!! PIT STARVED` (per-CPU, falsified by `pitstarvetest`), NMI report with
`if=`/`task=`/`isr-vec=`/stack scan, monitor replies kept in `<log>.mon`,
`scripts/wav-analyze.py`, `sessiontest`, `sessionstorm`.

### Open — measured or read from today's tree

| # | defect | severity | notes |
|---|---|---|---|
| 1 | ~~Sign-out / shutdown kill the session tree at once~~ | — | **FIXED 2026-09-25** (DOCS §4.92): close requests with a grace, forced by name after it, then desktop, then compositor; power-off goes through it; Editor asks and keeps a `.unsaved` copy.  Only the Editor has a close guard so far |
| 2 | ~~virtio-blk busy-waits for every request~~ | — | **FIXED 2026-09-25** (DOCS §4.93) — and it uncovered worse: the whole storage stack was unlocked (`diskstorm`: corrupted another file on the volume), and `task_current()` could return another task.  All fixed.  aarch64's virtio-mmio-blk is locked and bounded but still POLLED (no interrupt wired) |
| 2b | Intermittent x86_64 NMI after many session switches (both CPUs inside `vfont_label_pts`) | watch | **probably fixed 2026-09-25**: 2 crashes in ~180 storm rounds before the config store and glyph cache were locked, 0 in 120 x86_64 + 80 i386 rounds after (1-2 were expected — suggestive, not proof).  Most likely cause: the unlocked config store (a concurrent double free corrupts the heap).  `!! KSTACK` now checks every context switch, so a recurrence names the task |
| 4 | `ktimer` lateness ~1.2-1.8 ms | low — **the emulator's floor** | a one-shot LAPIC deadline was built and measured (2026-09-25, DOCS §4.53.1): the LAPIC's own interrupt arrives 1.6-1.7 ms late here, so it bought nothing and tripped a false `PIT STARVED` at -smp 4 — removed.  Worth rebuilding only on real hardware |
| 5 | Real storm case: no in-guest report escaped (all serial output stopped) | low | the cause is fixed; the `PIT STARVED` path is proven only on its falsifier |
| 6 | §4.67.1 "host-load dependent" NMI lockup, and today's i386 NMI after `gui stop` | watch | very probably the storm (a disk is attached on every everyday run since §4.66); not proven — the NMI report now names task, IF, in-service vector and stack |
| 7 | HDA sound splits when its pump task is starved | low | only under starvation; the gap is silence, not stale audio |
| 8 | exFAT `dir_is_empty` scans at most 4096 entries | low | a limitation, stated in the code |
| 9 | aarch64 does not publish `/dev/vda` (its block driver initialises after `devfs_init`) | low | not chased |
| 10 | `load_balance_pull` counts a migration attempt whose insert can be refused | cosmetic | diagnostic counter only |

### Checked and no longer open

- §M50-era `sh -c` SMP failure — fixed by §M51/§M52.
- §M54 reap-sweep "STILL QUEUED" and the i386 `killstorm` hang — `killstorm` 20/20 rounds,
  160/160 killed at `-smp 4`, shell answers after (2026-09-25).
- exFAT `rename` — implemented (`exfat_rename`).
- GRID / LIST item views without a scrollbar — both have one now.
- §M81 Task Manager per-frame regression — not reproducible (`bench-taskman-sweep.sh`).
- `gui stats` printing to the suppressed console — replaced by `gui_diag_service` (§M70).

A short, durable note so work can resume without re-deriving the session.
Everything below is measured unless it says otherwise.

## Branches

- `main` — everything, including `m32-gui-login` (the accounts panel, the
  default account, the Start-menu header + Lock/Sign out, the lock screen's
  user picker, the GUI sign-in and autologin), which is merged.

## The password DEFECT — measured, and it does not reproduce (2026-09-20)

Driven end to end on **i386 and x86_64**: Control Panel -> select `david` ->
Set password -> one character -> Enter -> Start -> Sign out -> pick `david` ->
same character:

    accounts: done_password received 1 character(s) for 'david'
    users: password set for 'david' (pbkdf2-sha256, 10000 iterations)
    lock: authenticated 'david'

**Not claimed fixed** — I do not know which change closed it.  If it still
fails on a running machine, the first thing to check is a **stale image**:
`run-x86_64.sh` boots without building, which is §M69's own trap and is why the
STALE IMAGE banner exists.

**Two real defects were found by the report, both from its second sentence**
(*"it writes three asterisks straight away"*):

- the password mask drew a FIXED `***`, so one character and five looked
  identical and the field gave no sign a key had landed.  §M32's reasoning —
  "a row of bullets publishes the length" — is reversed: anyone close enough to
  count bullets can count keystrokes.  **One mark per character** now.
- `" — uid "` was a literal UTF-8 em-dash in a DRAWN string, and the font is
  byte-indexed ISO-8859-2 — which is the *"david á uid 1000"* in the reporter's
  screenshot.  **95 such literals in 22 GUI files**; `devicepanel.c` had
  thirteen and a comment one screen away explaining the trap.
  `scripts/check-drawn-strings.py` now checks it, and its own first version
  cried wolf on wrapped `kprintf` calls before it learned to track call spans.

## The previous open DEFECT (superseded by the above)

**Changing a password from the Control Panel does not take effect.**  Reported
from use and reproduced by the reporter with a clean isolation:

- `passwd david y` from the SHELL, then signing in through the lock screen with
  `y` — **works**.
- the same password set through the Control Panel, then signing in — **fails**.

Everything around it has been cleared by measurement:

- the picker passes the right account (the failure message names `'david'`)
- the lock screen's keyboard input works (the reporter sees `***` while typing,
  and the shell-set password signs in)
- the panel's chain below `done_password` round-trips: `accttest david:x`
  reports field 1 char → `user_set_password` → `user_check_password` **OK**

So what remains is what reaches `done_password` when a HUMAN types, as opposed
to `w_textinput_set` filling the field — the one step no instrument performs.
The panel now prints the received COUNT (never the content) on its own detail
line and on the serial log, so the next attempt separates three outcomes that
previously all looked like "wrong password":

| what the panel says | meaning |
|---|---|
| `(1 characters)` and sign-in still fails | the SET is correct; the fault is on the sign-in side |
| `(2 characters)` or more | a stray character joins the typed one |
| `No password typed` | the keystroke never reaches the field |

## Instruments added (all needed because the GUI suppresses the console, §4.79)

- `gui.locktest <user>:<password>` — drives the lock screen's real submit.
- `accttest <user>:<password>` — drives the accounts panel's real prompt submit.
- Both must run on their OWN task; a shell-task kprintf reaches nobody with the
  GUI up.  `accttest` needs `gui.autorun` (§4.74: the harness cannot type once a
  GUI window holds focus) AND a `gui stop` first, because `gui.autorun` is read
  by `gui_start`, which returns immediately when the desktop is already up.

## Fixes in this branch, each from a report

- an empty password field no longer DELETES an account's sign-in
- a failed unlock no longer sleeps on the window's app-host task (the "lag")
- `-1,-1` is not "centre": the lock screen and the panel's prompt were off-screen
- the password mask drew three carons (0xB7 is a middle dot in Latin-1, a caron
  in this font's ISO-8859-2) — plain `***` now, still fixed width
- a disabled widget now receives no input at all (it was honoured in ONE of four
  dispatch paths)
- `PANEL_POPUP_MAX` 480 → 768, derived from the tallest menu at the 200 %
  density cap; a popup that overflows the strip now REPORTS it instead of being
  silently clipped (two Start-menu rows had vanished)
- the accounts panel checks `user_set_password`'s return value instead of
  reporting the outcome it hoped for

## §M81 (PLAN.md) — step 1 SHIPPED, step 2 next

The agreed design is written up there in full — **one hierarchy → one
constructor → window-as-widget**, with the threading rule (the widget tree lives
on ONE task; data crosses boundaries, not references), the four risks, and the
first measurement table that is also the proof of work when re-measured.

**Step 1 is done and on `main` (`86c53d1`): the container is a widget.**
`w_box.c`; ui.c is 322 lines lighter; `ui_draw_overlay()` and `ui_pointer_at()`
are deleted rather than ported, because `widget_draw_all` and `win->grabw` now
do both jobs.  `widget_ops.pointer` returns `WH_IGNORED`/`WH_DAMAGED`/
`WH_REPAINT` instead of void.  Proven by a driven mouse on i386 and x86_64, and
by a control run against the pre-change code in which the same gesture leaves
the panel's 378131 pixels **byte-identical**.

**Step 2 is done and on `main` (`0cc1abe`): `gui_app_open(&spec)`.**  Placement
is an INTENT, the singleton is a `slot` the compositor clears on every close
route, and a window built on a task with no app-host loop is named instead of
failing silently.  **The entangled band went from 8 files to 4**, and the
lifecycle cluster survives only in three declared exceptions.  Two falsifiers
ship with it — `gui hosttest` and `gui slottest`, both hidden from `help`.

**Step 3 is started and on `main` (`bab9013`): the Start menu is an
`item_model` drawn by the shared list view.**  Its row geometry went from THREE
copies (painter / click / hover) to one, and the conversion needed three
optional additions — `item_entry.group_start`, `item_view.height_for`,
`item_model.row_h`.  It also exposed two pre-existing defects in the shared
view: a density-blind `L_ROW_H 40`, and a row too short for the two lines the
view itself draws (the Control Panel's list had every sub-label painted across
the next row's label).

**Still hand-drawn, and the rest of step 3:** the taskbar, the volume and
keyboard flyouts, the desktop icon field's chrome, and the sign-in screen.
`shell_vista.c` stays at **17** on the coupling table and that is correct — its
calls are `gui_wm_*` / `gui_panel_*` / `gui_queue_*`, the chrome and WM APIs,
which the menu conversion never touched.  *Use the right metric per step:* the
table measures app↔compositor coupling, and a step that removes duplication
INSIDE a file has to be measured as duplication.

**The widget contract is done and on `main` (`0ed84c1`).**  widget.h states
which ops are mandatory (`draw`; `key` or `keycode` if focusable; construction
through `widget_init`) and `audit widget-contract` checks it — the registry half
statically, so it reaches a class nobody instantiated, and the live half by
walking open windows, so it reaches a widget from a hand-rolled constructor.
`gui contracttest` is the falsifier (needs `--allow-crash`).

It found that **two constructors had been bypassing `widget_init` for three
milestones** — `w_itemview_create` and `w_editor_create`, the very defect §M65
exported that function to prevent.  Both go through it now.

**The threading rule is checked too (`9eeb449`).**  `audit widget-threading` —
§M22.7's *a window's widgets belong to the task that hosts it*, observed in
`widget_init`, announced where the cause is and counted for cron.
`gui threadtest` is §M69's dialog bug in two lines and it is DETECTED; **the
existing tree is clean**.  The registry is 8 audits now, and on aarch64's serial
boot `widget-contract` correctly SKIPs while `widget-threading` reports ok.

**The keyboard flyout followed (`32769a4`).**  Same shape, two copies instead
of the menu's three, and the chrome's row-geometry copies are now **0 in code**.
The active layout is the view's `sel`, and it carries the keyboard ICON while
the others carry none — the glyph difference the old `*` existed for, since this
project's tests read pixels.  Driven end to end: a click on `hu` gives
`keyboard.layout = hu (was us)` and the tray reads **HU** in the screenshot.

It also exposed **`ICON_NONE` drawing the generic window tile** — an enum member
named NONE that painted something, because it fell through to the `default` arm.
So "this row has no icon" and "this icon id is a typo" rendered identically,
which is the one distinction that default exists to make.  Fixed; nothing had
asked for a blank icon before.

## THE CORRECTION THAT REFRAMED THE REST (2026-09-19)

Reported mid-work: *not everything into a list — only what belongs in a list;
that was just an example.  **The point is widgets that nest into each other
sensibly.*** So: measure before building.

| | count |
|---|---|
| windows that COMPOSE (`ui_build` with a spec tree) | **4** — settings, dialog, uikit, the file manager's menu bar |
| windows that HAND-PLACE | **12** |

*The mechanism exists and three quarters of the tree does not use it* — §M81's
finding restated one layer up: unconverted clients, not a missing abstraction.

**One piece WAS genuinely missing, and it is the one named in the brief: the
VIEW widget.**  `w_itemview` had ops and no `WIDGET_CLASS`, so no spec could name
it.  It is the `"view"` class now; the model is attached afterwards through
`ui_by_id`, because a spec is DATA (a ring-3 client sends the same array) and a
model is a pointer.

**The accounts panel is composed** — the first window with a real table to be —
and a weighted empty box replaces writing `base.x = cw - pad - w` for the
right-aligned Delete.

**AND THE CONVERSION HIT A TRAP THE TOOLKIT HAD DOCUMENTED RATHER THAN REMOVED.**
`ui_build` APPENDED, with ui.h carrying a convention ("build once, layout many")
that the two existing composers honour and the third did not.  The symptom is
not a duplicate control — the node table fills at 64 and the window goes EMPTY,
with stale nodes pointing at freed widgets.  `ui_build` REBUILDS now (widgets
and nodes together, the only safe order) and asks for a REDRAW rather than a
re-LAYOUT — the latter made a window whose `on_layout` IS the builder loop
forever.  Driven resize: 986x493 → 1232x609 with the node count still 8.

## Still open in §M81

- `struct gui_window` as a widget — **and worth questioning before building**:
  the container is a widget, the view is a class, and composition now works
  end to end.  What a window-as-widget buys beyond that has no named client yet
  (§M59's rule).
- the taskbar's own content (Start button, window buttons, clock, tray) is still
  imperative — though its two geometry duplications are gone.
- the remaining **10** hand-placed windows.  The accounts panel and the Devices
  panel are converted, and the second one proved the first was a PATTERN rather
  than a one-off: it was mechanical, with nothing new needed from the toolkit.
  The accounts file still hand-places its PROMPT window.

### ✅ THE TASK MANAGER IS COMPOSED (`5f26ae5`) — AND IT COST SOMETHING

Both capabilities below are built, with the Task Manager as the client in the
same change.  The conversion found three real defects: `row ? avail_w :
avail_w` (a ROW measured every child against the FULL width — the **third**
instance of §M69's `even ? w : w` tell, and fatal for the charts, which answer
`*pref_w = avail_w`); a hidden container that did not take its children with it;
and `ui_build` full-repainting a window its host repaints anyway (**0 → 3 → 1**
full repaints).

**THE OPEN NUMBER — RE-MEASURED 2026-09-23 AND NOT REPRODUCED.**  §M81 recorded
this, twelve samples a side, and did not record the gesture:

| | median us/frame | range | area |
|---|---|---|---|
| hand-placed (as recorded) | 1386 | 828..3673 | 7..33 kpx |
| composed (as recorded) | 2428 | 1781..3730 | 14..34 kpx |

Re-measured with one fixed, written-down sweep (`scripts/bench-taskman-sweep.sh`,
i386 -smp 4, only the 2 s windows with >= 24 frames), three ways — the two
revisions of `5f26ae5` itself, and HEAD with the conversion reverted, which
isolates the Task Manager as the one variable on today's engine:

| | windows | median us/frame | range | area |
|---|---|---|---|---|
| `e4e187e` hand-placed | 5 | 2693 | 2224..3050 | 18..25 kpx |
| `5f26ae5` composed | 5 | 2993 | 2644..3425 | 21..24 kpx |
| HEAD, conversion reverted | 16 | **2965** | 1568..5402 | 18..46 kpx |
| HEAD, composed | 15 | **2964** | 2565..4532 | 20..40 kpx |

**No difference beyond §M69's ±19 % noise floor, and the AREA did not move.**
A temporary per-call damage log (`gui_window_request_redraw_rect` +
`app_redraw`, caller by `__builtin_return_address`, mapped with `dos-sym.sh`)
gives the same profile on both sides: per tick, four `w_chart_refresh` rects
(156x96 vs 158x96), two or three `iv_damage_cell` cells (the CPU% and TIME
columns, 44 px rows), the footer from `tm_refresh`; one `app_redraw` and 2-3
`iv_damage_all` in the whole run, i.e. at open and layout, not per frame.  The
table itself damages NOTHING under the moving pointer (`iv_hover` returns 1),
and an empty `box` is zero-sized, so it cannot be hit and cannot be hovered.
The footer was damaged more often on the composed run (54 vs 30), and that is
DATA, not structure: it compares its text before it writes, and the CPU total in
it changes on most refreshes.

**So the recorded regression is most probably a different GESTURE, not a
different build**, and it cannot be settled because the original was never
written down.  *A benchmark whose recipe is not a file can be re-guessed, not
re-run* — hence the script.  **Converting the remaining hand-placed windows is
not blocked by this any more**; re-run the script before and after each one.

### The two capabilities, now built

Found by attempting the conversion, and **written down rather than built**,
because adding two layout features and converting the most timing-sensitive app
in one pass is where half-verified things come from:

1. **A child that is DROPPED when the window is too short.**  `UI_HIDE_COMPACT`
   drops on the SIZE CLASS, which is a fact about WIDTH; this is about the
   height left over, and a window can be wide and short.  The client is the
   chart strip, whose own comment is the argument: *"a chart below some height
   is a box with a header and no room for a line — which reads as a broken
   control, while its absence reads as a small window."*  (A `UI_DROP_TIGHT`
   flag was written and then REVERTED when gap 2 appeared: §M59's rule, applied
   to myself — a mechanism with no client to falsify it against is how a feature
   "works" until the first real user.)
2. **A child that runs EDGE TO EDGE.**  The root column always insets by
   `UI_PAD`, and the Task Manager's table deliberately does not: *"the design's
   table is a pane, not a box floating in a margin."*

Build both, then convert the Task Manager, then **measure the damage budget** —
§M81's own risk list says a composition tree is exactly what can undo §M69's and
§M79's work.
- **The volume flyout deliberately stays hand-drawn** — a slider and a mute row,
  not a list.

**A pattern worth keeping:** every instrument this milestone shipped caught the
FIRST VERSION of the next one.  `slottest` polled on the host task it had
blocked; `contracttest` built its window on the shell, so the layout hook never
ran and it reported "the check is broken" about a check that was fine — and step
2's hosting warning named the cause in the same log.  *The half that builds runs
on the host; the half that observes must not.*

The metric itself is reproducible — count the DISTINCT symbols defined in
`gui.c`/`wm.c`/`compose.c`/`input.c` that a file calls, and check that
`gterm.c` comes out at exactly 2 (§M70's own claim).  *A metric that cannot
reproduce a known answer is measuring something else.*
