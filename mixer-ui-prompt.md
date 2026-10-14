Design a **complete professional digital mixing console UI** for a modern live-performance audio system. This is a functional software mixer, not a conceptual dashboard.

Create a **single full-screen mixer interface in a 16:9 landscape layout**, approximately 1920×1080.

The interface should be dense but extremely organized, with the main mixer occupying most of the screen and a clearly separated utility/control area on the right.

Use a dark professional audio-console aesthetic: near-black/graphite background, slightly lighter channel surfaces, subtle borders, crisp typography, restrained blue/cyan active-state accents, and red only for clipping, recording, and mute/warning states.

The design should feel like a high-end commercial product used by musicians and sound engineers.

Do not add controls that are not specified below.

---

# GLOBAL LAYOUT

Divide the interface into three major areas:

### 1. Top system bar

A compact horizontal bar across the entire screen containing:

- **Feedback Elimination** toggle
- **Front LED** toggle
- **Mode: Indoor / Outdoor**
- **Mode: PA / AMP**
- Battery indicator and **78%**

Keep these controls compact and horizontally aligned.

The two Mode controls should use segmented buttons:

**INDOOR | OUTDOOR**

and

**PA | AMP**

with the currently selected option visually highlighted.

---

### 2. Main mixer area

The majority of the screen should contain **eight vertical channel strips**, arranged left to right:

1. INPUT 1
2. INPUT 2
3. INPUT 3
4. INPUT 4
5. AUX / BLUETOOTH
6. LOOP
7. PHONES
8. MAIN

The first four input channels should be substantially wider than the simple utility channels.

Use consistent channel-strip architecture and alignment so that all eight strips clearly belong to the same mixer.

---

### 3. Right utility/control panel

A dedicated vertical panel on the far right containing:

- LOOPER
- SNAPSHOTS
- SD-CARD RECORDING
- TUNER

Each should be its own clearly separated section.

---

# INPUT CHANNELS 1-4

Create four identical full-featured input channel strips.

Example channel names:

**01 MIC 1**  
**02 MIC 2**  
**03 GUITAR**  
**04 BASS**

The names are examples and should be clearly editable-looking channel labels.

Each channel contains the following, in this order from top to bottom:

---

## Channel header

At the top:

- Channel number
- Channel name
- Small single **CLIP** LED

The clip indicator is a physical-looking single red LED with the text **CLIP** beside it.

Do not use a large level meter.

---

## Auto Gain

Immediately below the header:

- **AUTOGAIN** toggle

This is a simple on/off control.

Do not include a tuner button on the individual channel strip.

---

## Gain

A large rotary knob:

**GAIN**

with a numeric value such as:

**+24.0 dB**

The knob should have a clear position indicator.

---

## EQ

A compact section labeled:

**EQ**

with an EQ enable toggle.

Below it, three rotary controls arranged horizontally:

**LOW**  
**MID**  
**HIGH**

Each has a numeric dB value.

Example:

**+2.5 dB**  
**-1.0 dB**  
**+3.0 dB**

Do not use redundant labels such as "amount".

---

## Compressor

A compact section labeled:

**COMP**

with a compressor enable toggle.

Use one rotary control representing compressor intensity from **0-100%**.

Label it simply:

**COMP**

and display:

**42%**

Do not add threshold, ratio, attack, release, or other compressor parameters.

---

## FX

A section labeled:

**FX**

containing three compact effect-send rows.

### FX1

- FX1 on/off toggle
- Effect selector displaying **Sm Room**
- Rotary send control
- Percentage value

Example:

**FX1   [ON]   Sm Room   28%**

### FX2

- FX2 on/off toggle
- Effect selector displaying **Chorus**
- Rotary send control
- Percentage value

Example:

**FX2   [ON]   Chorus   36%**

### External FX Loop

- **EXT FX LOOP** on/off toggle
- Rotary send control
- Percentage value

Example:

**EXT FX LOOP   [OFF]   18%**

Do not label these knobs "AMOUNT". Their function is already established by the FX section and effect row.

