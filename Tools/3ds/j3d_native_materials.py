"""Mac-only MAT3 fragment compiler. No source interpreter on 3DS.

Output is a native fragment template, not a complete original material instance.
Normalized native combiner arithmetic is explicit; integer GX equivalence and
source lighting/texgen/fog/animation bindings are not certified by this compiler.
"""
from fractions import Fraction as F
from collections import deque
import hashlib
from j3d_native_geometry import Unsupported,need,sections,u16
from j3d_material_source import materials
from native_material_pack import compile_materials

def const(values):return ('const',tuple(F(x) for x in values))
def is_const(x):return x[0]=='const'
def constant(x,value):return is_const(x) and all(c==value for c in x[1])
def leaf(source,channel):return ('leaf',source,channel,False)
def bounded(x):
    if is_const(x):return min(x[1]),max(x[1])
    return F(0),F(1) # Every emitted native arithmetic node is clamped.

def invert(x):
    if is_const(x):return const(1-c for c in x[1])
    if x[0]=='leaf':return (*x[:3],not x[3])
    return calc('subtract',(const([1]*3),x))

def calc(function,args,scale=0):
    if function=='modulate':
        if any(constant(x,0) for x in args):return const([0]*len(args[0][1])) if is_const(args[0]) else const([0]*len(args[1][1]))
        if constant(args[0],1):return scaled(args[1],scale)
        if constant(args[1],1):return scaled(args[0],scale)
    if function=='add':
        if constant(args[0],0):return scaled(args[1],scale)
        if constant(args[1],0):return scaled(args[0],scale)
    if function=='subtract' and constant(args[1],0):return scaled(args[0],scale)
    if all(is_const(x) for x in args):
        values=[]
        for a,b in zip(args[0][1],args[1][1]):values.append(a*b if function=='modulate' else a+b if function=='add' else a-b)
        return const(max(F(0),min(F(1),v*(1<<scale))) for v in values)
    return ('calc',function,tuple(args),scale)

def scaled(x,scale):
    if not scale:return x
    if is_const(x):return const(max(F(0),min(F(1),c*(1<<scale))) for c in x[1])
    if x[0]=='calc' and not x[3]:return (*x[:3],scale)
    return ('calc','replace',(x,),scale)

def mix(a,b,c):
    if a is b or (a[0]==b[0] and a[0] in ('const','leaf') and a==b):return a
    if constant(c,0):return a
    if constant(c,1):return b
    if constant(a,0):return calc('modulate',(b,c))
    if constant(b,0):return calc('modulate',(a,invert(c)))
    if is_const(a) and is_const(b):
        delta=[y-x for x,y in zip(a[1],b[1])]
        if all(x>=0 for x in delta):return calc('add',(a,calc('modulate',(const(delta),c))))
        if all(x<=0 for x in delta):return calc('subtract',(a,calc('modulate',(const(-x for x in delta),c))))
    return ('calc','interpolate',(b,a,c),0)

def colorize(x,cache=None):
    cache={} if cache is None else cache
    if id(x) in cache:return cache[id(x)]
    result=const([x[1][0]]*3) if is_const(x) else x if x[0]=='leaf' else ('calc',x[1],tuple(colorize(y,cache) for y in x[2]),x[3])
    cache[id(x)]=result;return result

def raw_range(function,args):
    a,b=bounded(args[0]),bounded(args[1])
    if function=='add':return a[0]+b[0],a[1]+b[1]
    if function=='subtract':return a[0]-b[1],a[1]-b[0]
    return min(x*y for x in a for y in b),max(x*y for x in a for y in b)

def stage_value(a,b,c,d,operation):
    op,bias,scale,clamp,destination=operation
    if op not in (0,1) or bias or scale>2:raise Unsupported(f'TEV compare/bias/divide scale needs native lowering: op={op}, bias={bias}, scale={scale}')
    need(clamp in (0,1) and destination<4,'Source TEV output flags')
    for value in (a,b,c,d):
        lo,hi=bounded(value)
        if lo<0 or hi>1:raise Unsupported('Signed/out-of-byte TEV input requires native wrap lowering')
    body=mix(a,b,c)
    if not clamp:
        lo,hi=raw_range('add' if op==0 else 'subtract',(d,body))
        if lo*(1<<scale)<0 or hi*(1<<scale)>1:raise Unsupported('Unclamped TEV result requires exact byte-wrap lowering')
    result=calc('add' if op==0 else 'subtract',(d,body),scale)
    return result,destination

