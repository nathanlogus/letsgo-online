// Ray-traced light shadows: the scene's triangles by transform feedback into an acceleration structure (rt.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

bool rt_available(void);  // the device has it (LARECOMP_RT=0: off) and the buffers are made
// Around one scene draw (inside the rendering): its nverts output vertices (a triangle list) go to this frame's capture.
// False: not captured (none left, or off) - then no rt_capture_end.
bool rt_capture_begin(VkCommandBuffer cb, uint64_t frame, uint32_t nverts);
void rt_capture_end(VkCommandBuffer cb);
// After the scene pass, outside rendering: the acceleration structures from this frame's capture, in the lights pass's view
// space (proj = P00, P11, P22, P23). False: none to trace.
bool rt_build(VkCommandBuffer cb, uint64_t frame, const float proj[4]);
VkAccelerationStructureKHR rt_tlas(void);  // the last one built, or none
