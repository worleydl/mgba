/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "libretro.h"

#include <mgba-util/common.h>

#include <mgba/core/blip_buf.h>
#include <mgba/core/cheats.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#ifdef M_CORE_GB
#include <mgba/gb/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/mbc.h>
#include <mgba/internal/gb/overrides.h>
#include <mgba/internal/gb/sio/tcp_lockstep.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#endif
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#ifndef __LIBRETRO__
#error "Can't compile the libretro core as anything other than libretro."
#endif

#ifdef _3DS
#include <3ds.h>
FS_Archive sdmcArchive;
#endif

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

#include "libretro_core_options.h"

#define GB_SAMPLES 512
#define SAMPLE_RATE 32768
/* An alpha factor of 1/180 is *somewhat* equivalent
 * to calculating the average for the last 180
 * frames, or 3 seconds of runtime... */
#define SAMPLES_PER_FRAME_MOVING_AVG_ALPHA (1.0f / 180.0f)
#define RUMBLE_PWM 35
#define EVENT_RATE 60

#define VIDEO_WIDTH_MAX  256
#define VIDEO_HEIGHT_MAX 224
#define VIDEO_BUFF_SIZE  (VIDEO_WIDTH_MAX * VIDEO_HEIGHT_MAX * sizeof(color_t))

static retro_environment_t environCallback;
static retro_video_refresh_t videoCallback;
static retro_audio_sample_batch_t audioCallback;
static retro_input_poll_t inputPollCallback;
static retro_input_state_t inputCallback;
static retro_log_printf_t logCallback;
static retro_set_rumble_state_t rumbleCallback;
static retro_sensor_get_input_t sensorGetCallback;
static retro_set_sensor_state_t sensorStateCallback;

static bool libretro_supports_bitmasks = false;

static void GBARetroLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args);

static void _postAudioBuffer(struct mAVStream*, blip_t* left, blip_t* right);
static void _setRumble(struct mRumble* rumble, int enable);
static uint8_t _readLux(struct GBALuminanceSource* lux);
static void _updateLux(struct GBALuminanceSource* lux);
static void _updateCamera(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch);
static void _startImage(struct mImageSource*, unsigned w, unsigned h, int colorFormats);
static void _stopImage(struct mImageSource*);
static void _requestImage(struct mImageSource*, const void** buffer, size_t* stride, enum mColorFormat* colorFormat);
static void _updateRotation(struct mRotationSource* source);
static int32_t _readTiltX(struct mRotationSource* source);
static int32_t _readTiltY(struct mRotationSource* source);
static int32_t _readGyroZ(struct mRotationSource* source);

static struct mCore* core;
static color_t* outputBuffer = NULL;
static int16_t *audioSampleBuffer = NULL;
static size_t audioSampleBufferSize;
static float audioSamplesPerFrameAvg;
static void* data;
static size_t dataSize;
static void* savedata;
static struct mAVStream stream;
static bool sensorsInitDone;
static bool rumbleInitDone;
static int rumbleUp;
static int rumbleDown;
static struct mRumble rumble;
static struct GBALuminanceSource lux;
static struct mRotationSource rotation;
static bool tiltEnabled;
static bool gyroEnabled;
static int luxLevelIndex;
static uint8_t luxLevel;
static bool luxSensorEnabled;
static bool luxSensorUsed;
static struct mLogger logger;
static struct retro_camera_callback cam;
static struct mImageSource imageSource;
static uint32_t* camData = NULL;
static unsigned camWidth;
static unsigned camHeight;
static unsigned imcapWidth;
static unsigned imcapHeight;
static size_t camStride;
static bool envVarsUpdated;
static unsigned frameskipType;
static unsigned frameskipThreshold;
static uint16_t frameskipCounter;
static bool retroAudioBuffActive;
static unsigned retroAudioBuffOccupancy;
static bool retroAudioBuffUnderrun;
static unsigned retroAudioLatency;
static bool updateAudioLatency;
static bool deferredSetup = false;
static bool useBitmasks = true;
static bool envVarsUpdated;
static int32_t tiltX = 0;
static int32_t tiltY = 0;
static int32_t gyroZ = 0;
static bool audioLowPassEnabled = false;
static int32_t audioLowPassRange = 0;
static int32_t audioLowPassLeftPrev = 0;
static int32_t audioLowPassRightPrev = 0;

static struct GBSIOSocket* sock;

static const int keymap[] = {
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_L,
};

#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
const char* const projectVersion = "0.10-dev" GIT_VERSION;
const char* const projectName = "mGBA";

/* Maximum number of consecutive frames that
 * can be skipped */
#define RETRO_FRAMESKIP_MAX 30

/* Frame skipping functions */

static void _retroAudioBuffStatusCallback(bool active, unsigned occupancy, bool underrunLikely) {
	retroAudioBuffActive    = active;
	retroAudioBuffOccupancy = occupancy;
	retroAudioBuffUnderrun  = underrunLikely;
}

static void _initFrameskip(void) {

	if (frameskipType > 0) {

		bool calculateAudioLatency = true;

		if (frameskipType == 3) { /* Fixed Interval */
			environCallback(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
		} else {

			struct retro_audio_buffer_status_callback BuffStatusCb;
			BuffStatusCb.callback = _retroAudioBuffStatusCallback;

			if (!environCallback(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &BuffStatusCb)) {

				if (logCallback)
					logCallback(RETRO_LOG_WARN, "Frameskip disabled - frontend does not support audio buffer status monitoring.\n");

				retroAudioBuffActive    = false;
				retroAudioBuffOccupancy = 0;
				retroAudioBuffUnderrun  = false;
				retroAudioLatency       = 0;
				calculateAudioLatency   = false;
			}
		}

		if (calculateAudioLatency) {

			/* Frameskip is enabled - increase frontend
			 * audio latency to minimise potential
			 * buffer underruns */
			float frameTimeMsec = 1000.0f * (float)core->frameCycles(core) /
					(float)core->frequency(core);

			/* Set latency to 6x current frame time... */
			retroAudioLatency = (unsigned)((6.0f * frameTimeMsec) + 0.5f);

			/* ...then round up to nearest multiple of 32 */
			retroAudioLatency = (retroAudioLatency + 0x1F) & ~0x1F;
		}

	} else {
		environCallback(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
		retroAudioLatency = 0;
	}

	updateAudioLatency = true;
}

static void _loadFrameskipSettings(struct mCoreOptions *opts) {

	struct retro_variable var;
	unsigned oldFrameskipType;
	unsigned frameskipInterval;

	var.key   = "mgba_frameskip";
	var.value = 0;

	oldFrameskipType = frameskipType;
	frameskipType    = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "auto") == 0) {
			frameskipType = 1;
		} else if (strcmp(var.value, "auto_threshold") == 0) {
			frameskipType = 2;
		} else if (strcmp(var.value, "fixed_interval") == 0) {
			frameskipType = 3;
		}
	}

	var.key   = "mgba_frameskip_threshold";
	var.value = 0;

	frameskipThreshold = 33;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		frameskipThreshold = strtol(var.value, NULL, 10);

	var.key   = "mgba_frameskip_interval";
	var.value = 0;

	frameskipInterval = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		frameskipInterval = strtol(var.value, NULL, 10);

	/* Update internal (mGBA config) frameskip value */
	if (opts) {
		opts->frameskip = (frameskipType == 3) ?
				frameskipInterval : 0;
	} else {
		mCoreConfigSetUIntValue(&core->config, "frameskip",
				(frameskipType == 3) ? frameskipInterval : 0);
		mCoreLoadConfig(core);
	}

	/* (Re)initialise frameskipping, if required */
	if (opts || (frameskipType != oldFrameskipType)) {
		_initFrameskip();
	}
}

/* Audio post processing */
static void _audioLowPassFilter(int16_t *buffer, int count) {

	int samples  = count;
	int16_t *out = buffer;

	/* Restore previous samples */
	int32_t audioLowPassLeft  = audioLowPassLeftPrev;
	int32_t audioLowPassRight = audioLowPassRightPrev;

	/* Single-pole low-pass filter (6 dB/octave) */
	int32_t factorA = audioLowPassRange;
	int32_t factorB = 0x10000 - factorA;

	do {
		/* Apply low-pass filter */
		audioLowPassLeft  = (audioLowPassLeft  * factorA) + (*out       * factorB);
		audioLowPassRight = (audioLowPassRight * factorA) + (*(out + 1) * factorB);

		/* 16.16 fixed point */
		audioLowPassLeft  >>= 16;
		audioLowPassRight >>= 16;

		/* Update sound buffer */
		*out++ = (int16_t) audioLowPassLeft;
		*out++ = (int16_t) audioLowPassRight;
	} while (--samples);

	/* Save last samples for next frame */
	audioLowPassLeftPrev  = audioLowPassLeft;
	audioLowPassRightPrev = audioLowPassRight;
}

static void _loadAudioLowPassFilterSettings(void) {

	struct retro_variable var;
	audioLowPassEnabled = false;
	audioLowPassRange = (60 * 0x10000) / 100;

	var.key = "mgba_audio_low_pass_filter";
	var.value = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "enabled") == 0) {
			audioLowPassEnabled = true;
		}
	}

	var.key = "mgba_audio_low_pass_range";
	var.value = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		audioLowPassRange = (strtol(var.value, NULL, 10) * 0x10000) / 100;
	}
}

/* Video post processing */
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)

/* Colour correction */
#define CC_TARGET_GAMMA   2.2f
#define CC_RGB_MAX        31.0f

/* > Note: GBC and GBA share almost identical
 *   colour space, but we maintain independent
 *   values in case of future adjustments */
#define GBC_CC_LUM        0.94f
#define GBC_CC_R          0.82f
#define GBC_CC_G          0.665f
#define GBC_CC_B          0.73f
#define GBC_CC_RG         0.125f
#define GBC_CC_RB         0.195f
#define GBC_CC_GR         0.24f
#define GBC_CC_GB         0.075f
#define GBC_CC_BR        -0.06f
#define GBC_CC_BG         0.21f
#define GBC_CC_GAMMA_ADJ -0.5f

