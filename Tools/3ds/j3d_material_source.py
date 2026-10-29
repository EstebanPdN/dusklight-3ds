"""Bounded Mac-only MAT3 declaration reader, independent of runtime GX state."""
import struct
from j3d_native_geometry import sections,span,u16,u32,need,Unsupported

DEFAULT_STAGE=bytes.fromhex('040a0f0f00000000010005070700000000010000')

class Tables:
    def __init__(self,block):
        self.block=block;span(block,0,132)
        self.pointers=[u32(block,n) for n in range(12,132,4)]
        need(all(p==0 or 132<=p<=len(block) for p in self.pointers),'MAT3 table pointer')
    def get(self,field,index,size):
        at=u32(self.block,field);need(at>=132,'Missing used MAT3 table')
        end=min([p for p in self.pointers if p>at]+[len(self.block)])
        need(index>=0 and at+index*size+size<=end,'MAT3 table index crosses its region')
        return span(self.block,at+index*size,size)

def materials(data):
    block=sections(data).get('MAT3');need(block is not None,'Missing MAT3')
    tables=Tables(block);count=u16(block,8);need(count<=4096,'Material count bound')
    for identifier in range(count):
        try:
            record=tables.get(12,u16(tables.get(16,identifier,2),0),332)
            stageIndex=record[4]
            if stageIndex==255:raise Unsupported('Unspecified initial TEV stage count')
            stageCount=tables.get(88,stageIndex,1)[0];need(1<=stageCount<=16,'Source TEV stage count')
            indirect=u32(block,24)
            if indirect and tables.get(24,identifier,312)[0]:raise Unsupported('Indirect source material requires native indirect pipeline')
            regs=[];konst=[]
            for n in range(4):
                index=u16(record,220+n*2)
                regs.append((255,255,255,255) if index==65535 else struct.unpack('>4h',tables.get(80,index,8)))
                index=u16(record,148+n*2);konst.append((255,255,255,255) if index==65535 else tuple(tables.get(84,index,4)))
            # Source helpers load register0..2 from these indices. PREV starts
            # from the fourth slot only when explicitly referenced before write.
            stages=[]
            for n in range(stageCount):
                index=u16(record,228+n*2);raw=DEFAULT_STAGE if index==65535 else tables.get(92,index,20)
                index=u16(record,188+n*2);order=bytes((255,255,255,255)) if index==65535 else tables.get(76,index,4)
                index=u16(record,260+n*2);swap=bytes(4) if index==65535 else tables.get(96,index,4)
                swizzles=[]
                for selected in [swap[0],swap[1]]:
                    need(selected<4,'Source swap selector');table=u16(record,292+selected*2)
                    swizzles.append((0,1,2,3) if table==65535 else tuple(tables.get(100,table,4)))
                need(all(c<4 for s in swizzles for c in s),'Source swap channel')
                stages.append({'rgb':tuple(raw[1:5]),'rgb_op':tuple(raw[5:10]),'alpha':tuple(raw[10:14]),'alpha_op':tuple(raw[14:19]),
                    'coordinate':order[0],'texture_slot':order[1],'raster':order[2],'raster_swap':swizzles[0],'texture_swap':swizzles[1],
                    'konst_color_selector':record[156+n],'konst_alpha_selector':record[172+n]})
            textures=[]
            for n in range(8):
                index=u16(record,132+n*2);textures.append(None if index==65535 else u16(tables.get(72,index,2),0))
            states={}
            for label,field,index,size in [('blend',112,u16(record,328),4),('alpha',108,u16(record,326),8),('fog',104,u16(record,324),44),('depth',116,record[6],4)]:
                if index==(255 if label=='depth' else 65535):raise Unsupported('Default '+label+' state needs explicit source default compilation')
                states[label]=tuple(tables.get(field,index,size))
            cull=record[1]
            if cull==255:raise Unsupported('Unspecified source cull state')
            states['cull']=u32(tables.get(28,cull,4),0)
            states['early_depth']=0 if record[5]==255 else tables.get(120,record[5],1)[0]
            states['dither']=1 if record[7]==255 else tables.get(124,record[7],1)[0]
            need(states['early_depth']<2 and states['dither']<2,'Source early-depth/dither flag')
            generatorIndex=record[3];generatorCount=0 if generatorIndex==255 else tables.get(52,generatorIndex,1)[0]
            need(generatorCount<=8,'Source texgen count');generators=[]
            for n in range(generatorCount):
                index=u16(record,40+n*2);generators.append((1,4,60,0) if index==65535 else tuple(tables.get(56,index,4)))
            yield {'id':identifier+1,'source_material':identifier,'stages':stages,'registers':regs,'konst':konst,'textures':textures,'states':states,'generators':generators}
        except (ValueError,struct.error) as error:
            yield {'id':identifier+1,'source_material':identifier,'error':str(error)}
