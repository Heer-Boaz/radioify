$env:Path = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;' + $env:Path
Set-Location 'B:\radioify'
.\build.ps1 -Config Release -Rebuild