#define GBA_CC_LUM        0.94f
#define GBA_CC_R          0.82f
#define GBA_CC_G          0.665f
#define GBA_CC_B          0.73f
#define GBA_CC_RG         0.125f
#define GBA_CC_RB         0.195f
#define GBA_CC_GR         0.24f
#define GBA_CC_GB         0.075f
#define GBA_CC_BR        -0.06f
#define GBA_CC_BG         0.21f
#define GBA_CC_GAMMA_ADJ  1.0f

static color_t* ccLUT              = NULL;
static unsigned ccType             = 0;
static bool colorCorrectionEnabled = false;

static void _initColorCorrection(void) {

	/* Constants */
	static const float displayGammaInv = 1.0f / CC_TARGET_GAMMA;
	static const float rgbMaxInv = 1.0f / CC_RGB_MAX;

	/* Variables */
	enum GBModel model = GB_MODEL_AUTODETECT;
	float ccLum;
	float ccR;
	float ccG;
	float ccB;
	float ccRG;
	float ccRB;
	float ccGR;
	float ccGB;
	float ccBR;
	float ccBG;
	float adjustedGamma;
	size_t color;

	/* Set colour correction parameters */
	colorCorrectionEnabled = false;
	switch (ccType) {
		case 1:
			model = GB_MODEL_AGB;
			break;
		case 2:
			model = GB_MODEL_CGB;
			break;
		case 3:
			{
				/* Autodetect
				 * (Note: This is somewhat clumsy due to the
				 *  M_CORE_GBA & M_CORE_GB defines... */
#ifdef M_CORE_GBA
				if (core->platform(core) == mPLATFORM_GBA) {
					model = GB_MODEL_AGB;
				}
#endif

#ifdef M_CORE_GB
				if (model != GB_MODEL_AGB) {
					if (core->platform(core) == mPLATFORM_GB) {

						const char* modelName = mCoreConfigGetValue(&core->config, "gb.model");
						struct GB* gb = core->board;

						if (modelName) {
							gb->model = GBNameToModel(modelName);
						} else {
							GBDetectModel(gb);
						}

						if (gb->model == GB_MODEL_CGB) {
							model = GB_MODEL_CGB;
						}
					}
				}
#endif
			}
			break;
		default:
			return;
	}

	switch (model) {
		case GB_MODEL_AGB:
			ccLum = GBA_CC_LUM;
			ccR   = GBA_CC_R;
			ccG   = GBA_CC_G;
			ccB   = GBA_CC_B;
			ccRG  = GBA_CC_RG;
			ccRB  = GBA_CC_RB;
			ccGR  = GBA_CC_GR;
			ccGB  = GBA_CC_GB;
			ccBR  = GBA_CC_BR;
			ccBG  = GBA_CC_BG;
			adjustedGamma = CC_TARGET_GAMMA + GBA_CC_GAMMA_ADJ;
			break;
		case GB_MODEL_CGB:
			ccLum = GBC_CC_LUM;
			ccR   = GBC_CC_R;
			ccG   = GBC_CC_G;
			ccB   = GBC_CC_B;
			ccRG  = GBC_CC_RG;
			ccRB  = GBC_CC_RB;
			ccGR  = GBC_CC_GR;
			ccGB  = GBC_CC_GB;
			ccBR  = GBC_CC_BR;
			ccBG  = GBC_CC_BG;
			adjustedGamma = CC_TARGET_GAMMA + GBC_CC_GAMMA_ADJ;
			break;
		default:
			return;
	}

	/* Allocate look-up table buffer, if required */
	if (!ccLUT) {
		size_t lutSize = 65536 * sizeof(color_t);
		ccLUT = malloc(lutSize);
		if (!ccLUT) {
			return;
		}
		memset(ccLUT, 0xFF, lutSize);
	}

	/* If we get this far, then colour correction is enabled... */
	colorCorrectionEnabled = true;

	/* Populate colour correction look-up table
	 * Note: This is somewhat slow (~100 ms on desktop),
	 * but using precompiled look-up tables would double
	 * the memory requirements, and make updating colour
	 * correction parameters an absolute nightmare...) */
	for (color = 0; color < 65536; color++) {
		unsigned rFinal = 0;
		unsigned gFinal = 0;
		unsigned bFinal = 0;
		/* Extract values from RGB565 input */
		const unsigned r = color >> 11 & 0x1F;
		const unsigned g = color >>  6 & 0x1F;
		const unsigned b = color       & 0x1F;
		/* Perform gamma expansion */
		float rFloat = pow((float)r * rgbMaxInv, adjustedGamma);
		float gFloat = pow((float)g * rgbMaxInv, adjustedGamma);
		float bFloat = pow((float)b * rgbMaxInv, adjustedGamma);
		/* Perform colour mangling */
		float rCorrect = ccLum * ((ccR  * rFloat) + (ccGR * gFloat) + (ccBR * bFloat));
		float gCorrect = ccLum * ((ccRG * rFloat) + (ccG  * gFloat) + (ccBG * bFloat));
		float bCorrect = ccLum * ((ccRB * rFloat) + (ccGB * gFloat) + (ccB  * bFloat));
		/* Range check... */
		rCorrect = rCorrect > 0.0f ? rCorrect : 0.0f;
		gCorrect = gCorrect > 0.0f ? gCorrect : 0.0f;
		bCorrect = bCorrect > 0.0f ? bCorrect : 0.0f;
		/* Perform gamma compression */
		rCorrect = pow(rCorrect, displayGammaInv);
		gCorrect = pow(gCorrect, displayGammaInv);
		bCorrect = pow(bCorrect, displayGammaInv);
		/* Range check... */
		rCorrect = rCorrect > 1.0f ? 1.0f : rCorrect;
		gCorrect = gCorrect > 1.0f ? 1.0f : gCorrect;
		bCorrect = bCorrect > 1.0f ? 1.0f : bCorrect;
		/* Convert back to RGB565 */
		rFinal = (unsigned)((rCorrect * CC_RGB_MAX) + 0.5f) & 0x1F;
		gFinal = (unsigned)((gCorrect * CC_RGB_MAX) + 0.5f) & 0x1F;
		bFinal = (unsigned)((bCorrect * CC_RGB_MAX) + 0.5f) & 0x1F;
		ccLUT[color] = rFinal << 11 | gFinal << 6 | bFinal;
	}
}

static void _loadColorCorrectionSettings(void) {
	struct retro_variable var;
	unsigned oldCcType = ccType;
	ccType = 0;

	var.key = "mgba_color_correction";
	var.value = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "GBA") == 0) {
			ccType = 1;
		} else if (strcmp(var.value, "GBC") == 0) {
			ccType = 2;
		} else if (strcmp(var.value, "Auto") == 0) {
			ccType = 3;
		}
	}

	if (ccType == 0) {
		colorCorrectionEnabled = false;
	} else if (ccType != oldCcType) {
		_initColorCorrection();
	}
}

/* Interframe blending */
#define LCD_RESPONSE_TIME 0.333f
/* > 'LCD Ghosting (Fast)' method does not
 *   correctly interpret the set response time,
 *   leading to an artificially subdued blur effect.
 *   We have to compensate for this by increasing
 *   the response time, hence this 'fake' value */
#define LCD_RESPONSE_TIME_FAKE 0.5f

enum frame_blend_method
{
   FRAME_BLEND_NONE = 0,
   FRAME_BLEND_MIX,
   FRAME_BLEND_MIX_SMART,
   FRAME_BLEND_LCD_GHOSTING,
   FRAME_BLEND_LCD_GHOSTING_FAST
};

static enum frame_blend_method frameBlendType = FRAME_BLEND_NONE;
static bool frameBlendEnabled                 = false;
static color_t* outputBufferPrev1             = NULL;
static color_t* outputBufferPrev2             = NULL;
static color_t* outputBufferPrev3             = NULL;
static color_t* outputBufferPrev4             = NULL;
static float* outputBufferAccR                = NULL;
static float* outputBufferAccG                = NULL;
static float* outputBufferAccB                = NULL;
static float frameBlendResponse[4]            = {0.0f};
static bool frameBlendResponseSet             = false;

static bool _allocateOutputBufferPrev(color_t** buf) {
	if (!*buf) {
		*buf = malloc(VIDEO_BUFF_SIZE);
		if (!*buf) {
			return false;
		}
	}
	memset(*buf, 0xFFFF, VIDEO_BUFF_SIZE);
	return true;
}

static bool _allocateOutputBufferAcc(void) {
	size_t i;
	size_t buf_size = VIDEO_WIDTH_MAX * VIDEO_HEIGHT_MAX * sizeof(float);

	if (!outputBufferAccR) {
		outputBufferAccR = malloc(buf_size);
		if (!outputBufferAccR) {
			return false;
		}
	}

	if (!outputBufferAccG) {
		outputBufferAccG = malloc(buf_size);
		if (!outputBufferAccG) {
			return false;
		}
	}

	if (!outputBufferAccB) {
		outputBufferAccB = malloc(buf_size);
		if (!outputBufferAccB) {
			return false;
		}
	}

	/* Cannot use memset() on arrays of floats... */
	for (i = 0; i < (VIDEO_WIDTH_MAX * VIDEO_HEIGHT_MAX); i++) {
		outputBufferAccR[i] = 1.0f;
		outputBufferAccG[i] = 1.0f;
		outputBufferAccB[i] = 1.0f;
	}
	return true;
}

