#pragma once
#include "gx_vertex_transform.hpp"
namespace dusk::native::gx {
// Failure-only synchronous observer. No retained game pointers or memory reads.
using TransformDiagnosticSink=void(*)(const char*,unsigned,unsigned,unsigned,unsigned);
struct TransformTrace {
    static constexpr unsigned cap=1024;
    TransformDiagnosticSink sink{};
    unsigned emitted{},attempted{};
    void emit(const char* tag,unsigned a=0,unsigned b=0,unsigned c=0,unsigned d=0){
        ++attempted;if(sink&&emitted<cap){sink(tag,a,b,c,d);++emitted;}
    }
    void bits(const char* tag,unsigned a,float b,unsigned c=0,unsigned d=0);
};
// Copies the failed plan, including warmed caches; production output/state stays intact.
// Returns replay success and its exact error; caller compares with original failure.
bool diagnoseVertexTransform(const VertexTransformPlan&,const DecodedVertex&,bool,
                             VertexTransformError&,TransformTrace&);
}
