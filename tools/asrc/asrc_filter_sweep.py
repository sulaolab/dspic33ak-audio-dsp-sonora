import numpy as np
L=128; fc=0.45; AUDIO=20000.0/48000.0; Nfft=1<<18

def win_val(window, wpos):
    if window=='blackman':
        return 0.42 - 0.5*np.cos(2*np.pi*wpos) + 0.08*np.cos(4*np.pi*wpos)
    if window=='bh4':
        return (0.35875 - 0.48829*np.cos(2*np.pi*wpos) + 0.14128*np.cos(4*np.pi*wpos)
                - 0.01168*np.cos(6*np.pi*wpos))
    if window.startswith('kaiser'):
        beta=float(window.split(':')[1])
        return np.i0(beta*np.sqrt(np.maximum(0.0,1-(2*wpos-1)**2)))/np.i0(beta)

def analyze(M, window):
    MH=M//2-1; C=np.zeros((L,M))
    for p in range(L):
        for k in range(M):
            d=k-MH-p/L; x=2*fc*d
            s=1.0 if abs(x)<1e-6 else np.sin(np.pi*x)/(np.pi*x)
            wpos=(d+MH+1)/M
            C[p,k]=2*fc*s*win_val(window,wpos)
        C[p]/=C[p].sum()
    hp=np.zeros(M*L)
    for p in range(L):
        for k in range(M):
            hp[k*L-p+(L-1)]=C[p,k]
    hp/=hp.sum()
    H=20*np.log10(np.maximum(np.abs(np.fft.rfft(hp,Nfft)),1e-15))
    fFs=np.arange(len(H))*L/Nfft
    pb=H[fFs<=AUDIO]
    return pb.max()-pb.min(), H[np.argmin(np.abs(fFs-AUDIO))], H[fFs>=(1-AUDIO)].max()

print(f"{'M':>3} {'window':>11} | {'@20k(dB)':>9} | {'worst-image(dB)':>15} | MAC vs M32")
print("-"*60)
for window in ('bh4','kaiser:11','kaiser:12','kaiser:13','kaiser:14','kaiser:16'):
    for M in (32,28,24,20,18,16):
        r,e,ir=analyze(M,window)
        print(f"{M:>3} {window:>11} | {e:>9.3f} | {ir:>15.1f} | {M/32*100:>5.1f}%")
    print()
