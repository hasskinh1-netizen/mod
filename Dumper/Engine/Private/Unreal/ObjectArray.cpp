
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <unistd.h>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Utils.h"
#include "Menu/Logger.h"
#include "Menu/Diagnostics.h"


namespace fs = std::filesystem;

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,
		.MaxObjectsOffset = 0x8,
		.NumObjectsOffset = 0xC
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // DeltaForce
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x4,
		.MaxChunksOffset = 0x0,
		.NumChunksOffset = 0x14,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
};

bool IsAddressValidGObjects(const uintptr Address, const FFixedUObjectArrayLayout& Layout)
{
	/* It is assumed that the FUObjectItem layout is constant amongst all games using FFixedUObjectArray for ObjObjects. */
	struct FUObjectItem
	{
		void* Object;
		uint8 Pad[0x10];
	};

	void* Objects = SafeRead<void*>(Address + Layout.ObjectsOffset, nullptr);
	const int32 MaxElements = SafeRead<int32>(Address + Layout.MaxObjectsOffset, 0);
	const int32 NumElements = SafeRead<int32>(Address + Layout.NumObjectsOffset, 0);


	FUObjectItem* ObjectsButDecrypted = (FUObjectItem*)ObjectArray::DecryptPtr(Objects);

	if (NumElements > MaxElements)
        return false;

	if (MaxElements > 0x400000)
        return false;

	if (NumElements < 0x1000)
		return false;

	if (IsBadReadPtr(ObjectsButDecrypted))
        return false;

	/* IsBadReadPtr only proves that the first byte is readable; the object and
	 * its index live further in, so those are read with SafeRead as well. */
	void* FifthObject = SafeRead<void*>(reinterpret_cast<uintptr_t>(ObjectsButDecrypted) + (5 * sizeof(FUObjectItem)), nullptr);
	if (!FifthObject || IsBadReadPtr(FifthObject))
		return false;

	const uintptr FithObject = reinterpret_cast<uintptr>(FifthObject);
	const int32 IndexOfFithobject = SafeRead<int32>(FithObject + 0xC, -1);

	if (IndexOfFithobject != 0x5)
		return false;

	LogInfo("FFixedUObjectArray validation successful");
	return true;
}

