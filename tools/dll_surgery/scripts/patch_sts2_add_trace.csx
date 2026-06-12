#!/usr/bin/env dotnet-script
/* Inject Console.WriteLine trace breadcrumbs into a curated set of StS2
 * startup methods so we can see, from the device's log.txt, exactly where
 * the lite/no_3d build hangs.
 *
 * For each method on the list we:
 *   1. Prepend `Console.WriteLine("[BOGOTRACE] <FullName> ENTER")` at the
 *      first instruction of the user-written method body.
 *   2. If the method is `async` (i.e. the compiler stamped it with
 *      [AsyncStateMachineAttribute(<StateMachineType>)]), we *also* walk
 *      into <StateMachineType>.MoveNext() and prepend a similar trace
 *      there — the wrapper just returns the Task, the real work happens
 *      inside MoveNext.
 *
 * Why not use Godot.GD.Print? The error-logging stubs in
 * patch_godot_sharp_strip_3d.csx neuter GD.PushError/PushWarning/PrintErr —
 * Console.WriteLine bypasses them and reaches stdout, which the launcher
 * captures into log.txt via `exec > >(tee log.txt) 2>&1`.
 *
 * Usage:
 *   dotnet-script patch_sts2_add_trace.csx -- <in.dll> <out.dll> <search-root>
 *
 * The <search-root> is the path to the directory holding the .NET
 * references needed for the assembly (data_sts2_linuxbsd_arm64/) so Cecil
 * can resolve System.Console etc.
 */
#r "nuget: Mono.Cecil, 0.11.5"

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (Args.Count < 3) {
    Console.Error.WriteLine("usage: patch_sts2_add_trace.csx -- <in.dll> <out.dll> <ref-dir>");
    Environment.Exit(2);
}

string inputPath = Args[0];
string outputPath = Args[1];
string refDir = Args[2];

var asmRes = new DefaultAssemblyResolver();
asmRes.AddSearchDirectory(refDir);
asmRes.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(inputPath)));
var asm = AssemblyDefinition.ReadAssembly(inputPath,
    new ReaderParameters { ReadWrite = false, AssemblyResolver = asmRes });

// The list of methods we want trace on. Match by "Type.Method" — Cecil's
// FullName format. Add liberally; missing matches are silently skipped.
var targets = new HashSet<string>(StringComparer.Ordinal) {
    "MegaCrit.Sts2.Core.Nodes.NGame::_EnterTree",
    "MegaCrit.Sts2.Core.Nodes.NGame::_Ready",
    "MegaCrit.Sts2.Core.Nodes.NGame::GameStartupWrapper",
    "MegaCrit.Sts2.Core.Nodes.NGame::GameStartup",
    "MegaCrit.Sts2.Core.Nodes.NGame::TryErrorInit",
    "MegaCrit.Sts2.Core.Nodes.NGame::LaunchMainMenu",
    "MegaCrit.Sts2.Core.Nodes.NGame::LoadMainMenu",
    "MegaCrit.Sts2.Core.Nodes.NGame::LoadDeferredStartupAssetsAsync",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadLogoAnimation",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadMainMenuEssentials",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadCommonAndMainMenuAssets",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadMainMenuAssets",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadAssetSets",
};

// Resolve Console.WriteLine(string)
var consoleType = asm.MainModule.ImportReference(typeof(Console));
TypeDefinition consoleDef = null;
try { consoleDef = consoleType.Resolve(); } catch { }
MethodReference writeLineRef = null;
if (consoleDef != null) {
    var writeLine = consoleDef.Methods.FirstOrDefault(m =>
        m.Name == "WriteLine" && m.IsStatic &&
        m.Parameters.Count == 1 &&
        m.Parameters[0].ParameterType.FullName == "System.String");
    if (writeLine != null) writeLineRef = asm.MainModule.ImportReference(writeLine);
}
if (writeLineRef == null) {
    // Fallback: build the reference by hand without resolving.
    var stringType = asm.MainModule.TypeSystem.String;
    var voidType = asm.MainModule.TypeSystem.Void;
    var sysConsoleRef = new TypeReference("System", "Console",
        asm.MainModule, asm.MainModule.TypeSystem.CoreLibrary, false);
    var mr = new MethodReference("WriteLine", voidType, sysConsoleRef) {
        HasThis = false,
    };
    mr.Parameters.Add(new ParameterDefinition(stringType));
    writeLineRef = asm.MainModule.ImportReference(mr);
}

static void PrependTrace(MethodDefinition m, string label, MethodReference writeLineRef) {
    if (m.Body == null) return;
    if (m.Body.Instructions.Count == 0) return;
    var il = m.Body.GetILProcessor();
    var first = m.Body.Instructions[0];
    il.InsertBefore(first, Instruction.Create(OpCodes.Ldstr, $"[BOGOTRACE] {label}"));
    il.InsertBefore(first, Instruction.Create(OpCodes.Call, writeLineRef));
}

int patched = 0;
int asyncPatched = 0;
var report = new List<string>();

foreach (var type in asm.MainModule.GetTypes()) {
    foreach (var m in type.Methods) {
        string key = type.FullName + "::" + m.Name;
        if (!targets.Contains(key)) continue;

        PrependTrace(m, key + " ENTER", writeLineRef);
        patched++;
        report.Add($"  wrapper: {key}");

        // If async — find the compiler-generated state machine and inject
        // trace into its MoveNext so we see the actual execution.
        var asyncAttr = m.CustomAttributes.FirstOrDefault(a =>
            a.AttributeType.FullName == "System.Runtime.CompilerServices.AsyncStateMachineAttribute");
        if (asyncAttr == null) continue;
        if (asyncAttr.ConstructorArguments.Count == 0) continue;

        var smTypeRef = asyncAttr.ConstructorArguments[0].Value as TypeReference;
        if (smTypeRef == null) continue;
        TypeDefinition smTypeDef = null;
        try { smTypeDef = smTypeRef.Resolve(); } catch { }
        if (smTypeDef == null) continue;

        var moveNext = smTypeDef.Methods.FirstOrDefault(mm => mm.Name == "MoveNext");
        if (moveNext == null) continue;

        PrependTrace(moveNext, key + " MoveNext", writeLineRef);
        asyncPatched++;
        report.Add($"  state-machine MoveNext: {key}  ({smTypeDef.FullName})");
    }
}

asm.Write(outputPath);

Console.WriteLine($"wrote: {outputPath}");
Console.WriteLine($"  wrapper traces injected:     {patched}");
Console.WriteLine($"  MoveNext traces injected:    {asyncPatched}");
Console.WriteLine();
foreach (var r in report) Console.WriteLine(r);
