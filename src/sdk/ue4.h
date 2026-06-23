#pragma once
#include <cstdint>
#include <string>
#include "offsets.h"

// ===========================================================================
//  Minimal Unreal Engine 4 runtime SDK.
//  Just enough of the engine's reflection system to:
//    - resolve GObjects / GNames / GWorld
//    - look objects up by name / class
//    - call ANY UFunction via ProcessEvent  ->  this is "full control"
//  For typed, comfortable access to AtomicHeart's own classes, generate a
//  Dumper-7 SDK and include it alongside this file.
// ===========================================================================

namespace UE
{
    // ---- Math ---------------------------------------------------------------
    struct FVector  { float X, Y, Z; };
    struct FVector2D{ float X, Y; };
    struct FRotator { float Pitch, Yaw, Roll; };

    template <typename T>
    struct TArray
    {
        T*      Data;
        int32_t Count;
        int32_t Max;
        int32_t Num() const { return Count; }
        T&  operator[](int i) { return Data[i]; }
        bool IsValid(int i) const { return Data && i >= 0 && i < Count; }
    };

    struct FString : TArray<wchar_t>
    {
        std::string ToString() const;
    };

    // ---- FName --------------------------------------------------------------
    struct FName
    {
        int32_t ComparisonIndex;
        int32_t Number;
        std::string ToString() const;
    };

    // ---- UObject and friends (opaque; we index by byte offset) --------------
    struct UObject
    {
        // helpers operate via raw offsets from Offsets:: so we never assume a
        // compiler-generated layout.
        void**      VTable()  { return *reinterpret_cast<void***>(this); }
        int32_t     Index()   { return *reinterpret_cast<int32_t*>((uint8_t*)this + Offsets::O_UObject_InternalIndex); }
        UObject*    Class()   { return *reinterpret_cast<UObject**>((uint8_t*)this + Offsets::O_UObject_Class); }
        FName*      NamePtr() { return  reinterpret_cast<FName*>((uint8_t*)this + Offsets::O_UObject_Name); }
        UObject*    Outer()   { return *reinterpret_cast<UObject**>((uint8_t*)this + Offsets::O_UObject_Outer); }

        std::string GetName();
        std::string GetFullName();           // "ClassName Outer.Object"
        bool        IsA(UObject* cmpClass);  // walks SuperStruct chain

        // The universal call gate. params must match the UFunction signature.
        void        ProcessEvent(UObject* function, void* params);
    };

    using UClass    = UObject;
    using UFunction = UObject;

    // ---- Engine globals -----------------------------------------------------
    bool      ResolveGlobals();      // populate GObjects/GNames/GWorld; sets G::sdkReady
    UObject*  GetWorld();            // *GWorld

    // Object lookup over GObjects.
    int       NumObjects();
    UObject*  GetObjectByIndex(int index);
    UObject*  FindObject(const char* name);           // substring of full name
    UObject*  FindObjectFast(const char* name);       // dumped index/name-index only; never scans
    void      StartObjectNameIndex();                 // background short-name index for fast assets
    bool      WriteObjectMapDump(const char* reason); // writes AtomicHeartMenu_gobjects.tsv beside the game exe
    UClass*   FindClass(const char* className);       // "Class Package.Name" or short
    UFunction*FindFunction(const char* funcFullName); // full path of a UFunction
    // Walk a class's Children (UField) list for a UFunction by its SHORT name.
    // No GObjects scan and no dependence on the owner's package path -- the right
    // way to resolve functions on BP content classes whose runtime full name is
    // "/Game/.../Pkg.Class.Fn". Returns the function defined on this class.
    UFunction*FindFunctionInClass(UClass* cls, const char* shortName);

    // Resolve a runtime FName for an exact short string (e.g. a material parameter
    // name like "BaseColor"). Prefer the background UObject name index, then fall
    // back to indexing the live FNamePool so material-only parameter names can be
    // resolved too. The result is a Number==0 FName.
    bool      TryGetFName(const char* shortName, FName& out);

    // Convenience traversal to the local player chain (engine-generic).
    //   World -> OwningGameInstance -> LocalPlayers[0] -> PlayerController -> Pawn
    UObject*  GetGameInstance();
    UObject*  GetLocalPlayer();
    UObject*  GetPlayerController();
    UObject*  GetLocalPawn();
}
