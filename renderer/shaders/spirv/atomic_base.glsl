#extension GL_EXT_samplerless_texture_functions : require
#define TARGET_VULKAN
#define ENABLE_INSTANCE_INDEX
#define PLS_IMPL_SUBPASS_LOAD
#define USING_PLS_STORAGE_TEXTURES
#define OPTIONALLY_FLAT flat
#include "glsl.minified.glsl"
#include "constants.minified.glsl"
#include "specialization.minified.glsl"
#include "advanced_blend.minified.glsl"
#include "common.minified.glsl"
#include "draw_path_common.minified.glsl"
#include "atomic_draw.minified.glsl"
