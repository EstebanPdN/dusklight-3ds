"""Host lab formats. Independent ETC1 decoder follows the Khronos bit grammar.

No native/game decoder or rg_etc1 unpack function is used for measurements.
Coordinates match the unflipped native diagnostic RGBA order. PICA ETC1 words
are little endian; 4x4 blocks run TL,TR,BL,BR inside row-major 8x8 tiles.
"""
import math
import struct
import subprocess
import time

FORMATS = {'RGB565': 2, 'RGBA4': 2, 'RGB5A1': 2, 'ETC1': .5, 'ETC1A4': 1}
MODIFIERS = ((2,8,-2,-8),(5,17,-5,-17),(9,29,-9,-29),(13,42,-13,-42),
             (18,60,-18,-60),(24,80,-24,-80),(33,106,-33,-106),(47,183,-47,-183))


def padded(n):
    return max(8, 1 << (n-1).bit_length())


def allocation(w, h, mips, bpp):
    return int(sum(padded(max(1,w>>i))*padded(max(1,h>>i))*bpp for i in range(mips)))


def morton(x, y):
    return sum(((x>>b)&1)<<(2*b) | ((y>>b)&1)<<(2*b+1) for b in range(3))


def alpha_class(rgba):
    a=set(rgba[3::4])
    return 'opaque' if a == {255} else 'binary' if a <= {0,255} else 'smooth'


