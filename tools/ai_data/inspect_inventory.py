import json
import sys
from db import ROOT, rows

sys.stdout.reconfigure(encoding='utf-8')

queries = {
    'schema': "SELECT JSON_OBJECT('table',TABLE_NAME,'column',COLUMN_NAME,'type',COLUMN_TYPE,'default',COLUMN_DEFAULT,'extra',EXTRA) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('equipment','equipment_catalog','audit_log') ORDER BY TABLE_NAME,ORDINAL_POSITION",
    'catalog': "SELECT JSON_OBJECT('id',id,'name',name,'category',category,'scope',usage_scope,'tracking',tracking_mode,'code',code,'enabled',enabled) FROM equipment_catalog ORDER BY id",
    'equipment': "SELECT JSON_OBJECT('id',id,'name',name,'room',room_id,'quantity',quantity,'condition',`condition`,'scope',usage_scope,'catalog_id',catalog_id,'catalog_code',catalog_code,'asset_code',asset_code,'tracking',tracking_mode,'note',note,'version',version) FROM equipment ORDER BY id",
    'rooms': "SELECT JSON_OBJECT('id',id,'name',name,'location_type',location_type) FROM room WHERE id IN ('DTD201','EAUT101','EAUT105')"
}
result = {k: rows(v) for k,v in queries.items()}
out = ROOT / '.codex-temp/ai-data/inventory'
out.mkdir(parents=True,exist_ok=True)
(out/'before.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
