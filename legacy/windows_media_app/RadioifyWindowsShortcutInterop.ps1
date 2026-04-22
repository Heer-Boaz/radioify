Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Initialize-RadioifyWindowsShortcutInterop {
    if ("Radioify.WindowsShell.ShortcutInterop" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Radioify.WindowsShell {
    internal static class NativeMethods {
        internal const int RpcEChangedMode = unchecked((int)0x80010106);
        internal const uint CoiApartmentThreaded = 0x2;
        internal const uint StgmRead = 0x00000000;

        [DllImport("ole32.dll")]
        internal static extern int CoInitializeEx(IntPtr pvReserved, uint dwCoInit);

        [DllImport("ole32.dll")]
        internal static extern void CoUninitialize();

        [DllImport("ole32.dll")]
        internal static extern int PropVariantClear(ref PropVariant pvar);
    }

    [ComImport]
    [Guid("00021401-0000-0000-C000-000000000046")]
    internal class CShellLink {
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("000214F9-0000-0000-C000-000000000046")]
    internal interface IShellLinkW {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
        void GetIDList(out IntPtr ppidl);
        void SetIDList(IntPtr pidl);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder pszName, int cch);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder pszDir, int cch);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder pszArgs, int cch);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
        void GetHotkey(out short pwHotkey);
        void SetHotkey(short wHotkey);
        void GetShowCmd(out int piShowCmd);
        void SetShowCmd(int iShowCmd);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder pszIconPath, int cch, out int piIcon);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
        void Resolve(IntPtr hwnd, uint fFlags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("0000010B-0000-0000-C000-000000000046")]
    internal interface IPersistFile {
        void GetClassID(out Guid pClassID);
        void IsDirty();
        void Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
        void Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, bool fRemember);
        void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
        void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string ppszFileName);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
    internal interface IPropertyStore {
        void GetCount(out uint cProps);
        void GetAt(uint iProp, out PropertyKey pkey);
        void GetValue(ref PropertyKey key, out PropVariant pv);
        void SetValue(ref PropertyKey key, ref PropVariant pv);
        void Commit();
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    internal struct PropertyKey {
        public Guid FormatId;
        public uint PropertyId;

        public PropertyKey(Guid formatId, uint propertyId) {
            FormatId = formatId;
            PropertyId = propertyId;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropVariant : IDisposable {
        public ushort vt;
        public ushort wReserved1;
        public ushort wReserved2;
        public ushort wReserved3;
        public IntPtr p;
        public int p2;

        public static PropVariant FromString(string value) {
            return new PropVariant {
                vt = 31,
                p = Marshal.StringToCoTaskMemUni(value ?? string.Empty)
            };
        }

        public string GetString() {
            if (vt == 31 && p != IntPtr.Zero) {
                return Marshal.PtrToStringUni(p);
            }
            if (vt == 8 && p != IntPtr.Zero) {
                return Marshal.PtrToStringBSTR(p);
            }
            return null;
        }

        public void Dispose() {
            NativeMethods.PropVariantClear(ref this);
        }
    }

    public static class ShortcutInterop {
        private static readonly PropertyKey AppUserModelIdKey =
            new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 5);

        public static void CreateShortcut(string shortcutPath,
                                          string targetPath,
                                          string workingDirectory,
                                          string arguments,
                                          string description,
                                          string iconPath,
                                          int iconIndex,
                                          string appUserModelId) {
            if (string.IsNullOrWhiteSpace(shortcutPath)) {
                throw new ArgumentException("Shortcut path is required.", "shortcutPath");
            }
            if (string.IsNullOrWhiteSpace(targetPath)) {
                throw new ArgumentException("Target path is required.", "targetPath");
            }

            string shortcutDirectory = Path.GetDirectoryName(shortcutPath);
            if (!string.IsNullOrEmpty(shortcutDirectory)) {
                Directory.CreateDirectory(shortcutDirectory);
            }

            int initHr = NativeMethods.CoInitializeEx(IntPtr.Zero, NativeMethods.CoiApartmentThreaded);
            bool uninitialize = initHr >= 0;
            if (initHr < 0 && initHr != NativeMethods.RpcEChangedMode) {
                Marshal.ThrowExceptionForHR(initHr);
            }

            object shellLinkObject = new CShellLink();
            try {
                IShellLinkW shellLink = (IShellLinkW)shellLinkObject;
                shellLink.SetPath(targetPath);

                if (!string.IsNullOrWhiteSpace(workingDirectory)) {
                    shellLink.SetWorkingDirectory(workingDirectory);
                }
                if (!string.IsNullOrWhiteSpace(arguments)) {
                    shellLink.SetArguments(arguments);
                }
                if (!string.IsNullOrWhiteSpace(description)) {
                    shellLink.SetDescription(description);
                }
                if (!string.IsNullOrWhiteSpace(iconPath)) {
                    shellLink.SetIconLocation(iconPath, iconIndex);
                }

                if (!string.IsNullOrWhiteSpace(appUserModelId)) {
                    IPropertyStore propertyStore = (IPropertyStore)shellLinkObject;
                    PropertyKey appUserModelIdKey = AppUserModelIdKey;
                    PropVariant appIdValue = PropVariant.FromString(appUserModelId);
                    try {
                        propertyStore.SetValue(ref appUserModelIdKey, ref appIdValue);
                        propertyStore.Commit();
                    } finally {
                        appIdValue.Dispose();
                    }
                }

                IPersistFile persistFile = (IPersistFile)shellLinkObject;
                persistFile.Save(shortcutPath, true);
            } finally {
                if (Marshal.IsComObject(shellLinkObject)) {
                    Marshal.FinalReleaseComObject(shellLinkObject);
                }
                if (uninitialize) {
                    NativeMethods.CoUninitialize();
                }
            }
        }

        public static string GetShortcutAppUserModelId(string shortcutPath) {
            if (string.IsNullOrWhiteSpace(shortcutPath) || !File.Exists(shortcutPath)) {
                return null;
            }

            int initHr = NativeMethods.CoInitializeEx(IntPtr.Zero, NativeMethods.CoiApartmentThreaded);
            bool uninitialize = initHr >= 0;
            if (initHr < 0 && initHr != NativeMethods.RpcEChangedMode) {
                Marshal.ThrowExceptionForHR(initHr);
            }

            object shellLinkObject = new CShellLink();
            try {
                IPersistFile persistFile = (IPersistFile)shellLinkObject;
                persistFile.Load(shortcutPath, NativeMethods.StgmRead);

                IPropertyStore propertyStore = (IPropertyStore)shellLinkObject;
                PropertyKey appUserModelIdKey = AppUserModelIdKey;
                PropVariant appIdValue;
                propertyStore.GetValue(ref appUserModelIdKey, out appIdValue);
                try {
                    return appIdValue.GetString();
                } finally {
                    appIdValue.Dispose();
                }
            } finally {
                if (Marshal.IsComObject(shellLinkObject)) {
                    Marshal.FinalReleaseComObject(shellLinkObject);
                }
                if (uninitialize) {
                    NativeMethods.CoUninitialize();
                }
            }
        }
    }
}
"@
}

function New-RadioifyWindowsShortcut {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TargetPath,
        [string]$WorkingDirectory,
        [string]$Arguments,
        [string]$Description,
        [string]$IconPath,
        [int]$IconIndex = 0,
        [string]$AppUserModelId
    )

    Initialize-RadioifyWindowsShortcutInterop
    [Radioify.WindowsShell.ShortcutInterop]::CreateShortcut(
        $Path,
        $TargetPath,
        $WorkingDirectory,
        $Arguments,
        $Description,
        $IconPath,
        $IconIndex,
        $AppUserModelId
    )
}

function Get-RadioifyWindowsShortcutAppUserModelId {
    param([Parameter(Mandatory = $true)][string]$Path)

    Initialize-RadioifyWindowsShortcutInterop
    return [Radioify.WindowsShell.ShortcutInterop]::GetShortcutAppUserModelId($Path)
}
