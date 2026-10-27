// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, getenv("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
// A pixel's view-space position (the SSAO's, the contact shadows', the GI's normals), and with surf its position c and
// normal n (the SSAO and the contact shadows), rebuilt from the depth. The depth at binding 0; lin() and pc.a, the
// projection, declared by each group below.
static const char *vpos_src =
    "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
    "ivec2 sz;\n"
    "vec3 vpos(ivec2 p) {\n"
    "  p = clamp(p, ivec2(0), sz - 1);\n"
    "  float l = lin(texelFetch(dep, p, 0).r);\n"
    "  vec2 ndc = (vec2(p) + 0.5) / vec2(sz) * 2.0 - 1.0;\n"
    "  return vec3(ndc.x * l / pc.a.x, ndc.y * l / pc.a.y, l);\n"
    "}\n";
static const char *surf =
    "void surface(ivec2 p, out vec3 c, out vec3 n) {\n"
    "  c = vpos(p);\n"
    "  vec3 l = vpos(p - ivec2(1, 0)), r = vpos(p + ivec2(1, 0)), u = vpos(p - ivec2(0, 1)), d = vpos(p + ivec2(0, 1));\n"
    // A pixel whose two neighbours of a direction are both nearer or both farther is no plane: a crack or a sliver one
    // pixel wide (the seams between the ground's pieces and the path patches drawn ~2 units over it). At 4K a pixel
    // covers ~0.5 units of ground, so its normal came out nearly level, the ground around it occluded it, and every
    // seam was a thin dark line (at 720p hardly). The surface is the neighbours'.
    "  float tc = 1e-4 * c.z;\n"
    "  if ((l.z - c.z) * (r.z - c.z) > 0.0 && min(abs(l.z - c.z), abs(r.z - c.z)) > tc) c = (l + r) * 0.5;\n"
    "  else if ((u.z - c.z) * (d.z - c.z) > 0.0 && min(abs(u.z - c.z), abs(d.z - c.z)) > tc) c = (u + d) * 0.5;\n"
    // (two pixels wide: the same with the neighbours two away)
    "  else {\n"
    "    vec3 l2 = vpos(p - ivec2(2, 0)), r2 = vpos(p + ivec2(2, 0)), u2 = vpos(p - ivec2(0, 2)), d2 = vpos(p + ivec2(0, 2));\n"
    "    if ((l2.z - c.z) * (r2.z - c.z) > 0.0 && min(abs(l2.z - c.z), abs(r2.z - c.z)) > tc) { c = (l2 + r2) * 0.5; l = l2; r = r2; }\n"
    "    else if ((u2.z - c.z) * (d2.z - c.z) > 0.0 && min(abs(u2.z - c.z), abs(d2.z - c.z)) > tc) { c = (u2 + d2) * 0.5; u = u2; d = d2; }\n"
    "  }\n"
    "  vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
    "  vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;\n"
    "  n = normalize(cross(dx, dy));\n"
    "  if (dot(n, c) > 0.0) n = -n;\n"
    "}\n";
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "dl_apply_rt.frag")) {  // (dl_apply.frag with its ray-traced shadow: RT defined, the ray query extension on)
        static char *rt;
        const char *base = builtin_src("dl_apply.frag", stage), *body = strchr(base, '\n') + 1;
        if (!rt && (rt = malloc(strlen(base) + 128)))
            sprintf(rt, "#version 460\n#extension GL_EXT_ray_query : require\n#define RT 1\n%s", body);
        return rt;
    }
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               // A render resolution above the window's (the launcher's 1440p / 4K on a smaller screen: supersampling)
               // is reduced with four bilinear taps spread over each window pixel's footprint - one bilinear tap
               // skips texels and keeps the jaggies (exactly 2x: the 2x2 box).
               "void main() {\n"
               "  vec2 d = vec2(dFdx(uv.x), dFdy(uv.y)), r = abs(d) * vec2(textureSize(t, 0));\n"
               "  if (max(r.x, r.y) < 1.05) { o = vec4(texture(t, uv).rgb, 1.0); return; }\n"
               "  vec2 q = d * 0.25;\n"
               "  vec3 c = texture(t, uv + vec2(-q.x, -q.y)).rgb + texture(t, uv + vec2(q.x, -q.y)).rgb +\n"
               "           texture(t, uv + vec2(-q.x, q.y)).rgb + texture(t, uv + vec2(q.x, q.y)).rgb;\n"
               "  o = vec4(c * 0.25, 1.0);\n"
               "}\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    // Debanding, the last post pass (exec.c post_process_ex): the game's 8-bit targets show steps in smooth gradients
    // (sky, fog, the menus' blurred backdrop). As mpv's / libplacebo's deband: around each pixel, at a random angle and
    // distance within a radius, four taps; when their mean is within the threshold of the pixel (a step of a band, not
    // an edge or a texture's detail) it takes the mean - twice, the second time twice as far and half the threshold.
    // Then a triangular dither of one 8-bit step, fixed per pixel (not per frame: TAA, FSR and the in-between images
    // see the same pattern). a = (1 / size, threshold), b = (size, radius in pixels).
    if (!strcmp(name, "deband.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float h12(vec2 p) { vec3 q = fract(vec3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return fract((q.x + q.y) * q.z); }\n"
               "void main() {\n"
               "  vec2 px = gl_FragCoord.xy, uv = px * pc.a.xy;\n"
               "  vec3 c = texelFetch(t, ivec2(px), 0).rgb;\n"
               "  for (int i = 1; i <= 2; i++) {\n"
               "    float ang = h12(px + float(i) * 17.0) * 6.2831853;\n"
               "    float rad = pc.b.z * float(i) * h12(px * 1.7 + float(i) * 31.0);\n"
               "    vec2 d = vec2(cos(ang), sin(ang)) * rad * pc.a.xy, e = vec2(-d.y, d.x);\n"
               "    vec3 m = 0.25 * (texture(t, uv + d).rgb + texture(t, uv - d).rgb + texture(t, uv + e).rgb + texture(t, uv - e).rgb);\n"
               "    c = mix(c, m, step(abs(c - m), vec3(pc.a.z / float(i))));\n"
               "  }\n"
               "  float n = h12(px) + h12(px * 1.31 + 7.0) - 1.0;\n"
               "  o = vec4(clamp(c + n / 255.0, 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    // SSAO (F12 menu; exec.c ssao_run): on the game's depth, between its opaque and its blended draws. a = the game's
    // projection (P00, P11, A, B: GL style, z_ndc = (A z + B) / -z, so the distance along the view axis is
    // B / (z_ndc + A)); b.z = 1 when depth was stored as (z_ndc + 1) / 2 (NVN's -1..1 mode). Positions are rebuilt
    // in view space (x right, y as the image, z the distance: one handedness throughout), the normal from the nearer
    // neighbours (no smearing across silhouettes).
    if (!strcmp(name, "ssao.frag") || !strcmp(name, "ssao_blur.frag") || !strcmp(name, "ssao_temporal.frag") ||
        !strcmp(name, "contact.frag")) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "float lin(float d) { return pc.a.w / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n";
        // b.x = radius (world units), b.y = intensity, b.w = the largest screen radius in pixels. c.y samples (8-16) on a
        // spiral turned per pixel (interleaved gradient noise; the blur evens it out), denser near the centre;
        // c.x turns it per frame too (temporal accumulation: other directions every frame).
        static const char *ao =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(1.0); return; }\n"
            "  vec3 c, n;\n"
            "  surface(p, c, n);\n"
            "  float rad = pc.b.x;\n"
            "  float rpx = min(rad * pc.a.x / c.z * 0.5 * float(sz.x), pc.b.w);\n"
            "  if (rpx < 1.0) { o = vec4(1.0); return; }\n"
            "  float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + pc.c.x);\n"
            "  float occ = 0.0, bias = 0.02 * c.z;\n"
            "  int ns = int(pc.c.y);\n"
            "  for (int i = 0; i < ns; i++) {\n"
            "    float t = (float(i) + 0.5) / float(ns), a = ang + float(i) * 2.39996323;\n"
            "    vec3 v = vpos(p + ivec2(vec2(cos(a), sin(a)) * (t * rpx + 1.0))) - c;\n"
            "    float vv = dot(v, v);\n"
            "    float cosv = (dot(v, n) - bias) * inversesqrt(vv + 1e-3);\n"
            "    occ += max(cosv, 0.0) * clamp(1.0 - vv / (rad * rad), 0.0, 1.0);\n"
            "  }\n"
            "  o = vec4(clamp(1.0 - pc.b.y * occ * (2.0 / float(ns)), 0.0, 1.0), 0.0, 0.0, 1.0);\n"
            "}\n";
        // Depth-aware blur along b.xy (one texel steps), 9 taps: neighbours more than a few % farther or nearer than
        // the centre don't count (no halo around silhouettes).
        static const char *blur =
            "layout(set = 0, binding = 0) uniform sampler2D t;\n"
            "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
            "void main() {\n"
            "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
            "  float l0 = lin(texelFetch(dep, p, 0).r), s = 0.0, w = 0.0;\n"
            "  for (int i = -4; i <= 4; i++) {\n"
            "    ivec2 q = clamp(p + ivec2(pc.b.xy * float(i)), ivec2(0), sz - 1);\n"
            "    float dl = abs(lin(texelFetch(dep, q, 0).r) - l0) / (0.04 * l0);\n"
            "    float k = exp(-float(i * i) / 10.0) * max(1.0 - dl, 0.0);\n"
            "    s += texelFetch(t, q, 0).r * k; w += k;\n"
            "  }\n"
            "  o = vec4(w > 0.0 ? s / w : texelFetch(t, p, 0).r, 0.0, 0.0, 1.0);\n"
            "}\n";
        // Temporal accumulation (exec.c ssao_run): t = this frame's blurred occlusion, dep = depth, mv = the camera
        // motion vectors (xy velocity, z previous / current distance), hist = the previous result (r occlusion,
        // g its distance). The history is fetched where the pixel was; it counts only if the distance stored there is
        // the one the pixel had then (else: uncovered, or a character moving on its own - camera vectors don't know
        // it), and clamped to this frame's 3x3 range - exactly: with any slack the history stayed that much darker for
        // good where a character's contact shadow had passed (a fixed point of the blend: tools/ssao_temporal_check.py).
        // b.y = the history's weight, b.w = 1: no history (a cut, the
        // first frame). Out: r = the occlusion, g = this distance (the next frame's history).
        static const char *temporal =
            "layout(set = 0, binding = 0) uniform sampler2D t;\n"
            "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
            "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
            "layout(set = 0, binding = 3) uniform sampler2D hist;\n"
            "void main() {\n"
            "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
            "  float d = texelFetch(dep, p, 0).r, l = lin(d), a = texelFetch(t, p, 0).r;\n"
            "  if (d >= 0.999999 || pc.b.w > 0.5) { o = vec4(a, l, 0.0, 1.0); return; }\n"
            "  float mn = a, mx = a;\n"
            "  for (int y = -1; y <= 1; y++)\n"
            "    for (int x = -1; x <= 1; x++) {\n"
            "      float c = texelFetch(t, clamp(p + ivec2(x, y), ivec2(0), sz - 1), 0).r;\n"
            "      mn = min(mn, c); mx = max(mx, c);\n"
            "    }\n"
            "  vec4 m = texelFetch(mv, p, 0);\n"
            "  vec2 puv = (vec2(p) + 0.5) / vec2(sz) - m.xy;\n"
            "  float w = 0.0, h = a;\n"
            "  if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {\n"
            "    float lh = texelFetch(hist, ivec2(puv * vec2(sz)), 0).g, le = m.z * l;\n"
            "    if (abs(lh - le) < 0.02 * le + 1.0) {\n"
            "      w = pc.b.y;\n"
            "      h = clamp(texture(hist, puv).r, mn, mx);\n"
            "    }\n"
            "  }\n"
            "  o = vec4(mix(a, h, w), l, 0.0, 1.0);\n"
            "}\n";
        // Contact shadows (exec.c contact_run; the graphics page): from each pixel a short ray toward the sun (c.xyz, view
        // space as above; the light found in the shadow map's pass), b.w steps along b.x world units, jittered per
        // pixel; a step behind the depth there by less than c.w (the thickness: not the far side of a wall) is in
        // shadow, fainter the farther along the ray the hit. Surfaces facing away from the sun are left to the game's
        // lighting. Out: 1 - b.y (the strength) in shadow, faded out with distance (the shadow map covers far away).
        static const char *contact =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(1.0); return; }\n"
            "  vec3 c, n;\n"
            "  surface(p, c, n);\n"
            "  vec3 L = normalize(pc.c.xyz);\n"
            "  if (dot(n, L) < 0.05) { o = vec4(1.0); return; }\n"
            "  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
            "  vec3 st = c + n * (0.003 * c.z);\n"
            "  int N = int(pc.b.w);\n"
            "  float sh = 0.0;\n"
            "  for (int i = 0; i < N; i++) {\n"
            "    float t = (float(i) + jit) / float(N);\n"
            "    vec3 q = st + L * (pc.b.x * t);\n"
            "    if (q.z <= 1.0) break;\n"
            "    vec2 uv = vec2(q.x * pc.a.x, q.y * pc.a.y) / q.z * 0.5 + 0.5;\n"
            "    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) break;\n"
            "    float dz = q.z - lin(texelFetch(dep, ivec2(uv * vec2(sz)), 0).r);\n"
            "    if (dz > 0.002 * q.z && dz < pc.c.w) { sh = 1.0 - t * t; break; }\n"
            "  }\n"
            "  float fade = clamp(2.0 - c.z / 2000.0, 0.0, 1.0);\n"
            "  o = vec4(1.0 - pc.b.y * sh * fade, 0.0, 0.0, 1.0);\n"
            "}\n";
        static char *src[4];
        int k = !strcmp(name, "ssao_blur.frag") ? 1 : !strcmp(name, "ssao_temporal.frag") ? 2 : !strcmp(name, "contact.frag") ? 3 : 0;
        if (!src[k]) {
            const char *body = k == 3 ? contact : k == 2 ? temporal : k ? blur : ao;
            const char *pre = k == 0 || k == 3 ? vpos_src : "", *pre2 = k == 0 || k == 3 ? surf : "";
            size_t n = strlen(common) + strlen(pre) + strlen(pre2) + strlen(body) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s%s", common, pre, pre2, body);
        }
        return src[k];
    }
    // ---- Screen-space indirect light, one bounce (F12 GI; exec.c gi_run), after FastGI's design: rays traced at a
    // reduced resolution against the depth, the scene's own colour where they hit, two bilateral blurs, an optional
    // temporal accumulation along the camera motion vectors, then an edge-aware upsample added onto the scene.
    // View space as in the SSAO (x right, y as the image, z the distance), the normals rebuilt from the depth (the
    // game has no normal buffer). a = the projection (P00, P11, A, B), d.x = 1: depth stored as (z_ndc + 1) / 2,
    // d.zw = the GI resolution. World units: the game's (~cm).
    // The trace, the blurs and the composite's taps read a depth and normals (RGBA8) at the GI's own size, each GI
    // pixel the full-size pixel under its centre (gi_depth / gi_normal: what the passes fetched before, from the
    // full-size images). The trace's 4 x 8 steps + 2 bisections per ray are scattered reads, bound by the cache: over
    // the 4K depth (D32, 33 MB) 0.87 ms at 4K, GI high; over the GI-size distance as R32F 0.72, as 16 bits 0.6. So
    // the distance is R16_UNORM of log2(distance) / 16 (0 on the sky): 1.7e-4 of it (0.3 units at 20 m; fp16 would be
    // 5e-4), up to 65536 units - 650 m, past the 60 m the GI lights.
    if (!strncmp(name, "gi_", 3)) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "float lin(float d) { return pc.a.w / ((pc.d.x > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
            "vec3 vpos(vec2 uv, float l) { vec2 n = uv * 2.0 - 1.0; return vec3(n.x * l / pc.a.x, n.y * l / pc.a.y, l); }\n"
            "vec2 puv(vec3 p) { return vec2(p.x * pc.a.x, p.y * pc.a.y) / p.z * 0.5 + 0.5; }\n"
            // the GI pixel's centre in uv, and the full-size pixel under it
            "vec2 low_uv() { return (floor(gl_FragCoord.xy) + 0.5) / pc.d.zw; }\n"
            // bilateral weight of a tap: depth within ~1.5% (2 units at least), normals alike
            "float geo(float l0, float l1, vec3 n0, vec3 n1) {\n"
            "  float w = clamp(1.0 - abs(l0 - l1) / max(2.0, l0 * 0.015), 0.0, 1.0) * step(0.001, l1);\n"
            "  float nw = clamp(dot(n0, n1), 0.0, 1.0);\n"
            "  nw *= nw;\n"
            "  return w * nw * nw;\n"
            "}\n"
            "vec3 dec_n(vec4 e) { return normalize(e.xyz * 2.0 - 1.0); }\n";
        // The normal from the nearer neighbour on each axis (no smearing across silhouettes), facing the camera. Not
        // the SSAO's surface(), tried (one pass shared by both, or the same function): its seam rule takes the edges of
        // the alpha-tested leaves for seams, and the GI's light broke into dark blocks and speckles over the foliage.
        // A full-size target shared with the SSAO also cost 0.125 ms at 4K and saved it nothing (its own rebuild hides
        // under its samples' reads: 1.94 ms either way).
        static const char *qnormal =
            "vec3 quick_normal(ivec2 p) {\n"
            "  vec3 c = vpos(p), l = vpos(p - ivec2(1, 0)), r = vpos(p + ivec2(1, 0)), u = vpos(p - ivec2(0, 1)), d = vpos(p + ivec2(0, 1));\n"
            "  vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
            "  vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;\n"
            "  vec3 n = normalize(cross(dx, dy));\n"
            "  return dot(n, c) > 0.0 ? -n : n;\n"
            "}\n";
        // GI size: the distance (R16_UNORM, log2 / 16 as above) and the normal (RGBA8: n * 0.5 + 0.5, a = 0 on the sky)
        // of the full-size pixel under each GI pixel's centre (the normals were a full-size pass: 0.08 ms at 4K)
        static const char *depth =
            "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
            "void main() {\n"
            "  float d = texelFetch(dep, ivec2(low_uv() * vec2(textureSize(dep, 0))), 0).r;\n"
            "  o = vec4(d >= 0.999999 ? 0.0 : max(log2(max(lin(d), 1.0)) / 16.0, 1.0 / 65535.0), 0.0, 0.0, 1.0);\n"
            "}\n";
        static const char *normal =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(low_uv() * vec2(sz));\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(0.5, 0.5, 0.5, 0.0); return; }\n"
            "  o = vec4(quick_normal(p) * 0.5 + 0.5, 1.0);\n"
            "}\n";
        // Trace (GI size, RGBA16F: rgb the gathered light, a the distance). b = (radius, thickness, origin bias,
        // max receiver distance), c = (rays, steps, frame turn, luminance clamp), d.y = hit rejection strength.
        // Per ray: cosine-distributed around the normal, steps placed quadratically (dense near the origin); the first
        // depth crossing is refined twice by bisection and is the end of the ray, hit or not (no leaking through
        // walls); a hit counts if within the thickness, faces the ray, lies on the ray (not just inside a depth slab)
        // and faces the camera - FastGI's validation. Its light: the scene pixel there, point sampled, luminance clamped.
        // ldep / nrm: the GI-size distance and normals; dep (the full-size depth) only gives the full size: the position
        // of a GI pixel is its full-size pixel's.
        static const char *trace =
            "layout(set = 0, binding = 0) uniform sampler2D ldep;\n"
            "layout(set = 0, binding = 1) uniform sampler2D nrm;\n"
            "layout(set = 0, binding = 2) uniform sampler2D scene;\n"
            "layout(set = 0, binding = 3) uniform sampler2D dep;\n"
            "vec2 fs, ls;\n"
            "vec2 at_uv(ivec2 q) { return (vec2(ivec2((vec2(q) + 0.5) / ls * fs)) + 0.5) / fs; }\n"
            "float dist_at(ivec2 q) { float v = texelFetch(ldep, q, 0).r; return v > 0.0 ? exp2(v * 16.0) : 0.0; }\n"
            "void main() {\n"
            "  fs = vec2(textureSize(dep, 0)), ls = pc.d.zw;\n"
            "  vec2 cs = vec2(textureSize(scene, 0));\n"
            "  ivec2 lp = ivec2(gl_FragCoord.xy);\n"
            "  float l0 = dist_at(lp);\n"
            "  if (l0 <= 0.0) { o = vec4(0.0); return; }\n"
            "  vec4 ne = texelFetch(nrm, lp, 0);\n"
            "  if (l0 >= pc.b.w || ne.a < 0.5) { o = vec4(0.0, 0.0, 0.0, l0); return; }\n"
            "  vec3 n = dec_n(ne);\n"
            "  vec3 org = vpos(at_uv(lp), l0) + n * pc.b.z;\n"
            "  vec3 ax = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);\n"
            "  vec3 tg = normalize(cross(ax, n)), bt = cross(n, tg);\n"
            "  float noise = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))) + pc.c.z);\n"
            "  int rays = int(pc.c.x), steps = int(pc.c.y);\n"
            "  float R = pc.b.x, th = pc.b.y, bias = pc.b.z;\n"
            "  vec3 rad = vec3(0.0);\n"
            "  for (int ray = 0; ray < rays; ray++) {\n"
            "    float u = (float(ray) + 0.5) / float(rays), an = 6.2831853 * fract(noise + float(ray) * 0.618034);\n"
            "    vec3 dir = tg * (sqrt(u) * cos(an)) + bt * (sqrt(u) * sin(an)) + n * sqrt(1.0 - u);\n"
            "    float prev = 0.0;\n"
            "    for (int i = 0; i < steps; i++) {\n"
            "      float t = (float(i) + 0.75 + noise * 0.25) / float(steps), dist = R * t * t;\n"
            "      vec3 p = org + dir * dist;\n"
            "      if (p.z <= 1.0) break;\n"
            "      vec2 h = puv(p);\n"
            "      if (any(lessThanEqual(h, vec2(0.0))) || any(greaterThanEqual(h, vec2(1.0)))) break;\n"
            "      float sd = dist_at(ivec2(h * ls));\n"
            "      if (sd <= 0.0) { prev = dist; continue; }\n"
            "      float delta = p.z - sd;\n"
            "      if (delta < 0.0) { prev = dist; continue; }\n"
            "      float lo = prev, hi = dist;\n"
            "      for (int k = 0; k < 2; k++) {\n"
            "        float mid = (lo + hi) * 0.5;\n"
            "        vec3 mp = org + dir * mid;\n"
            "        vec2 mh = puv(mp);\n"
            "        if (any(lessThanEqual(mh, vec2(0.0))) || any(greaterThanEqual(mh, vec2(1.0)))) break;\n"
            "        float md = dist_at(ivec2(mh * ls)), mdel = mp.z - md;\n"
            "        if (md > 0.0 && mdel >= 0.0) { hi = mid; h = mh; sd = md; delta = mdel; } else lo = mid;\n"
            "      }\n"
            "      dist = hi;\n"
            "      t = sqrt(hi / R);\n"
            "      if (delta <= th && dist > bias * 2.0) {\n"
            "        ivec2 hp = ivec2(h * ls);\n"
            "        vec3 hn = dec_n(texelFetch(nrm, hp, 0));\n"
            "        float facing = clamp(dot(hn, -dir) * 4.0, 0.0, 1.0), conf = 1.0;\n"
            "        if (pc.d.y > 0.0) {\n"
            "          float hl = sd;\n"
            "          vec3 hit = vpos(at_uv(hp), hl), to = hit - org;\n"
            "          float along = dot(to, dir), off = length(to - dir * along);\n"
            // (a GI pixel's footprint: the hit is found to within one)
            "          float foot = 2.0 * hl / (ls.y * pc.a.y);\n"
            "          float g = clamp(1.0 - off / max(1.0, min(th * 0.25, 10.0) + foot * 1.5), 0.0, 1.0);\n"
            "          vec3 ad = to * inversesqrt(max(dot(to, to), 1e-6));\n"
            "          g *= smoothstep(0.0, 0.15, dot(n, ad)) * smoothstep(0.0, 0.15, dot(hn, -ad));\n"
            "          g *= smoothstep(0.05, 0.25, dot(hn, normalize(-hit)));\n"
            "          g *= step(bias, along) * step(along, R);\n"
            "          conf = mix(1.0, g, pc.d.y);\n"
            "        }\n"
            "        vec2 e = min(h, 1.0 - h);\n"
            "        float fade = clamp(min(e.x, e.y) * 20.0, 0.0, 1.0) * (1.0 - t * t);\n"
            "        vec3 col = max(texelFetch(scene, ivec2(h * cs), 0).rgb, vec3(0.0));\n"
            "        col *= min(1.0, pc.c.w / max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 1e-4));\n"
            "        rad += col * facing * fade * conf;\n"
            "      }\n"
            "      break;\n"
            "    }\n"
            "  }\n"
            "  float df = clamp((pc.b.w - l0) / max(1.0, pc.b.w * 0.2), 0.0, 1.0);\n"
            "  o = vec4(rad * (df / float(rays)), l0);\n"
            "}\n";
        // Bilateral blur along b.xy (GI pixels), 5 taps at b.z spacing
        static const char *blur =
            "layout(set = 0, binding = 0) uniform sampler2D gi;\n"
            "layout(set = 0, binding = 1) uniform sampler2D nrm;\n"
            "void main() {\n"
            "  ivec2 ls = ivec2(pc.d.zw), p = ivec2(gl_FragCoord.xy);\n"
            "  vec4 c = texelFetch(gi, p, 0);\n"
            "  if (c.a <= 0.0) { o = vec4(0.0); return; }\n"
            "  vec3 n = dec_n(texelFetch(nrm, p, 0));\n"
            "  vec3 s = c.rgb;\n"
            "  float ws = 1.0;\n"
            "  for (int k = -2; k <= 2; k++) {\n"
            "    if (k == 0) continue;\n"
            "    ivec2 q = clamp(p + ivec2(pc.b.xy * (sign(float(k)) * floor(abs(float(k)) * pc.b.z + 0.5))), ivec2(0), ls - 1);\n"
            "    vec4 tp = texelFetch(gi, q, 0);\n"
            "    vec3 tn = dec_n(texelFetch(nrm, q, 0));\n"
            "    float w = (abs(k) == 1 ? 0.75 : 0.375) * geo(c.a, tp.a, n, tn);\n"
            "    s += tp.rgb * w; ws += w;\n"
            "  }\n"
            "  o = vec4(s / ws, c.a);\n"
            "}\n";
        // Temporal (GI size): the history where the pixel was (camera motion vectors at full size: xy velocity, z
        // previous / current distance), each of its 4 bilinear taps kept only at the expected distance, clamped to
        // this frame's neighbourhood (+15% of its range), weight b.y lowered with screen motion; b.w = 1: no history.
        static const char *temporal =
            "layout(set = 0, binding = 0) uniform sampler2D cur;\n"
            "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
            "layout(set = 0, binding = 2) uniform sampler2D hist;\n"
            "void main() {\n"
            "  ivec2 ls = ivec2(pc.d.zw), p = ivec2(gl_FragCoord.xy);\n"
            "  vec4 c = texelFetch(cur, p, 0);\n"
            "  if (c.a <= 0.0 || pc.b.w > 0.5) { o = c; return; }\n"
            "  vec2 uv = low_uv();\n"
            "  vec4 m = texelFetch(mv, ivec2(uv * vec2(textureSize(mv, 0))), 0);\n"
            "  vec2 pv = uv - m.xy;\n"
            "  if (any(lessThanEqual(pv, vec2(0.0))) || any(greaterThanEqual(pv, vec2(1.0)))) { o = c; return; }\n"
            "  float le = m.z * c.a;\n"
            "  vec2 px = pv * pc.d.zw - 0.5, b0 = floor(px), f = px - b0;\n"
            "  vec3 h = vec3(0.0);\n"
            "  float ws = 0.0;\n"
            "  for (int y = 0; y < 2; y++)\n"
            "    for (int x = 0; x < 2; x++) {\n"
            "      vec4 tp = texelFetch(hist, clamp(ivec2(b0) + ivec2(x, y), ivec2(0), ls - 1), 0);\n"
            "      vec2 bl = mix(1.0 - f, f, vec2(x, y));\n"
            "      float w = bl.x * bl.y * step(0.001, tp.a) * clamp(1.0 - abs(tp.a - le) / max(2.5, le * 0.01), 0.0, 1.0);\n"
            "      h += tp.rgb * w; ws += w;\n"
            "    }\n"
            "  if (ws < 0.05) { o = c; return; }\n"
            "  h /= ws;\n"
            "  vec3 lo = c.rgb, hi = c.rgb;\n"
            "  const ivec2 nb[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));\n"
            "  for (int i = 0; i < 4; i++) {\n"
            "    vec4 tp = texelFetch(cur, clamp(p + nb[i], ivec2(0), ls - 1), 0);\n"
            "    if (tp.a > 0.0 && abs(tp.a - c.a) < max(2.5, c.a * 0.01)) { lo = min(lo, tp.rgb); hi = max(hi, tp.rgb); }\n"
            "  }\n"
            "  vec3 mg = (hi - lo) * 0.15 + 0.001;\n"
            "  h = clamp(h, max(vec3(0.0), lo - mg), hi + mg);\n"
            "  float w = pc.b.y * clamp(ws, 0.0, 1.0) / (1.0 + length(m.xy * pc.d.zw) * 0.1);\n"
            "  o = vec4(mix(c.rgb, h, w), c.a);\n"
            "}\n";
        // Composite (full size): the GI's 4 nearest pixels, bilinear x bilateral; b.x = strength (intensity x
        // reflectance). Added onto the scene (exec.c: additive blending, colour only); b.y = 1 the debug view: the
        // light alone, b.z x it, tone mapped (x / (1 + x)) into RGBA8.
        // dep (binding 0) the full-size depth, nrm the GI-size normals; the pixel's own normal rebuilt here (5 reads:
        // 0.14 ms at 4K, as with the full-size normals read)
        static const char *composite =
            "layout(set = 0, binding = 1) uniform sampler2D gi;\n"
            "layout(set = 0, binding = 2) uniform sampler2D nrm;\n"
            "void main() {\n"
            "  ivec2 fsz = textureSize(dep, 0), ls = ivec2(pc.d.zw);\n"
            "  sz = fsz;\n"
            "  vec2 uv = gl_FragCoord.xy * pc.c.xy;\n"
            "  ivec2 fp = clamp(ivec2(uv * vec2(fsz)), ivec2(0), fsz - 1);\n"
            "  float d0 = texelFetch(dep, fp, 0).r;\n"
            "  vec3 g = vec3(0.0);\n"
            "  if (d0 < 0.999999) {\n"
            "    float l0 = lin(d0);\n"
            "    vec3 n = quick_normal(fp);\n"
            "    vec2 px = uv * pc.d.zw - 0.5, b0 = floor(px), f = px - b0;\n"
            "    float ws = 0.0;\n"
            "    for (int y = 0; y < 2; y++)\n"
            "      for (int x = 0; x < 2; x++) {\n"
            "        ivec2 q = clamp(ivec2(b0) + ivec2(x, y), ivec2(0), ls - 1);\n"
            "        vec4 tp = texelFetch(gi, q, 0);\n"
            "        vec3 tn = dec_n(texelFetch(nrm, q, 0));\n"
            "        vec2 bl = mix(1.0 - f, f, vec2(x, y));\n"
            "        float w = bl.x * bl.y * geo(l0, tp.a, n, tn);\n"
            "        g += tp.rgb * w; ws += w;\n"
            "      }\n"
            "    g = g / max(ws, 1e-4) * pc.b.x;\n"
            "  }\n"
            "  if (pc.b.y > 0.5) { g *= pc.b.z; o = vec4(g / (1.0 + g), 1.0); }\n"
            "  else o = vec4(g, 0.0);\n"
            "}\n";
        static char *src[6];
        int k = !strcmp(name, "gi_normal.frag") ? 0 : !strcmp(name, "gi_trace.frag") ? 1 : !strcmp(name, "gi_blur.frag") ? 2
              : !strcmp(name, "gi_temporal.frag") ? 3 : !strcmp(name, "gi_composite.frag") ? 4 : !strcmp(name, "gi_depth.frag") ? 5 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            const char *bodies[6] = {normal, trace, blur, temporal, composite, depth};
            const char *pre = k == 0 || k == 4 ? vpos_src : "", *pre2 = k == 0 || k == 4 ? qnormal : "";
            size_t n = strlen(common) + strlen(pre) + strlen(pre2) + strlen(bodies[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s%s", common, pre, pre2, bodies[k]);
        }
        return src[k];
    }
    // TAA (exec.c taa_run), on the game's HDR scene before its bloom: t = this frame (jittered), h = the history (rgb,
    // a = its distance), mv = the motion vectors (xy velocity, z previous / current distance), dep = depth.
    // a = the projection (P00, P11, A, B), b = (reset, history weight, depth mode, 0). Blended in a tone-mapped space
    // (c / (1 + max c): no fireflies from the bright highlights); the velocity of the nearest of the 3x3 (edges keep the
    // foreground's); the history by Catmull-Rom, clipped to the 3x3's YCoCg mean +- 1.25 sigma (variance clipping),
    // dropped where its distance is not the one expected (uncovered). Out: rgb, a = this distance.
    if (!strcmp(name, "taa.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
               "layout(set = 0, binding = 3) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }\n"
               "vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }\n"
               "vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
               "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
               "vec3 cat(vec2 uv, vec2 sz) {\n"  // Catmull-Rom, 5 bilinear taps (the corners dropped)
               "  vec2 sp = uv * sz, tp = floor(sp - 0.5) + 0.5, f = sp - tp;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (tp - 1.0) / sz, t3 = (tp + 2.0) / sz, t12 = (tp + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y) + texture(h, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y)\n"
               "         + texture(h, t12).rgb * (w12.x * w12.y) + texture(h, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y)\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return max(r / ws, vec3(0.0));\n"
               "}\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  vec3 c = max(texelFetch(t, p, 0).rgb, vec3(0.0));\n"
               "  vec2 dsc = vec2(dsz) / vec2(sz);\n"
               "  float l = lin(texelFetch(dep, min(ivec2(vec2(p) * dsc), dsz - 1), 0).r);\n"
               "  if (pc.b.x > 0.5) { o = vec4(c, l); return; }\n"
               "  vec3 m1 = vec3(0.0), m2 = vec3(0.0);\n"
               "  float dn = 1.0;\n"
               "  ivec2 np = p;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);\n"
               "      vec3 s = ycc(tm(max(texelFetch(t, q, 0).rgb, vec3(0.0))));\n"
               "      m1 += s; m2 += s * s;\n"
               "      float d = texelFetch(dep, min(ivec2(vec2(q) * dsc), dsz - 1), 0).r;\n"
               "      if (d < dn) { dn = d; np = q; }\n"
               "    }\n"
               "  m1 /= 9.0;\n"
               "  vec3 sg = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));\n"
               "  vec4 v = texelFetch(mv, min(ivec2(vec2(np) * vec2(textureSize(mv, 0)) / vec2(sz)), textureSize(mv, 0) - 1), 0);\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz), pu = uv - v.xy;\n"
               "  if (any(lessThan(pu, vec2(0.0))) || any(greaterThan(pu, vec2(1.0)))) { o = vec4(c, l); return; }\n"
               "  ivec2 hs = textureSize(h, 0);\n"  // (an in-between image at a reduced size: N's history is larger)
               "  float lh = texelFetch(h, clamp(ivec2(pu * vec2(hs)), ivec2(0), hs - 1), 0).a, le = v.z * l;\n"
               "  float keep = clamp(1.0 - (abs(lh - le) / max(le * 0.02, 2.0) - 1.0), 0.0, 1.0);\n"
               "  vec3 hy = ycc(tm(cat(pu, vec2(hs))));\n"
               "  vec3 lo = m1 - 1.25 * sg, hi = m1 + 1.25 * sg, ce = 0.5 * (hi + lo), ex = 0.5 * (hi - lo) + 1e-4;\n"
               "  vec3 dv = (hy - ce) / ex;\n"
               "  float mx = max(abs(dv.x), max(abs(dv.y), abs(dv.z)));\n"
               "  if (mx > 1.0) hy = ce + (hy - ce) / mx;\n"
               "  vec3 cy = ycc(tm(c));\n"
               "  float w = pc.b.y * keep;\n"
               "  o = vec4(itm(rgb(mix(cy, hy, w))), l);\n"
               "}\n";
    }
    // FSR 3's mask of the interface (exec.c post_process): the pixels the interface drew over the composited scene
    // (t0 the final image, t1 its copy before the first interface draw) - 1 there: FSR takes them as they are
    if (!strcmp(name, "ui_mask.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 d = abs(texelFetch(t0, p, 0).rgb - texelFetch(t1, min(p, textureSize(t1, 0) - 1), 0).rgb);\n"
               "  o = vec4(max(d.r, max(d.g, d.b)) > 2.0 / 255.0 ? 1.0 : 0.0);\n"
               "}\n";
    }
    // The scene depth as R32F (FSR 3 reads a colour image: exec.c post_process)
    if (!strcmp(name, "depth_r32.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texelFetch(dep, ivec2(gl_FragCoord.xy), 0).r); }\n";
    }
    // N's result moved to an in-between image (exec.c, frame interpolation: its SSAO / GI are N's, taken where its
    // motion vectors - relative to N - say each pixel was): src at uv - velocity (uv itself off the image).
    // b.xy = 1 / the output's size.
    if (!strcmp(name, "reproject.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D src;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.b.xy;\n"
               "  ivec2 ms = textureSize(mv, 0);\n"
               "  vec2 puv = uv - texelFetch(mv, min(ivec2(uv * vec2(ms)), ms - 1), 0).xy;\n"
               "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThanEqual(puv, vec2(1.0)))) puv = uv;\n"
               "  o = texture(src, puv);\n"
               "}\n";
    }
    // An in-between image with FSR 3.1 (exec.c post_process, frame interpolation): FSR's own history goes forward in time
    // with the real frames, so the image is resolved here instead, like TAA - t = the image (render size, after the
    // colour pass), h = N's FSR 3.1 output (the output's size), taken where the motion vectors (relative to N) say each
    // pixel was and clipped to t's 3x3 (YCoCg min / max), mixed with t by the history weight. Uncovered or off-image: t.
    // h is sampled Catmull-Rom (5 bilinear taps): bilinear, reprojecting softened it and the in-between images were 13 %
    // less detailed than the real frames - a sharpness that pulsed at the frame rate (exec.c then adds FSR's RCAS).
    // a.xy = 1 / the output's size, a.zw = 1 / t's size, b.x = the history's weight.
    if (!strcmp(name, "side_resolve.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
               "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
               "vec3 cr(vec2 uv) {\n"  // (Catmull-Rom from 5 bilinear taps, the corners left out)
               "  vec2 sz = vec2(textureSize(h, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (c1 - 1.0) / sz, t3 = (c1 + 2.0) / sz, t12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * w12.x * w0.y + texture(h, vec2(t0.x, t12.y)).rgb * w0.x * w12.y\n"
               "         + texture(h, t12).rgb * w12.x * w12.y + texture(h, vec2(t3.x, t12.y)).rgb * w3.x * w12.y\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return max(r / ws, vec3(0.0));\n"
               "}\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec3 c = texture(t, uv).rgb;\n"
               "  vec3 mn = vec3(1e9), mx = vec3(-1e9);\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec3 s = ycc(texture(t, uv + vec2(x, y) * pc.a.zw).rgb);\n"
               "      mn = min(mn, s); mx = max(mx, s);\n"
               "    }\n"
               "  ivec2 ms = textureSize(mv, 0);\n"
               "  vec2 puv = uv - texelFetch(mv, min(ivec2(uv * vec2(ms)), ms - 1), 0).xy;\n"
               "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThanEqual(puv, vec2(1.0)))) { o = vec4(c, 1.0); return; }\n"
               "  vec3 hc = rgb(clamp(ycc(cr(puv)), mn, mx));\n"
               "  o = vec4(mix(c, hc, pc.b.x), 1.0);\n"
               "}\n";
    }
    // FSR 3.1's output with the interface composited after it (exec.c post_process_ex): t0 = FSR's output, t1 = the image
    // it was given (the render size, the interface not jittered), m = the interface mask (render size). Where the mask
    // says interface (bilinear: its edges blend), t1 by Catmull-Rom (5 bilinear taps, as side_resolve.frag's history).
    // a.xy = 1 / the output's size.
    if (!strcmp(name, "ui_composite.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(set = 0, binding = 2) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 cr(vec2 uv) {\n"
               "  vec2 sz = vec2(textureSize(t1, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, q0 = (c1 - 1.0) / sz, q3 = (c1 + 2.0) / sz, q12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(t1, vec2(q12.x, q0.y)).rgb * w12.x * w0.y + texture(t1, vec2(q0.x, q12.y)).rgb * w0.x * w12.y\n"
               "         + texture(t1, q12).rgb * w12.x * w12.y + texture(t1, vec2(q3.x, q12.y)).rgb * w3.x * w12.y\n"
               "         + texture(t1, vec2(q12.x, q3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return clamp(r / ws, 0.0, 1.0);\n"
               "}\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec3 f = texelFetch(t0, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "  float k = texture(m, uv).r;\n"
               "  o = vec4(k > 0.0 ? mix(f, cr(uv), k) : f, 1.0);\n"
               "}\n";
    }
    // An in-between image with frame N's interface where N's mask says interface (exec.c ip_ui_keep): t = the image,
    // h = N's processed image of the same size, m = N's mask (the render size: sampled, any coverage counts - its edges'
    // antialiasing too). a.xy = 1 / the size.
    if (!strcmp(name, "ui_keep.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  o = vec4(texture(m, uv).r > 0.0 ? texture(h, uv).rgb : texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);\n"
               "}\n";
    }
    // ---- Frame generation by optical flow (fg_method = flow: exec.c flow_*). Vectors are N's uv - N-1's, as mv.frag's.
    // The luminance of N's (r) and N-1's (g) processed images at the flow's size (~640 wide: the search reaches as far
    // in the image at 720p as at 4K), b = N's interface mask there (those pixels take no part in the matching).
    // 4 bilinear taps a pixel (a box of ~4 x 4 source pixels at 4K). a.xy = 1 / the size, a.z = 1: a mask is bound.
    if (!strcmp(name, "flow_luma.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t1;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 2) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy, d = pc.a.xy * 0.25;\n"
               "  vec3 a = vec3(0.0), b = vec3(0.0);\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    vec2 q = uv + vec2((i & 1) != 0 ? d.x : -d.x, (i & 2) != 0 ? d.y : -d.y);\n"
               "    a += texture(t1, q).rgb; b += texture(t0, q).rgb;\n"
               "  }\n"
               "  o = vec4(lum(a * 0.25), lum(b * 0.25), pc.a.z > 0.5 ? texture(m, uv).r : 0.0, 0.0);\n"
               "}\n";
    }
    // Block matching on that luminance, one 8 x 8 block every 4 pixels (a.w: the stride): the analytic vector (the
    // motion vectors at the block's centre and corners: camera and objects) against a search around it (+-a.z pixels,
    // in steps of 2: flow_refine finds the pixel) and around no motion (the interface, still particles). The search
    // wins only clearly (its difference under 0.8 x the analytic one's, and by 0.4 % of white): flat or relit areas keep
    // the motion vectors. Out: xy = the vector (uv), zw = the analytic one at the block's centre (they differ: the
    // search replaced it). a.xy = 1 / the luminance's size, b.x = 1: motion vectors bound. (Every pixel of the search: 0.50
    // ms at 4K, the grid too small to fill the GPU; in steps of 2, 0.2.)
    // flow_refine.frag: again from the best of each block's and its 8 neighbours' results (spreading a good vector),
    // then +-1 and +-0.5 pixel around it (bilinear: sub-pixel), against the analytic vector.
    if (!strcmp(name, "flow_match.frag") || !strcmp(name, "flow_refine.frag")) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(set = 0, binding = 0) uniform sampler2D L;\n"
            "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
            "layout(set = 0, binding = 2) uniform sampler2D F;\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "vec2 lsz;\n"
            // N's 16 samples of the block, read once (prep) for all the candidates sad() tries: the same for every one
            // (-1: the interface's, left out). Half the matching's texture reads (the phone: 3.7 ms of a frame's GPU)
            "float na[16];\n"
            "void prep(vec2 c) {\n"
            "  for (int y = 0; y < 4; y++)\n"
            "    for (int x = 0; x < 4; x++) {\n"
            "      vec4 a = texture(L, (c + vec2(float(x * 2 - 3), float(y * 2 - 3))) / lsz);\n"
            "      na[y * 4 + x] = a.b > 0.0 ? -1.0 : a.r;\n"
            "    }\n"
            "}\n"
            "float sad(vec2 c, vec2 v) {\n"
            "  float s = 0.0, n = 0.0;\n"
            "  for (int y = 0; y < 4; y++)\n"
            "    for (int x = 0; x < 4; x++) {\n"
            "      float a = na[y * 4 + x];\n"
            "      if (a < 0.0) continue;\n"
            "      vec2 q = c + vec2(float(x * 2 - 3), float(y * 2 - 3));\n"
            "      s += abs(a - texture(L, (q - v) / lsz).g); n += 1.0;\n"
            "    }\n"
            "  return n > 0.0 ? s / n : 0.0;\n"
            "}\n"
            "vec2 va_at(vec2 c) {\n"
            "  if (pc.b.x < 0.5) return vec2(0.0);\n"
            "  ivec2 ms = textureSize(mv, 0);\n"
            "  return texelFetch(mv, clamp(ivec2(c / lsz * vec2(ms)), ivec2(0), ms - 1), 0).xy * lsz;\n"
            "}\n";
        static char src[2][8192];
        int k = !strcmp(name, "flow_refine.frag");
        if (!src[k][0] && !k)
            snprintf(src[k], sizeof src[k], "%s%s", common,
                     "void main() {\n"
                     "  lsz = 1.0 / pc.a.xy;\n"
                     "  vec2 c = (floor(gl_FragCoord.xy) + 0.5) * pc.a.w;\n"
                     "  prep(c);\n"
                     "  vec2 va = va_at(c), best = va;\n"
                     "  float sa = sad(c, va), sb = sa, s;\n"
                     "  int R = int(pc.a.z);\n"
                     "  vec2 v0 = floor(va + 0.5);\n"
                     "  for (int y = -R; y <= R; y += 2)\n"
                     "    for (int x = -R; x <= R; x += 2) {\n"
                     "      vec2 v = v0 + vec2(x, y);\n"
                     "      s = sad(c, v); if (s < sb) { sb = s; best = v; }\n"
                     "    }\n"
                     "  for (int y = -1; y <= 1; y++)\n"
                     "    for (int x = -1; x <= 1; x++) { s = sad(c, vec2(x, y)); if (s < sb) { sb = s; best = vec2(x, y); } }\n"
                     "  for (int i = 0; i < 4; i++) {\n"
                     "    vec2 v = va_at(c + vec2((i & 1) != 0 ? 3.0 : -3.0, (i & 2) != 0 ? 3.0 : -3.0));\n"
                     "    s = sad(c, v); if (s < sb) { sb = s; best = v; }\n"
                     "  }\n"
                     "  bool rep = sb < sa * 0.8 - 0.004;\n"
                     "  o = vec4((rep ? best : va) / lsz, va / lsz);\n"
                     "}\n");
        else if (!src[k][0])
            snprintf(src[k], sizeof src[k], "%s%s", common,
                     "void main() {\n"
                     "  lsz = 1.0 / pc.a.xy;\n"
                     "  ivec2 g = ivec2(gl_FragCoord.xy), gs = textureSize(F, 0);\n"
                     "  vec2 c = (vec2(g) + 0.5) * pc.a.w;\n"
                     "  prep(c);\n"
                     "  vec2 va = va_at(c), best = va;\n"
                     "  float sa = sad(c, va), sb = sa, s;\n"
                     "  for (int y = -1; y <= 1; y++)\n"
                     "    for (int x = -1; x <= 1; x++) {\n"
                     "      vec2 v = texelFetch(F, clamp(g + ivec2(x, y), ivec2(0), gs - 1), 0).xy * lsz;\n"
                     "      s = sad(c, v); if (s < sb) { sb = s; best = v; }\n"
                     "    }\n"
                     "  for (int k = 0; k < 2; k++) {\n"
                     "    float h = k == 0 ? 1.0 : 0.5;\n"
                     "    vec2 b0 = best;\n"
                     "    for (int i = 0; i < 4; i++) {\n"
                     "      vec2 w = b0 + h * (i == 0 ? vec2(1.0, 0.0) : i == 1 ? vec2(-1.0, 0.0) : i == 2 ? vec2(0.0, 1.0) : vec2(0.0, -1.0));\n"
                     "      s = sad(c, w); if (s < sb) { sb = s; best = w; }\n"
                     "    }\n"
                     "  }\n"
                     "  bool rep = sb < sa * 0.8 - 0.004;\n"
                     "  o = vec4((rep ? best : va) / lsz, va / lsz);\n"
                     "}\n");
        return src[k];
    }
    // Per pixel of the luminance: where a block near it replaced its vector, the analytic vector against the four
    // nearest blocks' on the pixel's 3 x 3 (the block's vector only clearly better, as above) - only blocks whose own
    // analytic vector is the pixel's (within a pixel): the same surface. Any block, a character's pixels took the
    // ground's vector next to them where the animation fit it better locally (grass in the shorts, the background torn
    // into the arms). Out: xy = the vector (uv), w = 1: the block's. a.xy = 1 / the luminance's size, a.w = the blocks'
    // stride, b.x = 1: motion vectors.
    if (!strcmp(name, "flow_pick.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D L;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
               "layout(set = 0, binding = 2) uniform sampler2D F;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec2 lsz;\n"
               "float err(vec2 p, vec2 v) {\n"
               "  float s = 0.0, n = 0.0;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec2 q = p + vec2(x, y);\n"
               "      vec4 a = texture(L, q / lsz);\n"
               "      if (a.b > 0.0) continue;\n"
               "      s += abs(a.r - texture(L, (q - v) / lsz).g); n += 1.0;\n"
               "    }\n"
               "  return n > 0.0 ? s / n : 0.0;\n"
               "}\n"
               "void main() {\n"
               "  lsz = 1.0 / pc.a.xy;\n"
               "  vec2 p = gl_FragCoord.xy;\n"
               "  vec2 va = vec2(0.0);\n"
               "  if (pc.b.x > 0.5) { ivec2 ms = textureSize(mv, 0); va = texelFetch(mv, clamp(ivec2(p / lsz * vec2(ms)), ivec2(0), ms - 1), 0).xy * lsz; }\n"
               "  ivec2 gs = textureSize(F, 0), g0 = ivec2(floor(p / pc.a.w - 0.5));\n"
               "  vec4 f[4];\n"
               "  bool any_rep = false;\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    f[i] = texelFetch(F, clamp(g0 + ivec2(i & 1, i >> 1), ivec2(0), gs - 1), 0);\n"
               "    bool use = f[i].xy != f[i].zw && length(f[i].zw * lsz - va) < 1.0;\n"
               "    if (!use) f[i].xy = vec2(1e6);\n"
               "    any_rep = any_rep || use;\n"
               "  }\n"
               "  if (!any_rep) { o = vec4(va / lsz, 0.0, 0.0); return; }\n"
               "  float ea = err(p, va), eb = 1e9;\n"
               "  vec2 best = va;\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    if (f[i].x > 1e5) continue;\n"
               "    vec2 v = f[i].xy * lsz; float e = err(p, v); if (e < eb) { eb = e; best = v; }\n"
               "  }\n"
               "  bool rep = eb < ea * 0.8 - 0.004;\n"
               "  o = vec4((rep ? best : va) / lsz, 0.0, rep ? 1.0 : 0.0);\n"
               "}\n";
    }
    // N's motion field at the depth's size: xy = the vector (the motion vectors', or the flow's where flow_pick took a
    // block's), z = the distance (linear depth), w = the distance in N-1 / in N (mv.frag's z; 1 for the flow's), + 16
    // where the flow's, negative where N's interface mask is set. a = P00, P11, A, B; b.x = 1: depth stored as
    // (z_ndc + 1) / 2, b.y / b.z / b.w = 1: motion vectors / depth / mask bound; c.x = 1: flow_pick's bound.
    if (!strcmp(name, "flow_field.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D mv;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 2) uniform sampler2D pk;\n"
               "layout(set = 0, binding = 3) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.x > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 uv = (vec2(p) + 0.5) / pc.c.zw;\n"
               "  vec4 m4 = vec4(0.0, 0.0, 1.0, 0.0);\n"
               "  if (pc.b.y > 0.5) { ivec2 ms = textureSize(mv, 0); m4 = texelFetch(mv, clamp(ivec2(uv * vec2(ms)), ivec2(0), ms - 1), 0); }\n"
               "  vec4 f = vec4(0.0);\n"
               "  if (pc.c.x > 0.5) { ivec2 ks = textureSize(pk, 0); f = texelFetch(pk, clamp(ivec2(uv * vec2(ks)), ivec2(0), ks - 1), 0); }\n"
               "  bool rep = f.w > 0.5;\n"
               "  vec2 v = rep ? f.xy : m4.xy;\n"
               "  float l = 1.0;\n"
               "  if (pc.b.z > 0.5) { ivec2 ds = textureSize(dep, 0); l = lin(texelFetch(dep, clamp(ivec2(uv * vec2(ds)), ivec2(0), ds - 1), 0).r); }\n"
               "  float r = rep || pc.b.y < 0.5 ? 1.0 : clamp(m4.z, 0.01, 15.0);\n"
               "  bool ui = pc.b.w > 0.5 && texture(m, uv).r > 0.0;\n"
               "  float w = r + (rep ? 16.0 : 0.0);\n"
               "  o = vec4(v, max(l, 1e-4), ui ? -w : w);\n"
               "}\n";
    }
    // The field dilated: xy = the vector of the nearest of each pixel's 3 x 3 (moving things keep their edges), zw =
    // of the farthest (the background's: what an uncovered pixel moves with)
    if (!strcmp(name, "flow_dilate.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D F;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), fs = textureSize(F, 0);\n"
               "  float ln = 1e30, lf = -1.0;\n"
               "  vec2 fg = vec2(0.0), bg = vec2(0.0);\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec4 f = texelFetch(F, clamp(p + ivec2(x, y), ivec2(0), fs - 1), 0);\n"
               "      if (f.z < ln) { ln = f.z; fg = f.xy; }\n"
               "      if (f.z > lf) { lf = f.z; bg = f.xy; }\n"
               "    }\n"
               "  o = vec4(fg, bg);\n"
               "}\n";
    }
    // The in-between image at t (a.x, 0..1 from N-1 to N), at the processed images' size: c0 = N-1's, c1 = N's, fn = N's
    // field, dn = it dilated, fp = N-1's field, m = N's interface mask. Each pixel q: the point of N that is at q at t
    // (q + (1 - t) v(p) = p, three steps from three starts: the dilated nearest vector, the farthest one, the pixel's
    // own), each checked with its own vector (within 1.5 of the field's pixels: found); of those found, the nearest
    // (what is in front at t). One start alone (the nearest), animated limbs - whose vectors differ pixel to pixel -
    // were often not found and took the background's way. Found: N there, and N-1 where the point was (p - v) unless N-1's field shows something
    // nearer there than the point was (its distance x w: it was covered - N alone) or N-1's interface; mixed by t. Not
    // found (uncovered between t and N: covered in N): N-1 along the background's vector. Where N's mask says interface,
    // N's own pixel. Both images Catmull-Rom (5 bilinear taps, as side_resolve.frag's history): bilinear, the moved
    // images were softer than the real ones. a.y = 1: fp bound, a.z = 1: m bound, a.w = 1: the depth test on;
    // b.xy = 1 / the output's size; c.x = 1: the debug view (red: the flow's vectors, blue: N-1 alone, green: N alone).
    if (!strcmp(name, "flow_warp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D c0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D c1;\n"
               "layout(set = 0, binding = 2) uniform sampler2D fn;\n"
               "layout(set = 0, binding = 3) uniform sampler2D dn;\n"
               "layout(set = 0, binding = 4) uniform sampler2D fp;\n"
               "layout(set = 0, binding = 5) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 cr(sampler2D h, vec2 uv) {\n"
               "  vec2 sz = vec2(textureSize(h, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (c1 - 1.0) / sz, t3 = (c1 + 2.0) / sz, t12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * w12.x * w0.y + texture(h, vec2(t0.x, t12.y)).rgb * w0.x * w12.y\n"
               "         + texture(h, t12).rgb * w12.x * w12.y + texture(h, vec2(t3.x, t12.y)).rgb * w3.x * w12.y\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return clamp(r / ws, 0.0, 1.0);\n"
               "}\n"
               "vec4 at(sampler2D s, vec2 uv) { ivec2 z = textureSize(s, 0); return texelFetch(s, clamp(ivec2(uv * vec2(z)), ivec2(0), z - 1), 0); }\n"
               "bool inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))); }\n"
               "void main() {\n"
               "  vec2 q = gl_FragCoord.xy * pc.b.xy;\n"
               "  float t = pc.a.x;\n"
               "  if (pc.a.z > 0.5 && texture(m, q).r > 0.0) { o = vec4(texelFetch(c1, ivec2(gl_FragCoord.xy), 0).rgb, 1.0); return; }\n"  // (a = 1: flow_sharp leaves it)
               "  vec2 fs = vec2(textureSize(fn, 0));\n"
               "  vec2 p = q;\n"
               "  vec4 f = vec4(0.0);\n"
               "  bool found = false;\n"
               "  vec4 d0 = at(dn, q);\n"
               "  for (int k = 0; k < 3; k++) {\n"
               "    vec2 c = q + (1.0 - t) * (k == 0 ? d0.xy : k == 1 ? d0.zw : at(fn, q).xy);\n"
               "    for (int i = 0; i < 2; i++) { vec4 d = at(dn, c); c = q + (1.0 - t) * (k == 0 ? d.xy : k == 1 ? d.zw : at(fn, c).xy); }\n"
               "    vec4 g = at(fn, c);\n"
               "    vec2 e = (q + (1.0 - t) * g.xy - c) * fs;\n"
               "    if (dot(e, e) < 2.25 && inside(c) && (!found || g.z < f.z)) { found = true; p = c; f = g; }\n"
               "  }\n"
               "  vec3 col, dbg = vec3(0.0);\n"
               "  if (found) {\n"
               "    vec2 r = p - f.xy;\n"
               "    bool ok0 = inside(r);\n"
               "    float w = abs(f.w);\n"
               "    if (w >= 16.0) { w -= 16.0; dbg.r = 1.0; }\n"
               "    if (ok0 && pc.a.y > 0.5) {\n"
               "      vec4 g = at(fp, r);\n"
               "      if (g.w < 0.0 || (pc.a.w > 0.5 && g.z < f.z * w * 0.94)) ok0 = false;\n"
               "    }\n"
               "    vec3 n1 = cr(c1, p);\n"
               "    col = ok0 ? mix(cr(c0, r), n1, t) : n1;\n"
               "    if (!ok0) dbg.g = 1.0;\n"
               "  } else {\n"
               "    vec2 vb = at(dn, q + (1.0 - t) * d0.zw).zw, r = q - t * vb;\n"
               "    bool ok0 = inside(r) && (pc.a.y < 0.5 || at(fp, r).w >= 0.0);\n"
               "    col = ok0 ? cr(c0, r) : cr(c1, q + (1.0 - t) * vb);\n"
               "    dbg.b = 1.0;\n"
               "  }\n"
               "  if (pc.c.x > 0.5) col = mix(col, dbg, 0.5 * max(dbg.r, max(dbg.g, dbg.b)));\n"
               "  o = vec4(col, 0.0);\n"
               "}\n";
    }
    // The in-between image sharpened back (a.x: the strength): mixing N-1 and N moved a fraction of a pixel apart
    // softens what both show. Unsharp on the 4 neighbours, clamped to the 3 x 3's range (no halos); the interface (alpha
    // 1, N's own) as it is.
    if (!strcmp(name, "flow_sharp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), z = textureSize(t, 0) - 1;\n"
               "  vec4 c4 = texelFetch(t, p, 0);\n"
               "  if (c4.a > 0.5) { o = vec4(c4.rgb, 1.0); return; }\n"
               "  vec3 c = c4.rgb, mn = c, mx = c, s = vec3(0.0);\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      if (x == 0 && y == 0) continue;\n"
               "      vec3 n = texelFetch(t, clamp(p + ivec2(x, y), ivec2(0), z), 0).rgb;\n"
               "      mn = min(mn, n); mx = max(mx, n);\n"
               "      if (x == 0 || y == 0) s += n;\n"
               "    }\n"
               "  o = vec4(clamp(c + pc.a.x * (c - s * 0.25), mn, mx), 1.0);\n"
               "}\n";
    }
    // A post image back into a target (b.xy = 1 / the output's size; sampled, the sizes may differ): TAA's result into
    // the game's scene
    if (!strcmp(name, "copy.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, gl_FragCoord.xy * pc.b.xy).rgb, 1.0); }\n";
    }
    // Camera motion vectors (exec.c scene_hook_run): each pixel's position, rebuilt from its depth, taken to the
    // previous frame by m = previous view-projection x inverse of this one (the game's matrices, GL style: NDC y up).
    // a.x = 1: depth stored as (z_ndc + 1) / 2; a.y = the sign of NDC y at the bottom of the image (+1: row 0 is NDC
    // y = -1). Out (RGBA16F): xy = velocity = this frame's uv - the previous frame's, in uv units; z = the pixel's
    // distance in the previous frame / its distance now (q.w: the input point is this frame's clip / its w), for the
    // temporal effects' disocclusion test.
    if (!strcmp(name, "mv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D obj;\n"  // (exec.c: per-object velocities, w = 1 written)
               "layout(push_constant) uniform P { mat4 m; vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(dep, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  vec4 ov = texelFetch(obj, min(p, textureSize(obj, 0) - 1), 0);\n"
               "  if (ov.w > 0.5) { o = vec4(ov.xyz, 1.0); return; }\n"
               "  float d = texelFetch(dep, p, 0).r;\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz);\n"
               "  vec2 n = uv * 2.0 - 1.0;\n"
               "  n.y *= pc.a.y;\n"
               "  vec4 q = pc.m * vec4(n, pc.a.x > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec2 pn = q.xy / q.w;\n"
               "  pn.y *= pc.a.y;\n"
               "  o = vec4(uv - (pn * 0.5 + 0.5), q.w, 1.0);\n"
               "}\n";
    }
    // Motion vectors shown (b.xy = 1 / the output's size, b.z = the scale: velocity in uv x b.z around mid-grey) - the
    // F12 debug view onto the scene, and LARECOMP_MV_DUMP's RGBA8 encoding (b.z = 32: +-1/64 of the image, 8 bits).
    if (!strcmp(name, "mv_show.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 v = texture(t, gl_FragCoord.xy * pc.b.xy).rg * pc.b.z;\n"
               "  o = vec4(clamp(0.5 + v, 0.0, 1.0), 0.5, 1.0);\n"
               "}\n";
    }
    // The occlusion onto the scene: multiplied by the pipeline's blending (exec.c), or shown alone (the debug view and
    // LARECOMP_SSAO_DUMP). b.xy = 1 / the output's size (the occlusion is at the depth's size: sampled, not fetched);
    // b.z = a scale when shown alone (the scene is HDR: 1.0 is far past white after the game's tone mapping), 0: none.
    // Day/night lights on window and lantern glass: a colour key on a copy of the scene; the output multiplies the scene
    // (blend 1): the tint, or for glass a warm glow (exec.c ssao_run). a.rgb = the tint (linear), a.w = night 0..1,
    // b.rgb = the glow's multiplier of the glass's daylight colour.
    if (!strcmp(name, "tint_glow.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               // b.w = the picture's height / 720 (the light pools' radii)
               // Pale cyan-blue pixels (green close to blue, red well below it): window and lantern glass. Measured in
               // Viridian: glass g/b 0.8-0.9, b/r 1.4-2; pond water g/b 0.4 (and sat 0.9); roofs and walls are red or green led.
               "float key(vec3 c) {\n"
               "  float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));\n"
               "  float sat = (mx - mn) / max(mx, 1e-4), lum = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
               "  float gb = c.g / max(c.b, 1e-4), br = c.b / max(c.r, 1e-4);\n"
               "  return smoothstep(0.55, 0.68, gb) * (1.0 - smoothstep(0.95, 1.05, gb)) * smoothstep(1.25, 1.5, br)\n"
               "       * smoothstep(0.15, 0.25, lum) * (1.0 - smoothstep(0.62, 0.75, sat));\n"
               "}\n"
               "vec3 at(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), textureSize(t, 0) - 1), 0).rgb; }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  float m = key(at(p));\n"
               // water foam is the same pale blue: leave out pixels with deep-blue water close by (8 taps, 7 px out)
               "  float water = 0.0;\n"
               "  for (int i = 0; i < 8; i++) {\n"
               "    vec2 d = vec2(cos(float(i) * 0.7854), sin(float(i) * 0.7854)) * 7.0;\n"
               "    vec3 n = at(p + ivec2(d));\n"
               "    float ns = (max(n.r, max(n.g, n.b)) - min(n.r, min(n.g, n.b))) / max(max(n.r, max(n.g, n.b)), 1e-4);\n"
               "    water = max(water, step(0.75, ns) * step(n.g / max(n.b, 1e-4), 0.55));\n"
               "  }\n"
               "  m *= 1.0 - water;\n"
               // light spilling out of the glass: three rings of taps (10, 26, 56 px at 720p) average the key; the ground
               // there is multiplied up so it keeps its own colours (a light pool, not fog)
               "  float spill = 0.0;\n"
               "  for (int r = 0; r < 3; r++) {\n"
               "    float rad = (r == 0 ? 10.0 : r == 1 ? 26.0 : 56.0) * pc.b.w, wt = r == 0 ? 0.45 : r == 1 ? 0.35 : 0.2;\n"
               "    for (int i = 0; i < 8; i++) {\n"
               "      float a = (float(i) + 0.5 * float(r)) * 0.7854;\n"
               "      spill += wt * 0.125 * key(at(p + ivec2(vec2(cos(a), sin(a)) * rad)));\n"
               "    }\n"
               "  }\n"
               "  spill = clamp(spill * 3.0, 0.0, 1.0);\n"
               "  vec3 lit = pc.a.rgb * (1.0 + pc.a.w * 3.2 * spill * vec3(1.0, 0.72, 0.38));\n"
               "  vec3 glow = mix(vec3(1.0), pc.b.rgb, pc.a.w);\n"
               "  o = vec4(mix(lit, glow, m), 1.0);\n"
               "}\n";
    }
    // Day/night dynamic lights (exec.c ssao_run): the lit pixels of the scene (what the tint will leave brighter than night: c * tint above
    // a threshold) become a light map at 1/4 size, a blur pyramid of it, and an apply pass that lights ground and objects near them.
    // dl_extract: a.rgb = tint, b.x = threshold. Out rgb = light colour (the pixel's hue, half toward warm) x intensity, averaged over 4x4.
    if (!strcmp(name, "dl_extract.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"  // a = (A, B, depth mode, -), b.xy = the scene's size
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               // (a.w = the block: 4, fewer in frame generation's reduced in-between images - the map then covers the same scene)
               "  int B = clamp(int(pc.a.w + 0.5), 1, 4);\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy) * B, sz = textureSize(t, 0), dsz = textureSize(dep, 0);\n"
               "  vec3 col = vec3(0.0);\n"
               "  float bi = 0.0, bz = 0.0;\n"
               "  for (int y = 0; y < B; y++)\n"
               "    for (int x = 0; x < B; x++) {\n"
               "      ivec2 q = min(p + ivec2(x, y), sz - 1);\n"
               "      vec3 e = texelFetch(t, q, 0).rgb;\n"
               "      float I = clamp(log2(max(dot(e, vec3(0.2126, 0.7152, 0.0722)), 1e-3) / 1.5) * 0.8, 0.0, 1.2);\n"
               "      if (e.b - max(e.r * 1.6, e.g * 1.1) > 0.12) I = 0.0;\n"  // (water foam and glints are not lights)
               "      if (e.b > e.r * 1.25) I = 0.0;\n"  // (nor cool glass: the Center's cyan door lit itself in a ragged patch; lamps and windows are warm)
               "      if (I > bi) {\n"
               "        bi = I;\n"
               "        col = mix(e / max(max(e.r, max(e.g, e.b)), 1e-3), vec3(1.0, 0.72, 0.38), 0.5) * I;\n"
               "        float d = texelFetch(dep, clamp(ivec2((vec2(q) + 0.5) / pc.b.xy * vec2(dsz)), ivec2(0), dsz - 1), 0).r;\n"
               "        bz = pc.a.y / ((pc.a.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.x);\n"
               "        if (isnan(bz) || isinf(bz) || bz <= 0.0) bi = 0.0, col = vec3(0.0);\n"  // (no NaN / inf into the map: the game's bloom spreads one over the whole screen)
               "      }\n"
               "    }\n"
               "  o = vec4(col, bi > 0.0 ? bz : 0.0);\n"  // rgb = colour x intensity; a = the source's view depth (0: none) - read back by nearest texel
               "}\n";
    }
    // dl_down: the next (half-size) pyramid level, a 4x4 box of the one above: colour averaged, depth (alpha) weighted by each
    // source's intensity (a texel without a source has alpha 0 and no weight).
    if (!strcmp(name, "dl_down.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 n = textureSize(t, 0), p = ivec2(gl_FragCoord.xy) * 2 - 1;\n"
               "  vec3 c = vec3(0.0);\n"
               "  float zs = 0.0, ws = 0.0;\n"
               "  for (int y = 0; y < 4; y++)\n"
               "    for (int x = 0; x < 4; x++) {\n"
               "      vec4 v = texelFetch(t, clamp(p + ivec2(x, y), ivec2(0), n - 1), 0);\n"
               "      float i = v.a > 0.0 ? max(v.r, max(v.g, v.b)) : 0.0;\n"
               "      c += v.rgb; zs += v.a * i; ws += i;\n"
               "    }\n"
               "  o = vec4(c / 16.0, ws > 0.0 ? zs / ws : 0.0);\n"
               "}\n";
    }
    // dl_apply: the scene copy (0), the depth (1), the light pyramid (2..5). a.rgb = tint, a.w = night; b = (A, B, depth mode, y sign);
    // c = (P00, P11, width, height); d.x = the threshold. Output multiplies the scene (blend 1): the tint, and for what is not itself lit,
    // (1 + the light that reaches it): a pool on the ground below each source (the map looked up where the source is above the pixel:
    // a lamp 230 cm up) and a halo around it, so the ground keeps its own colours.
    if (!strcmp(name, "dl_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 2) uniform sampler2D l0;\n"
               "layout(set = 0, binding = 3) uniform sampler2D l1;\n"
               "layout(set = 0, binding = 4) uniform sampler2D l2;\n"
               "layout(set = 0, binding = 5) uniform sampler2D l3;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; vec4 f; } pc;\n"  // f = (contrast mid, streaks, far lights, -)
               "layout(location = 0) out vec4 o;\n"
               // (dl_apply_rt.frag, the ray-traced variant: the scene's triangles in view space, rt.c; the pool's sources' position, weighted)
               "#ifdef RT\n"
               "layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;\n"
               "bool g_acc = false;\n"
               "vec3 g_lp = vec3(0.0);\n"
               "float g_lw = 0.0;\n"
               "#endif\n"
               "vec4 lvl(int k, vec2 uv) {\n"
               "  return k == 0 ? texture(l0, uv) : k == 1 ? texture(l1, uv) : k == 2 ? texture(l2, uv) : texture(l3, uv);\n"
               "}\n"
               // a light-map sample's colour, kept only where its source is about as far away as the pixel (the pool must not run down the
               // screen below a lamp, nor halos leak across depth): rgb = colour x intensity, a = intensity x depth
               // Ray-traced shadows, in screen space: march 7 points from the pixel to the light through the depth; a point behind the scene's
               // surface (by 20..450 cm: a wall, a tree, the lamp post) blocks the light. d.w = 1 turns it on.
               "float lin(float d) { return pc.b.y / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.b.x); }\n"
               "float vis(vec2 uvp, float zp, vec2 uvs, float zs, float jit) {\n"
               "  vec2 k = vec2(2.0 / pc.c.x, 2.0 / pc.c.y);\n"
               "  vec3 A = vec3((uvp - 0.5) * k * zp, zp), B = vec3((uvs - 0.5) * k * zs, zs);\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  float v = 1.0;\n"
               "  for (int i = 0; i < 7; i++) {\n"
               "    vec3 Q = mix(A, B, (float(i) + 1.0 + fract(jit * 7.0)) / 8.5);\n"
               "    vec2 q = Q.xy / (Q.z * k) + 0.5;\n"
               "    if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) continue;\n"
               "    float zq = lin(texelFetch(dep, clamp(ivec2(q * vec2(dsz)), ivec2(0), dsz - 1), 0).r), gap = Q.z - zq;\n"
               "    if (gap > 20.0 && gap < 450.0) v = min(v, 0.15);\n"
               "  }\n"
               "  return v;\n"
               "}\n"
               "vec3 near(vec4 s, float z, float width) {\n"
               "  float q = (s.a - z) / width;\n"
               "  return s.a > 0.0 ? s.rgb * exp(-q * q) : vec3(0.0);\n"
               "}\n"
               "vec4 lm(vec2 uv) { ivec2 n = textureSize(l0, 0); return texelFetch(l0, clamp(ivec2(uv * vec2(n)), ivec2(0), n - 1), 0); }\n"
               "vec4 lf(int k, ivec2 i) {\n"
               "  if (k == 0) return texelFetch(l0, clamp(i, ivec2(0), textureSize(l0, 0) - 1), 0);\n"
               "  if (k == 1) return texelFetch(l1, clamp(i, ivec2(0), textureSize(l1, 0) - 1), 0);\n"
               "  if (k == 2) return texelFetch(l2, clamp(i, ivec2(0), textureSize(l2, 0) - 1), 0);\n"
               "  return texelFetch(l3, clamp(i, ivec2(0), textureSize(l3, 0) - 1), 0);\n"
               "}\n"
               // pyramid level k, bilinear, each of the 4 texels kept by its own source depth first (smooth, never mixes depths)
               "vec3 lvb(int k, vec2 uv, float z, float width) {\n"
               "  ivec2 n = k == 0 ? textureSize(l0, 0) : k == 1 ? textureSize(l1, 0) : k == 2 ? textureSize(l2, 0) : textureSize(l3, 0);\n"
               "  vec2 f = uv * vec2(n) - 0.5, w = fract(f);\n"
               "  ivec2 i = ivec2(floor(f));\n"
               "  vec3 c = vec3(0.0);\n"
               "  for (int j = 0; j < 4; j++) {\n"
               "    ivec2 o = ivec2(j & 1, j >> 1);\n"
               "    vec4 sj = lf(k, i + o);\n"
               "    vec3 cj = near(sj, z, width) * ((o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y));\n"
               "    c += cj;\n"
               "#ifdef RT\n"  // (the source's view-space point: its texel's place at its own depth)
               "    float qj = max(cj.r, max(cj.g, cj.b));\n"
               "    if (g_acc && qj > 0.0) {\n"
               "      vec2 ut = (vec2(i + o) + 0.5) / vec2(n);\n"
               "      g_lp += qj * vec3((ut - 0.5) * vec2(2.0 / pc.c.x, 2.0 / pc.c.y) * sj.a, sj.a);\n"
               "      g_lw += qj;\n"
               "    }\n"
               "#endif\n"
               "  }\n"
               "  return c;\n"
               "}\n"
               "vec3 lmb(vec2 uv, float z) { return lvb(0, uv, z, 350.0 * pc.d.z); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 e = texelFetch(t, p, 0).rgb;\n"
               "  vec3 m = pc.a.rgb;\n"
               "  float lit = clamp(log2(max(dot(e, vec3(0.2126, 0.7152, 0.0722)), 1e-3) / 1.5) * 0.8, 0.0, 2.0);\n"
               "  float d = texelFetch(dep, clamp(ivec2((vec2(p) + 0.5) / pc.c.zw * vec2(textureSize(dep, 0))), ivec2(0), textureSize(dep, 0) - 1), 0).r;\n"
               "  float zp = d < 0.999999 ? pc.b.y / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.b.x) : 1e6;\n"
               // far lights (as DQ HD-2D's r.LightMaxDrawDistanceScale 2.5): the depth window widens with distance, so the pyramid's
               // averaged depths still match far sources instead of dropping them
               "  float zwf = mix(1.0, pc.f.z, smoothstep(1500.0, 6000.0, zp));\n"
               // (what is lit itself takes less of the others' light, fading out by lit 0.4: a hard cut at 0.02 split the Center's glass
               //  door, near that brightness, into texels with and without light - a stepped edge that crawled when walking)
               "  float rcv = 1.0 - smoothstep(0.0, 0.4, lit);\n"
               // (nor bright glass: cool and light - the Center's door took the sign's pool as a matte wall would, washed out in blotches
               //  that moved with the quarter-size map as the camera did; dark blue walls and roofs keep theirs)
               //  (by ratios: the scene is linear HDR, the door's glass ~0.18 luminance - an absolute 0.12..0.35 caught a fifth of it)
               "  float cool = (e.b - e.r) / max(e.b, 1e-3);\n"
               "  rcv *= 1.0 - smoothstep(0.45, 0.7, cool) * smoothstep(0.02, 0.06, dot(e, vec3(0.2126, 0.7152, 0.0722)));\n"
               "#ifdef RT\n"  // (LARECOMP_RT_DEBUG: a ray from the camera through the pixel - green where it hits at the depth buffer's distance, red off, blue none)
               "  if (pc.d.w < -1.5 && d < 0.999999) {\n"
               "    vec3 A = vec3(((vec2(p) + 0.5) / pc.c.zw - 0.5) * vec2(2.0 / pc.c.x, 2.0 / pc.c.y) * zp, zp);\n"
               "    rayQueryEXT rq;\n"
               "    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, vec3(0.0), 1.0, normalize(A), 1e6);\n"
               "    while (rayQueryProceedEXT(rq)) {}\n"
               "    bool hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;\n"
               "    float err = hit ? abs(rayQueryGetIntersectionTEXT(rq, true) - length(A)) : 0.0;\n"
               "    o = vec4(hit ? vec3(clamp(err / 50.0, 0.0, 1.0), 1.0 - clamp(err / 50.0, 0.0, 1.0), 0.0) : vec3(0.0, 0.0, 1.0), 1.0);\n"
               "    return;\n"
               "  }\n"
               "#endif\n"
               "  if (rcv > 0.0 && d < 0.999999) {\n"
               "    float z = zp;\n"
               "    vec2 uv = (vec2(p) + 0.5) / pc.c.zw;\n"
               "    float rpx = 380.0 * pc.c.x / z * 0.5 * pc.c.z / pc.f.w;\n"  // (f.w = the image's scale: in full-size pixels, the map's texel is 4 of them)
               "    float lod = clamp(log2(max(rpx, 6.0) / 6.0), 0.0, 2.99);\n"
               "    vec2 us = uv - vec2(0.0, 230.0 * pc.c.y / z * 0.5 * pc.b.w);\n"
               // the pool: the pyramid level whose texel is about the light's radius (two levels blended), each texel kept by its own
               // source depth: smooth, no noise (the old 32 random taps gave a salt-and-pepper grain on walls and ground)
               "    float rr = min(rpx, 0.4 * pc.c.z / pc.f.w);\n"
               "    float a0 = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
               "    float kl = clamp(log2(max(rr, 2.0) / 2.0), 0.0, 1.999);\n"
               "    int k0 = int(floor(kl));\n"
               // the source looked up 0..150 cm above the pixel (mostly around it: no beams down the screen), nearer weighing more: the spill fades away from its source instead of
               // copying a whole lit facade onto the ground as one flat block
               "    vec3 pool = vec3(0.0);\n"
               "#ifdef RT\n"
               "    g_acc = true;\n"
               "#endif\n"
               "    for (int j = 0; j < 4; j++) {\n"
               "      float hh = float[4](0.0, 40.0, 90.0, 150.0)[j], ww = float[4](0.45, 0.28, 0.17, 0.10)[j];\n"
               "      vec2 uj = uv - vec2(0.0, hh * pc.c.y / z * 0.5 * pc.b.w);\n"
               "      pool += ww * mix(lvb(k0, uj, z, 380.0 * pc.d.z * zwf), lvb(k0 + 1, uj, z, 380.0 * pc.d.z * zwf), fract(kl));\n"
               "    }\n"
               "    pool *= 0.35;\n"
               // ray-traced shadow: one ray from the pixel toward the weighted point of the sources lighting its pool, through the scene's
               // triangles (rt.c: captured this frame by transform feedback, in view space). It stops 50 cm short of the source (the
               // lamp's own head and glass); something in between leaves the pool at 12 %.
               "#ifdef RT\n"
               "    g_acc = false;\n"
               "    if (g_lw > 1e-4) {\n"
               "      vec3 B = g_lp / g_lw, A = vec3((uv - 0.5) * vec2(2.0 / pc.c.x, 2.0 / pc.c.y) * z, z), D = B - A;\n"
               "      float L = length(D);\n"
               "      if (L > 70.0) {\n"
               "        D /= L;\n"
               "        float bias = 3.0 + z * 0.004;\n"  // (off the pixel's own surface: depth precision grows with distance)
               // (and toward the camera, which sees the surface: a ray skimming along a roof toward its own windows hit it in blotches)
               "        vec3 O = A * (1.0 - 0.015) + D * bias;\n"
               // (4 rays toward points 60 cm around the source, turned per pixel: a soft edge instead of one ray's hard line)
               "        vec3 T = normalize(cross(D, abs(D.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), U = cross(D, T);\n"
               "        float vis = 0.0;\n"
               "        for (int r = 0; r < 4; r++) {\n"
               "          float an = a0 + float(r) * 1.5707963 + fract(pc.d.x * 0.618) * 6.2831853;\n"  // (turned each frame: TAA averages the 4 rays' pattern away)
               "          vec3 Br = B + 60.0 * (cos(an) * T + sin(an) * U), Dr = Br - O;\n"
               "          rayQueryEXT rq;\n"
               "          rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, O, 0.0, normalize(Dr), max(length(Dr) - 50.0, 0.0));\n"
               "          while (rayQueryProceedEXT(rq)) {}\n"
               "          vis += rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 0.12 : 1.0;\n"
               "        }\n"
               "        pool *= vis * 0.25;\n"
               "      }\n"
               "    }\n"
               "#endif\n"
               // the halo: pyramid level 1, bilinear (no random taps: they grain the wall)
               "    vec3 halo = (lvb(2, uv, z, 350.0 * pc.d.z * zwf) + lvb(3, uv, z, 350.0 * pc.d.z * zwf)) * 0.6;\n"
               "    m = pc.a.rgb * (1.0 + rcv * pc.a.w * pc.d.y * (24.0 * pool + 6.0 * halo));\n"
               // research (LARECOMP_DL_DEBUG=1): blue where the sample above the pixel has a source, green = its depth minus the pixel's / 2000
               "    if (pc.d.w < -0.5) {\n"
               "      vec4 q = lm(us);\n"
               "      float Iq = max(q.r, max(q.g, q.b)), zq = q.a;\n"
               "      m = Iq > 0.05 ? vec3(clamp(z / 4000.0, 0.0, 1.0), clamp(zq / 4000.0, 0.0, 1.0), 1.0) : vec3(0.1);\n"
               "    }\n"
               "  }\n"
               // as in ssrecomp's night: a vignette (edges 20% darker), and fireflies (soft yellow-green dots drifting over grass and
               // trees, 3 layers of cells). The output multiplies the scene, so an added light f is m += f / scene.
               // night contrast (as DQ HD-2D's r.Color.Mid 0.7): the dark and middle tones of the lit result down to f.x, the bright ones
               // (pools, windows, lamps) kept - the lights stand out more
               "  float Lt = dot(e * m, vec3(0.2126, 0.7152, 0.0722));\n"
               "  m *= mix(1.0, mix(pc.f.x, 1.0, smoothstep(0.12, 0.7, Lt)), pc.a.w);\n"
               // cross streaks (as its r.Bloom.Cross): 4 soft rays from each source, the light map's level 1 read along the row and the
               // column (a lens effect: no depth test), added light (m += f / scene)
               "  if (pc.f.y > 0.0) {\n"
               "    vec2 uv0 = (vec2(p) + 0.5) / pc.c.zw, st = vec2(7.0 * pc.c.w / 720.0) / pc.c.zw;\n"
               "    vec3 sk = vec3(0.0);\n"
               // (level 0, only its strong texels: thin rays from lamps and window centres; level 1 smeared whole windows into boxes)
               "    for (int i = 1; i <= 10; i++) {\n"
               "      float w = exp(-float(i) * 0.3);\n"
               "      vec2 ox = vec2(st.x * float(i), 0.0), oy = vec2(0.0, st.y * float(i));\n"
               "      vec3 s4 = max(texture(l0, uv0 + ox).rgb - 0.6, 0.0) + max(texture(l0, uv0 - ox).rgb - 0.6, 0.0) + max(texture(l0, uv0 + oy).rgb - 0.6, 0.0) + max(texture(l0, uv0 - oy).rgb - 0.6, 0.0);\n"
               "      sk += w * s4;\n"
               "    }\n"
               "    m += pc.a.w * pc.f.y * min(0.1 * sk * vec3(1.0, 0.85, 0.65) / max(e, vec3(0.06)), vec3(6.0));\n"
               "  }\n"
               "  vec2 ex = (vec2(p) + 0.5) / pc.c.zw * 2.0 - 1.0;\n"
               "  m *= mix(1.0, 1.0 - 0.2 * smoothstep(0.6, 1.3, sqrt(ex.x * ex.x * 0.8 + ex.y * ex.y)), pc.a.w);\n"
               // (the fireflies: their own pass, dl_ff.frag, in the world; the moon on water: inside the water's own shader, ShaderKey.glowck 2)
               "  if (any(isnan(m)) || any(isinf(m))) m = pc.a.rgb;\n"  // (one bad pixel -> the game's bloom -> a black frame)
               "  o = vec4(clamp(m, 0.0, 64.0), 1.0);\n"
               "}\n";
    }
    // dl_ff: fireflies in the world. The pixel's surface point (depth -> inverse view-projection) picks 1.5 m cells around it; a cell
    // has a fly (35%) hovering at a hashed point, drifting; drawn where it projects (pc.vp: this frame's camera), over grass only,
    // not behind the surface. Scene copy (0), depth (1); a = (width, height, seconds, depth mode), b = (y flip, night, -, -),
    // c.rgb = the tint (the output multiplies the tinted scene: the fly's light is divided by it).
    if (!strcmp(name, "dl_ff.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { layout(row_major) mat4 vp; vec4 a; vec4 b; vec4 c; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec2 scr(vec4 cl) { vec2 n = cl.xy / cl.w; return vec2(n.x * 0.5 + 0.5, pc.b.x > 0.5 ? 0.5 - n.y * 0.5 : n.y * 0.5 + 0.5) * pc.a.xy; }\n"
               // the surface point under a pixel (false: the sky)
               "mat4 g_iv;\n"
               "vec2 g_fc;\n"  // (this pixel in the full-size image - at half size, b.w = 2: its 2 x 2 block)  // (the inverse once per pixel, in main: at() runs up to 5 times)
               "bool at(vec2 fc, out vec3 P) {\n"
               "  vec2 uv = fc / pc.a.xy;\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  float d = texelFetch(dep, clamp(ivec2(uv * vec2(dsz)), ivec2(0), dsz - 1), 0).r;\n"
               "  P = vec3(0.0);\n"
               "  if (d >= 0.999999) return false;\n"
               "  vec4 wp = g_iv * vec4(uv.x * 2.0 - 1.0, pc.b.x > 0.5 ? 1.0 - uv.y * 2.0 : uv.y * 2.0 - 1.0, pc.a.w > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  if (abs(wp.w) < 1e-6) return false;\n"
               "  P = wp.xyz / wp.w;\n"
               "  return true;\n"
               "}\n"
               // one cell's fly, if it has one, as it lights this pixel
               "vec3 fly(vec3 c, float zp, float s, float T) {\n"
               "  vec4 h = fract(sin(vec4(dot(c, vec3(127.1, 311.7, 74.7)), dot(c, vec3(269.5, 183.3, 246.1)), dot(c, vec3(113.5, 271.9, 124.6)), dot(c, vec3(419.2, 371.9, 168.2)))) * 43758.5453);\n"
               "  if (h.w > 0.35) return vec3(0.0);\n"
               "  vec3 F = (c + 0.2 + 0.6 * h.xyz) * 150.0 + 30.0 * vec3(sin(T * (0.25 + h.x * 0.3) + h.y * 6.28), 0.5 * sin(T * (0.4 + h.z * 0.3) + h.x * 3.0), cos(T * (0.2 + h.y * 0.3) + h.x * 6.28));\n"
               "  vec4 cl = pc.vp * vec4(F, 1.0);\n"
               "  if (cl.w < 50.0 || cl.w > zp + 40.0) return vec3(0.0);\n"  // (behind the camera, or behind the surface here)
               "  vec2 S = scr(cl);\n"
               "  float r = clamp(1800.0 * s / cl.w, 1.0, 5.0 * s), dd = length(g_fc - S) / r;\n"
               "  if (dd > 5.0) return vec3(0.0);\n"
               "  float blink = pow(max(0.0, sin(T * (0.9 + h.z * 1.5) + h.x * 40.0)), 3.0);\n"
               "  if (pc.b.z > 0.0) blink = 1.0, dd /= 3.0;\n"  // (LARECOMP_FF_DEBUG: every fly lit, 3x)
               "  vec3 g = texelFetch(t, clamp(ivec2(S), ivec2(0), ivec2(pc.a.xy) - 1), 0).rgb;\n"
               "  float green = pc.b.z > 0.0 ? 1.0 : smoothstep(0.0, 0.08, g.g - max(g.r, g.b) * 1.05);\n"  // only over grass / leaves
               "  return vec3(0.9, 1.0, 0.35) * 3.0 * blink * green * (exp(-dd * dd) + 0.12 * exp(-dd * dd * 0.12));\n"
               "}\n"
               "void main() {\n"
               "  o = pc.b.w > 0.5 ? vec4(0.0) : vec4(1.0);\n"
               "  g_fc = pc.b.w > 0.5 ? gl_FragCoord.xy * pc.b.w : gl_FragCoord.xy;\n"
               "  vec3 P;\n"
               "  g_iv = inverse(pc.vp);\n"
               "  if (!at(g_fc, P)) return;\n"
               "  float zp = (pc.vp * vec4(P, 1.0)).w, s = pc.a.y / 720.0, T = pc.a.z;\n"
               "  vec3 add = vec3(0.0), base[9];\n"
               // (all 27 cells around the point: the top-down camera sees a fly hovering a metre up over ground well behind it - the 8
               //  nearest cut flies into half-moons)
               "  base[0] = floor(P / 150.0);\n"
               "  for (int j = 0; j < 27; j++) add += fly(base[0] + vec3(j % 3 - 1, (j / 3) % 3 - 1, j / 9 - 1), zp, s, T);\n"
               // at a depth edge, the cells around the neighbours' surfaces too (12 and 24 px away: a fly's glow reaches 5 radii, up to
               // 25 px): a fly hovering at a roof or a tree's edge was cut off where the pixels beyond it see the far ground; cells
               // already counted are skipped
               "  int nb = 1;\n"
               "  for (int k = 0; k < 8; k++) {\n"
               "    vec3 Q;\n"
               "    vec2 fq = g_fc + vec2[4](vec2(12.0, 0.0), vec2(-12.0, 0.0), vec2(0.0, 12.0), vec2(0.0, -12.0))[k & 3] * float(1 + (k >> 2)) * s;\n"
               // (only at a depth edge - the neighbour 10 % nearer or farther; 1 - depth goes as 1 / distance: 1.4 -> 0.4 ms)
               "    ivec2 dz = textureSize(dep, 0);\n"
               "    float d0 = texelFetch(dep, clamp(ivec2(g_fc / pc.a.xy * vec2(dz)), ivec2(0), dz - 1), 0).r;\n"
               "    float d1 = texelFetch(dep, clamp(ivec2(fq / pc.a.xy * vec2(dz)), ivec2(0), dz - 1), 0).r;\n"
               "    if (abs(d1 - d0) < 0.1 * (1.0 - d0)) continue;\n"
               "    if (!at(fq, Q)) continue;\n"
               "    vec3 bq = floor(Q / 150.0);\n"
               "    bool seen = false;\n"
               "    for (int i = 0; i < nb; i++) seen = seen || all(lessThanEqual(abs(bq - base[i]), vec3(0.5)));\n"
               "    if (seen) continue;\n"
               "    for (int j = 0; j < 27; j++) {\n"
               "      vec3 c = bq + vec3(j % 3 - 1, (j / 3) % 3 - 1, j / 9 - 1);\n"
               "      bool dup = false;\n"
               "      for (int i = 0; i < nb; i++) dup = dup || all(lessThanEqual(abs(c - base[i]), vec3(1.5)));\n"
               "      if (!dup) add += fly(c, zp, s, T);\n"
               "    }\n"
               "    base[nb++] = bq;\n"
               "  }\n"
               "  if (pc.b.w > 0.5) { o = vec4(add, 1.0); return; }\n"  // (half size: the light alone - ff_apply.frag puts it on)
               "  vec3 e = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "  vec3 m = 1.0 + pc.b.y * min(add / max(e * pc.c.rgb, vec3(0.03)), vec3(16.0));\n"
               "  if (any(isnan(m)) || any(isinf(m))) m = vec3(1.0);\n"
               "  o = vec4(m, 1.0);\n"
               "}\n";
    }
    // clouds: their shadows drifting over the ground. The pixel's surface point (as dl_ff) -> its point on the ground along the sun ->
    // two layers of warped fractal gradient noise scrolling with the wind; multiplied onto the scene. Depth (0); a = (width, height, seconds, depth mode),
    // b = (y flip, strength, -, -).
    // ff_apply: the fireflies' light made at half size (dl_ff.frag, b.w = 2), bilinear, onto the scene as dl_ff's own multiplier
    // (exec.c dn_run: 2.3 ms at full size, 1080p). Scene copy (0), the light (1); a = (tint rgb, night).
    if (!strcmp(name, "ff_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D ff;\n"
               "layout(push_constant) uniform P { vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 add = texture(ff, gl_FragCoord.xy / vec2(textureSize(t, 0))).rgb;\n"
               "  vec3 e = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "  vec3 m = 1.0 + pc.a.w * min(add / max(e * pc.a.rgb, vec3(0.03)), vec3(16.0));\n"
               "  if (any(isnan(m)) || any(isinf(m))) m = vec3(1.0);\n"
               "  o = vec4(m, 1.0);\n"
               "}\n";
    }
    // clouds_map: the cloud shadow over a 192 m square of the clouds' plane (768 x 768, 25 cm a texel; exec.c dn_run), which
    // clouds.frag samples per pixel - the noise per screen pixel was 0.48 ms at 1080p, twice with frame generation. a = (the
    // square's corner x, z in cm, seconds, coverage).
    if (!strcmp(name, "clouds_map.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               // (an integer hash: the sin() one lost its precision on the octaves' large cell numbers - straight-edged patches)
               "float h2(vec2 c) { uvec2 q = uvec2(ivec2(c) + 32768); uint h = q.x * 1597334677u ^ q.y * 3812015801u;\n"
               "  h = (h ^ (h >> 16)) * 2246822519u; h = (h ^ (h >> 13)) * 3266489917u; h ^= h >> 16; return float(h) * (1.0 / 4294967296.0); }\n"
               "float vn(vec2 x) { vec2 i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f);\n"
               "  return mix(mix(h2(i), h2(i + vec2(1, 0)), f.x), mix(h2(i + vec2(0, 1)), h2(i + vec2(1, 1)), f.x), f.y); }\n"
               // gradient noise (quintic fade: round level curves - value noise's were faceted, straight-edged shadows), 0..1
               "vec2 gr(vec2 c) { float a = 6.2831853 * h2(c); return vec2(cos(a), sin(a)); }\n"
               "float gn(vec2 x) { vec2 i = floor(x), f = fract(x), u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);\n"
               "  return 0.5 + 0.7 * mix(mix(dot(gr(i), f), dot(gr(i + vec2(1, 0)), f - vec2(1, 0)), u.x),\n"
               "                         mix(dot(gr(i + vec2(0, 1)), f - vec2(0, 1)), dot(gr(i + vec2(1, 1)), f - vec2(1, 1)), u.x), u.y); }\n"
               // (5 octaves, each turned 37 degrees, the first too: no grid direction lines up)
               "float fbm(vec2 x, int n) {\n"
               "  float s = 0.0, a = 0.5;\n"
               "  mat2 r = mat2(0.8, 0.6, -0.6, 0.8);\n"
               "  for (int i = 0; i < n; i++) { x = r * x; s += a * gn(x); x = x * 2.03 + 11.7; a *= 0.5; }\n"
               "  return s / (1.0 - exp2(-float(n)));\n"
               "}\n"
               "void main() {\n"
               "  vec2 Q = pc.a.xy + gl_FragCoord.xy * 25.0;\n"
               // two layers, as EntroPi's Cloud Shadows: big slow clouds, their outline warped by a second noise (rounded, billowy), and
               // smaller wisps drifting faster another way at 45 %; each by coverage and softness (its smoothstep), composited as
               // "multiply inverse": lit = (1 - a) x (1 - 0.45 b)
               "  float T = pc.a.z;\n"
               "  vec2 g = Q / 1600.0 + T * vec2(0.022, 0.009);\n"
               "  vec2 w = vec2(fbm(g * 0.7 + 3.1, 2), fbm(g * 0.7 + 7.7, 2));\n"
               "  float n1 = clamp((fbm(g + 3.0 * (w - 0.5), 4) - 0.5) * 2.4 + 0.5, 0.0, 1.0);\n"
               "  vec2 g2 = mat2(0.9, 0.44, -0.44, 0.9) * Q / 650.0 + T * vec2(0.05, -0.03);\n"
               "  float n2 = clamp((fbm(g2 + 5.3, 3) - 0.5) * 2.4 + 0.5, 0.0, 1.0);\n"
               "  float cov = pc.a.w, s1 = 0.12, s2 = 0.3;\n"
               "  float st1 = -s1 + (1.0 - cov) * (1.0 + 2.0 * s1), st2 = -s2 + (1.0 - 0.8 * cov) * (1.0 + 2.0 * s2);\n"
               "  float a1 = smoothstep(st1 - s1, st1 + s1, n1), a2 = smoothstep(st2 - s2, st2 + s2, n2);\n"
               "  o = vec4(1.0 - (1.0 - a1) * (1.0 - 0.45 * a2), 0.0, 0.0, 1.0);\n"
               "}\n";
    }
    // clouds: the cloud shadows drifting over the ground (exec.c dn_run), multiplied onto the scene: each pixel's point in the world
    // (cm), along the sun to the clouds' plane, looked up in clouds_map. Depth (0), the map (1). c = (sun slope x, z, -, the map's
    // corner z), b.w: its corner x.
    if (!strcmp(name, "clouds.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D map;\n"
               "layout(push_constant) uniform P { layout(row_major) mat4 ivp; vec4 a; vec4 b; vec4 c; vec4 d; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float h2(vec2 c) { uvec2 q = uvec2(ivec2(c) + 32768); uint h = q.x * 1597334677u ^ q.y * 3812015801u;\n"
               "  h = (h ^ (h >> 16)) * 2246822519u; h = (h ^ (h >> 13)) * 3266489917u; h ^= h >> 16; return float(h) * (1.0 / 4294967296.0); }\n"
               "void main() {\n"
               "  o = vec4(1.0);\n"
               "  vec2 uv = gl_FragCoord.xy / pc.a.xy;\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  float d = texelFetch(dep, clamp(ivec2(uv * vec2(dsz)), ivec2(0), dsz - 1), 0).r;\n"
               "  if (d >= 0.999999) return;\n"
               "  vec4 nd = vec4(uv.x * 2.0 - 1.0, pc.b.x > 0.5 ? 1.0 - uv.y * 2.0 : uv.y * 2.0 - 1.0, pc.a.w > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec4 wp = pc.ivp * nd;\n"
               "  if (abs(wp.w) < 1e-6) return;\n"
               "  vec3 P = wp.xyz / wp.w;\n"
               // (cm: clouds ~12 m, drifting ~40 cm/s; none past 30-50 m: the sky dome and the battles' far mountains)
               // (along the sun to the clouds' plane: its slope c.xy = sun.xz / sun.y - a wall's or a roof's point moves with its height)
               "  vec2 m = (P.xz - P.y * pc.c.xy - vec2(pc.b.w, pc.c.w)) / 19200.0;\n"
               "  vec2 e = min(m, 1.0 - m);\n"  // (outside the map: none, faded over its last 5 %)
               "  float sh = texture(map, m).r * clamp(min(e.x, e.y) * 20.0, 0.0, 1.0)\n"
               "           * (1.0 - smoothstep(3000.0, 5000.0, (pc.d.y / ((pc.a.w > 0.5 ? d * 2.0 - 1.0 : d) + pc.d.x))));\n"
               "  o = vec4(vec3(1.0 - pc.b.y * sh), 1.0);\n"
               "  if (pc.b.z > 1.5) { vec2 c = floor(P.xz / 200.0); o = vec4(0.4 + 0.6 * vec3(h2(c), h2(c + 7.0), h2(c + 13.0)), 1.0); return; }\n"  // (=2: 2 m world cells, each its own tint)
               "  if (pc.b.z > 0.0) { vec2 gl = step(fract(P.xz / 100.0), vec2(0.08)); o = vec4(1.0 - 0.8 * max(gl.x, gl.y)); }\n"  // (LARECOMP_CLOUDS_DEBUG: a 1 m world grid, dark lines)
               "}\n";
    }
    // atmo: the atmosphere by hour, after the tint (exec.c dn_run; runtime/daynight.c daynight_atmo). Scene copy (0), depth (1).
    // The sky (no depth, or past 200 m: the game's sky dome) is multiplied from its horizon colour to its zenith's by the view's
    // elevation; at night stars (a hashed field fixed in the world's directions, twinkling) and a moon. The rest takes the
    // distance haze. a = (width, height, seconds, depth mode), b = (horizon, stars), c = (zenith, haze density /cm), d = (haze, -).
    if (!strcmp(name, "atmo.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { layout(row_major) mat4 vp; vec4 a; vec4 b; vec4 c; vec4 d; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float h3(vec3 c) { uvec3 q = uvec3(ivec3(c) + 32768); uint h = q.x * 1597334677u ^ q.y * 3812015801u ^ q.z * 2654435761u;\n"
               "  h = (h ^ (h >> 16)) * 2246822519u; h = (h ^ (h >> 13)) * 3266489917u; h ^= h >> 16; return float(h) * (1.0 / 4294967296.0); }\n"
               "vec3 unproj(vec2 uv, float z) { vec4 w = pc.vp * vec4(uv * 2.0 - 1.0, z, 1.0); return w.xyz / w.w; }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 col = texelFetch(t, p, 0).rgb;\n"
               "  vec2 uv = (vec2(p) + 0.5) / pc.a.xy;\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  float d = texelFetch(dep, clamp(ivec2(uv * vec2(dsz)), ivec2(0), dsz - 1), 0).r;\n"
               "  float zn = pc.a.w > 0.5 ? -1.0 : 0.0;\n"  // (the near plane in clip z)
               "  vec3 C = unproj(uv, zn), F = unproj(uv, pc.a.w > 0.5 ? 0.999 : 0.9995), dir = normalize(F - C);\n"
               "  float dist = d < 0.999999 ? length(unproj(uv, pc.a.w > 0.5 ? d * 2.0 - 1.0 : d) - C) : 1e9;\n"
               "  if (dist > 7500.0 && dir.y > -0.05) {\n"  // the sky (the game's far plane is 100 m: its dome is inside it)
               "    float el = dir.y;\n"
               "    col *= mix(pc.b.rgb, pc.c.rgb, smoothstep(0.0, 0.55, el));\n"
               "    if (pc.b.w > 0.01) {\n"
               "      vec3 sd = dir * 260.0, cell = floor(sd);\n"
               "      float hs = h3(cell), st = 0.0;\n"
               "      if (hs > 0.9965) {\n"
               "        vec3 f = sd - cell - 0.5;\n"
               "        float tw = 0.65 + 0.35 * sin(pc.a.z * (1.5 + 3.0 * h3(cell + 7.0)) + hs * 60.0);\n"
               "        st = exp(-dot(f, f) * 14.0) * tw * (0.6 + 2.0 * h3(cell + 3.0));\n"
               "      }\n"
               "      vec3 md = normalize(vec3(-0.35, 0.45, 0.82));\n"
               "      float ca = dot(dir, md);\n"
               "      vec3 moon = vec3(0.95, 0.96, 1.0) * 2.2 * smoothstep(0.99935, 0.99955, ca) + vec3(0.3, 0.35, 0.5) * 0.25 * pow(max(ca, 0.0), 300.0);\n"
               "      col += pc.b.w * (vec3(0.8, 0.85, 1.0) * st * smoothstep(0.02, 0.25, el) + moon);\n"
               // a shooting star: in about half of the 6 s slots, at a hashed moment and place, 0.8 s across 20 degrees of sky
               "      float sl = floor(pc.a.z / 6.0), hs2 = h3(vec3(sl, 1.0, 2.0)), tt = (pc.a.z - sl * 6.0 - hs2 * 4.0) / 0.8;\n"
               "      if (hs2 < 0.5 && tt > 0.0 && tt < 1.0) {\n"
               "        float az = 6.2831853 * h3(vec3(sl, 5.0, 7.0)), e0 = 0.35 + 0.35 * h3(vec3(sl, 9.0, 3.0));\n"
               "        float g = 0.0;\n"
               "        for (int k = 0; k < 8; k++) {\n"
               "          float tk = tt - float(k) * 0.03;\n"  // (the tail: the head's last places)
               "          if (tk < 0.0) break;\n"
               "          float a2 = az + 0.35 * tk, e2 = e0 - 0.18 * tk;\n"
               "          vec3 q = vec3(cos(a2) * cos(e2), sin(e2), sin(a2) * cos(e2));\n"
               "          vec3 dq = dir - q;\n"
               "          g = max(g, exp(-dot(dq, dq) * 4e5) * (1.0 - float(k) / 8.0));\n"
               "        }\n"
               "        col += pc.b.w * vec3(0.9, 0.95, 1.0) * 3.0 * g * sin(3.14159 * tt);\n"
               "      }\n"
               "    }\n"
               "  } else {\n"  // the haze: past 6 m, by distance
               "    float f = 1.0 - exp(-max(dist - 600.0, 0.0) * pc.c.w);\n"
               "    col = mix(col, pc.d.rgb, 0.85 * f);\n"
               "  }\n"
               "  if (any(isnan(col)) || any(isinf(col))) col = texelFetch(t, p, 0).rgb;\n"
               "  o = vec4(col, 1.0);\n"
               "}\n";
    }
    // shafts: light shafts (god rays), added (exec.c dn_run). The sky (past 200 m) is the light; 28 samples from the pixel toward the
    // sun's place on the screen (it may be off it), each fading, so trees and roofs in front of the sky break the rays into shafts.
    // Scene copy (0), depth (1); a = (sun uv, strength, depth mode), b = (colour, -), c = (P22, P23, width, height).
    if (!strcmp(name, "shafts.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float skyw(vec2 uv) {\n"  // (the sky's light at uv: its brightness where the depth is far)
               "  float d = texture(dep, uv).r;\n"
               "  float z = d < 0.999999 ? pc.c.y / ((pc.a.w > 0.5 ? d * 2.0 - 1.0 : d) + pc.c.x) : 1e9;\n"
               "  if (z < 20000.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;\n"
               "  return min(dot(texture(t, uv).rgb, vec3(0.2126, 0.7152, 0.0722)), 4.0);\n"
               "}\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy / pc.c.zw, D = (pc.a.xy - uv) / 28.0;\n"
               "  float j = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"  // (per-pixel start: no banding)
               "  float s = 0.0, w = 1.0;\n"
               "  vec2 q = uv + D * j;\n"
               "  for (int i = 0; i < 28; i++) { s += skyw(q) * w; w *= 0.93; q += D; }\n"
               "  float fall = 1.0 - smoothstep(0.2, 1.4, length((uv - pc.a.xy) * vec2(pc.c.z / pc.c.w, 1.0)));\n"
               "  o = vec4(pc.b.rgb * pc.a.z * s / 28.0 * 3.0 * fall, 1.0);\n"
               "}\n";
    }
    // rain (exec.c dn_run; runtime/daynight.c daynight_rain): the scene darker and greyer, and three layers of slanted streaks at
    // 3, 7 and 15 m, each hidden behind what is nearer. Scene copy (0), depth (1); a = (width, height, seconds, rain),
    // b = (P22, P23, depth mode, night).
    if (!strcmp(name, "rain.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; layout(row_major) mat4 vp; vec4 c; } pc;\n"  // (vp: this frame's camera, for the splashes; c = (snow, lightning flash, -, -))
               "layout(location = 0) out vec4 o;\n"
               "float h1(vec2 c);\n"
               // splashes where the drops land: level surfaces (the world's up, from the depth), 35 cm world cells each with a drop
               // landing every ~0.7 s at a hashed point and time - a bright flick, then a ring spreading 14 cm and fading
               "float splash(vec3 P) {\n"
               "  float s = 0.0, T = pc.a.z;\n"
               "  vec2 g = P.xz / 35.0, base = floor(g);\n"
               "  for (int j = 0; j < 9; j++) {\n"
               "    vec2 c = base + vec2(j % 3 - 1, j / 3 - 1);\n"
               "    float ph = h1(c * 1.7 + 3.0), per = 0.55 + 0.35 * h1(c + 11.0);\n"
               "    float tt = fract(T / per + ph);\n"
               "    vec2 at = (c + 0.15 + 0.7 * vec2(h1(c + 5.0), h1(c + 9.0))) * 35.0;\n"
               "    float r = length(P.xz - at), R = 2.0 + 14.0 * sqrt(tt);\n"
               "    float ring = exp(-pow((r - R) / 1.6, 2.0)) * (1.0 - tt) * (1.0 - tt);\n"
               "    float flick = exp(-r * r / 6.0) * (1.0 - smoothstep(0.0, 0.12, tt));\n"
               "    s += ring * 0.8 + flick * 2.0;\n"
               "  }\n"
               "  return s;\n"
               "}\n"
               "float h1(vec2 c) { uvec2 q = uvec2(ivec2(c) + 32768); uint h = q.x * 1597334677u ^ q.y * 3812015801u;\n"
               "  h = (h ^ (h >> 16)) * 2246822519u; h = (h ^ (h >> 13)) * 3266489917u; h ^= h >> 16; return float(h) * (1.0 / 4294967296.0); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 col = texelFetch(t, p, 0).rgb;\n"
               "  float r = pc.a.w, T = pc.a.z;\n"
               "  vec2 uv = (vec2(p) + 0.5) / pc.a.xy;\n"
               "  float d = texture(dep, uv).r;\n"
               "  float z = d < 0.999999 ? pc.b.y / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.b.x) : 1e9;\n"
               "  float L = dot(col, vec3(0.2126, 0.7152, 0.0722));\n"
               "  float sn = pc.c.x, fl = pc.c.y;\n"
               "  col = mix(col, vec3(L), 0.35 * r + 0.2 * sn) * (1.0 - 0.28 * r - 0.1 * sn);\n"
               "  float s = 0.0;\n"
               "  for (int i = 0; i < 3; i++) {\n"
               "    float dist = float[3](300.0, 700.0, 1500.0)[i], sc = float[3](1.0, 1.8, 3.0)[i];\n"
               "    if (z < dist) continue;\n"
               "    vec2 q = vec2((uv.x + uv.y * 0.12) * pc.a.x / pc.a.y * 55.0 * sc, uv.y * 2.2 * sc - T * (3.2 - 0.6 * float(i)));\n"
               "    vec2 c = floor(q);\n"
               "    float hc = h1(c + vec2(float(i) * 97.0, 0.0));\n"
               "    if (hc > 0.3) continue;\n"
               "    float fx = fract(q.x) - (0.2 + 0.6 * h1(c + 13.0)), fy = fract(q.y + hc * 7.0);\n"
               "    s += exp(-fx * fx * 900.0) * smoothstep(0.0, 0.08, fy) * (1.0 - smoothstep(0.25, 0.45, fy)) * (1.0 - 0.25 * float(i));\n"
               "  }\n"
               "  col += vec3(0.6, 0.65, 0.75) * s * 0.22 * r * (1.0 - 0.6 * pc.b.w);\n"
               "  if (d < 0.999999) {\n"
               "    vec4 wp = pc.vp * vec4(uv * 2.0 - 1.0, pc.b.z > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "    vec3 P = wp.xyz / wp.w, N = normalize(cross(dFdx(P), dFdy(P)));\n"
               "    float up = smoothstep(0.8, 0.95, abs(N.y));\n"
               // the splashes: near ground only - too small to read past 25 m; by the scene's light (dimmer at night)
               "    if (r > 0.01 && z < 2500.0) col += vec3(0.75, 0.8, 0.9) * splash(P) * up * (1.0 - smoothstep(1500.0, 2500.0, z)) * r * 0.12 * max(L, 0.08) / 0.3;\n"
               // snow settled on what faces up: toward white at the scene's light, broken up by a world noise so the ground shows through
               "    if (sn > 0.01) {\n"
               // water frozen: the strongly blue level pixels (as ssr.frag finds them) to a pale icy blue at the scene's light
               "      vec3 c0 = texelFetch(t, p, 0).rgb;\n"
               "      float ice = up * smoothstep(0.06, 0.16, c0.b - max(c0.r, c0.g) * 1.15) * sn;\n"
               "      col = mix(col, vec3(0.72, 0.84, 0.95) * max(max(c0.r, max(c0.g, c0.b)) * 0.85, 0.05), 0.8 * ice);\n"  // (by its brightest channel: a blue's luminance is low)
               "      vec2 g = P.xz / 60.0; vec2 i = floor(g), f = fract(g); f = f * f * (3.0 - 2.0 * f);\n"
               "      float nz = mix(mix(h1(i), h1(i + vec2(1, 0)), f.x), mix(h1(i + vec2(0, 1)), h1(i + vec2(1, 1)), f.x), f.y);\n"
               "      float cover = up * smoothstep(0.2, 0.6, nz + 0.35 * sn) * sn;\n"
               "      col = mix(col, vec3(0.9, 0.93, 1.0) * max(L * 1.6, 0.05) + 0.04, 0.75 * cover);\n"
               "    }\n"
               "  }\n"
               // snowflakes: 3 layers of soft dots, at 2, 5 and 11 m, falling slowly and swaying, hidden behind what is nearer
               "  if (sn > 0.01) {\n"
               "    float f = 0.0;\n"
               "    for (int i = 0; i < 3; i++) {\n"
               "      float dist = float[3](200.0, 500.0, 1100.0)[i], sc = float[3](1.0, 1.7, 2.8)[i];\n"
               "      if (z < dist) continue;\n"
               "      vec2 q = vec2(uv.x * pc.a.x / pc.a.y * 14.0 * sc + 0.4 * sin(T * 0.7 + uv.y * 6.0 + float(i)), uv.y * 14.0 * sc - T * (0.9 - 0.2 * float(i)));\n"
               "      vec2 c = floor(q);\n"
               "      if (h1(c + vec2(0.0, float(i) * 53.0)) > 0.45) continue;\n"
               "      vec2 o2 = fract(q) - (0.25 + 0.5 * vec2(h1(c + 3.0), h1(c + 7.0)));\n"
               "      f += exp(-dot(o2, o2) * (260.0 - 60.0 * float(i))) * (1.0 - 0.3 * float(i));\n"
               "    }\n"
               "    col += vec3(0.9, 0.93, 1.0) * f * sn * mix(0.55, 0.2, pc.b.w);\n"
               "  }\n"
               // lightning: the whole scene lit blue-white for a moment (a double flicker, exec.c)
               "  col = col * (1.0 + 2.5 * fl) + vec3(0.25, 0.28, 0.35) * fl;\n"
               "  o = vec4(col, 1.0);\n"
               "}\n";
    }
    // rtao: ray-traced ambient occlusion (exec.c ssao_run; rt.c's acceleration structure, view space): 4 rays per pixel in the
    // hemisphere around its normal (from the depth), 120 cm, turned per pixel and frame; the share that hits darkens it. Multiplied
    // onto the scene. Depth (0), binding 6 the scene. a = (P00, P11, P22, P23), b = (width, height, frame, depth mode), c = (strength, -).
    if (!strcmp(name, "rtao.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 460\n"
               "#extension GL_EXT_ray_query : require\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.w > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "vec3 vpos(vec2 uv) { float z = lin(texture(dep, uv).r); return vec3((uv - 0.5) * vec2(2.0 / pc.a.x, 2.0 / pc.a.y) * z, z); }\n"
               "void main() {\n"
               "  o = vec4(1.0);\n"
               "  vec2 uv = gl_FragCoord.xy / pc.b.xy, px = 1.0 / pc.b.xy;\n"
               "  float d = texture(dep, uv).r;\n"
               "  if (d >= 0.999999) return;\n"
               "  vec3 A = vpos(uv);\n"
               "  if (A.z > 8000.0) return;\n"
               "  vec3 l = vpos(uv - vec2(px.x, 0.0)), r = vpos(uv + vec2(px.x, 0.0)), u = vpos(uv - vec2(0.0, px.y)), w = vpos(uv + vec2(0.0, px.y));\n"
               "  vec3 dx = abs(r.z - A.z) < abs(A.z - l.z) ? r - A : A - l, dy = abs(w.z - A.z) < abs(A.z - u.z) ? w - A : A - u;\n"
               "  vec3 N = normalize(cross(dx, dy));\n"
               "  if (dot(N, A) > 0.0) N = -N;\n"  // (facing the camera)
               "  vec3 T = normalize(cross(N, abs(N.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), B = cross(N, T);\n"
               "  float a0 = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + pc.b.z * 0.618034);\n"
               "  vec3 O = A * (1.0 - 0.01) + N * (2.0 + A.z * 0.002);\n"
               "  float occ = 0.0;\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    float an = a0 + float(i) * 1.5707963, el = 0.35 + 0.15 * float(i & 1);\n"  // (cosine-ish: two rings, 30 and 40 degrees off the plane)
               "    vec3 D = normalize(N * el * 2.0 + (cos(an) * T + sin(an) * B));\n"
               "    rayQueryEXT rq;\n"
               "    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, O, 0.0, D, 120.0);\n"
               "    while (rayQueryProceedEXT(rq)) {}\n"
               "    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)\n"
               "      occ += 1.0 - rayQueryGetIntersectionTEXT(rq, true) / 120.0;\n"  // (nearer occluders darker)
               "  }\n"
               "  o = vec4(vec3(1.0 - pc.c.x * occ * 0.25), 1.0);\n"
               "}\n";
    }
    // ssr: reflections on water (exec.c dn_run). Water = a strongly blue pixel on a level surface (its view-space normal from the
    // depth, against the world's up); the view ray is mirrored about up (rippled) and marched through the depth buffer; a hit takes
    // the scene's colour there, Fresnel-weighted, fading at the screen's edges and with the march's length. Scene copy (0), depth (1);
    // a = (P00, P11, P22, P23), b = (up in view space, depth mode), c = (width, height, seconds, strength).
    if (!strcmp(name, "ssr.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.w > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "vec2 K() { return vec2(2.0 / pc.a.x, 2.0 / pc.a.y); }\n"
               "vec3 vpos(vec2 uv) { float z = lin(texture(dep, uv).r); return vec3((uv - 0.5) * K() * z, z); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 col = texelFetch(t, p, 0).rgb;\n"
               "  o = vec4(col, 1.0);\n"
               "  float wat = max(smoothstep(0.06, 0.16, col.b - max(col.r, col.g) * 1.15) * smoothstep(0.02, 0.06, col.b), 0.35 * pc.d.x);\n"  // (d.x: rain - the level ground wet)
               "  if (wat < 0.01) return;\n"
               "  vec2 uv = (vec2(p) + 0.5) / pc.c.xy, px = 1.0 / pc.c.xy;\n"
               "  vec3 A = vpos(uv), ax = vpos(uv + vec2(px.x, 0.0)) - A, ay = vpos(uv + vec2(0.0, px.y)) - A;\n"
               "  vec3 N = normalize(cross(ax, ay)), up = normalize(pc.b.xyz);\n"
               "  if (abs(dot(N, up)) < 0.85 || A.z > 15000.0) return;\n"  // (level surfaces only, not the far sky)
               "  float T = pc.c.z;\n"
               "  vec3 Nw = normalize(up + 0.035 * vec3(sin(A.x * 0.02 + T * 1.3) + sin(A.z * 0.015 - T), 0.0, cos(A.z * 0.02 + T * 1.1) + sin(A.x * 0.013 + T * 0.7)));\n"
               "  vec3 I = normalize(A), R = reflect(I, Nw);\n"
               "  vec3 Q = A + R * 4.0;\n"
               "  float st = 8.0 + A.z * 0.01, hit = 0.0;\n"
               "  vec2 huv = uv;\n"
               "  for (int i = 0; i < 48; i++) {\n"
               "    Q += R * st; st *= 1.08;\n"
               "    if (Q.z <= 1.0) break;\n"
               "    vec2 q = Q.xy / (Q.z * K()) + 0.5;\n"
               "    if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) break;\n"
               "    float zq = lin(texture(dep, q).r);\n"
               "    if (Q.z > zq + 2.0 && Q.z - zq < 60.0 + st * 2.0) { hit = 1.0; huv = q; break; }\n"
               "  }\n"
               "  if (hit < 0.5) return;\n"
               "  vec2 e = min(huv, 1.0 - huv);\n"
               "  float fade = smoothstep(0.0, 0.08, min(e.x, e.y)) * (1.0 - smoothstep(0.6, 1.0, length(huv - uv) * 1.5));\n"
               "  float fr = 0.3 + 0.7 * pow(1.0 - clamp(dot(-I, Nw), 0.0, 1.0), 5.0);\n"
               "  vec3 rc = texture(t, huv).rgb;\n"
               "  o = vec4(mix(col, rc, clamp(wat * fade * fr * pc.c.w, 0.0, 0.85)), 1.0);\n"
               "}\n";
    }
    // Day/night: a constant colour, multiplied onto the scene by the blend (exec.c ssao_run)
    if (!strcmp(name, "tint_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(pc.a.rgb, 1.0); }\n";
    }
    if (!strcmp(name, "ssao_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float a = texture(t, gl_FragCoord.xy * pc.b.xy).r;\n"
               "  o = vec4(vec3(a * (pc.b.z > 0.0 ? pc.b.z : 1.0)), 1.0);\n"
               "}\n";
    }
    // Colour adjustment (F12 menu: runtime/ui/menu.mm), on the gamma-encoded image. a = (brightness, contrast,
    // saturation): brightness scales, contrast pivots around mid-grey, saturation mixes with the Rec. 709 luma.
    if (!strcmp(name, "color.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb * pc.a.x;\n"
               "  c = (c - 0.5) * pc.a.y + 0.5;\n"
               "  c = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, pc.a.z);\n"
               "  o = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
