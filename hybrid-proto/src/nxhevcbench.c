// nxhevcbench -- a standalone MediaCodec HEVC decode benchmark for the Pico 4.
//
// Scoping measurement for NX Warp hybrid mode: what does the (idle) hardware
// HEVC decoder actually cost, per 1088x1088 frame, on this device?
//
// It deliberately mirrors what client/decoder/android/android_decoder.cpp does,
// so the numbers transfer to the real client:
//   * AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE,
//       CPU_READ_NEVER | CPU_WRITE_NEVER | GPU_SAMPLED_IMAGE, 7)
//   * AMediaCodec_configure onto that reader's ANativeWindow
//   * KEY_OPERATING_RATE = fps, KEY_PRIORITY = 0
//   * async callbacks, releaseOutputBuffer(render = true)
// and adds the two knobs the client does not set, behind flags, so their effect
// is measurable: "low-latency" (AMEDIAFORMAT_KEY_LOW_LATENCY, API 30) and
// "vendor.qti-ext-dec-low-latency.enable" (commented out at android_decoder.cpp:141).
//
// Modes:
//   latency     one access unit in flight at a time. Serial submit->AImage
//               latency: what an atlas-refresh path would actually wait for.
//   throughput  keep the codec fed. Steady-state per-frame ASIC occupancy.
//   dump        ByteBuffer output (no surface), de-strided to yuv420p on disk,
//               for the bit-exactness comparison against a conforming decoder.
//
// Usage:
//   nxhevcbench --stream S.hevc --index S.idx --csd S.csd
//               [--mode latency|throughput|dump] [--frames N] [--warmup N]
//               [--fps 90] [--low-latency] [--qti-low-latency]
//               [--out FILE] [--json FILE]
//
// No Android app, no activity, no window: AImageReader gives a producer surface
// without a display, so this runs straight from `adb shell` in /data/local/tmp
// and touches neither wivrn-server nor the installed test client.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <android/hardware_buffer.h>

#define MAXF 4096

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------- stream/index

struct au {
	size_t off, size;
	int key;
};

static uint8_t * g_stream;
static size_t g_stream_sz;
static struct au g_au[MAXF];
static int g_naus;

static uint8_t * g_csd;
static size_t g_csd_sz;

