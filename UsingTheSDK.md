# Using the SDK to create a simple iOS dylib / tweak

Direct translation of [Encryqed/Dumper-7's UsingTheSDK.md](https://github.com/Encryqed/Dumper-7/blob/main/UsingTheSDK.md) for the iOS port — same structure, same examples, swapped out Windows-DLL plumbing for theos/Xcode and Mach-O.

---

## theos project setup
1. Create a new theos tweak: `$THEOS/bin/nic.pl` → `iphone/tweak`.
2. Edit your `Makefile`:
   ```makefile
   ARCHS = arm64
   TARGET = iphone:clang:latest:14.0

   include $(THEOS)/makefiles/common.mk

   TWEAK_NAME = MyTweak
   MyTweak_FILES   = Tweak.xm Basic.cpp
   MyTweak_CFLAGS  = -fobjc-arc -std=c++20 -Wno-unused -Wno-deprecated
   MyTweak_LDFLAGS = -framework Foundation

   include $(THEOS_MAKE_PATH)/tweak.mk
   ```
3. Set your **C++ Language Standard** to **c++20** (or later — the SDK uses C++20 features).
4. Add `Tweak.xm` (or `.xm` / `.mm`) — see code [here](#code).

## Xcode project setup
1. Create a new empty C++ / Objective-C target. Set product type to **Dynamic Library**.
2. In Build Settings:
   - `CLANG_CXX_LANGUAGE_STANDARD` = `c++20`
   - `MACH_O_TYPE` = `mh_dylib`
   - `HEADER_SEARCH_PATHS` += `$(SRCROOT)/CppSDK`
3. Add a `Tweak.mm` file with the entry point.

---

## Including the SDK into the project
1. Take the contents of your `CppSDK` folder (by default `~/Documents/[GameVersion-GameName]/CppSDK/` inside the dumped app's container — copy off-device).
2. Drop the contents into your project's directory (alongside `Tweak.xm`).
3. If you do not care about your project's compilation time, add `#include "SDK/SDK.hpp"` at the top of `Tweak.xm`.
4. If you **do** care, and you want faster compilation-times, directly include only the files you require. \
   Adding `#include "SDK/Engine_classes.hpp"` is a good start in this case.
5. Add `Basic.cpp` and `CoreUObject_functions.cpp` to your project (in the `_FILES` list for theos, or via "Add Files…" in Xcode).
6. If you call a function from the SDK you need to add the .cpp file that contains the function-body to your project. \
   Example: calling `GetViewportSize()` from `APlayerController` requires you to add `Engine_functions.cpp` to your project.
7. Make sure your tweak calls `SDK::FName::Init()` once during process startup — this populates `GNames` / `AppendString` and applies any per-game decryption hooks the dumper baked into the SDK. Without it, every `FName::ToString()` returns `"None"`.
8. If there are any `static_assert`s failing or other build errors, read the [Common pitfalls](#common-pitfalls) section below.

---

## Using the SDK

### 1. Retrieving instances of classes/structs to manipulate them
   - `FindObject`, used to find an object by its name:
     ```c++
     SDK::UObject* Obj1 = SDK::UObject::FindObject("ClassName PackageName.Outer1.Outer2.ObjectName");
     SDK::UObject* Obj2 = SDK::UObject::FindObjectFast("ObjectName");

     SDK::UObject* Obj3 = SDK::UObject::FindObjectFast("StructName", EClassCastFlags::Struct); // Finds a UStruct
     ```
   - StaticFunctions / GlobalVariables, used to retrieve class-instances from static variables:
     ```c++
     /* UWorld::GetWorld() replaces GWorld, no offset required */
     SDK::UWorld* World = SDK::UWorld::GetWorld();
     SDK::APlayerController* MyController = World->OwningGameInstance->LocalPlayers[0]->PlayerController;
     ```

### 2. Calling functions
  - Non-Static functions
    ```c++
    SDK::APlayerController* MyController = MagicFuncToGetPlayerController();

    float OutX, OutY;
    MyController->GetMousePosition(&OutX, &OutY);
    ```
  - Static functions
    ```c++
    /* static functions do not require an instance, they are automatically called using their DefaultObject */

    // UE >= 4.21 SDK (TCHAR = char16_t) — use u"..."
    SDK::FName MyNewName = SDK::UKismetStringLibrary::Conv_StringToName(u"DemoNetDriver");

    // UE <  4.21 SDK (TCHAR = wchar_t)  — use L"..."
    SDK::FName MyNewName = SDK::UKismetStringLibrary::Conv_StringToName(L"DemoNetDriver");
    ```
    > Note: the iOS port emits the literal width based on the dumped engine version. **UE 4.21+** uses `TCHAR = char16_t` (so `u"..."`). **UE 4.17–4.20** keeps the original `TCHAR = wchar_t` (so `L"..."`, which is 32-bit on Darwin — matches upstream behavior for those versions). Check the `using TCHAR = …` line at the top of generated `Basic.hpp` if unsure which one applies to your SDK.

### 3. Checking a UObject's type
  - With `EClassCastFlags`
    ```c++
    /* Limited to base types, but is the fastest option */
    const bool bIsActor = Obj->IsA(EClassCastFlags::Actor);
    ```
  - With `UClass*`
    ```c++
    /* Ideal for native classes. Use `StaticName()` for Blueprint classes instead */
    const bool bIsSpecificActor = Obj->IsA(ASomeSpecificActor::StaticClass());
    ```
  - With `FName` (class name)
    ```c++
    /* Works for every class */
    const bool bIsSpecificActor = Obj->IsA(ASomeSpecificActor_C::StaticName());
    ```

### 4. Casting
  UnrealEngine heavily relies on inheritance and often uses pointers to a base class, which are later assigned addresses to \
  instances of child classes.
  ```c++
  if (MyController->Pawn->IsA(SDK::AGameSpecificPawn::StaticClass()))
  {
      SDK::AGameSpecificPawn* MyGamePawn = static_cast<SDK::AGameSpecificPawn*>(MyController->Pawn);
      MyGamePawn->GameSpecificVariable = 30;
  }
  ```

### 5. `GObjects` — the global `UObject` array
  `SDK::UObject::GObjects` is a static pointer to the `TUObjectArray` (the inner `ObjObjects` sub-struct of `FUObjectArray`, not the outer wrapper). Populated automatically when `SDK::FName::Init()` runs and the resolver finds the array offset.

  ```c++
  /* Total number of objects (live + GC'd slots) */
  int32 Total = SDK::UObject::GObjects->Num();

  /* Index lookup — returns nullptr for invalid / freed slots */
  SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(42);
  ```

  Under the hood, `GetByIndex` calls `GetDecrytedObjPtr()` which applies the per-game `DecryptPtr` lambda (XOR or transform) the dumper baked into the SDK. Encrypted pools (Back4Blood, Multiversus, …) "just work" — you don't unwrap manually.

  Common iteration pattern:
  ```c++
  for (int32 i = 0; i < SDK::UObject::GObjects->Num(); ++i)
  {
      SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);
      if (!Obj) continue;                   // freed slot
      if (Obj->IsDefaultObject()) continue; // skip CDOs

      if (Obj->IsA(SDK::EClassCastFlags::Pawn))
      {
          SDK::APawn* Pawn = static_cast<SDK::APawn*>(Obj);
          // ...
      }
  }
  ```

  > Iterating `GObjects` is O(N) on the order of millions of entries — fine for one-shot scans, expensive in a tick handler. Cache the pointers you need.

  If you need the raw underlying chunk pointer (e.g. for memory inspection):
  ```c++
  SDK::FUObjectItem* ItemsBase = SDK::UObject::GObjects->GetDecrytedObjPtr();
  ```
  This returns the decrypted base pointer of the chunk array; from there it's standard chunk-walking.

---

## Code
### Tweak entry point and worker thread

```cpp
// Tweak.xm  (or Tweak.mm)
#import <Foundation/Foundation.h>
#include <thread>

#include "SDK/SDK.hpp"

static void Fun()
{
    /* Initialize the SDK — populates GNames/AppendString and applies any
       per-game decryption hooks the dumper baked in. Must be called once. */
    SDK::FName::Init();

    // Your code here
}
```

### Example program that enables the UnrealEngine console

```cpp
#import <Foundation/Foundation.h>
#include <thread>

#include "SDK/Engine_classes.hpp"

// Basic.cpp was added to the project
// Engine_functions.cpp was added to the project

static void Fun()
{
    SDK::FName::Init();

    /* Functions returning "static" instances */
    SDK::UEngine* Engine = SDK::UEngine::GetEngine();
    SDK::UWorld*  World  = SDK::UWorld::GetWorld();

    /* Getting the PlayerController, World, OwningGameInstance, ... should all be checked not to be nullptr! */
    SDK::APlayerController* MyController = World->OwningGameInstance->LocalPlayers[0]->PlayerController;

    /* Print the full-name of an object ("ClassName PackageName.OptionalOuter.ObjectName") */
    NSLog(@"%s", Engine->ConsoleClass->GetFullName().c_str());

    /* Manually iterating GObjects and printing the FullName of every UObject that is a Pawn (not recommended) */
    for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject())
            continue;

        /* Only the 'IsA' check using the cast flags is required, the other 'IsA' is redundant */
        if (Obj->IsA(SDK::APawn::StaticClass()) || Obj->HasTypeFlag(SDK::EClassCastFlags::Pawn))
        {
            NSLog(@"%s", Obj->GetFullName().c_str());
        }
    }

    /* You might need to loop all levels in UWorld::Levels */
    SDK::ULevel* Level = World->PersistentLevel;
    SDK::TArray<SDK::AActor*>& Actors = Level->Actors;

    for (SDK::AActor* Actor : Actors)
    {
        /* The 2nd and 3rd checks are equal, prefer using EClassCastFlags if available for your class. */
        if (!Actor || !Actor->IsA(SDK::EClassCastFlags::Pawn) || !Actor->IsA(SDK::APawn::StaticClass()))
            continue;

        SDK::APawn* Pawn = static_cast<SDK::APawn*>(Actor);
        // Use Pawn here
    }

    /*
    * Changes the keyboard-key that's used to open the UE console
    *
    * This is a rare case of a DefaultObjects' member-variables being changed.
    * By default you do not want to use the DefaultObject, this is a rare exception.
    */
    SDK::UInputSettings::GetDefaultObj()->ConsoleKeys[0].KeyName = SDK::UKismetStringLibrary::Conv_StringToName(u"F2");

    /* Creates a new UObject of class-type specified by Engine->ConsoleClass */
    SDK::UObject* NewObject = SDK::UGameplayStatics::SpawnObject(Engine->ConsoleClass, Engine->GameViewport);

    /* The Object we created is a subclass of UConsole, so this cast is **safe**. */
    Engine->GameViewport->ViewportConsole = static_cast<SDK::UConsole*>(NewObject);
}
```

---

## Common pitfalls

- **`SDK::FName::Init()` not called first** — every `FName::ToString()` returns `"None"`, every `FindObject` fails. Always call once before any SDK use.
- **Mismatched SDK after a game update** — UE classes/offsets shift between game patches. Re-dump after any binary update; the old SDK will silently read garbage.
- **Including `SDK.hpp` in headers** — heavy template instantiation, long compile times. Only include it in `.cpp` / `.mm` files; use forward decls in headers.
- **Wrong string-literal prefix for the SDK's `TCHAR`** — UE 4.21+ SDKs use `TCHAR = char16_t` → `u"..."`. UE 4.17–4.20 SDKs use `TCHAR = wchar_t` → `L"..."` (32-bit on Darwin). Mismatched literals will compile-error on `FString` / `FName(const TCHAR*)` constructions. Confirm with the `using TCHAR = …` line at the top of `Basic.hpp`.
- **`UnknownProperty` classes in `PropertyFixup.hpp`** — the dumper couldn't identify the property's class. The dummy struct is the right size and alignment but you can't navigate into it. Usually safe to ignore.
- **Custom `GetImageBase`** — `Basic.cpp` emits a default `_dyld_get_image_header(0)` implementation. For dylibs loaded into non-default images (rare), override `InSDKUtils::GetImageBase()` to walk `_dyld_image_count()` looking for your target.

---

## Credits

- **[Encryqed](https://github.com/Encryqed)** — original [Dumper-7](https://github.com/Encryqed/Dumper-7) (Windows/x86_64) — this guide is a direct iOS adaptation of upstream's [UsingTheSDK.md](https://github.com/Encryqed/Dumper-7/blob/main/UsingTheSDK.md).
- **[Aethereux](https://github.com/Aethereux)** — iOS/ARM64 port + ongoing maintenance.
