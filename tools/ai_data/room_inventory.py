"""Import an explicitly unverified room template for the three AI pilot rooms."""
import argparse
from datetime import datetime, timedelta, timezone
import json
import sys
import uuid

from db import ROOT, literal as q, rows, run
from prepare import canonical, insert

sys.stdout.reconfigure(encoding='utf-8')
OUT=ROOT/'.codex-temp/ai-data/inventory'
ROOMS=['DTD201','EAUT101','EAUT105']
BATCH=str(uuid.uuid5(uuid.NAMESPACE_URL,'classhub/room-inventory/photo-baseline/v1/'+','.join(ROOMS)))
NOTE='MOCK | Cấu hình mẫu theo ảnh + TV TCL; số lượng và tình trạng chưa kiểm kê tại phòng. Công suất và nhánh đo chưa xác định.'
# key, name, count, unit, category, manufacturer, electrical load
ITEMS=[
    ('projector','Máy chiếu Sony',1,'bộ','Thiết bị trình chiếu','Sony',True),
    ('board','Bảng viết phấn',1,'cái','Nội thất',None,False),
    ('camera','Camera Hikvision',1,'cái','Thiết bị mạng và an ninh','Hikvision',True),
    ('ac','Điều hòa Casper',1,'cái','Điều hòa và thông gió','Casper',True),
    ('teacher_chair','Ghế inox chân quỳ lưới đen',1,'cái','Nội thất',None,False),
    ('sino_lights','Đèn Sino',10,'bộ','Chiếu sáng','Sino',True),
    ('teacher_desk','Bàn giảng viên',1,'cái','Nội thất',None,False),
    ('folding_chair','Ghế gấp inox xanh',76,'cái','Nội thất',None,False),
    ('student_desk','Bàn gỗ 0,5 m x 1,2 m',38,'cái','Nội thất',None,False),
    ('standing_fan','Quạt cây Midea',1,'cái','Điều hòa và thông gió','Midea',True),
    ('wifi','Bộ phát Wi-Fi Grandstream',1,'cái','Thiết bị mạng và an ninh','Grandstream',True),
    ('ceiling_fan','Quạt trần Nanoco',2,'cái','Điều hòa và thông gió','Nanoco',True),
    ('led_lights','Đèn LED 1,2 m bán nguyệt',3,'cái','Chiếu sáng',None,True),
    ('tv','TV TCL 4K',1,'cái','Thiết bị trình chiếu','TCL',True),
]

SCHEMA='''CREATE TABLE IF NOT EXISTS room_equipment_profile (
 equipment_id INT NOT NULL PRIMARY KEY,
 batch_id CHAR(36) NOT NULL,
 item_key VARCHAR(40) NOT NULL,
 manufacturer VARCHAR(80) NULL,
 model VARCHAR(100) NULL,
 rated_input_w DECIMAL(12,3) NULL,
 is_electrical TINYINT(1) NOT NULL,
 measurement_branch VARCHAR(100) NULL,
 measurement_confirmed TINYINT(1) NOT NULL DEFAULT 0,
 inventory_source VARCHAR(40) NOT NULL,
 data_source VARCHAR(10) NOT NULL,
 properties_json LONGTEXT NOT NULL CHECK (JSON_VALID(properties_json)),
 UNIQUE KEY uq_profile_batch_item (batch_id,equipment_id),
 FOREIGN KEY (equipment_id) REFERENCES equipment(id),
 FOREIGN KEY (batch_id) REFERENCES data_batch(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;'''


def code(key):
    return str(uuid.uuid5(uuid.NAMESPACE_URL,'classhub/room-catalog/photo-baseline/v1/'+key))


