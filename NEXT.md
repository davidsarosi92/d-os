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

## Next milestone: §M81 (PLAN.md)

The agreed design is written up there in full — **one hierarchy → one
constructor → window-as-widget**, with the threading rule (the widget tree lives
on ONE task; data crosses boundaries, not references), the four risks, and the
first measurement table that is also the proof of work when re-measured.

First step: **make the container a widget**, so `ui_draw_overlay` disappears and
`UI_SCROLL` paints its own scrollbar.
