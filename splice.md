# HOW TO CLEAN SPLICE

You’re basically trying to do *surgery* on a live MPEG-TS stream without the patient waking up screaming.

---

## ✂️ So what makes a *clean splice* in MPEG-TS?

You need **three things** to line up:

1. **Aligned video access units**
2. **Continuous PCR/PTS/DTS timelines**
3. **Consistent stream metadata (PIDs, SPS/PPS, codec config)**

If you mess up any of these, VLC/OBS/whatever will start coping hard and drop frames or desync.

Let’s unpack the essentials.

---

## 🟩 1. You must splice at an **IDR frame** (keyframe)

This is the golden rule.

In H.264/H.265 inside MPEG-TS:

* A normal I-frame *might* reference older frames.
* An **IDR frame** is a clean “reset point,” meaning:

  * No previous frame dependencies
  * Decoder can start fresh
  * All reference lists reset

If you splice mid-GOP, the decoder literally goes:

> “Where tf is the frame I need to decode this?!”
> …and artifacts explode.

**So you always splice at the *start* of an IDR NAL unit.**

---

## ⏱ 2. PTS/DTS continuity matters — PCR continuity matters even more

### Think of it like this:

* **PCR = wall-clock of the transport stream**
* **PTS/DTS = timestamps for individual frames**

If PCR jumps → the client thinks time traveled backwards or forwards → playback stutters.

If PTS/DTS jump → the decoder thinks frames are out of order or missing.

### The rule:

When switching sources:

#### **Option A (The pro way):**

Re-base all PTS/DTS of the new stream so they're continuous with the outgoing stream.

#### **Option B (The lazy-but-valid way):**

Reset timestamps at the splice **but ONLY if your output container is FLV**
…because FLV is way chill about timestamp resets.

BUT MPEG-TS clients often freak out. For TS output, you really want:

```
last_outgoing_pts + frame_duration = first_incoming_pts
```

Same for DTS.

PCR must always increase monotonically.

---

## 🧩 3. PIDs must stay the same unless you push a new PMT

If your outgoing stream is:

```
Video PID = 256
Audio PID = 257
PCR PID = 256
```

Your incoming source **must match those PIDs**, *OR* you must:

* Emit a new PAT/PMT at the splice
* Make the PCR PID valid
* Ensure tables repeat a few times for safety

Easiest path: **normalize PIDs before splicing**.

---

## 🧱 4. SPS/PPS / codec config must match

If source A is:

* 720p
* Baseline profile
* 30fps
* certain SPS/PPS

…and source B is:

* 720p
* Main profile
* Different SPS/PPS

Switching without sending new SPS/PPS right before the IDR → corrupted decode.

So at the splice point:

* send SPS/PPS
* then IDR
* then continue normally

This is why ffmpeg’s `-force_key_frames "expr:gte(t,n*X)"` or `-g` control helps.

---

## 📌 TL;DR version you’d tell your future coding agent

To splice cleanly into an MPEG-TS stream:

1. **Switch ONLY on an IDR frame.**
2. **Ensure PTS/DTS continue smoothly** (re-timestamp incoming stream).
3. **Ensure PCR is continuous** (reclock PCR).
4. **Keep video/audio PID numbers the same** (or inject new PAT/PMT).
5. **Send SPS/PPS immediately before the first IDR** to reset decoder state.
6. **Make sure codec parameters match**
   (resolution, profile, level, etc).

Do all that, and you get a butter-smooth switch with zero corruption.

---

## If you want I can give you:

* sample code for a TS splicer
* ffmpeg command to enforce GOPs for switch points
* a C++ TSDuck splice skeleton
* a diagram of timestamps and continuity counters
* or a **coding agent prompt** to build the whole multiplexer

Just keep flowing — this stuff is actually kinda fun once you have the mental model locked in.
