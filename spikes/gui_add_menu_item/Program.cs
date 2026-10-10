// Adds one entry to a GUI list by copying an existing entry's element (spike; spikes/merchant_menu_test.md).
//   gui_add_menu_item dump <in.gui> [all]                 the list's elements and their attributes (all: every container)
//   gui_add_menu_item roundtrip <in.gui> <out.gui>        read and write unchanged (must come out byte-identical)
//   gui_add_menu_item add <in.gui> <out.gui> <copy> <new> a new element <new> after <copy>, sharing its children
// The copy shares the original's container (its children's definition), so nothing below it is duplicated; a
// Position attribute, if any, is moved on by the step between the two entries before it.
using System.Reflection;
using ReeLib;
using ReeLib.Gui;

static GuiFile Load(string path)
{
    var f = new GuiFile(new FileHandler(path));
    if (!f.Read()) throw new Exception("couldn't read " + path);
    return f;
}
static void Save(GuiFile f, string path)
{
    f.FileHandler = new FileHandler(new MemoryStream(), path);
    if (!f.Write()) throw new Exception("couldn't write " + path);
    f.FileHandler.Save(path);
}
static T Clone<T>(T o) where T : notnull =>
    (T)typeof(object).GetMethod("MemberwiseClone", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(o, null)!;
static void SetList<T>(object owner, string prop, List<T> value)
{
    var field = owner.GetType().GetField($"<{prop}>k__BackingField", BindingFlags.Instance | BindingFlags.NonPublic)
        ?? throw new Exception("no field for " + prop);
    field.SetValue(owner, value);
}
static (GuiContainer, int) Find(GuiFile f, string name)
{
    foreach (var c in f.Containers)
        for (int i = 0; i < c.Elements.Count; i++)
            if (c.Elements[i].Name == name) return (c, i);
    throw new Exception("no element named " + name);
}
static string Describe(ReeLib.Gui.Attribute a) => $"{a.Name} ({a.PropertyType}) = {Fmt(a.Value)}";
static string Fmt(object? v) => v switch
{
    null => "null",
    System.Numerics.Vector2 x => $"({x.X}, {x.Y})",
    System.Numerics.Vector3 x => $"({x.X}, {x.Y}, {x.Z})",
    System.Numerics.Vector4 x => $"({x.X}, {x.Y}, {x.Z}, {x.W})",
    _ => v.ToString() ?? "",
};

var mode = args.Length > 0 ? args[0] : "";
if (mode == "dump" && args.Length >= 2)
{
    var f = Load(args[1]);
    Console.WriteLine($"version {f.Header.version}, {f.Containers.Count} containers");
    foreach (var c in f.Containers)
    {
        if (args.Length < 3 && !c.Elements.Any(e => e.Name.StartsWith("si_"))) continue;
        Console.WriteLine($"container '{c.Info.Name}' ({c.Info.ClassName}) {c.Info.ID}: {c.Elements.Count} elements");
        foreach (var e in c.Elements)
        {
            Console.WriteLine($"  {e.Name} ({e.ClassName}) id {e.ID} container {e.ContainerID}");
            foreach (var a in e.Attributes) Console.WriteLine("    " + Describe(a));
            foreach (var a in e.ExtraAttributes) Console.WriteLine("    extra " + Describe(a));
            Console.WriteLine($"    data {Convert.ToHexString(e.ElementData)} staterefs {e.ExtraStateRefs.Count}");
        }
        Console.WriteLine($"  clips {c.Clips.Count}");
    }
    Console.WriteLine($"overrides {f.AttributeOverrides.Count}, resources {string.Join(",", f.Resources)}, guis {string.Join(",", f.LinkedGUIs)}, params {f.Parameters.Count}/{f.ParameterReferences.Count}/{f.ParameterOverrides.Count}");
    return 0;
}
if (mode == "roundtrip" && args.Length >= 3)
{
    Save(Load(args[1]), args[2]);
    var same = File.ReadAllBytes(args[1]).AsSpan().SequenceEqual(File.ReadAllBytes(args[2]));
    Console.WriteLine(same ? "byte-identical" : "DIFFERENT");
    return same ? 0 : 1;
}
if (mode == "add" && args.Length >= 5)
{
    var f = Load(args[1]);
    var (list, at) = Find(f, args[3]);
    var src = list.Elements[at];
    var e = Clone(src);
    e.Name = args[4];
    e.ID = new GuiObjectID(Guid.NewGuid());
    SetList(e, "Attributes", src.Attributes.Select(a => Clone(a)).ToList());
    SetList(e, "ExtraAttributes", src.ExtraAttributes.Select(a => Clone(a)).ToList());
    SetList(e, "ExtraStateRefs", src.ExtraStateRefs.Select(r => Clone(r)).ToList());
    e.ElementData = (byte[])src.ElementData.Clone();
    foreach (var a in e.Attributes.Where(a => a.Name == "Name")) a.Value = args[4];  // the Name attribute too
    var pos = e.Attributes.FirstOrDefault(a => a.Name == "Position");
    var prev = at > 0 ? list.Elements[at - 1].Attributes.FirstOrDefault(a => a.Name == "Position") : null;
    if (pos != null && prev != null && pos.Value is System.Numerics.Vector3 p && prev.Value is System.Numerics.Vector3 q)
    {
        pos.Value = p + (p - q);
        Console.WriteLine($"position {Fmt(p)} -> {Fmt(pos.Value)}");
    }
    else Console.WriteLine("no Position attribute moved (the list may lay its items out itself)");
    var prio = e.Attributes.FirstOrDefault(a => a.Name == "Priority");  // the entries count up 10, 11, 12, 13
    if (prio?.Value is ushort u) { prio.Value = (ushort)(u + 1); Console.WriteLine($"priority {u} -> {u + 1}"); }
    list.Elements.Insert(at + 1, e);
    Save(f, args[2]);
    var back = Load(args[2]);
    Console.WriteLine($"written; read back: '{back.Containers.First(c => c.Info.ID == list.Info.ID).Elements.Count}' elements in '{list.Info.Name}'");
    return 0;
}
Console.WriteLine("usage: dump <in> | roundtrip <in> <out> | add <in> <out> <copy> <new>");
return 2;
