# Closes native dialogs by title, the way a user clicking the X would (WM_CLOSE); "main:<title>" does the same to a window of the
# program itself, and "no:<title>" presses the No button of a Yes/No box. Used by run_window_selftest.sh so the
# real message box and the real Open dialog can be exercised without anyone at the keyboard.
#   powershell -File close_dialogs.ps1 "<title one>" "<title two>" ...
# Exit code 0 when every title was found and closed within 25 seconds, 1 otherwise.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Titles)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class DialogCloser {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string text);
}
'@

$deadline = (Get-Date).AddSeconds(25)
$closed = @{}
while ((Get-Date) -lt $deadline -and $closed.Count -lt $Titles.Count) {
    foreach ($title in $Titles) {
        if ($closed.ContainsKey($title)) { continue }
        # "main:<title>" is a top-level window of the program itself (the X pressed by a user), anything else is a dialog
        # "no:<title>" is a Yes/No box, which has no X: press its No button instead
        $press = $null
        if ($title.StartsWith('main:')) { $h = [DialogCloser]::FindWindow([NullString]::Value, $title.Substring(5)) }
        elseif ($title.StartsWith('no:')) { $h = [DialogCloser]::FindWindow('#32770', $title.Substring(3)); $press = '&No' }
        else { $h = [DialogCloser]::FindWindow('#32770', $title) }
        if ($h -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 400          # let it finish appearing
            if ($press) {
                $b = [DialogCloser]::FindWindowEx($h, [IntPtr]::Zero, 'Button', $press)
                [void][DialogCloser]::PostMessage($b, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
            } else {
                [void][DialogCloser]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
            }
            $closed[$title] = $true
        }
    }
    Start-Sleep -Milliseconds 100
}
if ($closed.Count -eq $Titles.Count) { exit 0 }
Write-Output ("close_dialogs: only closed " + $closed.Count + " of " + $Titles.Count)
exit 1
