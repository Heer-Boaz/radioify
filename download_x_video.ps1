param(
    [Parameter(Mandatory=$true)]
    [string]$VideoUrl,

    [Parameter(Mandatory=$true)]
    [string]$AudioUrl,

    [Parameter(Mandatory=$false)]
    [string]$OutputFile = "merged_output.mp4"
)

# Bestandsnamen
$videoFile = "video.mp4"
$audioFile = "audio.m4a"

Write-Host "🎥 Downloaden van video..."
ffmpeg -i $VideoUrl -c copy $videoFile

Write-Host "🔊 Downloaden van audio..."
ffmpeg -i $AudioUrl -c copy $audioFile

Write-Host "🔧 Samenvoegen..."
ffmpeg -i $videoFile -i $audioFile -c copy $OutputFile

Write-Host "✅ Klaar! Je bestand staat in $OutputFile"

#.\download_x_video.ps1 `
#  -VideoUrl "https://video.twimg.com/amplify_video/2039766386189217792/pl/avc1/640x640/Tz4GQtY83hkqHE6F.m3u8" `
#  -AudioUrl "https://video.twimg.com/amplify_video/2039766386189217792/pl/mp4a/128000/_J0CZHa303ifDkva.m3u8" `
#  -OutputFile "britain_fix.mp4"