bool IsAddressValidGObjects(const uintptr Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	void* Objects = SafeRead<void*>(Address + Layout.ObjectsOffset, nullptr);
	const int32 MaxElements = SafeRead<int32>(Address + Layout.MaxElementsOffset, 0);
	const int32 NumElements = SafeRead<int32>(Address + Layout.NumElementsOffset, 0);
	const int32 MaxChunks   = SafeRead<int32>(Address + Layout.MaxChunksOffset, 0);
	const int32 NumChunks   = SafeRead<int32>(Address + Layout.NumChunksOffset, 0);

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	if (NumChunks > 0x14 || NumChunks < 0x1)
		return false;

	if (MaxChunks > 0x22F || MaxChunks < 0x6)
        return false;
	
	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;
	
	/* There are never too many or too few chunks for all elements. Two different chunk-sizes (0x10000, 0x10400) occure on different UE versions and are checked for.*/
	const bool bNumChunksFitsNumElements = ((NumElements / 0x10000) + 1) == NumChunks || ((NumElements / 0x10400) + 1) == NumChunks;

	if (!bNumChunksFitsNumElements)
		return false;
	
	/* Same as above for the max number of elements/chunks. */
	const bool bMaxChunksFitsMaxElements = (MaxElements / 0x10000) == MaxChunks || (MaxElements / 0x10400) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;
	

	/* The chunk-pointer must always be valid (especially because it's already decrypted [if it was encrypted at all]) */
	if (!ObjectsPtrButDecrypted || IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;
	

	/* Check if every chunk-pointer is valid. Read through SafeRead: a candidate
	 * can point at a page that ends in the middle of this 20-entry array. */
	for (int i = 0; i < NumChunks; i++)
	{
		void* Chunk = SafeRead<void*>(reinterpret_cast<uintptr_t>(ObjectsPtrButDecrypted) + (i * sizeof(void*)), nullptr);
		if (!Chunk || IsBadReadPtr(Chunk))
			return false;
	}
	
	LogInfo("FChunkedFixedUObjectArray validation successful");
	return true;
}

namespace
{
	/* The scan used to dereference every 4-byte offset directly, which faults as
	 * soon as an offset lands on a hole in the segment - the whole game dies
	 * before the dumper can report "GObjects not found". Reading every field
	 * through SafeRead makes that safe but costs one Mach trap per field per
	 * offset (~23 per step), which turns a full-image pass into an hours-long
	 * hang. So the numeric fields are read from 1 MiB windows copied in bulk
	 * (one trap per window) and only the offsets that survive the cheap numeric
	 * filter pay for the pointer validation. */
	constexpr size_t ScanWindowSize  = 0x100000; /* 1 MiB */
	constexpr size_t ScanWindowSlack = 0x40;     /* furthest field is +0x38 */

	/* Copies up to Size bytes into Out and returns how many bytes the kernel
	 * actually copied (it stops at the first unmapped page). */
	vm_size_t ReadMemoryBulk(uintptr_t Address, size_t Size, void* Out)
	{
		vm_size_t Read = 0;
		const kern_return_t Kr = vm_read_overwrite(mach_task_self(),
		                                          (vm_address_t)Address,
		                                          (vm_size_t)Size,
		                                          (vm_address_t)Out,
		                                          &Read);

		if (Kr != KERN_SUCCESS && Read == 0)
			return 0;

		return Read;
	}

	/* Cheap numeric pre-filter evaluated on bytes we already own. Mirrors the
	 * scalar half of IsAddressValidGObjects() so that the expensive half only
	 * runs on plausible candidates. */
	bool IsPlausibleFixedGObjects(const uint8* Base, size_t Available, const FFixedUObjectArrayLayout& Layout)
	{
		if (Layout.MaxObjectsOffset < 0 || Layout.NumObjectsOffset < 0)
			return false;

		if (static_cast<size_t>(Layout.MaxObjectsOffset) + sizeof(int32) > Available ||
		    static_cast<size_t>(Layout.NumObjectsOffset) + sizeof(int32) > Available)
			return false;

		const int32 MaxElements = *reinterpret_cast<const int32*>(Base + Layout.MaxObjectsOffset);
		const int32 NumElements = *reinterpret_cast<const int32*>(Base + Layout.NumObjectsOffset);

		return NumElements >= 0x1000 && NumElements <= MaxElements && MaxElements <= 0x400000;
	}

	bool IsPlausibleChunkedGObjects(const uint8* Base, size_t Available, const FChunkedFixedUObjectArrayLayout& Layout)
	{
		if (Layout.MaxElementsOffset < 0 || Layout.NumElementsOffset < 0 ||
		    Layout.MaxChunksOffset < 0 || Layout.NumChunksOffset < 0)
			return false;

		if (static_cast<size_t>(Layout.MaxElementsOffset) + sizeof(int32) > Available ||
		    static_cast<size_t>(Layout.NumElementsOffset) + sizeof(int32) > Available ||
		    static_cast<size_t>(Layout.MaxChunksOffset) + sizeof(int32) > Available ||
		    static_cast<size_t>(Layout.NumChunksOffset) + sizeof(int32) > Available)
			return false;

		const int32 MaxElements = *reinterpret_cast<const int32*>(Base + Layout.MaxElementsOffset);
		const int32 NumElements = *reinterpret_cast<const int32*>(Base + Layout.NumElementsOffset);
		const int32 MaxChunks   = *reinterpret_cast<const int32*>(Base + Layout.MaxChunksOffset);
		const int32 NumChunks   = *reinterpret_cast<const int32*>(Base + Layout.NumChunksOffset);

		if (NumChunks < 0x1 || NumChunks > 0x14)
			return false;

		if (MaxChunks < 0x6 || MaxChunks > 0x22F)
			return false;

		if (NumElements > MaxElements || NumChunks > MaxChunks)
			return false;

		const bool bNumChunksFitsNumElements = ((NumElements / 0x10000) + 1) == NumChunks || ((NumElements / 0x10400) + 1) == NumChunks;
		if (!bNumChunksFitsNumElements)
			return false;

		const bool bMaxChunksFitsMaxElements = (MaxElements / 0x10000) == MaxChunks || (MaxElements / 0x10400) == MaxChunks;
		return bMaxChunksFitsMaxElements;
	}

	bool IsPlausibleGObjects(const uint8* Base, size_t Available)
	{
		if (Available < ScanWindowSlack)
			return false;

		for (const FFixedUObjectArrayLayout& Layout : FFixedUObjectArrayLayouts)
		{
			if (IsPlausibleFixedGObjects(Base, Available, Layout))
				return true;
		}

		for (const FChunkedFixedUObjectArrayLayout& Layout : FChunkedFixedUObjectArrayLayouts)
		{
			if (IsPlausibleChunkedGObjects(Base, Available, Layout))
				return true;
		}

		return false;
	}
}

void ObjectArray::InitializeFUObjectItem(uint8* FirstItemPtr)
{
    LogInfo("Initializing FUObjectItem...");

    /* A bad GObjects candidate can leave us with a null or unreadable item
     * pointer; walking it below would fault immediately. */
    if (!FirstItemPtr || IsBadReadPtr(FirstItemPtr))
    {
        LogError("InitializeFUObjectItem: invalid first item pointer (0x%llX) - keeping defaults",
                 (unsigned long long)(uintptr_t)FirstItemPtr);
        return;
    }

    for (int i = 0x0; i < 0x10; i += 4)
    {
        if (!IsBadReadPtr(*reinterpret_cast<uint8**>(FirstItemPtr + i)))
        {
            FUObjectItemInitialOffset = i;
            LogInfo("Found FUObjectItemInitialOffset: 0x%X", i);
            break;
        }
    }

    for (int i = FUObjectItemInitialOffset + 0x8; i <= 0x38; i += 4)
    {
        void* SecondObject = *reinterpret_cast<uint8**>(FirstItemPtr + i);
        void* ThirdObject  = *reinterpret_cast<uint8**>(FirstItemPtr + (i * 2) - FUObjectItemInitialOffset);

        if (!IsBadReadPtr(SecondObject) && !IsBadReadPtr(*reinterpret_cast<void**>(SecondObject)) && !IsBadReadPtr(ThirdObject) && !IsBadReadPtr(*reinterpret_cast<void**>(ThirdObject)))
        {
            SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
            LogInfo("Found SizeOfFUObjectItem: 0x%X", SizeOfFUObjectItem);
            break;
        }
    }

    Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
    Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;
    LogSuccess("FUObjectItem initialized successfully (Offset: 0x%X, Size: 0x%X)", FUObjectItemInitialOffset, SizeOfFUObjectItem);
}

void ObjectArray::InitDecryption(uint8* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	LogInfo("Initializing decryption: %s", DecryptionLambdaAsStr);
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
	LogSuccess("Decryption initialized");
}

void ObjectArray::InitializeChunkSize(uint8* ChunksPtr)
{
    LogInfo("Initializing chunk size...");

    if (!ChunksPtr || !ByIndex)
    {
        LogError("InitializeChunkSize: invalid chunk pointer (0x%llX) or missing ByIndex - keeping defaults",
                 (unsigned long long)(uintptr_t)ChunksPtr);
        return;
    }

    int IndexOffset = 0x0;
    uint8* ObjAtIdx374 = (uint8*)ByIndex(ChunksPtr, 0x374, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000);
    uint8* ObjAtIdx106 = (uint8*)ByIndex(ChunksPtr, 0x106, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000);

    if (!ObjAtIdx374 || !ObjAtIdx106 || IsBadReadPtr(ObjAtIdx374) || IsBadReadPtr(ObjAtIdx106))
    {
        LogError("InitializeChunkSize: could not resolve the probe objects (0x%llX / 0x%llX) - keeping default chunk size",
                 (unsigned long long)(uintptr_t)ObjAtIdx374, (unsigned long long)(uintptr_t)ObjAtIdx106);
        return;
    }

    for (int i = 0x8; i < 0x20; i++)
    {
        if (*reinterpret_cast<int32*>(ObjAtIdx374 + i) == 0x374 && *reinterpret_cast<int32*>(ObjAtIdx106 + i) == 0x106)
        {
            IndexOffset = i;
            LogInfo("Found IndexOffset: 0x%X", i);
            break;
        }
    }

    int IndexToCheck = 0x10400;
    while (ObjectArray::Num() > IndexToCheck)
    {
        if (void* Obj = ByIndex(ChunksPtr, IndexToCheck, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000))
        {
            const bool bHasBiggerChunkSize = (*reinterpret_cast<int32*>((uint8*)Obj + IndexOffset) != IndexToCheck);
            NumElementsPerChunk = bHasBiggerChunkSize ? 0x10400 : 0x10000;
            LogInfo("Determined chunk size: 0x%X", NumElementsPerChunk);
            break;
        }
        IndexToCheck += 0x10400;
    }

    Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;
    LogSuccess("Chunk size initialized: 0x%X", NumElementsPerChunk);
}

/* We don't speak about this function... */
void ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
    if (!bScanAllMemory)
        LogInfo("\nDumper-7 by me, you & him\n\n\n");

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);

    uintptr SearchBase = ImageBase;
    uintptr SearchRange = ImageSize;

    if (!bScanAllMemory)
    {
        /* UObjectArray/GObjects is a mutable global, so it lives in a data
         * segment - never in __TEXT. The original code scanned __TEXT first,
         * which can never match and forces the (very slow) full-image rescan on
         * every run. Try __DATA first and keep the full scan as the fallback. */
        const auto [DataSection, DataSize] = GetSegmentByName(Header, "__DATA");

        if (DataSection != 0x0 && DataSize > 0x1000)
        {
            SearchBase = DataSection;
            SearchRange = DataSize;
        }
        else
        {
            LogInfo("__DATA segment not found, scanning all memory");
            bScanAllMemory = true;
        }
    }

    /* Sub 0x50 so we don't try to read out of bounds memory when checking FixedArray->IsValid() or ChunkedArray->IsValid() */
    if (SearchRange <= 0x50)
    {
        LogError("ObjectArray::Init: search range too small (0x%lX) - nothing to scan", (unsigned long)SearchRange);
        DiagMarkFatal("GObjects search range is empty");
        return;
    }

    SearchRange -= 0x50;

    if (!bScanAllMemory)
        LogInfo("Searching for GObjects...\n\n");

    auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr Address)
    {
        for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
        {
            if (!IsAddressValidGObjects(Address, Layout))
                continue;

            if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
            {
                Off::FUObjectArray::bIsChunked = false;
                Off::FUObjectArray::FixedLayout = Layout;
            }
            else
            {
                Off::FUObjectArray::bIsChunked = true;
                Off::FUObjectArray::ChunkedFixedLayout = Layout;
            }

            return true;
        }
        
        return false;
    };

    /* The scan walks 1 MiB windows copied in bulk. Byte offsets that survive
     * the cheap numeric filter in IsPlausibleGObjects() are then validated for
     * real, which is the only place that pays for a Mach trap per field. */
    bool bStopScan = false;
    const uintptr SearchEnd = SearchBase + SearchRange;
    std::vector<uint8> ScanWindow(ScanWindowSize + ScanWindowSlack);

    for (uintptr WindowBase = SearchBase; WindowBase < SearchEnd && !bStopScan; )
    {
        const uintptr Remaining = SearchEnd - WindowBase;
        const size_t Want = static_cast<size_t>(std::min<uintptr>(static_cast<uintptr>(ScanWindowSize + ScanWindowSlack), Remaining));
        const size_t Got = static_cast<size_t>(ReadMemoryBulk(WindowBase, Want, ScanWindow.data()));

        if (Got < ScanWindowSlack)
        {
            /* Unmapped page inside the range: step one page and keep going. */
            WindowBase += 0x4000;
            continue;
        }

        const size_t CandidateEnd = Got - ScanWindowSlack;

        for (size_t Offset = 0; Offset < CandidateEnd && !bStopScan; Offset += 0x4)
        {
            const uintptr CurrentAddress = WindowBase + Offset;

            /* Progress marker: tells us after the fact whether the scan was running
             * (and how far it got) or whether it died on a candidate. */
            if (((CurrentAddress - SearchBase) & 0x3FFFFFF) == 0)
                DiagSetStage("ObjectArray::Init: scanning for GObjects, 0x%lX / 0x%lX", (unsigned long)(CurrentAddress - SearchBase), (unsigned long)SearchRange);

            /* Cheap filter on bytes we already have in memory: no Mach trap. */
            if (!IsPlausibleGObjects(ScanWindow.data() + Offset, Got - Offset))
                continue;

            if (MatchesAnyLayout(FFixedUObjectArrayLayouts, CurrentAddress))
            {
                GObjects = reinterpret_cast<uint8*>(CurrentAddress);
                NumElementsPerChunk = -1;

                Off::InSDK::ObjArray::GObjects = CurrentAddress - ImageBase;

                LogSuccess("Found FFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

                ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
                {
                    if (Index < 0 || Index > Num())
                        return nullptr;

                    uint8* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray));

                    return *reinterpret_cast<void**>(ChunkPtr + FUObjectItemOffset + (Index * FUObjectItemSize));
                };

                uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

                if (!ChunksPtr || IsBadReadPtr(ChunksPtr))
                {
                    LogError("ObjectArray::Init: FFixedUObjectArray candidate has an unreadable item array (0x%llX)",
                             (unsigned long long)(uintptr_t)ChunksPtr);
                    DiagMarkFatal("GObjects item array is unreadable");
                    GObjects = nullptr;
                    bStopScan = true;
                    break;
                }

                ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));

                bStopScan = true;
                break;
            }
            else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, CurrentAddress))
            {
                GObjects = reinterpret_cast<uint8*>(CurrentAddress);
                NumElementsPerChunk = 0x10000;
                SizeOfFUObjectItem = 0x18;
                FUObjectItemInitialOffset = 0x0;

                Off::InSDK::ObjArray::GObjects = CurrentAddress - ImageBase;

                LogSuccess("Found FChunkedFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

                ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
                {
                    if (Index < 0 || Index > Num())
                        return nullptr;

                    const int32 ChunkIndex = Index / PerChunk;
                    const int32 InChunkIdx = Index % PerChunk;

                    uint8* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray));

                    uint8* Chunk = reinterpret_cast<uint8**>(ChunkPtr)[ChunkIndex];
                    uint8* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize);

                    return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
                };

                uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

                if (!ChunksPtr || IsBadReadPtr(ChunksPtr))
                {
                    LogError("ObjectArray::Init: FChunkedFixedUObjectArray candidate has an unreadable chunk table (0x%llX)",
                             (unsigned long long)(uintptr_t)ChunksPtr);
                    DiagMarkFatal("GObjects chunk table is unreadable");
                    GObjects = nullptr;
                    bStopScan = true;
                    break;
                }

                ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));

                ObjectArray::InitializeChunkSize(GObjects + Off::FUObjectArray::GetObjectsOffset());

                bStopScan = true;
                break;
            }
        }

        /* Advance one window. The slack read above overlaps into the next
         * window, so no offset between two windows is skipped. If the kernel
         * stopped early at an unmapped page, only advance over what it copied. */
        if (Got < Want)
            WindowBase += (Got & ~static_cast<size_t>(0x3));
        else
            WindowBase += ScanWindowSize;
    }

    /* A candidate either matched (and the globals are set) or was rejected as
     * unusable; in both cases there is nothing left to look for. */
    if (bStopScan)
        return;

    if (!bScanAllMemory)
    {
        LogInfo("Retrying with full memory scan...");
        ObjectArray::Init(true);
        return;
    }

    if (GObjects == nullptr)
    {
        /* Killing the host game with exit(1) here looks exactly like a crash to
         * the user. Report the failure and let the caller bail out cleanly. */
        LogError("GObjects couldn't be found!");
        DiagMarkFatal("GObjects couldn't be found (full memory scan exhausted)");
    }
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	LogInfo("Initializing ObjectArray with FFixedUObjectArray at offset 0x%X", GObjectsOffset);
	
	GObjects = reinterpret_cast<uint8*>(GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	LogInfo("GObjects: 0x%p", (void*)GObjects);

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index > Num())
			return nullptr;

		uint8* ItemPtr = *reinterpret_cast<uint8**>(ObjectsArray) + (Index * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));
	LogSuccess("FFixedUObjectArray initialized successfully");
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	LogInfo("Initializing ObjectArray with FChunkedFixedUObjectArray at offset 0x%X", GObjectsOffset);
	GObjects = reinterpret_cast<uint8*>(GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = ElementsPerChunk;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index > Num())
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		uint8* Chunk = (*reinterpret_cast<uint8***>(ObjectsArray))[ChunkIndex];
		uint8* ItemPtr = reinterpret_cast<uint8*>(Chunk) + (InChunkIdx * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));
	LogSuccess("FChunkedFixedUObjectArray initialized successfully");
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	LogInfo("Dumping objects to %s...", (Path / "GObjects-Dump.txt").string().c_str());
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
        
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}
	}

	DumpStream.close();
	LogSuccess("Objects dumped successfully to %s", (Path / "GObjects-Dump.txt").string().c_str());
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	LogInfo("Dumping objects with properties to %s...", (Path / "GObjects-Dump-WithProperties.txt").string().c_str());
	
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				DumpStream << std::format("[{:08X}] {{{}}}\t{} {}\n", Prop.GetOffset(), Prop.GetAddress(), Prop.GetPropClassName(), Prop.GetName());
			}
		}
	}

	DumpStream.close();
	LogSuccess("Objects with properties dumped successfully to %s", (Path / "GObjects-Dump-WithProperties.txt").string().c_str());
}