---

## Volume / Mute

At the bottom of each input channel:

- Large vertical **VOLUME** fader
- dB scale
- Current value such as **-6.0 dB**
- Large **MUTE** button

The fader should occupy significant vertical space.

The MUTE button should be easy to hit and visually distinct.

---

# AUX / BLUETOOTH CHANNEL

The fifth strip is a simpler and noticeably narrower channel.

Header:

**AUX / BLUETOOTH**

Include:

- Single red **CLIP** LED
- **MUTE** toggle

Then:

**EQ**

with an EQ toggle and three rotary controls:

**LOW**  
**MID**  
**HIGH**

Then a vertical:

**VOLUME**

fader with dB scale and numeric value.

This channel must NOT contain:

- Gain
- Auto Gain
- Compressor
- FX
- Tuner
- Any other processing controls

Keep this strip visually simpler than the four input channels.

---

# LOOP CHANNEL

The sixth strip is extremely simple.

Header:

**LOOP**

Below it:

- Large vertical **VOLUME** fader
- dB scale
- Current volume value

Nothing else.

Do not add EQ, mute, gain, compressor, FX, clip, or other controls.

---

# PHONES CHANNEL

The seventh strip is extremely simple.

Header:

**PHONES**

Below it:

- Large vertical **VOLUME** fader
- dB scale
- Current volume value

Nothing else.

---

# MAIN CHANNEL

The eighth strip is the main output channel.

Header:

**MAIN**

Include:

- Single red **CLIP** LED
- **MUTE** toggle

Below it:

- Large vertical **VOLUME** fader
- dB scale
- Current volume value

Do not add EQ, gain, compressor, FX, or tuner.

The MAIN strip should visually communicate that it is the final output.

---

# RIGHT SIDEBAR

Create a dedicated utility sidebar with four stacked sections.

---

# LOOPER

Section title:

**LOOPER**

Include four clearly separated transport controls:

**REC / OVERDUB**  
**PLAY**  
**STOP**  
**CLEAR**

Use intuitive audio transport icons alongside the labels.

REC / OVERDUB should be visually prominent and red when active.

Below the controls:

**STATUS**

with one clear status indicator and one status text.

The UI must support these exact states:

- **Recording**
- **Overdubbing**
- **Playing**
- **Stopped**
- **Cleared**

Show one current state at a time.

Example displayed state:

**Stopped**

Do not show multiple states simultaneously.

---

# SNAPSHOTS

Section title:

**SNAPSHOTS**

Create six recall-slot buttons:

**1**  
**2**  
**3**  
**4**  
**5**  
**Defaults**

Slots 1-5 are user snapshots.

**Defaults** is a special read-only preset.

Make the distinction visually clear: the five numbered slots are writable, while **Defaults** is read-only.

Below the slots:

**STORE**  
**RECALL**

Store should only make sense for slots 1-5.

Defaults should visually communicate that it cannot be overwritten, for example with a small lock icon.

Do not add unnecessary snapshot metadata.

---

# SD-CARD RECORDING

Section title:

**SD-CARD RECORDING**

Include one large button:

**RECORD / SAVE**

This button should visually change depending on state.

Below it:

**STATUS**

with one status indicator and one status label.

Supported states:

- **No SD card**
- **Ready to record**
- **Recording**
- **Saving**

Show one current state at a time.

The SD card status should be immediately understandable without opening another menu.

---

# TUNER

At the bottom of the sidebar:

A clearly visible button:

**TUNER**

This is the global tuner control.

When activated, it should open a **tuner overlay/popover** over the mixer rather than being another channel-strip control.

For the image, show the tuner **open**, demonstrating what the interaction produces.

The tuner overlay should contain:

### Channel selection

Four buttons:

**1   2   3   4**

These select which input channel feeds the tuner.

One channel should be visibly selected.

### Note display

A very large central note display, for example:

**A3**

The detected note should be the dominant element.

### Pitch accuracy

