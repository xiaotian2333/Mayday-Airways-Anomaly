// MetaDump —— 用游戏 MelonLoader 自带的 AsmResolver 读取 interop 程序集元数据
// 用法: dotnet run --project tools/MetaDump -- <类型名子串> [成员名过滤]
// 示例: dotnet run --project tools/MetaDump -- UIF VLayout
using System;
using System.Linq;
using AsmResolver.DotNet;

internal static class Program
{
    private static void Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.WriteLine("用法: MetaDump <类型名子串> [成员名过滤]");
            return;
        }
        var gameDir = Environment.GetEnvironmentVariable("MAYDAY_GAME_DIR")
            ?? @"D:\games\steam\steamapps\common\Mayday Airways Demo";
        // 程序集查找顺序: MAYDAY_DUMP_ASM 覆盖 → 游戏内 MelonLoader interop → 当前目录 .local/il2cpp-interop（Cpp2IL 生成）
        var overrideAsm = Environment.GetEnvironmentVariable("MAYDAY_DUMP_ASM");
        string asmPath;
        if (!string.IsNullOrEmpty(overrideAsm))
        {
            asmPath = overrideAsm;
        }
        else
        {
            var candidates = new[]
            {
                System.IO.Path.Combine(gameDir, @"MelonLoader\Il2CppAssemblies\Assembly-CSharp.dll"),
                System.IO.Path.Combine(Environment.CurrentDirectory, @".local\il2cpp-interop\Assembly-CSharp.dll"),
            };
            asmPath = candidates.FirstOrDefault(System.IO.File.Exists) ?? candidates[0];
        }
        var module = ModuleDefinition.FromFile(asmPath);

        string needle = args[0];
        string member = args.Length > 1 ? args[1] : null;

        foreach (var t in module.GetAllTypes())
        {
            string tname = (string)t.Name;
            if (tname.IndexOf(needle, StringComparison.OrdinalIgnoreCase) < 0) continue;
            Console.WriteLine($"\n=== {t.FullName} ===");
            if (t.IsEnum)
            {
                foreach (var f in t.Fields.Where(f => f.IsStatic))
                    Console.WriteLine($"  {f.Name} = {f.Constant?.Value}");
                continue;
            }
            foreach (var m in t.Methods)
            {
                string mname = (string)m.Name;
                if (mname.StartsWith('.')) continue;                        // 跳过构造器
                if (member != null && mname.IndexOf(member, StringComparison.OrdinalIgnoreCase) < 0) continue;
                var ps = string.Join(", ", m.Parameters.Select(p => $"{p.ParameterType.Name} {p.Name}"));
                var ret = m.Signature.ReturnType.Name;
                var st = m.IsStatic ? "static " : "";
                Console.WriteLine($"  {st}{ret} {mname}({ps})");
            }
            foreach (var f in t.Fields)
            {
                string fname = (string)f.Name;
                if (member != null && fname.IndexOf(member, StringComparison.OrdinalIgnoreCase) < 0) continue;
                Console.WriteLine($"  field {f.Signature.FieldType.Name} {fname}");
            }
        }
    }
}
