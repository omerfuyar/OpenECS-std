// cglm in C: cglm.h ends with cglm's own header, so a C plugin calls cglm's inline functions, such as glm_mat4_mul, directly (DESIGN 16).
// The build puts cglm's headers in OpenECS's include/ folder. The depth of cglm's projections goes from 0 to 1, as SDL's GPU API takes it and as
// the plugin's functions for Lua give it; a plugin that includes cglm itself defines CGLM_FORCE_DEPTH_ZERO_TO_ONE first too.

#ifndef CGLM_FORCE_DEPTH_ZERO_TO_ONE
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

#include "cglm/cglm.h"
