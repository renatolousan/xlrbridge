# xlrbridge

> *fixing Discord's problem with handling xlr mics with fancy interfaces*

A small native macOS CLI that routes one input channel of a pro/XLR audio interface into a clean virtual device, so Discord (and similar apps) stop **chopping** the audio and **auto-limiting** the input level.

## Is this you?

- You use an XLR mic through an audio interface (Topping, Focusrite, MOTU, PreSonus…).
- In Discord your voice **stutters/chops**, or the input level gets **pulled down** when you start talking.
- But the same mic records perfectly in QuickTime/your DAW, and Google Meet sounds fine.

That's a macOS quirk: apps that open a multichannel USB interface directly get a voice-processing path that mangles the audio. The fix is to route a single channel through a clean virtual device. `xlrbridge` automates that.

## Status

🚧 **Design complete, not yet implemented.** See [`HANDOFF.md`](./HANDOFF.md) for the full diagnosis, design, and phased implementation plan. A working proof-of-concept (Pure Data + a CoreAudio Aggregate Device) already validates the approach; `xlrbridge` is the native, dependency-light productization of it.

## Platform

macOS only (v1). Linux's audio stack makes this near-trivial via PipeWire and is out of scope here.

## License

MIT.
