$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Frontend\src\views\client\HomeView.vue'
$source = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
$start = $source.IndexOf('          <p class="mb-3 text-sm font-bold uppercase tracking-wide text-slate-400">')
$end = $source.IndexOf('          <template v-if="activeBooking', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Layout anchors missing' }
$block = $source.Substring($start, $end - $start)
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$fixed = $strictUtf8.GetString([Text.Encoding]::GetEncoding(1252).GetBytes($block))
$source = $source.Substring(0, $start) + $fixed + $source.Substring($end)
[IO.File]::WriteAllText($path, $source, [Text.UTF8Encoding]::new($false))
