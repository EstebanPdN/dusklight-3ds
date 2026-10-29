"""Mac-only original model association -> native model-plan assembly.

Emits entry-queue order, not original draw-buffer sorting or a ready scene.
Pose/texgen/lighting/environment/animation producers remain external work.
"""
import hashlib
from j3d_native_geometry import sections,span,u16,u32,need,Unsupported,convert as geometry_convert
from j3d_material_source import Tables
from j3d_native_materials import convert as material_convert
from j3d_native_textures import source_images
from j3d_native_samplers import sampler
from native_model_plan import compile_plan

def hierarchy(blocks):
    info=blocks.get('INF1');need(info is not None,'Missing source hierarchy')
    span(info,0,24);at=u32(info,20);need(24<=at<=len(info)-4,'Source hierarchy offset')
    joint_count=u16(blocks['JNT1'],8);shape_count=u16(blocks['SHP1'],8);material_count=u16(blocks['MAT3'],8)
    need(0<joint_count<=4096 and shape_count<=4096 and material_count<=4096,'Hierarchy object count bound')
    frames=[{'parent':None,'current':None}];parents={};lists={};owners={};shapes={};shape_owners={};events=[]
    for work in range(32768):
        kind,value=u16(info,at),u16(info,at+2);at+=4;frame=frames[-1];parent=frame['parent']
        if kind==0:
            need(len(frames)==1,'Hierarchy terminates inside child scope');break
        if kind==1:
            need(frame['current'] is not None and len(frames)<129,'Hierarchy child owner/depth')
            frames.append({'parent':frame['current'],'current':frame['current']});continue
        if kind==2:
            need(len(frames)>1,'Hierarchy child-scope underflow');frames.pop();continue
        if kind==16:
            need(value<joint_count and value not in parents,'Hierarchy joint index/duplicate');parents[value]=parent;lists[value]=[];frame['current']=value
        elif kind==17:
            need(parent is not None and value<material_count and value not in owners,'Hierarchy material owner/index/duplicate')
            lists[parent].insert(0,value);owners[value]=parent
        elif kind==18:
            need(parent is not None and value<shape_count and lists[parent],'Hierarchy shape owner/index')
            material=lists[parent][0]
            if material in shapes or value in shape_owners:raise Unsupported('Source shape/material aliases require explicit native instance semantics')
            shapes[material]=value;shape_owners[value]=material
        else:raise Unsupported('Unknown source hierarchy command '+str(kind))
        events.append((kind,value,parent))
    else:raise Unsupported('Source hierarchy work bound exceeded')
    need(parents,'Empty source joint hierarchy')
    # Original J3DModel::entry iterates logical joint index, then its prepended
    # material list. A later draw buffer may change this enqueue order.
    for joint in sorted(lists):
        for material in lists[joint]:
            need(material in shapes,'Attached source material has no shape');events.append(('entry',joint,material,shapes[material]))
    return {'parents':parents,'owners':owners,'shape_owners':shape_owners,'entries':[e[1:] for e in events if e[0]=='entry'],'events':events}

def assemble(data,external_images=None,*,allow_normalized_arithmetic=False,allow_sdk_sampler_model=False):
    blocks=sections(data);tree=hierarchy(blocks);document,mesh_bytes,geometry=geometry_convert(data)
    material_bytes,fragments=material_convert(data,allow_normalized_arithmetic=allow_normalized_arithmetic,preserve_color_parameters=True,allow_native_temporaries=True)
    records={r['id']:r for r in fragments['records']};metadata={r['id']:r for r in fragments['materials']};sampling={};sampler_receipts=[]
    for index,bti,details in source_images(data,external_images):
        native,receipt=sampler(bti,details['mip_levels'],allow_sdk_sampler_model=allow_sdk_sampler_model);sampling[index+1]=native;sampler_receipts.append({'id':index+1,**receipt})
    groups={}
    for group in geometry['meshes']:groups.setdefault(group['source_shape'],[]).append(group)
    table=Tables(blocks['MAT3']);parts=[];uniforms=[];lookup={};correspondence=[];requirements={'poses','texgen','lighting','ordering'}
    for joint,source_material,shape in tree['entries']:
        identifier=source_material+1
        if identifier not in records:raise Unsupported('Referenced material has no native program: '+str(metadata.get(identifier,{}).get('reason','missing')))
        fragment=records[identifier];mode=table.get(12,u16(table.get(16,source_material,2),0),332)[0]
        if fragment['requires_fog']:requirements.add('fog')
        if any(any(s.get('constant_bindings',[])) for s in fragment['stages']):requirements.add('colors')
        for group in groups.get(shape,[]):
            palette=tuple(p+1 for p in group['draw_matrix_palette'])
            key=(identifier,joint+1,palette)
            if key not in lookup:lookup[key]=len(uniforms);uniforms.append({'material':identifier,'anchor':joint+1,'palette':list(palette)})
            samples=[sampling[image] if image in sampling else {} for image in fragment['images']]
            for image in fragment['images']:
                if image and image not in sampling:raise Unsupported('Referenced image has no native sampler')
            parts.append({'mesh':group['mesh'],'material':identifier,'uniform_slot':lookup[key],'sampler_mask':sum(1<<n for n,image in enumerate(fragment['images']) if image),
                          'samplers':samples,'pass':'opaque' if mode&3 else 'alpha'})
            correspondence.append({'part':len(parts)-1,'source_joint':joint,'source_material':source_material,'source_shape':shape,'source_group':group['source_group']})
    need(parts,'No referenced native triangle geometry')
    plan={'schema':'dusklight-native-model-plan/1','parts':parts,'uniforms':uniforms,'requirements':sorted(requirements)}
    native=compile_plan(plan)
    referenced={p['mesh'] for p in parts};referenced_poses={m for u in uniforms for m in u['palette']};pose_map=[]
    for n,m in enumerate(geometry['draw_matrices']):
        if n+1 in referenced_poses and not m['weighted_envelope']:need(m['pose_index'] in tree['parents'],'Referenced rigid pose joint is outside the hierarchy')
        pose_map.append({'native_matrix':n+1,'source':m})
    receipt={'source_sha256':hashlib.sha256(data).hexdigest(),'native_plan_sha256':hashlib.sha256(native).hexdigest(),'parts':len(parts),'uniform_slots':len(uniforms),
        'source_correspondence':correspondence,'source_joint_parents':tree['parents'],'source_transform_style':u16(blocks['INF1'],8)&15,'pose_matrix_correspondence':pose_map,'samplers':sampler_receipts,
        'unused_native_meshes':[m['mesh'] for m in geometry['meshes'] if m['mesh'] not in referenced],
        'source_order':'logical joint index, prepended material list, shape group order; before draw-buffer sorting',
        'final_render_order_converted':False,'pose_producers_converted':False,'texgen_lighting_producers_converted':False,'rendering_ready':False}
    return plan,native,receipt
