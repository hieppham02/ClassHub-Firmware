"""Apply a prepared, validated batch. No changes to existing business rows."""
import argparse
import hashlib
import json
from pathlib import Path

from db import literal, rows, run
from prepare import OUT, canonical
from verify import load_and_verify


def protected():
    # No accounts, credentials, or borrower identities are exported.
    return dict(
        counts=rows("SELECT JSON_OBJECT('sessions',(SELECT COUNT(*) FROM borrowing_session),'accounts',(SELECT COUNT(*) FROM account),'equipment',(SELECT COUNT(*) FROM equipment),'cabinets',(SELECT COUNT(*) FROM cabinet),'nodes',(SELECT COUNT(*) FROM iot_node))"),
        nodes=rows("SELECT JSON_OBJECT('id',id,'code',node_code,'room',room_id,'source',data_source,'status',status,'last',last_seen_at,'interval',publish_interval_seconds) FROM iot_node ORDER BY id"),
        cabinets=rows("SELECT JSON_OBJECT('id',id,'room',room_id,'online',is_online,'lock',lock_state,'last',last_seen_at) FROM cabinet ORDER BY id")
    )


def verify_db(data):
    batch=data['batch']['id']; b=literal(batch)
    counts=rows(f"""SELECT JSON_OBJECT('samples',(SELECT COUNT(*) FROM data_batch_sensor WHERE batch_id={b}),
      'events',(SELECT COUNT(*) FROM cabinet_event WHERE batch_id={b}),
      'contexts',(SELECT COUNT(*) FROM operation_context WHERE batch_id={b}),
      'anomalies',(SELECT COUNT(*) FROM anomaly_event WHERE batch_id={b}),
      'non_mock',(SELECT COUNT(*) FROM iot_sensor s JOIN data_batch_sensor m ON m.sensor_id=s.id WHERE m.batch_id={b} AND s.data_source<>'MOCK'),
      'invalid_json',(SELECT COUNT(*) FROM iot_sensor s JOIN data_batch_sensor m ON m.sensor_id=s.id WHERE m.batch_id={b} AND NOT JSON_VALID(s.readings_json)))""")[0]
    assert counts==dict(samples=len(data['samples']),events=len(data['events']),contexts=len(data['contexts']),
                        anomalies=len(data['anomalies']),non_mock=0,invalid_json=0), counts
    # Validate DB content, not just row counts.
    sensor_rows=rows(f"""SELECT JSON_OBJECT('node_id',s.node_id,'boot_id',s.boot_id,'sequence',s.sequence,
      'measured_at',DATE_FORMAT(s.measured_at,'%Y-%m-%d %H:%i:%s.%f'),
      'received_at',DATE_FORMAT(s.received_at,'%Y-%m-%d %H:%i:%s.%f'),
      'clock_synced',s.clock_synced,'data_source',s.data_source,'readings_json',s.readings_json)
      FROM iot_sensor s JOIN data_batch_sensor m ON m.sensor_id=s.id WHERE m.batch_id={b}
      ORDER BY s.node_id,s.sequence""")
    for s in sensor_rows:
        s['measured_at']=s['measured_at'][:-3]; s['received_at']=s['received_at'][:-3]
    wanted=sorted(data['samples'],key=lambda s:(s['node_id'],s['sequence']))
    assert canonical(sensor_rows)==canonical(wanted),'Stored samples differ from prepared dataset'
    for table,key in [('cabinet_event','events'),('operation_context','contexts'),('anomaly_event','anomalies')]:
        columns=list(data[key][0])
        expressions=[]
        for c in columns:
            expr=f"DATE_FORMAT(`{c}`,'%Y-%m-%d %H:%i:%s.%f')" if c.endswith('_utc') else '`'+c+'`'
            expressions.extend([literal(c),expr])
        actual=rows(f"SELECT JSON_OBJECT({','.join(expressions)}) FROM {table} WHERE batch_id={b} ORDER BY id")
        for record in actual:
            for c in columns:
                if c.endswith('_utc'): record[c]=record[c][:-3]
        assert canonical(actual)==canonical(sorted(data[key],key=lambda x:x['id'])),table+' content mismatch'
    return counts