int32 ObjectArray::Num()
{
	/* Num() is called from ByIndex() on every access, so only the null check
	 * here - a bad GObjects candidate would fault on the first read. */
	if (!GObjects)
		return 0;

	const int32 NumElements = *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
	return NumElements > 0 ? NumElements : 0;
}

template<typename UEType>
UEType ObjectArray::GetByIndex(int32 Index)
{
	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
        
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	auto ObjArray = ObjectArray();

	const int32 Total = ObjectArray::Num();
	int32 i = 0;

	for (UEObject Object : ObjArray)
	{
		// Progress heartbeat so a slow / stuck scan is visible in the console
		if ((i & 0xFFFF) == 0 && i > 0)
			LogInfo("FindObjectFast(\"%s\"): scanned %d / %d", Name.c_str(), i, Total);
		i++;

		// Skip objects whose UObject* pointer or class read would fault
		const void* Addr = Object.GetAddress();
		if (!Addr || IsBadReadPtr(Addr))
			continue;

		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			LogSuccess("FindObjectFast(\"%s\"): found at index %d", Name.c_str(), i - 1);
			return Object.Cast<UEType>();
		}
	}

	LogError("FindObjectFast(\"%s\"): not found after scanning %d objects", Name.c_str(), Total);
	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
}

UEObject ObjectArray::ObjectsIterator::operator*()
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
[[maybe_unused]] void TemplateTypeCreationForObjectArray(void)
{
	ObjectArray::FindObject<UEObject>("");
	ObjectArray::FindObject<UEField>("");
	ObjectArray::FindObject<UEEnum>("");
	ObjectArray::FindObject<UEStruct>("");
	ObjectArray::FindObject<UEClass>("");
	ObjectArray::FindObject<UEFunction>("");
	ObjectArray::FindObject<UEProperty>("");
	ObjectArray::FindObject<UEByteProperty>("");
	ObjectArray::FindObject<UEBoolProperty>("");
	ObjectArray::FindObject<UEObjectProperty>("");
	ObjectArray::FindObject<UEClassProperty>("");
	ObjectArray::FindObject<UEStructProperty>("");
	ObjectArray::FindObject<UEArrayProperty>("");
	ObjectArray::FindObject<UEMapProperty>("");
	ObjectArray::FindObject<UESetProperty>("");
	ObjectArray::FindObject<UEEnumProperty>("");

	ObjectArray::FindObjectFast<UEObject>("");
	ObjectArray::FindObjectFast<UEField>("");
	ObjectArray::FindObjectFast<UEEnum>("");
	ObjectArray::FindObjectFast<UEStruct>("");
	ObjectArray::FindObjectFast<UEClass>("");
	ObjectArray::FindObjectFast<UEFunction>("");
	ObjectArray::FindObjectFast<UEProperty>("");
	ObjectArray::FindObjectFast<UEByteProperty>("");
	ObjectArray::FindObjectFast<UEBoolProperty>("");
	ObjectArray::FindObjectFast<UEObjectProperty>("");
	ObjectArray::FindObjectFast<UEClassProperty>("");
	ObjectArray::FindObjectFast<UEStructProperty>("");
	ObjectArray::FindObjectFast<UEArrayProperty>("");
	ObjectArray::FindObjectFast<UEMapProperty>("");
	ObjectArray::FindObjectFast<UESetProperty>("");
	ObjectArray::FindObjectFast<UEEnumProperty>("");

	ObjectArray::FindObjectFastInOuter<UEObject>("", "");
	ObjectArray::FindObjectFastInOuter<UEField>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnum>("", "");
	ObjectArray::FindObjectFastInOuter<UEStruct>("", "");
	ObjectArray::FindObjectFastInOuter<UEClass>("", "");
	ObjectArray::FindObjectFastInOuter<UEFunction>("", "");
	ObjectArray::FindObjectFastInOuter<UEProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEByteProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEBoolProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEObjectProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEClassProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEStructProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEArrayProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEMapProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UESetProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnumProperty>("", "");

	ObjectArray::GetByIndex<UEObject>(-1);
	ObjectArray::GetByIndex<UEField>(-1);
	ObjectArray::GetByIndex<UEEnum>(-1);
	ObjectArray::GetByIndex<UEStruct>(-1);
	ObjectArray::GetByIndex<UEClass>(-1);
	ObjectArray::GetByIndex<UEFunction>(-1);
	ObjectArray::GetByIndex<UEProperty>(-1);
	ObjectArray::GetByIndex<UEByteProperty>(-1);
	ObjectArray::GetByIndex<UEBoolProperty>(-1);
	ObjectArray::GetByIndex<UEObjectProperty>(-1);
	ObjectArray::GetByIndex<UEClassProperty>(-1);
	ObjectArray::GetByIndex<UEStructProperty>(-1);
	ObjectArray::GetByIndex<UEArrayProperty>(-1);
	ObjectArray::GetByIndex<UEMapProperty>(-1);
	ObjectArray::GetByIndex<UESetProperty>(-1);
	ObjectArray::GetByIndex<UEEnumProperty>(-1);
}


bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
    return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
    if (CurrenStructHasMoreMembers())
    {
        PropertyIndex++;
        return *this;
    }
    IterateToNextStructWithMembers();
    return *this;
}

UEProperty AllFieldIterator::operator*() const
{
    return Fields[PropertyIndex];
}

void AllFieldIterator::IterateToNextStruct()
{
    if (IsEndIterator()) return;
    ++CurrentObject;
    while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
        ++CurrentObject;
}

void AllFieldIterator::IterateToNextStructWithMembers()
{
    while (!CurrenStructHasMoreMembers())
    {
        IterateToNextStruct();
        PropertyIndex = 0;
        if (IsEndIterator()) return;
        Fields = GetCurrentStruct().GetProperties();
    }
}


/*
 * Explicit instantiation definitions.
 *
 * TemplateTypeCreationForObjectArray() above only *implicitly* instantiates these member
 * function templates, and clang emits implicit instantiations as weak (linkonce_odr)
 * symbols that an optimized Release build can drop. When another translation unit
 * references them, the linker then reports:
 *
 *   ld: Undefined symbols for architecture arm64:
 *     "UEObject ObjectArray::GetByIndex<UEObject>(int)", referenced from: ...
 *
 * Spelling the instantiations out explicitly forces the compiler to emit real
 * definitions in this object file, so the symbols always exist at link time.
 * Keep this list in sync with TemplateTypeCreationForObjectArray().
 */

