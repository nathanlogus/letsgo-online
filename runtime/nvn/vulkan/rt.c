// Ray-traced light shadows (day/night dynamic lights, docs/DAYNIGHT.md). The scene pass's opaque draws run their vertex shaders'
// transform feedback variant (ShaderKey.xfb: the clip position before zoom and jitter, 16 bytes a vertex) into one buffer, each
// draw at its own place; after the pass (exec.c ssao_run) one bottom-level acceleration structure is built from it, with the
// geometry transform that takes the game's clip position to the lights pass's view space (x / P00, y / P11, the distance:
// linear in clip z), and a top-level one holding it. dl_apply_rt.frag traces its shadow rays through it (ray query).
// The in-between images of frame generation capture nothing: they trace N's (half a frame off, in view space).
#include "rt.h"
#include <stdlib.h>
#include <string.h>
#include "vk.h"

#define RT_CAP_VERTS (3u << 20)  // 3 M vertices (48 MB): a town frame's opaque draws are ~0.5-1.5 M triangles' worth

static PFN_vkCmdBindTransformFeedbackBuffersEXT p_bind_xfb;
static PFN_vkCmdBeginTransformFeedbackEXT p_begin_xfb;
static PFN_vkCmdEndTransformFeedbackEXT p_end_xfb;
static PFN_vkCreateAccelerationStructureKHR p_create_as;
static PFN_vkGetAccelerationStructureBuildSizesKHR p_build_sizes;
static PFN_vkCmdBuildAccelerationStructuresKHR p_build_as;
static PFN_vkGetAccelerationStructureDeviceAddressKHR p_as_addr;

typedef struct { VkBuffer buf; VmaAllocation mem; VkDeviceAddress addr; } Buf;
static Buf xfb, xform, inst, blas_mem, tlas_mem, scratch;
static VkAccelerationStructureKHR blas, tlas;
static VkDeviceAddress blas_addr;
static VkDeviceSize scratch_align = 128;
static int state;  // 0 not tried, 1 ready, -1 off
static uint64_t cap_frame = ~0ull;
static uint32_t cap_verts;  // vertices captured in cap_frame
static bool built;          // tlas holds a scene

static Buf make_buf(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf b = {0};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &ci, &ai, &b.buf, &b.mem, NULL));
    VkBufferDeviceAddressInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf};
    b.addr = vkGetBufferDeviceAddress(g_vk.dev, &di);
    return b;
}

static VkAccelerationStructureKHR make_as(Buf *mem, VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
    *mem = make_buf(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                               .buffer = mem->buf, .size = size, .type = type};
    VkAccelerationStructureKHR as;
    VK_CHECK(p_create_as(g_vk.dev, &ci, NULL, &as));
    return as;
}

// The BLAS's geometry for n vertices of the capture buffer (a triangle list, transformed by xform)
static VkAccelerationStructureGeometryKHR blas_geom(uint32_t n) {
    return (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = xfb.addr,
                               .vertexStride = 16, .maxVertex = n ? n - 1 : 0, .indexType = VK_INDEX_TYPE_NONE_KHR,
                               .transformData.deviceAddress = xform.addr}};
}
static VkAccelerationStructureGeometryKHR tlas_geom(void) {
    return (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = inst.addr}};
}

static bool init(void) {
    if (state) return state > 0;
    state = -1;
    if (!g_vk.rt) return false;
#define GET(p, n) if (!(p = (void *)vkGetDeviceProcAddr(g_vk.dev, n))) { LOG("[rt] no %s\n", n); return false; }
    GET(p_bind_xfb, "vkCmdBindTransformFeedbackBuffersEXT");
    GET(p_begin_xfb, "vkCmdBeginTransformFeedbackEXT");
    GET(p_end_xfb, "vkCmdEndTransformFeedbackEXT");
    GET(p_create_as, "vkCreateAccelerationStructureKHR");
    GET(p_build_sizes, "vkGetAccelerationStructureBuildSizesKHR");
    GET(p_build_as, "vkCmdBuildAccelerationStructuresKHR");
    GET(p_as_addr, "vkGetAccelerationStructureDeviceAddressKHR");
#undef GET
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ap};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp);
    if (ap.minAccelerationStructureScratchOffsetAlignment) scratch_align = ap.minAccelerationStructureScratchOffsetAlignment;
    const VkBufferUsageFlags in = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    xfb = make_buf((VkDeviceSize)RT_CAP_VERTS * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | in);
    xform = make_buf(sizeof(VkTransformMatrixKHR), in);
    inst = make_buf(sizeof(VkAccelerationStructureInstanceKHR), in);
    // sizes for the largest capture; the TLAS: one instance
    VkAccelerationStructureGeometryKHR g = blas_geom(RT_CAP_VERTS);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
    uint32_t maxp = RT_CAP_VERTS / 3;
    VkAccelerationStructureBuildSizesInfoKHR bs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_build_sizes(g_vk.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxp, &bs);
    VkAccelerationStructureGeometryKHR tg = tlas_geom();
    VkAccelerationStructureBuildGeometryInfoKHR ti = bi;
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, ti.pGeometries = &tg;
    uint32_t one = 1;
    VkAccelerationStructureBuildSizesInfoKHR ts = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_build_sizes(g_vk.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &one, &ts);
    blas = make_as(&blas_mem, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, bs.accelerationStructureSize);
    tlas = make_as(&tlas_mem, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, ts.accelerationStructureSize);
    VkDeviceSize sc = bs.buildScratchSize > ts.buildScratchSize ? bs.buildScratchSize : ts.buildScratchSize;
    scratch = make_buf(sc + scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                      .accelerationStructure = blas};
    blas_addr = p_as_addr(g_vk.dev, &ai);
    LOG("[rt] ready: capture %u vertices, BLAS %llu KB, scratch %llu KB\n", RT_CAP_VERTS,
        (unsigned long long)(bs.accelerationStructureSize >> 10), (unsigned long long)(sc >> 10));
    state = 1;
    return true;
}

