#!/usr/bin/env dotnet-script
// Patch MegaCrit.Sts2.Core.Models.ModelDb to lazy-instantiate missing
// entries on Get<T>/Get(Type). Required for godot 4.5 mono running with
// the MonoVM runtime on Linux — Mono fires cctor of every type passed to
// Activator.CreateInstance immediately, so a static field initializer
// like `_workerValidCounts = { ModelDb.Monster<BowlbugEgg>(), ... }`
// fires when ModelDb.Init() instantiates BowlbugsNormal (index 655 in
// AllAbstractModelSubtypes), before BowlbugEgg (index 842) is in the
// dictionary. The same code runs fine on CoreCLR because CoreCLR's
// beforefieldinit semantics defer the cctor until first static field
// access.
//
// The patch matches the lazy-inject pattern used by the community Android
// port of StS2, but the IL is written from scratch here. We do not link
// against or import any of that port's bytes — we only borrow the idea.
//
// What the patched Get<T>() does in C# pseudocode:
//
//     var id = GetId<T>();
//     if (!_contentById.ContainsKey(id))
//     {
//         _contentById[id] = (AbstractModel)Activator.CreateInstance(typeof(T));
//     }
//     return (T)_contentById[id];
//
// Get(Type) gets the same treatment with `type` in place of `typeof(T)`.
//
// Usage:
//     dotnet-script patch_modeldb_lazy.csx -- <input.dll> <output.dll>

#r "nuget: Mono.Cecil, 0.11.5"

using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Cecil.Rocks;

if (Args.Count < 2)
{
    Console.Error.WriteLine("usage: patch_modeldb_lazy.csx <input.dll> <output.dll>");
    Environment.Exit(2);
}

var inputPath = Args[0];
var outputPath = Args[1];

Console.WriteLine($"reading {inputPath}");
// Cecil's writer cross-checks every TypeReference during BuildMetadata, so
// we have to give it a resolver that can find Sentry / GodotSharp / fmod /
// spine and friends. By convention these are siblings of sts2.dll on disk —
// the full data_sts2_linuxbsd_arm64/ folder is shipped together.
var resolver = new DefaultAssemblyResolver();
var siblingDir = Path.GetDirectoryName(Path.GetFullPath(inputPath));
resolver.AddSearchDirectory(siblingDir);
// extra search dirs the user can pass after input/output
for (int i = 2; i < Args.Count; i++) resolver.AddSearchDirectory(Args[i]);

var asm = AssemblyDefinition.ReadAssembly(inputPath, new ReaderParameters { AssemblyResolver = resolver });
var module = asm.MainModule;

var modelDb = module.GetType("MegaCrit.Sts2.Core.Models.ModelDb")
    ?? throw new InvalidOperationException("ModelDb type not found");

// ------------------------------------------------------------------
// Resolve every member/method we need.
// ------------------------------------------------------------------

var contentByIdField = modelDb.Fields.Single(f => f.Name == "_contentById");
var dictTypeRef = (GenericInstanceType)contentByIdField.FieldType;
var modelIdType = dictTypeRef.GenericArguments[0];        // MegaCrit.Sts2.Core.Models.ModelId
var abstractModelType = dictTypeRef.GenericArguments[1];  // MegaCrit.Sts2.Core.Models.AbstractModel

// We patch the two private overloads.
var getOfT = modelDb.Methods.Single(m => m.Name == "Get" && m.HasGenericParameters && m.Parameters.Count == 0);
var getOfType = modelDb.Methods.Single(m => m.Name == "Get" && !m.HasGenericParameters && m.Parameters.Count == 1);

// Public helpers we will call from the rewritten bodies.
var getIdOfT = modelDb.Methods.Single(m => m.Name == "GetId" && m.HasGenericParameters);
var getIdOfType = modelDb.Methods.Single(m => m.Name == "GetId" && !m.HasGenericParameters && m.Parameters.Count == 1);

// Dictionary<ModelId, AbstractModel> methods we need: ContainsKey, set_Item, get_Item.
// Cecil needs each call site to reference the closed generic instance of the
// declaring type (Dictionary<TKey, TValue> → Dictionary<ModelId, AbstractModel>),
// not the open generic from mscorlib.
var dictDef = dictTypeRef.Resolve();
MethodReference HostInstance(MethodDefinition def)
{
    var generic = new GenericInstanceType(def.DeclaringType);
    generic.GenericArguments.Add(modelIdType);
    generic.GenericArguments.Add(abstractModelType);
    var r = new MethodReference(def.Name, def.ReturnType, generic)
    {
        HasThis = def.HasThis,
        ExplicitThis = def.ExplicitThis,
        CallingConvention = def.CallingConvention,
    };
    foreach (var p in def.Parameters) r.Parameters.Add(new ParameterDefinition(p.ParameterType));
    foreach (var gp in def.GenericParameters) r.GenericParameters.Add(new GenericParameter(gp.Name, r));
    return module.ImportReference(r);
}
var dictContainsKey = HostInstance(dictDef.Methods.Single(m => m.Name == "ContainsKey"));
var dictGetItem    = HostInstance(dictDef.Methods.Single(m => m.Name == "get_Item"));
var dictSetItem    = HostInstance(dictDef.Methods.Single(m => m.Name == "set_Item"));