template UEObject ObjectArray::GetByIndex<UEObject>(int32);
template UEField ObjectArray::GetByIndex<UEField>(int32);
template UEEnum ObjectArray::GetByIndex<UEEnum>(int32);
template UEStruct ObjectArray::GetByIndex<UEStruct>(int32);
template UEClass ObjectArray::GetByIndex<UEClass>(int32);
template UEFunction ObjectArray::GetByIndex<UEFunction>(int32);
template UEProperty ObjectArray::GetByIndex<UEProperty>(int32);
template UEByteProperty ObjectArray::GetByIndex<UEByteProperty>(int32);
template UEBoolProperty ObjectArray::GetByIndex<UEBoolProperty>(int32);
template UEObjectProperty ObjectArray::GetByIndex<UEObjectProperty>(int32);
template UEClassProperty ObjectArray::GetByIndex<UEClassProperty>(int32);
template UEStructProperty ObjectArray::GetByIndex<UEStructProperty>(int32);
template UEArrayProperty ObjectArray::GetByIndex<UEArrayProperty>(int32);
template UEMapProperty ObjectArray::GetByIndex<UEMapProperty>(int32);
template UESetProperty ObjectArray::GetByIndex<UESetProperty>(int32);
template UEEnumProperty ObjectArray::GetByIndex<UEEnumProperty>(int32);

