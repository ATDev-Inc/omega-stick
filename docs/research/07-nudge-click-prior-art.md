# Nudge clicks: prior art for click gestures without cursor movement

Point-in-time snapshot, captured 2026-11-22. Not maintained.

Question: in mouse mode the cursor moves while the user performs a nudge click.
How do other products tell a quick click gesture apart from a deliberate
deflection on the same stick, and how do they keep the pointer still while
deciding?

Method: three parallel web research passes (power wheelchair mouse modes,
assistive pointing devices and accessibility software, gaming and general
pointing-device engineering). Many primary sources were blocked or too large to
fetch, so most of what follows is a lead, not a fact. Only the three items under
"Confirmed" were read in the primary source. Everything marked `UNVERIFIED` came
from search snippets, summaries or abstracts.

## Why the cursor moves today

Read from `mouse_step()` in
[omega_control.hpp](../../components/omega_control/include/omega_control.hpp)
at `95db1d2`, with the default settings in
[omega_calibration.hpp](../../components/omega_calibration/include/omega_calibration.hpp)
(enter delta 0.5, exit 0.8, window 100 ms). This is a code read, not a hardware
measurement.

1. **Before recognition.** A nudge is only decided once per `nudge_hold_time`
   window. Until the window ends, every 10 ms tick is sent as ordinary movement
   (deflection x `SENSITIVITY`). A push that straddles two windows can take two
   windows to register, or not register at all.
2. **While held.** `nudge_exit_threshold` is an absolute magnitude, 0.8 by
   default. A push that peaks below it is released at the next window boundary
   although the stick is still deflected, and the stick goes back to moving the
   cursor.
3. **After release.** Nothing suppresses movement while the stick travels back
   to centre, so the return stroke moves the cursor until it is inside the
   deadzone.

## Confirmed from primary sources

### R-net power wheelchair mouse mode

Source: R-net CJSM2 Technical Manual SK81302-04, chapter 7 (Bluetooth
Programming),
<https://amylior.com/wp-content/uploads/bsk-pdf-manager/SK81302-04-R-net_CJSM2-manual-1.pdf>.

- 7.1.1: "In Bluetooth Mode a Nudge is a quick activation of the Input Device
  in the Forward, Reverse, Left or Right directions." Each direction maps to
  one of: none, left click, right click, scroll up, scroll down, double click
  left or right, or an Android key.
- 7.1.2: "Nudge Time sets the time limit in which an Input Device must be
  deflected and released to activate a mouse command." Range 0 to 5 s in 20 ms
  steps; 0 disables nudges. No default was found in the extracted text.
- 7.1.14 to 7.1.16 (iDevice and Switch Control modes): Short, Medium and Long
  Nudge Time default to 0.5 s, 2.0 s and 3.0 s.
- 7.1.3: Pointer Speed 0% disables pointer movement. The configured action then
  fires immediately on deflection, a button stays pressed while the stick is
  held, and a scroll repeats every 200 ms.
- 7.1.5, 7.1.6: optional beeps, continuous while the pointer moves and a single
  beep for a button function such as a nudge click.

Inferences from that text, not statements in it:

- "Deflected and released" implies the click fires on release, not on
  deflection.
- The manual documents no deflection threshold and no suppression of pointer
  movement during a nudge. Whether the pointer moves during a nudge is
  UNVERIFIED. The only documented way to get no pointer movement is Pointer
  Speed 0%, where the stick is no longer a pointer.

