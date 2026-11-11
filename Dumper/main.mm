#include <iostream>
#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <cstdio>

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"

#include "Generators/Generator.h"

#import <Foundation/Foundation.h>

#include "main.h"
#include "Menu/Logger.h"
#include "Menu/Diagnostics.h"

#include "Unreal/NameArray.h"

using namespace std::chrono_literals;

/* Resolves GameName/GameVersion through KismetSystemLibrary.
 * Every step is guarded: on unusual targets the Kismet class or one of the two
 * UFunctions can be missing, and calling ProcessEvent with a null function (or
 * with PEIndex still 0) jumps into garbage and kills the whole game. */
static bool TryReadGameInfoFromKismet()
{
    UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
    if (!Kismet)
    {
        LogError("KismetSystemLibrary was not found - skipping GetGameName/GetEngineVersion\n");
        return false;
    }

    UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
    UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

    if (!GetGameName || !GetEngineVersion)
    {
        LogError("KismetSystemLibrary functions missing (GetGameName=%s, GetEngineVersion=%s) - skipping\n",
                 GetGameName ? "found" : "null", GetEngineVersion ? "found" : "null");
        return false;
    }

    FString Name;
    FString Version;

    DiagSetStage("StartDump: KismetSystemLibrary::GetGameName");
    Kismet.ProcessEvent(GetGameName, &Name);

    DiagSetStage("StartDump: KismetSystemLibrary::GetEngineVersion");
    Kismet.ProcessEvent(GetEngineVersion, &Version);

    Settings::Generator::GameName = Name.ToString();
    Settings::Generator::GameVersion = Version.ToString();

    return !Settings::Generator::GameName.empty() || !Settings::Generator::GameVersion.empty();
}

void StartDump()
{
    DiagInstall();
    DiagSetStage("StartDump: entered (2s settle delay)");

    LogInfo("[DIAG] Log file: %s\n", DiagGetLogPath()[0] ? DiagGetLogPath() : "<none, console only>");

    std::this_thread::sleep_for(2s);

    try
    {
        DiagSetStage("StartDump: Settings::Config::DelayDumperStart");
        Settings::Config::DelayDumperStart();

        auto t_1 = std::chrono::high_resolution_clock::now();

        LogSuccess("Started Generation [Dumper-7]!\n");

        DiagSetStage("StartDump: Generator::InitEngineCore");
        Generator::InitEngineCore();

        if (DiagHasFatal())
        {
            LogError("Engine core setup failed (see the first error above) - generation stopped, "
                     "the game was left running.\n");
            DiagSetStage("StartDump: aborted after InitEngineCore");
            return;
        }

        DiagSetStage("StartDump: Generator::InitInternal");
        Generator::InitInternal();

        if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
        {
            DiagSetStage("StartDump: reading GameName/GameVersion via ProcessEvent");
            TryReadGameInfoFromKismet();
        }

        if (Settings::Generator::GameName.empty())
            Settings::Generator::GameName = "Unknown";

        if (Settings::Generator::GameVersion.empty())
            Settings::Generator::GameVersion = "Unknown";

        LogInfo("GameName: %s\n", Settings::Generator::GameName.c_str());
        LogInfo("GameVersion: %s\n\n", Settings::Generator::GameVersion.c_str());

        DiagSetStage("StartDump: Generator::Generate<CppGenerator>");
        Generator::Generate<CppGenerator>();

        DiagSetStage("StartDump: Generator::Generate<MappingGenerator>");
        Generator::Generate<MappingGenerator>();

        DiagSetStage("StartDump: Generator::Generate<IDAMappingGenerator>");
        Generator::Generate<IDAMappingGenerator>();

        DiagSetStage("StartDump: Generator::Generate<DumpspaceGenerator>");
        Generator::Generate<DumpspaceGenerator>();

        auto t_C = std::chrono::high_resolution_clock::now();
        auto ms_int_ = std::chrono::duration_cast<std::chrono::milliseconds>(t_C - t_1);
        std::chrono::duration<double, std::milli> ms_double_ = t_C - t_1;

        DiagSetStage("StartDump: finished");
        LogInfo("\n\nGenerating SDK took (%fms)\n\n\n", ms_double_.count());
    }
    catch (const std::exception& Exception)
    {
        LogError("StartDump: unhandled C++ exception: %s\n", Exception.what());
        DiagLog("[EXCEPTION] StartDump: %s\n", Exception.what());
    }
    catch (...)
    {
        LogError("StartDump: unhandled non-std exception\n");
        DiagLog("[EXCEPTION] StartDump: non-std exception\n");
    }
}
