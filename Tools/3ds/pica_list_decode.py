"""Decode a PICA command list saved by the GPU guard or a dump.

usage: python3 -I pica_list_decode.py list.bin

The input is 32 bytes of GSP job words followed by the list's command words
(the CMDS chunk of a gpu-hang-NN.bin, extracted). Prints every register write
with its PICA register name; long consecutive writes are summarized."""
import struct,sys
N={0x10:'FINALIZE',0x40:'FACECULLING',0x41:'VIEWPORT_W',0x43:'VIEWPORT_H',0x4d:'DEPTHMAP_SCALE',0x4e:'DEPTHMAP_OFFSET',0x4f:'SH_OUTMAP_TOTAL',
0x50:'SH_OUTMAP_O0',0x64:'SH_OUTATTR_MODE',0x65:'SCISSORTEST_MODE',0x68:'VIEWPORT_XY',0x6d:'DEPTHMAP_ENABLE',0x6f:'SH_OUTATTR_CLOCK',
0x80:'TEXUNIT_CONFIG',0x81:'TEXUNIT0_BORDER',0x82:'TEXUNIT0_DIM',0x83:'TEXUNIT0_PARAM',0x84:'TEXUNIT0_LOD',0x85:'TEXUNIT0_ADDR1',0x8e:'TEXUNIT0_TYPE',
0x91:'TEXUNIT1_BORDER',0x92:'TEXUNIT1_DIM',0x93:'TEXUNIT1_PARAM',0x94:'TEXUNIT1_LOD',0x95:'TEXUNIT1_ADDR',0x96:'TEXUNIT1_TYPE',
0x99:'TEXUNIT2_BORDER',0x9a:'TEXUNIT2_DIM',0x9b:'TEXUNIT2_PARAM',0x9c:'TEXUNIT2_LOD',0x9d:'TEXUNIT2_ADDR',0x9e:'TEXUNIT2_TYPE',
0xe0:'TEXENV_UPDATE_BUFFER',0xe1:'FOG_COLOR',0xe6:'FOG_LUT_INDEX',0xe8:'FOG_LUT_DATA0',
0x100:'COLOR_OPERATION',0x101:'BLEND_FUNC',0x102:'LOGIC_OP',0x103:'BLEND_COLOR',0x104:'FRAGOP_ALPHA_TEST',0x105:'STENCIL_TEST',0x106:'STENCIL_OP',0x107:'DEPTH_COLOR_MASK',
0x110:'FRAMEBUFFER_INVALIDATE',0x111:'FRAMEBUFFER_FLUSH',0x112:'COLORBUFFER_READ',0x113:'COLORBUFFER_WRITE',0x114:'DEPTHBUFFER_READ',0x115:'DEPTHBUFFER_WRITE',
0x116:'DEPTHBUFFER_FORMAT',0x117:'COLORBUFFER_FORMAT',0x11b:'FRAMEBUFFER_BLOCK32',0x11c:'DEPTHBUFFER_LOC',0x11d:'COLORBUFFER_LOC',0x11e:'FRAMEBUFFER_DIM',
0x200:'ATTRIBBUFFERS_LOC',0x201:'ATTRIBBUFFERS_FORMAT_LOW',0x202:'ATTRIBBUFFERS_FORMAT_HIGH',0x203:'ATTRIBBUFFER0_OFFSET',0x204:'ATTRIBBUFFER0_CONFIG1',0x205:'ATTRIBBUFFER0_CONFIG2',
0x227:'INDEXBUFFER_CONFIG',0x228:'NUMVERTICES',0x229:'GEOSTAGE_CONFIG',0x22a:'VERTEX_OFFSET',0x22d:'POST_VERTEX_CACHE_NUM',0x22e:'DRAWARRAYS',0x22f:'DRAWELEMENTS',
0x231:'VTX_FUNC',0x232:'FIXEDATTRIB_INDEX',0x233:'FIXEDATTRIB_DATA0',0x238:'CMDBUF_SIZE0',0x23a:'CMDBUF_ADDR0',0x23c:'CMDBUF_JUMP0',
0x242:'VSH_NUM_ATTR',0x244:'VSH_COM_MODE',0x245:'START_DRAW_FUNC0',0x24a:'VSH_OUTMAP_TOTAL1',0x251:'VSH_OUTMAP_TOTAL2',0x252:'GSH_MISC0',0x253:'GEOSTAGE_CONFIG2',0x254:'GSH_MISC1',
0x25e:'PRIMITIVE_CONFIG',0x25f:'RESTART_PRIMITIVE',
0x280:'GSH_BOOLUNIFORM',0x289:'GSH_INPUTBUFFER_CONFIG',0x28a:'GSH_ENTRYPOINT',0x29b:'GSH_CODETRANSFER_END',
0x2b0:'VSH_BOOLUNIFORM',0x2b1:'VSH_INTUNIFORM_I0',0x2b9:'VSH_INPUTBUFFER_CONFIG',0x2ba:'VSH_ENTRYPOINT',0x2bb:'VSH_ATTRIBUTES_PERMUTATION_LOW',0x2bc:'VSH_ATTRIBUTES_PERMUTATION_HIGH',
0x2bd:'VSH_OUTMAP_MASK',0x2bf:'VSH_CODETRANSFER_END',0x2c0:'VSH_FLOATUNIFORM_CONFIG',0x2c1:'VSH_FLOATUNIFORM_DATA',0x2cb:'VSH_CODETRANSFER_CONFIG',0x2cc:'VSH_CODETRANSFER_DATA',0x2d5:'VSH_OPDESCS_CONFIG',0x2d6:'VSH_OPDESCS_DATA'}
def name(r):
    if r in N:return N[r]
    if 0x203<=r<=0x226:return 'ATTRIBBUFFER%d_%s'%((r-0x203)//3,['OFFSET','CONFIG1','CONFIG2'][(r-0x203)%3])
    if 0xc0<=r<=0xfd:return 'TEXENV/%03x'%r
    if 0x1c0<=r<=0x1ff:return 'LIGHTING/%03x'%r
    if 0x2c1<=r<=0x2c8:return 'VSH_FLOATUNIFORM_DATA'
    if 0x2cc<=r<=0x2d3:return 'VSH_CODETRANSFER_DATA'
    if 0x2d6<=r<=0x2dd:return 'VSH_OPDESCS_DATA'
    return '%03x'%r
def decode(words,quiet=set()):
    i=0;out=[]
    while i+1<len(words):
        p,h=words[i],words[i+1];reg=h&0xffff;mask=(h>>16)&0xf;extra=(h>>20)&0xff;seq=h>>31
        params=[p]+words[i+2:i+2+extra]
        out.append((i*4,reg,mask,seq,params))
        i+=2+extra; i+= (i&1)
    return out
if __name__=='__main__':
    data=open(sys.argv[1],'rb').read();hdr=struct.unpack('<8I',data[:32]);words=list(struct.unpack('<%dI'%((len(data)-32)//4),data[32:]))
    print('entry',' '.join('%08x'%w for w in hdr),'list words',len(words))
    for off,reg,mask,seq,params in decode(words):
        regs=[reg+(k if seq else 0) for k in range(len(params))]
        if len(params)>6:print('%05x %-30s mask=%x seq=%d n=%d first=%s'%(off,name(reg),mask,seq,len(params),' '.join('%08x'%x for x in params[:4])))
        else:print('%05x %s'%(off,'  '.join('%s=%08x'%(name(r),x)+('' if mask==0xf else '/m%x'%mask) for r,x in zip(regs,params))))
