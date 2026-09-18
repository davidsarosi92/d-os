# Where things stand (2026-09-18)

A short, durable note so work can resume without re-deriving the session.
Everything below is measured unless it says otherwise.

## Branches

- `main` — §M32 (users, permissions) stages 1–10, merged.
- `m32-gui-login` — **current work, NOT merged.**  The accounts panel, the
  default account, the Start-menu header + Lock/Sign out, the lock screen's
  user picker, §M81's first measurement, and the fixes listed below.

## The one open DEFECT

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

**Step 3: THE WINDOW ITSELF BECOMES A WIDGET.**  `shell_vista.c` is the file the
table still puts at **17**, and it is the right one: it calls the CHROME APIs
(`gui_panel_*`, `gui_wm_*`, `gui_queue_*`) because the taskbar, the Start menu
and the desktop icon field are hand-drawn surfaces rather than compositions.
Only after the window is a widget do those become ordinary ones — which is also
what the user asked for (*"a window widget contains a menubar widget, a menubar
contains a dropdown; a view widget holds the icons and the desktop can use the
same one; list items the Start menu can use too"*).

Still to do in the milestone, from the agreed design: mandatory-vs-optional
`widget_ops` declared per class plus an `AUDIT()` with a shipped falsifier, and
the checkable threading rule (*a widget's parent and its host are the same
task*).

The metric itself is reproducible — count the DISTINCT symbols defined in
`gui.c`/`wm.c`/`compose.c`/`input.c` that a file calls, and check that
`gterm.c` comes out at exactly 2 (§M70's own claim).  *A metric that cannot
reproduce a known answer is measuring something else.*
