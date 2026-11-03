"""Check generated timelines, derive unlabeled features, and render pilot figures."""
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
import hashlib
import json

from prepare import OUT, canonical, dt_local, write_csv


def load_and_verify():
    data=json.loads((OUT/'dataset.json').read_text(encoding='utf-8'))
    batch=data['batch']; samples=data['samples']; params=data['parameters']
    start=dt_local(batch['starts_at_utc']); end=dt_local(batch['ends_at_utc'])
    source=params['source_sessions']
    assert hashlib.sha256(canonical(source).encode()).hexdigest()==params['source_sessions_sha256']
    nodes={n['id']:n for n in params['nodes']}
    bynode=defaultdict(dict)
    for sample in samples:
        assert sample['node_id'] in nodes and sample['data_source']=='MOCK'
        t=dt_local(sample['measured_at'])
        assert start<=t<end
        assert 0<=(dt_local(sample['received_at'])-t).total_seconds()<=2
        assert sample['sequence']==int((t-start).total_seconds()/30)+1
        assert sample['boot_id']==params['boot_ids'][str(sample['node_id'])]
        assert t not in bynode[sample['node_id']]
        reading=json.loads(sample['readings_json'])
        assert len(reading)==1 and reading[0]['metric']=='current' and reading[0]['unit']=='A'
        assert reading[0]['quality']=='GOOD' and reading[0]['value']>=0
        bynode[sample['node_id']][t]=sample
    anomalies=data['anomalies']
    assert Counter(a['anomaly_type'] for a in anomalies)=={
        'DOOR_OPEN_LONG':3,'CURRENT_AFTER_RETURN':3,'TELEMETRY_GAP':3}
    for n in nodes.values():
        missing=[]
        t=start
        gap=next(a for a in anomalies if a['room_id']==n['room_id'] and a['anomaly_type']=='TELEMETRY_GAP')
        while t<end:
            should_miss=dt_local(gap['starts_at_utc'])<=t<dt_local(gap['ends_at_utc'])
            assert (t not in bynode[n['id']])==should_miss
            if should_miss: missing.append(t)
            t+=timedelta(seconds=30)
        assert len(missing)>0
    doors=defaultdict(list)
    for event in data['events']:
        assert event['batch_id']==batch['id'] and event['data_source']=='MOCK'
        if event['command_id']: doors[event['command_id']].append(event)
    for events in doors.values():
        events.sort(key=lambda e:e['occurred_at_utc'])
        assert [e['event_type'] for e in events]==['OPEN_REQUESTED','UNLOCK_PULSE','DOOR_OPENED','DOOR_CLOSED']
        assert len({e['session_id'] for e in events})==1
    for a in anomalies:
        a0,a1=dt_local(a['starts_at_utc']),dt_local(a['ends_at_utc'])
        assert start<=a0<a1<end
        if a['anomaly_type']=='DOOR_OPEN_LONG':
            related=[es for es in doors.values() if es[0]['room_id']==a['room_id'] and
                     es[0]['session_id']==a['session_id'] and es[2]['occurred_at_utc']==a['starts_at_utc']]
            assert len(related)==1 and related[0][3]['occurred_at_utc']==a['ends_at_utc']
        if a['anomaly_type']=='CURRENT_AFTER_RETURN':
            assert not any(c['context_type']=='BORROWING' and c['room_id']==a['room_id'] and
                           dt_local(c['starts_at_utc'])<a1 and dt_local(c['ends_at_utc'])>a0 for c in data['contexts'])
            assert json.loads(next(s for s in samples if nodes[s['node_id']]['room_id']==a['room_id'] and
                                   a0<=dt_local(s['measured_at'])<a1)['readings_json'])[0]['value']>1
    return data,bynode,doors


def features(data,bynode,doors):
    """Feature CSV has no ground truth, generator profile, batch ID, or anomaly type."""
    start=dt_local(data['batch']['starts_at_utc']); end=dt_local(data['batch']['ends_at_utc'])
    result=[]; labels=[]
    for n in data['parameters']['nodes']:
        room=n['room_id']; room_samples=bynode[n['id']]
        contexts=[c for c in data['contexts'] if c['room_id']==room]
        room_doors=[es for es in doors.values() if es[0]['room_id']==room]
        t=start
        while t<end:
            stop=t+timedelta(minutes=5)
            values=[json.loads(room_samples[t+timedelta(seconds=i*30)]['readings_json'])[0]['value']
                    for i in range(10) if t+timedelta(seconds=i*30) in room_samples]
            mean=sum(values)/len(values) if values else None
            borrow_seconds=sum(max(0,(min(stop,dt_local(c['ends_at_utc']))-max(t,dt_local(c['starts_at_utc']))).total_seconds())
                               for c in contexts if c['context_type']=='BORROWING')
            cleaning=any(c['context_type']=='CLEANING' and dt_local(c['starts_at_utc'])<stop and dt_local(c['ends_at_utc'])>t for c in contexts)
            door_seconds=sum(max(0,(min(stop,dt_local(es[3]['occurred_at_utc']))-max(t,dt_local(es[2]['occurred_at_utc']))).total_seconds()) for es in room_doors)
            delays=[(dt_local(s['received_at'])-dt_local(s['measured_at'])).total_seconds() for s in
                    (room_samples[t+timedelta(seconds=i*30)] for i in range(10) if t+timedelta(seconds=i*30) in room_samples)]
            result.append(dict(room_id=room,window_start_utc=t.astimezone(timezone.utc).isoformat(),
                               local_hour=t.hour,local_weekday=t.weekday(),borrowing_fraction=round(borrow_seconds/300,4),
                               authorized_cleaning=int(cleaning),current_mean_a=round(mean,4) if mean is not None else '',
                               current_max_a=max(values) if values else '',
                               current_std_a=round((sum((v-mean)**2 for v in values)/len(values))**.5,4) if values else '',
                               sample_count=len(values),missing_fraction=round(1-len(values)/10,2),
                               receive_delay_mean_seconds=round(sum(delays)/len(delays),4) if delays else '',
                               door_open_seconds=round(door_seconds,3),
                               door_open_count=sum(t<=dt_local(es[2]['occurred_at_utc'])<stop for es in room_doors)))
            types=sorted({a['anomaly_type'] for a in data['anomalies'] if a['room_id']==room and
                          dt_local(a['starts_at_utc'])<stop and dt_local(a['ends_at_utc'])>t})
            labels.append(dict(room_id=room,window_start_utc=t.astimezone(timezone.utc).isoformat(),is_anomaly=int(bool(types)),anomaly_types='|'.join(types)))
            t=stop
    assert len(result)==3*7*24*12
    write_csv('features_5min.csv',result)
    write_csv('labels_5min.csv',labels)
    return result,labels


