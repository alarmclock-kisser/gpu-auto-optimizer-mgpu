param(
	[ValidateRange(1, 600)]
	[int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class OptimizerExitRequest
{
	public delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

	[DllImport("user32.dll")]
	public static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

	[DllImport("user32.dll")]
	public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

	[DllImport("user32.dll", CharSet = CharSet.Unicode)]
	public static extern int GetClassName(IntPtr window, StringBuilder className, int maxCount);

	[DllImport("user32.dll", EntryPoint = "PostMessageW", SetLastError = true)]
	public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

	public static IntPtr FindApplicationWindow(uint targetProcessId)
	{
		IntPtr found = IntPtr.Zero;
		EnumWindows((window, parameter) =>
		{
			uint processId;
			GetWindowThreadProcessId(window, out processId);
			if (processId != targetProcessId)
				return true;

			var className = new StringBuilder(256);
			if (GetClassName(window, className, className.Capacity) > 0 &&
				className.ToString() == "GpuAutoOptimizerWindow")
			{
				found = window;
				return false;
			}
			return true;
		}, IntPtr.Zero);
		return found;
	}
}
'@

$processFilter = "Name = 'GpuAutoOptimizer.exe' OR Name = 'gao.exe'"
$guiProcesses = @(Get-CimInstance -ClassName Win32_Process -Filter $processFilter |
	Where-Object { $_.Name -eq 'GpuAutoOptimizer.exe' })

foreach ($process in $guiProcesses) {
	$window = [OptimizerExitRequest]::FindApplicationWindow([uint32]$process.ProcessId)
	if ($window -eq [IntPtr]::Zero) {
		Write-Warning "No GUI window found for process $($process.ProcessId); waiting for it to exit naturally."
		continue
	}

	# WM_COMMAND / kMenuExit: follow the same cleanup path as Tray > Exit.
	if ([OptimizerExitRequest]::PostMessage($window, 0x0111, [IntPtr]4, [IntPtr]::Zero)) {
		Write-Host "Requested a clean tray Exit for GpuAutoOptimizer.exe PID $($process.ProcessId)."
	} else {
		$win32Error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
		Write-Warning "Could not request Exit for PID $($process.ProcessId) (Win32 error $win32Error); waiting without force-killing."
	}
}

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while ($true) {
	$remaining = @(Get-CimInstance -ClassName Win32_Process -Filter $processFilter)
	if ($remaining.Count -eq 0) {
		Write-Host 'All GPU Auto Optimizer processes have exited.'
		exit 0
	}

	if ([DateTime]::UtcNow -ge $deadline) {
		foreach ($process in $remaining) {
			Write-Host "ERROR: Process $($process.Name) PID $($process.ProcessId) is still running. No force-kill was attempted."
		}
		exit 1
	}

	Start-Sleep -Seconds 1
}
