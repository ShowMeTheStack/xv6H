# dry-run every xv6H patch against pristine files from the kernel tarball
import subprocess, os, re, glob, sys
W=os.path.expanduser("~/xv6h-linux"); V=sys.argv[1] if len(sys.argv)>1 else "6.12.41"; T="/tmp/pc-"+V; subprocess.run(["rm","-rf",T]); os.makedirs(T)
bad=0
for p in sorted(glob.glob(W+"/src/patches/*.patch")):
    s=open(p).read(); files=set(re.findall(r"(?m)^\+\+\+ b/(\S+)",s))
    for f in files:
        d=os.path.join(T,os.path.dirname(f)); os.makedirs(d,exist_ok=True)
        try: open(os.path.join(T,f),"w").write(subprocess.check_output(["tar","xf",W+"/linux-%s.tar.xz"%V,"linux-%s/"%V+f,"-O"]).decode())
        except subprocess.CalledProcessError: open(os.path.join(T,f),"w").write("")   # a new file
    r=subprocess.run(["patch","-d",T,"-p1","-N","-s"],input=s,capture_output=True,text=True)
    print(V, os.path.basename(p), "OK" if r.returncode==0 else "FAILS\n"+r.stdout+r.stderr); bad|=r.returncode
sys.exit(bad)
