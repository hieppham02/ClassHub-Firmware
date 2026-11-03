"""Prepare a reproducible seven-day pilot; no database writes in this command."""
import csv
from datetime import datetime, timedelta, timezone
import hashlib
import json
import math
from pathlib import Path
import random
import uuid

from db import ROOT, literal, rows

OUT = ROOT / ".codex-temp/ai-data/pilot"
SEED = 20261031
VERSION = "1.0"
LOCAL = timezone(timedelta(hours=7))
UTC = timezone.utc


def dt(value):
    return datetime.fromisoformat(value).replace(tzinfo=LOCAL)


def stamp(value):
    return value.astimezone(UTC).replace(tzinfo=None).isoformat(sep=" ", timespec="milliseconds")


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def uid(value):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "classhub/ai-data/" + value))


def write_csv(name, values):
    if not values:
        return
    with (OUT / name).open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(values[0]))
        writer.writeheader()
        writer.writerows(values)


def insert(table, columns, values):
    result = []
    for i in range(0, len(values), 200):
        group = values[i:i + 200]
        result.append("INSERT INTO " + table + " (" + ",".join("`"+c+"`" for c in columns) + ") VALUES\n" +
                      ",\n".join("(" + ",".join(literal(row[c]) for c in columns) + ")" for row in group) + ";\n")
    return "".join(result)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sessions = rows("""SELECT JSON_OBJECT('id',s.id,'room_id',s.room_id,'slot_id',s.slot_id,
      'received_at',s.received_at,'returned_at',s.returned_at,'status',s.status)
      FROM borrowing_session s WHERE s.room_id IN
        (SELECT room_id FROM iot_node WHERE data_source='MOCK' AND status<>'DISABLED')
      ORDER BY s.room_id,s.received_at,s.id""")
    nodes = rows("""SELECT JSON_OBJECT('id',n.id,'room_id',n.room_id,'interval',n.publish_interval_seconds,
      'latest_sample',MAX(s.measured_at),'cabinet_id',c.id) FROM iot_node n
      JOIN cabinet c ON c.room_id=n.room_id LEFT JOIN iot_sensor s ON s.node_id=n.id
      WHERE n.data_source='MOCK' AND n.status<>'DISABLED'
      GROUP BY n.id,n.room_id,n.publish_interval_seconds,c.id ORDER BY n.id,c.id""")
    valid, excluded = [], []
    for s in sessions:
        if s['status'] != 'COMPLETED' or not s['received_at'] or not s['returned_at']:
            excluded.append({'id': s['id'], 'reason': 'not_completed_or_missing_time'})
            continue
        start, end = dt(s['received_at']), dt(s['returned_at'])
        if not timedelta(0) < end-start <= timedelta(hours=12):
            excluded.append({'id': s['id'], 'reason': 'invalid_duration'})
            continue
        valid.append(s)
    if not valid:
        raise RuntimeError('No valid source sessions')
    # Search historical weeks before each node's latest existing reading. New history
    # must not become the dashboard's latest sample. Reject overlapping room sessions.
    best = None
    earliest = min(dt(s['received_at']) for s in valid).replace(hour=0,minute=0,second=0,microsecond=0)
    latest = max(dt(s['returned_at']) for s in valid).replace(hour=0,minute=0,second=0,microsecond=0)
    cursor = earliest
    while cursor + timedelta(days=7) <= latest + timedelta(days=1):
        end = cursor + timedelta(days=7)
        candidates = []
        seen_rooms = set()
        for n in nodes:
            room = n['room_id']
            if room in seen_rooms or not n['latest_sample'] or n['interval'] != 30:
                continue
            # Existing telemetry uses UTC, borrowing datetime values use Vietnam time.
            latest_utc = datetime.fromisoformat(n['latest_sample']).replace(tzinfo=UTC)
            if end.astimezone(UTC) + timedelta(seconds=10) >= latest_utc:
                continue
            relevant = [s for s in valid if s['room_id']==room and
                        dt(s['received_at']) < end and dt(s['returned_at']) > cursor]
            if any(dt(s['received_at']) < cursor or dt(s['returned_at']) >= end for s in relevant):
                continue
            ordered = sorted(relevant,key=lambda s:s['received_at'])
            if any(dt(a['returned_at']) > dt(b['received_at']) for a,b in zip(ordered,ordered[1:])):
                continue
            if len(ordered) >= 5:
                candidates.append((len(ordered),n,ordered))
                seen_rooms.add(room)
        candidates.sort(key=lambda x:(x[0],x[1]['room_id']),reverse=True)
        if len(candidates) >= 3:
            chosen = candidates[:3]
            score = (sum(x[0] for x in chosen),cursor.isoformat())
            if best is None or score > best[0]:
                best = (score,cursor,end,chosen)
        cursor += timedelta(days=1)
    if best is None:
        raise RuntimeError('No complete nonoverlapping historical seven-day window for three nodes')
    _, start, end, chosen = best
    source = sorted([s for _,_,ss in chosen for s in ss],key=lambda s:s['id'])
    source_hash = hashlib.sha256(canonical(source).encode()).hexdigest()
    batch = uid(f'{VERSION}/{SEED}/{source_hash}')
    rng = random.Random(SEED)
    selected_nodes = [n for _,n,_ in chosen]
    contexts, events, anomalies, samples = [], [], [], []

    def ident(group,index):
        return uid(f'{batch}/{group}/{index}')

    def context(room,sid,kind,a,b,details):
        contexts.append(dict(id=ident('context',len(contexts)),batch_id=batch,room_id=room,session_id=sid,
                             context_type=kind,starts_at_utc=stamp(a),ends_at_utc=stamp(b),data_source='MOCK',details_json=canonical(details)))

    def event(n,sid,command,kind,time,details=None):
        events.append(dict(id=ident('event',len(events)),batch_id=batch,cabinet_id=n['cabinet_id'],room_id=n['room_id'],
                           session_id=sid,command_id=command,event_type=kind,occurred_at_utc=stamp(time),
                           received_at_utc=stamp(time+timedelta(milliseconds=rng.randint(30,1800))),
                           data_source='MOCK',details_json=canonical(details or {})))

    def anomaly(room,sid,kind,a,b,details):
        anomalies.append(dict(id=ident('anomaly',len(anomalies)),batch_id=batch,room_id=room,session_id=sid,
                              anomaly_type=kind,starts_at_utc=stamp(a),ends_at_utc=stamp(b),
                              label_source='GENERATOR',details_json=canonical(details)))

    profiles, boot_ids = {}, {}
    for room_index,(_,n,ss) in enumerate(chosen):
        room = n['room_id']
        ss = sorted(ss,key=lambda s:s['received_at'])
        profile = dict(idle_a=round(rng.uniform(.08,.3),3),occupied_a=round(rng.uniform(2.5,5.8),3),
                       variation_a=round(rng.uniform(.12,.35),3))
        profiles[room] = profile
        boot = ident('boot',n['id']); boot_ids[str(n['id'])] = boot
        for s in ss:
            a,b = dt(s['received_at']),dt(s['returned_at'])
            context(room,s['id'],'BORROWING',a,b,{'snapshot_source':'borrowing_session','slot_id':s['slot_id']})
        # One longer door-open episode per room; other handovers remain ordinary.
        long_sid = ss[min(1,len(ss)-1)]['id']
        for s in ss:
            for purpose,anchor in [('RECEIVE',dt(s['received_at'])),('RETURN',dt(s['returned_at']))]:
                # Events describe nearby handover, not exact observed physical truth.
                opened = anchor+timedelta(seconds=3)
                duration = rng.randint(20,90)
                if s['id']==long_sid and purpose=='RETURN':
                    duration = rng.randint(480,720)
                closed = opened+timedelta(seconds=duration)
                following = [dt(x['received_at']) for x in ss if dt(x['received_at'])>anchor]
                if closed>=end or (following and closed>=min(following)):
                    raise RuntimeError('Door episode crosses next session/window')
                command = ident('command',f"{s['id']}/{purpose}").replace('-','')
                event(n,s['id'],command,'OPEN_REQUESTED',anchor,{'purpose':purpose})
                event(n,s['id'],command,'UNLOCK_PULSE',anchor+timedelta(milliseconds=100),{'duration_ms':1500})
                event(n,s['id'],command,'DOOR_OPENED',opened)
                event(n,s['id'],command,'DOOR_CLOSED',closed)
                if duration>300:
                    anomaly(room,s['id'],'DOOR_OPEN_LONG',opened,closed,{'duration_seconds':duration})
        # Pick a real return followed by >= 90 minutes of no borrowing.
        tail = next((s for s in ss if dt(s['returned_at'])+timedelta(minutes=90)<end and
                     not any(dt(x['received_at']) < dt(s['returned_at'])+timedelta(minutes=90)
                             and dt(x['returned_at'])>dt(s['returned_at']) for x in ss if x['id']!=s['id'])),None)
        if not tail:
            raise RuntimeError('No suitable unoccupied interval for after-return episode')
        tail_start = dt(tail['returned_at'])
        tail_end = tail_start+timedelta(minutes=rng.randint(35,65))
        anomaly(room,tail['id'],'CURRENT_AFTER_RETURN',tail_start,tail_end,{'added_current_a':round(profile['occupied_a']*.7,3)})
        # Telemetry gap while a real borrowing session is active; sequence numbers
        # advance through omitted readings, rather than filling missing values with 0.
        long_session = max(ss,key=lambda s:dt(s['returned_at'])-dt(s['received_at']))
        gap_start = dt(long_session['received_at'])+timedelta(minutes=20)
        gap_end = gap_start+timedelta(minutes=12+room_index*3)
        if gap_end>=dt(long_session['returned_at']):
            raise RuntimeError('Borrowing session too short for gap')
        anomaly(room,long_session['id'],'TELEMETRY_GAP',gap_start,gap_end,{'scope':'SENSOR_NODE','expected_interval_seconds':30})
        event(n,long_session['id'],None,'NODE_OFFLINE',gap_start,{'node_id':n['id']})
        event(n,long_session['id'],None,'NODE_ONLINE',gap_end,{'node_id':n['id']})
        # A legitimate out-of-hours cleaning interval, separate from anomalies.
        cleaning_start = next((candidate for day in range(7) for hour in (19,20,21,22)
                               if (candidate := start+timedelta(days=(day+room_index+2)%7,hours=hour))
                               and not any(dt(s['received_at'])<candidate+timedelta(minutes=40)
                                           and dt(s['returned_at'])>candidate for s in ss)
                               and not (candidate<tail_end and candidate+timedelta(minutes=40)>tail_start)),None)
        if cleaning_start is None:
            raise RuntimeError('No unoccupied cleaning interval')
        cleaning_end = cleaning_start+timedelta(minutes=40)
        if any(dt(s['received_at'])<cleaning_end and dt(s['returned_at'])>cleaning_start for s in ss):
            raise RuntimeError('Cleaning overlaps borrowing')
        context(room,None,'CLEANING',cleaning_start,cleaning_end,{'authorized':True,'added_current_a':1.8})
        when,sequence = start,0
        noise = 0.0
        while when<end:
            sequence+=1
            active = next((s for s in ss if dt(s['received_at'])<=when<dt(s['returned_at'])),None)
            cleaning = cleaning_start<=when<cleaning_end
            noise = .82*noise+rng.gauss(0,profile['variation_a']*.3)
            base = profile['occupied_a'] if active else profile['idle_a']
            # Smooth within-session load variation plus room-specific noise.
            value = max(0,base+(math.sin(sequence/45)*.25 if active else 0)+noise*(1 if active else .12))
            if cleaning: value+=1.8
            if tail_start<=when<tail_end: value+=profile['occupied_a']*.7
            if not gap_start<=when<gap_end:
                reading = [dict(sensorKey='CURRENT-01',metric='current',value=round(value,3),unit='A',quality='GOOD')]
                samples.append(dict(node_id=n['id'],boot_id=boot,sequence=sequence,measured_at=stamp(when),
                                    received_at=stamp(when+timedelta(milliseconds=rng.randint(30,1800))),clock_synced=1,
                                    data_source='MOCK',readings_json=canonical(reading)))
            when+=timedelta(seconds=30)
    params = dict(timezone='Asia/Ho_Chi_Minh',telemetry_timezone='UTC',session_timezone='UTC+07:00',
                  rooms=[n['room_id'] for n in selected_nodes],nodes=selected_nodes,profiles=profiles,boot_ids=boot_ids,
                  source_sessions_sha256=source_hash,source_sessions=source,excluded_source_count=len(excluded),
                  assumptions=['current values are assumed, not physically calibrated','session history provenance is unverified',
                               'physical door events are generated, not measurements','only current in A; no inferred W or kWh',
                               'no missing-session inference outside selected context is used as ground truth occupancy'])
    metadata = dict(id=batch,name='ClassHub operational pilot: 3 rooms / 7 days',data_source='MOCK',generator_version=VERSION,
                    seed=SEED,starts_at_utc=stamp(start),ends_at_utc=stamp(end),parameters_json=canonical(params),
                    created_at_utc=stamp(datetime.now(UTC)))
    # Local validation before producing any executable database changes.
    expected = 3*7*24*120
    missing = sum(int((dt_local(a['ends_at_utc'])-dt_local(a['starts_at_utc'])).total_seconds()/30)
                  for a in anomalies if a['anomaly_type']=='TELEMETRY_GAP')
    assert len(samples)==expected-missing
    assert len({(s['node_id'],s['boot_id'],s['sequence']) for s in samples})==len(samples)
    assert all(s['received_at']>=s['measured_at'] for s in samples)
    assert len(anomalies)==9 and len(contexts)==len(source)+3
    assert {a['anomaly_type'] for a in anomalies}=={'DOOR_OPEN_LONG','CURRENT_AFTER_RETURN','TELEMETRY_GAP'}
    for sample in samples:
        value=json.loads(sample['readings_json'])[0]['value']
        assert math.isfinite(value) and value>=0
    payload = dict(batch=metadata,parameters=params,samples=samples,contexts=contexts,events=events,anomalies=anomalies)
    (OUT/'dataset.json').write_text(canonical(payload),encoding='utf-8')
    for name,data in [('power_samples.csv',samples),('cabinet_events.csv',events),('operation_context.csv',contexts),
                      ('anomaly_labels.csv',anomalies),('source_sessions.csv',source)]:
        write_csv(name,data)
    sql = "SET time_zone='+00:00';\nSTART TRANSACTION;\n"
    sql += insert('data_batch',list(metadata),[metadata])
    for table,data in [('operation_context',contexts),('cabinet_event',events),('anomaly_event',anomalies),('iot_sensor',samples)]:
        sql+=insert(table,list(data[0]),data)
    boots=','.join(literal(b) for b in boot_ids.values())
    sql+=f"INSERT INTO data_batch_sensor (batch_id,sensor_id) SELECT {literal(batch)},id FROM iot_sensor WHERE boot_id IN ({boots}) AND data_source='MOCK';\nCOMMIT;\n"
    (OUT/'insert.sql').write_text(sql,encoding='utf-8')
    rollback = f"""-- Removes rows belonging to this batch only; preserves the shared schemas.
SET time_zone='+00:00';
START TRANSACTION;
DELETE s FROM iot_sensor s JOIN data_batch_sensor m ON m.sensor_id=s.id
 WHERE m.batch_id={literal(batch)} AND s.data_source='MOCK' AND s.boot_id IN ({boots});
DELETE FROM cabinet_event WHERE batch_id={literal(batch)};
DELETE FROM anomaly_event WHERE batch_id={literal(batch)};
DELETE FROM operation_context WHERE batch_id={literal(batch)};
DELETE FROM data_batch WHERE id={literal(batch)};
COMMIT;
"""
    (OUT/'rollback.sql').write_text(rollback,encoding='utf-8')
    summary = dict(batch_id=batch,start_local=start.isoformat(),end_local_exclusive=end.isoformat(),
                   rooms=params['rooms'],source_sessions=len(source),expected_samples=expected,missing_samples=missing,
                   inserted_samples=len(samples),cabinet_events=len(events),contexts=len(contexts),anomalies=len(anomalies),
                   data_source='MOCK',local_checks='PASS',source_sessions_sha256=source_hash)
    (OUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(summary,ensure_ascii=False,indent=2))


def dt_local(value):
    return datetime.fromisoformat(value).replace(tzinfo=UTC).astimezone(LOCAL)


if __name__=='__main__':
    main()
