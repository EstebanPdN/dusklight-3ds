"""Mac-only MAT3 fragment compiler. No source interpreter on 3DS.

Output is a native fragment template, not a complete original material instance.
Normalized native combiner arithmetic is explicit; integer GX equivalence and
source lighting/texgen/fog/animation bindings are not certified by this compiler.
"""
from fractions import Fraction as F
from collections import deque
import hashlib
from j3d_native_geometry import Unsupported,need
from j3d_material_source import materials
from native_material_pack import compile_materials,COMPARE

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
    if op not in (0,1) or bias or scale>2:raise Unsupported('TEV compare/bias/divide scale needs native lowering')
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

def konst(material,selector,alpha,parameterized=False):
    if selector==0:return const([1] if alpha else [1]*3)
    if selector<8:raise Unsupported('Source konst fraction requires an explicit native precision recipe')
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
    if parameterized:
        if any(not 0<=c<=255 for r in material['registers'][:3]+material['konst'] for c in r):raise Unsupported('Native color parameters require unsigned RGBA8 values')
        rgb=[None]+[leaf(('parameter',n),'color') for n in range(3)]
        alpha=[None]+[leaf(('parameter',n),'alpha') for n in range(3)]
    else:
        rgb=[None]+[const(F(c,255) for c in r[:3]) for r in material['registers'][:3]]
        alpha=[None]+[const([F(r[3],255)]) for r in material['registers'][:3]]
    for stage in material['stages']:
        slot=stage['texture_slot'];coord=stage['coordinate']
        image=material['textures'][slot] if slot<8 else None
        tex=('texture',image,coord)
        raster=('primary',stage['raster'])
        def argument(code,is_alpha):
            if is_alpha:
                if code<4:
                    if alpha[code] is None:raise Unsupported('Initial PREV alpha is not defined by the material')
                    return alpha[code]
                if code==4:return swizzled(tex,stage['texture_swap'],True)
                if code==5:return swizzled(raster,stage['raster_swap'],True)
                if code==6:return konst(material,stage['konst_alpha_selector'],True,parameterized)
                need(code==7,'Source alpha argument');return const([0])
            if code<8:
                register=code//2;value=alpha[register] if code%2 else rgb[register]
                if value is None:raise Unsupported('Initial PREV color is not defined by the material')
                return colorize(value) if code%2 else value
            if code in (8,9):return swizzled(tex,stage['texture_swap'],code==9)
            if code in (10,11):return swizzled(raster,stage['raster_swap'],code==11)
            if code==12:return const([1]*3)
            if code==13:return const([F(128,255)]*3)
            if code==14:return konst(material,stage['konst_color_selector'],False,parameterized)
            need(code==15,'Source color argument');return const([0]*3)
        # Both lanes read the same pre-stage register state.
        color,cd=stage_value(*(argument(x,False) for x in stage['rgb']),stage['rgb_op'])
        av,ad=stage_value(*(argument(x,True) for x in stage['alpha']),stage['alpha_op'])
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
        elif source[0]!='parameter' and source!=('primary',4):raise Unsupported('Raster channel requires another native vertex/lighting output')
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
            else:source='primary' if native[0]=='primary' else 'texture'+str(units[native[1:]])
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

