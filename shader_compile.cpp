// 用现成的 host glslang 库把 GLSL compute shader 编译成 SPIRV。
// 编译: clang++ ... -lglslang -lSPIRV-Tools -lSPIRV-Tools-opt -lOSDependent
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Include/ResourceLimits.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static TBuiltInResource DefaultRes() {
    TBuiltInResource r{};
    r.maxLights = 32; r.maxClipPlanes = 6; r.maxTextureUnits = 32;
    r.maxTextureCoords = 32; r.maxVertexAttribs = 64; r.maxVertexUniformComponents = 4096;
    r.maxVaryingFloats = 64; r.maxVertexTextureImageUnits = 32; r.maxCombinedTextureImageUnits = 80;
    r.maxTextureImageUnits = 32; r.maxFragmentUniformComponents = 4096;
    r.maxDrawBuffers = 32; r.maxVertexUniformVectors = 128; r.maxVaryingVectors = 8;
    r.maxFragmentUniformVectors = 128; r.maxVertexOutputVectors = 16; r.maxFragmentInputVectors = 15;
    r.minProgramTexelOffset = -8; r.maxProgramTexelOffset = 7;
    r.maxClipDistances = 8; r.maxComputeWorkGroupCountX = 65535;
    r.maxComputeWorkGroupCountY = 65535; r.maxComputeWorkGroupCountZ = 65535;
    r.maxComputeWorkGroupSizeX = 1024; r.maxComputeWorkGroupSizeY = 1024; r.maxComputeWorkGroupSizeZ = 64;
    r.maxComputeUniformComponents = 1024; r.maxComputeTextureImageUnits = 16;
    r.maxComputeImageUniforms = 8; r.maxComputeAtomicCounters = 8; r.maxComputeAtomicCounterBuffers = 1;
    r.maxVaryingComponents = 60; r.maxVertexOutputComponents = 64; r.maxFragmentInputComponents = 60;
    r.maxImageUnits = 12; r.maxCombinedImageUnitsAndFragmentOutputs = 12;
    r.maxCombinedShaderOutputResources = 8; r.maxImageSamples = 0;
    r.maxVertexImageUniforms = 0; r.maxTessControlImageUniforms = 0; r.maxTessEvaluationImageUniforms = 0;
    r.maxGeometryImageUniforms = 0; r.maxFragmentImageUniforms = 8; r.maxCombinedImageUniforms = 8;
    r.maxCombinedTextureImageUnits = 80;
    return r;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s in.comp out.spv entry\n", argv[0]); return 1; }
    std::ifstream f(argv[1]);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    glslang::InitializeProcess();
    glslang::TShader sh(EShLangCompute);
    const char* srcptr = src.c_str();
    sh.setStrings(&srcptr, 1);
    sh.setAutoMapLocations(true);
    sh.setAutoMapBindings(true);
    const char* defines[] = {};
    TBuiltInResource res = DefaultRes();
    EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    // GLSL 4.50 for Vulkan compute
    if (!sh.parse(&res, 450, ENoProfile, false, false, msgs)) {
        std::fprintf(stderr, "PARSE FAIL:\n%s\n", sh.getInfoLog()); return 2;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
        std::fprintf(stderr, "LINK FAIL:\n%s\n", prog.getInfoLog()); return 3;
    }
    std::vector<unsigned int> spv;
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangCompute), spv);
    std::ofstream out(argv[2], std::ios::binary);
    out.write((const char*)spv.data(), spv.size()*4);
    std::fprintf(stderr, "OK: %s (%zu words)\n", argv[2], spv.size());
    glslang::FinalizeProcess();
    return 0;
}
