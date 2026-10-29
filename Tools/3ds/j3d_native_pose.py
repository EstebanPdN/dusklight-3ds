"""Mac-only rest hierarchy/envelope compiler -> native affine graph.

Dynamic animations, callbacks, billboards and instance-base scale conventions
are not silently replaced by the compiled identity-base rest pose.
"""
import hashlib,math,struct
from j3d_native_geometry import sections,span,u16,u32,need,Unsupported
from native_pose_pack import compile_pose

def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]
def mul(a,b):return f32(a*b)
def add(a,b):return f32(a+b)
def identity():return [[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.]]
def trig(short):
    angle=((short&65535)>>3)*f32(6.2831855)/8192
    return f32(math.sin(angle)),f32(math.cos(angle))
def rotation(angles,translation):
    sx,cx=trig(angles[0]);sy,cy=trig(angles[1]);sz,cz=trig(angles[2])
    return [[mul(cz,cy),f32(mul(mul(sx,cz),sy)-mul(cx,sz)),add(mul(mul(cx,cz),sy),mul(sx,sz)),translation[0]],
            [mul(sz,cy),add(mul(mul(sx,sz),sy),mul(cx,cz)),f32(mul(mul(cx,sz),sy)-mul(sx,cz)),translation[1]],
            [-sy,mul(cy,sx),mul(cy,cx),translation[2]]]

def parents(blocks):
    d=blocks.get('INF1');need(d is not None,'Missing source pose hierarchy');span(d,0,24);at=u32(d,20);need(at>=24,'Pose hierarchy offset')
    count=u16(blocks['JNT1'],8);need(0<count<=4096,'Source joint count');frames=[[None,None]];result={}
    for _ in range(32768):
        kind,value=u16(d,at),u16(d,at+2);at+=4;frame=frames[-1]
        if kind==0:
            need(len(frames)==1 and len(result)==count,'Incomplete pose hierarchy');return result
        if kind==1:
            need(frame[1] is not None and len(frames)<129,'Pose hierarchy child owner/depth');frames.append([frame[1],frame[1]])
        elif kind==2:
            need(len(frames)>1,'Pose hierarchy underflow');frames.pop()
        elif kind==16:
            need(value<count and value not in result,'Pose joint index/duplicate');result[value]=frame[0];frame[1]=value
        elif kind not in (17,18):raise Unsupported('Unknown source pose hierarchy command')
    raise Unsupported('Source pose hierarchy work bound')

def convert(data):
    blocks=sections(data);tree=parents(blocks);style=u16(blocks['INF1'],8)&15;need(style<=2,'Unsupported source transform style')
    d=blocks['JNT1'];base=u32(d,12);remap=u32(d,16);need(base>=24 and remap>=24,'Source joint table offsets');span(d,remap,len(tree)*2)
    nodes=[];outputs={};states={};scales={};cumulative={};unit={};correspondence=[]
    for joint,parent in tree.items():
        at=base+64*u16(d,remap+2*joint);span(d,at,64)
        kind=u16(d,at)&255;need(not kind>>4,'Billboard requires native camera producer');compensate=d[at+2];need(compensate in (0,1,255),'Source scale compensation flag')
        scale=list(struct.unpack_from('>3f',d,at+4));angles=list(struct.unpack_from('>3h',d,at+16));translation=list(struct.unpack_from('>3f',d,at+24))
        need(all(math.isfinite(x) for x in scale+translation),'Nonfinite source joint')
        previous=cumulative[parent] if parent is not None else [1.,1.,1.];total=[mul(a,b) for a,b in zip(previous,scale)];scales[joint]=scale;cumulative[joint]=total
        rt=rotation(angles,translation);parent_node=states[parent] if parent is not None else 0
        if style==1:
            for r in range(3):rt[r][3]=mul(translation[r],previous[r])
            nodes.append({'parent':parent_node,'anchor':0,'local':rt});states[joint]=len(nodes);suffix=identity()
            for r in range(3):suffix[r][r]=total[r]
            nodes.append({'parent':states[joint],'anchor':joint+1,'local':suffix});outputs[joint]=len(nodes);unit[joint]=total==[1.,1.,1.]
        else:
            unit[joint]=(scale if style==2 else total)==[1.,1.,1.]
            if not unit[joint]:
                for r in range(3):
                    for c in range(3):rt[r][c]=mul(rt[r][c],scale[c])
            if style==2 and compensate==1:
                inherited=scales[parent] if parent is not None else [1.,1.,1.];need(all(x!=0 for x in inherited),'Zero parent scale compensation')
                for r in range(3):
                    inv=f32(1/inherited[r])
                    for c in range(3):rt[r][c]=mul(rt[r][c],inv)
            nodes.append({'parent':parent_node,'anchor':joint+1,'local':rt});states[joint]=outputs[joint]=len(nodes)
        correspondence.append({'source_joint':joint,'native_state_node':states[joint],'native_anchor_node':outputs[joint]})
    dr=blocks['DRW1'];count=u16(dr,8);need(count>0,'Empty source draw matrices');need(u32(dr,12)>=20 and u32(dr,16)>=20,'Source draw table offsets');flags=span(dr,u32(dr,12),count);indices=span(dr,u32(dr,16),count*2);recipes=[];envelopes={}
    ev=blocks.get('EVP1');env_count=u16(ev,8) if ev is not None else 0
    if env_count:
        need(all(u32(ev,at)>=28 for at in [12,16,20,24]),'Source envelope table offsets')
        counts=span(ev,u32(ev,12),env_count);total=sum(counts);need(total<=65536 and all(counts),'Source envelope work bound');ix=u32(ev,16);weights=u32(ev,20);inverse=u32(ev,24);span(ev,ix,total*2);span(ev,weights,total*4);cursor=0
        for number,size in enumerate(counts):
            terms=[];normal='linear'
            for k in range(size):
                joint=u16(ev,ix+2*cursor);need(joint in outputs,'Envelope joint index');span(ev,inverse+joint*48,48);matrix=list(struct.unpack_from('>12f',ev,inverse+joint*48));weight=struct.unpack_from('>f',ev,weights+4*cursor)[0];need(math.isfinite(weight) and 0<=weight<=1,'Source envelope weight')
                terms.append({'node':outputs[joint],'weight':weight,'inverse_bind':[matrix[r*4:r*4+4] for r in range(3)]});cursor+=1
                if not unit[joint]:normal='inverse_transpose'
            envelopes[number]={'terms':terms,'normal':normal}
    for n,flag in enumerate(flags):
        index=u16(indices,2*n);need(flag in (0,1),'Source draw matrix flag')
        if flag:need(index in envelopes,'Source envelope index');recipes.append(envelopes[index])
        else:need(index in outputs,'Source rigid joint index');recipes.append({'node':outputs[index],'normal':'linear' if unit[index] else 'inverse_transpose'})
    document={'schema':'dusklight-native-pose/1','nodes':nodes,'matrices':recipes};packed=compile_pose(document)
    return document,packed,{'source_sha256':hashlib.sha256(data).hexdigest(),'native_sha256':hashlib.sha256(packed).hexdigest(),'source_transform_style':style,'joint_correspondence':correspondence,'nodes':len(nodes),'matrices':len(recipes),'blend_terms':sum(len(r.get('terms',[])) for r in recipes),'basis':'original rest pose with identity model base/scale; no view matrix','animation_converted':False,'billboards_converted':False,'callbacks_converted':False,'rendering_ready':False,'arithmetic':'Original table-angle/float32 model on Mac; native composition uses bounded double intermediates, not a bit-exact PPC promise.'}
