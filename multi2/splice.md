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

Re-base all PTS/DTS of the new stream so they're continuous with the outgoing stream.

MPEG-TS clients often freak out. For TS output, you really want:

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

## 📌 TL;DR version you'd tell your future coding agent

To splice cleanly into an MPEG-TS stream:

1. **Switch ONLY on an IDR frame.**
2. **Ensure PTS/DTS continue smoothly** (re-timestamp incoming stream).
3. **Ensure PCR is continuous** (reclock PCR).
4. **Keep video/audio PID numbers the same** (or inject new PAT/PMT).
5. **Send SPS/PPS immediately before the first IDR** to reset decoder state.
6. **Make sure codec parameters match**
   (resolution, profile, level, etc).
7. **Preserve PCR/PTS alignment offset** (see section 5).

Do all that, and you get a butter-smooth switch with zero corruption.

---

## 🔧 5. PCR/PTS Alignment — The Hidden Killer

This one's sneaky. You can do everything else right and STILL get audio gaps.

### The problem:

In MPEG-TS, **PCR** (Program Clock Reference) and **PTS** (Presentation Time Stamp) are NOT necessarily aligned:

```
Source stream example:
- First PCR value: 19,476,000 (27MHz) = 0.72 seconds
- First PTS value: 126,000 (90kHz) = 1.40 seconds
- Offset: PTS is 0.68 seconds AHEAD of PCR
```

This offset is **intentional** — it gives the decoder time to:
1. Receive the data (at PCR time)
2. Decode it (takes time)
3. Present it (at PTS time)

### The trap:

When rebasing timestamps, if you naively set both PCR base and PTS base independently:

```cpp
// WRONG approach - destroys timing relationship
pcrBase = firstPCRValue;    // 19,476,000
ptsBase = firstPTSValue;    // 126,000
// After rebasing: both PCR and PTS start at 0
// The 0.68s buffer is GONE!
```

The decoder now receives data at time 0 but is told to present at time 0. No buffer time = audio underruns = gaps.

### The fix:

**Maintain the PCR/PTS offset when rebasing.**

```cpp
// CORRECT approach - preserves timing relationship
pcrBase = firstPCRValue;                    // 19,476,000
ptsBase = firstPTSValue;                    // 126,000

// Calculate alignment offset (how far PTS is ahead of PCR)
alignmentOffset = (ptsBase * 300) - pcrBase; // In 27MHz units
ptsAlignmentIn90kHz = alignmentOffset / 300; // Convert to 90kHz

// Initialize offsets
globalPCROffset = 0;                        // PCR starts at 0
globalPTSOffset = ptsAlignmentIn90kHz;      // PTS starts ahead

// Result:
// - Rebased PCR: 0
// - Rebased PTS: 0.68s
// - Decoder has proper buffer time ✓
```

### Why this matters for audio specifically:

Video can tolerate some timing jitter because:
- Frames are larger, less frequent (~30fps)
- B-frames provide natural buffering
- Visual glitches are less noticeable

Audio is brutal because:
- Frames are tiny and constant (~47fps for 48kHz AAC)
- No B-frames, strict sequential decode
- Gaps are VERY audible (pops, clicks, silence)

### TL;DR for PCR/PTS alignment:

When rebasing timestamps:
1. **Extract the original PCR/PTS offset** from the source
2. **Add this offset to PTS** (not PCR) when initializing
3. **PCR starts at 0**, PTS starts at the offset value
4. The decoder gets its buffer time, audio plays smoothly
