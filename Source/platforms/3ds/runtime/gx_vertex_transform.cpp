#include "gx_vertex_transform_diagnostic.hpp"
#include "gx_vertex_transform.hpp"
#include "gx_pica_math.hpp"
#include <bit>
#include <cmath>
namespace dusk::native::gx {
namespace {
bool fail(VertexTransformError value,VertexTransformError* error){if(error)*error=value;return false;}
bool scalar(const CommandRegisters& r,unsigned at,float& value,VertexTransformError* error){
    if(at>=r.xf.size()||!r.xfValid[at])return fail(VertexTransformError::missingState,error);
    value=std::bit_cast<float>(r.xf[at]);return std::isfinite(value)||fail(VertexTransformError::nonfinite,error);
}
template<class Cache>
bool matrix(const CommandRegisters& r,unsigned address,unsigned rows,const float* input,float* output,VertexTransformError* error,Cache& cache,unsigned columns=4){
    if(cache.rows<rows){
        for(unsigned row=cache.rows;row<rows;++row)for(unsigned c=0;c<columns;++c)
            if(!scalar(r,address+columns*row+c,cache.values[columns*row+c],error))return false;
        cache.rows=rows;
    }
    for(unsigned row=0;row<rows;++row){
        float total=0;
        for(unsigned c=0;c<columns;++c)total+=cache.values[columns*row+c]*input[c];
        if(!std::isfinite(total))return fail(VertexTransformError::nonfinite,error);
        output[row]=total;
    }
    return true;
}
bool source(const DecodedVertex& v,unsigned row,float* out,VertexTransformError* error){
    const float* data=nullptr;unsigned count=3;out[2]=out[3]=1;
    if(row==0){if(!(v.present&(1u<<GX_VA_POS)))return fail(VertexTransformError::missingAttribute,error);data=v.position;}
    else if(row==1||row==3||row==4){
        const unsigned slice=row==1?0:row-2;
        if(!(v.present&(1u<<GX_VA_NRM))||v.normalCount<=slice)return fail(VertexTransformError::missingAttribute,error);
        data=v.normal[slice];
    }else if(row==2||row==13){
        const unsigned channel=row==2?0:1;
        if(!(v.present&(1u<<(GX_VA_CLR0+channel))))return fail(VertexTransformError::missingAttribute,error);
        data=v.color[channel];count=4;
    }else if(row>=5&&row<=12){
        if(!(v.present&(1u<<(GX_VA_TEX0+row-5))))return fail(VertexTransformError::missingAttribute,error);
        data=v.texcoord[row-5];count=2;
    }else return fail(VertexTransformError::texgen,error);
    for(unsigned c=0;c<count;++c){if(!std::isfinite(data[c]))return fail(VertexTransformError::nonfinite,error);out[c]=data[c];}
    return true;
}
}
bool planVertexTransform(const CommandRegisters& r,std::uint8_t requested,VertexTransformPlan& out,VertexTransformError* error){
    out={};if(error)*error=VertexTransformError::none;
    // Build in the caller's storage: the palette caches are sizeable and a
    // second zeroed plan plus full copy costs bandwidth on the ARM11.
    // Preserve the public cleared-output contract on every rejection.
    struct ResetOnFailure {
        VertexTransformPlan& value;bool accepted{};
        ~ResetOnFailure(){if(!accepted)value={};}
    } reset{out};
    auto& p=out;
    // Per-vertex palette transforms precede this shared viewport/depth/tilt map.
    std::array<float,16> expanded{};
    if(!r.xfValid[0x1026])return fail(VertexTransformError::missingState,error);
    const auto type=r.xf[0x1026];if(type>1)return fail(VertexTransformError::projection,error);
    float projection[6],viewport[6];
    for(unsigned i=0;i<6;++i)if(!scalar(r,0x1020+i,projection[i],error)||!scalar(r,0x101a+i,viewport[i],error))return false;
    expanded[0]=projection[0];expanded[type?3:2]=projection[1];
    expanded[5]=projection[2];expanded[type?7:6]=projection[3];
    expanded[10]=projection[4];expanded[11]=projection[5];expanded[type?15:14]=type?1:-1;
    constexpr float zScale=16777215.0f;
    const Viewport viewportState{viewport[3]-340-viewport[0],viewport[4]-340+viewport[1],2*viewport[0],-2*viewport[1],
                   (viewport[5]-viewport[2])/zScale,viewport[5]/zScale};
    if(!picaProjection(expanded,viewportState,p.projection))return fail(VertexTransformError::viewport,error);
    if(requested){
        if(!r.xfValid[0x103f])return fail(VertexTransformError::missingState,error);
        const auto count=r.xf[0x103f];if(count>8||(unsigned(requested)>>count))return fail(VertexTransformError::texgen,error);
    }
    for(unsigned i=0;i<8;++i)if(requested&(1u<<i)){
        if(!r.xfValid[0x1040+i]||!r.xfValid[0x1050+i])return fail(VertexTransformError::missingState,error);
        const auto config=r.xf[0x1040+i],post=r.xf[0x1050+i];auto& t=p.texgen[i];
        t.type=(config>>4)&7;t.projection=(config>>1)&1;t.row=(config>>7)&31;t.post=post&63;t.normalize=(post>>8)&1;
        if(t.type==2||t.type==3){t.row=t.type==2?2:13;t.projection=false;}
        else if(t.type!=0||t.row>12)return fail(VertexTransformError::texgen,error);
        if(t.post!=61&&(t.post>57||t.post%3))return fail(VertexTransformError::matrixIndex,error);
        if(t.projection)p.projective|=1u<<i;
    }
    p.registers=&r;p.mask=requested;reset.accepted=true;return true;
}
bool transformVertex(const VertexTransformPlan& p,const DecodedVertex& v,TransformedVertex& out,VertexTransformError* error,bool normalRequired){
    out={};if(error)*error=VertexTransformError::none;
    if(!p.registers)return fail(VertexTransformError::missingState,error);
    const auto& r=*p.registers;TransformedVertex value{};float input[4];
    if(!source(v,0,input,error))return false;
    unsigned position;
    if(v.present&1)position=v.matrix[0];
    else{if(!r.matrixIndexValid[0])return fail(VertexTransformError::missingState,error);position=r.matrixIndex[0]&63;}
    if(position>27||position%3)return fail(VertexTransformError::matrixIndex,error);
    if(!matrix(r,4*position,3,input,value.viewPosition.data(),error,p.matrices[position/3]))return false;
    if(normalRequired){
        float normal[4];if(!source(v,1,normal,error))return false;
        if(!matrix(r,0x400+3*position,3,normal,value.viewNormal.data(),error,p.normals[position/3],3))return false;
        const auto& n=value.viewNormal;const float length2=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
        if(!std::isfinite(length2))return fail(VertexTransformError::normalLengthOverflow,error);
        if(length2>1e-10f){const float inverse=1/std::sqrt(length2);for(auto& c:value.viewNormal)c*=inverse;}
        value.normalValid=true;
    }
    for(unsigned row=0;row<4;++row){
        float n=p.projection[row*4+3];for(unsigned c=0;c<3;++c)n+=p.projection[row*4+c]*value.viewPosition[c];
        if(!std::isfinite(n))return fail(VertexTransformError::nonfinite,error);
        value.clip[row]=n;
    }
    for(unsigned i=0;i<8;++i)if(p.mask&(1u<<i)){
        const auto& t=p.texgen[i];if(!source(v,t.row,input,error))return false;
        float temp[4]{0,0,1,1};
        if(t.type==2||t.type==3){temp[0]=input[0];temp[1]=input[1];}
        else{
            unsigned selected;
            if(v.present&(1u<<(GX_VA_TEX0MTXIDX+i)))selected=v.matrix[1+i];
            else{
                const unsigned slot=i/4,shift=slot?6*(i-4):6*(i+1);
                if(!r.matrixIndexValid[slot])return fail(VertexTransformError::missingState,error);
                selected=(r.matrixIndex[slot]>>shift)&63;
            }
            if(selected>60||selected%3)return fail(VertexTransformError::matrixIndex,error);
            if(selected==60){for(unsigned c=0;c<3;++c)temp[c]=input[c];}
            else if(!matrix(r,4*selected,t.projection?3:2,input,temp,error,p.matrices[selected/3]))return false;
            if(!t.projection)temp[2]=1;
        }
        if(t.normalize){
            const float length=std::hypot(temp[0],temp[1],temp[2]);
            if(!std::isfinite(length)||length==0)return fail(VertexTransformError::normalization,error);
            for(unsigned c=0;c<3;++c)temp[c]/=length;
        }
        auto& uv=value.texcoord[i];
        if(t.post==61){for(unsigned c=0;c<3;++c)uv[c]=temp[c];}
        else if(!matrix(r,0x500+4*t.post,3,temp,uv.data(),error,p.postMatrices[t.post/3]))return false;
        // For non-projective texgens the pinned shader consumes only s,t even
        // if a post matrix computed q. Preserve that contract for the GPU sink.
        if(!t.projection)uv[2]=1;
    }
    out=value;return true;
}
const char* vertexTransformErrorText(VertexTransformError e){
    switch(e){case VertexTransformError::none:return "none";
    case VertexTransformError::missingState:return "XF transform state missing";
    case VertexTransformError::projection:return "GX projection type unsupported";
    case VertexTransformError::viewport:return "GX viewport invalid";
    case VertexTransformError::matrixIndex:return "GX matrix selector invalid";
    case VertexTransformError::texgen:return "GX texgen unsupported";
    case VertexTransformError::missingAttribute:return "GX transform source attribute missing";
    case VertexTransformError::normalLengthOverflow: // Exact failure site; keep human diagnostics stable.
    case VertexTransformError::nonfinite:return "GX transform nonfinite";
    case VertexTransformError::normalization:return "GX texgen normalization undefined";}
    return "unknown vertex transform error";
}
}

