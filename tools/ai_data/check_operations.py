"""Verify repeatability and rehearse batch removal inside a rolled-back transaction."""
import contextlib
import io
import json

import prepare
from apply import verify_db
from db import literal, run
from prepare import OUT, canonical
from verify import load_and_verify


def main():
    before=json.loads((OUT/'dataset.json').read_text(encoding='utf-8'))
    # A creation timestamp is an audit field, not generated measurement content.
    with contextlib.redirect_stdout(io.StringIO()):
        prepare.main()
    after=json.loads((OUT/'dataset.json').read_text(encoding='utf-8'))
    before['batch'].pop('created_at_utc'); after['batch'].pop('created_at_utc')
    assert canonical(before)==canonical(after),'Same seed/source did not reproduce data'
    data,_,_=load_and_verify()
    verify_db(data)
    batch=literal(data['batch']['id'])
    rollback=(OUT/'rollback.sql').read_text(encoding='utf-8')
    check=f"""SELECT JSON_OBJECT(
      'batch',(SELECT COUNT(*) FROM data_batch WHERE id={batch}),
      'samples',(SELECT COUNT(*) FROM data_batch_sensor WHERE batch_id={batch}),
      'events',(SELECT COUNT(*) FROM cabinet_event WHERE batch_id={batch}),
      'contexts',(SELECT COUNT(*) FROM operation_context WHERE batch_id={batch}),
      'anomalies',(SELECT COUNT(*) FROM anomaly_event WHERE batch_id={batch}),
      'remaining_telemetry',(SELECT COUNT(*) FROM iot_sensor));
ROLLBACK;
"""
    assert rollback.count('COMMIT;')==1
    result=run(rollback.replace('COMMIT;',check))
    removed=json.loads(result.strip())
    original_count=len(json.loads((OUT/'before.json').read_text(encoding='utf-8'))['original_sensors'])
    assert removed==dict(batch=0,samples=0,events=0,contexts=0,anomalies=0,remaining_telemetry=original_count),removed
    counts=verify_db(data)
    report=dict(status='PASS',reproducible=True,removal_rehearsal='ALL_BATCH_ROWS_REMOVED_WITHIN_TRANSACTION',
                rehearsal_transaction='ROLLED_BACK',batch_present_after_rehearsal=True,counts=counts,
                original_telemetry_count=original_count)
    (OUT/'operations-check.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
