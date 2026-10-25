"""Align submitted PCM against this process's actual WASAPI loopback samples."""
import argparse, json, wave
from pathlib import Path
import numpy as np

parser=argparse.ArgumentParser()
parser.add_argument('directory',type=Path)
args=parser.parse_args()
capture=np.fromfile(args.directory/'capture-24k-float.raw',dtype='<f4').astype(np.float64)
results=[]
def align(reference,offset,count=4800):
    anchor=reference[offset:offset+count].astype(np.float64)
    sums=np.concatenate(([0.],np.cumsum(capture*capture)))
    window_energy=np.maximum(sums[len(anchor):]-sums[:-len(anchor)],0.)
    norms=np.sqrt(np.maximum(window_energy,1e-12)*np.sum(anchor*anchor))
    n=1 << (len(capture)+len(anchor)-2).bit_length()
    correlation=np.fft.irfft(np.fft.rfft(capture,n)*np.fft.rfft(anchor[::-1],n),n)[len(anchor)-1:len(capture)]
    scores=correlation/norms
    scores[window_energy < max(float(window_energy.max())*1e-5,1e-10)]=-np.inf
    best=int(np.argmax(scores))
    gain=float(np.dot(capture[best:best+len(anchor)],anchor)/np.dot(anchor,anchor))
    return best-offset,float(scores[best]),gain
files=sorted(args.directory.glob('pcm-*.raw'),key=lambda p:int(p.stem.split('-')[-1]))
prepared=sorted(args.directory.glob('prepared-*.wav'),key=lambda p:int(p.stem.split('-')[-1]))
if prepared:
    files=files[:1]+prepared
for file in files:
    if file.suffix=='.wav':
        with wave.open(str(file),'rb') as wav:
            if (wav.getframerate(),wav.getnchannels(),wav.getsampwidth())!=(24000,1,2):raise SystemExit('Unexpected prepared WAV format')
            reference=np.frombuffer(wav.readframes(wav.getnframes()),dtype='<i2').astype(np.float64)/32768
    else:reference=np.fromfile(file,dtype='<i2').astype(np.float64)/32768
    indices=np.flatnonzero(np.abs(reference)>0.001)
    if not len(indices): continue
    first=int(indices[0]);last=int(indices[-1])
    start,cs,gain=align(reference,first,min(4800,len(reference)-first))
    tail,ce,_=align(reference,max(0,last-4800),min(4800,last+1))
    results.append(dict(file=file.name,samples=len(reference),start_sample=start,end_sample=tail+len(reference),start_correlation=cs,end_correlation=ce,gain=gain,alignment_drift_samples=tail-start))
gaps=[]
if len(results)!=3:
    raise SystemExit(f'Expected three nonempty PCM references, found {len(results)}.')
for a,b in zip(results,results[1:]):
    gaps.append(dict(a=a['file'],b=b['file'],pcm_boundary_gap_ms=(b['start_sample']-a['end_sample'])*1000/24000))
report=dict(sample_rate=24000,method='process loopback; normalized waveform correlation; silent PCM padding included',streams=results,gaps=gaps)
print(json.dumps(report,ensure_ascii=False,indent=2))
(args.directory/'sample-gaps.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
if any(min(x['start_correlation'],x['end_correlation'])<0.7 or abs(x['alignment_drift_samples'])>48 for x in results):
    raise SystemExit('Low confidence/clock drift: do not treat gaps as a precise measurement.')