static void _initFrameBlend(void) {

	frameBlendEnabled = false;

	/* Allocate interframe blending buffers, as required
	 * NOTE: In all cases, any used buffers are 'reset'
	 * (filled with 0xFF) to avoid drawing garbage on
	 * the next frame */
	switch (frameBlendType) {
		case FRAME_BLEND_MIX:
			/* Simple 50:50 blending requires a single buffer */
			if (!_allocateOutputBufferPrev(&outputBufferPrev1)) {
				return;
			}
			break;
		case FRAME_BLEND_MIX_SMART:
			/* Smart 50:50 blending requires three buffers */
			if (!_allocateOutputBufferPrev(&outputBufferPrev1)) {
				return;
			}
			if (!_allocateOutputBufferPrev(&outputBufferPrev2)) {
				return;
			}
			if (!_allocateOutputBufferPrev(&outputBufferPrev3)) {
				return;
			}
			break;
		case FRAME_BLEND_LCD_GHOSTING:
			/* 'Accurate' LCD ghosting requires four buffers */
			if (!_allocateOutputBufferPrev(&outputBufferPrev1)) {
				return;
			}
			if (!_allocateOutputBufferPrev(&outputBufferPrev2)) {
				return;
			}
			if (!_allocateOutputBufferPrev(&outputBufferPrev3)) {
				return;
			}
			if (!_allocateOutputBufferPrev(&outputBufferPrev4)) {
				return;
			}
			break;
		case FRAME_BLEND_LCD_GHOSTING_FAST:
			/* 'Fast' LCD ghosting requires three (RGB)
			 * 'accumulator' buffers */
			if (!_allocateOutputBufferAcc()) {
				return;
			}
			break;
		case FRAME_BLEND_NONE:
		default:
			/* Error condition - cannot happen
			 * > Just leave frameBlendEnabled set to false */
			return;
	}

	/* Set LCD ghosting response time factors,
	 * if required */
	if ((frameBlendType == FRAME_BLEND_LCD_GHOSTING) &&
	    !frameBlendResponseSet) {

		/* For the default response time of 0.333,
		 * only four previous samples are required
		 * since the response factor for the fifth
		 * is:
		 *    pow(LCD_RESPONSE_TIME, 5.0f) -> 0.00409
		 * ...which is less than half a percent, and
		 * therefore irrelevant.
		 * If the response time were significantly
		 * increased, we may need to rethink this
		 * (but more samples == greater performance
		 * overheads) */
		frameBlendResponse[0] = LCD_RESPONSE_TIME;
		frameBlendResponse[1] = pow(LCD_RESPONSE_TIME, 2.0f);
		frameBlendResponse[2] = pow(LCD_RESPONSE_TIME, 3.0f);
		frameBlendResponse[3] = pow(LCD_RESPONSE_TIME, 4.0f);

		frameBlendResponseSet = true;
	}

	/* If we get this far, then interframe blending is enabled... */
	frameBlendEnabled = true;
}

static void _loadFrameBlendSettings(void) {

	struct retro_variable var;
	enum frame_blend_method oldFrameBlendType = frameBlendType;
	frameBlendType = FRAME_BLEND_NONE;

	var.key = "mgba_interframe_blending";
	var.value = 0;

	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "mix") == 0) {
			frameBlendType = FRAME_BLEND_MIX;
		} else if (strcmp(var.value, "mix_smart") == 0) {
			frameBlendType = FRAME_BLEND_MIX_SMART;
		} else if (strcmp(var.value, "lcd_ghosting") == 0) {
			frameBlendType = FRAME_BLEND_LCD_GHOSTING;
		} else if (strcmp(var.value, "lcd_ghosting_fast") == 0) {
			frameBlendType = FRAME_BLEND_LCD_GHOSTING_FAST;
		}
	}

	if (frameBlendType == FRAME_BLEND_NONE) {
		frameBlendEnabled = false;
	} else if (frameBlendType != oldFrameBlendType) {
		_initFrameBlend();
	}
}

/* General post processing buffers/functions */
static color_t* ppOutputBuffer = NULL;

static void (*videoPostProcess)(unsigned width, unsigned height) = NULL;

/* > Note: The individual post processing functions
 *   are somewhat WET (Write Everything Twice), in that
 *   we duplicate the entire nested for loop.
 *   This code is performance-critical, so we want to
 *   minimise logic in the inner loops where possible  */
static void videoPostProcessCc(unsigned width, unsigned height) {

	color_t *src = outputBuffer;
	color_t *dst = ppOutputBuffer;
	size_t x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			*(dst + x) = *(ccLUT + *(src + x));
		}
		src += VIDEO_WIDTH_MAX;
		dst += VIDEO_WIDTH_MAX;
	}
}

static void videoPostProcessMix(unsigned width, unsigned height) {

	color_t *srcCurr = outputBuffer;
	color_t *srcPrev = outputBufferPrev1;
	color_t *dst     = ppOutputBuffer;
	size_t x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {

			/* Get colours from current + previous frames */
			color_t rgbCurr = *(srcCurr + x);
			color_t rgbPrev = *(srcPrev + x);

			/* Store colours for next frame */
			*(srcPrev + x)  = rgbCurr;

			/* Mix colours
			 * > "Mixing Packed RGB Pixels Efficiently"
			 *   http://blargg.8bitalley.com/info/rgb_mixing.html */
			color_t rgbMix  = (rgbCurr + rgbPrev + ((rgbCurr ^ rgbPrev) & 0x821)) >> 1;

			/* Assign colours for current frame */
			*(dst + x)      = colorCorrectionEnabled ?
					*(ccLUT + rgbMix) : rgbMix;
		}
		srcCurr += VIDEO_WIDTH_MAX;
		srcPrev += VIDEO_WIDTH_MAX;
		dst     += VIDEO_WIDTH_MAX;
	}
}

static void videoPostProcessMixSmart(unsigned width, unsigned height) {

	color_t *srcCurr  = outputBuffer;
	color_t *srcPrev1 = outputBufferPrev1;
	color_t *srcPrev2 = outputBufferPrev2;
	color_t *srcPrev3 = outputBufferPrev3;
	color_t *dst      = ppOutputBuffer;
	size_t x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {

			/* Get colours from current + previous frames */
			color_t rgbCurr  = *(srcCurr + x);
			color_t rgbPrev1 = *(srcPrev1 + x);
			color_t rgbPrev2 = *(srcPrev2 + x);
			color_t rgbPrev3 = *(srcPrev3 + x);

			/* Store colours for next frame */
			*(srcPrev1 + x) = rgbCurr;
			*(srcPrev2 + x) = rgbPrev1;
			*(srcPrev3 + x) = rgbPrev2;

			/* Determine whether mixing is required
			 * i.e. whether alternate frames have the same pixel colour,
			 * but adjacent frames do not */
			if (((rgbCurr == rgbPrev2) || (rgbPrev1 == rgbPrev3)) &&
				 (rgbCurr != rgbPrev1) &&
				 (rgbCurr != rgbPrev3) &&
				 (rgbPrev1 != rgbPrev2)) {

				/* Mix colours
				 * > "Mixing Packed RGB Pixels Efficiently"
				 *   http://blargg.8bitalley.com/info/rgb_mixing.html */
				color_t rgbMix = (rgbCurr + rgbPrev1 + ((rgbCurr ^ rgbPrev1) & 0x821)) >> 1;

				/* Assign colours for current frame */
				*(dst + x) = colorCorrectionEnabled ?
						*(ccLUT + rgbMix) : rgbMix;

			} else {
				/* Just use colours for current frame */
				*(dst + x) = colorCorrectionEnabled ?
						*(ccLUT + rgbCurr) :	rgbCurr;
			}
		}
		srcCurr  += VIDEO_WIDTH_MAX;
		srcPrev1 += VIDEO_WIDTH_MAX;
		srcPrev2 += VIDEO_WIDTH_MAX;
		srcPrev3 += VIDEO_WIDTH_MAX;
		dst      += VIDEO_WIDTH_MAX;
	}
}

static void videoPostProcessLcdGhost(unsigned width, unsigned height) {

	color_t *srcCurr  = outputBuffer;
	color_t *srcPrev1 = outputBufferPrev1;
	color_t *srcPrev2 = outputBufferPrev2;
	color_t *srcPrev3 = outputBufferPrev3;
	color_t *srcPrev4 = outputBufferPrev4;
	color_t *dst      = ppOutputBuffer;
	float *response   = frameBlendResponse;
	size_t x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {

			/* Get colours from current + previous frames */
			color_t rgbCurr  = *(srcCurr + x);
			color_t rgbPrev1 = *(srcPrev1 + x);
			color_t rgbPrev2 = *(srcPrev2 + x);
			color_t rgbPrev3 = *(srcPrev3 + x);
			color_t rgbPrev4 = *(srcPrev4 + x);

			/* Store colours for next frame */
			*(srcPrev1 + x) = rgbCurr;
			*(srcPrev2 + x) = rgbPrev1;
			*(srcPrev3 + x) = rgbPrev2;
			*(srcPrev4 + x) = rgbPrev3;

			/* Unpack colours and convert to float */
			float rCurr = (float)(rgbCurr >> 11 & 0x1F);
			float gCurr = (float)(rgbCurr >>  6 & 0x1F);
			float bCurr = (float)(rgbCurr       & 0x1F);

			float rPrev1 = (float)(rgbPrev1 >> 11 & 0x1F);
			float gPrev1 = (float)(rgbPrev1 >>  6 & 0x1F);
			float bPrev1 = (float)(rgbPrev1       & 0x1F);

			float rPrev2 = (float)(rgbPrev2 >> 11 & 0x1F);
			float gPrev2 = (float)(rgbPrev2 >>  6 & 0x1F);
			float bPrev2 = (float)(rgbPrev2       & 0x1F);

			float rPrev3 = (float)(rgbPrev3 >> 11 & 0x1F);
			float gPrev3 = (float)(rgbPrev3 >>  6 & 0x1F);
			float bPrev3 = (float)(rgbPrev3       & 0x1F);

			float rPrev4 = (float)(rgbPrev4 >> 11 & 0x1F);
			float gPrev4 = (float)(rgbPrev4 >>  6 & 0x1F);
			float bPrev4 = (float)(rgbPrev4       & 0x1F);

			/* Mix colours for current frame and convert back to color_t
			 * > Response time effect implemented via an exponential
			 *   drop-off algorithm, taken from the 'Gameboy Classic Shader'
			 *   by Harlequin:
			 *      https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/gameboy/shader-files/gb-pass0.glsl */
			rCurr += (rPrev1 - rCurr) * *response;
			rCurr += (rPrev2 - rCurr) * *(response + 1);
			rCurr += (rPrev3 - rCurr) * *(response + 2);
			rCurr += (rPrev4 - rCurr) * *(response + 3);
			color_t rMix = (color_t)(rCurr + 0.5f) & 0x1F;

			gCurr += (gPrev1 - gCurr) * *response;
			gCurr += (gPrev2 - gCurr) * *(response + 1);
			gCurr += (gPrev3 - gCurr) * *(response + 2);
			gCurr += (gPrev4 - gCurr) * *(response + 3);
			color_t gMix = (color_t)(gCurr + 0.5f) & 0x1F;

			bCurr += (bPrev1 - bCurr) * *response;
			bCurr += (bPrev2 - bCurr) * *(response + 1);
			bCurr += (bPrev3 - bCurr) * *(response + 2);
			bCurr += (bPrev4 - bCurr) * *(response + 3);
			color_t bMix = (color_t)(bCurr + 0.5f) & 0x1F;

			/* Repack colours for current frame */
			*(dst + x) = colorCorrectionEnabled ?
					*(ccLUT + (rMix << 11 | gMix << 6 | bMix)) :
							rMix << 11 | gMix << 6 | bMix;
		}
		srcCurr  += VIDEO_WIDTH_MAX;
		srcPrev1 += VIDEO_WIDTH_MAX;
		srcPrev2 += VIDEO_WIDTH_MAX;
		srcPrev3 += VIDEO_WIDTH_MAX;
		srcPrev4 += VIDEO_WIDTH_MAX;
		dst      += VIDEO_WIDTH_MAX;
	}
}

