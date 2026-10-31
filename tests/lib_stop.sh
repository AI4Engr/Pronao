# Shared by the real-window tests (Windows). stop_program PATH ends the processes that run exactly this file: the test program a story started and left running when it
# failed. `kill $pid` is not enough: $pid is the shell that runs `timeout`, and the program under it would live on, holding a window and a tray icon that the next run then finds.
stop_program() {
    powershell -NoProfile -Command "Get-Process | Where-Object { \$_.Path -eq '$(cygpath -w "$1")' } | Stop-Process -Force" > /dev/null 2>&1
    return 0
}
