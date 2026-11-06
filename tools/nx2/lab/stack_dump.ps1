# Writes a minidump with every thread's stack (no heap) of a running process.
# Usage: stack_dump.ps1 -ProcessId <pid> -Out <file.dmp>
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Out
)
if (-not ("LabDump" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class LabDump {
    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr process, uint processId, IntPtr file, uint type,
                                         IntPtr exception, IntPtr user, IntPtr callback);
    public static bool Write(System.Diagnostics.Process p, string path) {
        // MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory
        const uint type = 0x1000 | 0x20 | 0x40;
        using (var fs = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None)) {
            return MiniDumpWriteDump(p.Handle, (uint)p.Id, fs.SafeFileHandle.DangerousGetHandle(), type,
                                     IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        }
    }
}
"@
}
$p = Get-Process -Id $ProcessId -ErrorAction Stop
if ([LabDump]::Write($p, $Out)) {
    "dump written: $Out ($([math]::Round((Get-Item $Out).Length / 1MB, 1)) MiB)"
} else {
    "dump FAILED for pid $ProcessId"
}