static void videoPostProcessLcdGhostFast(unsigned width, unsigned height) {

	color_t *srcCurr = outputBuffer;
	float *srcPrevR  = outputBufferAccR;
	float *srcPrevG  = outputBufferAccG;
	float *srcPrevB  = outputBufferAccB;
	color_t *dst     = ppOutputBuffer;
	size_t x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {

			/* Get colours from current + previous frames */
			color_t rgbCurr = *(srcCurr + x);
			float rPrev     = *(srcPrevR + x);
			float gPrev     = *(srcPrevG + x);
			float bPrev     = *(srcPrevB + x);

			/* Unpack current colours and convert to float */
			float rCurr = (float)(rgbCurr >> 11 & 0x1F);
			float gCurr = (float)(rgbCurr >>  6 & 0x1F);
			float bCurr = (float)(rgbCurr       & 0x1F);

			/* Mix colours for current frame */
			float rMix = (rCurr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * rPrev);
			float gMix = (gCurr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * gPrev);
			float bMix = (bCurr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * bPrev);

			/* Store colours for next frame */
			*(srcPrevR + x) = rMix;
			*(srcPrevG + x) = gMix;
			*(srcPrevB + x) = bMix;

			/* Convert and repack current frame colours */
			color_t rgbMix =   ((color_t)(rMix + 0.5f) & 0x1F) << 11
								  | ((color_t)(gMix + 0.5f) & 0x1F) << 6
								  | ((color_t)(bMix + 0.5f) & 0x1F);

			/* Assign colours for current frame */
			*(dst + x) = colorCorrectionEnabled ?
					*(ccLUT + rgbMix) : rgbMix;
		}
		srcCurr  += VIDEO_WIDTH_MAX;
		srcPrevR += VIDEO_WIDTH_MAX;
		srcPrevG += VIDEO_WIDTH_MAX;
		srcPrevB += VIDEO_WIDTH_MAX;
		dst      += VIDEO_WIDTH_MAX;
	}
}

static void _initPostProcessing(void) {

	/* Early return if all post processing elements
	 * are disabled */
	videoPostProcess = NULL;
	if (!colorCorrectionEnabled && !frameBlendEnabled) {
		return;
	}

	/* Allocate output buffer, if required */
	if (!ppOutputBuffer) {
#ifdef _3DS
		ppOutputBuffer = linearMemAlign(VIDEO_BUFF_SIZE, 0x80);
#else
		ppOutputBuffer = malloc(VIDEO_BUFF_SIZE);
#endif
		if (!ppOutputBuffer) {
			return;
		}
		memset(ppOutputBuffer, 0xFFFF, VIDEO_BUFF_SIZE);
	}

	/* Assign post processing function */
	if (frameBlendEnabled) {
		switch (frameBlendType) {
			case FRAME_BLEND_MIX:
				videoPostProcess = videoPostProcessMix;
				return;
			case FRAME_BLEND_MIX_SMART:
				videoPostProcess = videoPostProcessMixSmart;
				return;
			case FRAME_BLEND_LCD_GHOSTING:
				videoPostProcess = videoPostProcessLcdGhost;
				return;
			case FRAME_BLEND_LCD_GHOSTING_FAST:
				videoPostProcess = videoPostProcessLcdGhostFast;
				return;
			case FRAME_BLEND_NONE:
			default:
				/* Cannot happen */
				videoPostProcess = colorCorrectionEnabled ?
						videoPostProcessCc : NULL;
				return;
		}
	} else if (colorCorrectionEnabled) {
		videoPostProcess = videoPostProcessCc;
	}
}

static void _loadPostProcessingSettings(void) {

	/* Load settings and initialise individual
	 * post processing elements */
	_loadColorCorrectionSettings();
	_loadFrameBlendSettings();

	/* Initialise post processing buffers/functions
	 * based on configured options */
	_initPostProcessing();
}

static void _deinitPostProcessing(void) {

	ccType                 = 0;
	frameBlendType         = FRAME_BLEND_NONE;
	colorCorrectionEnabled = false;
	frameBlendEnabled      = false;
	videoPostProcess       = NULL;

	/* Free all allocated buffers */
	if (ppOutputBuffer) {
#ifdef _3DS
		linearFree(ppOutputBuffer);
#else
		free(ppOutputBuffer);
#endif
		ppOutputBuffer = NULL;
	}

	/* > Colour correction */
	if (ccLUT) {
		free(ccLUT);
		ccLUT = NULL;
	}

	/* > Interframe blending */
	if (outputBufferPrev1) {
		free(outputBufferPrev1);
		outputBufferPrev1 = NULL;
	}

	if (outputBufferPrev2) {
		free(outputBufferPrev2);
		outputBufferPrev2 = NULL;
	}

	if (outputBufferPrev3) {
		free(outputBufferPrev3);
		outputBufferPrev3 = NULL;
	}

	if (outputBufferPrev4) {
		free(outputBufferPrev4);
		outputBufferPrev4 = NULL;
	}

	if (outputBufferAccR) {
		free(outputBufferAccR);
		outputBufferAccR = NULL;
	}

	if (outputBufferAccG) {
		free(outputBufferAccG);
		outputBufferAccG = NULL;
	}

	if (outputBufferAccB) {
		free(outputBufferAccB);
		outputBufferAccB = NULL;
	}
}

#endif

static void _initSensors(void) {
	if (sensorsInitDone) {
		return;
	}

	struct retro_sensor_interface sensorInterface;
	if (environCallback(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensorInterface)) {
		sensorGetCallback = sensorInterface.get_sensor_input;
		sensorStateCallback = sensorInterface.set_sensor_state;

		if (sensorStateCallback && sensorGetCallback) {
			if (sensorStateCallback(0, RETRO_SENSOR_ACCELEROMETER_ENABLE, EVENT_RATE)) {
				tiltEnabled = true;
			}

			if (sensorStateCallback(0, RETRO_SENSOR_GYROSCOPE_ENABLE, EVENT_RATE)) {
				gyroEnabled = true;
			}

			if (sensorStateCallback(0, RETRO_SENSOR_ILLUMINANCE_ENABLE, EVENT_RATE)) {
				luxSensorEnabled = true;
			}
		}
	}

	sensorsInitDone = true;
}

static void _initRumble(void) {
	if (rumbleInitDone) {
		return;
	}

	struct retro_rumble_interface rumbleInterface;
	if (environCallback(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumbleInterface)) {
		rumbleCallback = rumbleInterface.set_rumble_state;
	}

	rumbleInitDone = true;
}

#ifdef M_CORE_GB
static void _updateGbPal(void) {
	struct retro_variable var;
	var.key = "mgba_gb_colors";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		const struct GBColorPreset* presets;
		size_t listSize = GBColorPresetList(&presets);
		size_t i;
		for (i = 0; i < listSize; ++i) {
			if (strcmp(presets[i].name, var.value) != 0) {
				continue;
			}
			mCoreConfigSetUIntValue(&core->config, "gb.pal[0]", presets[i].colors[0] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[1]", presets[i].colors[1] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[2]", presets[i].colors[2] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[3]", presets[i].colors[3] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[4]", presets[i].colors[4] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[5]", presets[i].colors[5] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[6]", presets[i].colors[6] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[7]", presets[i].colors[7] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[8]", presets[i].colors[8] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[9]", presets[i].colors[9] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[10]", presets[i].colors[10] & 0xFFFFFF);
			mCoreConfigSetUIntValue(&core->config, "gb.pal[11]", presets[i].colors[11] & 0xFFFFFF);
			core->reloadConfigOption(core, "gb.pal", NULL);
			break;
		}
	}
}
#endif

static void _reloadSettings(void) {
	struct mCoreOptions opts = {
		.useBios = true,
		.volume = 0x100,
	};

	struct retro_variable var;
#ifdef M_CORE_GB
	enum GBModel model;
	const char* modelName;

	var.key = "mgba_gb_model";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "Game Boy") == 0) {
			model = GB_MODEL_DMG;
		} else if (strcmp(var.value, "Super Game Boy") == 0) {
			model = GB_MODEL_SGB;
		} else if (strcmp(var.value, "Game Boy Color") == 0) {
			model = GB_MODEL_CGB;
		} else if (strcmp(var.value, "Game Boy Advance") == 0) {
			model = GB_MODEL_AGB;
		} else {
			model = GB_MODEL_AUTODETECT;
		}

		modelName = GBModelToName(model);
		mCoreConfigSetDefaultValue(&core->config, "gb.model", modelName);
		mCoreConfigSetDefaultValue(&core->config, "sgb.model", modelName);
		mCoreConfigSetDefaultValue(&core->config, "cgb.model", modelName);
	}

	var.key = "mgba_sgb_borders";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mCoreConfigSetDefaultIntValue(&core->config, "sgb.borders", strcmp(var.value, "ON") == 0);
	}

	var.key = "mgba_gb_colors_preset";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mCoreConfigSetDefaultIntValue(&core->config, "gb.colors", strtol(var.value, NULL, 10));
	}

	_updateGbPal();
