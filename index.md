---
title: The Wave of Hormuz
layout: default
---

# The Wave of Hormuz

<div class="screenshots">
  <figure>
    <img src="waveofhormuz.png" alt="16 HP panel — dark-navy / teal / gold"/>
    <figcaption>16 HP panel — dark-navy / teal / gold</figcaption>
  </figure>
  <figure>
    <img src="woh_vcv.png" alt="Patched in VCV Rack 2"/>
    <figcaption>Patched in VCV Rack 2</figcaption>
  </figure>
</div>

A VCV Rack 2 oscillator that encodes the 2023–2026 Strait of Hormuz conflict as a dual-closure square wave. The waveform is always **+1 (OPEN / free transit)** except during the two programmed closure windows, where it drops to **−1 (CLOSED / blockade)**. Every knob name is a pun on the Strait.

Default parameters reproduce the real conflict timeline at **data-accurate proportions** — **947 days** from Oct 7 2023 to May 11 2026 — with all knob defaults set so the unmodified module plays the historical square wave exactly:

| Phase | Days | Event |
|---|---|---|
| 0.000 – 0.200 | 189 | OPEN |
| 0.200 – 0.223 | 22 | **CLOSED** — Iran seizes MV MSC Aries (Apr 13 – May 5 2024) |
| 0.223 – 0.926 | 666 | OPEN |
| 0.926 – 1.000 | 70 | **CLOSED** — Renewed blockade (Mar 2 2026 – present) |

The panel waveform graphic shows these proportions accurately: the first closure appears as a hairline notch (~1.5 mm at panel scale); the second as a solid block at the right edge.

---

## Prerequisites