Below or surrounding the note, create a highly visual tuning indicator showing:

**-12 cents ← CENTER → +12 cents**

Use a curved or horizontal pitch meter with a central zero/target position.

The current pitch should be visibly slightly flat, for example:

**-4 cents**

The indicator should make it immediately obvious whether the note is:

- too low
- perfectly in tune
- too high

Use a smooth visual scale rather than merely displaying a number.

The tuner should look like a professional musical instrument tuner integrated into the mixer.

---

# INTERACTION AND STATE DESIGN

The UI should clearly distinguish:

### Active controls
Use a restrained blue/cyan illuminated accent.

### Inactive controls
Dark neutral controls with subtle borders.

### Clipping
Use a small bright red LED.

### Mute
Use red only when a channel is actually muted.

### Recording
Use red for active recording.

### Normal operation
Do not make everything glow.

The interface should remain calm and readable even with many controls visible.

---

# IMPORTANT DESIGN PRINCIPLES

This is a **mixer**, not a dashboard.

The vertical channel strips must remain the visual focus.

The four full input channels should have the highest information density.

The AUX/Bluetooth channel should be simpler.

LOOP, PHONES, and MAIN should be progressively simpler according to their specified functionality.

Do not artificially fill empty space in the simpler channels.

Do not invent controls to make the strips look symmetrical.

Use whitespace deliberately.

The right sidebar should feel like a utility/control area rather than additional mixer channels.

Use consistent vertical alignment for faders across all channels.

All fader caps should have the same physical design, with the main output fader potentially slightly more prominent.

---

# EXACT CONTROL INVENTORY

The final UI must contain exactly these functional elements.

### Four input channels

Each:

- Channel number
- Channel name
- Clip LED
- Auto Gain toggle
- Gain knob
- EQ toggle
- EQ Low knob
- EQ Mid knob
- EQ High knob
- Compressor toggle
- Compressor 0-100% knob
- FX1 toggle
- FX1 effect selector = Sm Room
- FX1 send knob
- FX2 toggle
- FX2 effect selector = Chorus
- FX2 send knob
- External FX Loop toggle
- External FX Loop send knob
- Volume fader
- Mute toggle

### AUX / Bluetooth

- Channel name
- Clip LED
- Mute toggle
- EQ toggle
- EQ Low knob
- EQ Mid knob
- EQ High knob
- Volume fader

### LOOP

- Channel name
- Volume fader

### PHONES

- Channel name
- Volume fader

### MAIN

- Channel name
- Clip LED
- Mute toggle
- Volume fader

### Global controls

- Feedback Elimination toggle
- Front LED toggle
- Indoor / Outdoor mode
- PA / AMP mode
- Battery percentage

### Looper

- Rec / Overdub
- Play
- Stop
- Clear
- Looper status

### Snapshots

- Slot 1
- Slot 2
- Slot 3
- Slot 4
- Slot 5
- Defaults
- Store
- Recall

### SD recording

- Record / Save
- SD recording status

### Tuner

- Tuner button
- Channel 1 selector
- Channel 2 selector
- Channel 3 selector
- Channel 4 selector
- Detected note
- Cent deviation
- Visual tuning indicator

---

# CRITICAL RESTRICTIONS

Do not add:

- Solo buttons
- Pan controls
- Input meters
- Channel meters
- Balance controls
- Additional EQ bands
- Compressor parameters
- Reverb parameters
- Delay parameters
- Master EQ
- Master compressor
- Additional buses
- Additional sends
- Additional channels
- Input routing controls
- Phantom power
- Phase inversion
- Polarity
- Gate controls
- Limiter controls
- Any other invented mixer functionality

Do not put the tuner on individual channel strips.

Do not put the TUNER button inside a channel strip.

Do not use the word **AMOUNT** as a generic control label.

Do not use redundant labels when the section and control context already communicate the function.

Do not fill empty space in simple channels with invented controls.

The result should look like a **real, coherent, commercially shippable digital mixer UI**, with every element having an obvious reason to exist.
