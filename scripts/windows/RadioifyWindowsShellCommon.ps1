Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-RadioifyWindowsHost {
    $platform = [System.Environment]::OSVersion.Platform
    if ($platform -ne [System.PlatformID]::Win32NT) {
        throw "This script must be run on Windows."
    }
}

function Invoke-RadioifyShellAssociationRefresh {
    if (-not ("Radioify.ShellNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace Radioify {
    public static class ShellNative {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
"@
    }

    [Radioify.ShellNative]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
}
