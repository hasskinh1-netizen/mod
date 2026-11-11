# iOS-Dumper-7

**iOS-Dumper-7** is a runtime SDK generator for Unreal Engine games on iOS / arm64. It loads as a dylib inside the game process, scans memory to discover the engine's reflection layout, then emits a complete C++ SDK, IDA mapping files, JSON metadata, and a single-header summary of every offset it found.

Originally a port of [Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7) (Windows/x86_64) — heavily reworked for iOS/clang/Mach-O.

---

## Features

* **Runtime SDK generation** — C++ SDK headers, `.idmap` IDA scripts, Dumpspace JSON, USMAP, and a `UEOffsets.hpp` summary header.
* **Dynamic offset scanning** — `GObjects`, `GNames`, `GWorld`, every `UObject`/`UStruct`/`UFunction`/`UProperty` field offset is discovered without hardcoded values for most games.
* **ProcessEvent autodiscovery** — ports the iOS_UEDumper vtable-scoring algorithm: walks UObject's vtable and fingerprints each slot against UE-source-level invariants (`UObject.Index` load, `FUObjectItem` stride, `UFunction.FunctionFlags+1/+2` byte loads, `UStruct.Size` LDR, `ChildProperties` LDR, ADRP chain to GUObjectArray). No more manual `InitPE(idx)` for most games.
* **FNamePool sharded-layout support** — `GetNumChunks` / `GetByteCursor` walk `Blocks[]` and the live block dynamically instead of relying on fixed-offset header fields that don't exist in every UE variant.
* **Per-game decryption hooks** — five independent hooks for games that XOR/scramble different layers:
  * `InitObjectArrayDecryption` — UObject pointer XOR (Back4Blood, Multiversus)
  * `InitNameEntryDecryption` — raw FNameEntry-bytes transform (header itself encrypted)
  * `InitNameStringDecryption` — output std::string transform inside `ToString` (DeltaForce)
  * `InitNameArrayDecryption` — TNameEntryArray pointer indirection (PUBG, UE ≤ 4.22)
  * `InitNamePoolDecryption` — FNamePool pointer indirection (Valorant, UE 4.23+)
* **Floating ImGui menu** — a draggable logo appears ~3 seconds after launch; tap to open, drag if it covers game UI.
* **UE 4.17 → 4.26** verified (ARK 2.0, ARK Revamp, Special Forces 3, ArenaBreakout, HOK: World / NGR).

---

## Quick Start

### 1. Build

1. Open `Dumper.xcodeproj` in Xcode.
2. Set your signing identity.
3. Build target `Dumper` → produces `Dumper.dylib` under `build/Release-iphoneos/`.

### 2. Inject

Sideload with any tool that supports dylib injection — Sideloadly, ESign, GBox, TrollStore + Choicy, etc.

### 3. Dump

1. Launch the game and wait for the engine to fully initialize (load past the splash).
2. After ~3 seconds, a **floating logo** appears on screen.
3. Tap the logo to open the menu. Drag it if it's in the way.
4. Tap **Start Dump** and wait for completion.

### 4. Output

Files land under your app's Documents directory (enable "Supports Document Browser" in your Info.plist before signing so iOS Files.app can browse it):

```
/Documents/[GameVersion-GameName]/
├── CppSDK/                    # Full C++ SDK headers (one .hpp per package)
├── Mappings/                  # USMAP files
├── IDAMappings/               # .idmap script for IDAExecFunctionsImporter plugin
├── Dumpspace/                 # JSON reflection metadata
├── GObjects-Dump.txt          # Flat list of every UObject + full path name
├── GObjects-Dump-WithProperties.txt
└── UEOffsets.hpp              # Single-header summary of every offset + runtime pointer (new)
```

`UEOffsets.hpp` is a self-contained header you can `#include` in any external tool (cheat client, debug helper, profile generator) to get every offset the dumper discovered in one place — UObject layout, FNamePool layout, every FProperty subclass field, ProcessEvent vtable index, GWorld/GObjects/GNames absolute addresses, etc.

---

## Configuration & Manual Overrides

Most games work without configuration. For protected games — anti-cheat, custom encryption, non-standard FNamePool — edit `Generator::InitEngineCore()` in [`Dumper/Generator/Private/Generators/Generator.cpp`](Dumper/Generator/Private/Generators/Generator.cpp). The function ships with commented examples for each supported game.

### GObjects (Global Object Array)

If auto-scan fails, supply the address and layout:

