$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Backend\ClassHub-API\ClassHub-API\Services\QrAccessService.cs'
$source = [IO.File]::ReadAllText($path)
$old = 'action = "qr_scanned", purpose = qr.Purpose, expiresAt = qr.ExpiresAt'
$new = 'action = "qr_scanned", purpose = qr.Purpose, expiresAt = DateTime.SpecifyKind(qr.ExpiresAt, DateTimeKind.Utc)'
if (-not $source.Contains($old)) { throw 'Scan feedback anchor missing' }
[IO.File]::WriteAllText($path, $source.Replace($old, $new), [Text.UTF8Encoding]::new($false))
