"""QPC trace summary; client START events alone are not sample-gap evidence."""
import argparse, ctypes, json
from pathlib import Path
from collections import defaultdict
import numpy as np
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);args=p.parse_args()
rows=[]
for file in args.directory.glob('trace-*.csv'):
    for line in file.read_text().splitlines():
        tick,pid,tid,call,phase,a,b=line.split(',');rows.append(dict(t=int(tick),pid=int(pid),tid=int(tid),call=int(call),phase=phase,a=int(a),b=int(b)))
rows.sort(key=lambda r:r['t'])
freq=ctypes.c_longlong();ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(freq));freq=freq.value
origin=rows[0]['t'];ms=lambda ticks:round(ticks*1000/freq,3)
calls=defaultdict(list)
for row in rows:
    if row['phase'] in ('engine_begin','engine_return','first_pcm','synthesis_request','synthesis_complete','write_begin','write_return','native_cancel_requested','cancel_drained_reusable'):calls[(row['pid'],row['call'])].append(row)
summary=[]
for (pid,call),items in sorted(calls.items()):
    phases=defaultdict(list)
    for row in items:phases[row['phase']].append(row)
    if not phases['engine_begin']:continue
    writes=phases['write_return'];end=phases['engine_return'];synth=[]
    for request in phases['synthesis_request']:
        child=request['b'];number=request['a']
        matching=[r for r in rows if r['pid']==child and r['call']==number]
        native={r['phase']:r for r in matching}
        synth.append(dict(request=number,helper_pid=child,start_ms=ms(native.get('native_begin',request)['t']-origin),first_pcm_ms=ms(native['native_first_pcm']['t']-origin) if 'native_first_pcm' in native else None,completed_ms=ms(native['native_complete']['t']-origin) if 'native_complete' in native else None))
    summary.append(dict(pid=pid,call=call,engine_begin_ms=ms(phases['engine_begin'][0]['t']-origin),engine_return_ms=ms(end[-1]['t']-origin) if end else None,last_write_return_ms=ms(writes[-1]['t']-origin) if writes else None,return_after_last_write_ms=ms(end[-1]['t']-writes[-1]['t']) if end and writes else None,synthesis=synth,cancel_reused=bool(phases['cancel_drained_reusable'])))
events=[dict(stream=r['call'],event=r['a'],audio_offset=r['b'],ms=ms(r['t']-origin)) for r in rows if r['phase']=='client_event' and r['a'] in (1,2)]
report=dict(qpc_frequency=freq,engine_calls=summary,stream_events=events)
controls=[r for r in rows if r['phase']=='control_begin']
packets=[r for r in rows if r['phase']=='capture_first_packet']
if controls and packets:
    capture=np.fromfile(args.directory/'capture-24k-float.raw',dtype='<f4')
    # A stop-only recording: measures last non-silent physical sample after
    # submission of purge, rather than just the time Speak returned.
    name=args.directory.name
    if name.endswith('stop'):
        audible=np.flatnonzero(np.abs(capture)>0.0005)
        if len(audible):
            control_100ns=controls[0]['t']*10000000/freq
            sample_100ns=packets[0]['b']+int(audible[-1])*10000000/24000
            delta=(sample_100ns-control_100ns)/10000
            if delta>=-20:report['purge_to_last_non_silent_sample_ms']=round(delta,3)
            else:report['physical_stop_latency']='unavailable: process-loopback position clock paused while idle; do not infer wall-clock stop latency from its first packet'
report_path=args.directory/'timing-summary.json';report_path.write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2))
