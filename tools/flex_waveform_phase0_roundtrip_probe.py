#!/usr/bin/env python3
"""Phase 0 stage 1b — round-trip fidelity A/B.

Produced RADE V2 RFC §8 "1b", the finding that forced the local-render decision
(§6.3): the waveform's monitor round-trip is flat to ~2.5 kHz, -6 dB @ ~2800 Hz,
-60..-72 dB by 3-4 kHz, and sweeping the slice audio filter to 11 kHz does NOT
move the rolloff. The cap is fixed and not filter-adjustable, so routing decoded
RADE speech back through it would reproduce the Flex-FreeDV container's quality
loss. This is also the rig to re-run for the §9.1 / §16 Q1 rx_stream_out A/B,
with real decoded speech substituted for the sweep.

Probe (sole client): register RADP waveform, create pan+slice in RADP, subscribe
remote_audio_rx (uncompressed), SEND a known log-sweep on rx_stream_out, capture
what returns via the monitor, FFT-compare to find the band-limit / rolloff.

RX-only: rx_stream_out is the RX-monitor return, NOT a transmit path. No TX keying.
Run with AetherSDR DOWN so the probe can own the slice.

CAVEATS for anyone re-running it:
  - RX_FILTER below uses a NEGATIVE low_cut. Per RFC §10 that is silently clamped
    to 0 on a USB-family waveform. Harmless here (1b deliberately wanted the
    filter wide open, and the finding is that the cap is filter-independent
    anyway), but do not copy the sign convention into the provider.
  - It creates its OWN pan+slice, which can hit the unexplained 0x50000003 noted
    in RFC §10.3. The later probes route around this by using the auto-restored
    slice; this one predates that workaround.

Preserved verbatim from the 2026-07-26 Phase 0 session apart from the radio host
being made overridable (it was hardcoded).
"""
import os, socket, sys, time, struct, threading
import numpy as np

HOST = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
PORT = int(os.environ.get("FLEX_RADIO_PORT", "4992"))
VITA_PORT = 4991
WF_NAME, WF_MODE = "AetherRadeProbe", "RADP"
FS = 24000
SAMPLES_PER_PKT = 128
CIDH = 0x00001C2D
CIDL = 0x534C03E3
SEND_SECS = 5.0
RX_FILTER = (-8000, 8000)   # wide, to test whether the filter limits the OUTPUT

class Flex:
    def __init__(s, host, port):
        s.sock=socket.create_connection((host,port),timeout=5); s.sock.settimeout(0.5)
        s.buf=b""; s.seq=0; s.log=[]; s.replies={}; s.status=[]; s.stop=False
        s.t=threading.Thread(target=s._rx,daemon=True); s.t.start()
    def _rx(s):
        while not s.stop:
            try: d=s.sock.recv(8192)
            except socket.timeout: continue
            except Exception: break
            if not d: break
            s.buf+=d
            while b"\n" in s.buf:
                ln,s.buf=s.buf.split(b"\n",1); t=ln.decode("utf-8","replace").rstrip("\r")
                if not t: continue
                if t[0]=="S": s.status.append(t)
                if t[0]=="R":
                    try:
                        b=t[1:]; sq,rest=b.split("|",1); p=rest.split("|",1)
                        s.replies[int(sq)]=(int(p[0],16), p[1] if len(p)>1 else "")
                    except Exception: pass
    def cmd(s,c,wait=4.0):
        s.seq+=1; sq=s.seq; s.sock.sendall(f"C{sq}|{c}\n".encode())
        end=time.time()+wait
        while time.time()<end:
            if sq in s.replies: return s.replies[sq]
            time.sleep(0.03)
        return None
    def close(s):
        s.stop=True
        try: s.sock.close()
        except Exception: pass

def log_sweep(f0,f1,secs,fs):
    n=int(secs*fs); t=np.arange(n)/fs
    k=(f1/f0)**(1.0/secs)
    phase=2*np.pi*f0*((k**t-1)/np.log(k))
    return (0.5*np.sin(phase)).astype(np.float32)

def build_out(stream_id, real_samples, pkt_count):
    n=len(real_samples); size_words=7+n*2
    header=(0x10000000|0x08000000|0x00400000|0x00200000|((pkt_count&0xF)<<16)|(size_words&0xFFFF))
    now=time.time(); ti=int(now); fp=int((now-ti)*1e12)
    hdr=struct.pack(">IIIIIII",header,stream_id,CIDH,CIDL,ti,(fp>>32)&0xFFFFFFFF,fp&0xFFFFFFFF)
    inter=np.zeros(n*2,dtype='>f4'); inter[0::2]=real_samples   # real=audio, imag=0
    return hdr+inter.tobytes()

def parse_hdr(p):
    if len(p)<28: return None
    h,sid,cidh,cidl,ti,fh,fl=struct.unpack(">7I",p[:28])
    return dict(sid=sid,payload=p[28:],size=h&0xFFFF)