```cpp
// UE 4.21+ / UE 5  (FChunkedFixedUObjectArray)
ObjectArray::Init(/*GObjectsOffset*/ 0x0E23DAF0,
                  /*ElementsPerChunk*/ 0x10000,
                  FChunkedFixedUObjectArrayLayout{
                      .ObjectsOffset     = 0x00,
                      .MaxElementsOffset = 0x10,
                      .NumElementsOffset = 0x14,
                      .MaxChunksOffset   = 0x18,
                      .NumChunksOffset   = 0x1C,
                  },
                  "NGR");  // optional Mach-O image name

// UE <= 4.20  (FFixedUObjectArray)
ObjectArray::Init(0x12345678, FFixedUObjectArrayLayout{
    .ObjectsOffset    = 0x0,
    .MaxObjectsOffset = 0x8,
    .NumObjectsOffset = 0xC,
});
```

`GObjectsOffset` is the offset to the `ObjObjects` sub-struct (not the `FUObjectArray` wrapper), measured from imagebase. Same convention as Mj0x's iOS_UEDumper.

### GNames (Global Name Array / FNamePool)

```cpp
// Args: Offset, EOffsetOverrideType::GNames, bIsNamePool, ModuleName (optional)
FName::Init((int32)0x0E226540, FName::EOffsetOverrideType::GNames, true /*FNamePool*/, "NGR");
```

For UE ≤ 4.22 (TNameEntryArray) pass `false` for the `bIsNamePool` argument.

### ProcessEvent

You usually shouldn't need this. The scorer runs by default and prints:

```
[Info] PE-Index (auto): 0x47
[Info] PE-Offset: 0x...
```

If the scorer picks the wrong slot for a future game (rare — needs 7 fingerprints to disagree with the canonical UObject::ProcessEvent body), override manually:

```cpp
Off::InSDK::ProcessEvent::InitPE(70);  // direct vtable index
```

### Per-Game Decryption Hooks

```cpp
// (a) Back4Blood / Multiversus — UObject pointer XOR.
//     Install BEFORE ObjectArray::Init.
InitObjectArrayDecryption([](void* ObjPtr) -> uint8* {
    return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375ACDE);
});

// (b) DeltaForce — output-string XOR (header is plaintext, chars are XOR'd).
//     Install BEFORE FName::Init. Runs at the tail of FNameEntry::GetString,
//     after the entry has been decoded normally.
InitNameStringDecryption([](std::string Decoded) -> std::string {
    const uint32_t Len = (uint32_t)Decoded.size();
    if (Len == 0) return Decoded;

    uint32_t Key = 0;
    switch (Len % 9)
    {
        case 0: Key = (Len & 0x1F) + Len; break;
        case 1: Key = (Len ^ 0xDF) + Len; break;
        case 2: Key = (Len | 0xCF) + Len; break;
        case 3: Key = 33 * Len;           break;
        case 4: Key = Len + (Len >> 2);   break;
        case 5: Key = 3 * Len + 5;        break;
        case 6: Key = ((4 * Len) | 5) + Len; break;
        case 7: Key = ((Len >> 4) | 7) + Len; break;
        case 8: Key = (Len ^ 0xC) + Len;  break;
        default: Key = (Len ^ 0x40) + Len; break;
    }
    for (uint32_t i = 0; i < Len; ++i)
        Decoded[i] = (char)((Key & 0x80) ^ ~(uint8_t)Decoded[i]);
    return Decoded;
});

// (c) PUBG (UE 4.17, TNameEntryArray) — ADRP+ADD lands on an encrypted struct
//     [int32 Header | uintptr_t* FirstHop]; walk Hops = (Header - 100) / 3
//     dereferences and return the resulting TNameEntryArray** (the runtime
//     does the final deref). Mirrors Mj0x's PUBG GetNamesPtr verbatim.
//     Install BEFORE FName::Init with bIsNamePool == false.
//     `RawAddr` is `ImageBase + GNamesOffset` (no upfront deref). Every
//     dereference is guarded by IsBadReadPtr because corrupted / mid-init
//     states return junk values that look pointer-ish.
InitNameArrayDecryption([](uintptr_t RawAddr) -> uintptr_t {
    if (!RawAddr || IsBadReadPtr((void*)RawAddr) || IsBadReadPtr((void*)(RawAddr + 8)))
        return 0;

    const int32_t Header = *reinterpret_cast<int32_t*>(RawAddr);
    if (Header < 100) return 0;
    uint32_t Hops = (uint32_t)((Header - 100) / 3);
    if (Hops == 0 || Hops > 16) return 0;

    uint64_t Chain[16]{};
    Chain[Hops - 1] = *reinterpret_cast<int64_t*>(RawAddr + 8);

    while (Hops >= 2) {
        const uintptr_t Next = Chain[Hops - 1];
        if (!Next || IsBadReadPtr((void*)Next)) return 0;
        Chain[Hops - 2] = *reinterpret_cast<int64_t*>(Next);
        --Hops;
    }
    return Chain[0]; // TNameEntryArray** — runtime derefs once
});

```

