using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;

static class Probe
{
    static int Main()
    {
        string dir = AppContext.BaseDirectory;
        Console.WriteLine("[probe] basedir=" + dir);

        // Resolve game-side assemblies (GodotSharp, Sentry, etc.) from the same folder,
        // replicating the environment godot's load_assembly_and_get_function_pointer runs in.
        AssemblyLoadContext.Default.Resolving += (ctx, name) =>
        {
            string p = Path.Combine(dir, name.Name + ".dll");
            if (File.Exists(p))
            {
                Console.WriteLine("   [resolve] " + name.Name + " -> " + p);
                try { return ctx.LoadFromAssemblyPath(p); }
                catch (Exception e) { Console.WriteLine("   [resolve FAIL] " + name.Name + ": " + e.Message); return null; }
            }
            Console.WriteLine("   [resolve MISS] " + name.Name);
            return null;
        };

        try
        {
            string sts2 = Path.Combine(dir, "sts2.dll");
            Console.WriteLine("[probe] sts2.dll exists=" + File.Exists(sts2));
            var asm = AssemblyLoadContext.Default.LoadFromAssemblyPath(sts2);
            Console.WriteLine("[probe] sts2 loaded: " + asm.FullName);

            var t = asm.GetType("GodotPlugins.Game.Main", throwOnError: false);
            Console.WriteLine("[probe] type GodotPlugins.Game.Main = " + (t == null ? "NULL" : t.FullName));
            if (t == null) return 2;

            var m = t.GetMethod("InitializeFromGameProject",
                BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
            Console.WriteLine("[probe] method InitializeFromGameProject = " + (m == null ? "NULL" : "found"));
            if (m == null)
            {
                foreach (var mm in t.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Instance))
                    Console.WriteLine("   has method: " + mm.Name);
                return 3;
            }

            foreach (var a in m.GetCustomAttributesData())
                Console.WriteLine("   attr: " + a.ToString());
            Console.WriteLine("   ret=" + m.ReturnType.FullName + " (" + m.ReturnType.Assembly.GetName().Name + ")");
            foreach (var p in m.GetParameters())
                Console.WriteLine("   param: " + p.ParameterType.FullName);

            // THE decisive test: prepare + grab the function pointer. This forces the JIT to
            // fully resolve the method signature types and the declaring type — the same work
            // coreclr does inside load_assembly_and_get_function_pointer with UNMANAGEDCALLERSONLY.
            try
            {
                RuntimeHelpers.PrepareMethod(m.MethodHandle);
                Console.WriteLine("[probe] PrepareMethod OK");
            }
            catch (Exception e) { DumpEx("PrepareMethod", e); }

            try
            {
                IntPtr fp = m.MethodHandle.GetFunctionPointer();
                Console.WriteLine("[probe] GetFunctionPointer OK = 0x" + fp.ToString("x"));
            }
            catch (Exception e) { DumpEx("GetFunctionPointer", e); }

            // Also force the declaring type's static ctor / full load.
            try
            {
                RuntimeHelpers.RunClassConstructor(t.TypeHandle);
                Console.WriteLine("[probe] RunClassConstructor OK");
            }
            catch (Exception e) { DumpEx("RunClassConstructor", e); }
        }
        catch (Exception e) { DumpEx("LOAD", e); }
        return 0;
    }

    static void DumpEx(string where, Exception e)
    {
        Console.WriteLine("[probe] " + where + " EXCEPTION: " + e.GetType().FullName + ": " + e.Message);
        if (e is ReflectionTypeLoadException rtle && rtle.LoaderExceptions != null)
            foreach (var le in rtle.LoaderExceptions)
                Console.WriteLine("   LOADER: " + (le == null ? "null" : le.GetType().FullName + ": " + le.Message));
        for (var ie = e.InnerException; ie != null; ie = ie.InnerException)
            Console.WriteLine("   INNER: " + ie.GetType().FullName + ": " + ie.Message);
        Console.WriteLine(e.ToString());
    }
}
