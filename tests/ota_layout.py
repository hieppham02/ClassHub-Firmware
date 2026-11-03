"""Check migration preserves stored data and that OTA slots fit the physical flash."""
from pathlib import Path
rows={}
for line in Path('partitions.csv').read_text().splitlines():
    if not line.strip() or line.lstrip().startswith('#'):continue
    name,kind,subtype,offset,size,*_=map(str.strip,line.split(','))
    rows[name]=(kind,subtype,int(offset,0),int(size,0))
assert rows['nvs']==('data','nvs',0x9000,0x6000)
assert rows['phy_init']==('data','phy',0xf000,0x1000)
assert rows['factory']==('app','factory',0x10000,0x1f0000)
assert rows['otadata'][0:2]==('data','ota') and rows['otadata'][3]==0x2000
last_end=0x9000
for name,(kind,subtype,start,size) in sorted(rows.items(),key=lambda item:item[1][2]):
    assert start>=last_end and start+size<=16*1024*1024,(name,'overlap or flash overflow')
    assert start%(0x10000 if kind=='app' else 0x1000)==0,name
    last_end=start+size
assert rows['ota_0'][3]==rows['ota_1'][3]==0x300000
for name in ('sdkconfig.defaults','sdkconfig.esp32-s3-devkitc-1'):
    assert 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' in Path(name).read_text()
print('PASS: OTA slots, rollback configuration and preserved NVS/factory layout')
