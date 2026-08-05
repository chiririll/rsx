param (
    [string]$projectDir
)

function Write-VSParsableErrorAndExit {
    param (
        [string]$Message,
        [int]$ErrorCode,
        [int]$Line = $MyInvocation.ScriptLineNumber
    )

    $file = $MyInvocation.ScriptName
    $formatted = "$file($Line): error PS{0:D4}: $Message" -f $ErrorCode
    [Console]::Error.WriteLine($formatted)
    exit $ExitCode
}


if (-not $projectDir) {
    Write-VSParsableErrorAndExit "Missing `projectDir` param" 1
}

function Write-UECDNFile {
    param (
        [string]$RemotePath,
        [string]$PackHash,
        [string]$OutputPath,
        [int]$FileOffset,
        [int]$FileSize,
        [int]$PackUncompressedSize,
        [string]$FileExpectedHash
    )

    if (Test-Path $OutputPath) {
        return
    }

    $Url = "https://cdn.unrealengine.com/dependencies/$RemotePath/$PackHash"
    try {
        $response = Invoke-WebRequest -Uri $Url -UseBasicParsing
    }
    catch {
        Write-VSParsableErrorAndExit "Error during request to UE CDN for $OutputPath; $_" 2
    }

    try {
        $gzipStream = New-Object System.IO.Compression.GzipStream(
            $response.RawContentStream,
            [System.IO.Compression.CompressionMode]::Decompress
        )
        $memoryStream = New-Object System.IO.MemoryStream
        $gzipStream.CopyTo($memoryStream)
        $decompressedData = $memoryStream.ToArray()
    }
    catch {
        Write-VSParsableErrorAndExit "Error during decompression for $OutputPath; $_" 3
    }
    finally {
        if ($gzipStream) { $gzipStream.Dispose() }
        if ($memoryStream) { $memoryStream.Dispose() }
    }

    if ($decompressedData.Length -ne $PackUncompressedSize) {
        Write-VSParsableErrorAndExit "Decompressed data size mismatch for $OutputPath; expected $PackUncompressedSize, got $($decompressedData.Length)" 4
    }

    $dataSlice = $decompressedData[$FileOffset..($FileOffset + $FileSize - 1)]


    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $sha256.ComputeHash($dataSlice)
    $hash = [BitConverter]::ToString($hashBytes).Replace("-", "")

    if ($hash.ToUpper() -ne $FileExpectedHash.ToUpper()) {
        Write-VSParsableErrorAndExit "SHA-256 hash mismatch for $OutputPath; got $hash" 5
    }
    
    try {
        [System.IO.File]::WriteAllBytes($OutputPath, $dataSlice)
    }
    catch {
        Write-VSParsableErrorAndExit "Error writing file to $OutputPath; $_" 6
    }
}

$OUT_PATH = Join-Path $projectDir "\thirdparty\binka\binka_ue_decode_win64_static_x64D.lib"
Write-UECDNFile "UnrealEngine-31312565" "7e2a40af2d9721f69c43e48fee940332c37cf3c4" $OUT_PATH 797396 452462 1920810 "F1CD322361BFDF25708E06CD4941DFC50D59C9C6EFAA3C87E383F51E71EB87B5"

$OUT_PATH = Join-Path $projectDir "\thirdparty\binka\binka_ue_decode_win64_static_x64.lib"
Write-UECDNFile "UnrealEngine-25887585" "9ce4a076844fff6f758111f2d38b6dbabbc2ecee" $OUT_PATH 8 149056 327040 "63A0B56048090841A1D867924B65C4273C6C929DF3A616527F0C671956C114B9"

$OUT_PATH = Join-Path $projectDir "\thirdparty\radaudio\radaudio_decoder_win64.lib"
Write-UECDNFile "UnrealEngine-40594131" "3578368bc5d11cd2e80c63f7063c2828954c1562" $OUT_PATH 1693906 330314 2024220 "F7089A7A48B304CB54D00EB2BB75FD56A30923550BA4B74BAAE6FD7697BD1807"

# tek-steamclient Windows DLL (vendored headers/lib are committed; DLL is fetched).
$tekDll = Join-Path $projectDir "thirdparty\tek-steamclient\libtek-steamclient-2.dll"
if (-not (Test-Path $tekDll)) {
    $tekZip = Join-Path $env:TEMP "tek-steamclient-2.1.5-win-x86_64.zip"
    $tekExtract = Join-Path $env:TEMP "tek-steamclient-rsx-extract"
    try {
        Invoke-WebRequest -Uri "https://github.com/teknology-hub/tek-steamclient/releases/download/v2.1.5/tek-steamclient-2.1.5-win-x86_64.zip" -OutFile $tekZip
        if (Test-Path $tekExtract) { Remove-Item $tekExtract -Recurse -Force }
        Expand-Archive -Path $tekZip -DestinationPath $tekExtract -Force
        Copy-Item (Join-Path $tekExtract "libtek-steamclient-2.dll") $tekDll -Force
    }
    catch {
        Write-VSParsableErrorAndExit "Failed to download tek-steamclient DLL; $_" 7
    }
}

exit 0