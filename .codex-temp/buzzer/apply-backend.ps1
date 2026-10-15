$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Backend\ClassHub-API\ClassHub-API\Services\QrAccessService.cs'
$source = [IO.File]::ReadAllText($path)
$anchor = '        return new { sessionId = id, roomId = session.ma_phong, roomName = room!.ten_phong, purpose = qr.Purpose, expiresAt = qr.ExpiresAt };'
$replacement = @'
        await tx.CommitAsync();
        try {
            await mqtt.PublishAsync($"backend/cabinet/{session.ma_phong}/action", JsonSerializer.Serialize(new {
                room = session.ma_phong, sessionId = id, action = "qr_scanned", purpose = qr.Purpose, expiresAt = qr.ExpiresAt }));
        } catch (Exception ex) {
            // Audible feedback is optional; a valid scan still permits confirmation.
            System.Diagnostics.Trace.TraceWarning("QR scan feedback delivery failed for session {0}: {1}", id, ex.Message);
        }
        return new { sessionId = id, roomId = session.ma_phong, roomName = room!.ten_phong, purpose = qr.Purpose, expiresAt = qr.ExpiresAt };
'@
if (-not $source.Contains($anchor)) { throw 'Preview anchor missing' }
[IO.File]::WriteAllText($path, $source.Replace($anchor, $replacement), [Text.UTF8Encoding]::new($false))
