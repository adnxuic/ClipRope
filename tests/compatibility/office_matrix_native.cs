using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Runtime.InteropServices.ComTypes;

public static class MatrixDesktop {
    delegate bool Visitor(IntPtr hwnd, IntPtr arg);
    [DllImport("user32")] static extern bool EnumWindows(Visitor visit, IntPtr arg);
    [DllImport("user32")] static extern bool EnumChildWindows(IntPtr hwnd, Visitor visit, IntPtr arg);
    [DllImport("user32")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int cap);
    [DllImport("user32")] static extern IntPtr GetProcessWindowStation();
    [DllImport("user32", CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr obj,int kind,StringBuilder name,int bytes,out int used);
    [DllImport("oleacc")] static extern int AccessibleObjectFromWindow(IntPtr hwnd,uint id,ref Guid type,[MarshalAs(UnmanagedType.IDispatch)] out object result);
    [DllImport("user32")] static extern IntPtr GetClipboardOwner();
    [DllImport("user32")] static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32")] static extern bool CloseClipboard();
    [DllImport("user32")] static extern bool EmptyClipboard();
    [DllImport("user32",CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowEx(uint ex,string cls,string title,uint style,int x,int y,int w,int h,IntPtr owner,IntPtr menu,IntPtr module,IntPtr data);
    [DllImport("user32")] static extern IntPtr SetClipboardData(uint format,IntPtr value);
    [DllImport("kernel32")] static extern IntPtr GlobalAlloc(uint flags,UIntPtr size);
    [DllImport("kernel32")] static extern IntPtr GlobalFree(IntPtr value);
    [DllImport("user32")] static extern uint EnumClipboardFormats(uint last);
    [DllImport("user32")] static extern IntPtr GetClipboardData(uint f);
    [DllImport("user32",CharSet=CharSet.Unicode)] static extern uint RegisterClipboardFormat(string name);
    [DllImport("kernel32")] static extern UIntPtr GlobalSize(IntPtr h);
    [DllImport("kernel32")] static extern IntPtr GlobalLock(IntPtr h);
    [DllImport("kernel32")] static extern bool GlobalUnlock(IntPtr h);
    [DllImport("user32")] public static extern uint GetClipboardSequenceNumber();

    // Only probe availability. Do not request or rewrite any delayed data.
    public static bool ClipboardReady(uint owner) {
        if(!PrivateStation() || !OpenClipboard(IntPtr.Zero))return false;
        try {return Owner()==owner && EnumClipboardFormats(0)!=0;}
        finally {CloseClipboard();}
    }

    public static bool PrivateStation() {
        var name=new StringBuilder(256);int size;
        return GetUserObjectInformation(GetProcessWindowStation(),2,name,512,out size) &&
               !name.ToString().Equals("WinSta0",StringComparison.OrdinalIgnoreCase) &&
               name.ToString().Equals(Environment.GetEnvironmentVariable("CLIPROPE_PROBE_STATION"),StringComparison.OrdinalIgnoreCase);
    }
    public static uint Owner() {
        uint pid;GetWindowThreadProcessId(GetClipboardOwner(),out pid);return pid;
    }
    static uint sealedFormat;
    static string sealedHash;
    static string Digest(uint format) {
        var h=GetClipboardData(format); if(h==IntPtr.Zero)return null;
        ulong n=GlobalSize(h).ToUInt64(); if(n==0 || n>32*1024*1024)return null;
        var data=new byte[(int)n];var p=GlobalLock(h);if(p==IntPtr.Zero)return null;
        try {Marshal.Copy(p,data,0,data.Length);} finally {GlobalUnlock(h);}
        using(var sha=SHA256.Create()){return Convert.ToBase64String(sha.ComputeHash(data));}
    }
    public static bool Seal(uint owner,uint sequence) {
        if(!PrivateStation() || sequence==0 || !OpenClipboard(IntPtr.Zero))return false;
        try {
            if(GetClipboardSequenceNumber()!=sequence || (owner!=0 && Owner()!=owner))return false;
            foreach(var format in new uint[]{13,RegisterClipboardFormat("PNG"),8,RegisterClipboardFormat("Art::GVML ClipFormat"),RegisterClipboardFormat("HTML Format")}) {
                var hash=Digest(format);if(hash!=null){sealedFormat=format;sealedHash=hash;return true;}
            }
            return false;
        } finally {CloseClipboard();}
    }
    public static bool Clear(uint pid,uint sequence) {
        if(!PrivateStation() || !OpenClipboard(IntPtr.Zero))return false;
        try {
            if(EnumClipboardFormats(0)==0 && GetClipboardOwner()==IntPtr.Zero)return true;
            if((pid!=0 && Owner()==pid) || (sequence!=0 && sequence==GetClipboardSequenceNumber()) ||
               (Owner()==0 && sealedHash!=null && Digest(sealedFormat)==sealedHash))
                return EmptyClipboard();
            return false;
        } finally {CloseClipboard();}
    }
    public static uint Pid(IntPtr window) {
        uint pid;GetWindowThreadProcessId(window,out pid);return pid;
    }
    static IntPtr sentinelOwner;
    static bool Put(uint format,byte[] bytes) {
        var h=GlobalAlloc(2,(UIntPtr)(uint)bytes.Length);
        if(h==IntPtr.Zero)return false;
        var p=GlobalLock(h);
        if(p!=IntPtr.Zero){Marshal.Copy(bytes,0,p,bytes.Length);GlobalUnlock(h);
            if(SetClipboardData(format,h)!=IntPtr.Zero)return true;}
        GlobalFree(h);return false;
    }
    public static bool Overwrite() {
        if(!PrivateStation())return false;
        if(sentinelOwner==IntPtr.Zero)sentinelOwner=CreateWindowEx(0,"STATIC","ClipRope synthetic overwrite",0x80000000u,0,0,0,0,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);
        if(sentinelOwner==IntPtr.Zero || !OpenClipboard(sentinelOwner))return false;
        try {
            const string marker="ClipRope synthetic overwrite";
            if(!EmptyClipboard() || !Put(RegisterClipboardFormat("ExcludeClipboardContentFromMonitorProcessing"),BitConverter.GetBytes(1)) ||
               !Put(13,Encoding.Unicode.GetBytes(marker+"\0")))return false;
            var h=GetClipboardData(13);var p=GlobalLock(h);if(p==IntPtr.Zero)return false;
            try{return Marshal.PtrToStringUni(p)==marker;}finally{GlobalUnlock(h);}
        } finally {CloseClipboard();}
    }
    public static bool Recall(int process) {
        IntPtr hit=IntPtr.Zero;
        EnumWindows(delegate(IntPtr top,IntPtr arg){if(Pid(top)==process && ClassOf(top)=="ClipRope.Workspace"){hit=top;return false;}return true;},IntPtr.Zero);
        if(hit==IntPtr.Zero)return false;
        const int recallRopeHead = 2100; // WorkspaceCommand::head in the native rope menu.
        return PostMessage(hit,0x0111,(IntPtr)recallRopeHead,IntPtr.Zero);
    }
    static string ClassOf(IntPtr h) {
        var name=new StringBuilder(64);GetClassName(h,name,64);return name.ToString();
    }
    public static IntPtr NativePane(int process,string cls,out object doc) {
        IntPtr pane=IntPtr.Zero;
        object found=null;
        EnumWindows(delegate(IntPtr top,IntPtr arg) {
            if(Pid(top)!=process)return true;
            EnumChildWindows(top,delegate(IntPtr child,IntPtr unused) {
                if(ClassOf(child).Equals(cls,StringComparison.OrdinalIgnoreCase)) {
                    var iid=new Guid("00020400-0000-0000-C000-000000000046");
                    if(AccessibleObjectFromWindow(child,0xfffffff0,ref iid,out found)>=0 && found!=null) pane=child;
                }
                return pane==IntPtr.Zero;
            },IntPtr.Zero);
            return pane==IntPtr.Zero;
        },IntPtr.Zero);
        doc=found;return pane;
    }
    public static object Bind(int process,string cls) {
        object doc=null;
        NativePane(process,cls,out doc);
        return doc;
    }
    [DllImport("ole32")] static extern int GetRunningObjectTable(int reserved,out IRunningObjectTable table);
    [DllImport("ole32",CharSet=CharSet.Unicode)] static extern int CreateFileMoniker(string path,out IMoniker moniker);
    public static object RunningDocument(string syntheticPath) {
        IRunningObjectTable table=null;IMoniker file=null;
        try {
            if(GetRunningObjectTable(0,out table)!=0 || CreateFileMoniker(syntheticPath,out file)!=0)return null;
            object document;return table.GetObject(file,out document)==0 ? document : null;
        } finally {
            if(file!=null)Marshal.ReleaseComObject(file);
            if(table!=null)Marshal.ReleaseComObject(table);
        }
    }
    public static string Classes(int process) {
        var seen=new SortedSet<string>();
        EnumWindows(delegate(IntPtr top,IntPtr arg){uint pid;GetWindowThreadProcessId(top,out pid);
            if(pid==process){seen.Add(ClassOf(top));EnumChildWindows(top,delegate(IntPtr child,IntPtr unused){seen.Add(ClassOf(child));return true;},IntPtr.Zero);}
            return true;},IntPtr.Zero);
        return string.Join(",",seen);
    }
    public static IntPtr Listener(int process) {
        IntPtr result=IntPtr.Zero;
        EnumWindows(delegate(IntPtr top,IntPtr arg){uint pid;GetWindowThreadProcessId(top,out pid);
            if(pid==process && ClassOf(top)=="ClipRope.Listener"){result=top;return false;}return true;},IntPtr.Zero);
        return result;
    }
    [DllImport("user32")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
}
