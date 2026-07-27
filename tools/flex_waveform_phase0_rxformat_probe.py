#!/usr/bin/env python3
"""Phase 0 stage 1a — capture the waveform rx_stream_in VITA-49 format (live).

Produced RADE V2 RFC §8 "1a": VITA-49 IF-Data w/ Stream ID, FlexRadio OUI 0x1c2d,
class 0x534C03E3, 128 Complex-float32 samples/packet big-endian, 24 kHz steady
state — i.e. the waveform transport is NOT lower fidelity than RADEv1's.

Self-contained on the probe's own GUI connection: registers a waveform, creates
its OWN pan + slice in RADP mode (does NOT touch AetherSDR's slice 0), captures
rx_stream_in packets, parses the VITA-49 header, derives sample rate/format.
Full teardown; disconnect auto-frees anything left. RX-only, no TX.

CAVEAT — this probe's own conclusion about the payload was WRONG and is corrected
by RFC §10.3 item 5: it read the interleaved floats as real passband. They are a
CONJUGATE I/Q pair; the imaginary component must be negated (`A - jB`) before the
codec sees it. The later tools/flex_waveform_dataplane_probe.py settled this. The
script is preserved as the historical article, not as a correct reference.

Preserved verbatim from the 2026-07-26 Phase 0 session apart from the radio host
being made overridable (it was hardcoded).
"""
import os, socket, sys, time, struct, threading

HOST = os.environ.get("FLEX_RADIO_HOST", "192.168.45.12")
PORT = int(os.environ.get("FLEX_RADIO_PORT", "4992"))
WF_NAME, WF_MODE = "AetherRadeProbe", "RADP"

class Flex:
    def __init__(s, host, port):
        s.sock = socket.create_connection((host, port), timeout=5); s.sock.settimeout(0.5)
        s.buf=b""; s.seq=0; s.log=[]; s.replies={}; s.stop=False
        s.t=threading.Thread(target=s._rx, daemon=True); s.t.start()
    def _rx(s):
        while not s.stop:
            try: d=s.sock.recv(8192)
            except socket.timeout: continue
            except Exception: break
            if not d: break
            s.buf+=d
            while b"\n" in s.buf:
                ln,s.buf=s.buf.split(b"\n",1)
                t=ln.decode("utf-8","replace").rstrip("\r")
                if not t: continue
                s.log.append("< "+t)
                if t[0]=="R":
                    try:
                        body=t[1:]; sq,rest=body.split("|",1); p=rest.split("|",1)
                        s.replies[int(sq)]=(int(p[0],16), p[1] if len(p)>1 else "")
                    except Exception: pass
    def cmd(s,c,wait=4.0):
        s.seq+=1; sq=s.seq; s.sock.sendall(f"C{sq}|{c}\n".encode()); s.log.append("> "+c)
        end=time.time()+wait
        while time.time()<end:
            if sq in s.replies: return s.replies[sq]
            time.sleep(0.03)
        return None
    def close(s):
        s.stop=True
        try: s.sock.close()
        except Exception: pass

def parse_vita(pkt):
    if len(pkt)<28: return None
    hdr,sid,cidh,cidl,tsi,tfh,tfl = struct.unpack(">7I", pkt[:28])
    ptype=hdr&0xF0000000; C=bool(hdr&0x08000000); T=bool(hdr&0x04000000)
    pcount=(hdr>>16)&0xF; psize=hdr&0xFFFF
    payload=pkt[28:]
    return dict(type=hex(ptype),C=C,T=T,pcount=pcount,size_words=psize,
                stream_id=hex(sid),oui=hex(cidh&0xFFFFFF),
                info_class=hex((cidl>>16)&0xFFFF),pkt_class=hex(cidl&0xFFFF),
                payload_bytes=len(payload))