#endif

	var.key = "mgba_use_bios";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		opts.useBios = strcmp(var.value, "ON") == 0;
	}

	var.key = "mgba_skip_bios";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		opts.skipBios = strcmp(var.value, "ON") == 0;
	}

#ifdef M_CORE_GB
	var.key = "mgba_sgb_borders";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mCoreConfigSetDefaultIntValue(&core->config, "sgb.borders", strcmp(var.value, "ON") == 0);
	}
#endif

	_loadFrameskipSettings(&opts);
	_loadAudioLowPassFilterSettings();

	var.key = "mgba_idle_optimization";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "Don't Remove") == 0) {
			mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
		} else if (strcmp(var.value, "Remove Known") == 0) {
			mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "remove");
		} else if (strcmp(var.value, "Detect and Remove") == 0) {
			mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
		}
	}

#ifdef M_CORE_GBA
	var.key = "mgba_force_gbp";
	var.value = 0;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mCoreConfigSetDefaultIntValue(&core->config, "gba.forceGbp", strcmp(var.value, "ON") == 0);
	}
#endif

	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);
}

static void _doDeferredSetup(void) {
	// Libretro API doesn't let you know when it's done copying data into the save buffers.
	// On the off-hand chance that a core actually expects its buffers to be populated when
	// you actually first get them, you're out of luck without workarounds. Yup, seriously.
	// Here's that workaround, but really the API needs to be thrown out and rewritten.
	struct VFile* save = VFileFromMemory(savedata, SIZE_CART_FLASH1M);
	if (!core->loadSave(core, save)) {
		save->close(save);
	}
	deferredSetup = false;
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t env)
{
	environCallback = env;

#ifdef M_CORE_GB
	const struct GBColorPreset* presets;
	size_t listSize = GBColorPresetList(&presets);

	size_t colorOpt;
	for (colorOpt = 0; option_defs_us[colorOpt].key; ++colorOpt) {
		if (strcmp(option_defs_us[colorOpt].key, "mgba_gb_colors") == 0) {
			break;
		}
	}
	size_t i;
	for (i = 0; i < listSize && i < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++i) {
		option_defs_us[colorOpt].values[i].value = presets[i].name;
	}
#endif

	bool categoriesSupported;
	libretro_set_core_options(environCallback, &categoriesSupported);
}

void retro_set_video_refresh(retro_video_refresh_t video) {
	videoCallback = video;
}

void retro_set_audio_sample(retro_audio_sample_t audio) {
	UNUSED(audio);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t audioBatch) {
	audioCallback = audioBatch;
}

void retro_set_input_poll(retro_input_poll_t inputPoll) {
	inputPollCallback = inputPoll;
}

void retro_set_input_state(retro_input_state_t input) {
	inputCallback = input;
}

void retro_get_system_info(struct retro_system_info* info) {
#ifdef GEKKO
	info->need_fullpath = true;
#else
	info->need_fullpath = false;
#endif
#ifdef M_CORE_GB
	info->valid_extensions = "gba|gb|gbc|sgb";
#else
	info->valid_extensions = "gba";
#endif
	info->library_version = projectVersion;
	info->library_name = projectName;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	unsigned width, height;
	core->desiredVideoDimensions(core, &width, &height);
	info->geometry.base_width = width;
	info->geometry.base_height = height;
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		info->geometry.max_width = VIDEO_WIDTH_MAX;
		info->geometry.max_height = VIDEO_HEIGHT_MAX;
	} else
#endif
	{
		info->geometry.max_width = width;
		info->geometry.max_height = height;
	}

	info->geometry.aspect_ratio = width / (double) height;
	info->timing.fps = core->frequency(core) / (float) core->frameCycles(core);
	info->timing.sample_rate = SAMPLE_RATE;
}

void retro_init(void) {
	enum retro_pixel_format fmt;
#ifdef COLOR_16_BIT
#if defined(COLOR_5_6_5) || defined(PS2)
	fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
#warning This pixel format is unsupported. Please use -DCOLOR_16-BIT -DCOLOR_5_6_5
	fmt = RETRO_PIXEL_FORMAT_0RGB1555;
#endif
#else
#warning This pixel format is unsupported. Please use -DCOLOR_16-BIT -DCOLOR_5_6_5
	fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#endif
	environCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	struct retro_input_descriptor inputDescriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Turbo R" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Turbo L" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Brighten Solar Sensor" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Darken Solar Sensor" },
		{ 0 }
	};
	environCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &inputDescriptors);

	useBitmasks = environCallback(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);

	// TODO: RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME when BIOS booting is supported

	rumbleInitDone = false;
	rumble.setRumble = _setRumble;
	rumbleCallback = 0;

	sensorsInitDone = false;
	sensorGetCallback = 0;
	sensorStateCallback = 0;

	tiltEnabled = false;
	gyroEnabled = false;
	rotation.sample = _updateRotation;
	rotation.readTiltX = _readTiltX;
	rotation.readTiltY = _readTiltY;
	rotation.readGyroZ = _readGyroZ;

	envVarsUpdated = true;
	luxSensorUsed = false;
	luxSensorEnabled = false;
	luxLevelIndex = 0;
	luxLevel = 0;
	lux.readLuminance = _readLux;
	lux.sample = _updateLux;
	_updateLux(&lux);

	struct retro_log_callback log;
	if (environCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logCallback = log.log;
	} else {
		logCallback = 0;
	}
	logger.log = GBARetroLog;
	mLogSetDefaultLogger(&logger);

	stream.videoDimensionsChanged = 0;
	stream.postAudioFrame = 0;
	stream.postAudioBuffer = _postAudioBuffer;
	stream.postVideoFrame = 0;

	imageSource.startRequestImage = _startImage;
	imageSource.stopRequestImage = _stopImage;
	imageSource.requestImage = _requestImage;

	if (environCallback(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
		libretro_supports_bitmasks = true;

	frameskipType           = 0;
	frameskipThreshold      = 0;
	frameskipCounter        = 0;
	retroAudioBuffActive    = false;
	retroAudioBuffOccupancy = 0;
	retroAudioBuffUnderrun  = false;
	retroAudioLatency       = 0;
	updateAudioLatency      = false;


}

void retro_deinit(void) {
	if (outputBuffer) {
#ifdef _3DS
		linearFree(outputBuffer);
#else
		free(outputBuffer);
#endif
		outputBuffer = NULL;
	}
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	_deinitPostProcessing();
#endif

	if (audioSampleBuffer) {
		free(audioSampleBuffer);
		audioSampleBuffer = NULL;
	}
	audioSampleBufferSize = 0;
	audioSamplesPerFrameAvg = 0.0f;

	if (sensorStateCallback) {
		sensorStateCallback(0, RETRO_SENSOR_ACCELEROMETER_DISABLE, EVENT_RATE);
		sensorStateCallback(0, RETRO_SENSOR_GYROSCOPE_DISABLE, EVENT_RATE);
		sensorStateCallback(0, RETRO_SENSOR_ILLUMINANCE_DISABLE, EVENT_RATE);
		sensorGetCallback = NULL;
		sensorStateCallback = NULL;
	}

	tiltEnabled = false;
	gyroEnabled = false;
	luxSensorEnabled = false;
	sensorsInitDone = false;
	useBitmasks = false;

	audioLowPassEnabled = false;
	audioLowPassRange = 0;
	audioLowPassLeftPrev = 0;
	audioLowPassRightPrev = 0;
}

static int turboclock = 0;
static bool indownstate = true;

int16_t cycleturbo(bool a, bool b, bool l, bool r) {
	int16_t buttons = 0;
	turboclock++;
	if (turboclock >= 2) {
		turboclock = 0;
		indownstate = !indownstate;
	}

	if (a) {
		buttons |= indownstate << 0;
	}

	if (b) {
		buttons |= indownstate << 1;
	}

	if (l) {
		buttons |= indownstate << 9;
	}

	if (r) {
		buttons |= indownstate << 8;
	}

	return buttons;
}

void retro_run(void) {
	if (deferredSetup) {
		_doDeferredSetup();
	}
	uint16_t keys;
	bool skipFrame = false;

	inputPollCallback();

	bool updated = false;
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		envVarsUpdated = true;

		struct retro_variable var = {
			.key = "mgba_allow_opposing_directions",
			.value = 0
		};
		if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
			mCoreConfigSetIntValue(&core->config, "allowOpposingDirections", strcmp(var.value, "yes") == 0);
			core->reloadConfigOption(core, "allowOpposingDirections", NULL);
		}

		_loadFrameskipSettings(NULL);
		_loadAudioLowPassFilterSettings();

#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
		_loadPostProcessingSettings();
#endif
#ifdef M_CORE_GB
		_updateGbPal();
#endif
	}

	keys = 0;
	int i;
	if (useBitmasks) {
		int16_t joypadMask = inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		for (i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
			keys |= ((joypadMask >> keymap[i]) & 1) << i;
		}
		// XXX: turbo keys, should be moved to frontend
#define JOYPAD_BIT(BUTTON) (1 << RETRO_DEVICE_ID_JOYPAD_ ## BUTTON)
		keys |= cycleturbo(joypadMask & JOYPAD_BIT(X), joypadMask & JOYPAD_BIT(Y), joypadMask & JOYPAD_BIT(L2), joypadMask & JOYPAD_BIT(R2));
#undef JOYPAD_BIT
	} else {
		for (i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
			keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, keymap[i])) << i;
		}
		// XXX: turbo keys, should be moved to frontend
		keys |= cycleturbo(
			inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X),
			inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y),
			inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2),
			inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2)
		);
	}

	core->setKeys(core, keys);

	if (!luxSensorUsed) {
		static bool wasAdjustingLux = false;
		if (wasAdjustingLux) {
			wasAdjustingLux = inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) ||
			                  inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
		} else {
			if (inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3)) {
				++luxLevelIndex;
				if (luxLevelIndex > 10) {
					luxLevelIndex = 10;
				}
				wasAdjustingLux = true;
			} else if (inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3)) {
				--luxLevelIndex;
				if (luxLevelIndex < 0) {
					luxLevelIndex = 0;
				}
				wasAdjustingLux = true;
			}
		}
	}

	/* Check whether current frame should
	 * be skipped */
	if ((frameskipType > 0)  &&
		 (frameskipType != 3) && /* Ignore 'Fixed Interval' - handled internally */
		 retroAudioBuffActive) {

		switch (frameskipType) {
			case 1: /* Auto */
				skipFrame = retroAudioBuffUnderrun;
				break;
			case 2: /* Auto (Threshold) */
				skipFrame = (retroAudioBuffOccupancy < frameskipThreshold);
				break;
			default:
				skipFrame = false;
				break;
		}

		if (skipFrame) {
			if(frameskipCounter < RETRO_FRAMESKIP_MAX) {

				switch (core->platform(core)) {
#ifdef M_CORE_GBA
				case mPLATFORM_GBA:
					((struct GBA*) core->board)->video.frameskipCounter = 1;
					break;
#endif
#ifdef M_CORE_GB
				case mPLATFORM_GB:
					((struct GB*) core->board)->video.frameskipCounter = 1;
					break;
#endif
				default:
					break;
				}
				frameskipCounter++;

			} else {
				frameskipCounter = 0;
				skipFrame        = false;
			}
		} else {
			frameskipCounter = 0;
		}
	}

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (updateAudioLatency)
   {
      environCallback(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &retroAudioLatency);
      updateAudioLatency = false;
   }

	core->runFrame(core);
	unsigned width, height;
	core->desiredVideoDimensions(core, &width, &height);

	/* If using 'Fixed Interval' frameskipping, check
	 * whether a frame is currently available  */
	if (frameskipType == 3) {
		switch (core->platform(core)) {
	#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			skipFrame = ((struct GBA*) core->board)->video.frameskipCounter > 0;
			break;
	#endif
	#ifdef M_CORE_GB
		case mPLATFORM_GB:
			skipFrame = ((struct GB*) core->board)->video.frameskipCounter > 0;
			break;
	#endif
		default:
			break;
		}
	}

	if (!skipFrame) {
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
		if (videoPostProcess) {
			videoPostProcess(width, height);
			videoCallback(ppOutputBuffer, width, height, VIDEO_WIDTH_MAX * sizeof(color_t));
		} else
#endif
			videoCallback(outputBuffer, width, height, VIDEO_WIDTH_MAX * sizeof(color_t));
	} else {
		videoCallback(NULL, width, height, VIDEO_WIDTH_MAX * sizeof(color_t));
	}