// System.Type.GetTypeFromHandle(RuntimeTypeHandle) — needed to turn
// `ldtoken !!T` into a Type object.
var typeRef = module.ImportReference(typeof(Type));
var getTypeFromHandle = module.ImportReference(typeof(Type).GetMethod("GetTypeFromHandle"));

// System.Activator.CreateInstance(Type) → object
var activatorCreateInstance = module.ImportReference(
    typeof(Activator).GetMethod("CreateInstance", new[] { typeof(Type) }));

// ------------------------------------------------------------------
// Helper that emits the lazy-init check for the body.
//
//   var id = GetId<T>();       (caller's choice of GetId overload)
//   if (!_contentById.ContainsKey(id))
//   {
//       _contentById[id] = (AbstractModel)Activator.CreateInstance(typeof(T));
//   }
//   var v = _contentById[id];
//   return (T)v;               (cast left to caller depending on T vs object)
//
// `loadTypeOnto` is supposed to leave a `System.Type` on the eval stack
// — that lets us reuse this for both Get<T> (load via ldtoken !!T) and
// Get(Type) (load via ldarg.0).
// ------------------------------------------------------------------
void RewriteBody(MethodDefinition method, Action<ILProcessor> loadTypeOnto, bool castReturnToGenericArg)
{
    method.Body = new MethodBody(method);
    method.Body.InitLocals = true;

    var idLocal = new VariableDefinition(modelIdType);
    method.Body.Variables.Add(idLocal);

    var il = method.Body.GetILProcessor();

    // -- compute id and stash it
    if (method == getOfT)
    {
        var getIdOfTClosed = new GenericInstanceMethod(getIdOfT);
        getIdOfTClosed.GenericArguments.Add(method.GenericParameters[0]);
        il.Emit(OpCodes.Call, getIdOfTClosed);
    }
    else
    {
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, getIdOfType);
    }
    il.Emit(OpCodes.Stloc_0);

    // -- if (_contentById.ContainsKey(id)) skip
    il.Emit(OpCodes.Ldsfld, contentByIdField);
    il.Emit(OpCodes.Ldloc_0);
    il.Emit(OpCodes.Callvirt, dictContainsKey);
    var skipInjection = il.Create(OpCodes.Nop);
    il.Emit(OpCodes.Brtrue, skipInjection);

    // -- _contentById[id] = (AbstractModel)Activator.CreateInstance(typeof(T) or type)
    il.Emit(OpCodes.Ldsfld, contentByIdField);
    il.Emit(OpCodes.Ldloc_0);
    loadTypeOnto(il);
    il.Emit(OpCodes.Call, activatorCreateInstance);
    il.Emit(OpCodes.Castclass, abstractModelType);
    il.Emit(OpCodes.Callvirt, dictSetItem);

    il.Append(skipInjection);

    // -- return _contentById[id]   (with cast to T for the generic flavour)
    il.Emit(OpCodes.Ldsfld, contentByIdField);
    il.Emit(OpCodes.Ldloc_0);
    il.Emit(OpCodes.Callvirt, dictGetItem);
    if (castReturnToGenericArg)
    {
        // Cast object to T (which is constrained to AbstractModel, so this is
        // really an upcast in IL terms but `castclass` is correct.)
        il.Emit(OpCodes.Castclass, method.GenericParameters[0]);
    }
    il.Emit(OpCodes.Ret);
}

Console.WriteLine("patching ModelDb.Get<T>()");
RewriteBody(getOfT,
    loadTypeOnto: il =>
    {
        il.Emit(OpCodes.Ldtoken, getOfT.GenericParameters[0]);
        il.Emit(OpCodes.Call, getTypeFromHandle);
    },
    castReturnToGenericArg: true);

Console.WriteLine("patching ModelDb.Get(Type)");
RewriteBody(getOfType,
    loadTypeOnto: il => il.Emit(OpCodes.Ldarg_0),
    castReturnToGenericArg: false);

