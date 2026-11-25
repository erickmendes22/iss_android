"""Keep the update key offline. Exchange only a verified APK signing block."""
import argparse
import base64
import hashlib
import json
import struct
from pathlib import Path

def directory(apk):
    end = apk.rfind(b'PK\x05\x06', max(0,len(apk)-65557))
    if end < 0 or end+22+int.from_bytes(apk[end+20:end+22],'little') != len(apk):
        raise ValueError('Invalid ZIP end record')
    size,offset = struct.unpack_from('<II',apk,end+12)
    if offset+size!=end or apk[offset:offset+4]!=b'PK\x01\x02':
        raise ValueError('Invalid ZIP central directory')
    return offset,end

def unpack(apk):
    cd,end=directory(apk)
    if apk[cd-16:cd]!=b'APK Sig Block 42':
        raise ValueError('Missing APK signature')
    size,=struct.unpack_from('<Q',apk,cd-24)
    start=cd-size-8
    if start<0 or struct.unpack_from('<Q',apk,start)[0]!=size:
        raise ValueError('Invalid APK signing block')
    return apk[:start]+apk[cd:end+16]+struct.pack('<I',start)+apk[end+20:],apk[start:cd]

def apply(unsigned,block):
    cd,end=directory(unsigned)
    if not 32<=len(block)<=65536 or block[-16:]!=b'APK Sig Block 42':
        raise ValueError('Invalid detached APK signature')
    return unsigned[:cd]+block+unsigned[cd:end+16]+struct.pack('<I',cd+len(block))+unsigned[end+20:]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('operation',choices=['normalize','request-response','attach'])
    p.add_argument('paths',nargs='+',type=Path)
    args=p.parse_args()
    if args.operation=='normalize':
        source,out=args.paths
        out.write_bytes(unpack(source.read_bytes())[0])
        return
    unsigned,other,out=args.paths
    data=unsigned.read_bytes()
    digest=hashlib.sha256(data).hexdigest()
    if args.operation=='request-response':
        clean,block=unpack(other.read_bytes())
        if data!=clean or len(block)>65536:
            raise ValueError('Signed content differs from the Actions build')
        out.write_text(json.dumps(dict(sha256=digest,block=base64.b64encode(block).decode('ascii'))))
    else:
        response=json.loads(other.read_text())
        if response['sha256']!=digest:
            raise ValueError('Signature response is for a different build')
        out.write_bytes(apply(data,base64.b64decode(response['block'],validate=True)))

if __name__=='__main__':
    main()