| Tool | Notes |
|---|---|
| [VCV Rack 2](https://vcvrack.com) | Free or Pro |
| [VCV Rack 2 SDK](https://vcvrack.com/downloads) | Extract so that `<path>/plugin.mk` exists |
| [MSYS2](https://www.msys2.org) | Windows build environment |
| MinGW64 toolchain | Run in the MSYS2 shell: `pacman -S mingw-w64-x86_64-toolchain` |

> **Platform note:** The plugin currently builds on Windows only. The CRT heap bridge at the top of `src/WaveOfHormuz.cpp` is Windows-specific (see [Windows build notes](#windows-build-notes) below). macOS/Linux contributors can remove that block and build normally against the Rack SDK.

---

## Building

```bash
# Auto-detects the SDK if it is in a standard location.
# Override with RACK_DIR if needed.
RACK_DIR="$HOME/Documents/Rack-SDK" ./build.sh
```

| Command | Effect |
|---|---|
| `./build.sh` | Compile `plugin.dll` |
| `./build.sh install` | Compile and copy to the Rack 2 plugins folder |
| `./build.sh clean` | Remove all build artefacts |
| `./build.sh dist` | Build a distributable `.vcvplugin` zip |

`install` copies `plugin.dll`, `plugin.json`, and `res/` to:

```
%LOCALAPPDATA%\Rack2\plugins-win-x64\WaveOfHormuz\
```

Restart VCV Rack to pick up the updated plugin.

> **Before running `install`:** close VCV Rack completely. Rack holds `plugin.dll` open while running; the copy will fail with a "device busy" error otherwise. `build.sh` will detect this and print a clear message.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `install` fails: *device or resource busy* | VCV Rack is running and has `plugin.dll` locked | Close Rack, then re-run `./build.sh install` |
| `jq` warnings during build (`pipe: No error`) | `jq` is not installed | Safe to ignore — install with `choco install jq` to silence |
| Silent `Error 1` compile failure | `make` found Git's `sh.exe` as its shell | Always build via `./build.sh` rather than bare `make` |
| Module crashes Rack on load | Wrong CRT in the build toolchain (UCRT vs MSVCRT) | Use the MSYS2 **MINGW64** shell, not UCRT64 or Chocolatey MinGW |
| Closure windows overlap | C1 takes priority over C2 when windows share a phase region | Intended — dial the windows apart |

---

## Module reference

**16 HP.** One module: *The Wave of Hormuz*. All panel labels are lowercase.

### Knobs

| Label | Range | Default | Pun | Function |
|---|---|---|---|---|
| **knot speed** | −4…+4 oct | 0 (C4) | nautical knots | Base pitch; adds to v/oct and swell CVs |
| **opening ceremony** | 0…1 | 189/947 ≈ 0.200 | strait opening | Phase start of closure 1 (C1) |
| **strait jacket** | 0…1 | 22/947 ≈ 0.023 | straightjacket / strait | Width of closure 1 (C1) |
| **sanctions** | 0…1 | 877/947 ≈ 0.926 | international sanctions | Phase start of closure 2 (C2) |
| **embargo** | 0…1 | 70/947 ≈ 0.074 | embargo duration | Width of closure 2 (C2) |
| **oil slick** | 0…1 | 0 | oil-tanker spill | One-pole LP smooths hard square edges |
| **choke point** | 0…1 | 0 | strategic chokepoint | Tanh soft-clip; drive 1× → 10× (gain-compensated) |
| **persian tilt** | −1…+1 | 0 | Persian Gulf | Shape inside closures: 0 = flat, +1 = triangle peak, −1 = rising ramp |
| **tanker** | 0…1 | 1 | oil-tanker cargo | Output level (1.0 = ±5 V peak) |
| **gulf / dry** | 0…1 | 1 | Gulf of Oman | Crossfade: 0 = plain 50% square, 1 = dual-closure wave |

### Inputs

All parameter CVs add 0.1× per volt and are clamped to the knob's valid range, except **tilt** (0.2×/V, clamped ±1).

| Jack | Signal | Function |
|---|---|---|
| **v/oct** | ±5 V | 1 V/oct pitch CV |
| **tide** | Gate/Trig | Hard sync — rising edge resets phase to 0 |
| **swell** | ±5 V | FM — adds ¼ V/oct per volt to pitch |
| **dry cv** | ±5 V | CV for gulf / dry mix |
| **open** | ±5 V | CV for opening ceremony (C1 start) |
| **jckt** | ±5 V | CV for strait jacket (C1 width) |
| **lock** | ±5 V | CV for sanctions (C2 start) |
| **emgo** | ±5 V | CV for embargo (C2 width) |
| **slck** | ±5 V | CV for oil slick (LP filter) |
| **chok** | ±5 V | CV for choke point (tanh drive) |
| **tilt** | ±5 V | CV for persian tilt — applies to both C1 and C2 |
| **tnk** | ±5 V | CV for tanker (output level) |

### Outputs

| Jack | Signal | Function |
|---|---|---|
| **eoc** | 0 / 10 V | 1 ms trigger at the end of every cycle |
| **c1** | 0 / 10 V | Gate — high while inside closure 1 (yellow LED) |
| **c2** | 0 / 10 V | Gate — high while inside closure 2 (red LED) |
| **passage** | ±5 V audio | Main oscillator output (green LED = level) |

---

## Windows build notes

### Why there is a CRT heap bridge

`libRack.dll` is compiled against **MSVCRT** (`msvcrt.dll`), which uses its own private heap. Modern MSYS2 MinGW64 defaults to **UCRT** (`ucrtbase.dll`), a separate private heap.

At runtime Rack calls `delete` on widgets our plugin allocates with `new`. Because `new` used UCRT's `malloc` but `delete` reaches MSVCRT's `free`, the two heap handles don't match and `RtlFreeHeap` crashes with signal 11.

The fix at the top of `src/WaveOfHormuz.cpp` overrides global `operator new` / `operator delete` to load `msvcrt.dll` at startup via `LoadLibrary` and call its `malloc`/`free` directly.

### Why labels are drawn in C++ rather than SVG

NanoSVG (the Rack SVG renderer) silently discards all `<text>` elements. All visible text is rendered programmatically via the `PanelText` widget in `src/WaveOfHormuz.cpp`.

---

## File layout

```
WaveOfHormuz/
├── plugin.json          manifest (slug, version, module list)
├── Makefile             standard VCV Rack 2 Makefile
├── build.sh             build helper (build / install / clean / dist)
├── src/
│   ├── plugin.hpp       shared header (Plugin*, Model* declarations)
│   ├── plugin.cpp       init() — registers the module
│   └── WaveOfHormuz.cpp all DSP, widget, and label code
└── res/
    └── WaveOfHormuz.svg panel artwork (16 HP, dark-navy / teal / gold)
```

---

## License

[MIT](LICENSE) — free to use, modify, and redistribute (attribution required).