def check():
    actual=rows(f'''SELECT JSON_OBJECT('equipment_id',e.id,'room_id',e.room_id,'name',e.name,
      'quantity',e.quantity,'scope',e.usage_scope,'tracking',e.tracking_mode,'condition',e.`condition`,
      'note',e.note,'catalog_id',e.catalog_id,'catalog_scope',c.usage_scope,'catalog_code',e.catalog_code,
      'code',c.code,'item_key',p.item_key,'source',p.data_source,'inventory_source',p.inventory_source,
      'manufacturer',p.manufacturer,'model',p.model,'rated_input_w',p.rated_input_w,
      'is_electrical',p.is_electrical,'branch',p.measurement_branch,'measurement_confirmed',p.measurement_confirmed,
      'properties',p.properties_json)
      FROM room_equipment_profile p JOIN equipment e ON e.id=p.equipment_id
      JOIN equipment_catalog c ON c.id=e.catalog_id WHERE p.batch_id={q(BATCH)} ORDER BY e.room_id,p.item_key''')
    assert len(actual)==42
    expected={key:(name,count,brand,electric) for key,name,count,unit,category,brand,electric in ITEMS}
    assert {(a['room_id'],a['item_key']) for a in actual}=={(r,i[0]) for r in ROOMS for i in ITEMS}
    for a in actual:
        name,count,brand,electric=expected[a['item_key']]
        assert (a['name'],a['quantity'],a['manufacturer'],a['is_electrical'])==(name,count,brand,int(electric))
        assert a['scope']==a['catalog_scope']=='ROOM' and a['tracking']=='BULK'
        assert a['source']=='MOCK' and a['inventory_source']=='TEMPLATE_UNVERIFIED'
        assert a['note']==NOTE and a['code']==a['catalog_code']
        assert a['rated_input_w'] is None and a['model'] is None and a['branch'] is None and a['measurement_confirmed']==0
        props=json.loads(a['properties'])
        assert props['condition_verified'] is False and props['quantity_verified_in_room'] is False
        if a['item_key']=='tv':
            assert props['screen_inches_estimate']==80 and props['screen_inches_confirmed'] is False
    for room in ROOMS:
        room_rows=[a for a in actual if a['room_id']==room]
        assert len(room_rows)==14 and sum(a['quantity'] for a in room_rows)==138
    return actual


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--apply',action='store_true'); args=ap.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    table=rows("SELECT JSON_OBJECT('table',TABLE_NAME) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='room_equipment_profile'")
    existing=rows(f"SELECT JSON_OBJECT('id',id) FROM data_batch WHERE id={q(BATCH)}")
    if existing:
        if not table: raise RuntimeError('Batch exists but profile table is missing')
        actual=check()
        print(json.dumps(dict(status='ALREADY_APPLIED',batch_id=BATCH,rooms=ROOMS,rows=len(actual)),ensure_ascii=False,indent=2))
        return
    if table:
        # Do not silently accept a different pre-existing schema.
        raise RuntimeError('room_equipment_profile already exists; inspect before importing a new batch')
    before=rows("SELECT JSON_OBJECT('id',id,'name',name,'room',room_id,'quantity',quantity,'condition',`condition`,'scope',usage_scope,'catalog_id',catalog_id,'catalog_code',catalog_code,'asset_code',asset_code,'tracking',tracking_mode,'note',note,'version',version) FROM equipment ORDER BY id")
    catalogs=rows("SELECT JSON_OBJECT('id',id,'name',name,'category',category,'scope',usage_scope,'tracking',tracking_mode,'code',code,'enabled',enabled,'version',version) FROM equipment_catalog ORDER BY id")
    present=rows("SELECT JSON_OBJECT('id',id,'location_type',location_type) FROM room WHERE id IN ("+','.join(q(r) for r in ROOMS)+") ORDER BY id")
    assert {r['id'] for r in present}==set(ROOMS) and all(r['location_type']=='ROOM' for r in present)
    assert all(c['name'] not in [i[1] for i in ITEMS] or c['scope']!='ROOM' for c in catalogs),'Matching ROOM catalog exists; reconcile before adding'
    protected=rows("SELECT JSON_OBJECT('sessions',(SELECT COUNT(*) FROM borrowing_session),'borrowed_items',(SELECT COUNT(*) FROM borrowing_equipment),'samples',(SELECT COUNT(*) FROM iot_sensor),'cabinet_count',(SELECT COUNT(*) FROM cabinet),'node_count',(SELECT COUNT(*) FROM iot_node))")
    baseline=dict(equipment=before,catalogs=catalogs,protected_counts=protected)
    (OUT/'import-before.json').write_text(json.dumps(baseline,ensure_ascii=False,indent=2),encoding='utf-8')
    now=datetime.now(timezone.utc).replace(tzinfo=None)
    params=dict(rooms=ROOMS,inventory_source='TEMPLATE_UNVERIFIED',reference='User-supplied room inventory photo + user-requested TCL 4K TV',
                reference_room_confirmed=False,counts_applied_as_template=True,powers_known=False,existing_pilot_samples_recalculated=False,
                condition_assumed='TOT; not inspected',catalog_codes={i[0]:code(i[0]) for i in ITEMS})
    batch=dict(id=BATCH,name='Room inventory baseline: photo + TCL TV',data_source='MOCK',generator_version='inventory-1.0',seed=0,
               starts_at_utc=now.isoformat(sep=' ',timespec='milliseconds'),ends_at_utc=(now+timedelta(milliseconds=1)).isoformat(sep=' ',timespec='milliseconds'),
               parameters_json=canonical(params),created_at_utc=now.isoformat(sep=' ',timespec='milliseconds'))
    sql="SET time_zone='+00:00';\nSTART TRANSACTION;\n"+insert('data_batch',list(batch),[batch])
    for key,name,count,unit,category,brand,electric in ITEMS:
        catalog=dict(name=name,category=category,tracking_mode='BULK',usage_scope='ROOM',enabled=1,version=1,code=code(key))
        sql+=insert('equipment_catalog',list(catalog),[catalog])+f"SET @catalog_{key}=LAST_INSERT_ID();\n"
    manifest=[]
    for room in ROOMS:
        for key,name,count,unit,category,brand,electric in ITEMS:
            columns=['name','quantity','condition','note','room_id','category','usage_scope','tracking_mode','asset_code','catalog_code','version','catalog_id']
            values=[q(name),str(count),q('TOT'),q(NOTE),q(room),q(category),q('ROOM'),q('BULK'),'NULL',q(code(key)),'1',f'@catalog_{key}']
            sql+='INSERT INTO equipment ('+','.join('`'+c+'`' for c in columns)+') VALUES ('+','.join(values)+');\nSET @equipment_id=LAST_INSERT_ID();\n'
            properties=dict(unit=unit,quantity_verified_in_room=False,condition_verified=False,source_reference='photo template',
                            historical_installation_unknown=True,measurement_scope='UNCONFIRMED')
            if key=='tv': properties.update(screen_inches_estimate=80,screen_inches_confirmed=False,resolution='4K',source_reference='user addition')
            if key=='sino_lights': properties['lamp_description']='Unreadable subtype in photo; number of bulbs per set unknown'
            cols=['equipment_id','batch_id','item_key','manufacturer','model','rated_input_w','is_electrical','measurement_branch','measurement_confirmed','inventory_source','data_source','properties_json']
            vals=['@equipment_id',q(BATCH),q(key),q(brand),'NULL','NULL',str(int(electric)),'NULL','0',q('TEMPLATE_UNVERIFIED'),q('MOCK'),q(canonical(properties))]
            sql+='INSERT INTO room_equipment_profile ('+','.join(cols)+') VALUES ('+','.join(vals)+');\n'
            detail=canonical(dict(batchId=BATCH,assetName=name,toRoomId=room,quantity=count,source='MOCK',inventorySource='TEMPLATE_UNVERIFIED',
                                  note='Initial assumed inventory import, not a physical stock transfer or confirmed receipt'))
            sql+=f"INSERT INTO audit_log (account_id,action,details,user_agent) VALUES (NULL,'ASSET_BASELINE_IMPORT',{q(detail)},'ClassHub Inventory Import');\n"
            manifest.append(dict(room_id=room,item_key=key,name=name,quantity=count,unit=unit,is_electrical=electric,manufacturer=brand,
                                 model=None,rated_input_w=None,source='MOCK',inventory_source='TEMPLATE_UNVERIFIED',properties=properties))
    sql+='COMMIT;\n'
    (OUT/'import.sql').write_text(sql,encoding='utf-8')
    (OUT/'template.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    summary=dict(status='PREPARED',batch_id=BATCH,rooms=ROOMS,types_per_room=14,rows=len(manifest),quantity_per_room=138,
                 catalog_rows_added=14,data_source='MOCK',power_values='UNKNOWN',measurement_scope='UNCONFIRMED')
    if args.apply:
        run(SCHEMA)
        run(sql)
        actual=check()
        ids=','.join(str(e['id']) for e in before)
        original=rows("SELECT JSON_OBJECT('id',id,'name',name,'room',room_id,'quantity',quantity,'condition',`condition`,'scope',usage_scope,'catalog_id',catalog_id,'catalog_code',catalog_code,'asset_code',asset_code,'tracking',tracking_mode,'note',note,'version',version) FROM equipment WHERE id IN ("+ids+") ORDER BY id") if ids else []
        assert before==original,'Old equipment changed'
        old_catalog_ids=','.join(str(c['id']) for c in catalogs)
        original_catalogs=rows("SELECT JSON_OBJECT('id',id,'name',name,'category',category,'scope',usage_scope,'tracking',tracking_mode,'code',code,'enabled',enabled,'version',version) FROM equipment_catalog WHERE id IN ("+old_catalog_ids+") ORDER BY id") if old_catalog_ids else []
        assert catalogs==original_catalogs,'Old catalogs changed'
        current_protected=rows("SELECT JSON_OBJECT('sessions',(SELECT COUNT(*) FROM borrowing_session),'borrowed_items',(SELECT COUNT(*) FROM borrowing_equipment),'samples',(SELECT COUNT(*) FROM iot_sensor),'cabinet_count',(SELECT COUNT(*) FROM cabinet),'node_count',(SELECT COUNT(*) FROM iot_node))")
        assert current_protected==protected,'Unrelated table counts changed'
        assert rows(f"SELECT JSON_OBJECT('count',COUNT(*)) FROM audit_log WHERE action='ASSET_BASELINE_IMPORT' AND JSON_UNQUOTE(JSON_EXTRACT(details,'$.batchId'))={q(BATCH)}")[0]['count']==42
        summary.update(status='APPLIED_AND_VERIFIED',old_equipment_preserved=len(before),old_catalogs_preserved=len(catalogs),
                       protected_counts='UNCHANGED',audit_entries=42)
        (OUT/'stored-inventory.json').write_text(json.dumps(actual,ensure_ascii=False,indent=2),encoding='utf-8')
        # Exact-ID cleanup is intentionally an artifact, not executed here. Old
        # catalogs cannot be removed because only newly inserted IDs are included.
        new_ids=','.join(str(a['equipment_id']) for a in actual)
        cat_ids=','.join(str(i) for i in sorted({a['catalog_id'] for a in actual}))
        audit_ids=rows(f"SELECT JSON_OBJECT('id',id) FROM audit_log WHERE action='ASSET_BASELINE_IMPORT' AND JSON_UNQUOTE(JSON_EXTRACT(details,'$.batchId'))={q(BATCH)} ORDER BY id")
        audits=','.join(str(a['id']) for a in audit_ids)
        cleanup=f'''-- Remove only this inventory import after reviewing any subsequent asset changes.
START TRANSACTION;
DELETE FROM room_equipment_profile WHERE batch_id={q(BATCH)};
DELETE FROM equipment WHERE id IN ({new_ids}) AND usage_scope='ROOM' AND version=1 AND note={q(NOTE)};
DELETE FROM equipment_catalog WHERE id IN ({cat_ids}) AND usage_scope='ROOM' AND version=1;
DELETE FROM audit_log WHERE id IN ({audits}) AND action='ASSET_BASELINE_IMPORT';
DELETE FROM data_batch WHERE id={q(BATCH)};
COMMIT;
'''
        (OUT/'rollback.sql').write_text(cleanup,encoding='utf-8')
    (OUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(summary,ensure_ascii=False,indent=2))


if __name__=='__main__': main()
