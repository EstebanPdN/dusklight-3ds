"""Native affine pose graph writer. No source file or renderer commands."""
import math,struct,zlib
MINIMUM=2**-62

def scalar(value,limit):
    if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or abs(value)>limit or (value and abs(value)<MINIMUM):raise ValueError('Native pose scalar domain')
    packed=struct.pack('<f',value);rounded=struct.unpack('<f',packed)[0]
    if abs(rounded)>limit or (rounded and abs(rounded)<MINIMUM):raise ValueError('Native pose rounded domain')
    return packed

def matrix(rows):
    if not isinstance(rows,list) or len(rows)!=3 or any(not isinstance(row,list) or len(row)!=4 for row in rows):raise ValueError('Native affine shape')
    return b''.join(scalar(value,2**20 if c==3 else 32768) for row in rows for c,value in enumerate(row))

def integer(value,low,high):
    if type(value)!=int or not low<=value<=high:raise ValueError('Native pose integer domain')
    return value

def compile_pose(document):
    if set(document)!={'schema','nodes','matrices'} or document['schema']!='dusklight-native-pose/1':raise ValueError('Native pose schema')
    nodes=document['nodes'];recipes=document['matrices'];integer(len(nodes),1,8192);integer(len(recipes),1,65535)
    body=bytearray();anchors=set();terms=bytearray();draws=bytearray()
    for n,node in enumerate(nodes):
        if set(node)!={'parent','anchor','local'}:raise ValueError('Native pose node fields')
        parent=integer(node['parent'],0,n);anchor=integer(node['anchor'],0,4096)
        if anchor and anchor in anchors:raise ValueError('Duplicate native anchor')
        if anchor:anchors.add(anchor)
        body.extend(struct.pack('<II',parent,anchor)+matrix(node['local'])+bytes(8))
    for recipe in recipes:
        normal={'inverse_transpose':0,'linear':1}.get(recipe.get('normal'))
        if normal is None:raise ValueError('Native normal policy')
        if set(recipe)=={'node','normal'}:
            draws.extend(struct.pack('<4I',0,integer(recipe['node'],1,len(nodes)),0,normal))
        elif set(recipe)=={'terms','normal'}:
            values=recipe['terms'];integer(len(values),1,255);first=len(terms)//56
            for term in values:
                if set(term)!={'node','weight','inverse_bind'}:raise ValueError('Native blend term fields')
                terms.extend(struct.pack('<I',integer(term['node'],1,len(nodes)))+scalar(term['weight'],16)+matrix(term['inverse_bind']))
            draws.extend(struct.pack('<4I',1,first,len(values),normal))
        else:raise ValueError('Native matrix recipe fields')
    integer(len(terms)//56,0,65536);size=48+len(body)+len(draws)+len(terms);integer(size,48,8<<20)
    header=struct.pack('<8s9I',b'DUSKPS01',1,size,len(nodes),len(recipes),len(terms)//56,48,48+len(body),48+len(body)+len(draws),0)
    payload=body+draws+terms
    return header+struct.pack('<I',zlib.crc32(header+payload))+payload
