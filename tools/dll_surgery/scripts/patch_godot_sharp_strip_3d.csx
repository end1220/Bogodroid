#!/usr/bin/env dotnet-script
/* Surgical IL strip of 3D-specific method-bind lookups in GodotSharp.dll, so a
 * matching godot runtime built with disable_3d=yes doesn't fall over on the
 * (missing) 3D natives while still leaving every 2D method bind intact.
 *
 * The godot mono binding generator emits a static constructor on every class
 * containing one self-contained "method bind lookup" block per native method:
 *
 *     ldsfld <Class>::NativeName                   StringName
 *     ldsfld <Class>/MethodName::<Method>          StringName, e.g. SetWorld3D
 *     ldc.i4 <hash>                                int
 *     conv.u8 / conv.i8                            long
 *     call IntPtr GodotObject::ClassDB_get_method_with_compatibility(
 *              StringName, StringName, ulong)
 *     stsfld <Class>::MethodBindN                  IntPtr
 *
 * Stock godot would push two StringName references, the hash, and call the
 * native bridge — which on a disable_3d runtime returns an empty IntPtr and
 * the C# class is then half-baked from that point on. A previous version of
 * this patcher wrapped the whole cctor in try/catch — that broke far more
 * than it fixed because the 2D method binds (canvas modulate, glow shader,
 * particle texture) live in the same cctor and were stranded with the 3D
 * ones. The runtime cost was a black-rendering main menu where Tween
 * animations stopped and only static labels survived.
 *
 * This pass replaces each 3D-specific 6-instruction block with six nops, one
 * block at a time. The 2D blocks above and below stay intact and run as
 * before. The 3D MethodBind static fields stay at their default IntPtr.Zero,
 * which is what godot uses to mean "no bind" — every consumer of the bind
 * already null-checks (godot tests it before reaching native code).
 *
 * Two additional minor surgeries kept from previous versions:
 *   * Stub out the error-logging chain (GD.PushError / ErrPrintError /
 *     ExceptionUtils.LogException) so any *runtime* error from a missing
 *     3D class can't blow the call stack via the recursive
 *     ProjectSettings.LocalizePath → get_Singleton() → log → loop. We
 *     never reach those code paths in a clean run, but a single un-found
 *     bind on an obscure code path used to cascade into a SIGABRT before
 *     this stub landed.
 *   * Nothing else. No cctor wrapping. No type-level interventions.
 *
 * Usage:
 *   dotnet-script patch_godot_sharp_strip_3d.csx -- <in.dll> <out.dll>
 */
#r "nuget: Mono.Cecil, 0.11.5"

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (Args.Count < 2) {
    Console.Error.WriteLine("usage: patch_godot_sharp_strip_3d.csx -- <in.dll> <out.dll>");
    Environment.Exit(2);
}

string inputPath = Args[0];
string outputPath = Args[1];

var asmRes = new DefaultAssemblyResolver();
asmRes.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(inputPath)));
var asm = AssemblyDefinition.ReadAssembly(inputPath,
    new ReaderParameters { ReadWrite = false, AssemblyResolver = asmRes });

// Heuristic: "this field name targets a 3D-only native". Match the StringName
// fields the binding generator emits onto nested MethodName / PropertyName /
// SignalName classes — those are how every 3D method/property/signal is
// referenced from the cctor block we want to NOP.
//
// Naming we deliberately catch:
//   *3D suffix (Camera3D, World3D, Mesh3D…)
//   *3d suffix (set_world_3d, get_camera_3d…) — godot's snake_case native names
//   embedded names: Camera3D, World3D, Mesh3D
static bool IsFieldName3D(string name) {
    if (string.IsNullOrEmpty(name)) return false;
    if (name.EndsWith("3D") || name.EndsWith("3d")) return true;
    if (name.Contains("World3D") || name.Contains("Camera3D") || name.Contains("Mesh3D")) return true;
    if (name.Contains("Light3D") || name.Contains("Node3D")) return true;
    return false;
}

// Pure-3D types whose entire cctor we still NOP wholesale — they have no 2D
// content to preserve. (Cheaper than per-block surgery on hundreds of 3D-only
// fields.) Detect by name + by stock-godot 3D namespace patterns.
static bool IsPure3DType(TypeDefinition t) {
    string n = t.Name;
    if (n.EndsWith("3D")) return true;
    // Common bare-name 3D types that don't end in "3D"
    string[] roots = {
        "Camera3D", "Mesh", "ArrayMesh", "PrimitiveMesh", "ImmediateMesh", "PlaceholderMesh",
        "Skin", "Skeleton3D",
        "VoxelGI", "VoxelGIData",
        "FogVolume", "FogMaterial",
        "WorldEnvironment", "World3D",
        "BoneMap",
        "Light3D", "DirectionalLight3D", "OmniLight3D", "SpotLight3D",
        "NavigationServer3D",
    };
    if (roots.Contains(n)) return true;
    return false;
}

// ─── Helpers ──────────────────────────────────────────────────────────────

static MethodDefinition GetCctor(TypeDefinition t)
    => t.Methods.FirstOrDefault(m => m.IsConstructor && m.IsStatic);

static int IndexOfInstruction(MethodBody body, Instruction ins) {
    for (int i = 0; i < body.Instructions.Count; i++)
        if (body.Instructions[i] == ins) return i;
    return -1;
}

// Convert the whole method body to "nop * N; ret" — used for the pure-3D
// cctors and for the GD/ExceptionUtils error-logging stubs.
static void StubMethodBody(MethodDefinition m) {
    if (m.Body == null) return;
    m.Body.Instructions.Clear();
    m.Body.ExceptionHandlers.Clear();
    m.Body.Variables.Clear();
    var il = m.Body.GetILProcessor();
    if (m.ReturnType.FullName != "System.Void") return; // would need a default-value return
    il.Append(Instruction.Create(OpCodes.Ret));
}

