// kate: hl glsl
//
// Tunables of the motion estimator, shared verbatim between the compute shader
// (motion_estimate.comp, motion_downsample.comp), the C++ side that drives them
// (motion_estimator.cpp) and the offline block matcher test. The file contains
// nothing but preprocessor defines so that it is valid in GLSL and in C++.
//
// Do not put anything else in here.

#pragma once

// Nominal size, in full resolution pixels, of one motion vector cell. The actual
// cell size is image_size / grid_size, which is this value rounded so that the
// grid covers the image exactly.
#define MOTION_BLOCK_PX 64

// Number of pyramid levels. Level 0 is the finest.
#define MOTION_LEVELS 3

// Level 0 is the source downscaled by 1 << MOTION_L0_SHIFT, level L by
// 1 << (MOTION_L0_SHIFT + L).
#define MOTION_L0_SHIFT 2

// Side, in texels of the level being searched, of the square window whose sum of
// absolute differences is minimised. The window is centred on the cell centre and
// is deliberately wider than the cell stride: overlapping windows are much more
// stable than disjoint ones.
//
// MOTION_WINDOW * MOTION_WINDOW * 255 must stay below 1 << 20, see the cost/index
// packing in motion_estimate.comp.
#define MOTION_WINDOW 8

// Search radius, in texels of the level, at the coarsest level. Exhaustive.
// (2 * r + 1)^2 must stay below 256, see the packing.
#define MOTION_RADIUS_COARSE 6

// Search radius at every finer level, around the vector propagated from the level
// above.
#define MOTION_RADIUS_FINE 2

// Work group size of motion_estimate.comp, one work group per cell. Must be a
// power of two.
#define MOTION_THREADS 64

// How far past the last real application frame a frame may be warped, in units of
// the interval the field spans. At 10 fps on a 90 Hz display this turns one frame
// into four, which is about where the smearing starts to cost more than the judder
// it removes. The headset-side warp caps itself at the same value
// (constants::stream::motion_max_steps); keep the two equal or the two modes stop
// looking alike.
#define MOTION_MAX_STEPS 3

// Work group side of motion_retain.comp and motion_warp.comp. Both are trivially
// parallel over output pixels.
#define MOTION_WARP_GROUP 8

// Largest displacement, as a fraction of the eye image, that is transmitted. A
// vector longer than this is clamped: at the frame rates this feature targets it
// is a matching failure rather than real motion, and letting it through would cost
// quantisation precision for every other cell.
#define MOTION_MAX_DISPLACEMENT 0.25