A Permobil M1 manual describes a nudge as "a quick, full deflection"
(<https://www.manualslib.com/manual/1551301/Permobil-M1.html?page=168>).

### Windows pen flicks

Source: <https://learn.microsoft.com/en-us/windows/win32/tablet/flicks-gestures>.

Detection starts at pen-down, and while it runs "pen packets are held back and
not sent to the system". Length, velocity, time and curvature are updated per
packet. As soon as a metric leaves the flick bounds, detection ends and the held
packets are released; on a flick they are dropped. The stated basis is that
drags are typically slow, so speed and straightness separate the two.

### libinput tap-to-click

Source: libinput 1.29.1,
[evdev-mt-touchpad-tap.c](https://chromium.googlesource.com/external/gitlab.freedesktop.org/libinput/libinput/+/refs/tags/upstream/1.29.1/src/evdev-mt-touchpad-tap.c)
and
[evdev-mt-touchpad.c](https://chromium.googlesource.com/external/gitlab.freedesktop.org/libinput/libinput/+/refs/tags/upstream/1.29.1/src/evdev-mt-touchpad.c).

- The tap timeout is 180 ms (`DEFAULT_TAP_TIMEOUT_PERIOD`) and the movement
  threshold 1.3 mm (`DEFAULT_TAP_MOVE_THRESHOLD`).
- With tapping enabled, small pointer motion is filtered while a touch might
  still be a tap. `tp_tap_handle_state()` returns `filter_motion` set for
  `TAP_STATE_TOUCH` (and for the tapped and multi-finger touch states), and
  `tp_post_events()` then returns before its normal motion processing.
- Normal processing resumes once the touch stops qualifying as a tap: moving
  past the threshold takes the state to `TAP_STATE_DEAD`, and the timeout takes
  it to `TAP_STATE_HOLD`. Neither state filters motion.

So libinput is another example of suppressing movement while deciding whether
an input is a click. Whether the filtered motion is sent afterwards, and when
the click itself is sent, were not checked.

## Field data point

One daily user of a power wheelchair mouse mode reports a nudge time setting of
about 300 ms on their chair's joystick. This is recalled from memory, not read
off the programmer, and the controller model is not recorded. It sits below the
0.5 s short-nudge default above and is a starting point for the time limit in
point 4 of "What this suggests".

## Leads (UNVERIFIED)

- **US11301064** (<https://patents.google.com/patent/US11301064B2/en>): blocks
  spatial data for a set interval after a switch changes state and buffers the
  motion in the device. If the device keeps moving after the interval the
  buffer is sent, otherwise it is discarded. Abstract and summary only; claims
  not read.
- **US7761814B2**, pen flick gesture
  (<https://patents.google.com/patent/US7761814B2/en>): holds input back until
  the flick decision is made, with direction, distance and pen-down time
  criteria.
- **US6084572**: isometric stick click needs force held for at least 10 ms in
  one zone, and clicks are blocked while a pointing-mode flag is set.
- **FLipMouse** (AsTeRICS,
  <https://github.com/asterics/FLipMouse/blob/master/Documentation/UserManual/Markdown/FLipMouseUserManual.md>):
  separates the two by mode. "Mouse movement" moves the cursor; "Alternative
  Actions" maps stick directions past the deadzone to actions. A strong sip or
  puff followed by a stick deflection within about 1 s gives a different
  action. Firmware source not read.
- **QuadStick Singleton**
  (<https://quadstick.s3.amazonaws.com/documents/user_manual/um/the_singleton_default_configuration.htm>):
  modal. A soft sip enters a recognition mode, the next stick move fires a key
  and returns to mouse mode. Clicks are sip and puff.
- **LipSync**: clicks are sip and puff only; the stick only moves the cursor.
- **Steady Clicks** (Trewin et al., <https://doi.org/10.1145/1168987.1168993>):
  freezes the cursor during a button press and suppresses clicks made while the
  pointer is moving fast. Freezing made dragging harder for some users.
- **mouse-damper** (<https://github.com/mtwebster/mouse-damper>): freezes the
  pointer on button press until a double-click completes or times out, or
  until movement exceeds a "breakout" distance.
- **Smash-style games**: a flick is separated from a tilt by time to threshold.
  Community figures give windows of 2 to 6 frames at 60 Hz, about 33 to 100 ms
  (<https://smashboards.com/threads/all-about-stick-sensitivity.466696/>).
- **TrackPoint press-to-select**: Linux exposes `press_to_select` and
  `upthresh`, an offset from a running average
  (<https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-platform-trackpoint>).
  A jump above a running average is close to our rise-in-window test. Nothing
  found on pointer suppression during the press.
- **Steam Input**: long press defaults to 0.15 s and cancels interruptable
  activators
  (<https://partner.steamgames.com/doc/features/steam_controller/activators>).
  No flick-versus-tilt parameters for sticks were found.

## Not found

- Quantum Q-Logic 2/3 and Invacare/Dynamic LiNX Mouse Mover: no nudge
  documentation could be fetched.
- Sunrise, Switch-It, Dynamic DX, mo-vis, Stealth, ASL, Tecla: nothing on nudge
  logic.
- Jouse, IntegraMouse, TetraMouse, GlassOuse: not researched.
- Rolling the cursor back after a gesture is recognised (sending the opposite
  movement): no example in any source. Host pointer acceleration would make it
  inexact.
- Isometric and rate-control HCI literature on gesture segmentation: nothing
  found in this pass.
- User complaints about the cursor moving during an R-net nudge: none found
  either way.

## What this suggests

None of this is decided. It is input to a fix, which changes input behaviour
and needs a hardware test.

1. **Evaluate every tick.** Decide on each 10 ms sample instead of at window
   boundaries, using a time-to-threshold or rate criterion.
2. **Buffer, then decide** (the pen-flick model). Hold cursor output from the
   start of a fast rise. Drop it if a nudge is recognised; send it if the push
   turns out slow or sustained. The cost is a short, bounded delay on the start
   of fast cursor moves only.
3. **Suppress the return stroke.** After a nudge, keep the output neutral until
   the stick is back at centre.
4. **Keep a hard time limit and a way out.** A long deliberate push must never
   be swallowed (compare R-net Nudge Time and mouse-damper's breakout
   distance).

## Open questions

- Click on deflection and hold while deflected (today) or click on release
  inside a time limit (R-net)? Points 2 and 3 work with either.
- Is the absolute exit threshold still wanted once the return stroke is
  suppressed, and should it default to 0.8?
- How much start-up delay on a fast cursor move is acceptable? No source gives
  a number for a rate-controlled stick.
- Feedback on recognition: R-net offers a beep. Is there an equivalent worth
  having here (see STA-1 in [requirements.md](../requirements.md))?