template UEObject ObjectArray::FindObject<UEObject>(const std::string&, EClassCastFlags);
template UEField ObjectArray::FindObject<UEField>(const std::string&, EClassCastFlags);
template UEEnum ObjectArray::FindObject<UEEnum>(const std::string&, EClassCastFlags);
template UEStruct ObjectArray::FindObject<UEStruct>(const std::string&, EClassCastFlags);
template UEClass ObjectArray::FindObject<UEClass>(const std::string&, EClassCastFlags);
template UEFunction ObjectArray::FindObject<UEFunction>(const std::string&, EClassCastFlags);
template UEProperty ObjectArray::FindObject<UEProperty>(const std::string&, EClassCastFlags);
template UEByteProperty ObjectArray::FindObject<UEByteProperty>(const std::string&, EClassCastFlags);
template UEBoolProperty ObjectArray::FindObject<UEBoolProperty>(const std::string&, EClassCastFlags);
template UEObjectProperty ObjectArray::FindObject<UEObjectProperty>(const std::string&, EClassCastFlags);
template UEClassProperty ObjectArray::FindObject<UEClassProperty>(const std::string&, EClassCastFlags);
template UEStructProperty ObjectArray::FindObject<UEStructProperty>(const std::string&, EClassCastFlags);
template UEArrayProperty ObjectArray::FindObject<UEArrayProperty>(const std::string&, EClassCastFlags);
template UEMapProperty ObjectArray::FindObject<UEMapProperty>(const std::string&, EClassCastFlags);
template UESetProperty ObjectArray::FindObject<UESetProperty>(const std::string&, EClassCastFlags);
template UEEnumProperty ObjectArray::FindObject<UEEnumProperty>(const std::string&, EClassCastFlags);

