#!/usr/bin/env dotnet-script
/* Replace every Node.GetNode<WorldEnvironment>(...) call in sts2.dll with
 * a 3-instruction sequence that consumes the arguments and pushes null.
 * On a runtime built with disable_3d=yes, godot has no ClassDB entry for
 * `WorldEnvironment`, so the C# binding's GetNode<WorldEnvironment> call
 * throws — silently with our stubbed logger, and NGame._EnterTree never
 * reaches the line below where it kicks off GameStartupWrapper().
 *
 * The game's only use of the WorldEnvironment field is via
 * GodotTreeExtensions.AddChildSafely / RemoveChildSafely, both of which
 * already have `if (child != null) { … }` guards. Holding `null` is safe.
 *
 * Replacement at IL level:
 *
 *   call <T> Node::GetNode<WorldEnvironment>(<args>)
 *      ↓
 *   pop                               (drop NodePath/string)
 *   pop                               (drop `this`)
 *   ldnull                            (push null as the return value)
 *
 * The original call sites end with `stfld <…>k__BackingField` (auto-property
 * setter). With `ldnull` on the stack the field gets set to null cleanly,
 * and execution proceeds to TaskHelper.RunSafely(GameStartupWrapper()) — the
 * actual startup chain the lite/no_3d build needs.
 *
 * Usage:
 *   dotnet-script patch_sts2_null_worldenv.csx -- <in.dll> <out.dll> <ref-dir>
 */
#r "nuget: Mono.Cecil, 0.11.5"

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (Args.Count < 3) {
    Console.Error.WriteLine("usage: patch_sts2_null_worldenv.csx -- <in.dll> <out.dll> <ref-dir>");
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

int patched = 0;
var report = new List<string>();

foreach (var type in asm.MainModule.GetTypes()) {
    foreach (var m in type.Methods) {
        if (m.Body == null) continue;
        var il = m.Body.GetILProcessor();
        // Collect instructions to replace first; mutating during iteration
        // would invalidate the operand pointers Cecil hands us.
        var toReplace = new List<Instruction>();
        foreach (var ins in m.Body.Instructions) {
            if (ins.OpCode != OpCodes.Call && ins.OpCode != OpCodes.Callvirt) continue;
            var mref = ins.Operand as GenericInstanceMethod;
            if (mref == null) continue;
            if (mref.ElementMethod.Name != "GetNode") continue;
            if (mref.GenericArguments.Count != 1) continue;
            if (mref.GenericArguments[0].Name != "WorldEnvironment") continue;
            toReplace.Add(ins);
        }
        foreach (var ins in toReplace) {
            // pop NodePath (or string) arg
            // pop `this` arg
            // ldnull as the return value
            var pop1 = Instruction.Create(OpCodes.Pop);
            var pop2 = Instruction.Create(OpCodes.Pop);
            var ldnull = Instruction.Create(OpCodes.Ldnull);
            il.InsertBefore(ins, pop1);
            il.InsertBefore(ins, pop2);
            il.InsertBefore(ins, ldnull);
            il.Remove(ins);
            patched++;
            report.Add($"  {type.FullName}::{m.Name}");
        }
    }
}

asm.Write(outputPath);

Console.WriteLine($"wrote: {outputPath}");
Console.WriteLine($"  GetNode<WorldEnvironment> calls neutralised: {patched}");
foreach (var r in report) Console.WriteLine(r);
