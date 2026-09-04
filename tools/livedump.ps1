# livedump.ps1 - write a full minidump of a hung FiveM game process, for the
# hang analysis described in docs/diagnostics.md.

param([string]$Out)
$src = @"
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class MD {
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern bool MiniDumpWriteDump(IntPtr hProcess, uint pid, IntPtr hFile, uint type, IntPtr a, IntPtr b, IntPtr c);
}
"@
Add-Type -TypeDefinition $src
$p = Get-Process FiveM_b3751_GTAProcess -ErrorAction Stop
$fs = [System.IO.File]::Create($Out)
# MiniDumpWithThreadInfo (0x1000) | MiniDumpWithIndirectlyReferencedMemory (0x40) | MiniDumpNormal
$type = 0x1000 -bor 0x40
$ok = [MD]::MiniDumpWriteDump($p.Handle, [uint32]$p.Id, $fs.SafeFileHandle.DangerousGetHandle(), $type, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
$fs.Close()
"MiniDumpWriteDump ok=$ok err=$err size=$((Get-Item $Out).Length)"