// ------------------------------------------------------------------
// Patch ModelDb.Init() to defer the "instantiate every model" step to
// Inject(Type), which already has a `if (!Contains(type)) ...` guard
// pre-baked. Without this, our lazy Get<T> patch can add a model to the
// dict during a sibling model's cctor, and then ModelDb.Init's plain
// `_contentById[id] = (AbstractModel)Activator.CreateInstance(type);`
// runs again on the same type — AbstractModel's ctor sees a duplicate
// canonical entry and throws DuplicateModelException.
//
// Equivalent C# we synthesise:
//
//     public static void Init()
//     {
//         Type[] types = AllAbstractModelSubtypes;
//         for (int i = 0; i < types.Length; i++)
//             Inject(types[i]);
//     }
// ------------------------------------------------------------------
Console.WriteLine("patching ModelDb.Init()");
var initMethod = modelDb.Methods.Single(m => m.Name == "Init" && m.Parameters.Count == 0);
var allSubtypesGetter = modelDb.Methods.Single(m => m.Name == "get_AllAbstractModelSubtypes");
var injectMethod = modelDb.Methods.Single(m => m.Name == "Inject" && m.Parameters.Count == 1);
var typeArrayRef = allSubtypesGetter.ReturnType;  // Type[]

initMethod.Body = new MethodBody(initMethod);
initMethod.Body.InitLocals = true;
var typesLocal = new VariableDefinition(typeArrayRef);
var iLocal = new VariableDefinition(module.TypeSystem.Int32);
initMethod.Body.Variables.Add(typesLocal);
initMethod.Body.Variables.Add(iLocal);

var initIl = initMethod.Body.GetILProcessor();
// var types = AllAbstractModelSubtypes;
initIl.Emit(OpCodes.Call, allSubtypesGetter);
initIl.Emit(OpCodes.Stloc_0);
// int i = 0;
initIl.Emit(OpCodes.Ldc_I4_0);
initIl.Emit(OpCodes.Stloc_1);
// while (i < types.Length)  →  br loopCheck
var loopCheck = initIl.Create(OpCodes.Ldloc_1);
initIl.Emit(OpCodes.Br, loopCheck);

// loop body: Inject(types[i]);
var loopBody = initIl.Create(OpCodes.Ldloc_0);
initIl.Append(loopBody);
initIl.Emit(OpCodes.Ldloc_1);
initIl.Emit(OpCodes.Ldelem_Ref);
initIl.Emit(OpCodes.Call, injectMethod);
// i++
initIl.Emit(OpCodes.Ldloc_1);
initIl.Emit(OpCodes.Ldc_I4_1);
initIl.Emit(OpCodes.Add);
initIl.Emit(OpCodes.Stloc_1);
// check: i < types.Length → loopBody
initIl.Append(loopCheck);
initIl.Emit(OpCodes.Ldloc_0);
initIl.Emit(OpCodes.Ldlen);
initIl.Emit(OpCodes.Conv_I4);
initIl.Emit(OpCodes.Blt, loopBody);
initIl.Emit(OpCodes.Ret);

// ------------------------------------------------------------------
// Patch PreloadManager.Enabled's default initializer from `true` to
// `false`. The public bool property is auto-implemented and assigned
// in the type's static constructor (`.cctor`) via `ldc.i4.1; stsfld`.
// We just flip the ldc.i4.1 to ldc.i4.0 — the rest of the runtime path
// stays intact and `LoadAssetSets()` already early-returns when
// Enabled is false (returns AssetLoadingSession.Empty()).
//
// Effect: the 769-asset "Common" preload at main menu init never runs;
// each Cache.GetTexture2D/GetScene call falls back to lazy
// ResourceLoader.Load via AssetCache.LoadAsset (which only logs a
// warning per miss). Expected savings on a 1 GB-RAM handheld: ~100-200 MB
// of heap/VRAM that would otherwise sit caching gameplay assets at the
// main menu.
// ------------------------------------------------------------------
Console.WriteLine("patching PreloadManager.Enabled default → false");
var preloadMgr = module.GetType("MegaCrit.Sts2.Core.Assets.PreloadManager")
    ?? throw new InvalidOperationException("PreloadManager type not found");
var preloadCctor = preloadMgr.Methods.SingleOrDefault(m => m.Name == ".cctor")
    ?? throw new InvalidOperationException("PreloadManager .cctor not found");
var enabledBackingField = preloadMgr.Fields.Single(
    f => f.Name == "<Enabled>k__BackingField");

bool flipped = false;
var cctorInstructions = preloadCctor.Body.Instructions.ToList();
for (int i = 0; i < cctorInstructions.Count - 1; i++)
{
    var cur = cctorInstructions[i];
    var next = cctorInstructions[i + 1];
    if (cur.OpCode == OpCodes.Ldc_I4_1 &&
        next.OpCode == OpCodes.Stsfld &&
        ((FieldReference)next.Operand).FullName == enabledBackingField.FullName)
    {
        var il = preloadCctor.Body.GetILProcessor();
        il.Replace(cur, il.Create(OpCodes.Ldc_I4_0));
        flipped = true;
        break;
    }
}
if (!flipped)
    throw new InvalidOperationException("did not find `ldc.i4.1; stsfld Enabled` in PreloadManager.cctor — maybe the IL shape changed");

Console.WriteLine($"writing {outputPath}");
asm.Write(outputPath);
Console.WriteLine("done");

