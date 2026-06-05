$env:PATH = 'C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64;' + $env:PATH
Write-Host "Temporarily prepended MSVC bin to PATH: $($env:PATH.Split(';')[0])"
# Run build script with clang disabled (explicit)
.\\build.ps1 -Static -Ninja -ClangCl:$false
