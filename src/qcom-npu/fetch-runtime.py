import io, urllib.request, zipfile, sys
class RemoteZip(io.RawIOBase):
 def __init__(self, version):
  self.url=f"https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/{version}/v{version}.zip"
  r=urllib.request.urlopen(urllib.request.Request(self.url,headers={"Range":"bytes=0-15"}),timeout=30)
  self.size=int(r.headers["Content-Range"].split("/")[1]); r.close(); self.pos=0; self.cache={}
 def readable(self):return True
 def seekable(self):return True
 def tell(self):return self.pos
 def seek(self,n,whence=0):
  self.pos=n if whence==0 else self.pos+n if whence==1 else self.size+n
  return self.pos
 def read(self,n=-1):
  n=min(n if n>=0 else self.size-self.pos,self.size-self.pos);data=[]
  while n>0:
   block=self.pos//262144;off=self.pos%262144
   if block not in self.cache:
    a=block*262144;b=min(a+262143,self.size-1)
    r=urllib.request.urlopen(urllib.request.Request(self.url,headers={"Range":f"bytes={a}-{b}"}),timeout=30)
    if r.status!=206 or r.headers.get("Content-Range")!=f"bytes {a}-{b}/{self.size}":raise RuntimeError("Invalid range response")
    self.cache[block]=r.read();r.close()
   v=self.cache[block][off:off+n]
   if not v:raise RuntimeError("Short range response")
   data.append(v);self.pos+=len(v);n-=len(v)
  return b"".join(data)
if __name__ == "__main__":
 from pathlib import Path
 import hashlib
 root = Path(__file__).resolve().parents[2]
 sdk = root / "out/qcom-npu/inference/sdk"
 version = "2.50.0.260828"
 files = {
  "lib/hexagon-v79/unsigned/libQnnHtpV79Skel.so": "238675526b2932efe83a66784937e310ce238e76577e16e9c4358e81711e5333",
  "lib/hexagon-v79/unsigned/libQnnHtpV79.so": "1691ce54f93d3624d374111e5264617e214ab7d05aee7ba1b4ea721ce127a1df",
 }
 missing = [name for name, sha in files.items() if not (sdk/name).is_file() or hashlib.sha256((sdk/name).read_bytes()).hexdigest() != sha]
 if not missing and (sdk/"LICENSE.pdf").is_file():
  print("V79 runtime files verified")
  sys.exit(0)
 z = zipfile.ZipFile(RemoteZip(version))
 for name in missing + (["LICENSE.pdf"] if not (sdk/"LICENSE.pdf").is_file() else []):
  data = z.read("qairt/" + version + "/" + name)
  if name in files and hashlib.sha256(data).hexdigest() != files[name]: raise RuntimeError("Runtime hash mismatch: " + name)
  out = sdk/name; out.parent.mkdir(parents=True, exist_ok=True)
  temporary = out.with_name(out.name + ".new")
  temporary.write_bytes(data); temporary.replace(out)
  print("Downloaded", name)
