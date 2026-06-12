#!/usr/bin/env dotnet-script
/* Short-circuit StS2's heavy startup preload methods so the 769 common +
 * main-menu assets aren't all pinned in Mali GPU memory at once. Mirrors
 * what the community Android port does inside its rewritten PreloadManager
 * (wrapping the bodies in `if (Enabled)` with Enabled defaulting false).
 *
 * Each targeted method is an `async Task` wrapper. The user-written body
 * lives in a compiler-generated state machine; the wrapper just builds the
 * state machine and returns the task. We can short-circuit either layer.
 * The simplest and most robust is replacing the wrapper body with:
 *
 *     ldnull                      // (not actually needed — see below)
 *     call Task.get_CompletedTask // push a finished Task
 *     ret
 *
 * Now whoever awaits these methods gets an already-completed task and
 * proceeds immediately. Mali never sees the 769-asset bulk upload; assets
 * are loaded lazily by `Cache.LoadAsset` when game code first asks for
 * each sprite. Trade-off:
 *   * first-time-use latency for each atlas during gameplay
 *   * main-menu load FAST (no preload phase)
 *   * Mali peak drops by however much the preload was pinning
 *
 * Methods short-circuited:
 *   PreloadManager.LoadCommonAndMainMenuAssets   ← biggest, 769 assets
 *   PreloadManager.LoadMainMenuAssets            ← also bulky
 *   NGame.LoadDeferredStartupAssetsAsync         ← deferred bulk too
 *
 * Usage:
 *   dotnet-script patch_sts2_skip_preload.csx -- <in.dll> <out.dll> <ref-dir>
 */
#r "nuget: Mono.Cecil, 0.11.5"

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (Args.Count < 3) {
    Console.Error.WriteLine("usage: patch_sts2_skip_preload.csx -- <in.dll> <out.dll> <ref-dir>");
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

// Resolve System.Threading.Tasks.Task::get_CompletedTask
MethodReference completedTaskGetter = null;
try {
    var taskType = asm.MainModule.ImportReference(typeof(System.Threading.Tasks.Task)).Resolve();
    var getter = taskType.Methods.FirstOrDefault(m => m.Name == "get_CompletedTask" && m.IsStatic);
    if (getter != null) completedTaskGetter = asm.MainModule.ImportReference(getter);
} catch {}

if (completedTaskGetter == null) {
    // Fall back: build the reference manually.
    var voidType = asm.MainModule.TypeSystem.Void;
    var taskRef = new TypeReference("System.Threading.Tasks", "Task",
        asm.MainModule, asm.MainModule.TypeSystem.CoreLibrary, false);
    var mr = new MethodReference("get_CompletedTask", taskRef, taskRef) {
        HasThis = false,
    };
    completedTaskGetter = asm.MainModule.ImportReference(mr);
}

string[] targets = {
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadCommonAndMainMenuAssets",
    "MegaCrit.Sts2.Core.Assets.PreloadManager::LoadMainMenuAssets",
    "MegaCrit.Sts2.Core.Nodes.NGame::LoadDeferredStartupAssetsAsync",
};

int patched = 0;
var report = new List<string>();

foreach (var type in asm.MainModule.GetTypes()) {
    foreach (var m in type.Methods) {
        string key = type.FullName + "::" + m.Name;
        if (!targets.Contains(key)) continue;
        if (m.Body == null) continue;
        if (m.ReturnType.FullName != "System.Threading.Tasks.Task") {
            report.Add($"  SKIP {key}: returns {m.ReturnType.FullName}, not Task");
            continue;
        }
        // Replace the entire body with: call Task.get_CompletedTask; ret
        m.Body.Instructions.Clear();
        m.Body.ExceptionHandlers.Clear();
        m.Body.Variables.Clear();
        var il = m.Body.GetILProcessor();
        il.Append(Instruction.Create(OpCodes.Call, completedTaskGetter));
        il.Append(Instruction.Create(OpCodes.Ret));
        // Strip the [AsyncStateMachine] attribute so .NET doesn't try to
        // step into a state machine that no longer matches the (empty) body.
        var asyncAttr = m.CustomAttributes.FirstOrDefault(a =>
            a.AttributeType.FullName == "System.Runtime.CompilerServices.AsyncStateMachineAttribute");
        if (asyncAttr != null) m.CustomAttributes.Remove(asyncAttr);
        patched++;
        report.Add($"  short-circuited: {key}");
    }
}

asm.Write(outputPath);

Console.WriteLine($"wrote: {outputPath}");
Console.WriteLine($"  methods short-circuited: {patched}");
foreach (var r in report) Console.WriteLine(r);
