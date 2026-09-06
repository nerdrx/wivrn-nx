// The edge bleed ring, proved on the GPU: with the ring on, the reprojection pass never
// leaves the clear colour anywhere on the layer, however far the pose has moved.
//
// The complaint this exists to answer is a black band along the side of the view. It
// appears when the compositor's own late reprojection swings the layer further than the
// decoded picture reaches: past the last column of real pixels there is nothing to sample,
// and nothing is whatever the layer was cleared to, which is black. It is the one artefact
// that is not part of the picture, so it is the one the eye finds instantly.
//
// The fix under test is client/shaders/reprojection.glsl's `bleed` push constant: the
// outermost band of the defoveation grid is stretched over a margin outside the picture and
// filled out of the picture's own edge, either by clamping (mode 1) or by clamping and then
// decaying into that edge's averaged colour (mode 2). The content is invented. The point is
// that it is invented in the right colour, and that there is some of it everywhere.
//
// Nothing here mocks the shader. The real reprojection.glsl is compiled at test time by
// shelling out to glslangValidator, exactly as the client's build does, and run through a
// headless Vulkan device with a descriptor set layout and a vertex format copied from
// client/scenes/stream_defoveator.cpp. A change to the shader that breaks the guarantee
// fails here without anyone having to put a headset on.
//
// What is rendered, four times over, into a 512x512 target cleared to pure black:
//
//   BEFORE      the ring off AND every grid position shrunk by 1 / (1 + margin). That is
//               precisely the situation being fixed -- the layer is wider than the picture
//               and nothing fills the difference -- and it is the only honest way to
//               produce the artefact here, since this test cannot run the runtime whose
//               reprojection would otherwise expose the margin. Its border pixels are the
//               clear colour, and the first assertion insists on that: a "before" with no
//               black would mean the rest of the test measures nothing.
//   CLAMP       bleed = (0.05, 1, 0.25, 0), grid at full extent, ring drawn.
//   FADE        bleed = (0.05, 2, 0.25, 0).
//   FADE WIDE   bleed = (0.12, 2, 0.25, 0), to show the guarantee is not an accident of the
//               margin being narrow.
//
// All four carry the same large pose delta, expressed the way the shader expresses one:
// motion smoothing, with motion.x = 1 step along a motion field whose every vector is at
// full scale and motion.y = 0.15, so motion_offset() shifts the sample by 0.15 of the image
// diagonally -- fifteen per cent of the picture, far more than a frame of latency ever
// costs, and enough that a naive sampler would run off the side.
//
// A fifth render, FADE with the warp off, is used only for the join continuity measurement,
// so that the join is measured against real picture content rather than against the flat
// region the motion clamp produces at the edge.
//
// A sixth and seventh render cover the other half of the guarantee, the one `bleed_uv` was
// added for: when the encoder codes both eyes into one image, the ring must stretch this
// eye's edge and not walk into the eye beside it. The two halves of the test picture are
// given disjoint colour ranges -- the left is red-dominant everywhere, the right
// blue-dominant everywhere -- and a leg that draws the LEFT eye only is rendered twice, once
// with bleed_uv spanning the whole image (which is exactly what the code did before the
// rectangle was passed in, and which smears the other eye) and once with bleed_uv spanning
// the left half. "Did the other eye leak in" is then a decidable question: count the
// blue-dominant pixels.
//
// Build (needs only Vulkan and zlib; no project headers, no CMake). The Vulkan headers are
// not installed system wide on this machine, so they come from the tree's own toolchain
// prefix; drop the -I if your distribution ships vulkan-headers:
//
//   chrt -i 0 taskset -c 20-23 nice -n 19 g++ -std=c++20 -O2 -I /run/media/nerdrx/Lex/claude/tools/local/include -o /run/media/nerdrx/Lex/claude/nx-scratch/edge_bleed_test tests/edge_bleed_test.cpp -lvulkan -lz
//
// Run from the top of the working tree; argv[1] is the shader and argv[2] the directory the
// PNGs are written to:
//
//   chrt -i 0 taskset -c 20-23 nice -n 19 /run/media/nerdrx/Lex/claude/nx-scratch/edge_bleed_test client/shaders/reprojection.glsl /run/media/nerdrx/Lex/claude/nx-scratch/edgebleed
//
// (Both are one line each on purpose: a trailing backslash inside a // comment is a line
// continuation, and a copied recipe that swallows the line after it is worse than a long one.)
//
// It skips with status 0, rather than failing, when glslangValidator or a Vulkan device is
// not there, so it is safe to run on a machine that cannot do either.

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace
{

// ---------------------------------------------------------------------------------------
// Assertions, in the house style: every check prints, nothing throws, the count comes back
// as the exit status.

int failures = 0;
int checks = 0;

void check(bool ok, const std::string & what)
{
	++checks;
	if (ok)
	{
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	++failures;
	std::printf("  FAIL %s\n", what.c_str());
}

void check_eq(long got, long want, const std::string & what)
{
	++checks;
	if (got == want)
	{
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	++failures;
	std::printf("  FAIL %s\n         got:  %ld\n         want: %ld\n", what.c_str(), got, want);
}

[[noreturn]] void skip(const std::string & why)
{
	std::printf("SKIP: %s\n", why.c_str());
	std::exit(0);
}

#define VK_CHECK(x)                                                                       \
	do                                                                                \
	{                                                                                 \
		VkResult vk_check_result_ = (x);                                          \
		if (vk_check_result_ != VK_SUCCESS)                                       \
		{                                                                         \
			std::fprintf(stderr, "%s:%d: %s failed with VkResult %d\n",       \
			             __FILE__, __LINE__, #x, int(vk_check_result_));      \
			std::exit(2);                                                     \
		}                                                                         \
	} while (false)

// ---------------------------------------------------------------------------------------
// A PNG encoder, because the alternative is a third party dependency for something the
// deflate already in zlib does in sixty lines. Filter type 0 on every row: the images are
// small and written once, and a filter that has to be got right is a filter that can be got
// wrong.

void png_chunk(std::vector<uint8_t> & out, const char type[4], const uint8_t * data, size_t len)
{
	auto be32 = [&out](uint32_t v) {
		out.push_back(uint8_t(v >> 24));
		out.push_back(uint8_t(v >> 16));
		out.push_back(uint8_t(v >> 8));
		out.push_back(uint8_t(v));
	};
	be32(uint32_t(len));
	size_t crc_start = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data, data + len);
	uLong crc = crc32(0, out.data() + crc_start, uInt(out.size() - crc_start));
	be32(uint32_t(crc));
}

bool write_png(const std::string & path, const uint8_t * rgba, int w, int h)
{
	// The raw scanlines, each prefixed by its filter byte.
	std::vector<uint8_t> raw;
	raw.reserve(size_t(h) * (1 + size_t(w) * 4));
	for (int y = 0; y < h; ++y)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + (size_t(y) + 1) * w * 4);
	}

	uLongf comp_len = compressBound(uLong(raw.size()));
	std::vector<uint8_t> comp(comp_len);
	if (compress2(comp.data(), &comp_len, raw.data(), uLong(raw.size()), 6) != Z_OK)
		return false;
	comp.resize(comp_len);

	std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

	uint8_t ihdr[13];
	auto put32 = [](uint8_t * p, uint32_t v) {
		p[0] = uint8_t(v >> 24);
		p[1] = uint8_t(v >> 16);
		p[2] = uint8_t(v >> 8);
		p[3] = uint8_t(v);
	};
	put32(ihdr, uint32_t(w));
	put32(ihdr + 4, uint32_t(h));
	ihdr[8] = 8;  // bit depth
	ihdr[9] = 6;  // colour type: truecolour with alpha
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // adaptive filtering
	ihdr[12] = 0; // no interlace
	png_chunk(png, "IHDR", ihdr, sizeof(ihdr));
	png_chunk(png, "IDAT", comp.data(), comp.size());
	png_chunk(png, "IEND", nullptr, 0);

	std::FILE * f = std::fopen(path.c_str(), "wb");
	if (not f)
		return false;
	bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
	std::fclose(f);
	return ok;
}

// The border crop the PNGs carry beside the full frames: the left 96 columns, whole height,
// blown up three times by nearest neighbour so the band is legible without a zoom tool.
std::vector<uint8_t> crop_left_3x(const std::vector<uint8_t> & img, int w, int h, int cw, int & out_w, int & out_h)
{
	out_w = cw * 3;
	out_h = h * 3;
	std::vector<uint8_t> dst(size_t(out_w) * out_h * 4);
	for (int y = 0; y < out_h; ++y)
		for (int x = 0; x < out_w; ++x)
			std::memcpy(&dst[(size_t(y) * out_w + x) * 4],
			            &img[(size_t(y / 3) * w + (x / 3)) * 4],
			            4);
	(void)h;
	return dst;
}

// ---------------------------------------------------------------------------------------
// Geometry and format constants, all copied from the pass being tested.

constexpr int kSrc = 256;   // the decoded picture, square for simplicity
constexpr int kOut = 512;   // the layer the pass renders into
constexpr int kCells = 32;  // grid cells per axis; 33 vertices, so 256 divides evenly

// stream_defoveator::vertex. Both members are alignas(8), which makes the stride 16 and the
// uv offset 8; the pipeline's attribute descriptions below depend on exactly that.
struct vertex
{
	alignas(8) float position[2];
	alignas(8) uint32_t uv[2];
};
static_assert(sizeof(vertex) == 16);
static_assert(offsetof(vertex, uv) == 8);

// vert_pc from client/scenes/stream_defoveator.cpp, member for member. The static_assert is
// the same one that file carries, and for the same reason: a member added to the shader's
// block and not to this struct is a silent misalignment of everything after it.
struct vert_pc
{
	int32_t rgb_rect[4];
	int32_t a_rect[4];
	float scale[4];
	float bias[4];
	float post[4];
	float motion[4];
	float glow[4];
	float deband[4];
	float atlas_size[4];
	float atlas_geom[4];
	float atlas_range[4];
	float bleed[4];
	// The sub-rectangle this eye's picture occupies in the colour image, half a texel in
	// from each side: (x_lo, x_hi, y_lo, y_hi). Not (0, 1, 0, 1) whenever the encoder codes
	// the eyes as one image.
	float bleed_uv[4];
};
static_assert(sizeof(vert_pc) == 208);

// The atlas table the fragment shader declares: vec4 e[4 * 289 * 2]. Nothing reads it here
// (atlas_mode is 0, so the whole path compiles out) but the descriptor still has to point at
// a buffer big enough for the declared block, or validation objects and the driver may fault.
constexpr size_t kAtlasTableBytes = 4 * 289 * 2 * 4 * sizeof(float);

// ---------------------------------------------------------------------------------------
// Shader compilation. The client's build runs glslangValidator over this same file with the
// same two defines; doing it here rather than consuming a prebuilt module is the difference
// between testing the shader and testing whatever happened to be in the build directory.

std::vector<uint32_t> compile(const std::string & glsl, const char * stage, const char * define, const std::string & tmp)
{
	std::string spv = tmp + "/reprojection." + stage + ".spv";
	std::string cmd = "glslangValidator -V --target-env vulkan1.1 -S " + std::string(stage) +
	                  " -D" + define + " " + glsl + " -o " + spv + " > /dev/null";
	if (std::system(cmd.c_str()) != 0)
	{
		std::fprintf(stderr, "glslangValidator failed for stage %s; command was:\n  %s\n", stage, cmd.c_str());
		std::exit(2);
	}

	std::FILE * f = std::fopen(spv.c_str(), "rb");
	if (not f)
	{
		std::fprintf(stderr, "no SPIR-V at %s\n", spv.c_str());
		std::exit(2);
	}
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint32_t> code(size_t(len) / 4);
	if (std::fread(code.data(), 1, size_t(len), f) != size_t(len))
	{
		std::fclose(f);
		std::fprintf(stderr, "short read on %s\n", spv.c_str());
		std::exit(2);
	}
	std::fclose(f);
	return code;
}

// ---------------------------------------------------------------------------------------
// The headless device and everything hung off it. One struct rather than a class: this is a
// test, it lives for a second, and nothing is ever freed before the process exits.

struct vk_context
{
	VkInstance instance{};
	VkPhysicalDevice phys{};
	VkDevice device{};
	uint32_t queue_family{};
	VkQueue queue{};
	VkCommandPool pool{};
	VkPhysicalDeviceMemoryProperties mem_props{};

	uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want) const
	{
		for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
			if ((bits & (1u << i)) and (mem_props.memoryTypes[i].propertyFlags & want) == want)
				return i;
		std::fprintf(stderr, "no memory type with 0x%x\n", want);
		std::exit(2);
	}
};