bool rt_available(void) { return init(); }

bool rt_capture_begin(VkCommandBuffer cb, uint64_t frame, uint32_t nverts) {
    if (!nverts || nverts % 3 || !init()) return false;
    if (frame != cap_frame) cap_frame = frame, cap_verts = 0;
    if (cap_verts + nverts > RT_CAP_VERTS) return false;  // (full: the rest of the frame casts no ray-traced shadow)
    VkDeviceSize off = (VkDeviceSize)cap_verts * 16, size = (VkDeviceSize)nverts * 16;
    p_bind_xfb(cb, 0, 1, &xfb.buf, &off, &size);
    p_begin_xfb(cb, 0, 0, NULL, NULL);
    cap_verts += nverts;
    return true;
}

void rt_capture_end(VkCommandBuffer cb) { p_end_xfb(cb, 0, 0, NULL, NULL); }

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

bool rt_build(VkCommandBuffer cb, uint64_t frame, const float proj[4]) {
    if (!init()) return false;
    {   static uint64_t n;  // (LARECOMP_RT_DEBUG: the state every 60 calls)
        if (getenv("LARECOMP_RT_DEBUG") && n++ % 60 == 0)
            LOG("[rt] build call %llu: frame %llu, captured frame %llu, %u vertices, built %d\n", (unsigned long long)n,
                (unsigned long long)frame, (unsigned long long)cap_frame, cap_verts, built);
    }
    if (frame != cap_frame || cap_verts < 3) return built;  // (nothing new: an in-between image keeps N's)
    // clip -> the lights pass's view space: (x / P00, y / P11, w), w = (P23 - z) / P22 (row-major P, w = -z_view)
    VkTransformMatrixKHR m = {{{1.0f / proj[0], 0, 0, 0}, {0, 1.0f / proj[1], 0, 0}, {0, 0, -1.0f / proj[2], proj[3] / proj[2]}}};
    VkAccelerationStructureInstanceKHR in = {.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .mask = 0xFF,
                                             .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
                                             .accelerationStructureReference = blas_addr};
    // (the last frame's ray queries and builds before this one's writes)
    barrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cb, xform.buf, 0, sizeof m, &m);
    vkCmdUpdateBuffer(cb, inst.buf, 0, sizeof in, &in);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    VkDeviceAddress sc = (scratch.addr + scratch_align - 1) & ~(scratch_align - 1);
    VkAccelerationStructureGeometryKHR g = blas_geom(cap_verts);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = blas,
                                                      .geometryCount = 1, .pGeometries = &g, .scratchData.deviceAddress = sc};
    VkAccelerationStructureBuildRangeInfoKHR br = {.primitiveCount = cap_verts / 3}, *pbr = &br;
    p_build_as(cb, 1, &bi, (const VkAccelerationStructureBuildRangeInfoKHR *const *)&pbr);
    barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    VkAccelerationStructureGeometryKHR tg = tlas_geom();
    VkAccelerationStructureBuildGeometryInfoKHR ti = bi;
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, ti.dstAccelerationStructure = tlas, ti.pGeometries = &tg;
    VkAccelerationStructureBuildRangeInfoKHR tr = {.primitiveCount = 1}, *ptr = &tr;
    p_build_as(cb, 1, &ti, (const VkAccelerationStructureBuildRangeInfoKHR *const *)&ptr);
    // (the ray queries after it; the next frame's capture after the BLAS read this one's)
    barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
    static int logged;
    if (logged++ < 3) LOG("[rt] frame %llu: %u triangles\n", (unsigned long long)frame, cap_verts / 3);
    {   // LARECOMP_RT_DEBUG: the first captured vertices, copied out at the 40th build and logged at the 80th (research)
        static VkBuffer rb;
        static VmaAllocation rm;
        static void *rp;
        static int nb;
        if (getenv("LARECOMP_RT_DEBUG") && ++nb == 40) {
            VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo inf;
            VK_CHECK(vmaCreateBuffer(g_vk.vma, &ci, &ai, &rb, &rm, &inf));
            rp = inf.pMappedData;
            barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy c[4];  // (16 vertices from the start, a quarter, the middle and three quarters of the frame's capture)
            for (int i = 0; i < 4; i++) c[i] = (VkBufferCopy){(VkDeviceSize)(cap_verts / 4 * i) * 16, (VkDeviceSize)i * 16 * 16, 16 * 16};
            vkCmdCopyBuffer(cb, xfb.buf, rb, 4, c);
            LOG("[rt] debug: proj %g %g %g %g, %u vertices\n", proj[0], proj[1], proj[2], proj[3], cap_verts);
        }
        if (nb == 80 && rp) {
            vmaInvalidateAllocation(g_vk.vma, rm, 0, VK_WHOLE_SIZE);
            const float *v = rp;
            for (int r = 0; r < 4; r++) {
                int nz = 0;
                for (int i = 0; i < 64; i++) nz += v[r * 64 + i] != 0.0f;
                LOG("[rt] debug: region %d: %d of 64 floats non-zero, first %g %g %g %g\n", r, nz, v[r * 64], v[r * 64 + 1], v[r * 64 + 2], v[r * 64 + 3]);
            }
        }
    }
    built = true;
    return true;
}

VkAccelerationStructureKHR rt_tlas(void) { return built ? tlas : VK_NULL_HANDLE; }