#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		blip_t *audioChannelLeft  = core->getAudioChannel(core, 0);
		blip_t *audioChannelRight = core->getAudioChannel(core, 1);
		int samplesAvail          = blip_samples_avail(audioChannelLeft);
		if (samplesAvail > 0) {
			/* Update 'running average' of number of
			 * samples per frame.
			 * Note that this is not a true running
			 * average, but just a leaky-integrator/
			 * exponential moving average, used because
			 * it is simple and fast (i.e. requires no
			 * window of samples). */
			audioSamplesPerFrameAvg = (SAMPLES_PER_FRAME_MOVING_AVG_ALPHA * (float)samplesAvail) +
					((1.0f - SAMPLES_PER_FRAME_MOVING_AVG_ALPHA) * audioSamplesPerFrameAvg);
			size_t samplesToRead = (size_t)(audioSamplesPerFrameAvg);
			/* Resize audio output buffer, if required */
			if (audioSampleBufferSize < (samplesToRead * 2)) {
				audioSampleBufferSize = (samplesToRead * 2);
				audioSampleBuffer     = realloc(audioSampleBuffer, audioSampleBufferSize * sizeof(int16_t));
			}
			int produced = blip_read_samples(audioChannelLeft, audioSampleBuffer, samplesToRead, true);
			blip_read_samples(audioChannelRight, audioSampleBuffer + 1, samplesToRead, true);
			if (produced > 0) {
				if (audioLowPassEnabled) {
					_audioLowPassFilter(audioSampleBuffer, produced);
				}
				audioCallback(audioSampleBuffer, (size_t)produced);
			}
		}
	}
#endif

	if (rumbleCallback) {
		if (rumbleUp) {
			rumbleCallback(0, RETRO_RUMBLE_STRONG, rumbleUp * 0xFFFF / (rumbleUp + rumbleDown));
			rumbleCallback(0, RETRO_RUMBLE_WEAK, rumbleUp * 0xFFFF / (rumbleUp + rumbleDown));
		} else {
			rumbleCallback(0, RETRO_RUMBLE_STRONG, 0);
			rumbleCallback(0, RETRO_RUMBLE_WEAK, 0);
		}
		rumbleUp = 0;
		rumbleDown = 0;
	}
}