def compile_material(material,*,allow_normalized_arithmetic=False,specialize_initial_constants=False,preserve_color_parameters=False,allow_native_temporaries=False):
    if not allow_normalized_arithmetic:raise Unsupported('Source integer equivalence unproved; native normalized arithmetic must be selected explicitly')
    if specialize_initial_constants and preserve_color_parameters:raise Unsupported('Choose either snapshot specialization or native color parameters')
    if not specialize_initial_constants and not preserve_color_parameters:raise Unsupported('Initial constants are not proven immutable; snapshot specialization must be selected explicitly')
    if 'error' in material:raise Unsupported(material['error'])
    color,alpha=expressions(material,preserve_color_parameters);pairs=set();texture_sources(color,pairs);texture_sources(alpha,pairs)
    if len(pairs)>3:raise Unsupported('More than three native texture bindings')
    bindings=[]
    for image,coordinate in pairs:
        if image is None or image==65535 or coordinate>=len(material['generators']):raise Unsupported('Unresolved source texture/coordinate binding')
        generator=material['generators'][coordinate];projective=generator[0]==0 and generator[1]==0
        if not projective and not (generator[0]==1 and 4<=generator[1]<=6):raise Unsupported('Source texgen needs another native pipeline')
        bindings.append({'image':image,'coordinate':coordinate,'projective':projective,'source_generator':generator})
    bindings.sort(key=lambda x:(not x['projective'],x['image'],x['coordinate']))
    if sum(b['projective'] for b in bindings)>1:raise Unsupported('Multiple projected bindings require native multipass lowering')
    units={(b['image'],b['coordinate']):n for n,b in enumerate(bindings)}
    rgb,alp=native_schedule(color,units,False,allow_native_temporaries),native_schedule(alpha,units,True,allow_native_temporaries);count=max(len(rgb),len(alp))
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
    images=[0]*3
    for n,binding in enumerate(bindings):images[n]=binding['image']+1
    equation='reverseSubtract' if blend[0]==3 else 'add'
    record={'id':material['id'],'images':images,'stages':stages,'blend':'custom','custom_blend':{'color':equation,'alpha':equation,'color_source':source,'color_destination':destination,'alpha_source':alpha_source,'alpha_destination':alpha_destination},
        'cull':['none','front','back'][cull],'depth':native_depth,'depth_test':bool(depth[0]),'depth_write':bool(depth[2]),'alpha':native_alpha,'alpha_reference':reference,
        'requires_fog':bool(states['fog'][0] or states['fog'][1]),'initial_snapshot_only':not preserve_color_parameters}
    for lane,sequence in [('rgb',rgb),('alpha',alp)]:
        mask=sum(1<<n for n,item in enumerate(sequence) if item.get('buffer_update',False))
        if mask:record['buffer_'+lane+'_mask']=mask
    return record,{'id':material['id'],'source_stage_count':len(material['stages']),'native_stage_count':count,'bindings':bindings,'requires_fog':record['requires_fog'],
        'source_early_depth':states['early_depth'],'source_dither':states['dither'],'native_framebuffer_precision_validated':False,
        'blend_context':'RGBA-like native target, without source destination-alpha override; original framebuffer policy remains unconverted',
        'initial_snapshot_only':not preserve_color_parameters,'runtime_constant_updates_supported':preserve_color_parameters,
        'procedural_color_updates_supported':False,'specialized_source_constants':[] if preserve_color_parameters else ['tev_registers','konst_registers'],
        'native_color_parameter_mapping':({'registers':[0,1,2],'konst':[3,4,5,6],'domain':'unsigned RGBA8; game animation/procedural producers not implemented'} if preserve_color_parameters else None),
        'native_temporary_scheduling':allow_native_temporaries,'native_buffer_latency_gpu_validated':False,
        'arithmetic':'normalized native combiners','source_integer_equivalence':False,'lighting_texgen_sampler_animation_bindings_converted':False,'rendering_ready':False}

def convert(data,*,allow_normalized_arithmetic=False,specialize_initial_constants=False,preserve_color_parameters=False,allow_native_temporaries=False):
    records=[];rows=[]
    for source in materials(data):
        try:record,row=compile_material(source,allow_normalized_arithmetic=allow_normalized_arithmetic,specialize_initial_constants=specialize_initial_constants,preserve_color_parameters=preserve_color_parameters,allow_native_temporaries=allow_native_temporaries);records.append(record);row['status']='native_parameter_template' if preserve_color_parameters else 'native_fragment_snapshot'
        except ValueError as error:row={'id':source['id'],'status':'refused','reason':str(error)}
        rows.append(row)
    packed=compile_materials(records) if records else None
    return packed,{'source_sha256':hashlib.sha256(data).hexdigest(),'records':records,'materials':rows,'rendering_ready':False,
        'scope':('Parameterized native fragment templates; unsigned color inputs require native scene/animation producers. ' if preserve_color_parameters else 'Initial native fragment snapshots only. ')+
            'Normalized arithmetic explicitly selected; original integer precision, vertex lighting, texgen, samplers, fog parameters and animation unresolved.'}