def main():
    f=Flex(HOST,PORT); pan_id=None; slice_id=None
    try:
        time.sleep(1.2)
        print("greeting:", [l for l in f.log if l.startswith("< V") or l.startswith("< H")])
        f.log.clear()
        # Full-ish GUI-client bind sequence (mirrors FlexLib) so display creation is allowed
        print("client program:", f.cmd(f"client program {WF_NAME}"))
        print("start_persist :", f.cmd("client start_persistence off"))
        print("client gui:", f.cmd("client gui"))
        print("station   :", f.cmd(f"client station {WF_NAME}"))
        print("local_ptt :", f.cmd("client set local_ptt=1"))
        for sub in ("sub client all","sub pan all","sub slice all","sub tx all",
                    "sub audio_stream all","sub dax all","sub daxiq all"):
            f.cmd(sub)
        time.sleep(0.4)
        f.cmd(f"waveform remove {WF_NAME}")
        cr=f.cmd(f"waveform create name={WF_NAME} mode={WF_MODE} underlying_mode=USB version=0.0.1")
        print("waveform create:",cr)
        streams={}
        if cr and cr[0]==0:
            for tok in cr[1].split():
                if "=" in tok:
                    k,v=tok.split("=",1); streams[k]=int(v,16)
        rx_in=streams.get("rx_stream_in_id")
        print("rx_stream_in_id:", hex(rx_in) if rx_in else None, "| all:", {k:hex(v) for k,v in streams.items()})

        udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        udp.bind(("0.0.0.0",0)); uport=udp.getsockname()[1]; udp.settimeout(0.3)
        print("udp listener port:",uport)
        for c in [f"waveform set {WF_NAME} tx=1",
                  f"waveform set {WF_NAME} rx_filter low_cut=-8000 high_cut=8000 depth=256",
                  f"waveform set {WF_NAME} tx_filter low_cut=0 high_cut=8000 depth=256",
                  f"waveform set {WF_NAME} udpport={uport}"]:
            print("  set:",c.split(WF_NAME+' ')[1] if WF_NAME+' ' in c else c, "->", f.cmd(c))

        # own pan + slice, mode RADP
        pf=f.cmd("display panafall create x=100 y=100")
        print("panafall create:",pf)
        if pf and pf[0]==0:
            raw=pf[1].split(",")[0].strip()
            pan_hex=raw[2:] if raw.lower().startswith("0x") else raw
            pan_id="0x"+pan_hex
            time.sleep(0.5)
            # put the pan on 20m so a 14.1 slice is in range
            print("pan band=20:", f.cmd(f"display panafall set {pan_id} band=20"))
            print("pan center :", f.cmd(f"display panafall set {pan_id} center=14.100"))
            time.sleep(0.6)
        # try slice-create variants; dump radio M/S lines after each to see the reason
        variants=[
            f"slice create pan={pan_id} freq=14.100 rxant=ANT1 mode=USB",
            f"slice create pan={pan_id} freq=14.100 mode=USB",
            f"slice create pan={pan_id} mode=USB",
            f"slice create pan={pan_id}",
            "slice create freq=14.100 mode=USB",
        ]
        for v in variants:
            f.log.clear()
            sc=f.cmd(v)
            print(f"\ntry: {v}\n  -> {sc}")
            time.sleep(0.4)
            for l in f.log:
                if l.startswith("< M") or ("slice" in l and l.startswith("< S")): print("   ",l)
            if sc and sc[0]==0:
                slice_id=sc[1].strip().split()[0] if sc[1].strip() else None
                print("  SLICE CREATED, id=",slice_id); break
        if slice_id is not None:
            time.sleep(0.4)
            print(f"slice {slice_id} -> {WF_MODE}:",f.cmd(f"slice set {slice_id} mode={WF_MODE}"))
            time.sleep(0.5)

        # capture rx_stream_in for ~4.5s, timestamp each packet, drop first 1.0s (startup burst)
        print("\n=== capturing rx_stream_in for 4.5s (steady-state rate excludes first 1.0s) ===")
        arr=[]; t0=time.time()
        while time.time()-t0<4.5:
            try: data,_=udp.recvfrom(4096)
            except socket.timeout: continue
            arr.append((time.time(), data))
        udp.close()
        matched=[(t,p) for (t,p) in arr if rx_in and len(p)>=8 and struct.unpack(">I",p[4:8])[0]==rx_in]
        print(f"total {len(arr)} pkts; matching rx_stream_in {len(matched)}")
        if matched:
            info=parse_vita(matched[len(matched)//2][1]); print("VITA header:",info)
            pb=info["payload_bytes"]; samp=pb//8   # Complex float32
            # steady-state window: packets arriving after t0+1.0
            ss=[t for (t,_) in matched if t>=t0+1.0]
            if len(ss)>=2:
                span=ss[-1]-ss[0]; pps=(len(ss)-1)/span
                print(f"payload {pb}B = {samp} Complex-f32 samples/pkt")
                print(f"steady-state: {len(ss)} pkts over {span:.2f}s = {pps:.1f} pkt/s -> {samp*pps:.0f} Hz")
            pl=matched[len(matched)//2][1][28:28+32]
            vals=struct.unpack(">"+"f"*(len(pl)//4), pl)
            print("first payload floats (BE):",[round(v,4) for v in vals[:8]])
        else:
            print("NO matching packets")
    finally:
        print("\n=== teardown ===")
        if slice_id is not None: print("slice remove:",f.cmd(f"slice remove {slice_id}"))
        if pan_id: print("pan remove:",f.cmd(f"display pan remove {pan_id}"))
        print("waveform remove:",f.cmd(f"waveform remove {WF_NAME}"))
        f.close()

if __name__=="__main__": main()