template UEObject ObjectArray::FindObjectFast<UEObject>(const std::string&, EClassCastFlags);
template UEField ObjectArray::FindObjectFast<UEField>(const std::string&, EClassCastFlags);
template UEEnum ObjectArray::FindObjectFast<UEEnum>(const std::string&, EClassCastFlags);
template UEStruct ObjectArray::FindObjectFast<UEStruct>(const std::string&, EClassCastFlags);
template UEClass ObjectArray::FindObjectFast<UEClass>(const std::string&, EClassCastFlags);
template UEFunction ObjectArray::FindObjectFast<UEFunction>(const std::string&, EClassCastFlags);
template UEProperty ObjectArray::FindObjectFast<UEProperty>(const std::string&, EClassCastFlags);
template UEByteProperty ObjectArray::FindObjectFast<UEByteProperty>(const std::string&, EClassCastFlags);
template UEBoolProperty ObjectArray::FindObjectFast<UEBoolProperty>(const std::string&, EClassCastFlags);
template UEObjectProperty ObjectArray::FindObjectFast<UEObjectProperty>(const std::string&, EClassCastFlags);
template UEClassProperty ObjectArray::FindObjectFast<UEClassProperty>(const std::string&, EClassCastFlags);
template UEStructProperty ObjectArray::FindObjectFast<UEStructProperty>(const std::string&, EClassCastFlags);
template UEArrayProperty ObjectArray::FindObjectFast<UEArrayProperty>(const std::string&, EClassCastFlags);
template UEMapProperty ObjectArray::FindObjectFast<UEMapProperty>(const std::string&, EClassCastFlags);
template UESetProperty ObjectArray::FindObjectFast<UESetProperty>(const std::string&, EClassCastFlags);
template UEEnumProperty ObjectArray::FindObjectFast<UEEnumProperty>(const std::string&, EClassCastFlags);

