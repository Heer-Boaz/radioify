$dump = Get-ChildItem .\radioify_crash_*.dmp | Sort-Object LastWriteTime -Desc | Select-Object -First 1
$sym = "srv*C:\symbols*https://msdl.microsoft.com/download/symbols;B:\radioify\build\Release;B:\radioify\dist"
& "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -y $sym -z $dump.FullName -c ".reload /f; .ecxr; kv; q" > debug.txt