// ─── Pass 1: NOP 3D method-bind blocks in mixed cctors ────────────────────

// "Mixed" type: namespace Godot, not pure-3D, but its cctor still references
// 3D methods (Viewport.SetWorld3D, SceneTree.set_current_scene_3d, …). We do
// surgical block-NOPs instead of a full cctor wrap.
int blocksNopped = 0;
int mixedTypesPatched = 0;
var mixedReport = new List<string>();

foreach (var type in asm.MainModule.GetTypes()) {
    string ns = type.Namespace ?? "";
    if (!(ns == "Godot" || ns.StartsWith("Godot."))) continue;
    if (ns.StartsWith("Godot.NativeInterop")) continue;
    if (ns.StartsWith("Godot.Bridge")) continue;
    if (ns.StartsWith("Godot.SourceGenerators")) continue;
    if (ns.StartsWith("Godot.Collections")) continue;
    if (IsPure3DType(type)) continue;          // handled in Pass 2 wholesale

    var cctor = GetCctor(type);
    if (cctor == null || cctor.Body == null) continue;

    // Locate every "ldsfld <3D MethodName field>" in this cctor — each one
    // starts a 6-instruction lookup block we want to NOP.
    var blockStartIndexes = new List<int>();
    for (int i = 0; i < cctor.Body.Instructions.Count; i++) {
        var ins = cctor.Body.Instructions[i];
        if (ins.OpCode != OpCodes.Ldsfld) continue;
        var fr = ins.Operand as FieldReference;
        if (fr == null) continue;
        if (!IsFieldName3D(fr.Name)) continue;
        // The block's first instruction is the *preceding* ldsfld NativeName.
        if (i == 0) continue;
        var prev = cctor.Body.Instructions[i - 1];
        if (prev.OpCode != OpCodes.Ldsfld) continue;
        blockStartIndexes.Add(i - 1);
    }

    if (blockStartIndexes.Count == 0) continue;

    // NOP each block. Process in reverse so we can swap instructions in
    // place without disturbing the indices of earlier blocks.
    var il = cctor.Body.GetILProcessor();
    blockStartIndexes.Reverse();
    int localCount = 0;
    foreach (var startIdx in blockStartIndexes) {
        // The block ends at the first stsfld at or after startIdx + 5
        // (typical block: ldsfld, ldsfld, ldc, conv, call, stsfld). Walk
        // forward to find that stsfld within a small window.
        int endIdx = -1;
        for (int i = startIdx + 1; i < Math.Min(startIdx + 12, cctor.Body.Instructions.Count); i++) {
            if (cctor.Body.Instructions[i].OpCode == OpCodes.Stsfld) {
                endIdx = i;
                break;
            }
        }
        if (endIdx == -1) continue;

        for (int i = startIdx; i <= endIdx; i++) {
            il.Replace(cctor.Body.Instructions[i], Instruction.Create(OpCodes.Nop));
        }
        blocksNopped++;
        localCount++;
    }

    if (localCount > 0) {
        mixedTypesPatched++;
        mixedReport.Add($"{type.FullName}: {localCount} 3D block(s) nopped");
    }
}

// ─── Pass 2: wholesale-stub pure-3D types' cctors ─────────────────────────

int pure3DTypesStubbed = 0;
foreach (var type in asm.MainModule.GetTypes()) {
    string ns = type.Namespace ?? "";
    if (!(ns == "Godot" || ns.StartsWith("Godot."))) continue;
    if (!IsPure3DType(type)) continue;
    var cctor = GetCctor(type);
    if (cctor == null) continue;
    StubMethodBody(cctor);
    pure3DTypesStubbed++;
}

// ─── Pass 3: stub error-logging chain ─────────────────────────────────────

int methodsStubbed = 0;
var methodsStubbedNames = new List<string>();
string[] stubTargets = {
    "Godot.NativeInterop.ExceptionUtils::LogException",
    "Godot.GD::PushError",
    "Godot.GD::PushWarning",
    "Godot.GD::PrintErr",
    "Godot.GD::ErrPrintError",
};
foreach (var type in asm.MainModule.GetTypes()) {
    foreach (var m in type.Methods) {
        string key = type.FullName + "::" + m.Name;
        if (stubTargets.Any(t => key == t || key.StartsWith(t))) {
            StubMethodBody(m);
            methodsStubbed++;
            methodsStubbedNames.Add($"{key}({string.Join(",", m.Parameters.Select(p => p.ParameterType.Name))})");
        }
    }
}

asm.Write(outputPath);

Console.WriteLine($"wrote: {outputPath}");
Console.WriteLine($"  3D method-bind blocks NOPed (in mixed-type cctors): {blocksNopped} across {mixedTypesPatched} types");
Console.WriteLine($"  pure-3D type cctors stubbed wholesale:               {pure3DTypesStubbed}");
Console.WriteLine($"  error-logging methods stubbed:                       {methodsStubbed}");
Console.WriteLine();
Console.WriteLine("mixed-type breakdown (preserved 2D cctor body, only nopped 3D blocks):");
foreach (var r in mixedReport.Take(15)) Console.WriteLine($"    {r}");
if (mixedReport.Count > 15) Console.WriteLine($"    ... and {mixedReport.Count - 15} more types");
Console.WriteLine();
Console.WriteLine("stubbed error-loggers:");
foreach (var n in methodsStubbedNames) Console.WriteLine($"    {n}");