def finish(data, counts, before):
    after=protected()
    assert {k:v for k,v in before.items() if k!='original_sensors'}==after,'Business rows/node state changed during import'
    originals=before['original_sensors']
    ids=','.join(str(s['id']) for s in originals)
    original_after=rows(f"SELECT JSON_OBJECT('id',id,'node_id',node_id,'boot_id',boot_id,'sequence',sequence,'measured',measured_at,'received',received_at,'source',data_source,'readings',readings_json,'synced',clock_synced) FROM iot_sensor WHERE id IN ({ids}) ORDER BY id") if ids else []
    assert original_after==originals,'Original telemetry changed'
    result=dict(status='APPLIED_AND_VERIFIED',batch_id=data['batch']['id'],counts=counts,existing_telemetry_preserved=len(originals),
                existing_business_counts_and_node_cabinet_state='UNCHANGED',
                sample_sha256=hashlib.sha256(canonical(data['samples']).encode()).hexdigest())
    (OUT/'database-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))


def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--apply',action='store_true'); args=parser.parse_args()
    data,_,_=load_and_verify()
    batch=data['batch']['id']
    validation=json.loads((OUT/'validation.json').read_text())
    assert validation['status']=='PASS'
    names={r['table'] for r in rows("SELECT JSON_OBJECT('table',TABLE_NAME) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE()")}
    new_tables={'data_batch','data_batch_sensor','cabinet_event','operation_context','anomaly_event'}
    if new_tables & names:
        if new_tables <= names:
            found=rows(f"SELECT JSON_OBJECT('id',id,'parameters',parameters_json) FROM data_batch WHERE id={literal(batch)}")
            if found:
                assert found[0]['parameters']==data['batch']['parameters_json']
                counts=verify_db(data)
                if (OUT/'before.json').exists() and not (OUT/'database-result.json').exists():
                    finish(data,counts,json.loads((OUT/'before.json').read_text(encoding='utf-8')))
                print(json.dumps(dict(status='ALREADY_APPLIED',batch_id=batch,counts=counts),indent=2)); return
        raise RuntimeError('History tables already exist without this batch. Inspect schema before continuing.')
    before=protected()
    originals=rows("SELECT JSON_OBJECT('id',id,'node_id',node_id,'boot_id',boot_id,'sequence',sequence,'measured',measured_at,'received',received_at,'source',data_source,'readings',readings_json,'synced',clock_synced) FROM iot_sensor ORDER BY id")
    source_ids=','.join(str(s['id']) for s in data['parameters']['source_sessions'])
    current_source=rows(f"SELECT JSON_OBJECT('id',id,'room_id',room_id,'slot_id',slot_id,'received_at',received_at,'returned_at',returned_at,'status',status) FROM borrowing_session WHERE id IN ({source_ids}) ORDER BY id")
    assert canonical(current_source)==canonical(data['parameters']['source_sessions']), 'Source sessions changed'
    boots=','.join(literal(b) for b in data['parameters']['boot_ids'].values())
    assert rows(f"SELECT JSON_OBJECT('count',COUNT(*)) FROM iot_sensor WHERE boot_id IN ({boots})")[0]['count']==0
    before['original_sensors']=originals
    (OUT/'before.json').write_text(json.dumps(before,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(status='READY',batch_id=batch,samples=len(data['samples']),existing_samples=len(originals)),indent=2))
    if not args.apply:
        return
    # DDL is additive and commits separately in MariaDB. If a later DML statement
    # fails, the CLI disconnect rolls back the entire new batch; empty tables remain.
    run((Path(__file__).parent/'schema.sql').read_text(encoding='utf-8'))
    run((OUT/'insert.sql').read_text(encoding='utf-8'))
    counts=verify_db(data)
    finish(data,counts,before)


if __name__=='__main__':
    main()