static void _setupMaps(struct mCore* core) {
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		struct GBA* gba = core->board;
		struct retro_memory_descriptor descs[11];
		struct retro_memory_map mmaps;
		size_t romSize = gba->memory.romSize + (gba->memory.romSize & 1);

		memset(descs, 0, sizeof(descs));
		size_t savedataSize = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

		/* Map internal working RAM */
		descs[0].ptr    = gba->memory.iwram;
		descs[0].start  = BASE_WORKING_IRAM;
		descs[0].len    = SIZE_WORKING_IRAM;
		descs[0].select = 0xFF000000;

		/* Map working RAM */
		descs[1].ptr    = gba->memory.wram;
		descs[1].start  = BASE_WORKING_RAM;
		descs[1].len    = SIZE_WORKING_RAM;
		descs[1].select = 0xFF000000;

		/* Map save RAM */
		/* TODO: if SRAM is flash, use start=0 addrspace="S" instead */
		descs[2].ptr    = savedataSize ? savedata : NULL;
		descs[2].start  = BASE_CART_SRAM;
		descs[2].len    = savedataSize;

		/* Map ROM */
		descs[3].ptr    = gba->memory.rom;
		descs[3].start  = BASE_CART0;
		descs[3].len    = romSize;
		descs[3].flags  = RETRO_MEMDESC_CONST;

		descs[4].ptr    = gba->memory.rom;
		descs[4].start  = BASE_CART1;
		descs[4].len    = romSize;
		descs[4].flags  = RETRO_MEMDESC_CONST;

		descs[5].ptr    = gba->memory.rom;
		descs[5].start  = BASE_CART2;
		descs[5].len    = romSize;
		descs[5].flags  = RETRO_MEMDESC_CONST;

		/* Map BIOS */
		descs[6].ptr    = gba->memory.bios;
		descs[6].start  = BASE_BIOS;
		descs[6].len    = SIZE_BIOS;
		descs[6].flags  = RETRO_MEMDESC_CONST;

		/* Map VRAM */
		descs[7].ptr    = gba->video.vram;
		descs[7].start  = BASE_VRAM;
		descs[7].len    = SIZE_VRAM;
		descs[7].select = 0xFF000000;

		/* Map palette RAM */
		descs[8].ptr    = gba->video.palette;
		descs[8].start  = BASE_PALETTE_RAM;
		descs[8].len    = SIZE_PALETTE_RAM;
		descs[8].select = 0xFF000000;

		/* Map OAM */
		descs[9].ptr    = &gba->video.oam; /* video.oam is a structure */
		descs[9].start  = BASE_OAM;
		descs[9].len    = SIZE_OAM;
		descs[9].select = 0xFF000000;

		/* Map mmapped I/O */
		descs[10].ptr    = gba->memory.io;
		descs[10].start  = BASE_IO;
		descs[10].len    = SIZE_IO;

		mmaps.descriptors = descs;
		mmaps.num_descriptors = sizeof(descs) / sizeof(descs[0]);

		bool yes = true;
		environCallback(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
		environCallback(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
	}
#endif
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		struct GB* gb = core->board;
		struct retro_memory_descriptor descs[11];
		struct retro_memory_map mmaps;

		memset(descs, 0, sizeof(descs));
		size_t savedataSize = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

		unsigned i = 0;

		/* Map ROM */
		descs[i].ptr    = gb->memory.rom;
		descs[i].start  = GB_BASE_CART_BANK0;
		descs[i].len    = GB_SIZE_CART_BANK0;
		descs[i].flags  = RETRO_MEMDESC_CONST;
		i++;

		descs[i].ptr    = gb->memory.rom;
		descs[i].offset = GB_SIZE_CART_BANK0;
		descs[i].start  = GB_BASE_CART_BANK1;
		descs[i].len    = GB_SIZE_CART_BANK0;
		descs[i].flags  = RETRO_MEMDESC_CONST;
		i++;

		/* Map VRAM */
		descs[i].ptr    = gb->video.vram;
		descs[i].start  = GB_BASE_VRAM;
		descs[i].len    = GB_SIZE_VRAM_BANK0;
		i++;

		/* Map working RAM */
		descs[i].ptr    = gb->memory.wram;
		descs[i].start  = GB_BASE_WORKING_RAM_BANK0;
		descs[i].len    = GB_SIZE_WORKING_RAM_BANK0;
		i++;

		descs[i].ptr    = gb->memory.wram;
		descs[i].offset = GB_SIZE_WORKING_RAM_BANK0;
		descs[i].start  = GB_BASE_WORKING_RAM_BANK1;
		descs[i].len    = GB_SIZE_WORKING_RAM_BANK0;
		i++;

		/* Map OAM */
		descs[i].ptr    = &gb->video.oam; /* video.oam is a structure */
		descs[i].start  = GB_BASE_OAM;
		descs[i].len    = GB_SIZE_OAM;
		descs[i].select = 0xFFFFFF60;
		i++;

		/* Map mmapped I/O */
		descs[i].ptr    = gb->memory.io;
		descs[i].start  = GB_BASE_IO;
		descs[i].len    = GB_SIZE_IO;
		i++;

		/* Map High RAM */
		descs[i].ptr    = gb->memory.hram;
		descs[i].start  = GB_BASE_HRAM;
		descs[i].len    = GB_SIZE_HRAM;
		descs[i].select = 0xFFFFFF80;
		i++;

		/* Map IE Register */
		descs[i].ptr    = &gb->memory.ie;
		descs[i].start  = GB_BASE_IE;
		descs[i].len    = 1;
		i++;

		/* Map External RAM */
		if (savedataSize) {
			descs[i].ptr    = savedata;
			descs[i].start  = GB_BASE_EXTERNAL_RAM;
			descs[i].len    = savedataSize;
			i++;
		}

		if (gb->model >= GB_MODEL_CGB) {
			/* Map working RAM */
			/* banks 2-7 of wram mapped in virtual address so it can be
			 * accessed without bank switching, GBC only */
			descs[i].ptr    = gb->memory.wram + 0x2000;
			descs[i].start  = 0x10000;
			descs[i].len    = GB_SIZE_WORKING_RAM - 0x2000;
			descs[i].select = 0xFFFFA000;
			i++;
		}

		mmaps.descriptors = descs;
		mmaps.num_descriptors = i;

		bool yes = true;
		environCallback(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
		environCallback(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
	}
#endif
}

void retro_reset(void) {
	core->reset(core);
	_setupMaps(core);

	rumbleUp = 0;
	rumbleDown = 0;
}

#ifdef GEKKO
static size_t _readRomFile(const char *path, void **buf) {
	size_t rc;
	long len;
	FILE *file = fopen(path, "rb");

	if (!file) {
		goto error;
	}

	fseek(file, 0, SEEK_END);
	len = ftell(file);
	rewind(file);
	*buf = anonymousMemoryMap(len);
	if (!*buf) {
		goto error;
	}

	if ((rc = fread(*buf, 1, len, file)) < len) {
		goto error;
	}

	fclose(file);
	return rc;

error:
	if (file) {
		fclose(file);
	}
	mappedMemoryFree(*buf, len);
	*buf = NULL;
	return -1;
}
#endif

bool retro_load_game(const struct retro_game_info* game) {
	struct VFile* rom;

	if (!game) {
		return false;
	}

	if (game->data) {
		data = anonymousMemoryMap(game->size);
		dataSize = game->size;
		memcpy(data, game->data, game->size);
		rom = VFileFromMemory(data, game->size);
	} else {
#ifdef GEKKO
		if ((dataSize = _readRomFile(game->path, &data)) == -1) {
			return false;
		}
		rom = VFileFromMemory(data, dataSize);
#else
		data = 0;
		rom = VFileOpen(game->path, O_RDONLY);
#endif
	}
	if (!rom) {
		return false;
	}

	core = mCoreFindVF(rom);
	if (!core) {
		rom->close(rom);
		mappedMemoryFree(data, game->size);
		return false;
	}
	mCoreInitConfig(core, NULL);
	core->init(core);

#ifdef _3DS
	outputBuffer = linearMemAlign(VIDEO_BUFF_SIZE, 0x80);
#else
	outputBuffer = malloc(VIDEO_BUFF_SIZE);
#endif
	memset(outputBuffer, 0xFFFF, VIDEO_BUFF_SIZE);
	core->setVideoBuffer(core, outputBuffer, VIDEO_WIDTH_MAX);

#ifdef M_CORE_GBA
	/* GBA emulation produces a fairly regular number
	 * of audio samples per frame that is consistent
	 * with the set sample rate. We therefore consume
	 * audio samples in retro_run() to achieve the
	 * best possible frame pacing */
	if (core->platform(core) == mPLATFORM_GBA) {
		/* Set initial output audio buffer size
		 * to nominal number of samples per frame.
		 * Buffer will be resized as required in
		 * retro_run(). */
		size_t audioSamplesPerFrame = (size_t)((float)SAMPLE_RATE * (float)core->frameCycles(core) /
				(float)core->frequency(core));
		audioSampleBufferSize       = audioSamplesPerFrame * 2;
		audioSampleBuffer           = malloc(audioSampleBufferSize * sizeof(int16_t));
		audioSamplesPerFrameAvg     = (float)audioSamplesPerFrame;
		/* Internal audio buffer size should be
		 * audioSamplesPerFrame, but number of samples
		 * actually generated varies slightly on a
		 * frame-by-frame basis. We therefore allow
		 * for some wriggle room by setting double
		 * what we need (accounting for the hard
		 * coded blip buffer limit of 0x4000). */
		size_t internalAudioBufferSize = audioSamplesPerFrame * 2;
		if (internalAudioBufferSize > 0x4000) {
			internalAudioBufferSize = 0x4000;
		}
		core->setAudioBufferSize(core, internalAudioBufferSize);
	} else
#endif
	{
		/* GB/GBC emulation does not produce a number
		 * of samples per frame that is consistent with
		 * the set sample rate, and so it is unclear how
		 * best to handle this. We therefore fallback to
		 * using the regular stream-set _postAudioBuffer()
		 * callback with a fixed buffer size, which seems
		 * (historically) to produce adequate results */
		core->setAVStream(core, &stream);
		audioSampleBufferSize   = GB_SAMPLES * 2;
		audioSampleBuffer       = malloc(audioSampleBufferSize * sizeof(int16_t));
		audioSamplesPerFrameAvg = GB_SAMPLES;
		core->setAudioBufferSize(core, GB_SAMPLES);
	}

	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);

	core->setPeripheral(core, mPERIPH_RUMBLE, &rumble);
	core->setPeripheral(core, mPERIPH_ROTATION, &rotation);

	savedata = anonymousMemoryMap(SIZE_CART_FLASH1M);
	memset(savedata, 0xFF, SIZE_CART_FLASH1M);

	_reloadSettings();
	core->loadROM(core, rom);
	deferredSetup = true;

	const char* sysDir = 0;
	const char* biosName = 0;
	char biosPath[PATH_MAX];
	environCallback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysDir);

#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		core->setPeripheral(core, mPERIPH_GBA_LUMINANCE, &lux);
		biosName = "gba_bios.bin";
	}
#endif

#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		memset(&cam, 0, sizeof(cam));
		cam.height = GBCAM_HEIGHT;
		cam.width = GBCAM_WIDTH;
		cam.caps = 1 << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
		cam.frame_raw_framebuffer = _updateCamera;
		if (environCallback(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE, &cam)) {
			core->setPeripheral(core, mPERIPH_IMAGE_SOURCE, &imageSource);
		}

		const char* modelName = mCoreConfigGetValue(&core->config, "gb.model");
		struct GB* gb = core->board;

		if (modelName) {
			gb->model = GBNameToModel(modelName);
		} else {
			GBDetectModel(gb);
		}

		switch (gb->model) {
		case GB_MODEL_AGB:
		case GB_MODEL_CGB:
			biosName = "gbc_bios.bin";
			break;
		case GB_MODEL_SGB:
			biosName = "sgb_bios.bin";
			break;
		case GB_MODEL_DMG:
		default:
			biosName = "gb_bios.bin";
			break;
		}
	}
#endif

	if (core->opts.useBios && sysDir && biosName) {
		snprintf(biosPath, sizeof(biosPath), "%s%s%s", sysDir, PATH_SEP, biosName);
		struct VFile* bios = VFileOpen(biosPath, O_RDONLY);
		if (bios) {
			core->loadBIOS(core, bios, 0);
		}
	}

	core->reset(core);

	// Register TCP SIO driver
	struct retro_variable var = {
		.key = "mgba_link_server",
		.value = 0
	};
	if (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		core->opts.linkServer = strcmp(var.value, "ON") == 0;
	}


	#ifdef M_CORE_GB
	struct GB* gb = (struct GB*) core->board;
	sock = malloc(sizeof(struct GBSIOSocket));
	GBSIOSocketCreate(sock);
	GBSIOSocketConnect(sock, core->opts.linkServer);
	GBSIOSetDriver(&gb->sio, &sock->d);

	#endif

	_setupMaps(core);

#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	_loadPostProcessingSettings();
#endif

	return true;
}

void retro_unload_game(void) {
	if (!core) {
		return;
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	mappedMemoryFree(data, dataSize);
	data = 0;
	mappedMemoryFree(savedata, SIZE_CART_FLASH1M);
	savedata = 0;
}

size_t retro_serialize_size(void) {
	if (deferredSetup) {
		_doDeferredSetup();
	}
	struct VFile* vfm = VFileMemChunk(NULL, 0);
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	size_t size = vfm->size(vfm);
	vfm->close(vfm);
	return size;
}

bool retro_serialize(void* data, size_t size) {
	if (deferredSetup) {
		_doDeferredSetup();
	}
	struct VFile* vfm = VFileMemChunk(NULL, 0);
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	if ((ssize_t) size > vfm->size(vfm)) {
		size = vfm->size(vfm);
	} else if ((ssize_t) size < vfm->size(vfm)) {
		vfm->close(vfm);
		return false;
	}
	vfm->seek(vfm, 0, SEEK_SET);
	vfm->read(vfm, data, size);
	vfm->close(vfm);
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	if (deferredSetup) {
		_doDeferredSetup();
	}
	struct VFile* vfm = VFileFromConstMemory(data, size);
	bool success = mCoreLoadStateNamed(core, vfm, SAVESTATE_RTC);
	vfm->close(vfm);
	return success;
}

void retro_cheat_reset(void) {
	mCheatDeviceClear(core->cheatDevice(core));
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	UNUSED(index);
	UNUSED(enabled);
	struct mCheatDevice* device = core->cheatDevice(core);
	struct mCheatSet* cheatSet = NULL;
	if (mCheatSetsSize(&device->cheats)) {
		cheatSet = *mCheatSetsGetPointer(&device->cheats, 0);
	} else {
		cheatSet = device->createSet(device, NULL);
		mCheatAddSet(device, cheatSet);
	}
// Convert the super wonky unportable libretro format to something normal
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		char realCode[] = "XXXXXXXX XXXXXXXX";
		size_t len = strlen(code) + 1; // Include null terminator
		size_t i, pos;
		for (i = 0, pos = 0; i < len; ++i) {
			if (isspace((int) code[i]) || code[i] == '+') {
				realCode[pos] = ' ';
			} else {
				realCode[pos] = code[i];
			}
			if ((pos == 13 && (realCode[pos] == ' ' || !realCode[pos])) || pos == 17) {
				realCode[pos] = '\0';
				mCheatAddLine(cheatSet, realCode, 0);
				pos = 0;
				continue;
			}
			++pos;
		}
	}
#endif
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		char realCode[] = "XXX-XXX-XXX";
		size_t len = strlen(code) + 1; // Include null terminator
		size_t i, pos;
		for (i = 0, pos = 0; i < len; ++i) {
			if (isspace((int) code[i]) || code[i] == '+') {
				realCode[pos] = '\0';
			} else {
				realCode[pos] = code[i];
			}

			if (pos == 11 || !realCode[pos]) {
				realCode[pos] = '\0';
				mCheatAddLine(cheatSet, realCode, 0);
				pos = 0;
				continue;
			}
			++pos;
		}
	}