// Diagnostic replay intentionally leaves the production transform above unchanged.
// Differential host checks guard this failure-only mirror against semantic drift.
namespace dusk::native::gx {
void TransformTrace::bits(const char* tag,unsigned a,float b,unsigned c,unsigned d){emit(tag,a,std::bit_cast<unsigned>(b),c,d);}
namespace transform_diagnostic {
bool scalar(const CommandRegisters& r,unsigned at,float& value,VertexTransformError* error,TransformTrace& trace){
    if(at>=r.xf.size()||!r.xfValid[at]){trace.emit("reason",2,at);return fail(VertexTransformError::missingState,error);}
    trace.emit("xf",at,r.xf[at],r.xfValid[at]);
    value=std::bit_cast<float>(r.xf[at]);if(!std::isfinite(value))trace.emit("reason",1,at);return std::isfinite(value)||fail(VertexTransformError::nonfinite,error);
}
template<class Cache>
bool matrix(const CommandRegisters& r,unsigned address,unsigned rows,const float* input,float* output,VertexTransformError* error,TransformTrace& trace,Cache& cache,unsigned columns=4){
    trace.emit("matrix",address,rows,columns,cache.rows);
    for(unsigned c=0;c<columns;++c)trace.bits("operand",c,input[c]);
    // Cache words already consumed by the real failing call are authoritative.
    for(unsigned row=0;row<cache.rows&&row<3;++row)for(unsigned c=0;c<columns;++c){
        const unsigned at=address+columns*row+c;
        trace.bits("effective",at,cache.values[columns*row+c]);
        if(at<r.xf.size())trace.emit("raw",at,r.xf[at],r.xfValid[at]);
    }
    if(cache.rows<rows){
        for(unsigned row=cache.rows;row<rows;++row)for(unsigned c=0;c<columns;++c)
            if(!scalar(r,address+columns*row+c,cache.values[columns*row+c],error,trace))return false;
        cache.rows=rows;
    }
    for(unsigned row=0;row<rows;++row){
        float total=0;
        for(unsigned c=0;c<columns;++c)total+=cache.values[columns*row+c]*input[c];
        trace.bits("result",row,total);
        if(!std::isfinite(total)){trace.emit("reason",3,row);return fail(VertexTransformError::nonfinite,error);}
        output[row]=total;
    }
    return true;
}
bool source(const DecodedVertex& v,unsigned row,float* out,VertexTransformError* error,TransformTrace& trace){
    const float* data=nullptr;unsigned count=3;out[2]=out[3]=1;
    if(row==0){if(!(v.present&(1u<<GX_VA_POS)))return fail(VertexTransformError::missingAttribute,error);data=v.position;}
    else if(row==1||row==3||row==4){
        const unsigned slice=row==1?0:row-2;
        if(!(v.present&(1u<<GX_VA_NRM))||v.normalCount<=slice)return fail(VertexTransformError::missingAttribute,error);
        data=v.normal[slice];
    }else if(row==2||row==13){
        const unsigned channel=row==2?0:1;
        if(!(v.present&(1u<<(GX_VA_CLR0+channel))))return fail(VertexTransformError::missingAttribute,error);
        data=v.color[channel];count=4;
    }else if(row>=5&&row<=12){
        if(!(v.present&(1u<<(GX_VA_TEX0+row-5))))return fail(VertexTransformError::missingAttribute,error);
        data=v.texcoord[row-5];count=2;
    }else return fail(VertexTransformError::texgen,error);
    for(unsigned c=0;c<count;++c){if(!std::isfinite(data[c])){trace.emit("reason",4,row,c,std::bit_cast<unsigned>(data[c]));return fail(VertexTransformError::nonfinite,error);}out[c]=data[c];}
    return true;
}
}
struct VertexTransformDiagnostic {
static bool run(const VertexTransformPlan& p,const DecodedVertex& v,TransformedVertex& out,VertexTransformError* error,bool normalRequired,TransformTrace& trace){
    using transform_diagnostic::source;using transform_diagnostic::matrix;
    out={};if(error)*error=VertexTransformError::none;
    if(!p.registers)return fail(VertexTransformError::missingState,error);
    const auto& r=*p.registers;TransformedVertex value{};float input[4];
    trace.emit("stage",1);
    if(!source(v,0,input,error,trace))return false;
    unsigned position;
    if(v.present&1)position=v.matrix[0];
    else{if(!r.matrixIndexValid[0])return fail(VertexTransformError::missingState,error);position=r.matrixIndex[0]&63;}
    if(position>27||position%3)return fail(VertexTransformError::matrixIndex,error);
    trace.emit("stage",2,position);
    if(!matrix(r,4*position,3,input,value.viewPosition.data(),error,trace,p.matrices[position/3]))return false;
    if(normalRequired){
        trace.emit("stage",3,position);
        float normal[4];if(!source(v,1,normal,error,trace))return false;
        if(!matrix(r,0x400+3*position,3,normal,value.viewNormal.data(),error,trace,p.normals[position/3],3))return false;
        trace.emit("stage",4,position);
        const auto& n=value.viewNormal;const float length2=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
        trace.bits("length2",0,length2);
        if(!std::isfinite(length2)){trace.emit("reason",5);return fail(VertexTransformError::normalLengthOverflow,error);}
        if(length2>1e-10f){const float inverse=1/std::sqrt(length2);for(auto& c:value.viewNormal)c*=inverse;}
        value.normalValid=true;
    }
    trace.emit("stage",5);
    for(unsigned row=0;row<4;++row){
        float n=p.projection[row*4+3];for(unsigned c=0;c<3;++c)n+=p.projection[row*4+c]*value.viewPosition[c];
        trace.bits("clip",row,n);
        if(!std::isfinite(n)){trace.emit("reason",6,row);return fail(VertexTransformError::nonfinite,error);}
        value.clip[row]=n;
    }
    for(unsigned i=0;i<8;++i)if(p.mask&(1u<<i)){
        trace.emit("stage",6,i);
        const auto& t=p.texgen[i];if(!source(v,t.row,input,error,trace))return false;
        float temp[4]{0,0,1,1};
        if(t.type==2||t.type==3){temp[0]=input[0];temp[1]=input[1];}
        else{
            unsigned selected;
            if(v.present&(1u<<(GX_VA_TEX0MTXIDX+i)))selected=v.matrix[1+i];
            else{
                const unsigned slot=i/4,shift=slot?6*(i-4):6*(i+1);
                if(!r.matrixIndexValid[slot])return fail(VertexTransformError::missingState,error);
                selected=(r.matrixIndex[slot]>>shift)&63;
            }
            trace.emit("stage",7,i,selected);
            if(selected>60||selected%3)return fail(VertexTransformError::matrixIndex,error);
            if(selected==60){for(unsigned c=0;c<3;++c)temp[c]=input[c];}
            else if(!matrix(r,4*selected,t.projection?3:2,input,temp,error,trace,p.matrices[selected/3]))return false;
            if(!t.projection)temp[2]=1;
        }
        if(t.normalize){
            trace.emit("stage",8,i);
            const float length=std::hypot(temp[0],temp[1],temp[2]);
            trace.bits("length",0,length);
            if(!std::isfinite(length)||length==0){trace.emit("reason",7);return fail(VertexTransformError::normalization,error);}
            for(unsigned c=0;c<3;++c)temp[c]/=length;
        }
        trace.emit("stage",9,i,t.post);
        auto& uv=value.texcoord[i];
        if(t.post==61){for(unsigned c=0;c<3;++c)uv[c]=temp[c];}
        else if(!matrix(r,0x500+4*t.post,3,temp,uv.data(),error,trace,p.postMatrices[t.post/3]))return false;
        // For non-projective texgens the pinned shader consumes only s,t even
        // if a post matrix computed q. Preserve that contract for the GPU sink.
        if(!t.projection)uv[2]=1;
    }
    out=value;return true;
}
static bool capture(const VertexTransformPlan& original,const DecodedVertex& v,bool normalRequired,VertexTransformError& error,TransformTrace& trace){
    auto p=original;
    trace.emit("begin",p.mask,normalRequired,p.projective,2);
    // Synchronous failure capture: retain all four exact target words. Replay
    // must restore this context before planning; projection remains independent.
    const auto& target=currentTargetExtent;
    trace.emit("target",target.logicalWidth,target.logicalHeight,target.pixelWidth,target.pixelHeight);
    trace.emit("vertex",v.present,v.normalCount);
    for(unsigned c=0;c<3;++c)trace.bits("position",c,v.position[c]);
    for(unsigned n=0;n<3;++n)for(unsigned c=0;c<3;++c)trace.bits("normal",n,v.normal[n][c],c);
    for(unsigned n=0;n<2;++n)for(unsigned c=0;c<4;++c)trace.bits("color",n,v.color[n][c],c);
    for(unsigned n=0;n<8;++n)for(unsigned c=0;c<2;++c)trace.bits("texcoord",n,v.texcoord[n][c],c);
    for(unsigned i=0;i<9;++i)trace.emit("selector",i,v.matrix[i]);
    for(unsigned i=0;i<16;++i)trace.bits("projection",i,p.projection[i]);
    if(p.registers){
        const auto& r=*p.registers;
        auto xf=[&](unsigned at){trace.emit("xf",at,r.xf[at],r.xfValid[at]);};
        for(unsigned at=0x101a;at<=0x1026;++at)xf(at);
        xf(0x103f);
        for(unsigned i=0;i<8;++i)if(p.mask&(1u<<i)){xf(0x1040+i);xf(0x1050+i);}
        for(unsigned i=0;i<2;++i)trace.emit("index",i,r.matrixIndex[i],r.matrixIndexValid[i]);
    }
    TransformedVertex out;
    const bool ok=run(p,v,out,&error,normalRequired,trace);
    if(ok){
        for(unsigned c=0;c<3;++c){trace.bits("view",c,out.viewPosition[c]);trace.bits("viewnormal",c,out.viewNormal[c]);}
        for(unsigned i=0;i<8;++i)for(unsigned c=0;c<3;++c)trace.bits("uv",i,out.texcoord[i][c],c);
    }
    // Terminal record bypasses cap so truncation is always explicit.
    if(trace.sink)trace.sink("end",ok,unsigned(error),trace.emitted,trace.attempted==trace.emitted);
    return ok;
}
};
bool diagnoseVertexTransform(const VertexTransformPlan& p,const DecodedVertex& v,bool normalRequired,VertexTransformError& error,TransformTrace& trace){
    return VertexTransformDiagnostic::capture(p,v,normalRequired,error,trace);
}
}