def plot(data,bynode):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    colors={'CURRENT_AFTER_RETURN':'#dc2626','TELEMETRY_GAP':'#9333ea','DOOR_OPEN_LONG':'#f59e0b'}
    fig,axes=plt.subplots(3,1,figsize=(17,10),sharex=True)
    for ax,n in zip(axes,data['parameters']['nodes']):
        series=sorted(bynode[n['id']].items())
        # Include missing readings explicitly as NaN: never draw through an outage.
        t=dt_local(data['batch']['starts_at_utc']); end=dt_local(data['batch']['ends_at_utc'])
        times=[]; values=[]
        while t<end:
            sample=bynode[n['id']].get(t)
            times.append(t); values.append(json.loads(sample['readings_json'])[0]['value'] if sample else float('nan'))
            t+=timedelta(seconds=30)
        ax.plot(times,values,color='#2563eb',linewidth=.65,label='Generated current (A)')
        used=set()
        for c in data['contexts']:
            if c['room_id']!=n['room_id']: continue
            name='Borrowing' if c['context_type']=='BORROWING' else 'Authorized cleaning'
            ax.axvspan(dt_local(c['starts_at_utc']),dt_local(c['ends_at_utc']),alpha=.13,
                       color='#16a34a' if name=='Borrowing' else '#0891b2',label=name if name not in used else None)
            used.add(name)
        for a in data['anomalies']:
            if a['room_id']!=n['room_id']: continue
            ax.axvspan(dt_local(a['starts_at_utc']),dt_local(a['ends_at_utc']),color=colors[a['anomaly_type']],alpha=.4,label=a['anomaly_type'])
        ax.set_ylabel(n['room_id']+'\nCurrent (A)'); ax.grid(alpha=.2); ax.legend(loc='upper right',fontsize=7,ncol=3)
    axes[-1].xaxis.set_major_locator(mdates.DayLocator(tz=times[0].tzinfo))
    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter('%d/%m',tz=times[0].tzinfo))
    axes[-1].set_xlabel('Vietnam time (UTC+07:00), 24-30 September 2026')
    fig.suptitle('ClassHub operational pilot | MOCK data | 3 rooms, 7 days\nBorrowing history is context; current and physical events are generated',fontsize=13)
    fig.tight_layout(rect=(0,0,1,.94)); fig.savefig(OUT/'overview.png',dpi=140); plt.close(fig)
    fig,axes=plt.subplots(3,1,figsize=(14,9))
    for ax,kind in zip(axes,['CURRENT_AFTER_RETURN','DOOR_OPEN_LONG','TELEMETRY_GAP']):
        a=next(a for a in data['anomalies'] if a['anomaly_type']==kind)
        n=next(n for n in data['parameters']['nodes'] if n['room_id']==a['room_id'])
        a0,a1=dt_local(a['starts_at_utc']),dt_local(a['ends_at_utc'])
        t=a0.replace(second=0,microsecond=0)-timedelta(minutes=20); stop=a1+timedelta(minutes=20)
        ts=[]; vs=[]
        while t<stop:
            sample=bynode[n['id']].get(t)
            ts.append(t); vs.append(json.loads(sample['readings_json'])[0]['value'] if sample else float('nan'))
            t+=timedelta(seconds=30)
        if kind=='DOOR_OPEN_LONG':
            ax.step([a0-timedelta(minutes=20),a0,a1,a1+timedelta(minutes=20)],[0,1,0,0],where='post',color='#2563eb')
            ax.set_ylabel('Door open (0/1)')
        else:
            ax.plot(ts,vs,color='#2563eb'); ax.set_ylabel('Current (A)')
        ax.axvspan(a0,a1,color=colors[kind],alpha=.2)
        ax.set_title(a['room_id']+' | '+kind+' | '+a0.strftime('%d/%m/%Y'))
        ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M',tz=a0.tzinfo)); ax.grid(alpha=.2)
        ax.set_xlabel('Vietnam time (UTC+07:00)')
    fig.suptitle('Labeled episodes: generated data, not physical measurements',fontsize=13)
    fig.tight_layout(rect=(0,0,1,.96)); fig.savefig(OUT/'episodes.png',dpi=140); plt.close(fig)


if __name__=='__main__':
    data,bynode,doors=load_and_verify()
    feature_rows,labels=features(data,bynode,doors)
    plot(data,bynode)
    report=dict(status='PASS',samples=len(data['samples']),events=len(data['events']),episodes=len(data['anomalies']),
                feature_windows=len(feature_rows),labeled_windows=sum(x['is_anomaly'] for x in labels),
                checks=['UTC/local conversion','exact sequence and missing intervals','nonnegative current',
                        'command lifecycle','after-return occupancy exclusion','separate feature and label exports'])
    (OUT/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))