#endif
	if (cheatSet->refresh) {
		cheatSet->refresh(cheatSet, device);
	}
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC; // TODO: This isn't strictly true
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	UNUSED(port);
	UNUSED(device);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	UNUSED(game_type);
	UNUSED(info);
	UNUSED(num_info);
	return false;
}

void* retro_get_memory_data(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return savedata;
	case RETRO_MEMORY_RTC:
		switch (core->platform(core)) {
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			switch (((struct GB*) core->board)->memory.mbcType) {
			case GB_MBC3_RTC:
				return &((uint8_t*) savedata)[((struct GB*) core->board)->sramSize];
			default:
				break;
			}
#endif
		default:
			break;
		}
	case RETRO_MEMORY_SYSTEM_RAM:
		switch (core->platform(core)) {
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			return ((struct GB*)core->board)->memory.wram;
#endif
#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			return ((struct GBA*)core->board)->memory.wram;
#endif
		default:
			break;
		}
		break;
	case RETRO_MEMORY_VIDEO_RAM:
		switch (core->platform(core)) {
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			return ((struct GB*)core->board)->video.renderer->vram;
#endif
#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			return ((struct GBA*)core->board)->video.renderer->vram;
#endif
		default:
			break;
		}
		break;
	default:
		break;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		switch (core->platform(core)) {
#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			switch (((struct GBA*) core->board)->memory.savedata.type) {
			case SAVEDATA_AUTODETECT:
				return SIZE_CART_FLASH1M;
			default:
				return GBASavedataSize(&((struct GBA*) core->board)->memory.savedata);
			}
#endif
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			return ((struct GB*) core->board)->sramSize;
#endif
		default:
			break;
		}
		break;
	case RETRO_MEMORY_RTC:
		switch (core->platform(core)) {
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			switch (((struct GB*) core->board)->memory.mbcType) {
			case GB_MBC3_RTC:
				return sizeof(struct GBMBCRTCSaveBuffer);
			default:
				break;
			}
#endif
		default:
			break;
		}
		break;
	case RETRO_MEMORY_SYSTEM_RAM:
		return SIZE_WORKING_RAM;
	case RETRO_MEMORY_VIDEO_RAM:
		return SIZE_VRAM;
	default:
		break;
	}
	return 0;
}

void GBARetroLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (!logCallback) {
		return;
	}

	char message[128];
	vsnprintf(message, sizeof(message), format, args);

	enum retro_log_level retroLevel = RETRO_LOG_INFO;
	switch (level) {
	case mLOG_ERROR:
	case mLOG_FATAL:
		retroLevel = RETRO_LOG_ERROR;
		break;
	case mLOG_WARN:
		retroLevel = RETRO_LOG_WARN;
		break;
	case mLOG_INFO:
		retroLevel = RETRO_LOG_INFO;
		break;
	case mLOG_GAME_ERROR:
	case mLOG_STUB:
#ifdef NDEBUG
		return;
#else
		retroLevel = RETRO_LOG_DEBUG;
		break;
#endif
	case mLOG_DEBUG:
		retroLevel = RETRO_LOG_DEBUG;
		break;
	}
#ifdef NDEBUG
	static int biosCat = -1;
	if (biosCat < 0) {
		biosCat = mLogCategoryById("gba.bios");
	}

	if (category == biosCat) {
		return;
	}
#endif
	logCallback(retroLevel, "%s: %s\n", mLogCategoryName(category), message);
}

/* Used only for GB/GBC content */
static void _postAudioBuffer(struct mAVStream* stream, blip_t* left, blip_t* right) {
	UNUSED(stream);
	int produced = blip_read_samples(left, audioSampleBuffer, GB_SAMPLES, true);
	blip_read_samples(right, audioSampleBuffer + 1, GB_SAMPLES, true);
	if (produced > 0) {
		if (audioLowPassEnabled) {
			_audioLowPassFilter(audioSampleBuffer, produced);
		}
		audioCallback(audioSampleBuffer, (size_t)produced);
	}
}

static void _setRumble(struct mRumble* rumble, int enable) {
	UNUSED(rumble);
	if (!rumbleInitDone) {
		_initRumble();
	}
	if (!rumbleCallback) {
		return;
	}
	if (enable) {
		++rumbleUp;
	} else {
		++rumbleDown;
	}
}

static void _updateLux(struct GBALuminanceSource* lux) {
	UNUSED(lux);
	struct retro_variable var = {
		.key = "mgba_solar_sensor_level",
		.value = 0
	};
	bool luxVarUpdated = envVarsUpdated;

	if (luxVarUpdated && (!environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value)) {
		luxVarUpdated = false;
	}

	if (luxVarUpdated) {
		luxSensorUsed = strcmp(var.value, "sensor") == 0;
	}

	if (luxSensorUsed) {
		_initSensors();
		float fLux = luxSensorEnabled ? sensorGetCallback(0, RETRO_SENSOR_ILLUMINANCE) : 0.0f;
		luxLevel = cbrtf(fLux) * 8;
	} else {
		if (luxVarUpdated) {
			char* end;
			int newLuxLevelIndex = strtol(var.value, &end, 10);

			if (!*end) {
				if (newLuxLevelIndex > 10) {
					luxLevelIndex = 10;
				} else if (newLuxLevelIndex < 0) {
					luxLevelIndex = 0;
				} else {
					luxLevelIndex = newLuxLevelIndex;
				}
			}
		}

		luxLevel = 0x16;
		if (luxLevelIndex > 0) {
			luxLevel += GBA_LUX_LEVELS[luxLevelIndex - 1];
		}
	}

	envVarsUpdated = false;
}

static uint8_t _readLux(struct GBALuminanceSource* lux) {
	UNUSED(lux);
	return 0xFF - luxLevel;
}

static void _updateCamera(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch) {
	if (!camData || width > camWidth || height > camHeight) {
		if (camData) {
			free(camData);
			camData = NULL;
		}
		unsigned bufPitch = pitch / sizeof(*buffer);
		unsigned bufHeight = height;
		if (imcapWidth > bufPitch) {
			bufPitch = imcapWidth;
		}
		if (imcapHeight > bufHeight) {
			bufHeight = imcapHeight;
		}
		camData = malloc(sizeof(*buffer) * bufHeight * bufPitch);
		memset(camData, 0xFF, sizeof(*buffer) * bufHeight * bufPitch);
		camWidth = width;
		camHeight = bufHeight;
		camStride = bufPitch;
	}
	size_t i;
	for (i = 0; i < height; ++i) {
		memcpy(&camData[camStride * i], &buffer[pitch * i / sizeof(*buffer)], pitch);
	}
}

static void _startImage(struct mImageSource* image, unsigned w, unsigned h, int colorFormats) {
	UNUSED(image);
	UNUSED(colorFormats);

	if (camData) {
		free(camData);
	}
	camData = NULL;
	imcapWidth = w;
	imcapHeight = h;
	cam.start();
}

static void _stopImage(struct mImageSource* image) {
	UNUSED(image);
	cam.stop();
}

static void _requestImage(struct mImageSource* image, const void** buffer, size_t* stride, enum mColorFormat* colorFormat) {
	UNUSED(image);
	if (!camData) {
		cam.start();
		*buffer = NULL;
		return;
	}
	size_t offset = 0;
	if (imcapWidth < camWidth) {
		offset += (camWidth - imcapWidth) / 2;
	}
	if (imcapHeight < camHeight) {
		offset += (camHeight - imcapHeight) / 2 * camStride;
	}

	*buffer = &camData[offset];
	*stride = camStride;
	*colorFormat = mCOLOR_XRGB8;
}

static void _updateRotation(struct mRotationSource* source) {
	UNUSED(source);
	tiltX = 0;
	tiltY = 0;
	gyroZ = 0;
	_initSensors();
	if (tiltEnabled) {
		tiltX = sensorGetCallback(0, RETRO_SENSOR_ACCELEROMETER_X) * 3e8f;
		tiltY = sensorGetCallback(0, RETRO_SENSOR_ACCELEROMETER_Y) * -3e8f;
	}
	if (gyroEnabled) {
		gyroZ = sensorGetCallback(0, RETRO_SENSOR_GYROSCOPE_Z) * -1.1e9f;
	}
}

static int32_t _readTiltX(struct mRotationSource* source) {
	UNUSED(source);
	return tiltX;
}

static int32_t _readTiltY(struct mRotationSource* source) {
	UNUSED(source);
	return tiltY;
}

static int32_t _readGyroZ(struct mRotationSource* source) {
	UNUSED(source);
	return gyroZ;
}