Each `InitX(...)` macro auto-captures the lambda source string for future SDK emission.

---

## Architecture

| Stage | Driver | What happens |
|---|---|---|
| Inject | `+load` on `DumperObjC` | Dylib loaded by dyld; schedules a 3 s wakeup and returns immediately so it never blocks init. |
| UI | ImGui + Metal/UIKit | Floating logo → menu overlay; rendered into the game's CAMetalLayer. |
| Discover | `ObjectArray::Init` / `NameArray::TryInit` | Pattern + heuristic scans of `__TEXT` to find `GObjects` and `GNames`. Manual overrides via `Generator::InitEngineCore`. |
| Probe offsets | `Off::Init` | Iterates known UE classes (`Object`, `Field`, `Struct`, …), reads byte patterns, derives every field offset (~40 values). |
| ProcessEvent | `Off::InSDK::ProcessEvent::InitPE` | Walks UObject vtable, scores each function against 7 ProcessEvent fingerprints, picks the winner. |
| GWorld | `Off::InSDK::World::InitGWorld` | Finds the `World` UObject, scans BSS for a `UWorld**` pointing at it. |
| Generate | `CppGenerator`, `MappingGenerator`, … | Emits per-package SDK headers, USMAP, IDA mappings, Dumpspace JSON, and `UEOffsets.hpp`. |

The Engine + OffsetFinder code is shared with upstream Dumper-7; the iOS-specific layer lives under [`Dumper/Platform/`](Dumper/Platform/) (ARM64 instruction decoding, Mach-O segment walks, vm-region reads).

---

## Project Layout

```
Dumper/
├── main.mm                  # entry point, +load, menu lifecycle
├── Settings.h               # global settings + per-game flags
├── Menu/                    # ImGui menu code
├── ImGui/                   # vendored ImGui (Metal + UIKit backends)
├── Platform/                # iOS/arm64 platform layer (replaces upstream Windows surface)
│   ├── Public/{Platform,Architecture}.h
│   └── Private/{PlatformIOS,Arch_arm64}.{h,cpp}
├── Engine/                  # UE reflection layer (shared shape with upstream)
│   ├── Public/Unreal/       # NameArray, ObjectArray, UnrealTypes, wrappers
│   └── Private/             # implementations
└── Generator/               # SDK emitters
    ├── Public/Generators/   # CppGenerator, MappingGenerator, IDAMappingGenerator, DumpspaceGenerator, Generator
    └── Private/Generators/  # implementations
```

---

## Credits

* **[Encryqed](https://github.com/Encryqed)** — original [Dumper-7](https://github.com/Encryqed/Dumper-7) (Windows/x86_64).
* **[MJx0](https://github.com/MJx0)** — [AndUEDumper](https://github.com/MJx0/AndUEDumper) / iOS_UEDumper — ProcessEvent vtable-scoring algorithm, KittyMemory.
* **[Aethereux](https://github.com/Aethereux)** — iOS/ARM64 port + ongoing maintenance.

Contributions welcome — open an issue or PR with the game name, UE version, and a snippet of `Generator::InitEngineCore` config that worked.

---

## TODO

- [x] Port ProcessEvent autodiscovery from iOS_UEDumper's vtable scorer.
- [x] Make `GetNumChunks` / `GetByteCursor` layout-independent (walk `Blocks[]` instead of reading a fixed offset).
- [x] Emit `UEOffsets.hpp` consolidating every discovered offset + runtime pointer.
- [x] Per-game `InitNameArrayDecryption` / `InitNamePoolDecryption` hooks.
- [ ] Auto-discover TNameEntryArray's layout for UE ≤ 4.22 games (currently heuristic, brittle on obfuscated builds).
- [ ] Fix `FName::AppendString` fallback path in [`UnrealTypes.cpp`](Dumper/Engine/Private/Unreal/UnrealTypes.cpp).
- [ ] Wire `DecryptionLambdaStr` / `NamePoolDecryptionLambdaStr` into `CppGenerator` so SDK output includes per-game decryption stubs automatically.
- [ ] Auto-expose `Off::UFunction::NumParms` + `Off::UFunction::ParmsSize` (currently derived by walking Children).