KONST_BYTES=(255,223,191,159,128,96,64,32)

def konst(material,selector,alpha,parameterized=False):
    if selector<8:return const([F(KONST_BYTES[selector],255)]*(1 if alpha else 3))
    if not alpha and 12<=selector<=15:
        if parameterized:return leaf(('parameter',3+selector-12),'color')
        return const(F(c,255) for c in material['konst'][selector-12][:3])
    if 16<=selector<=31:
        if parameterized:return leaf(('parameter',3+(selector-16)%4),['red','green','blue','alpha'][(selector-16)//4])
        value=F(material['konst'][(selector-16)%4][(selector-16)//4],255)
        return const([value] if alpha else [value]*3)
    raise Unsupported('Source konst selector')

def swizzled(source,mapping,alpha):
    channels=tuple(mapping[3:] if alpha else mapping[:3])
    if not alpha and channels==(0,1,2):return leaf(source,'color')
    if len(set(channels))==1:return leaf(source,['red','green','blue','alpha'][channels[0]])
    raise Unsupported('RGB swap permutation needs native texture/combiner conversion')

def expressions(material,parameterized=False):
    rgb=[None]+[leaf(('initial',n),'color') for n in range(3)]
    alpha=[None]+[leaf(('initial',n),'alpha') for n in range(3)]
    def initial(value,wrap):
        if value[0]!='leaf' or value[1][0]!='initial':return value
        n=value[1][1];channel=value[2];components=(0,1,2) if channel=='color' else (3,)
        if parameterized:return leaf(('parameter',n,'wrap' if wrap else 'signed'),channel)
        values=[material['registers'][n][c] for c in components]
        return const(F(v&255 if wrap else v,255) for v in values)
    for stage in material['stages']:
        slot=stage['texture_slot'];coord=stage['coordinate']
        image=material['textures'][slot] if slot<8 else None
        tex=('texture',image,coord,slot)
        selector=stage['raster'];raster=('primary',selector&1 if selector in (0,1,2,3,4,5) else selector)
        def raster_argument(scalar):
            if selector==6:return const([0]*(1 if scalar else 3))
            if selector in (2,3):return leaf(raster,'alpha')
            return swizzled(raster,stage['raster_swap'],scalar)
        def argument(code,is_alpha,wrap):
            if is_alpha:
                if code<4:
                    if alpha[code] is None:raise Unsupported('Initial PREV alpha is not defined by the material')
                    return initial(alpha[code],wrap)
                if code==4:return swizzled(tex,stage['texture_swap'],True)
                if code==5:return raster_argument(True)
                if code==6:return konst(material,stage['konst_alpha_selector'],True,parameterized)
                need(code==7,'Source alpha argument');return const([0])
            if code<8:
                register=code//2;value=alpha[register] if code%2 else rgb[register]
                if value is None:raise Unsupported('Initial PREV color is not defined by the material')
                value=initial(value,wrap)
                return colorize(value) if code%2 else value
            if code in (8,9):return swizzled(tex,stage['texture_swap'],code==9)
            if code in (10,11):return raster_argument(code==11)
            if code==12:return const([1]*3)
            if code==13:return const([F(128,255)]*3)
            if code==14:return konst(material,stage['konst_color_selector'],False,parameterized)
            need(code==15,'Source color argument');return const([0]*3)
        # Both lanes read the same pre-stage register state.
        color,cd=stage_value(*(argument(x,False,n<3) for n,x in enumerate(stage['rgb'])),stage['rgb_op'])
        av,ad=stage_value(*(argument(x,True,n<3) for n,x in enumerate(stage['alpha'])),stage['alpha_op'])
        rgb[cd],alpha[ad]=color,av
    if rgb[0] is None or alpha[0] is None:raise Unsupported('Final PREV result undefined')
    return rgb[0],alpha[0]

def texture_sources(expr,found,seen=None):
    seen=set() if seen is None else seen
    if id(expr) in seen:return
    seen.add(id(expr))
    if len(seen)>1024:raise Unsupported('Native expression work bound exceeded')
    if expr[0]=='leaf':
        source=expr[1]
        if source[0]=='texture':found.add(source[1:])
        elif source[0]!='parameter' and not (source[0]=='primary' and source[1] in (0,1,2,3,4,5)):raise Unsupported('Unsupported raster channel selector')
    elif expr[0]=='calc':
        for child in expr[2]:texture_sources(child,found,seen)

def native_instruction(fn,args,scale,units,alpha,storage):
    sources=[];operands=[];constantValue=None;binding=None;constantSpec=None
    for arg in args:
        if id(arg) in storage:source,channel,complement=storage[id(arg)],'alpha' if alpha else 'color',False
        elif is_const(arg):
            values=tuple(max(0,min(255,round(c*255))) for c in arg[1])
            spec=('literal',values)
            if constantSpec is not None and constantSpec!=spec:raise Unsupported('Two distinct constants need native constant scheduling')
            constantSpec=spec;constantValue=values;source,channel,complement='constant','alpha' if alpha else 'color',False
        else:
            need(arg[0]=='leaf','Native expression leaf');native=arg[1]
            channel,complement=arg[2:]
            if alpha and channel=='color':raise Unsupported('Vector value used as scalar alpha')
            if native[0]=='parameter':
                channels=(0,1,2) if channel=='color' else ({'red':0,'green':1,'blue':2,'alpha':3}[channel],)*(1 if alpha else 3)
                selected=tuple(1+native[1]*4+c for c in channels);spec=('parameter',selected)
                if constantSpec is not None and constantSpec!=spec:raise Unsupported('Two distinct constants need native constant scheduling')
                constantSpec=spec;binding=selected;source,channel='constant','alpha' if alpha else 'color'
            elif native[0]=='primary':
                source='buffer' if native[1] in (1,3,5) else 'primary'
            else:source='texture'+str(units[native[1:]])
        sources.append(source);operands.append(('oneMinus'+channel[0].upper()+channel[1:]) if complement else channel)
    while len(sources)<3:sources.append('primary');operands.append('alpha' if alpha else 'color')
    return {'sources':sources,'operands':operands,'function':fn,'scale':scale,'constant':constantValue,'binding':binding}

def schedule(expr,units,alpha=False):
    result=[]
    def emit(node):
        if node[0]=='calc':
            fn,args,scale=node[1:];complex_nodes={id(arg):arg for arg in args if arg[0]=='calc'}
            if len(complex_nodes)>1:raise Unsupported('Branched TEV expression needs native temporary allocation')
            previous=next(iter(complex_nodes.values()),None)
            if previous is not None:emit(previous)
        else:fn,args,scale,previous='replace',(node,),0,None
        result.append(native_instruction(fn,args,scale,units,alpha,{id(previous):'previous'} if previous is not None else {}))
    emit(expr);return result

def schedule_buffer(expr,units,alpha=False):
    """Bounded Mac-only allocation of PREVIOUS and the delayed native buffer.

    A mask bit for stage k stores its output for stage k+2, not k+1.
    Stage0's buffer input is not used, nor is the authored initial buffer.
    This generates fixed descriptors; no search/graph crosses the runtime ABI.
    """
    nodes=[];seen=set()
    def collect(node):
        if id(node) in seen:return
        seen.add(id(node))
        if len(seen)>1024:raise Unsupported('Native expression work bound exceeded')
        if node[0]=='calc':
            for child in node[2]:collect(child)
        if node[0] in ('calc','const') or node[0]=='leaf' and node[1][0]=='parameter' or node is expr:nodes.append(node)
    collect(expr)
    if len(nodes)>64:raise Unsupported('Native temporary node budget exceeded')
    root=next(n for n,node in enumerate(nodes) if node is expr)
    queue=deque([(0,-1,-1,-1,[])])
    visited={(0,-1,-1,-1)}
    while queue:
        depth,previous,buffer,pending,path=queue.popleft()
        if previous==root:return path
        if depth==6:continue
        storage={}
        if buffer>=0:storage[id(nodes[buffer])]='buffer'
        if previous>=0:storage[id(nodes[previous])]='previous'
        for index,node in enumerate(nodes):
            if node[0]=='calc':
                fn,args,scale=node[1:]
                if any(arg[0]=='calc' and id(arg) not in storage for arg in args):continue
            else:fn,args,scale='replace',(node,),0
            try:item=native_instruction(fn,args,scale,units,alpha,storage)
            except Unsupported:continue # A second distinct constant may need its own load.
            for update in (False,True) if depth<4 else (False,):
                # At the next stage the old pending value becomes visible;
                # this stage's new store is pending for one further stage.
                state=(depth+1,index,pending,index if update else pending)
                if state in visited:continue
                visited.add(state)
                if len(visited)>20000:raise Unsupported('Native temporary search budget exceeded')
                queue.append((*state,path+[dict(item,buffer_update=update)]))
    raise Unsupported('Native temporary allocation exceeds six stages')

def native_schedule(expr,units,alpha,allow_temporaries):
    try:return schedule(expr,units,alpha)
    except Unsupported as error:
        if not allow_temporaries or str(error) not in ('Branched TEV expression needs native temporary allocation','Two distinct constants need native constant scheduling'):raise
        return schedule_buffer(expr,units,alpha)

def predicate(code,value,reference):
    need(0<=code<8,'Source comparison')
    return [False,value<reference,value==reference,value<=reference,value>reference,value!=reference,value>=reference,True][code]

def alpha_test(data):
    a,ra,op,b,rb=data[:5];need(op<4,'Source alpha boolean op')
    truth=0
    for value in range(256):
        x,y=predicate(a,value,ra),predicate(b,value,rb)
        if [x and y,x or y,x!=y,x==y][op]:truth|=1<<value
    names=['never','less','equal','lessEqual','greater','notEqual','greaterEqual','always']
    for code in [7,0,6,4,3,1,2,5]:
        for ref in range(256):
            full=(1<<256)-1;low=(1<<ref)-1;through=(1<<(ref+1))-1;equal=1<<ref
            mask=[0,low,equal,through,full^through,full^equal,full^low,full][code]
            if mask==truth:return names[code],ref
    raise Unsupported('Alpha predicate needs native interval/multipass handling')


def consumed_inputs(color,alpha,material):
    raster=[0,0];reads=[0]*7;wraps=[0]*7;signed=[0]*7;seen=set()
    def visit(node):
        if id(node) in seen:return
        seen.add(id(node))
        if node[0]=='calc':
            for child in node[2]:visit(child)
        elif node[0]=='leaf':
            source,channel=node[1:3];mask=7 if channel=='color' else 1<<{'red':0,'green':1,'blue':2,'alpha':3}[channel]
            if source[0]=='primary':
                need(source[1] in (0,1),'Consumed raster channel is not a native RGBA raster')
                raster[source[1]]|=mask
            if source[0]=='parameter':
                n=source[1];reads[n]|=mask
                if n<3:
                    (wraps if source[2]=='wrap' else signed)[n]|=mask
    visit(color);visit(alpha)
    for n,mask in enumerate(signed[:3]):
        for c in range(4):
            if mask&(1<<c) and not 0<=material['registers'][n][c]<=255:
                raise Unsupported(f'Initial S10 register {n} component {c} consumed as signed D is outside native RGBA8: {material["registers"][n][c]}')
    for n,mask in enumerate(reads[:3]):
        for c in range(4):
            if mask&(1<<c):need(-1024<=material['registers'][n][c]<=1023,'Consumed initial register is outside signed S10')
    # Mixed A/B/C and D reads require a byte-valued producer; only exclusively
    # wrapped components may accept arbitrary signed S10 values at runtime.
    wraps=[w&~s for w,s in zip(wraps,signed)]
    return raster,reads,wraps,signed

def color_lookup_images():
    images=[]
    for plane in range(2):
        data=bytearray(256)
        for y in range(8):
            for x in range(8):
                rgba=(0,0,255 if x else 0,255 if y else 0) if plane else (255 if x else 0,255 if y else 0,0,0)
                texel=sum(((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1) for bit in range(3))
                data[4*texel:4*texel+4]=bytes(reversed(rgba))
        images.append({'key':'raster1_'+('ba' if plane else 'rg'),'kind':'raster_color','width':8,'height':8,'format':'rgba8','mip_levels':1,
            'wrap_s':'clamp','wrap_t':'clamp','linear':True,'data_hex':data.hex(),
            'texgen':{'raster':1,'components':[2,3] if plane else [0,1],'scale':[0.125,-0.125],'bias':[0.0625,0.9375]},
            'sampler':{'wrap_s':'clamp','wrap_t':'clamp','magnify':'linear','minify':'linear','mipmap':'nearest','first_level':0,'last_level':0,'lod_bias':0}})
    return images

def fog_lookup_image():
    data=bytearray(8*256*4)
    for y in range(256):
        for x in range(8):
            texel=(y//8)*64+sum(((x>>bit)&1)<<(2*bit)|(((y%8)>>bit)&1)<<(2*bit+1) for bit in range(3))
            data[4*texel]=255-y # PICA T increases from the bottom of this authored row layout.
    return {'key':'fog_factor','kind':'fog_factor','width':8,'height':256,'format':'rgba8','mip_levels':1,
        'wrap_s':'clamp','wrap_t':'clamp','linear':True,'data_hex':data.hex(),
        'texgen':{'fog_factor':True,'scale':[0,255/256],'bias':[0.5,0.5/256]},
        'sampler':{'wrap_s':'clamp','wrap_t':'clamp','magnify':'linear','minify':'linear','mipmap':'nearest','first_level':0,'last_level':0,'lod_bias':0}}


def raster_requirements(material,raster,bindings):
    attributes=1<<9;channel_mask=0;lights=0;lighting=False
    channels=material['channels']
    for r,mask in enumerate(raster):
        if mask:need(r<material['channel_count'],'Consumed raster exceeds declared source color channel count')
        for scalar,components in [(False,7),(True,8)]:
            if not mask&components:continue
            native=r+(2 if scalar else 0);channel_mask|=1<<native
            enabled,mat,light,diffuse,attenuation,ambient=channels[2*r+scalar][:6]
            need(enabled<2 and mat<2 and ambient<2 and diffuse<3 and attenuation<3,'Consumed source channel control')
            if mat or enabled and ambient:attributes|=1<<(11+r)
            if enabled:
                lighting=True;lights|=light
                if light and (diffuse!=0 or attenuation!=2):attributes|=1<<10
    for binding in bindings:
        if binding.get('synthetic'):continue
        src=binding['source_generator'][1]
        if src==0:attributes|=1<<9
        elif src in (1,2,3):attributes|=1<<10
        elif 4<=src<=11:attributes|=1<<(13+src-4)
    return {'source_attributes':attributes,'channel_read_mask':channel_mask,'raster_read_masks':raster,
        'light_mask':lights,'requires_lighting':lighting,'requires_texgen':any(not b.get('synthetic') for b in bindings),
        'source_channels':channels,'source_channel_order':['rgb0','alpha0','rgb1','alpha1'],'source_channel_count':material['channel_count']}

def compile_material(material,*,allow_normalized_arithmetic=False,specialize_initial_constants=False,preserve_color_parameters=False,allow_native_temporaries=False,synthetic_image_base=None):
    if not allow_normalized_arithmetic:raise Unsupported('Source integer equivalence unproved; native normalized arithmetic must be selected explicitly')
    if specialize_initial_constants and preserve_color_parameters:raise Unsupported('Choose either snapshot specialization or native color parameters')
    if not specialize_initial_constants and not preserve_color_parameters:raise Unsupported('Initial constants are not proven immutable; snapshot specialization must be selected explicitly')
    if 'error' in material:raise Unsupported(material['error'])
    color,alpha=expressions(material,preserve_color_parameters);pairs=set();texture_sources(color,pairs);texture_sources(alpha,pairs)
    raster,reads,wraps,signed=consumed_inputs(color,alpha,material);lookup=bool(raster[1])
    if lookup and len(pairs)>1:raise Unsupported('Raster1 RGBA lookup requires two spare native texture units; source consumes '+str(len(pairs)))
    if len(pairs)>3:raise Unsupported('More than three native texture bindings')
    bindings=[]
    for image,coordinate,slot in pairs:
        if image is None or image==65535 or coordinate>=len(material['generators']):raise Unsupported('Unresolved source texture/coordinate binding')
        generator=material['generators'][coordinate];projective=generator[0]==0 and generator[1]==0
        if not projective and not (generator[0]==1 and 4<=generator[1]<=6):raise Unsupported('Source texgen needs another native pipeline')
        bindings.append({'image':image,'coordinate':coordinate,'texture_slot':slot,'texture_generator':coordinate,'projective':projective,'source_generator':generator})
    bindings.sort(key=lambda x:(not x['projective'],x['image'],x['coordinate'],x['texture_slot']))
    if sum(b['projective'] for b in bindings)>1:raise Unsupported('Multiple projected bindings require native multipass lowering')
    units={(b['image'],b['coordinate'],b['texture_slot']):n for n,b in enumerate(bindings)}
    rgb,alp=native_schedule(color,units,False,allow_native_temporaries and not lookup),native_schedule(alpha,units,True,allow_native_temporaries and not lookup);count=max(len(rgb),len(alp))
    if count>6:raise Unsupported('Native combiner stage budget exceeded')
    stages=[]
    for n in range(count):
        stage={};rgba=[255]*4;constantBindings=[0]*4
        for name,sequence in [('rgb',rgb),('alpha',alp)]:
            item=sequence[n] if n<len(sequence) else {'sources':['previous','primary','primary'],'operands':['color' if name=='rgb' else 'alpha']*3,'function':'replace','scale':0,'constant':None}
            stage[name]=item['sources'];stage[name+'_operand']=item['operands'];stage[name+'_function']=item['function'];stage[name+'_scale']=item['scale']
            if item['constant'] is not None:
                if name=='rgb':rgba[:3]=item['constant']
                else:rgba[3]=item['constant'][0]
            if item.get('binding') is not None:
                if name=='rgb':constantBindings[:3]=item['binding']
                else:constantBindings[3]=item['binding'][0]
        stage['constant']=sum(c<<(8*n) for n,c in enumerate(rgba));stages.append(stage)
        if any(constantBindings):stage['constant_bindings']=constantBindings
    synthetic=[];base=len(bindings)
    source_image_count=max((x for x in material['textures'] if x is not None and x!=65535),default=-1)+1
    if synthetic_image_base is None:synthetic_image_base=source_image_count
    if lookup:
        if count>4:raise Unsupported(f'Raster1 lookup needs two preparation stages; original native equation needs {count}, maximum 4')
        need(type(synthetic_image_base) is int and 0<=synthetic_image_base<=0xfffffffc,'Synthetic image ID exceeds native uint32 range')
        need(synthetic_image_base>=source_image_count,'Synthetic image base overlaps original texture IDs')
        synthetic=color_lookup_images()
        for plane,image in enumerate(synthetic):
            image['image']=synthetic_image_base+plane;image['id']=image['image']+1
            bindings.append({'image':image['image'],'coordinate':255,'texture_slot':255,'texture_generator':255,'projective':False,'synthetic':image['key']})
        prepare={'rgb':['texture'+str(base),'texture'+str(base+1),'primary'],'alpha':['texture'+str(base+1),'primary','primary'],
            'rgb_operand':['color']*3,'alpha_operand':['alpha']*3,'rgb_function':'add','alpha_function':'replace','rgb_scale':0,'alpha_scale':0,'constant':0}
        delay={'rgb':['previous','primary','primary'],'alpha':['previous','primary','primary'],'rgb_operand':['color']*3,'alpha_operand':['alpha']*3,
            'rgb_function':'replace','alpha_function':'replace','rgb_scale':0,'alpha_scale':0,'constant':0}
        stages=[prepare,delay]+stages
    states=material['states'];blend=states['blend'];depth=states['depth'];cull=states['cull']
    if cull>2:raise Unsupported('Cull-all requires native draw visibility lowering')
    if blend[0] not in (0,1,3):raise Unsupported('Source logic blend requires native state lowering')
    factors=['zero','one','sourceColor','oneMinusSourceColor','sourceAlpha','oneMinusSourceAlpha','destinationAlpha','oneMinusDestinationAlpha']
    need(blend[1]<8 and blend[2]<8 and depth[0]<2 and depth[2]<2,'Source blend/depth flags')
    source,destination=(factors[blend[1]],factors[blend[2]]) if blend[0]==1 else ('one','one') if blend[0]==3 else ('one','zero')
    if blend[0]==1:
        if blend[1] in (2,3):source=['destinationColor','oneMinusDestinationColor'][blend[1]-2]
        # GX's overlapping CLR enum has distinct source/destination meanings.
    alpha_source={'destinationColor':'destinationAlpha','oneMinusDestinationColor':'oneMinusDestinationAlpha'}.get(source,source)
    alpha_destination={'sourceColor':'sourceAlpha','oneMinusSourceColor':'oneMinusSourceAlpha'}.get(destination,destination)
    compare=['never','less','equal','lessEqual','greater','notEqual','greaterEqual','always'];need(depth[1]<8,'Source depth comparison')
    reversed_depth={'less':'greater','lessEqual':'greaterEqual','greater':'less','greaterEqual':'lessEqual'}
    native_depth=reversed_depth.get(compare[depth[1]],compare[depth[1]])
    native_alpha,reference=alpha_test(states['alpha'])
    if states['early_depth'] and depth[2] and native_alpha!='always':raise Unsupported('Observable pre-alpha depth writes require native pass lowering')
    equation='reverseSubtract' if blend[0]==3 else 'add'
    # GX and native rendering use opposite front/back cull mappings for this winding.
    record={'id':material['id'],'stages':stages,'blend':'custom','custom_blend':{'color':equation,'alpha':equation,'color_source':source,'color_destination':destination,'alpha_source':alpha_source,'alpha_destination':alpha_destination},
        'cull':['none','back','front'][cull],'depth':native_depth,'depth_test':bool(depth[0]),'depth_write':bool(depth[2]),'alpha':native_alpha,'alpha_reference':reference,
        'requires_fog':bool(states['fog'][0] or states['fog'][1]),'initial_snapshot_only':not preserve_color_parameters}
    for lane,sequence in [('rgb',rgb),('alpha',alp)]:
        mask=sum(1<<n for n,item in enumerate(sequence) if item.get('buffer_update',False))
        if mask:record['buffer_'+lane+'_mask']=mask
    if lookup:
        record.update(raster_lookup=True,lookup_base=base,buffer_rgb_mask=1,buffer_alpha_mask=1)
    fog_carrier='none';fog=states['fog']
    need(fog[1] in (0,1),'Source fog range enable')
    if fog[0] not in (0,2,4,5,6,7,10,12,13,14,15):raise Unsupported('Source fog function has no native equation')
    if record['requires_fog']:
        if len(stages)>=6:raise Unsupported('Source fog requires one final fragment stage; native six-stage budget exhausted')
        # Only the source expression, not vertex colors or a gray-color guess,
        # proves that replacing this independently interpolated component is safe.
        source='primary'
        if not raster[0]&8:fog_carrier='primaryAlpha';operand='alpha'
        elif not raster[0]&1:fog_carrier='primaryRed';operand='red'
        else:
            unit=len(bindings)
            if unit>=3:raise Unsupported(f'Source fog consumes primary alpha/red and needs one spare texture unit; {len(pairs)} source bindings and {2 if lookup else 0} raster lookup bindings occupy all three')
            need(type(synthetic_image_base) is int and 0<=synthetic_image_base<=0xfffffffc,'Synthetic image ID exceeds native uint32 range')
            need(synthetic_image_base>=source_image_count,'Synthetic image base overlaps original texture IDs')
            image=fog_lookup_image();image['image']=synthetic_image_base+2;image['id']=image['image']+1;synthetic.append(image)
            bindings.append({'image':image['image'],'coordinate':255,'texture_slot':255,'texture_generator':255,'projective':False,'synthetic':image['key']})
            fog_carrier=source='texture'+str(unit);operand='alpha'
        stages.append({'rgb':['constant','previous',source],'alpha':['previous','primary','primary'],
            'rgb_operand':['color','color',operand],'alpha_operand':['alpha']*3,'rgb_function':'interpolate','alpha_function':'replace',
            'rgb_scale':0,'alpha_scale':0,'constant':0,'constant_bindings':[29,30,31,32]})
        record['fog_carrier']=fog_carrier
    images=[0]*3
    for n,binding in enumerate(bindings):images[n]=binding['image']+1
    record['images']=images
    requirements=raster_requirements(material,raster,bindings)
    for unit,binding in enumerate(bindings):binding['native_unit']=unit
    return record,{'id':material['id'],'source_stage_count':len(material['stages']),'source_texgen_count':len(material['generators']),'native_stage_count':len(stages),'texture_bindings':bindings,'requires_fog':record['requires_fog'],
        **requirements,'synthetic_images':synthetic,'raster_lookup':lookup,'lookup_base':base if lookup else 0,'fog_carrier':fog_carrier,
        'parameter_read_masks':reads,'parameter_wrap_masks':wraps,'parameter_signed_masks':signed,
        'source_indirect':material.get('indirect'),
        'readied_delta':material.get('readied_delta'),
        'channel_producer_policy':('Runtime channel/light producers apply g_env_light.light_mask_type to source rgb0; source_channels/light_mask describe the unfiltered source upper bound, not a fixed readied mask' if material.get('readied_delta') else 'Source declarations; runtime channel/light producers remain external'),
        'source_fog':{'type':fog[0],'range_enabled':bool(fog[1]),'center':int.from_bytes(bytes(fog[2:4]),'big'),'raw_hex':bytes(fog).hex()},
        'fog_recipe':'interpolated vertex fog factor; RGB lerp only, alpha unchanged; range table required when enabled',
        'fog_source_equivalence':False,
        'konst_fraction_recipe':{'gx_bytes':list(KONST_BYTES),'native':'exact UNORM8 byte transport; native normalized arithmetic, not GX integer rounding','source_integer_equivalence':False},
        'source_early_depth':states['early_depth'],'source_dither':states['dither'],'native_framebuffer_precision_validated':False,
        'blend_context':'RGBA-like native target, without source destination-alpha override; original framebuffer policy remains unconverted',
        'initial_snapshot_only':not preserve_color_parameters,'runtime_constant_updates_supported':preserve_color_parameters,
        'procedural_color_updates_supported':False,'specialized_source_constants':[] if preserve_color_parameters else ['tev_registers','konst_registers'],
        'native_color_parameter_mapping':({'registers':[0,1,2],'konst':[3,4,5,6],'domain':'read masks only; exclusively A/B/C lanes wrap S10 &255, signed D lanes must remain 0..255'} if preserve_color_parameters else None),
        'native_temporary_scheduling':allow_native_temporaries,'native_buffer_latency_gpu_validated':False,
        'arithmetic':'normalized native combiners','source_integer_equivalence':False,'lighting_texgen_sampler_animation_bindings_converted':False,'rendering_ready':False}

def convert(data,*,allow_normalized_arithmetic=False,specialize_initial_constants=False,preserve_color_parameters=False,allow_native_temporaries=False,declaration=None):
    records=[];rows=[];texture_block=sections(data).get('TEX1');image_base=u16(texture_block,8) if texture_block is not None else 0
    if declaration is not None:
        from j3d_material_source import checked_declaration
        image_base=len(checked_declaration(data,declaration)['images'])
    for source in materials(data,declaration=declaration):
        try:record,row=compile_material(source,allow_normalized_arithmetic=allow_normalized_arithmetic,specialize_initial_constants=specialize_initial_constants,preserve_color_parameters=preserve_color_parameters,allow_native_temporaries=allow_native_temporaries,synthetic_image_base=image_base);records.append(record);row['status']='native_parameter_template' if preserve_color_parameters else 'native_fragment_snapshot'
        except ValueError as error:row={'id':source['id'],'status':'refused','reason':str(error)}
        rows.append(row)
    packed=compile_materials(records) if records else None
    return packed,{'source_sha256':hashlib.sha256(data).hexdigest(),'records':records,'materials':rows,'rendering_ready':False,
        'scope':('Parameterized native fragment templates; consumed initial lanes require producers honoring read/wrap/signed masks. ' if preserve_color_parameters else 'Initial native fragment snapshots only. ')+
            'Normalized arithmetic explicitly selected; raster lookup metadata and alpha-preserving fog carriers compiled. Original integer precision, physical interpolation, live lighting/texgen/sampler/fog/animation producers remain external.'}
