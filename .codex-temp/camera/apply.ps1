$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Frontend\src\components\shared\QrAccessPanel.vue'
$source = [IO.File]::ReadAllText($path)
$old = '    else controls = started'
$new = @'
    else {
      controls = started
      const track = video.value?.srcObject?.getVideoTracks()[0]
      const capabilities = track?.getCapabilities?.() || {}
      const preferences = []
      if (capabilities.focusMode?.includes('continuous')) preferences.push({ focusMode: 'continuous' })
      if (capabilities.exposureMode?.includes('continuous')) preferences.push({ exposureMode: 'continuous' })
      if (capabilities.torch) preferences.push({ torch: false })
      for (const preference of preferences) {
        if (current !== generation) break
        try {
          await track.applyConstraints({ advanced: [preference] })
        } catch {
          // Optional camera settings must not interrupt QR scanning.
        }
      }
    }
'@
if (-not $source.Contains($old)) { throw 'Camera setup anchor missing; no changes applied.' }
[IO.File]::WriteAllText($path, $source.Replace($old, $new), [Text.UTF8Encoding]::new($false))
