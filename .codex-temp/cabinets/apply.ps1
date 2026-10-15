$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Frontend\src\views\client\HomeView.vue'
$source = [IO.File]::ReadAllText($path)
$old = '  return groups'
$new = @'
  return Object.fromEntries(Object.entries(groups).map(([building, floors]) => [
    building,
    Object.fromEntries(Object.entries(floors).sort(([a], [b]) => a.localeCompare(b, 'vi', { numeric: true })))
  ]))
'@
foreach ($anchor in @($old, 'flex flex-col-reverse gap-8', 'h-28 sm:h-32')) {
    if (-not $source.Contains($anchor)) { throw "Missing anchor: $anchor" }
}
$source = $source.Replace($old, $new).Replace('flex flex-col-reverse gap-8', 'flex flex-col gap-8').Replace('h-28 sm:h-32', 'h-32 sm:h-36')
[IO.File]::WriteAllText($path, $source, [Text.UTF8Encoding]::new($false))
