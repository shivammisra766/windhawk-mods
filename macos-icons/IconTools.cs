// Helpers for build-macos-icons.ps1: PNG -> multi-size ICO, ICO -> resource-only DLL,
// reading icon group names from an existing PE (read-only, as data file).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class MacIconTools
{
    const uint LOAD_LIBRARY_AS_DATAFILE = 0x2;
    const uint LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x20;
    static readonly IntPtr RT_ICON = (IntPtr)3;
    static readonly IntPtr RT_GROUP_ICON = (IntPtr)14;

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool FreeLibrary(IntPtr h);
    delegate bool EnumResNameProc(IntPtr hModule, IntPtr type, IntPtr name, IntPtr param);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool EnumResourceNames(IntPtr hModule, IntPtr type, EnumResNameProc proc, IntPtr param);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateResource(IntPtr h, IntPtr type, string name, ushort lang, byte[] data, uint size);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool EndUpdateResource(IntPtr h, bool discard);
    [DllImport("user32", CharSet = CharSet.Unicode)]
    static extern uint PrivateExtractIcons(string file, int index, int cx, int cy, IntPtr[] icons, int[] ids, uint n, uint flags);
    [DllImport("user32")]
    static extern bool DestroyIcon(IntPtr h);

    // Names of the RT_GROUP_ICON resources in enumeration order (index 0 = app icon).
    // Integer ids are returned as "#123".
    public static string[] GetIconGroupNames(string path)
    {
        var names = new List<string>();
        IntPtr h = LoadLibraryEx(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (h == IntPtr.Zero) return null;
        try
        {
            EnumResourceNames(h, RT_GROUP_ICON, (m, t, n, p) =>
            {
                long v = n.ToInt64();
                names.Add((v >> 16) == 0 ? "#" + v : Marshal.PtrToStringUni(n));
                return true;
            }, IntPtr.Zero);
        }
        finally { FreeLibrary(h); }
        return names.ToArray();
    }

    static Bitmap Resize(Image src, int size)
    {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.Transparent);
            g.CompositingMode = CompositingMode.SourceCopy;
            g.CompositingQuality = CompositingQuality.HighQuality;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.SmoothingMode = SmoothingMode.HighQuality;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            using (var wrap = new ImageAttributes())
            {
                wrap.SetWrapMode(WrapMode.TileFlipXY);
                g.DrawImage(src, new Rectangle(0, 0, size, size), 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, wrap);
            }
        }
        return bmp;
    }

    // 32bpp BITMAPINFOHEADER + XOR (bottom-up BGRA) + AND mask, as stored in .ico / RT_ICON.
    static byte[] DibEntry(Bitmap bmp)
    {
        int s = bmp.Width;
        int maskStride = ((s + 31) / 32) * 4;
        using (var ms = new MemoryStream())
        using (var w = new BinaryWriter(ms))
        {
            w.Write(40); w.Write(s); w.Write(s * 2); w.Write((short)1); w.Write((short)32);
            w.Write(0); w.Write(s * s * 4 + maskStride * s); w.Write(0); w.Write(0); w.Write(0); w.Write(0);
            var data = bmp.LockBits(new Rectangle(0, 0, s, s), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var row = new byte[s * 4];
            var mask = new byte[maskStride * s];
            for (int y = s - 1; y >= 0; y--)
            {
                Marshal.Copy(data.Scan0 + y * data.Stride, row, 0, row.Length);
                w.Write(row);
                int my = s - 1 - y;
                for (int x = 0; x < s; x++)
                    if (row[x * 4 + 3] == 0) mask[my * maskStride + x / 8] |= (byte)(0x80 >> (x % 8));
            }
            bmp.UnlockBits(data);
            w.Write(mask);
            return ms.ToArray();
        }
    }

    static byte[] PngEntry(Bitmap bmp)
    {
        using (var ms = new MemoryStream()) { bmp.Save(ms, ImageFormat.Png); return ms.ToArray(); }
    }

    // Returns the image entries (sizes ascending) for one source PNG.
    // Sizes <= 48 are stored as DIBs (widest compatibility), larger ones as PNG.
    public static List<KeyValuePair<int, byte[]>> MakeEntries(string pngPath, int[] sizes)
    {
        var list = new List<KeyValuePair<int, byte[]>>();
        using (var src = Image.FromFile(pngPath))
            foreach (int s in sizes)
                using (var b = Resize(src, s))
                    list.Add(new KeyValuePair<int, byte[]>(s, s <= 48 ? DibEntry(b) : PngEntry(b)));
        return list;
    }

    static void WriteDirEntry(BinaryWriter w, int size, byte[] img)
    {
        w.Write((byte)(size >= 256 ? 0 : size)); w.Write((byte)(size >= 256 ? 0 : size));
        w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
        w.Write(img.Length);
    }

    public static void WriteIco(string pngPath, int[] sizes, string icoPath)
    {
        var e = MakeEntries(pngPath, sizes);
        using (var fs = File.Create(icoPath))
        using (var w = new BinaryWriter(fs))
        {
            w.Write((short)0); w.Write((short)1); w.Write((short)e.Count);
            int offset = 6 + 16 * e.Count;
            foreach (var kv in e) { WriteDirEntry(w, kv.Key, kv.Value); w.Write(offset); offset += kv.Value.Length; }
            foreach (var kv in e) w.Write(kv.Value);
        }
    }

    // Reads an .ico file into (size, imageBytes) entries.
    static List<KeyValuePair<int, byte[]>> ReadIco(string icoPath)
    {
        var list = new List<KeyValuePair<int, byte[]>>();
        var all = File.ReadAllBytes(icoPath);
        int count = BitConverter.ToUInt16(all, 4);
        for (int i = 0; i < count; i++)
        {
            int o = 6 + i * 16;
            int size = all[o] == 0 ? 256 : all[o];
            int len = BitConverter.ToInt32(all, o + 8);
            int off = BitConverter.ToInt32(all, o + 12);
            var img = new byte[len];
            Buffer.BlockCopy(all, off, img, 0, len);
            list.Add(new KeyValuePair<int, byte[]>(size, img));
        }
        return list;
    }

    // Copies stubDll to dllPath and writes the .ico as RT_ICON entries plus one
    // RT_GROUP_ICON per requested group name ("#123" = integer id), all sharing the images.
    public static void WriteResourceDll(string stubDll, string icoPath, string[] groupNames, string dllPath)
    {
        File.Copy(stubDll, dllPath, true);
        var e = ReadIco(icoPath);
        IntPtr h = BeginUpdateResource(dllPath, false);
        if (h == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        bool ok = false;
        try
        {
            for (int i = 0; i < e.Count; i++)
                if (!UpdateResource(h, RT_ICON, (IntPtr)(i + 1), 0, e[i].Value, (uint)e[i].Value.Length))
                    throw new System.ComponentModel.Win32Exception();
            byte[] grp;
            using (var ms = new MemoryStream())
            using (var w = new BinaryWriter(ms))
            {
                w.Write((short)0); w.Write((short)1); w.Write((short)e.Count);
                for (int i = 0; i < e.Count; i++)
                {
                    int s = e[i].Key;
                    w.Write((byte)(s >= 256 ? 0 : s)); w.Write((byte)(s >= 256 ? 0 : s));
                    w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
                    w.Write(e[i].Value.Length); w.Write((short)(i + 1));
                }
                grp = ms.ToArray();
            }
            foreach (var name in groupNames)
            {
                bool r = name.StartsWith("#")
                    ? UpdateResource(h, RT_GROUP_ICON, (IntPtr)int.Parse(name.Substring(1)), 0, grp, (uint)grp.Length)
                    : UpdateResource(h, RT_GROUP_ICON, name, 0, grp, (uint)grp.Length);
                if (!r) throw new System.ComponentModel.Win32Exception();
            }
            ok = true;
        }
        finally
        {
            if (!EndUpdateResource(h, !ok) && ok) throw new System.ComponentModel.Win32Exception();
        }
    }

    // Self-test: how many icons PrivateExtractIcons gets from file at index (or -id).
    public static uint TestExtract(string file, int index, int size)
    {
        var icons = new IntPtr[1]; var ids = new int[1];
        uint n = PrivateExtractIcons(file, index, size, size, icons, ids, 1, 0);
        if (icons[0] != IntPtr.Zero) DestroyIcon(icons[0]);
        return n;
    }
}
