#include "preview.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <cstring>

extern const unsigned char preview_shader[];
extern const unsigned int preview_shader_size;
namespace dusk::native {
namespace {
DVLB_s* shader;
shaderProgram_s program{};
C3D_Tex texture{};
C3D_Mtx projection;
int projectionLocation;
bool ready, programCreated, textureCreated;
TextureExtent extent;
}
bool initializePreview(const PreviewImage& image) {
    if (ready) return true;
    if (!image.rgba || !image.extent.width || !image.extent.height ||
        image.extent.paddedWidth > 1024 || image.extent.paddedHeight > 1024) return false;
    extent = image.extent;
    shader = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(preview_shader)),
                           preview_shader_size);
    if (!shader) return false;
    shaderProgramInit(&program); programCreated = true;
    if (R_FAILED(shaderProgramSetVsh(&program, &shader->DVLE[0]))) { destroyPreview(); return false; }
    projectionLocation = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
    if (projectionLocation < 0 ||
        !C3D_TexInit(&texture, extent.paddedWidth, extent.paddedHeight, GPU_RGBA8)) {
        destroyPreview(); return false;
    }
    textureCreated = true;
    if (texture.size != extent.outputBytes) { destroyPreview(); return false; }
    std::memcpy(texture.data, image.rgba, extent.outputBytes);
    C3D_TexFlush(&texture);
    C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    Mtx_OrthoTilt(&projection, 0, 400, 0, 240, 0, 1, true);
    ready = true;
    return true;
}
void drawPreview() {
    if (!ready) return;
    C3D_BindProgram(&program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, projectionLocation, &projection);
    C3D_AttrInfo* attributes = C3D_GetAttrInfo();
    AttrInfo_Init(attributes);
    AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attributes, 1, GPU_FLOAT, 2);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ZERO);
    for (unsigned i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexBind(0, &texture);
    const float left = (400.0f - extent.width) / 2.0f, right = left + extent.width;
    const float bottom = (240.0f - extent.height) / 2.0f, top = bottom + extent.height;
    const float u = float(extent.width) / extent.paddedWidth, v = float(extent.height) / extent.paddedHeight;
    auto vertex = [](float x, float y, float u, float v) {
        C3D_ImmSendAttrib(x, y, 0.5f, 1);
        C3D_ImmSendAttrib(u, v, 0, 1);
    };
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    // PICA samples texture Y from the bottom. The decoder stores the first GX
    // row at memory Y=0, so logical top is V=1, including padded allocations.
    vertex(left, top, 0, 1); vertex(left, bottom, 0, 1 - v); vertex(right, bottom, u, 1 - v);
    vertex(left, top, 0, 1); vertex(right, bottom, u, 1 - v); vertex(right, top, u, 1);
    C3D_ImmDrawEnd();
}
void destroyPreview() {
    ready = false;
    if (textureCreated) { C3D_TexDelete(&texture); textureCreated = false; }
    if (programCreated) { shaderProgramFree(&program); programCreated = false; }
    if (shader) { DVLB_Free(shader); shader = nullptr; }
}
}
