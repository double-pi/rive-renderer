#version 310 es
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#define VERTEX
#define TARGET_VULKAN
#define ENABLE_INSTANCE_INDEX
#define OPTIONALLY_FLAT flat
#define DRAW_PATH
#include "glsl.minified.glsl"
#include "constants.minified.glsl"
#include "specialization.minified.glsl"
#include "common.minified.glsl"
#include "draw_path_common.minified.glsl"
#include "draw_path.minified.glsl"