struct buffer
{
	VkBuffer buf{};
	VkDeviceMemory mem{};
	void * mapped{};
};

buffer make_buffer(const vk_context & c, VkDeviceSize size, VkBufferUsageFlags usage, bool host)
{
	buffer b;
	VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                      .size = size,
	                      .usage = usage,
	                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VK_CHECK(vkCreateBuffer(c.device, &bi, nullptr, &b.buf));

	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(c.device, b.buf, &req);
	VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	                                  : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                        .allocationSize = req.size,
	                        .memoryTypeIndex = c.find_memory(req.memoryTypeBits, want)};
	VK_CHECK(vkAllocateMemory(c.device, &ai, nullptr, &b.mem));
	VK_CHECK(vkBindBufferMemory(c.device, b.buf, b.mem, 0));
	if (host)
		VK_CHECK(vkMapMemory(c.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
	return b;
}

struct image
{
	VkImage img{};
	VkDeviceMemory mem{};
	VkImageView view{};
	uint32_t w{}, h{};
};

image make_image(const vk_context & c, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
	image im{.w = w, .h = h};
	VkImageCreateInfo ii{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                     .imageType = VK_IMAGE_TYPE_2D,
	                     .format = fmt,
	                     .extent = {w, h, 1},
	                     .mipLevels = 1,
	                     .arrayLayers = 1,
	                     .samples = VK_SAMPLE_COUNT_1_BIT,
	                     .tiling = VK_IMAGE_TILING_OPTIMAL,
	                     .usage = usage,
	                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VK_CHECK(vkCreateImage(c.device, &ii, nullptr, &im.img));

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(c.device, im.img, &req);
	VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                        .allocationSize = req.size,
	                        .memoryTypeIndex = c.find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VK_CHECK(vkAllocateMemory(c.device, &ai, nullptr, &im.mem));
	VK_CHECK(vkBindImageMemory(c.device, im.img, im.mem, 0));

	VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                         .image = im.img,
	                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                         .format = fmt,
	                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VK_CHECK(vkCreateImageView(c.device, &vi, nullptr, &im.view));
	return im;
}

VkCommandBuffer begin_once(const vk_context & c)
{
	VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                               .commandPool = c.pool,
	                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                               .commandBufferCount = 1};
	VkCommandBuffer cb{};
	VK_CHECK(vkAllocateCommandBuffers(c.device, &ai, &cb));
	VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

void end_and_wait(const vk_context & c, VkCommandBuffer cb)
{
	VK_CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
	VK_CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
	VK_CHECK(vkQueueWaitIdle(c.queue));
	vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                       .srcAccessMask = src_access,
	                       .dstAccessMask = dst_access,
	                       .oldLayout = from,
	                       .newLayout = to,
	                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                       .image = img,
	                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------------------
// The test picture.
//
// Three properties matter and they pull against each other.
//
// It has to be unmistakably not the clear colour, so no channel goes below 60 anywhere and
// the periphery is a bright border.
//
// Its outer band has to be SMOOTH, because the continuity assertion measures the step
// between adjacent output pixels across the join and a hard edge in the source would be read
// as a discontinuity the shader never introduced. So the vivid hard edged bars live in the
// middle of the picture, well away from any border, and the outer band is a gentle field
// ramping into a uniform border over 48 texels.
//
// And its two halves have to be TELLABLE APART by a rule that survives bilinear filtering
// and blending: every texel of the left half has red at least 40 above blue, and every texel
// of the right half has blue at least 40 above red. Any mix of colours from one half stays
// on that half's side of the rule, because the rule is linear in the channels, so a single
// blue-dominant output pixel in a render of the LEFT eye means the ring reached across the
// eye split. That is the whole of the bleed_uv assertion.

// The margin by which a half's dominant channel leads the other, in 8-bit units. Chosen far
// enough above the filtering noise floor that a mix of a half's own colours can never trip
// the opposite test, and far enough below the roughly 90 unit lead the colours actually have
// that a genuine leak is unmissable.
constexpr int kDominance = 40;

bool blue_dominant(const uint8_t * p)
{
	return int(p[2]) >= int(p[0]) + kDominance;
}

std::vector<uint8_t> test_picture()
{
	std::vector<uint8_t> px(size_t(kSrc) * kSrc * 4);
	const float tau = 6.28318530718f;
	for (int y = 0; y < kSrc; ++y)
	{
		for (int x = 0; x < kSrc; ++x)
		{
			const bool right = x >= kSrc / 2;

			// A smooth vivid field, gradient bounded by 60 * tau / 128 per texel. The
			// dominant channel leads by at least 90 everywhere.
			float hot = 190 + 60 * (0.5f + 0.5f * std::sin(x * tau / 128.0f));
			float mid = 70 + 50 * (0.5f + 0.5f * std::sin(y * tau / 96.0f + 1.0f));
			float cold = 60 + 40 * (0.5f + 0.5f * std::sin((x + y) * tau / 160.0f + 2.0f));
			float r = right ? cold : hot;
			float g = mid;
			float b = right ? hot : cold;

			// Hard edged bars, but only in the middle half, where nothing this test
			// measures the smoothness of ever looks. Each side's bars keep that side's
			// dominance, so the halves stay decidable even across the bars.
			if (x >= 64 and x < 192 and y >= 64 and y < 192)
			{
				static const uint8_t left_bars[3][3] = {{255, 64, 64}, {255, 180, 64}, {230, 64, 150}};
				static const uint8_t right_bars[3][3] = {{64, 64, 255}, {64, 200, 255}, {120, 64, 240}};
				const uint8_t * c = right ? right_bars[((x - 128) / 22) % 3] : left_bars[((x - 64) / 22) % 3];
				r = c[0];
				g = c[1];
				b = c[2];
			}

			// Ramp into the border over the outermost 48 texels, smoothstepped so the
			// derivative at both ends is zero and nothing in the band reads as a step. The
			// border keeps its half's dominance too: a neutral white border would make the
			// eye split assertion undecidable, since a leak from the far edge would land
			// on a colour that is neither red- nor blue-dominant.
			const float bl[3] = {255, 230, 200};
			const float br[3] = {150, 190, 255};
			const float * border = right ? br : bl;
			int d = std::min(std::min(x, y), std::min(kSrc - 1 - x, kSrc - 1 - y));
			float t = std::min(1.0f, d / 48.0f);
			float w = t * t * (3.0f - 2.0f * t);
			r = border[0] + (r - border[0]) * w;
			g = border[1] + (g - border[1]) * w;
			b = border[2] + (b - border[2]) * w;

			size_t o = (size_t(y) * kSrc + x) * 4;
			px[o + 0] = uint8_t(r + 0.5f);
			px[o + 1] = uint8_t(g + 0.5f);
			px[o + 2] = uint8_t(b + 0.5f);
			px[o + 3] = 255;
		}
	}
	return px;
}

// ---------------------------------------------------------------------------------------
// The vertex grid, built the way stream_defoveator::defoveate() builds it: one triangle
// strip per row of cells, two vertices per column, then the row's last column twice over so
// the strip is broken before the next row starts. The extra half triangle that break leaves
// behind is drawn before the row that covers it, exactly as it is on the headset, so it is
// invisible there and invisible here.
//
// Positions span -1 to +1 INCLUSIVE, which is the whole point: the vertex shader's ring
// detection is step(1 - 1e-4, abs(p)), so a grid that stops a hair short of the border grows
// no ring at all and the test would quietly measure nothing.

//
// `uv_span` is how many source texels the grid covers, which is 256 for a stream carrying one
// eye and 128 for the left eye of a stereo pair coded side by side. rgb_rect stays at the
// whole image either way, because that is what the shader normalizes against.

std::vector<vertex> build_grid(float shrink, int uv_span)
{
	std::vector<vertex> v;
	v.reserve(size_t(2 * (kCells + 1) + 1) * kCells);
	const int cell_uv = uv_span / kCells;
	auto pos = [shrink](int i) { return shrink * (-1.0f + 2.0f * float(i) / float(kCells)); };
	auto push = [&v, &pos, cell_uv](int ix, int iy) {
		v.push_back(vertex{{pos(ix), pos(iy)}, {uint32_t(ix * cell_uv), uint32_t(iy * cell_uv)}});
	};

	for (int iy = 0; iy < kCells; ++iy)
	{
		for (int ix = 0; ix < kCells; ++ix)
		{
			push(ix, iy);
			push(ix, iy + 1);
		}
		push(kCells, iy);
		push(kCells, iy + 1);
		push(kCells, iy + 1);
	}
	return v;
}

// ---------------------------------------------------------------------------------------
// Image inspection helpers, all working on the 512x512 RGBA readback.

bool is_clear_colour(const uint8_t * p)
{
	// The clear colour is opaque black. Only the colour channels are compared: the pass
	// writes alpha from the picture and the assertion is about the colour that reaches
	// the eye.
	return p[0] == 0 and p[1] == 0 and p[2] == 0;
}

constexpr int kBandFrac = 12; // per cent of the image each border band covers

bool in_border_band(int x, int y)
{
	const int band = kOut * kBandFrac / 100;
	return x < band or x >= kOut - band or y < band or y >= kOut - band;
}

struct darkest
{
	int x = -1, y = -1, r = 255, g = 255, b = 255;
	int sum = 1 << 30;
};

darkest find_darkest(const std::vector<uint8_t> & img, bool band_only)
{
	darkest d;
	for (int y = 0; y < kOut; ++y)
		for (int x = 0; x < kOut; ++x)
		{
			if (band_only and not in_border_band(x, y))
				continue;
			const uint8_t * p = &img[(size_t(y) * kOut + x) * 4];
			int s = p[0] + p[1] + p[2];
			if (s < d.sum)
				d = darkest{x, y, p[0], p[1], p[2], s};
		}
	return d;
}

int count_clear(const std::vector<uint8_t> & img, bool band_only)
{
	int n = 0;
	for (int y = 0; y < kOut; ++y)
		for (int x = 0; x < kOut; ++x)
		{
			if (band_only and not in_border_band(x, y))
				continue;
			if (is_clear_colour(&img[(size_t(y) * kOut + x) * 4]))
				++n;
		}
	return n;
}

// The largest step between horizontally adjacent pixels over [x0, x1) on row y, over the
// colour channels.
struct step_result
{
	int delta = 0;
	int x = -1;
};

step_result max_step(const std::vector<uint8_t> & img, int y, int x0, int x1)
{
	step_result r;
	for (int x = x0; x + 1 < x1; ++x)
	{
		const uint8_t * a = &img[(size_t(y) * kOut + x) * 4];
		const uint8_t * b = &img[(size_t(y) * kOut + x + 1) * 4];
		for (int c = 0; c < 3; ++c)
		{
			int d = std::abs(int(a[c]) - int(b[c]));
			if (d > r.delta)
				r = step_result{d, x};
		}
	}
	return r;
}

} // namespace

int main(int argc, char ** argv)
{
	const std::string shader = argc > 1 ? argv[1] : "client/shaders/reprojection.glsl";
	const std::string out_dir = argc > 2 ? argv[2] : "nx-scratch/edgebleed";

	if (not std::filesystem::exists(shader))
		skip("shader not found at " + shader + " (run from the top of the working tree, or pass a path)");
	if (std::system("glslangValidator -v > /dev/null 2>&1") != 0)
		skip("glslangValidator is not on PATH");

	std::error_code ec;
	std::filesystem::create_directories(out_dir, ec);
	if (ec)
		skip("cannot create output directory " + out_dir + ": " + ec.message());

	char tmpl[] = "/tmp/edge_bleed_test.XXXXXX";
	const char * tmp = mkdtemp(tmpl);
	if (not tmp)
		skip("cannot create a temporary directory for the SPIR-V");

	std::printf("compiling %s\n", shader.c_str());
	auto vert_code = compile(shader, "vert", "VERT_SHADER", tmp);
	auto frag_code = compile(shader, "frag", "FRAG_SHADER", tmp);
	std::printf("  vert %zu words, frag %zu words\n", vert_code.size(), frag_code.size());

	// -------------------------------------------------------------------------------
	// Device

	vk_context c;
	{
		VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		                      .pApplicationName = "edge_bleed_test",
		                      .apiVersion = VK_API_VERSION_1_1};
		VkInstanceCreateInfo ii{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
		if (vkCreateInstance(&ii, nullptr, &c.instance) != VK_SUCCESS)
			skip("no Vulkan instance");
	}

	uint32_t n_phys = 0;
	vkEnumeratePhysicalDevices(c.instance, &n_phys, nullptr);
	if (n_phys == 0)
		skip("no Vulkan physical device");
	std::vector<VkPhysicalDevice> devices(n_phys);
	vkEnumeratePhysicalDevices(c.instance, &n_phys, devices.data());

	bool found = false;
	for (VkPhysicalDevice pd: devices)
	{
		uint32_t n_q = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &n_q, nullptr);
		std::vector<VkQueueFamilyProperties> qs(n_q);
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &n_q, qs.data());
		for (uint32_t i = 0; i < n_q; ++i)
			if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				c.phys = pd;
				c.queue_family = i;
				found = true;
				break;
			}
		if (found)
			break;
	}
	if (not found)
		skip("no physical device with a graphics queue");

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(c.phys, &props);
	std::printf("device: %s\n", props.deviceName);
	if (props.limits.maxPushConstantsSize < sizeof(vert_pc))
		skip("device's push constant limit is below the pass's 192 bytes");

	{
		float prio = 1.0f;
		VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		                           .queueFamilyIndex = c.queue_family,
		                           .queueCount = 1,
		                           .pQueuePriorities = &prio};
		VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		                      .queueCreateInfoCount = 1,
		                      .pQueueCreateInfos = &qi};
		VK_CHECK(vkCreateDevice(c.phys, &di, nullptr, &c.device));
	}
	vkGetDeviceQueue(c.device, c.queue_family, 0, &c.queue);
	vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem_props);
	{
		VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		                           .queueFamilyIndex = c.queue_family};
		VK_CHECK(vkCreateCommandPool(c.device, &pi, nullptr, &c.pool));
	}

	// -------------------------------------------------------------------------------
	// Resources

	image src = make_image(c, kSrc, kSrc, VK_FORMAT_R8G8B8A8_UNORM,
	                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	// The motion field, in the format the pass really uses: one signed byte per axis,
	// scaled by motion.y. Every texel is at full positive extent, so the warp is a uniform
	// diagonal shift and there is nothing in the field for a bug to hide behind.
	image motion = make_image(c, 8, 8, VK_FORMAT_R8G8_SNORM,
	                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	// The atlas prototype's three attachments. atlas_mode is 0 so none of them is read, but
	// they are in the descriptor set layout unconditionally on the headset too and the
	// layout here has to match that one binding for binding. The storage image is R16_UNORM
	// rather than anything wider because the shader declares the binding `r16`, and a
	// storage image view whose format does not match the declared one is invalid.
	image atlas_r16 = make_image(c, 8, 8, VK_FORMAT_R16_UNORM,
	                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	image atlas_rgba = make_image(c, 8, 8, VK_FORMAT_R8G8B8A8_UNORM,
	                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	image target = make_image(c, kOut, kOut, VK_FORMAT_R8G8B8A8_UNORM,
	                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	buffer table = make_buffer(c, kAtlasTableBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
	std::memset(table.mapped, 0, kAtlasTableBytes);

	buffer readback = make_buffer(c, size_t(kOut) * kOut * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

	// Two grids: the layer at full extent, and the layer shrunk with no ring, which is the
	// "before" the whole exercise is about.
	const float kBeforeMargin = 0.05f;
	// The margin every ringed leg below uses, named so Part C can work out where the ring
	// begins rather than restating the number.
	const double kBleedMargin = 0.05;
	auto grid_full = build_grid(1.0f, kSrc);
	auto grid_shrunk = build_grid(1.0f / (1.0f + kBeforeMargin), kSrc);
	// The left eye of a side-by-side pair: the same layer, drawn out of the left half of the
	// same decoded image.
	auto grid_left_eye = build_grid(1.0f, kSrc / 2);
	const uint32_t vertex_count = uint32_t(grid_full.size());

	auto upload_grid = [&](const std::vector<vertex> & g) {
		buffer b = make_buffer(c, g.size() * sizeof(vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		std::memcpy(b.mapped, g.data(), g.size() * sizeof(vertex));
		return b;
	};
	buffer vb_full = upload_grid(grid_full);
	buffer vb_shrunk = upload_grid(grid_shrunk);
	buffer vb_left_eye = upload_grid(grid_left_eye);

	// Upload the picture and give every other image a defined layout and defined contents.
	{
		auto picture = test_picture();
		buffer stage_src = make_buffer(c, picture.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		std::memcpy(stage_src.mapped, picture.data(), picture.size());

		// Every motion texel at +127, which is +1.0 through the SNORM conversion.
		std::vector<int8_t> mf(8 * 8 * 2, 127);
		buffer stage_motion = make_buffer(c, mf.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		std::memcpy(stage_motion.mapped, mf.data(), mf.size());

		std::vector<uint8_t> zeros(8 * 8 * 4, 0);
		buffer stage_zero = make_buffer(c, zeros.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		std::memcpy(stage_zero.mapped, zeros.data(), zeros.size());

		VkCommandBuffer cb = begin_once(c);
		auto upload = [&](image & im, buffer & st) {
			barrier(cb, im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        0, VK_ACCESS_TRANSFER_WRITE_BIT,
			        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			                       .imageExtent = {im.w, im.h, 1}};
			vkCmdCopyBufferToImage(cb, st.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			barrier(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		};
		upload(src, stage_src);
		upload(motion, stage_motion);
		upload(atlas_rgba, stage_zero);
		upload(atlas_r16, stage_zero);
		// The storage view of the atlas has to be in GENERAL, which is what the pass
		// binds it in on the headset too.
		barrier(cb, atlas_r16.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		end_and_wait(c, cb);
	}

	// -------------------------------------------------------------------------------
	// Samplers, descriptors, render pass, pipeline.
	//
	// The headset's pass uses immutable samplers because the colour one may carry a YCbCr
	// conversion that has to be baked into the layout. Nothing here decodes YCbCr, so
	// ordinary samplers keep the setup honest and short.

	VkSampler sampler{};
	{
		VkSamplerCreateInfo si{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		                       .magFilter = VK_FILTER_LINEAR,
		                       .minFilter = VK_FILTER_LINEAR,
		                       .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		                       .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                       .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                       .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                       .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
		VK_CHECK(vkCreateSampler(c.device, &si, nullptr, &sampler));
	}

	// Bindings 0..6, matching stream_defoveator::ensure_pipeline() exactly. Binding 0 has
	// descriptorCount alpha + 1, and alpha is specialized to 0 here, so it is 1.
	VkDescriptorSetLayout ds_layout{};
	{
		VkDescriptorSetLayoutBinding b[7]{};
		auto set = [&](int i, VkDescriptorType t, uint32_t n) {
			b[i].binding = uint32_t(i);
			b[i].descriptorType = t;
			b[i].descriptorCount = n;
			b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		};
		set(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
		set(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
		set(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
		set(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
		set(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1);
		set(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
		set(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
		VkDescriptorSetLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		                                   .bindingCount = 7,
		                                   .pBindings = b};
		VK_CHECK(vkCreateDescriptorSetLayout(c.device, &li, nullptr, &ds_layout));
	}

	VkDescriptorPool ds_pool{};
	VkDescriptorSet ds{};
	{
		VkDescriptorPoolSize sizes[3]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
		                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
		                              {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
		VkDescriptorPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                              .maxSets = 1,
		                              .poolSizeCount = 3,
		                              .pPoolSizes = sizes};
		VK_CHECK(vkCreateDescriptorPool(c.device, &pi, nullptr, &ds_pool));
		VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		                               .descriptorPool = ds_pool,
		                               .descriptorSetCount = 1,
		                               .pSetLayouts = &ds_layout};
		VK_CHECK(vkAllocateDescriptorSets(c.device, &ai, &ds));

		VkDescriptorImageInfo i_src{sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo i_motion{sampler, motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo i_atlas_s{sampler, atlas_r16.view, VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorImageInfo i_atlas_st{VK_NULL_HANDLE, atlas_r16.view, VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorImageInfo i_atlas_8{sampler, atlas_rgba.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo i_table{table.buf, 0, VK_WHOLE_SIZE};

		VkWriteDescriptorSet w[7]{};
		auto wr = [&](int i, uint32_t binding, VkDescriptorType t, const VkDescriptorImageInfo * ii,
		              const VkDescriptorBufferInfo * bi) {
			w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[i].dstSet = ds;
			w[i].dstBinding = binding;
			w[i].descriptorCount = 1;
			w[i].descriptorType = t;
			w[i].pImageInfo = ii;
			w[i].pBufferInfo = bi;
		};
		wr(0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i_src, nullptr);
		wr(1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i_motion, nullptr);
		// prev_rgb, bound to the colour image itself, which is what the pass does when
		// there is no previous frame. motion.z stays 0, so it is never sampled.
		wr(2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i_src, nullptr);
		wr(3, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i_atlas_s, nullptr);
		wr(4, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i_atlas_st, nullptr);
		wr(5, 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &i_table);
		wr(6, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i_atlas_8, nullptr);
		vkUpdateDescriptorSets(c.device, 7, w, 0, nullptr);
	}

	VkRenderPass render_pass{};
	{
		VkAttachmentDescription att{.format = VK_FORMAT_R8G8B8A8_UNORM,
		                            .samples = VK_SAMPLE_COUNT_1_BIT,
		                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		                            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		                            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
		VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		                         .colorAttachmentCount = 1,
		                         .pColorAttachments = &ref};
		VkRenderPassCreateInfo ri{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		                          .attachmentCount = 1,
		                          .pAttachments = &att,
		                          .subpassCount = 1,
		                          .pSubpasses = &sub};
		VK_CHECK(vkCreateRenderPass(c.device, &ri, nullptr, &render_pass));
	}

	VkFramebuffer framebuffer{};
	{
		VkFramebufferCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		                           .renderPass = render_pass,
		                           .attachmentCount = 1,
		                           .pAttachments = &target.view,
		                           .width = kOut,
		                           .height = kOut,
		                           .layers = 1};
		VK_CHECK(vkCreateFramebuffer(c.device, &fi, nullptr, &framebuffer));
	}

	VkPipelineLayout pipeline_layout{};
	{
		VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vert_pc)};
		VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                              .setLayoutCount = 1,
		                              .pSetLayouts = &ds_layout,
		                              .pushConstantRangeCount = 1,
		                              .pPushConstantRanges = &pc};
		VK_CHECK(vkCreatePipelineLayout(c.device, &li, nullptr, &pipeline_layout));
	}

	VkPipeline pipeline{};
	{
		auto module_of = [&](const std::vector<uint32_t> & code) {
			VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			                            .codeSize = code.size() * 4,
			                            .pCode = code.data()};
			VkShaderModule m{};
			VK_CHECK(vkCreateShaderModule(c.device, &mi, nullptr, &m));
			return m;
		};
		VkShaderModule vs = module_of(vert_code);
		VkShaderModule fs = module_of(frag_code);

		// Every specialization constant the fragment shader declares, in id order. All the
		// optional kernels are off: what is under test is the ring, and a sharpener or an
		// upscaler between the sample and the output would only blur the question.
		struct spec_data
		{
			int32_t alpha;
			uint32_t do_srgb;
			uint32_t cas_full_kernel;
			uint32_t fsr_enable;
			int32_t atlas_mode;
			int32_t atlas_tiles;
			int32_t atlas_eye;
			uint32_t lowpoly_enable;
			uint32_t lowpoly_full_kernel;
		} spec{0, 0, 0, 0, 0, 17, 0, 0, 0};
		VkSpecializationMapEntry entries[9];
		for (uint32_t i = 0; i < 9; ++i)
			entries[i] = VkSpecializationMapEntry{i, i * 4u, 4};
		VkSpecializationInfo spec_info{9, entries, sizeof(spec), &spec};

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		             .stage = VK_SHADER_STAGE_VERTEX_BIT,
		             .module = vs,
		             .pName = "main"};
		stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		             .module = fs,
		             .pName = "main",
		             .pSpecializationInfo = &spec_info};

		VkVertexInputBindingDescription vb_desc{0, sizeof(vertex), VK_VERTEX_INPUT_RATE_VERTEX};
		VkVertexInputAttributeDescription attrs[2]{
		        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vertex, position)},
		        {1, 0, VK_FORMAT_R32G32_UINT, offsetof(vertex, uv)}};
		VkPipelineVertexInputStateCreateInfo vi{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		                                        .vertexBindingDescriptionCount = 1,
		                                        .pVertexBindingDescriptions = &vb_desc,
		                                        .vertexAttributeDescriptionCount = 2,
		                                        .pVertexAttributeDescriptions = attrs};
		VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		                                          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
		VkViewport viewport{0, 0, float(kOut), float(kOut), 0, 1};
		VkRect2D scissor{{0, 0}, {kOut, kOut}};
		VkPipelineViewportStateCreateInfo vp{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		                                     .viewportCount = 1,
		                                     .pViewports = &viewport,
		                                     .scissorCount = 1,
		                                     .pScissors = &scissor};
		VkPipelineRasterizationStateCreateInfo rs{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		                                          .polygonMode = VK_POLYGON_MODE_FILL,
		                                          .cullMode = VK_CULL_MODE_NONE,
		                                          .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		                                          .lineWidth = 1};
		VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
		VkPipelineColorBlendAttachmentState cba{.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
		VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		                                       .attachmentCount = 1,
		                                       .pAttachments = &cba};
		VkGraphicsPipelineCreateInfo gi{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		                                .stageCount = 2,
		                                .pStages = stages,
		                                .pVertexInputState = &vi,
		                                .pInputAssemblyState = &ia,
		                                .pViewportState = &vp,
		                                .pRasterizationState = &rs,
		                                .pMultisampleState = &ms,
		                                .pColorBlendState = &cb,
		                                .layout = pipeline_layout,
		                                .renderPass = render_pass,
		                                .subpass = 0};
		VK_CHECK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline));
	}

	// -------------------------------------------------------------------------------
	// Rendering

	// The pose delta, in the shader's own terms. motion.x is one full step along the field
	// and motion.y is the longest vector as a fraction of the eye image, so with every field
	// texel at +1 the sample moves by 0.15 of the picture up and to the left -- fifteen per
	// cent, several times the worst real reprojection, and enough that anything not defended
	// against running off the edge would do so.
	const float kMotionStep = 1.0f;
	const float kMotionScale = 0.15f;

	// This eye's picture inside the decoded image, half a texel in from each side, the shape
	// eye_uv_limits() in stream_defoveator.cpp produces. Whole image for the single eye legs,
	// left half for the eye split ones.
	const float uv_whole[4]{0.5f / kSrc, (kSrc - 0.5f) / kSrc, 0.5f / kSrc, (kSrc - 0.5f) / kSrc};
	const float uv_left_half[4]{0.5f / kSrc, (kSrc / 2 - 0.5f) / kSrc, 0.5f / kSrc, (kSrc - 0.5f) / kSrc};

	enum class grid
	{
		full,
		shrunk,
		left_eye,
	};

	struct config
	{
		const char * name;
		float bleed[4];
		bool warp;
		grid mesh;
		const float * bleed_uv;
	};

	auto render = [&](const config & cfg) {
		vert_pc pc{};
		pc.rgb_rect[0] = 0;
		pc.rgb_rect[1] = 0;
		pc.rgb_rect[2] = kSrc;
		pc.rgb_rect[3] = kSrc;
		// No alpha stream: alpha is specialized to 0, so a_rect is never read. It is still
		// given the picture's geometry rather than zeros, because a zero rect would be a
		// division by zero in the vertex shader whatever the fragment shader does with it.
		pc.a_rect[0] = 0;
		pc.a_rect[1] = 0;
		pc.a_rect[2] = kSrc;
		pc.a_rect[3] = kSrc;
		pc.scale[0] = pc.scale[1] = pc.scale[2] = pc.scale[3] = 1.0f;
		pc.motion[0] = cfg.warp ? kMotionStep : 0.0f;
		pc.motion[1] = kMotionScale;
		pc.atlas_size[0] = pc.atlas_size[1] = float(kSrc);
		pc.atlas_geom[0] = pc.atlas_geom[1] = 8.0f;
		pc.atlas_geom[2] = pc.atlas_geom[3] = 1.0f / 8.0f;
		std::memcpy(pc.bleed, cfg.bleed, sizeof(pc.bleed));
		std::memcpy(pc.bleed_uv, cfg.bleed_uv, sizeof(pc.bleed_uv));

		VkCommandBuffer cmd = begin_once(c);
		VkClearValue clear{};
		clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // the clear colour every assertion is about
		VkRenderPassBeginInfo bi{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		                         .renderPass = render_pass,
		                         .framebuffer = framebuffer,
		                         .renderArea = {{0, 0}, {kOut, kOut}},
		                         .clearValueCount = 1,
		                         .pClearValues = &clear};
		vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &ds, 0, nullptr);
		vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                   0, sizeof(pc), &pc);
		VkDeviceSize offset = 0;
		VkBuffer vb = cfg.mesh == grid::shrunk ? vb_shrunk.buf
		              : cfg.mesh == grid::left_eye ? vb_left_eye.buf
		                                           : vb_full.buf;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdDraw(cmd, vertex_count, 1, 0, 0);
		vkCmdEndRenderPass(cmd);

		VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		                       .imageExtent = {kOut, kOut, 1}};
		vkCmdCopyImageToBuffer(cmd, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &copy);
		end_and_wait(c, cmd);

		std::vector<uint8_t> out(size_t(kOut) * kOut * 4);
		std::memcpy(out.data(), readback.mapped, out.size());
		return out;
	};

	const config cfg_before{"before", {0, 0, 0, 0}, true, grid::shrunk, uv_whole};
	const config cfg_clamp{"clamp", {0.05f, 1.0f, 0.25f, 0}, true, grid::full, uv_whole};
	const config cfg_fade{"fade", {0.05f, 2.0f, 0.25f, 0}, true, grid::full, uv_whole};
	const config cfg_fade_wide{"fade-wide", {0.12f, 2.0f, 0.25f, 0}, true, grid::full, uv_whole};
	// Used only for the join continuity measurement: with the warp on, the shader's own
	// motion clamp flattens the outer fifteen per cent of the picture into a stretch of
	// column zero, and a flat region proves nothing about a join.
	const config cfg_fade_still{"fade-still", {0.05f, 2.0f, 0.25f, 0}, false, grid::full, uv_whole};
	// The off path, twice: once with the whole bleed vector zero, once with a mode and a
	// fade distance set but the margin at zero, which is how the pass carries it whenever
	// the server overscanned. The two must be byte identical.
	const config cfg_off_zero{"off-zero", {0, 0, 0, 0}, true, grid::full, uv_whole};
	const config cfg_off_armed{"off-armed", {0.0f, 2.0f, 0.25f, 0}, true, grid::full, uv_whole};
	// The eye split pair. Both draw the LEFT eye out of a decoded image that holds both, and
	// they differ only in bleed_uv: the whole image, which is what the stretch was clamped
	// against before the rectangle was passed in, against this eye's half. The warp is off
	// for these two, because the motion clamp would pull the sample back inside the picture
	// and hide the very reach being measured.
	const config cfg_split_bug{"eyesplit-whole-image", {0.05f, 1.0f, 0.25f, 0}, false, grid::left_eye, uv_whole};
	const config cfg_split_fix{"eyesplit-this-eye", {0.05f, 1.0f, 0.25f, 0}, false, grid::left_eye, uv_left_half};

	auto img_before = render(cfg_before);
	auto img_clamp = render(cfg_clamp);
	auto img_fade = render(cfg_fade);
	auto img_fade_wide = render(cfg_fade_wide);
	auto img_fade_still = render(cfg_fade_still);
	auto img_off_zero = render(cfg_off_zero);
	auto img_off_armed = render(cfg_off_armed);
	auto img_split_bug = render(cfg_split_bug);
	auto img_split_fix = render(cfg_split_fix);

	// -------------------------------------------------------------------------------
	// Assertions

	std::printf("\nPart A: the artefact exists to be fixed\n");
	{
		int n = count_clear(img_before, true);
		check(n > 0,
		      "BEFORE (layer shrunk by 1/(1 + 0.05), ring off) leaves clear colour in the border band: " +
		              std::to_string(n) + " pixels of " + std::to_string(kOut * kOut - (kOut - 2 * (kOut * kBandFrac / 100)) * (kOut - 2 * (kOut * kBandFrac / 100))) +
		              " in the band");
	}

	std::printf("\nPart B: with the ring on, nothing is ever the clear colour\n");
	{
		auto no_black = [](const std::vector<uint8_t> & img, const char * name) {
			darkest band = find_darkest(img, true);
			darkest all = find_darkest(img, false);
			check(count_clear(img, true) == 0,
			      std::string(name) + ": no clear-colour pixel in the outer " + std::to_string(kBandFrac) +
			              "% border band (darkest there is (" + std::to_string(band.r) + ", " +
			              std::to_string(band.g) + ", " + std::to_string(band.b) + ") at " +
			              std::to_string(band.x) + ", " + std::to_string(band.y) + ")");
			check(count_clear(img, false) == 0,
			      std::string(name) + ": no clear-colour pixel anywhere in the frame (darkest is (" +
			              std::to_string(all.r) + ", " + std::to_string(all.g) + ", " + std::to_string(all.b) +
			              ") at " + std::to_string(all.x) + ", " + std::to_string(all.y) + ")");
		};
		no_black(img_clamp, "CLAMP");
		no_black(img_fade, "FADE");
		no_black(img_fade_wide, "FADE WIDE");
	}

	std::printf("\nPart C: the join between the picture and the ring is continuous\n");
	{
		// What "continuous" has to mean here, and what it cannot.
		//
		// An absolute threshold on the step across the join measures the SOURCE, not the
		// join: the test picture's two halves carry disjoint colour ranges so that Part F
		// can detect a leak across the eye split, and that gives it hard bar edges whose
		// own adjacent steps reach into the eighties wherever one lands. A join sitting
		// on top of a bar edge would fail a fixed threshold while being perfectly smooth,
		// and a join sitting on flat colour would pass one while being visibly creased.
		// The first version of this check did exactly that and reported a step of 133 on
		// the right and 3 on the left, from one render, for no reason but where the bars
		// fell.
		//
		// So the claim is made relative to the content instead: a crease at the join is a
		// step ACROSS the join that is larger than the steps the picture is already making
		// on either side of it. bleed_clamp() smoothsteps the stretch, so its derivative
		// at the join is zero and the join pair should be unremarkable among its
		// neighbours. `kSlack` is there so that a join landing on flat colour, where the
		// local steps are 0 or 1, is not held to an impossible standard; a black band
		// would show roughly 150 units and clears it by an order of magnitude either way.
		const int kSlack = 8;
		const int row = kOut / 2;

		// Where the ring begins. The vertex shader leaves the boundary vertices at +/-1
		// and moves every interior one inward by 1 / (1 + margin), so the outermost cell
		// runs from the second-to-last vertex, at NDC (1 - 1/kCells) / (1 + margin), out
		// to the border.
		const double inner_ndc = (1.0 - 1.0 / kCells) / (1.0 + kBleedMargin);
		const int join_right = int(std::lround(kOut * (1.0 + inner_ndc) * 0.5));
		const int join_left = kOut - join_right;

		// The step across the join itself, and the largest step the picture makes within
		// `kWindow` pixels on either side of it, excluding the join pair.
		const int kWindow = 10;
		const auto join_check = [&](int join, const char * side) {
			const step_result across = max_step(img_fade_still, row, join - 1, join + 1);
			const step_result before = max_step(img_fade_still, row, join - 1 - kWindow, join - 1);
			const step_result after = max_step(img_fade_still, row, join + 1, join + 1 + kWindow);
			const int local = std::max(before.delta, after.delta);
			const int allowed = local + kSlack;
			check(across.delta <= allowed,
			      std::string("FADE, warp off: the ") + side + " join at x = " + std::to_string(join) +
			              " is no sharper than the picture around it (step across the join " +
			              std::to_string(across.delta) + ", largest step within " +
			              std::to_string(kWindow) + " pixels either side " + std::to_string(local) +
			              ", allowed " + std::to_string(allowed) + ")");
		};
		join_check(join_left, "left");
		join_check(join_right, "right");

		// And the ring's own interior must be smooth in absolute terms, which IS a fair
		// absolute claim: the ring is a stretch of one cell's worth of texels over more
		// output pixels than the interior gets, plus a fade toward a single colour, so
		// whatever the source does it cannot make the ring change faster than the picture
		// it came from. Measured from just inside the ring to the last column.
		const step_result ring = max_step(img_fade_still, row, join_right + 2, kOut);
		const step_result picture = max_step(img_fade_still, row, kOut / 4, 3 * kOut / 4);
		check(ring.delta <= picture.delta,
		      "FADE, warp off: the ring is no busier than the picture it is stretched from "
		      "(largest step in the ring " +
		              std::to_string(ring.delta) + " at x = " + std::to_string(ring.x) +
		              ", largest step in the picture " + std::to_string(picture.delta) + ")");
	}

	std::printf("\nPart D: the guarantee does not depend on the margin being narrow\n");
	{
		darkest d = find_darkest(img_fade_wide, false);
		check(count_clear(img_fade_wide, false) == 0,
		      "FADE at a margin of 0.12, more than twice the default, still has no clear-colour pixel "
		      "(darkest is (" +
		              std::to_string(d.r) + ", " + std::to_string(d.g) + ", " + std::to_string(d.b) + "))");
	}

	std::printf("\nPart E: the feature costs nothing when it is off\n");
	{
		check_eq(long(std::memcmp(img_off_zero.data(), img_off_armed.data(), img_off_zero.size())), 0,
		         "bleed = (0, 0, 0, 0) and bleed = (0, 2, 0.25, 0) render byte-identical frames: a zero "
		         "margin disables the ring outright, whatever the mode says");
		check(count_clear(img_off_zero, false) == 0,
		      "the off path at full grid extent has no clear-colour pixel either, so Part A's black "
		      "comes from the shrunk layer and not from the warp");
	}

	std::printf("\nPart F: the ring never reaches across the eye split\n");
	{
		auto count_blue = [](const std::vector<uint8_t> & img) {
			int n = 0;
			for (size_t i = 0; i < img.size(); i += 4)
				if (blue_dominant(&img[i]))
					++n;
			return n;
		};
		int leaked = count_blue(img_split_bug);
		int clean = count_blue(img_split_fix);
		check(leaked > 0,
		      "the control leg, clamping the left eye's ring against the WHOLE image, does pull the "
		      "right eye in: " +
		              std::to_string(leaked) + " blue-dominant pixels, so the assertion below can see a leak");
		check_eq(clean, 0,
		         "with bleed_uv set to this eye's half of the image, no blue-dominant pixel appears "
		         "anywhere: the stretch stops at the eye split");
	}

	// -------------------------------------------------------------------------------
	// Pictures

	std::printf("\nWriting PNGs to %s\n", out_dir.c_str());
	struct output
	{
		const char * name;
		const std::vector<uint8_t> * img;
		bool crop;
	};
	const output outputs[]{{"before", &img_before, true},
	                       {"clamp", &img_clamp, true},
	                       {"fade", &img_fade, true},
	                       {"fade-wide", &img_fade_wide, false},
	                       {"eyesplit-whole-image", &img_split_bug, false},
	                       {"eyesplit-this-eye", &img_split_fix, false}};
	for (const auto & o: outputs)
	{
		std::string full = out_dir + "/edgebleed-" + o.name + ".png";
		check(write_png(full, o.img->data(), kOut, kOut), std::string("wrote ") + full);
		if (not o.crop)
			continue;
		int cw = 0, ch = 0;
		auto crop = crop_left_3x(*o.img, kOut, kOut, 96, cw, ch);
		std::string border = out_dir + "/edgebleed-border-" + o.name + ".png";
		check(write_png(border, crop.data(), cw, ch), std::string("wrote ") + border);
	}

	std::printf("\n%d checks, %d failures\n", checks, failures);
	std::filesystem::remove_all(tmp, ec);
	return failures == 0 ? 0 : 1;
}
