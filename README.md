# Husk

<p align="center">
  <a href="https://trendshift.io/repositories/233057?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-233057" target="_blank" rel="noopener noreferrer">
    <img src="https://trendshift.io/api/badge/trendshift/repositories/233057/daily?language=Swift" alt="Leviidev/Husk | Trendshift" width="250" height="55"/>
  </a>
</p>

[![Husk Downloads](https://img.shields.io/github/downloads/leviidev/husk/total?style=for-the-badge&color=5865F2&labelColor=111111)](https://github.com/leviidev/husk/releases)

Android games on your iPhone, running natively.

> This is a fork of [Leviidev/Husk](https://github.com/Leviidev/Husk) that
> keeps **only the translation layer**: emulation (QEMU and the LineageOS
> guest) is removed, and the app is redesigned around games.

Add an APK, tap Play, and it opens full-screen. The game's own 64-bit Android
code runs directly on the iPhone's processor, and Husk stands in for Android
around it: the C library, the Java calls the game makes, OpenGL ES (through
ANGLE), Vulkan (through MoltenVK), sound, touch and controllers. No Android
boots, so a game starts in seconds, and its code is never emulated.
See [docs/04-translation-layer.md](docs/04-translation-layer.md).

While a game runs nothing is drawn over it. Swipe down from the top edge to bring
down its toolbar (close, on-screen controller, performance overlay); it hides
itself again a few seconds later. The performance overlay (frame rate, frame
time, memory, thermal state) is turned on in Settings or from that toolbar.

## Screenshots

### Games on the Translation Layer

<table>
  <tr>
    <td align="center"><img src="Screenshots/gta-san-andreas.jpeg" alt="GTA: San Andreas" width="100%"><br><sub><b>GTA: San Andreas</b></sub></td>
    <td align="center"><img src="Screenshots/minecraft-dungeons.jpeg" alt="Minecraft Dungeons" width="100%"><br><sub><b>Minecraft Dungeons</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="Screenshots/beach-buggy-racing-2.jpeg" alt="Beach Buggy Racing 2" width="100%"><br><sub><b>Beach Buggy Racing 2</b></sub></td>
    <td align="center"><img src="Screenshots/geometry-dash.jpeg" alt="Geometry Dash" width="100%"><br><sub><b>Geometry Dash</b></sub></td>
  </tr>
</table>

### The app

<table>
  <tr>
    <td align="center" width="33%"><img src="Screenshots/library.png" alt="Library" width="100%"><br><sub><b>Library</b></sub></td>
    <td align="center" width="33%"><img src="Screenshots/game-settings.png" alt="Game Settings" width="100%"><br><sub><b>Game Settings</b></sub></td>
    <td align="center" width="33%"><img src="Screenshots/settings.png" alt="Settings" width="100%"><br><sub><b>Settings</b></sub></td>
  </tr>
</table>

## What the Translation Layer runs

Games made with an engine Husk has a driver for:

- Unity
- Unreal Engine 4 (through Vulkan)
- cocos2d-x
- Godot 3 and 4 (GLES2, GLES3 and the Compatibility renderer)
- SDL2 and SDL3, including LÖVE games
- GameActivity (Minecraft) and NativeActivity
- Rockstar's own engine (GTA: San Andreas)

Games see Google Play services as installed and signed out, so the ones that
check for it start normally; Play Games sign-in, cloud saves and purchases are
not available. Geometry Dash can load [Geode](https://geode-sdk.org) mods.

An APK needs 64-bit (`arm64-v8a`) native code. iPhones cannot run 32-bit ARM
code, so an APK that only has 32-bit libraries cannot run here. APKs and
split bundles (`.xapk`, `.apkm`, `.apks`, or a Play download's separate split
APKs and asset packs) can all be added. Apps written only
in Java, with no native engine, are not supported.

One game runs per launch of Husk. To switch, press **Close Husk to Play** on
the other game's page; when you open Husk again, that game starts by itself.

A game's saves can be backed up to a `.zip` from its page and restored later,
on the same iPhone or another. If a game crashes Husk, the next launch shows
what happened, with a report you can share.

## Installing

Download `Husk.ipa` from [Releases](https://github.com/leviidev/husk/releases)
and install it with SideStore, AltStore or TrollStore. It is one IPA for all
of them: it carries Husk's entitlements, which TrollStore keeps, and a
sideloader re-signs it with your own. Husk needs iOS 16.4 or later.

## JIT

Games need JIT, which on iOS comes from a debugger. Husk can get it in several ways, and walks you through each one
(Settings › JIT & Sideload):

- **Built-in StikJIT** (iOS 26 and later): Husk turns JIT on itself, with no
  second app. On iOS 27 it pairs with your iPhone from Settings, with no
  computer. See [docs/06-built-in-jit.md](docs/06-built-in-jit.md).
- **StikDebug**, installed alongside Husk.
- **TrollStore or a Dopamine jailbreak** ("Allow JIT in Apps"), on iOS
  versions before 26 only. From iOS 26 only a debugger can grant JIT.

## Building

Husk is built on a Mac with Xcode. The build scripts also call `python3`,
`cmake`, `xcodegen` and a Rust toolchain with the `aarch64-apple-ios` target.

No Mac? Every push to this repository builds `Husk.ipa` on a GitHub Actions
macOS runner (`.github/workflows/build-ipa.yml`); download it from the run's
**Husk-ipa** artifact.

```sh
./scripts/ci_build.sh
```

From a clean checkout this builds everything Husk embeds or links:

- ANGLE (EGL and OpenGL ES over Metal)
- MoltenVK (Vulkan over Metal)
- the on-device pairing library

It then builds the app and writes the IPA to `build/Husk.ipa`. The first run
takes about fifteen minutes, and later runs reuse what is already built. After
that, `./scripts/package_ipa.sh` rebuilds just the app and writes
`~/Desktop/Husk.ipa`.

`tools/regress/run.sh` plays a set of games on the Mac through the
translation layer and checks each against a reference screenshot (see the
top of `tools/regress/cases.txt` for what it needs).

## Licence

GPL-2.0-or-later, as upstream Husk; the full source is public. Husk cannot go
on the App Store, because it needs `get-task-allow` plus a debugger attaching
at runtime. See [docs/01-licensing.md](docs/01-licensing.md).