template UEObject ObjectArray::FindObjectFastInOuter<UEObject>(const std::string&, std::string);
template UEField ObjectArray::FindObjectFastInOuter<UEField>(const std::string&, std::string);
template UEEnum ObjectArray::FindObjectFastInOuter<UEEnum>(const std::string&, std::string);
template UEStruct ObjectArray::FindObjectFastInOuter<UEStruct>(const std::string&, std::string);
template UEClass ObjectArray::FindObjectFastInOuter<UEClass>(const std::string&, std::string);
template UEFunction ObjectArray::FindObjectFastInOuter<UEFunction>(const std::string&, std::string);
template UEProperty ObjectArray::FindObjectFastInOuter<UEProperty>(const std::string&, std::string);
template UEByteProperty ObjectArray::FindObjectFastInOuter<UEByteProperty>(const std::string&, std::string);
template UEBoolProperty ObjectArray::FindObjectFastInOuter<UEBoolProperty>(const std::string&, std::string);
template UEObjectProperty ObjectArray::FindObjectFastInOuter<UEObjectProperty>(const std::string&, std::string);
template UEClassProperty ObjectArray::FindObjectFastInOuter<UEClassProperty>(const std::string&, std::string);
template UEStructProperty ObjectArray::FindObjectFastInOuter<UEStructProperty>(const std::string&, std::string);
template UEArrayProperty ObjectArray::FindObjectFastInOuter<UEArrayProperty>(const std::string&, std::string);
template UEMapProperty ObjectArray::FindObjectFastInOuter<UEMapProperty>(const std::string&, std::string);
template UESetProperty ObjectArray::FindObjectFastInOuter<UESetProperty>(const std::string&, std::string);
template UEEnumProperty ObjectArray::FindObjectFastInOuter<UEEnumProperty>(const std::string&, std::string);