def encode16(rgba, w, h, fmt):
    pw, ph=padded(w), padded(h)
    out=bytearray(pw*ph*2)
    for y in range(ph):
        for x in range(pw):
            at=(min(y,h-1)*w+min(x,w-1))*4
            r,g,b,a=rgba[at:at+4]
            if fmt=='RGB565': v=((r>>3)<<11)|((g>>2)<<5)|(b>>3)
            elif fmt=='RGB5A1': v=((r>>3)<<11)|((g>>3)<<6)|((b>>3)<<1)|(a>=128)
            elif fmt=='RGBA4': v=((r>>4)<<12)|((g>>4)<<8)|((b>>4)<<4)|(a>>4)
            else: raise ValueError(fmt)
            pos=((y//8*(pw//8)+x//8)*64+morton(x,y))*2
            struct.pack_into('<H',out,pos,v)
    return bytes(out)


def decode16(data, w, h, fmt):
    pw=padded(w)
    if len(data)!=pw*padded(h)*2: raise ValueError('16-bit extent')
    out=bytearray(w*h*4)
    for y in range(h):
        for x in range(w):
            pos=((y//8*(pw//8)+x//8)*64+morton(x,y))*2
            v=int.from_bytes(data[pos:pos+2],'little')
            if fmt=='RGB565':
                r,g,b=v>>11,(v>>5)&63,v&31
                c=((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2),255)
            elif fmt=='RGB5A1':
                r,g,b=v>>11,(v>>6)&31,(v>>1)&31
                c=((r<<3)|(r>>2),(g<<3)|(g>>2),(b<<3)|(b>>2),255*(v&1))
            elif fmt=='RGBA4': c=tuple(((v>>shift)&15)*17 for shift in (12,8,4,0))
            else: raise ValueError(fmt)
            out[(y*w+x)*4:(y*w+x+1)*4]=bytes(c)
    return bytes(out)


def decode_etc1_block(block):
    if len(block)!=8: raise ValueError('ETC1 block extent')
    v=int.from_bytes(block,'big')
    flip=(v>>32)&1
    tables=((v>>37)&7,(v>>34)&7)
    colors=[[],[]]
    for shift in (56,48,40):
        if (v>>33)&1:
            base=(v>>(shift+3))&31
            delta=(v>>shift)&7
            second=base+(delta if delta<4 else delta-8)
            if not 0<=second<=31: raise ValueError('Invalid ETC1 differential')
            colors[0].append((base<<3)|(base>>2))
            colors[1].append((second<<3)|(second>>2))
        else:
            colors[0].append(((v>>(shift+4))&15)*17)
            colors[1].append(((v>>shift)&15)*17)
    out=bytearray()
    for y in range(4):
        for x in range(4):
            sub=int(y>=2) if flip else int(x>=2)
            bit=x*4+y
            selector=((v>>bit)&1)|(((v>>(bit+16))&1)<<1)
            mod=MODIFIERS[tables[sub]][selector]
            out.extend([max(0,min(255,c+mod)) for c in colors[sub]]+[255])
    return bytes(out)


def block_positions(w,h):
    for ty in range(0,padded(h),8):
        for tx in range(0,padded(w),8):
            for by,bx in ((0,0),(0,4),(4,0),(4,4)):
                yield tx+bx,ty+by


def encode_etc(rgba,w,h,fmt,encoder,scratch):
    blocks=bytearray(); alphas=[]
    for bx,by in block_positions(w,h):
        a=0
        for y in range(4):
            for x in range(4):
                at=(min(by+y,h-1)*w+min(bx+x,w-1))*4
                blocks.extend(rgba[at:at+3]+b'\xff')
                a|=(rgba[at+3]>>4)<<(4*(x*4+y))
        alphas.append(a.to_bytes(8,'little'))
    inp,out=scratch/'blocks.rgba',scratch/'blocks.etc'
    inp.write_bytes(blocks)
    start=time.perf_counter()
    run=subprocess.run([str(encoder),'encode',str(inp),str(out)],check=True,capture_output=True,timeout=60)
    wall_ms=(time.perf_counter()-start)*1000
    import json
    timing=json.loads(run.stdout)
    encoded=out.read_bytes()
    if len(encoded)!=len(alphas)*8: raise ValueError('Encoder extent')
    data=b''.join((alphas[i] if fmt=='ETC1A4' else b'')+encoded[i*8:i*8+8][::-1] for i in range(len(alphas)))
    return data,dict(timing,host_process_wall_ms=wall_ms)


def decode_etc(data,w,h,fmt):
    step=16 if fmt=='ETC1A4' else 8
    if len(data)!=allocation(w,h,1,FORMATS[fmt]): raise ValueError('PICA ETC extent')
    out=bytearray(w*h*4)
    for i,(bx,by) in enumerate(block_positions(w,h)):
        chunk=data[i*step:(i+1)*step]
        pixels=decode_etc1_block(chunk[-8:][::-1])
        alpha=int.from_bytes(chunk[:8],'little') if step==16 else None
        for y in range(4):
            for x in range(4):
                if bx+x>=w or by+y>=h: continue
                at=((by+y)*w+bx+x)*4
                src=(y*4+x)*4
                out[at:at+4]=pixels[src:src+4]
                if alpha is not None: out[at+3]=((alpha>>(4*(x*4+y)))&15)*17
    return bytes(out)


def half_box(rgba,w,h):
    # Straight-alpha output, alpha-weighted RGB, linear arithmetic in stored
    # 8-bit channel space. No inferred gamma or texture role. Transparent RGB
    # is retained as an unweighted mean when all four samples have alpha=0.
    nw,nh=max(1,w//2),max(1,h//2)
    if (w>1 and w%2) or (h>1 and h%2): raise ValueError('Half-box requires even axes or 1')
    out=bytearray()
    for y in range(nh):
        for x in range(nw):
            pixels=[rgba[(sy*w+sx)*4:(sy*w+sx+1)*4]
                    for sy in range(2*y,min(h,2*y+2)) for sx in range(2*x,min(w,2*x+2))]
            weight=sum(p[3] for p in pixels); n=len(pixels)
            rgb=[(sum(p[c]*p[3] for p in pixels)+weight//2)//weight if weight else
                 (sum(p[c] for p in pixels)+n//2)//n for c in range(3)]
            out.extend(rgb+[(weight+n//2)//n])
    return bytes(out),nw,nh


def expand_nearest(rgba,w,h,ow,oh):
    return b''.join(rgba[(min(h-1,y*h//oh)*w+min(w-1,x*w//ow))*4: 
                            (min(h-1,y*h//oh)*w+min(w-1,x*w//ow))*4+4]
                    for y in range(oh) for x in range(ow))


def metrics(original,proposed):
    if len(original)!=len(proposed) or not original or len(original)%4: raise ValueError('Metric extent')
    n=len(original)//4; channels={}
    for c,name in enumerate('rgba'):
        errors=[abs(a-b) for a,b in zip(original[c::4],proposed[c::4])]
        mse=sum(e*e for e in errors)/n
        channels[name]={'max_abs':max(errors),'mean_abs':sum(errors)/n,'mse':mse,
                        'psnr_db':10*math.log10(255**2/mse) if mse else None,
                        'lossless':not any(errors)}
    coverage={}
    for threshold in (1,128,255):
        a=[v>=threshold for v in original[3::4]]; b=[v>=threshold for v in proposed[3::4]]
        coverage[str(threshold)]={'original':sum(a)/n,'proposed':sum(b)/n,
                                 'delta':(sum(b)-sum(a))/n,'changed_pixels':sum(x!=y for x,y in zip(a,b))}
    return {'channels':channels,'alpha_coverage_at_least':coverage,
            'psnr_infinity_representation':'null with lossless=true',
            'scope':'all stored channels, including RGB under zero alpha; no perceptual weighting'}