static uint8_t * slurp(const char * p, size_t * n)
{
	FILE * f = fopen(p, "rb");
	if (!f)
	{
		fprintf(stderr, "open %s: %s\n", p, strerror(errno));
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t * b = malloc(sz);
	if (fread(b, 1, sz, f) != (size_t)sz)
	{
		fprintf(stderr, "short read %s\n", p);
		exit(2);
	}
	fclose(f);
	*n = sz;
	return b;
}

static void load_index(const char * p)
{
	FILE * f = fopen(p, "r");
	if (!f)
	{
		fprintf(stderr, "open %s: %s\n", p, strerror(errno));
		exit(2);
	}
	unsigned long o, s;
	int k;
	while (g_naus < MAXF && fscanf(f, "%lu %lu %d", &o, &s, &k) == 3)
		g_au[g_naus++] = (struct au){o, s, k};
	fclose(f);
}

// ---------------------------------------------------------------- shared state

struct ctx {
	AMediaCodec * codec;
	AImageReader * reader;

	int mode; // 0 latency, 1 throughput, 2 dump
	int frames, warmup;
	int w, h;

	pthread_mutex_t mu;
	pthread_cond_t cv;

	// input buffer indices handed to us by the async callback
	int32_t inq[64];
	int inq_n;

	// how many pictures have come all the way out
	int out_n;
	double out_t[MAXF];   // arrival time of output n
	int32_t out_idx[MAXF]; // frame index recovered from pts

	double submit_t[MAXF]; // when AU n was queued

	int fed;  // AUs queued so far
	int eos;
	int err;

	FILE * dump;
	int dumped;
	// output format, discovered on the first output-format-changed
	int32_t cf, stride, slice_h, crop_w, crop_h;
	int fmt_known;
};

static struct ctx C;

__attribute__((unused)) static void on_input(AMediaCodec * mc, void * ud, int32_t idx)
{
	(void)mc;
	(void)ud;
	pthread_mutex_lock(&C.mu);
	if (C.inq_n < 64)
		C.inq[C.inq_n++] = idx;
	pthread_cond_broadcast(&C.cv);
	pthread_mutex_unlock(&C.mu);
}

static void drain_bytebuffer(int32_t idx, AMediaCodecBufferInfo * bi);

__attribute__((unused)) static void on_output(AMediaCodec * mc, void * ud, int32_t idx, AMediaCodecBufferInfo * bi)
{
	(void)ud;
	if (C.mode == 2)
	{
		drain_bytebuffer(idx, bi);
		AMediaCodec_releaseOutputBuffer(mc, idx, false);
		pthread_mutex_lock(&C.mu);
		int n = C.out_n;
		if (n < MAXF)
		{
			C.out_t[n] = now_ms();
			C.out_idx[n] = (int32_t)(bi->presentationTimeUs / 10000);
		}
		C.out_n++;
		pthread_cond_broadcast(&C.cv);
		pthread_mutex_unlock(&C.mu);
		return;
	}
	// surface path: hand it to the ImageReader. Time is taken in the image
	// callback, which is where a real client first has an AHardwareBuffer.
	AMediaCodec_releaseOutputBuffer(mc, idx, true);
}

static void on_format(AMediaCodec * mc, void * ud, AMediaFormat * fmt)
{
	(void)mc;
	(void)ud;
	fprintf(stderr, "[format] %s\n", AMediaFormat_toString(fmt));
	AMediaFormat_getInt32(fmt, "color-format", &C.cf);
	if (!AMediaFormat_getInt32(fmt, "stride", &C.stride))
		C.stride = 0;
	if (!AMediaFormat_getInt32(fmt, "slice-height", &C.slice_h))
		C.slice_h = 0;
	int32_t cl = 0, cr = 0, ct = 0, cb = 0;
	if (AMediaFormat_getInt32(fmt, "crop-right", &cr) && AMediaFormat_getInt32(fmt, "crop-left", &cl))
		C.crop_w = cr - cl + 1;
	if (AMediaFormat_getInt32(fmt, "crop-bottom", &cb) && AMediaFormat_getInt32(fmt, "crop-top", &ct))
		C.crop_h = cb - ct + 1;
	if (!C.crop_w)
		AMediaFormat_getInt32(fmt, "width", &C.crop_w);
	if (!C.crop_h)
		AMediaFormat_getInt32(fmt, "height", &C.crop_h);
	if (!C.stride)
		C.stride = C.crop_w;
	if (!C.slice_h)
		C.slice_h = C.crop_h;
	C.fmt_known = 1;
}

__attribute__((unused)) static void on_error(AMediaCodec * mc, void * ud, media_status_t e, int32_t a, const char * d)
{
	(void)mc;
	(void)ud;
	(void)a;
	fprintf(stderr, "[codec error] %d %s\n", e, d ? d : "");
	pthread_mutex_lock(&C.mu);
	C.err = 1;
	pthread_cond_broadcast(&C.cv);
	pthread_mutex_unlock(&C.mu);
}

// de-stride the decoder's ByteBuffer output into planar yuv420p on disk.
static void drain_bytebuffer(int32_t idx, AMediaCodecBufferInfo * bi)
{
	if (!C.dump || !C.fmt_known)
		return;
	size_t cap = 0;
	uint8_t * b = AMediaCodec_getOutputBuffer(C.codec, idx, &cap);
	if (!b)
		return;
	b += bi->offset;
	int w = C.crop_w, h = C.crop_h, st = C.stride, sh = C.slice_h;
	for (int y = 0; y < h; y++)
		fwrite(b + (size_t)y * st, 1, w, C.dump);
	uint8_t * uv = b + (size_t)st * sh;
	// COLOR_FormatYUV420SemiPlanar / flexible NV12: interleaved CbCr.
	// COLOR_FormatYUV420Planar: separate planes.
	int cw = w / 2, ch = h / 2;
	if (C.cf == 19 /* COLOR_FormatYUV420Planar */)
	{
		for (int y = 0; y < ch; y++)
			fwrite(uv + (size_t)y * (st / 2), 1, cw, C.dump);
		uint8_t * v = uv + (size_t)(st / 2) * (sh / 2);
		for (int y = 0; y < ch; y++)
			fwrite(v + (size_t)y * (st / 2), 1, cw, C.dump);
	}
	else
	{
		uint8_t * row = malloc(cw);
		for (int y = 0; y < ch; y++)
		{
			uint8_t * s = uv + (size_t)y * st;
			for (int x = 0; x < cw; x++)
				row[x] = s[2 * x];
			fwrite(row, 1, cw, C.dump);
		}
		for (int y = 0; y < ch; y++)
		{
			uint8_t * s = uv + (size_t)y * st;
			for (int x = 0; x < cw; x++)
				row[x] = s[2 * x + 1];
			fwrite(row, 1, cw, C.dump);
		}
		free(row);
	}
	C.dumped++;
}

// AImageReader callback: the instant a real client would have an AHardwareBuffer.
static void on_image(void * ud, AImageReader * r)
{
	(void)ud;
	AImage * img = NULL;
	if (AImageReader_acquireNextImage(r, &img) != AMEDIA_OK || !img)
		return;
	int64_t pts = 0;
	AImage_getTimestamp(img, &pts);
	AHardwareBuffer * ahb = NULL;
	AImage_getHardwareBuffer(img, &ahb); // what map_hardware_buffer() imports
	double t = now_ms();
	pthread_mutex_lock(&C.mu);
	int n = C.out_n;
	if (n < MAXF)
	{
		C.out_t[n] = t;
		C.out_idx[n] = (int32_t)(pts / 10000000); // pts is ns here
	}
	C.out_n++;
	pthread_cond_broadcast(&C.cv);
	pthread_mutex_unlock(&C.mu);
	AImage_delete(img); // the pool is 7 deep; a real client holds it longer
	(void)ahb;
}

// ---------------------------------------------------------------- drain thread

static volatile int g_stop;

static void * drain_thread(void * ud)
{
	(void)ud;
	while (!g_stop && !C.err)
	{
		AMediaCodecBufferInfo bi;
		ssize_t idx = AMediaCodec_dequeueOutputBuffer(C.codec, &bi, 2000);
		if (idx >= 0)
		{
			if (C.mode == 2)
			{
				drain_bytebuffer(idx, &bi);
				AMediaCodec_releaseOutputBuffer(C.codec, idx, false);
				double t = now_ms();
				pthread_mutex_lock(&C.mu);
				int n = C.out_n;
				if (n < MAXF)
				{
					C.out_t[n] = t;
					C.out_idx[n] = (int32_t)(bi.presentationTimeUs / 10000);
				}
				C.out_n++;
				pthread_cond_broadcast(&C.cv);
				pthread_mutex_unlock(&C.mu);
			}
			else
			{
				// render to the AImageReader; on_image() takes the timestamp,
				// which is where a real client first holds an AHardwareBuffer
				AMediaCodec_releaseOutputBuffer(C.codec, idx, true);
			}
		}
		else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
		{
			AMediaFormat * f = AMediaCodec_getOutputFormat(C.codec);
			on_format(C.codec, NULL, f);
			AMediaFormat_delete(f);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------- statistics

static int cmpd(const void * a, const void * b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

struct stat_s {
	double mean, p50, p95, p99, min, max;
	int n;
};

static struct stat_s stats(double * v, int n)
{
	struct stat_s s = {0};
	if (n <= 0)
		return s;
	double * t = malloc(n * sizeof *t);
	memcpy(t, v, n * sizeof *t);
	qsort(t, n, sizeof *t, cmpd);
	double sum = 0;
	for (int i = 0; i < n; i++)
		sum += t[i];
	s.n = n;
	s.mean = sum / n;
	s.p50 = t[n / 2];
	s.p95 = t[(int)(n * 0.95)];
	s.p99 = t[(int)(n * 0.99)];
	s.min = t[0];
	s.max = t[n - 1];
	free(t);
	return s;
}

// ---------------------------------------------------------------- main

static void usage(void)
{
	fprintf(stderr,
	        "nxhevcbench --stream S.hevc --index S.idx [--csd S.csd]\n"
	        "  [--mode latency|throughput|dump] [--frames N] [--warmup N]\n"
	        "  [--fps 90] [--low-latency] [--qti-low-latency]\n"
	        "  [--width W] [--height H] [--out FILE] [--json FILE] [--name TAG]\n");
	exit(1);
}

int main(int argc, char ** argv)
{
	const char *sp = NULL, *ip = NULL, *cp = NULL, *op = NULL, *jp = NULL, *tag = "run";
	int fps = 90, ll = 0, qll = 0;
	C.mode = 0;
	C.frames = 240;
	C.warmup = 48;
	C.w = 1088;
	C.h = 1088;

	for (int i = 1; i < argc; i++)
	{
#define ARG(s) (!strcmp(argv[i], s))
		if (ARG("--stream") && i + 1 < argc)
			sp = argv[++i];
		else if (ARG("--index") && i + 1 < argc)
			ip = argv[++i];
		else if (ARG("--csd") && i + 1 < argc)
			cp = argv[++i];
		else if (ARG("--out") && i + 1 < argc)
			op = argv[++i];
		else if (ARG("--json") && i + 1 < argc)
			jp = argv[++i];
		else if (ARG("--name") && i + 1 < argc)
			tag = argv[++i];
		else if (ARG("--frames") && i + 1 < argc)
			C.frames = atoi(argv[++i]);
		else if (ARG("--warmup") && i + 1 < argc)
			C.warmup = atoi(argv[++i]);
		else if (ARG("--fps") && i + 1 < argc)
			fps = atoi(argv[++i]);
		else if (ARG("--width") && i + 1 < argc)
			C.w = atoi(argv[++i]);
		else if (ARG("--height") && i + 1 < argc)
			C.h = atoi(argv[++i]);
		else if (ARG("--low-latency"))
			ll = 1;
		else if (ARG("--qti-low-latency"))
			qll = 1;
		else if (ARG("--mode") && i + 1 < argc)
		{
			i++;
			C.mode = !strcmp(argv[i], "throughput") ? 1 : !strcmp(argv[i], "dump") ? 2
			                                                                       : 0;
		}
		else
			usage();
#undef ARG
	}
	if (!sp || !ip)
		usage();

	g_stream = slurp(sp, &g_stream_sz);
	load_index(ip);
	if (cp)
		g_csd = slurp(cp, &g_csd_sz);
	if (!g_naus)
	{
		fprintf(stderr, "empty index\n");
		return 2;
	}
	fprintf(stderr, "[stream] %s  %zu bytes  %d AUs  mode=%d %dx%d\n",
	        sp, g_stream_sz, g_naus, C.mode, C.w, C.h);

	pthread_mutex_init(&C.mu, NULL);
	pthread_cond_init(&C.cv, NULL);

	ANativeWindow * win = NULL;
	if (C.mode != 2)
	{
		// exactly android_decoder.cpp:121-133
		media_status_t r = AImageReader_newWithUsage(
		        C.w, C.h, AIMAGE_FORMAT_PRIVATE,
		        AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
		                AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER |
		                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
		        7, &C.reader);
		if (r != AMEDIA_OK)
		{
			fprintf(stderr, "AImageReader_newWithUsage: %d\n", r);
			return 3;
		}
		AImageReader_ImageListener il = {NULL, on_image};
		AImageReader_setImageListener(C.reader, &il);
		if (AImageReader_getWindow(C.reader, &win) != AMEDIA_OK)
		{
			fprintf(stderr, "AImageReader_getWindow failed\n");
			return 3;
		}
	}

	C.codec = AMediaCodec_createDecoderByType("video/hevc");
	if (!C.codec)
	{
		fprintf(stderr, "createDecoderByType(video/hevc) failed\n");
		return 3;
	}
	char * cname = NULL;
	if (AMediaCodec_getName(C.codec, &cname) == AMEDIA_OK)
	{
		fprintf(stderr, "[codec] %s\n", cname);
	}

	AMediaFormat * fmt = AMediaFormat_new();
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, C.w);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, C.h);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, fps);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);
	if (ll)
		AMediaFormat_setInt32(fmt, "low-latency", 1);
	if (qll)
		AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
	if (C.mode == 2)
		AMediaFormat_setInt32(fmt, "color-format", 0x7f420888 /* COLOR_FormatYUV420Flexible */);
	if (g_csd)
		AMediaFormat_setBuffer(fmt, "csd-0", g_csd, g_csd_sz);

	// NOTE: AMediaCodec_setAsyncNotifyCallback is accepted by
	// OMX.qcom.video.decoder.hevc on this device but never delivers an
	// input-buffer callback (measured: 0 callbacks in 5 s). The whole harness
	// therefore uses the synchronous dequeue API, with a drain thread so that
	// throughput mode can keep the codec fed while pictures come out.

	media_status_t r = AMediaCodec_configure(C.codec, fmt, win, NULL, 0);
	if (r != AMEDIA_OK)
	{
		fprintf(stderr, "AMediaCodec_configure: %d\n", r);
		return 3;
	}
	if (C.mode == 2 && op)
	{
		C.dump = fopen(op, "wb");
		if (!C.dump)
		{
			fprintf(stderr, "open %s: %s\n", op, strerror(errno));
			return 2;
		}
	}
	if (AMediaCodec_start(C.codec) != AMEDIA_OK)
	{
		fprintf(stderr, "AMediaCodec_start failed\n");
		return 3;
	}

	pthread_t dt;
	pthread_create(&dt, NULL, drain_thread, NULL);

	int total = C.warmup + C.frames;
	static double lat[MAXF];
	double t_first_submit = 0, t_last_out = 0;

	for (int n = 0; n < total && !C.err; n++)
	{
		const struct au * a = &g_au[n % g_naus];

		// take an input buffer (synchronous, 5 s budget)
		ssize_t bidx = -1;
		for (int spin = 0; spin < 1000 && !C.err; spin++)
		{
			bidx = AMediaCodec_dequeueInputBuffer(C.codec, 5000);
			if (bidx >= 0)
				break;
		}
		if (bidx < 0)
		{
			fprintf(stderr, "no input buffer after 5 s at frame %d\n", n);
			C.err = 1;
			break;
		}

		size_t cap = 0;
		uint8_t * ib = AMediaCodec_getInputBuffer(C.codec, bidx, &cap);
		if (!ib || cap < a->size)
		{
			fprintf(stderr, "input buffer too small (%zu < %zu)\n", cap, a->size);
			C.err = 1;
			break;
		}
		memcpy(ib, g_stream + a->off, a->size);

		double ts = now_ms();
		if (n < MAXF)
			C.submit_t[n] = ts;
		if (n == C.warmup)
			t_first_submit = ts;
		AMediaCodec_queueInputBuffer(C.codec, bidx, 0, a->size,
		                             (uint64_t)n * 10000, // 10 ms/frame, as the client does
		                             0);

		if (C.mode == 0)
		{
			// one AU in flight: wait for its picture before feeding the next
			pthread_mutex_lock(&C.mu);
			while (C.out_n <= n && !C.err)
			{
				struct timespec to;
				clock_gettime(CLOCK_REALTIME, &to);
				to.tv_sec += 5;
				if (pthread_cond_timedwait(&C.cv, &C.mu, &to) == ETIMEDOUT)
					break;
			}
			int got = C.out_n;
			double ot = got > 0 && got - 1 < MAXF ? C.out_t[got - 1] : 0;
			pthread_mutex_unlock(&C.mu);
			if (got <= n)
			{
				fprintf(stderr, "no picture for AU %d within 5 s\n", n);
				C.err = 1;
				break;
			}
			if (n >= C.warmup)
				lat[n - C.warmup] = ot - ts;
			t_last_out = ot;
		}
	}

	if (C.mode != 0 && !C.err)
	{
		// drain
		pthread_mutex_lock(&C.mu);
		while (C.out_n < total && !C.err)
		{
			struct timespec to;
			clock_gettime(CLOCK_REALTIME, &to);
			to.tv_sec += 5;
			if (pthread_cond_timedwait(&C.cv, &C.mu, &to) == ETIMEDOUT)
				break;
		}
		int got = C.out_n;
		t_last_out = got > 0 && got - 1 < MAXF ? C.out_t[got - 1] : 0;
		pthread_mutex_unlock(&C.mu);
		fprintf(stderr, "[drain] %d/%d pictures out\n", got, total);
	}

	int nl = 0;
	struct stat_s S = {0};
	double fps_meas = 0, mean_interval = 0;

	if (C.mode == 0)
	{
		nl = C.frames;
		if (C.err)
			nl = 0;
		S = stats(lat, nl);
	}
	else
	{
		// steady-state output interval over the measured window
		static double iv[MAXF];
		int n = 0;
		for (int i = C.warmup + 1; i < C.out_n && i < MAXF; i++)
			iv[n++] = C.out_t[i] - C.out_t[i - 1];
		S = stats(iv, n);
		nl = n;
		mean_interval = S.mean;
		if (t_last_out > t_first_submit)
			fps_meas = (C.out_n - C.warmup) * 1e3 / (t_last_out - t_first_submit);
	}

	printf("== %s  mode=%s  %dx%d  stream=%s\n", tag,
	       C.mode == 0 ? "latency" : C.mode == 1 ? "throughput"
	                                             : "dump",
	       C.w, C.h, sp);
	printf("   codec=%s low-latency=%d qti-low-latency=%d frames=%d warmup=%d\n",
	       cname ? cname : "?", ll, qll, C.frames, C.warmup);
	if (C.mode == 2)
	{
		printf("   dumped=%d color-format=0x%x stride=%d slice-height=%d crop=%dx%d\n",
		       C.dumped, C.cf, C.stride, C.slice_h, C.crop_w, C.crop_h);
	}
	else if (C.mode == 0)
	{
		printf("   submit->AImage latency ms: mean %.3f  p50 %.3f  p95 %.3f  p99 %.3f  min %.3f  max %.3f  (n=%d)\n",
		       S.mean, S.p50, S.p95, S.p99, S.min, S.max, S.n);
	}
	else
	{
		printf("   output interval ms: mean %.3f  p50 %.3f  p95 %.3f  p99 %.3f  min %.3f  max %.3f  (n=%d)\n",
		       S.mean, S.p50, S.p95, S.p99, S.min, S.max, S.n);
		printf("   sustained %.1f fps  (= %.3f ms/frame of decoder occupancy)\n",
		       fps_meas, fps_meas > 0 ? 1e3 / fps_meas : 0);
	}

	if (jp)
	{
		FILE * j = fopen(jp, "w");
		if (j)
		{
			fprintf(j,
			        "{\"name\":\"%s\",\"mode\":%d,\"stream\":\"%s\",\"codec\":\"%s\","
			        "\"w\":%d,\"h\":%d,\"low_latency\":%d,\"qti_low_latency\":%d,"
			        "\"frames\":%d,\"n\":%d,\"mean\":%.4f,\"p50\":%.4f,\"p95\":%.4f,"
			        "\"p99\":%.4f,\"min\":%.4f,\"max\":%.4f,\"fps\":%.3f,"
			        "\"mean_interval\":%.4f,\"err\":%d,\"color_format\":%d,"
			        "\"stride\":%d,\"slice_height\":%d,\"dumped\":%d}\n",
			        tag, C.mode, sp, cname ? cname : "?", C.w, C.h, ll, qll,
			        C.frames, S.n, S.mean, S.p50, S.p95, S.p99, S.min, S.max,
			        fps_meas, mean_interval, C.err, C.cf, C.stride, C.slice_h, C.dumped);
			fclose(j);
		}
	}

	g_stop = 1;
	pthread_join(dt, NULL);
	if (C.dump)
		fclose(C.dump);
	AMediaCodec_stop(C.codec);
	AMediaCodec_delete(C.codec);
	if (C.reader)
		AImageReader_delete(C.reader);
	AMediaFormat_delete(fmt);
	return C.err ? 4 : 0;
}