def main():
    f=Flex(HOST,PORT); pan_id=None; slice_id=None; rax_id=None
    udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    udp.bind(("0.0.0.0",0)); P=udp.getsockname()[1]; udp.settimeout(0.2)
    try:
        time.sleep(1.2); f.log.clear()
        f.cmd(f"client program {WF_NAME}"); f.cmd("client start_persistence off")
        print("client gui:",f.cmd("client gui")); f.cmd(f"client station {WF_NAME}")
        f.cmd("client set local_ptt=1")
        print("client udpport:",f.cmd(f"client udpport {P}"))
        for sub in ("sub client all","sub pan all","sub slice all","sub audio_stream all"): f.cmd(sub)
        time.sleep(0.3)
        f.cmd(f"waveform remove {WF_NAME}")
        cr=f.cmd(f"waveform create name={WF_NAME} mode={WF_MODE} underlying_mode=USB version=0.0.1")
        streams={k:int(v,16) for k,v in (tok.split("=",1) for tok in cr[1].split() if "=" in tok)}
        rx_out=streams["rx_stream_out_id"]; rx_in=streams["rx_stream_in_id"]
        print("rx_stream_out_id:",hex(rx_out))
        f.cmd(f"waveform set {WF_NAME} tx=1")
        f.cmd(f"waveform set {WF_NAME} rx_filter low_cut={RX_FILTER[0]} high_cut={RX_FILTER[1]} depth=256")
        f.cmd(f"waveform set {WF_NAME} udpport={P}")
        pf=f.cmd("display panafall create x=100 y=100")
        pan_id="0x"+pf[1].split(",")[0].strip().replace("0x","").replace("0X","")
        f.cmd(f"display panafall set {pan_id} band=20"); f.cmd(f"display panafall set {pan_id} center=14.100"); time.sleep(0.5)
        sc=f.cmd(f"slice create pan={pan_id} freq=14.100 rxant=ANT1 mode=USB")
        slice_id=sc[1].strip().split()[0]; print("slice:",slice_id)
        time.sleep(0.3); f.cmd(f"slice set {slice_id} mode={WF_MODE}"); time.sleep(0.5)
        rax=f.cmd("stream create type=remote_audio_rx compression=none")
        rax_id=int(rax[1].split()[0],16) if rax and rax[0]==0 and rax[1].strip() else None
        print("remote_audio_rx stream:",hex(rax_id) if rax_id else rax)
        time.sleep(0.4)

        import re
        def applied_filter():
            for stt in f.status[::-1]:
                if re.search(rf"slice {slice_id} .*filter_hi=", stt):
                    fh=re.search(r"filter_hi=(\d+)",stt); fl=re.search(r"filter_lo=(-?\d+)",stt)
                    md=re.search(r" mode=(\w+)",stt)
                    return (md.group(1) if md else "?"), (fl.group(1) if fl else "?"), (fh.group(1) if fh else "?")
            return "?","?","?"

        def measure(tag):
            sweep=log_sweep(100.0,11000.0,SEND_SECS,FS); npk=len(sweep)//SAMPLES_PER_PKT
            cap=[]; stop_cap=threading.Event()
            def capture():
                while not stop_cap.is_set():
                    try: data,_=udp.recvfrom(4096)
                    except socket.timeout: continue
                    cap.append(data)
            ct=threading.Thread(target=capture,daemon=True); ct.start()
            t0=time.perf_counter(); dt=SAMPLES_PER_PKT/FS
            for i in range(npk):
                udp.sendto(build_out(rx_out,sweep[i*SAMPLES_PER_PKT:(i+1)*SAMPLES_PER_PKT],i),(HOST,VITA_PORT))
                target=t0+(i+1)*dt
                while True:
                    rem=target-time.perf_counter()
                    if rem<=0: break
                    if rem>0.002: time.sleep(rem-0.001)
            time.sleep(0.7); stop_cap.set(); time.sleep(0.2)
            mon=[h for h in (parse_hdr(p) for p in cap) if h and h["sid"]==rax_id]
            if not mon: print(f"  [{tag}] NO monitor audio"); return
            audio=np.concatenate([np.frombuffer(h["payload"],dtype='>f4').astype(np.float32) for h in mon])
            L=audio[0::2] if len(audio)%2==0 else audio
            L=L[int(0.6*FS):] if len(L)>int(0.6*FS) else L
            x=L*np.hanning(len(L)); X=np.abs(np.fft.rfft(x)); frq=np.fft.rfftfreq(len(x),1/FS)
            def bd(lo,hi):
                m=(frq>=lo)&(frq<hi); return 20*np.log10(np.maximum(np.median(X[m]),1e-12))
            ref=bd(300,2500)
            edges=np.arange(300,11000,100); lev=np.array([bd(e,e+100)-ref for e in edges])
            def roll(th):
                for i,e in enumerate(edges):
                    if e>2000 and lev[i]<th: return int(e)
                return None
            print(f"  [{tag}] applied {applied_filter()} | -6dB@{roll(-6)}Hz  -20dB@{roll(-20)}Hz | "
                  f"2.5-3k:{bd(2500,3000)-ref:+.0f} 3-4k:{bd(3000,4000)-ref:+.0f} 4-6k:{bd(4000,6000)-ref:+.0f} dB")

        print("\n=== filter-lever sweep: `filt <idx> 0 <hi>`, measure round-trip rolloff ===")
        for fh in [2800, 4000, 6000, 8000, 11000]:
            f.status.clear()
            r=f.cmd(f"filt {slice_id} 0 {fh}")
            time.sleep(0.6)
            print(f"request filt 0..{fh} (reply {r}):")
            measure(f"fh={fh}")
        udp.close()
    finally:
        print("\n=== teardown ===")
        try:
            if rax_id: f.cmd("stream remove 0x%08X"%rax_id)
        except Exception: pass
        if slice_id is not None: f.cmd(f"slice remove {slice_id}")
        if pan_id: f.cmd(f"display pan remove {pan_id}")
        f.cmd(f"waveform remove {WF_NAME}")
        f.close()

if __name__=="__main__": main()
