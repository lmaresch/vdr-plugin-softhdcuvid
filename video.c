///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id: bacf89f24503be74d113a83139a277ff2290014a $
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///
///	@todo disable screen saver support
///
///	Uses Xlib where it is needed for VA-API or cuvid.  XCB is used for
///	everything else.
///
///	- X11
///	- OpenGL rendering
///	- OpenGL rendering with GLX texture-from-pixmap
///	- Xrender rendering
///
///	@todo FIXME: use vaErrorStr for all VA-API errors.
///

#define USE_XLIB_XCB			///< use xlib/xcb backend
#define noUSE_SCREENSAVER		///< support disable screensaver
//#define USE_AUTOCROP			///< compile auto-crop support
#define USE_GRAB			///< experimental grab code
//#define USE_GLX			///< outdated GLX code
#define USE_DOUBLEBUFFER		///< use GLX double buffers
//#define USE_VAAPI				///< enable vaapi support
#define USE_CUVID				///< enable cuvid support
//#define USE_BITMAP			///< use cuvid bitmap surface
//#define AV_INFO				///< log a/v sync informations
#ifndef AV_INFO_TIME
#define AV_INFO_TIME (50 * 60)		///< a/v info every minute
#endif

#define USE_VIDEO_THREAD		///< run decoder in an own thread
//#define USE_VIDEO_THREAD2		///< run decoder+display in own threads

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/prctl.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#ifdef USE_VIDEO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#include <time.h>
#include <signal.h>
#ifndef HAVE_PTHREAD_NAME
    /// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#ifdef USE_XLIB_XCB
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <xcb/xcb.h>
//#include <xcb/bigreq.h>
#ifdef xcb_USE_GLX
#include <xcb/glx.h>
#endif
//#include <xcb/randr.h>
#ifdef USE_SCREENSAVER
#include <xcb/screensaver.h>
#include <xcb/dpms.h>
#endif
//#include <xcb/shm.h>
//#include <xcb/xv.h>

//#include <xcb/xcb_image.h>
//#include <xcb/xcb_event.h>
//#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
#include <xcb/xcb_ewmh.h>
#else // compatibility hack for old xcb-util

/**
 * @brief Action on the _NET_WM_STATE property
 */
typedef enum
{
    /* Remove/unset property */
    XCB_EWMH_WM_STATE_REMOVE = 0,
    /* Add/set property */
    XCB_EWMH_WM_STATE_ADD = 1,
    /* Toggle property	*/
    XCB_EWMH_WM_STATE_TOGGLE = 2
} xcb_ewmh_wm_state_action_t;
#endif
#endif

#ifdef USE_GLX
#include <GL/glew.h>
#include <GL/gl.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glext.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glx.h>
// only for gluErrorString
#include <GL/glu.h>

#include <GL/glut.h>
#include <GL/freeglut_ext.h>

#endif



#ifdef CUVID
//#define CUDA_API_PER_THREAD_DEFAULT_STREAM
#include <GL/gl.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glext.h>			// For GL_COLOR_BUFFER_BIT 
#include <libavutil/hwcontext.h>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <dynlink_nvcuvid.h>
#include <cudaGL.h>
#include <libavutil/hwcontext_cuda.h>
// CUDA includes
#define __DEVICE_TYPES_H__
#endif

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

// support old ffmpeg versions <1.0
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,18,102)
#define AVCodecID CodecID
#define AV_CODEC_ID_H263 CODEC_ID_H263
#define AV_CODEC_ID_H264 CODEC_ID_H264
#define AV_CODEC_ID_MPEG1VIDEO CODEC_ID_MPEG1VIDEO
#define AV_CODEC_ID_MPEG2VIDEO CODEC_ID_MPEG2VIDEO
#define AV_CODEC_ID_MPEG4 CODEC_ID_MPEG4
#define AV_CODEC_ID_VC1 CODEC_ID_VC1
#define AV_CODEC_ID_WMV3 CODEC_ID_WMV3
#endif
#include <libavcodec/vaapi.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54,86,100)
    ///
    /// ffmpeg version 1.1.1 calls get_format with zero width and height
    /// for H264 codecs.
    /// since version 1.1.3 get_format is called twice.
    /// ffmpeg 1.2 still buggy
    ///
#define FFMPEG_BUG1_WORKAROUND		///< get_format bug workaround
#endif

#include "iatomic.h"			// portable atomic_t
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"



//----------------------------------------------------------------------------
//	Declarations
//----------------------------------------------------------------------------

///
///	Video resolutions selector.
///
typedef enum _video_resolutions_
{
    VideoResolution576i,		///< ...x576 interlaced
    VideoResolution720p,		///< ...x720 progressive
    VideoResolutionFake1080i,		///< 1280x1080 1440x1080 interlaced
    VideoResolution1080i,		///< 1920x1080 interlaced
    VideoResolutionUHD,			/// UHD progressive
    VideoResolutionMax			///< number of resolution indexs
} VideoResolutions;

///
///	Video deinterlace modes.
///
typedef enum _video_deinterlace_modes_
{
    VideoDeinterlaceBob,		///< bob deinterlace
    VideoDeinterlaceWeave,		///< weave deinterlace
    VideoDeinterlaceTemporal,		///< temporal deinterlace
    VideoDeinterlaceTemporalSpatial,	///< temporal spatial deinterlace
    VideoDeinterlaceSoftBob,		///< software bob deinterlace
    VideoDeinterlaceSoftSpatial,	///< software spatial deinterlace
} VideoDeinterlaceModes;

///
///	Video scaleing modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

///
///	Video zoom modes.
///
typedef enum _video_zoom_modes_
{
    VideoNormal,			///< normal
    VideoStretch,			///< stretch to all edges
    VideoCenterCutOut,			///< center and cut out
    VideoAnamorphic,			///< anamorphic scaled (unsupported)
} VideoZoomModes;

///
///	Video color space conversions.
///
typedef enum _video_color_space_
{
    VideoColorSpaceNone,		///< no conversion
    VideoColorSpaceBt601,		///< ITU.BT-601 Y'CbCr
    VideoColorSpaceBt709,		///< ITU.BT-709 HDTV Y'CbCr
    VideoColorSpaceSmpte240		///< SMPTE-240M Y'PbPr
} VideoColorSpace;

///
///	Video output module structure and typedef.
///
typedef struct _video_module_
{
    const char *Name;			///< video output module name
    char Enabled;			///< flag output module enabled

    /// allocate new video hw decoder
    VideoHwDecoder *(*const NewHwDecoder)(VideoStream *);
    void (*const DelHwDecoder) (VideoHwDecoder *);
    unsigned (*const GetSurface) (VideoHwDecoder *, const AVCodecContext *);
    void (*const ReleaseSurface) (VideoHwDecoder *, unsigned);
    enum AVPixelFormat (*const get_format) (VideoHwDecoder *, AVCodecContext *,
	const enum AVPixelFormat *);
    void (*const RenderFrame) (VideoHwDecoder *, const AVCodecContext *,
	const AVFrame *);
    void *(*const GetHwAccelContext)(VideoHwDecoder *);
    void (*const SetClock) (VideoHwDecoder *, int64_t);
     int64_t(*const GetClock) (const VideoHwDecoder *);
    void (*const SetClosing) (const VideoHwDecoder *);
    void (*const ResetStart) (const VideoHwDecoder *);
    void (*const SetTrickSpeed) (const VideoHwDecoder *, int);
    uint8_t *(*const GrabOutput)(int *, int *, int *);
    void (*const GetStats) (VideoHwDecoder *, int *, int *, int *, int *, float *);
    void (*const SetBackground) (uint32_t);
    void (*const SetVideoMode) (void);
    void (*const ResetAutoCrop) (void);

    /// module display handler thread
    void (*const DisplayHandlerThread) (void);

    void (*const OsdClear) (void);	///< clear OSD
    /// draw OSD ARGB area
    void (*const OsdDrawARGB) (int, int, int, int, int, const uint8_t *, int,
	int);
    void (*const OsdInit) (int, int);	///< initialize OSD
    void (*const OsdExit) (void);	///< cleanup OSD

    int (*const Init) (const char *);	///< initialize video output module
    void (*const Exit) (void);		///< cleanup video output module
} VideoModule;

typedef struct {
    /** Left X co-ordinate. Inclusive. */
    uint32_t x0;
    /** Top Y co-ordinate. Inclusive. */
    uint32_t y0;
    /** Right X co-ordinate. Exclusive. */
    uint32_t x1;
    /** Bottom Y co-ordinate. Exclusive. */
    uint32_t y1;
} VdpRect;

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CODEC_SURFACES_MAX	16	///< maximal of surfaces

#define VIDEO_SURFACES_MAX	8	///< video output surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------
AVBufferRef *HwDeviceContext;		///< ffmpeg HW device context
char VideoIgnoreRepeatPict;		///< disable repeat pict warning

static const char *VideoDriverName="cuvid";	///< video output device
static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window
static xcb_screen_t const *VideoScreen;	///< video screen
static uint32_t VideoBlankTick;		///< blank cursor timer
static xcb_pixmap_t VideoCursorPixmap;	///< blank curosr pixmap
static xcb_cursor_t VideoBlankCursor;	///< empty invisible cursor

static int VideoWindowX;		///< video output window x coordinate
static int VideoWindowY;		///< video outout window y coordinate
static unsigned VideoWindowWidth;	///< video output window width
static unsigned VideoWindowHeight;	///< video output window height

static const VideoModule NoopModule;	///< forward definition of noop module

    /// selected video module
static const VideoModule *VideoUsedModule = &NoopModule;

signed char VideoHardwareDecoder = -1;	///< flag use hardware decoder

static char VideoSurfaceModesChanged;	///< flag surface modes changed

    /// flag use transparent OSD.
static const char VideoTransparentOsd = 1;

static uint32_t VideoBackground;	///< video background color
static char VideoStudioLevels;		///< flag use studio levels

    /// Default deinterlace mode.
static VideoDeinterlaceModes VideoDeinterlace[VideoResolutionMax];

    /// Default number of deinterlace surfaces
static const int VideoDeinterlaceSurfaces = 4;

    /// Default skip chroma deinterlace flag (CUVID only).
static char VideoSkipChromaDeinterlace[VideoResolutionMax];

    /// Default inverse telecine flag (CUVID only).
static char VideoInverseTelecine[VideoResolutionMax];

    /// Default amount of noise reduction algorithm to apply (0 .. 1000).
static int VideoDenoise[VideoResolutionMax];

    /// Default amount of sharpening, or blurring, to apply (-1000 .. 1000).
static int VideoSharpen[VideoResolutionMax];

    /// Default cut top and bottom in pixels
static int VideoCutTopBottom[VideoResolutionMax];

    /// Default cut left and right in pixels
static int VideoCutLeftRight[VideoResolutionMax];

    /// Color space ITU-R BT.601, ITU-R BT.709, ...
static const VideoColorSpace VideoColorSpaces[VideoResolutionMax] = {
    VideoColorSpaceBt601, VideoColorSpaceBt709, VideoColorSpaceBt709,
    VideoColorSpaceBt709,VideoColorSpaceBt709
};

    /// Default scaling mode
static VideoScalingModes VideoScaling[VideoResolutionMax];

    /// Default audio/video delay
int VideoAudioDelay;

    /// Default zoom mode for 4:3
static VideoZoomModes Video4to3ZoomMode;

    /// Default zoom mode for 16:9 and others
static VideoZoomModes VideoOtherZoomMode;

static char Video60HzMode;		///< handle 60hz displays
static char VideoSoftStartSync;		///< soft start sync audio/video
static const int VideoSoftStartFrames = 100;	///< soft start frames
static char VideoShowBlackPicture;	///< flag show black picture

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message atom
static xcb_atom_t NetWmState;		///< wm-state message atom
static xcb_atom_t NetWmStateFullscreen;	///< fullscreen wm-state message atom

#ifdef DEBUG
extern uint32_t VideoSwitch;		///< ticks for channel switch
#endif
extern void AudioVideoReady(int64_t);	///< tell audio video is ready

#ifdef USE_VIDEO_THREAD

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex
pthread_mutex_t OSDMutex;			///< OSD update mutex
#endif

#ifdef USE_VIDEO_THREAD2

static pthread_t VideoDisplayThread;	///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoDisplayMutex;	///< video condition mutex
static pthread_mutex_t VideoDisplayLockMutex;	///< video lock mutex

#endif

static int OsdConfigWidth;		///< osd configured width
static int OsdConfigHeight;		///< osd configured height
static char OsdShown;			///< flag show osd
static char Osd3DMode;			///< 3D OSD mode
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height
static int OsdDirtyX;			///< osd dirty area x
static int OsdDirtyY;			///< osd dirty area y
static int OsdDirtyWidth;		///< osd dirty area width
static int OsdDirtyHeight;		///< osd dirty area height
#ifdef USE_OPENGLOSD
static void (*VideoEventCallback)(void) = NULL;  /// callback function to notify VDR about Video Events
#endif
static int64_t VideoDeltaPTS;		///< FIXME: fix pts

#ifdef USE_SCREENSAVER
static char DPMSDisabled;		///< flag we have disabled dpms
static char EnableDPMSatBlackScreen;	///< flag we should enable dpms at black screen
#endif

static int GlxEnabled;			///< use GLX
static int GlxVSyncEnabled = 1;		///< enable/disable v-sync
static GLXContext GlxSharedContext;	///< shared gl context
static GLXContext GlxContext;		///< our gl context

#ifdef USE_VIDEO_THREAD
static GLXContext GlxThreadContext;	///< our gl context for the thread
#endif

static XVisualInfo *GlxVisualInfo;	///< our gl visual

static GLuint OsdGlTextures[2];		///< gl texture for OSD
static int OsdIndex=0;			///< index into OsdGlTextures
static void GlxSetupWindow(xcb_window_t window, int width, int height, GLXContext context);
GLXContext OSDcontext;

//----------------------------------------------------------------------------
//	Common Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread
static void VideoThreadExit(void);	///< exit/kill video thread

#ifdef USE_SCREENSAVER
static void X11SuspendScreenSaver(xcb_connection_t *, int);
static int X11HaveDPMS(xcb_connection_t *);
static void X11DPMSReenable(xcb_connection_t *);
static void X11DPMSDisable(xcb_connection_t *);
#endif
uint64_t gettid()
{
	return pthread_self();
}
///
///	Update video pts.
///
///	@param pts_p		pointer to pts
///	@param interlaced	interlaced flag (frame isn't right)
///	@param frame		frame to display
///
///	@note frame->interlaced_frame can't be used for interlace detection
///
static void VideoSetPts(int64_t * pts_p, int interlaced,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int64_t pts;
    int duration;

    //
    //	Get duration for this frame.
    //	FIXME: using framerate as workaround for av_frame_get_pkt_duration
    //

    if (video_ctx->framerate.num && video_ctx->framerate.den) {
		duration = 1000 * video_ctx->framerate.den / video_ctx->framerate.num;
    } else {
		duration = interlaced ? 40 : 20;	// 50Hz -> 20ms default
    }
    Debug(4, "video: %d/%d %" PRIx64 " -> %d\n", video_ctx->framerate.den,	video_ctx->framerate.num, av_frame_get_pkt_duration(frame), duration);


    // update video clock
    if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
		*pts_p += duration * 90;
		//Info("video: %s +pts\n", Timestamp2String(*pts_p));
    }
    //av_opt_ptr(avcodec_get_frame_class(), frame, "best_effort_timestamp");
    //pts = frame->best_effort_timestamp;
//    pts = frame->pkt_pts;
	pts = frame->pts;
    if (pts == (int64_t) AV_NOPTS_VALUE || !pts) {
		// libav: 0.8pre didn't set pts
		pts = frame->pkt_dts;
    }
    // libav: sets only pkt_dts which can be 0
    if (pts && pts != (int64_t) AV_NOPTS_VALUE) {
		// build a monotonic pts
		if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
			int64_t delta;

			delta = pts - *pts_p;
			// ignore negative jumps
			if (delta > -600 * 90 && delta <= -40 * 90) {
			if (-delta > VideoDeltaPTS) {
				VideoDeltaPTS = -delta;
				Debug(4,
				"video: %#012" PRIx64 "->%#012" PRIx64 " delta%+4"
				PRId64 " pts\n", *pts_p, pts, pts - *pts_p);
			}
			return;
			}
		} else {			// first new clock value
			AudioVideoReady(pts);
		}
		if (*pts_p != pts) {
			Debug(4,
			"video: %#012" PRIx64 "->%#012" PRIx64 " delta=%4" PRId64
			" pts\n", *pts_p, pts, pts - *pts_p);
			*pts_p = pts;
		}
    }
}
static int CuvidMessage(int level, const char *format, ...);
///
///	Update output for new size or aspect ratio.
///
///	@param input_aspect_ratio	video stream aspect
///
static void VideoUpdateOutput(AVRational input_aspect_ratio, int input_width,
    int input_height, VideoResolutions resolution, int video_x, int video_y,
    int video_width, int video_height, int *output_x, int *output_y,
    int *output_width, int *output_height, int *crop_x, int *crop_y,
    int *crop_width, int *crop_height)
{
    AVRational display_aspect_ratio;
    AVRational tmp_ratio;

    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
		input_aspect_ratio.num = 1;
		input_aspect_ratio.den = 1;
		Debug(3, "video: aspect defaults to %d:%d\n", input_aspect_ratio.num,
			input_aspect_ratio.den);
    }

    av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	input_width * input_aspect_ratio.num,
	input_height * input_aspect_ratio.den, 1024 * 1024);

    // InputWidth/Height can be zero = uninitialized
    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
		input_aspect_ratio.num = 1;
		input_aspect_ratio.den = 1;
    }

    display_aspect_ratio.num =
	VideoScreen->width_in_pixels * VideoScreen->height_in_millimeters;
    display_aspect_ratio.den =
	VideoScreen->height_in_pixels * VideoScreen->width_in_millimeters;

    display_aspect_ratio = av_mul_q(input_aspect_ratio, display_aspect_ratio);
    Debug(3, "video: aspect %d:%d\n", display_aspect_ratio.num,
	display_aspect_ratio.den);

    *crop_x = VideoCutLeftRight[resolution];
    *crop_y = VideoCutTopBottom[resolution];
    *crop_width = input_width - VideoCutLeftRight[resolution] * 2;
    *crop_height = input_height - VideoCutTopBottom[resolution] * 2;

    // FIXME: store different positions for the ratios
    tmp_ratio.num = 4;
    tmp_ratio.den = 3;
#ifdef DEBUG
    Debug(4, "ratio: %d:%d %d:%d\n", input_aspect_ratio.num, input_aspect_ratio.den, display_aspect_ratio.num, display_aspect_ratio.den);
#endif
    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
		switch (Video4to3ZoomMode) {
			case VideoNormal:
			goto normal;
			case VideoStretch:
			goto stretch;
			case VideoCenterCutOut:
			goto center_cut_out;
			case VideoAnamorphic:
			// FIXME: rest should be done by hardware
			goto stretch;
		}
    }
    switch (VideoOtherZoomMode) {
	case VideoNormal:
	    goto normal;
	case VideoStretch:
	    goto stretch;
	case VideoCenterCutOut:
	    goto center_cut_out;
	case VideoAnamorphic:
	    // FIXME: rest should be done by hardware
	    goto stretch;
    }

  normal:
    *output_x = video_x;
    *output_y = video_y;
    *output_width  = (video_height * display_aspect_ratio.num + display_aspect_ratio.den -1 ) / display_aspect_ratio.den;
    *output_height = (video_width  * display_aspect_ratio.den + display_aspect_ratio.num -1 ) / display_aspect_ratio.num;
// JOJO
    if (*output_width > video_width) {
		*output_width = video_width;
		*output_y += (video_height - *output_height) / 2;
    } else if (*output_height > video_height) {
		*output_height = video_height;
		*output_x += (video_width - *output_width) / 2;
    }
	
    CuvidMessage(2, "video: normal aspect output %dx%d%+d%+d Video %dx%d\n", *output_width, *output_height, *output_x, *output_y,video_width,video_height);
    return;

  stretch:
    *output_x = video_x;
    *output_y = video_y;
    *output_width = video_width;
    *output_height = video_height;
    Debug(3, "video: stretch output %dx%d%+d%+d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  center_cut_out:
    *output_x = video_x;
    *output_y = video_y;
    *output_height = video_height;
    *output_width = video_width;

    *crop_width =
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den -
	1) / display_aspect_ratio.den;
    *crop_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num -
	1) / display_aspect_ratio.num;

    // look which side must be cut
    if (*crop_width > video_width) {
	int tmp;

	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_width - video_width) * input_width) / (2 * video_width);
	// FIXME: round failure?
	if (tmp > *crop_x) {
	    *crop_x = tmp;
	}
	*crop_width = input_width - *crop_x * 2;
    } else if (*crop_height > video_height) {
	int tmp;

	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_height - video_height) * input_height)
	    / (2 * video_height);
	// FIXME: round failure?
	if (tmp > *crop_y) {
	    *crop_y = tmp;
	}
	*crop_height = input_height - *crop_y * 2;
    } else {
	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;
	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;
    }
    Debug(3, "video: aspect crop %dx%d%+d%+d\n", *crop_width, *crop_height,
	*crop_x, *crop_y);
    return;
}

//----------------------------------------------------------------------------
//	GLX
//----------------------------------------------------------------------------

#ifdef USE_GLX



///
///	GLX extension functions
///@{
#ifdef GLX_MESA_swap_control
static PFNGLXSWAPINTERVALMESAPROC GlxSwapIntervalMESA;
#endif
#ifdef GLX_SGI_video_sync
static PFNGLXGETVIDEOSYNCSGIPROC GlxGetVideoSyncSGI;
#endif
#ifdef GLX_SGI_swap_control
static PFNGLXSWAPINTERVALSGIPROC GlxSwapIntervalSGI;
#endif

///@}

///
///	GLX check error.
///
static void GlxCheck(void)
{
    GLenum err;

    if ((err = glGetError()) != GL_NO_ERROR) {
	Debug(3, "video/glx: error %d '%s'\n", err, gluErrorString(err));
    }
}

///
///	GLX check if a GLX extension is supported.
///
///	@param ext	extension to query
///	@returns true if supported, false otherwise
///
static int GlxIsExtensionSupported(const char *ext)
{
    const char *extensions;

    if ((extensions =
	    glXQueryExtensionsString(XlibDisplay,
		DefaultScreen(XlibDisplay)))) {
	const char *s;
	int l;

	s = strstr(extensions, ext);
	l = strlen(ext);
	return s && (s[l] == ' ' || s[l] == '\0');
    }
    return 0;
}

///
///	Setup GLX decoder
///
///	@param width		input video textures width
///	@param height		input video textures height
///	@param[OUT] textures	created and prepared textures
///
static void GlxSetupDecoder(int width, int height, GLuint * textures)
{
    int i;

    glEnable(GL_TEXTURE_2D);		// create 2d texture
    glGenTextures(2, textures);
    GlxCheck();
    for (i = 0; i < 2; ++i) {
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_TEXTURE_2D);

    GlxCheck();
}

///
///	Render texture.
///
///	@param texture	2d texture
///	@param x	window x
///	@param y	window y
///	@param width	window width
///	@param height	window height
///
static inline void GlxRenderTexture(GLuint texture, int x, int y, int width, int height)
{
	
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

//    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color

    glBegin(GL_QUADS); {
		glTexCoord2f(1.0f, 1.0f);
		glVertex2i(x + width, y + height);
		glTexCoord2f(0.0f, 1.0f);
		glVertex2i(x, y + height);
		glTexCoord2f(0.0f, 0.0f);
		glVertex2i(x, y);
		glTexCoord2f(1.0f, 0.0f);
		glVertex2i(x + width, y);
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Upload OSD texture.
///
///	@param x	x coordinate texture
///	@param y	y coordinate texture
///	@param width	argb image width
///	@param height	argb image height
///	@param argb	argb image
///
static void GlxUploadOsdTexture(int x, int y, int width, int height,
    const uint8_t * argb)
{
    // FIXME: use other / faster uploads
    // ARB_pixelbuffer_object GL_PIXEL_UNPACK_BUFFER glBindBufferARB()
    // glMapBuffer() glUnmapBuffer()

    glEnable(GL_TEXTURE_2D);		// upload 2d texture

    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_BGRA,	GL_UNSIGNED_BYTE, argb);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_TEXTURE_2D);

}

///
///	GLX initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void GlxOsdInit(int width, int height)
{
    int i;

#ifdef DEBUG
    if (!GlxEnabled) {
		Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
	return;
    }
#endif

    Debug(3, "video/glx: osd init context %p <-> %p\n", glXGetCurrentContext(), GlxContext);
	
#ifndef USE_OPENGLOSD
    //
    //	create a RGBA texture.
    //
    glEnable(GL_TEXTURE_2D);		// create 2d texture(s)
    glGenTextures(2, OsdGlTextures);
    for (i = 0; i < 2; ++i) {
		glBindTexture(GL_TEXTURE_2D, OsdGlTextures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
#else
	OsdGlTextures[0] = 0;
#endif
}

///
///	GLX cleanup osd.
///
static void GlxOsdExit(void)
{
    if (OsdGlTextures[0]) {
		glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext );
		glDeleteTextures(2, OsdGlTextures);
		OsdGlTextures[0] = 0;
		OsdGlTextures[1] = 0;
    }
}

///
///	Upload ARGB image to texture.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void GlxOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    uint8_t *tmp;

#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif

#ifdef DEBUG
    if (!GlxEnabled) {
		Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
		return;
    }
    start = GetMsTicks();	
#endif

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
		Error(_("video/glx: can't make glx context current\n"));
		return;
    }
    // FIXME: faster way
    tmp = malloc(width * height * 4);
    if (tmp) {
		int i;

		for (i = 0; i < height; ++i) {
		    memcpy(tmp + i * width * 4, argb + xi * 4 + (i + yi) * pitch, width * 4);
		}
		GlxUploadOsdTexture(x, y, width, height, tmp);
		glXMakeCurrent(XlibDisplay, None, NULL);
		free(tmp);
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug(4, "video/glx: osd upload %dx%d%+d%+d %dms %d\n", width, height, x, y, end - start, width * height * 4);
#endif
}

///
///	Clear OSD texture.
///
///	@note looked by caller
///
static void GlxOsdClear(void)
{
    void *texbuf;

#ifdef USE_OPENGLOSD
	return;
#endif
	
#ifdef DEBUG
    if (!GlxEnabled) {
		Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
	return;
    }

    Debug(3, "video/glx: osd context %p <-> %p\n", glXGetCurrentContext(),	GlxContext);
#endif

    // FIXME: any opengl function to clear an area?
    // FIXME: if not; use zero buffer
    // FIXME: if not; use dirty area

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
		Error(_("video/glx: can't make glx context current\n"));
	return;
    }

    texbuf = calloc(OsdWidth * OsdHeight, 4);
    GlxUploadOsdTexture(0, 0, OsdWidth, OsdHeight, texbuf);
    glXMakeCurrent(XlibDisplay, None, NULL);
    free(texbuf);
}

///
///	Setup GLX window.
///
///	@param window	xcb window id
///	@param width	window width
///	@param height	window height
///	@param context	GLX context
///
static void GlxSetupWindow(xcb_window_t window, int width, int height, GLXContext context)
{
#ifdef DEBUG
    uint32_t start;
    uint32_t end;
    int i;
    unsigned count;
#endif

    Debug(3, "video/glx: %s %x %dx%d context:%p", __FUNCTION__, window, width, height, context);

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, window, context)) {
		Fatal(_("video/glx: can't make glx context current\n"));
		GlxEnabled = 0;
		return;
    }

    Debug(3, "video/glx: ok\n");

#ifdef DEBUG
    // check if v-sync is working correct
    end = GetMsTicks();
    for (i = 0; i < 10; ++i) {
		start = end;

		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(XlibDisplay, window);
		end = GetMsTicks();

		GlxGetVideoSyncSGI(&count);
		Debug(4, "video/glx: %5d frame rate %dms\n", count, end - start);
		// nvidia can queue 5 swaps
		if (i > 5 && (end - start) < 15) {
		    Warning(_("video/glx: no v-sync\n"));
		}
    }
#endif

    // viewpoint
    GlxCheck();
    glViewport(0, 0, width, height);
    glDepthRange(-1.0, 1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glClearDepth(1.0);
    GlxCheck();
	if (glewInit())
		Fatal(_("glewinit failed\n"));

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    GlxCheck();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);		// setup 2d drawing
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_BACK);
#else
    glDrawBuffer(GL_FRONT);
#endif
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef DEBUG
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_FRONT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawBuffer(GL_BACK);
#endif
#endif

    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// intial background color
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef DEBUG
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// background color
#endif
    GlxCheck();
}

///
///	Initialize GLX.
///
static void GlxInit(void)
{

    XVisualInfo *vi=NULL;
	
		//The desired 30-bit color visual
	int attributeList10[] = { 
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, 
		GLX_RENDER_TYPE,   GLX_RGBA_BIT, 
		GLX_DOUBLEBUFFER,  True,  
		GLX_RED_SIZE,      10,   /*10bits for R */ 
		GLX_GREEN_SIZE,    10,   /*10bits for G */ 
		GLX_BLUE_SIZE,     10,   /*10bits for B */ 
		None 
	}; 
		int attributeList[] = { 
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, 
		GLX_RENDER_TYPE,   GLX_RGBA_BIT, 
		GLX_DOUBLEBUFFER,  True,  
		GLX_RED_SIZE,      8,   /*8 bits for R */ 
		GLX_GREEN_SIZE,    8,   /*8 bits for G */ 
		GLX_BLUE_SIZE,     8,   /*8 bits for B */ 
		None 
	}; 
	int fbcount; 
	
    GLXContext context;
    int major;
    int minor;
    int glx_GLX_EXT_swap_control;
    int glx_GLX_MESA_swap_control;
    int glx_GLX_SGI_swap_control;
    int glx_GLX_SGI_video_sync;

    if (!glXQueryVersion(XlibDisplay, &major, &minor)) {
		Error(_("video/glx: no GLX support\n"));
		GlxEnabled = 0;
	return;
    }
    Info(_("video/glx: glx version %d.%d\n"), major, minor);

    //
    //	check which extension are supported
    //
    glx_GLX_EXT_swap_control = GlxIsExtensionSupported("GLX_EXT_swap_control");
    glx_GLX_MESA_swap_control =
	GlxIsExtensionSupported("GLX_MESA_swap_control");
    glx_GLX_SGI_swap_control = GlxIsExtensionSupported("GLX_SGI_swap_control");
    glx_GLX_SGI_video_sync = GlxIsExtensionSupported("GLX_SGI_video_sync");

#ifdef GLX_MESA_swap_control
    if (glx_GLX_MESA_swap_control) {
		GlxSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");
    }
    Debug(3, "video/glx: GlxSwapIntervalMESA=%p\n", GlxSwapIntervalMESA);
#endif
#ifdef GLX_SGI_swap_control
    if (glx_GLX_SGI_swap_control) {
		GlxSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)
	    glXGetProcAddress((const GLubyte *)"wglSwapIntervalEXT");
    }
    Debug(3, "video/glx: GlxSwapIntervalSGI=%p\n", GlxSwapIntervalSGI);
#endif
#ifdef GLX_SGI_video_sync
    if (glx_GLX_SGI_video_sync) {
		GlxGetVideoSyncSGI = (PFNGLXGETVIDEOSYNCSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXGetVideoSyncSGI");
    }
    Debug(3, "video/glx: GlxGetVideoSyncSGI=%p\n", GlxGetVideoSyncSGI);
#endif
    // glXGetVideoSyncSGI glXWaitVideoSyncSGI



    // create glx context
    glXMakeCurrent(XlibDisplay, None, NULL);
	
	GLXFBConfig *fbc = glXChooseFBConfig(XlibDisplay, DefaultScreen(XlibDisplay),attributeList10,&fbcount);   // try 10 Bit 
	if (fbc==NULL) { 
		fbc = glXChooseFBConfig(XlibDisplay, DefaultScreen(XlibDisplay),attributeList,&fbcount); // fall back to 8 Bit
		if (fbc==NULL)
			Fatal(_("did not get FBconfig"));
	}

	vi = glXGetVisualFromFBConfig(XlibDisplay, fbc[0]); 
	

	int redSize, greenSize, blueSize;
	glXGetFBConfigAttrib(XlibDisplay, fbc[0], GLX_RED_SIZE, &redSize); 
	glXGetFBConfigAttrib(XlibDisplay, fbc[0], GLX_GREEN_SIZE, &greenSize); 
	glXGetFBConfigAttrib(XlibDisplay, fbc[0], GLX_BLUE_SIZE, &blueSize); 
	Debug(3,"RGB size %d:%d:%d\n",redSize, greenSize, blueSize); 
	
    if (!vi) {
		Fatal(_("video/glx: can't get a RGB visual\n"));
		GlxEnabled = 0;
		return;
    }
    if (!vi->visual) {
		Fatal(_("video/glx: no valid visual found\n"));
		GlxEnabled = 0;
		return;
    }
    if (vi->bits_per_rgb < 8) {
		Fatal(_("video/glx: need atleast 8-bits per RGB\n"));
		GlxEnabled = 0;
		return;
    }
	
	Debug(3, "Chosen visual ID = 0x%x\n", vi->visualid );

    context = glXCreateContext(XlibDisplay, vi, NULL, GL_TRUE);
    if (!context) {
		Fatal(_("video/glx: can't create glx context\n"));
		GlxEnabled = 0;
		return;
    }
    GlxSharedContext = context;
    context = glXCreateContext(XlibDisplay, vi, GlxSharedContext, GL_TRUE);
    if (!context) {
		Fatal(_("video/glx: can't create glx context\n"));
		GlxEnabled = 0;
		glXDestroyContext(XlibDisplay, GlxSharedContext);
		GlxSharedContext = 0;
		return;
    }
    GlxContext = context;

    GlxVisualInfo = vi;
    Debug(3, "video/glx: visual %#02x depth %u\n", (unsigned)vi->visualid, vi->depth);

    //
    //	query default v-sync state
    //
    if (glx_GLX_EXT_swap_control) {
		unsigned tmp;

		tmp = -1;
		glXQueryDrawable(XlibDisplay, DefaultRootWindow(XlibDisplay), GLX_SWAP_INTERVAL_EXT, &tmp);
		GlxCheck();

		Debug(3, "video/glx: default v-sync is %d\n", tmp);
    } else {
		Debug(3, "video/glx: default v-sync is unknown\n");
    }

    //
    //	disable wait on v-sync
    //
    // FIXME: sleep before swap / busy waiting hardware
    // FIXME: 60hz lcd panel
    // FIXME: config: default, on, off
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalSGI) {
		if (GlxSwapIntervalSGI(0)) {
		    GlxCheck();
	    	Warning(_("video/glx: can't disable v-sync\n"));
		} else {
	    	Info(_("video/glx: v-sync disabled\n"));
		}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalMESA) {
		if (GlxSwapIntervalMESA(0)) {
		    GlxCheck();
	    	Warning(_("video/glx: can't disable v-sync\n"));
		} else {
	    	Info(_("video/glx: v-sync disabled\n"));
		}
    }
#endif

    //
    //	enable wait on v-sync
    //
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalMESA) {
		if (GlxSwapIntervalMESA(1)) {
	    	GlxCheck();
	    	Warning(_("video/glx: can't enable v-sync\n"));
		} else {
	    	Info(_("video/glx: v-sync enabled\n"));
		}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalSGI) {
		if (GlxSwapIntervalSGI(1)) {
	    	GlxCheck();
	    	Warning(_("video/glx: SGI can't enable v-sync\n"));
		} else {
	    	Info(_("video/glx: SGI v-sync enabled\n"));
		}
    }
#endif

}


///
///	Cleanup GLX.
///
static void GlxExit(void)
{
    Debug(3, "video/glx: %s\n", __FUNCTION__);

    glFinish();

    // must destroy glx
    if (glXGetCurrentContext() == GlxContext) {
		// if currently used, set to none
		glXMakeCurrent(XlibDisplay, None, NULL);
    }
    if (GlxSharedContext) {
		glXDestroyContext(XlibDisplay, GlxSharedContext);
		GlxCheck();
    }
    if (GlxContext) {
		glXDestroyContext(XlibDisplay, GlxContext);
		GlxCheck();
    }
    if (GlxThreadContext) {
		glXDestroyContext(XlibDisplay, GlxThreadContext);
		GlxCheck();
    }
    // FIXME: must free GlxVisualInfo
}

#endif

//----------------------------------------------------------------------------
//	common functions
//----------------------------------------------------------------------------

///
///	Calculate resolution group.
///
///	@param width		video picture raw width
///	@param height		video picture raw height
///	@param interlace	flag interlaced video picture
///
///	@note interlace isn't used yet and probably wrong set by caller.
///
static VideoResolutions VideoResolutionGroup(int width, int height,
    __attribute__ ((unused))
    int interlace)
{
    if (height == 2160) {
	return VideoResolutionUHD;
    }
    if (height <= 576) {
	return VideoResolution576i;
    }
    if (height <= 720) {
	return VideoResolution720p;
    }
    if (height <= 1080) {
	return VideoResolutionFake1080i;
    }
    if (width < 1920) {
	return VideoResolutionFake1080i;
    }
    return VideoResolution1080i;
}

//----------------------------------------------------------------------------
//	auto-crop
//----------------------------------------------------------------------------

///
///	auto-crop context structure and typedef.
///
typedef struct _auto_crop_ctx_
{
    int X1;				///< detected left border
    int X2;				///< detected right border
    int Y1;				///< detected top border
    int Y2;				///< detected bottom border

    int Count;				///< counter to delay switch
    int State;				///< auto-crop state (0, 14, 16)

} AutoCropCtx;

#ifdef USE_AUTOCROP

#define YBLACK 0x20			///< below is black
#define UVBLACK 0x80			///< around is black
#define M64 UINT64_C(0x0101010101010101)	///< 64bit multiplicator

    /// auto-crop percent of video width to ignore logos
static const int AutoCropLogoIgnore = 24;
static int AutoCropInterval;		///< auto-crop check interval
static int AutoCropDelay;		///< auto-crop switch delay
static int AutoCropTolerance;		///< auto-crop tolerance

///
///	Detect black line Y.
///
///	@param data	Y plane pixel data
///	@param length	number of pixel to check
///	@param pitch	offset of pixels
///
///	@note 8 pixel are checked at once, all values must be 8 aligned
///
static int AutoCropIsBlackLineY(const uint8_t * data, int length, int pitch)
{
    int n;
    int o;
    uint64_t r;
    const uint64_t *p;

#ifdef DEBUG
    if ((size_t) data & 0x7 || pitch & 0x7) {
	abort();
    }
#endif
    p = (const uint64_t *)data;
    n = length;				// FIXME: can remove n
    o = pitch / 8;

    r = 0UL;
    while (--n >= 0) {
	r |= *p;
	p += o;
    }

    // below YBLACK(0x20) is black
    return !(r & ~((YBLACK - 1) * M64));
}

///
///	Auto detect black borders and crop them.
///
///	@param autocrop auto-crop variables
///	@param width	frame width in pixel
///	@param height	frame height in pixel
///	@param data	frame planes data (Y, U, V)
///	@param pitches	frame planes pitches (Y, U, V)
///
///	@note FIXME: can reduce the checked range, left, right crop isn't
///		used yet.
///
///	@note FIXME: only Y is checked, for black.
///
static void AutoCropDetect(AutoCropCtx * autocrop, int width, int height,
    void *data[3], uint32_t pitches[3])
{
    const void *data_y;
    unsigned length_y;
    int x;
    int y;
    int x1;
    int x2;
    int y1;
    int y2;
    int logo_skip;

    //
    //	ignore top+bottom 6 lines and left+right 8 pixels
    //
#define SKIP_X	8
#define SKIP_Y	6
    x1 = width - 1;
    x2 = 0;
    y1 = height - 1;
    y2 = 0;
    logo_skip = SKIP_X + (((width * AutoCropLogoIgnore) / 100 + 8) / 8) * 8;

    data_y = data[0];
    length_y = pitches[0];

    //
    //	search top
    //
    for (y = SKIP_Y; y < y1; ++y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == SKIP_Y) {
		y = 0;
	    }
	    y1 = y;
	    break;
	}
    }
    //
    //	search bottom
    //
    for (y = height - SKIP_Y - 1; y > y2; --y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == height - SKIP_Y - 1) {
		y = height - 1;
	    }
	    y2 = y;
	    break;
	}
    }
    //
    //	search left
    //
    for (x = SKIP_X; x < x1; x += 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y, length_y)) {
	    if (x == SKIP_X) {
		x = 0;
	    }
	    x1 = x;
	    break;
	}
    }
    //
    //	search right
    //
    for (x = width - SKIP_X - 8; x > x2; x -= 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y * 8, length_y)) {
	    if (x == width - SKIP_X - 8) {
		x = width - 1;
	    }
	    x2 = x;
	    break;
	}
    }

    if (0 && (y1 > SKIP_Y || x1 > SKIP_X)) {
	Debug(3, "video/autocrop: top=%d bottom=%d left=%d right=%d\n", y1, y2,
	    x1, x2);
    }

    autocrop->X1 = x1;
    autocrop->X2 = x2;
    autocrop->Y1 = y1;
    autocrop->Y2 = y2;
}

#endif

//----------------------------------------------------------------------------
//	CUVID
//----------------------------------------------------------------------------

#ifdef USE_CUVID

///
///	CUVID decoder
///
typedef struct _cuvid_decoder_
{
     xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
	enum AVColorSpace  ColorSpace;	/// ffmpeg ColorSpace
	enum AVColorTransferCharacteristic  trc;  // 
	enum AVColorPrimaries color_primaries;
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    void *AutoCropBuffer;		///< auto-crop buffer cache
    unsigned AutoCropBufferSize;	///< auto-crop buffer size
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
	
	int grabwidth,grabheight,grab;   // Grab Data
	void *grabbase;

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    int SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    int SurfacesFree[CODEC_SURFACES_MAX];
    /// video surface ring buffer
    int SurfacesRb[VIDEO_SURFACES_MAX];
	CUcontext cuda_ctx;

	cudaStream_t stream;		// make my own cuda stream
	CUgraphicsResource cuResource;
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used
	
	CUarray      		 cu_array[CODEC_SURFACES_MAX][2];
	CUgraphicsResource   cu_res[CODEC_SURFACES_MAX][2];
	GLuint gl_textures[CODEC_SURFACES_MAX*2];  // where we will copy the CUDA result
	
    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
	float Frameproc;				/// Time to process frame
} CuvidDecoder;

static CuvidDecoder *CuvidDecoders[2];	///< open decoder streams
static int CuvidDecoderN;		///< number of decoder streams

GLuint vao_buffer;  // 
//GLuint vao_vao[4];  // 
GLuint gl_shader=0,gl_prog = 0,gl_fbo=0;      // shader programm
GLint gl_colormatrix,gl_colormatrix_c;
GLuint OSDfb=0;
GLuint OSDtexture;

int OSDx,OSDy,OSDxsize,OSDysize;

static struct timespec CuvidFrameTime;	///< time of last display

#ifdef USE_BITMAP
    /// bitmap surfaces for osd
static VdpBitmapSurface CuvidOsdBitmapSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#else
#if 0
    /// output surfaces for osd
static VdpOutputSurface CuvidOsdOutputSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#endif
#endif
static int CuvidOsdSurfaceIndex;	///< index into double buffered osd

    /// grab render output surface
//static VdpOutputSurface CuvidGrabRenderSurface = VDP_INVALID_HANDLE;
static pthread_mutex_t CuvidGrabMutex;

unsigned int size_tex_data;
unsigned int num_texels;
unsigned int num_values;
int window_width,window_height;

#include "shaders.h"

void checkCudaErrors(CUresult err)
{
    if (CUDA_SUCCESS != err)
    {
        Fatal(_("checkCudaErrors() Driver API error = %04d"), err );
    }
}
//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
static int CuvidMessage(int level, const char *format, ...)
{
    if (SysLogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

void
createTextureDst(CuvidDecoder * decoder,int anz, unsigned int size_x, unsigned int size_y, enum AVPixelFormat PixFmt);
///
///	Create surfaces for CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void CuvidCreateSurfaces(CuvidDecoder * decoder, int width, int height,enum AVPixelFormat PixFmt )
{
    int i;
   
#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
		Error(_("video/cuvid: surface needed not set\n")); 
    	decoder->SurfacesNeeded = VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/cuvid: %s: %dx%d * %d \n", __FUNCTION__, width, height, decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;

	createTextureDst(decoder,decoder->SurfacesNeeded,width,height,PixFmt);
	
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {		
        decoder->SurfacesFree[i] = i;	    
	}

	Debug(4, "video/cuvid: created video surface %dx%d with id %d\n",width, height, decoder->SurfacesFree[i]);
}

///
///	Destroy surfaces of CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidDestroySurfaces(CuvidDecoder * decoder)
{
    int i,j;
	CUcontext dummy;
    CUdeviceptr d;

    Debug(3, "video/cuvid: %s\n", __FUNCTION__);
	
	glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
	GlxCheck();

	
	for (i=0;i<decoder->SurfacesNeeded;i++) {
		for (j=0;j<2;j++) {
			if (decoder->cu_res[i][j]) {
				checkCudaErrors(cuGraphicsUnregisterResource(decoder->cu_res[i][j]));
				decoder->cu_res[i][j] = 0;
			}
		}
	}

	
	glDeleteTextures(CODEC_SURFACES_MAX*2,(GLuint*)&decoder->gl_textures);
	GlxCheck();
	
	if (decoder == CuvidDecoders[0]) {   // only wenn last decoder closes
		Debug(3,"Last decoder closes\n");
    	glDeleteBuffers(1,(GLuint *)&vao_buffer);
		if (gl_prog)
			glDeleteProgram(gl_prog);
		gl_prog = 0;
	}
	
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
		decoder->SurfacesFree[i] = -1;
    }
	
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
		decoder->SurfacesUsed[i] = -1;
    }
	
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	CUVID hw decoder
///
///	@returns the oldest free surface
///
static int CuvidGetVideoSurface0(CuvidDecoder * decoder)
{
    int surface;
    int i;

    if (!decoder->SurfaceFreeN) {
//	Error(_("video/cuvid: out of surfaces\n"));
		return -1;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
		decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = -1;
    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	CUVID hw decoder
///	@param surface	surface no longer used
///
static void CuvidReleaseSurface(CuvidDecoder * decoder, int surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
		if (decoder->SurfacesUsed[i] == surface) {
			// no problem, with last used
			decoder->SurfacesUsed[i] = decoder->SurfacesUsed[--decoder->SurfaceUsedN];
			decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
			return;
		}
    }
    Fatal(_("video/cuvid: release surface %#08x, which is not in use\n"), surface);
}

///
///	Debug CUVID decoder frames drop...
///
///	@param decoder	CUVID hw decoder
///
static void CuvidPrintFrames(const CuvidDecoder * decoder)
{
    Debug(3, "video/cuvid: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

int CuvidTestSurfaces() {
	if (atomic_read(&CuvidDecoders[0]->SurfacesFilled) < VIDEO_SURFACES_MAX)
		return 1;
	return 0;
}

///
///	Allocate new CUVID decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared cuvid hardware decoder.
///
static CuvidDecoder *CuvidNewHwDecoder(VideoStream * stream)
{
    CuvidDecoder *decoder;
    int i=0;

	setenv ("DISPLAY", ":0", 0); 

	Debug(3,"Cuvid New HW Decoder\n");
    if ((unsigned)CuvidDecoderN >=	sizeof(CuvidDecoders) / sizeof(*CuvidDecoders)) {
		Error(_("video/cuvid: out of decoders\n"));
		return NULL;
    }
	
    if (i = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, X11DisplayName, NULL, 0)) {
		Fatal("codec: can't allocate HW video codec context err &d",i);
    }
    HwDeviceContext = av_buffer_ref(hw_device_ctx);
	
    if (!(decoder = calloc(1, sizeof(*decoder)))) {
		Error(_("video/cuvid: out of memory\n"));
		return NULL;
    }

    decoder->Window = VideoWindow;
    //decoder->VideoX = 0;		// done by calloc
    //decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
		decoder->SurfacesUsed[i] = -1;
		decoder->SurfacesFree[i] = -1;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
		decoder->SurfacesRb[i] = -1;
    }

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;
    decoder->PixFmt = AV_PIX_FMT_NONE;

#ifdef USE_AUTOCROP
    //decoder->AutoCropBuffer = NULL;	// done by calloc
    //decoder->AutoCropBufferSize = 0;
#endif

    decoder->Stream = stream;
    if (!CuvidDecoderN) {		// FIXME: hack sync on audio
		decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;
    decoder->PTS = AV_NOPTS_VALUE;

    CuvidDecoders[CuvidDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup CUVID.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidCleanup(CuvidDecoder * decoder)
{
    int i,n=0;
    CUcontext dummy;

Debug(3,"Cuvid Clean up\n");
	
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
		CuvidDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
		decoder->SurfacesRb[i] = -1;
    }
    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;

    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidDelHwDecoder(CuvidDecoder * decoder)
{
    int i,n;
Debug(3,"cuvid del hw decoder \n");
	if (decoder == CuvidDecoders[0])
  		pthread_mutex_lock(&VideoLockMutex);

	glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
	GlxCheck();
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
		CuvidDestroySurfaces(decoder);
    }
	if (decoder == CuvidDecoders[0])
  		pthread_mutex_unlock(&VideoLockMutex);

	glXMakeCurrent(XlibDisplay, None, NULL);
    for (i = 0; i < CuvidDecoderN; ++i) {
		if (CuvidDecoders[i] == decoder) {
			CuvidDecoders[i] = NULL;
			// copy last slot into empty slot
			if (i < --CuvidDecoderN) {
				CuvidDecoders[i] = CuvidDecoders[CuvidDecoderN];
			}
//			CuvidCleanup(decoder);
			CuvidPrintFrames(decoder);
#ifdef USE_AUTOCROP
			free(decoder->AutoCropBuffer);
#endif
			free(decoder);
			return;
		}
    }
    Error(_("video/cuvid: decoder not in decoder list.\n"));
}

static int CuvidGlxInit(const char *display_name)
{
    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
        GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxContext);
    }
    if (!GlxEnabled) {
        Error(_("video/glx: glx error\n"));
    }

    return 1;
}
///
///	CUVID cleanup.
///
static void CuvidExit(void)
{
    int i;

    for (i = 0; i < CuvidDecoderN; ++i) {
		if (CuvidDecoders[i]) {
			CuvidDelHwDecoder(CuvidDecoders[i]);
			CuvidDecoders[i] = NULL;
		}
    }
    CuvidDecoderN = 0;
    Debug(3,"CuvidExit\n");
    pthread_mutex_destroy(&CuvidGrabMutex);
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidUpdateOutput(CuvidDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

void SDK_CHECK_ERROR_GL() {
    GLenum gl_error = glGetError();

    if (gl_error != GL_NO_ERROR) {
       Fatal(_("video/cuvid: SDL error %d: %d\n"),gl_error);
    }
}

void
createTextureDst(CuvidDecoder * decoder,int anz, unsigned int size_x, unsigned int size_y, enum AVPixelFormat PixFmt)
{
	
    int n,i,size;
    CUcontext dummy;

	glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
	GlxCheck();
	
    glGenBuffers(1,&vao_buffer);
	GlxCheck();
	
    Debug(3,"video/vdpau: create %d Textures Format %s w %d h %d \n",anz,PixFmt==AV_PIX_FMT_NV12?"NV12":"P010",size_x,size_y);

    // create texture planes
    glGenTextures(CODEC_SURFACES_MAX*2, decoder->gl_textures);
	GlxCheck();
	for (i=0;i<anz;i++) {
		for (n=0;n<2;n++ ) {   // number of planes
			
			glBindTexture(GL_TEXTURE_2D, decoder->gl_textures[i*2+n]);
			GlxCheck();
			// set basic parameters
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					if (PixFmt == AV_PIX_FMT_NV12)
			   glTexImage2D(GL_TEXTURE_2D, 0,n==0?GL_R8 :GL_RG8  ,n==0?size_x:size_x/2, n==0?size_y:size_y/2, 0, n==0?GL_RED:GL_RG , GL_UNSIGNED_BYTE , NULL);
			else
			   glTexImage2D(GL_TEXTURE_2D, 0,n==0?GL_R16:GL_RG16 ,n==0?size_x:size_x/2, n==0?size_y:size_y/2, 0, n==0?GL_RED:GL_RG , GL_UNSIGNED_SHORT, NULL);
			SDK_CHECK_ERROR_GL();
			// register this texture with CUDA

			checkCudaErrors(cuGraphicsGLRegisterImage(&decoder->cu_res[i][n], decoder->gl_textures[i*2+n],GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
			checkCudaErrors(cuGraphicsMapResources(1, &decoder->cu_res[i][n], 0));
			checkCudaErrors(cuGraphicsSubResourceGetMappedArray(&decoder->cu_array[i][n], decoder->cu_res[i][n],0, 0));
			checkCudaErrors(cuGraphicsUnmapResources(1, &decoder->cu_res[i][n], 0));

		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);

}

// copy image and process using CUDA
void generateCUDAImage(CuvidDecoder * decoder,int index, const AVFrame *frame,int image_width , int image_height, int bytes)
{
    int n,version;
    CUcontext dummy=NULL;

    for (n = 0; n < 2; n++) { // 
        // widthInBytes must account for the chroma plane
        // elements being two samples wide.
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
		 	.dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcDevice     = (CUdeviceptr)frame->data[n],
            .srcPitch      = frame->linesize[n],
            .srcY          = 0,
			.dstArray      = decoder->cu_array[index][n],
            .WidthInBytes  = image_width * bytes, 
            .Height        = n==0?image_height:image_height/2 , 
        };
        checkCudaErrors(cuMemcpy2D(&cpy));        
    }
}



///
///	Configure CUVID for new video format.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidSetupOutput(CuvidDecoder * decoder)
{
    uint32_t width;
    uint32_t height;

    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution = VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight, decoder->Interlaced);
    CuvidCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight,decoder->PixFmt);
	
    CuvidUpdateOutput(decoder);		// update aspect/scaling

    window_width = decoder->OutputWidth;
    window_height = decoder->OutputHeight;
    Debug(3,"video/cuvid: init Surfaces sucessfull\n");


}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned CuvidGetVideoSurface(CuvidDecoder * decoder,
    const AVCodecContext * video_ctx)
{

    (void)video_ctx;

    return CuvidGetVideoSurface0(decoder);
}



typedef struct CUVIDContext {
    AVBufferRef *hw_frames_ctx;
    AVFrame *tmp_frame;
} CUVIDContext;


static int cuvid_get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    VideoDecoder        *ist = s->opaque;
    CUVIDContext        *ctx = ist->hwaccel_ctx;
    int ret;

    if (!ctx->hw_frames_ctx) {
	Debug(3,"CUDA fail get buffer\n");
        exit(0);
    }

    ret = av_hwframe_get_buffer(ctx->hw_frames_ctx, frame, 0);
    //ret = avcodec_default_get_buffer2(s, frame, flags);

    Debug(3,"CUDA hwframe got buffer %d\n",ret);
    return ret;
}


///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///

static enum AVPixelFormat Cuvid_get_format(CuvidDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;
    int bitformat16 = 0;
   
    VideoDecoder *ist = video_ctx->opaque;


    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
        Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
        if (*fmt_idx == AV_PIX_FMT_P010LE)
            bitformat16 = 1;
    }

    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
		Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
		// check supported pixel format with entry point
		switch (*fmt_idx) {
			case AV_PIX_FMT_CUDA:
#ifdef VAAPI
			case AV_PIX_FMT_VAAPI_VLD:
#endif
				break;
			default:
				continue;
		}
		break;
    }

	Debug(3,"video profile %d codec id %d\n",video_ctx->profile,video_ctx->codec_id);
    if (*fmt_idx == AV_PIX_FMT_NONE) {
		Error(_("video: no valid pixfmt found\n"));
    }

#ifndef VAAPI
    if (*fmt_idx != AV_PIX_FMT_CUDA) {
		Fatal(_("video: no valid profile found\n"));
    }
#endif
    Debug(3, "video: create decoder 16bit?=%d %dx%d \n",bitformat16, video_ctx->width, video_ctx->height);


    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 1; 
    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;


    if (*fmt_idx == AV_PIX_FMT_CUDA  )  {       // HWACCEL used 
        CuvidCleanup(decoder);
#if 0
		if (init_cuvid(video_ctx,decoder)) {
			Fatal(_("CUVID Init failed\n"));
		}
#endif

		CuvidMessage(2,"CUVID Init ok %dx%d\n",video_ctx->width,video_ctx->height);
        ist->active_hwaccel_id = HWACCEL_CUVID;
        ist->hwaccel_pix_fmt   = AV_PIX_FMT_CUDA;
        decoder->InputWidth = video_ctx->width;
        decoder->InputHeight = video_ctx->height;
        decoder->InputAspect = video_ctx->sample_aspect_ratio;
        if (bitformat16) { 
			decoder->PixFmt = AV_PIX_FMT_YUV420P;     // 10 Bit Planar
			ist->hwaccel_output_format = AV_PIX_FMT_YUV420P;
        } else {
			decoder->PixFmt = AV_PIX_FMT_NV12;        // 8 Bit Planar
			ist->hwaccel_output_format = AV_PIX_FMT_NV12;
        }
        CuvidSetupOutput(decoder);
        return AV_PIX_FMT_CUDA;
    }
	Fatal(_("NO Format valid"));
    return *fmt_idx;
}

#ifdef USE_GRAB

int get_RGB(CuvidDecoder *decoder) {
	uint8_t *base = decoder->grabbase;;
	int width = decoder->grabwidth;
	int height = decoder->grabheight;
	GLuint fb,texture;
	int current,i;
	GLint texLoc;
		
	glGenTextures(1, &texture);
	GlxCheck();
	glBindTexture(GL_TEXTURE_2D, texture);
	GlxCheck();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	GlxCheck();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	GlxCheck();

	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		Debug(3,"video/cuvid: grab Framebuffer is not complete!");
		return 0;
	}
	
	current = decoder->SurfacesRb[decoder->SurfaceRead];
		
	glViewport(0,0,width, height);

	if (gl_prog == 0)
		gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
	
	glUseProgram(gl_prog);
	texLoc = glGetUniformLocation(gl_prog, "texture0");
	glUniform1i(texLoc, 0);
	texLoc = glGetUniformLocation(gl_prog, "texture1");
	glUniform1i(texLoc, 1);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current*2+0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current*2+1]);
	
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	
	render_pass_quad(1);	
	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	
	Debug(3,"Read pixels %d %d\n",width,height);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	
	glReadPixels(0,0,width,height,GL_BGRA,GL_UNSIGNED_BYTE,base);
    GlxCheck();
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1,&fb);
	glDeleteTextures(1,&texture);
	
}

///
///	Grab output surface already locked.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CuvidGrabOutputSurfaceLocked(int *ret_size, int *ret_width, int *ret_height)
{
    int surface,i;

    int rgba_format;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint8_t *base;
    void *data[1];
    uint32_t pitches[1];
    VdpRect source_rect;
    VdpRect output_rect;
	CuvidDecoder *decoder;

	decoder = CuvidDecoders[0];
	if (decoder == NULL)   // no video aktiv
		return NULL;
	
//    surface = CuvidSurfacesRb[CuvidOutputSurfaceIndex];
	
	//	get real surface size
    width = decoder->InputWidth;
	height = decoder->InputHeight;
    
    
    Debug(3, "video/cuvid: grab %dx%d\n", width, height);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
		if (*ret_width <= -64) {	// this is an Atmo grab service request
			int overscan;

			// calculate aspect correct size of analyze image
			width = *ret_width * -1;
			height = (width * source_rect.y1) / source_rect.x1;

			// calculate size of grab (sub) window
			overscan = *ret_height;

			if (overscan > 0 && overscan <= 200) {
			source_rect.x0 = source_rect.x1 * overscan / 1000;
			source_rect.x1 -= source_rect.x0;
			source_rect.y0 = source_rect.y1 * overscan / 1000;
			source_rect.y1 -= source_rect.y0;
			}
		} else {
			if (*ret_width > 0 && (unsigned)*ret_width < width) {
				width = *ret_width;
			}
			if (*ret_height > 0 && (unsigned)*ret_height < height) {
				height = *ret_height;
			}
		}

		Debug(3, "video/cuvid: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
			source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1,
			width, height);


		output_rect.x0 = 0;
		output_rect.y0 = 0;
		output_rect.x1 = width;
		output_rect.y1 = height;
		
		size = width * height * sizeof(uint32_t);
		
		base = malloc(size);
		
		if (!base) {
			Error(_("video/cuvid: out of memory\n"));
			return NULL;
		}
		decoder->grabbase = base;
		decoder->grabwidth = width;
		decoder->grabheight = height;
		decoder->grab = 1;
		
		while(decoder->grab) {
			usleep(1000);				// wait for data
		}
		Debug(3,"got grab data\n");

		if (ret_size) {
			*ret_size = size;
		}
		if (ret_width) {
			*ret_width = width;
		}
		if (ret_height) {
			*ret_height = height;
		}
		return base;
	}

    return NULL;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CuvidGrabOutputSurface(int *ret_size, int *ret_width,
    int *ret_height)
{
    uint8_t *img;

//    pthread_mutex_lock(&CuvidGrabMutex);
//	pthread_mutex_lock(&VideoLockMutex);
    img = CuvidGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
//	pthread_mutex_unlock(&VideoLockMutex);
//    pthread_mutex_unlock(&CuvidGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	CUVID auto-crop support.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidAutoCrop(CuvidDecoder * decoder)
{
    int surface;

    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    int format;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1) %  VIDEO_SURFACES_MAX];

    //	get real surface size (can be different)
    status =
	CuvidVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    CuvidGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    // cache buffer for reuse
	    base = decoder->AutoCropBuffer;
	    if (size > decoder->AutoCropBufferSize) {
		free(base);
		decoder->AutoCropBuffer = malloc(size);
		base = decoder->AutoCropBuffer;
	    }
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = CuvidVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    CuvidGetErrorString(status));
	return;
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
		decoder->CropX = VideoCutLeftRight[decoder->Resolution];
		decoder->CropY =
			(next_state ==
			16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
		decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
		decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

		// FIXME: this overwrites user choosen output position
		// FIXME: resize kills the auto crop values
		// FIXME: support other 4:3 zoom modes
		decoder->OutputX = decoder->VideoX;
		decoder->OutputY = decoder->VideoY;
		decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
		decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
		if (decoder->OutputWidth > decoder->VideoWidth) {
			decoder->OutputWidth = decoder->VideoWidth;
			decoder->OutputY =
			(decoder->VideoHeight - decoder->OutputHeight) / 2;
		} else if (decoder->OutputHeight > decoder->VideoHeight) {
			decoder->OutputHeight = decoder->VideoHeight;
			decoder->OutputX =
			(decoder->VideoWidth - decoder->OutputWidth) / 2;
		}
		Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
			decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
			decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
		// sets AutoCrop->Count
		CuvidUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	CUVID check if auto-crop todo.
///
///	@param decoder	CUVID hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void CuvidCheckAutoCrop(CuvidDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// only 4:3 with 16:9/14:9 inside supported
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    CuvidAutoCrop(decoder);
	} else {
	    decoder->AutoCrop->Count = 0;
	    decoder->AutoCrop->State = 0;
	}
    }
}

///
///	CUVID reset auto-crop.
///
static void CuvidResetAutoCrop(void)
{
    int i;

    for (i = 0; i < CuvidDecoderN; ++i) {
	CuvidDecoders[i]->AutoCrop->State = 0;
	CuvidDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	CUVID hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void CuvidQueueVideoSurface(CuvidDecoder * decoder, int surface, int softdec)
{
    int old;

    ++decoder->FrameCounter;

    // can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    Warning(_("video/vdpau: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
			CuvidPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
			CuvidReleaseSurface(decoder, surface);
	    }
	    return;
	}
    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])!= -1) {
		// now we can release the surface, software surfaces only
		if (softdec) {
			CuvidReleaseSurface(decoder, old);
		}
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d ready\n", surface,	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);
}

extern void Nv12ToBgra32(uint8_t *dpNv12, int nNv12Pitch, uint8_t *dpBgra, int nBgraPitch, int nWidth, int nHeight, int iMatrix,cudaStream_t stream);
extern void P016ToBgra32(uint8_t *dpNv12, int nNv12Pitch, uint8_t *dpBgra, int nBgraPitch, int nWidth, int nHeight, int iMatrix,cudaStream_t stream);
extern void ResizeNv12(unsigned char *dpDstNv12, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrcNv12, int nSrcPitch, int nSrcWidth, int nSrcHeight, unsigned char* dpDstNv12UV);
extern void ResizeP016(unsigned char *dpDstP016, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrcP016, int nSrcPitch, int nSrcWidth, int nSrcHeight, unsigned char* dpDstP016UV);
extern void cudaLaunchNV12toARGBDrv(uint32_t *d_srcNV12, size_t nSourcePitch,uint32_t *d_dstARGB, size_t nDestPitch,uint32_t width, uint32_t height,CUstream streamID);

///
///	Render a ffmpeg frame.
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CuvidRenderFrame(CuvidDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int surface;
    VideoDecoder        *ist = video_ctx->opaque;


    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
		&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
		Debug(3, "video/vdpau: aspect ratio changed\n");

		decoder->InputAspect = frame->sample_aspect_ratio;
printf("new aspect %d:%d\n",frame->sample_aspect_ratio.num,frame->sample_aspect_ratio.den);
		CuvidUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
		&& av_cmp_q(decoder->InputAspect, video_ctx->sample_aspect_ratio)) {
		Debug(3, "video/vdpau: aspect ratio changed\n");

		decoder->InputAspect = video_ctx->sample_aspect_ratio;
		CuvidUpdateOutput(decoder);
    }
#endif


 	//
	//	Check image, format, size
	//
	if ( // decoder->PixFmt != video_ctx->pix_fmt
	     video_ctx->width != decoder->InputWidth
	    || video_ctx->height != decoder->InputHeight) {
Debug(3,"fmt %02d:%02d  width %d:%d hight %d:%d\n,",decoder->PixFmt,video_ctx->pix_fmt ,video_ctx->width, decoder->InputWidth,video_ctx->height, decoder->InputHeight);
//	    decoder->PixFmt = video_ctx->pix_fmt;
	    decoder->InputWidth = video_ctx->width;
	    decoder->InputHeight = video_ctx->height;
	    CuvidCleanup(decoder);
	    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 1;
	    CuvidSetupOutput(decoder);
	}
	//
	//	Copy data from frame to image
	//
	
	if (video_ctx->pix_fmt == AV_PIX_FMT_CUDA) { 
		int w = decoder->InputWidth;
		int h = decoder->InputHeight;

		decoder->ColorSpace = frame->colorspace;     // save colorspace
		decoder->trc = frame->color_trc;
		decoder->color_primaries = frame->color_primaries;
		
		surface = CuvidGetVideoSurface0(decoder);
		
		if (surface == -1)     // no free surfaces
			return;
		
		// copy to texture
		generateCUDAImage(decoder,surface,frame,w,h,decoder->PixFmt==AV_PIX_FMT_NV12?1:2);
// printf("put cuda %d ",surface);		
		CuvidQueueVideoSurface(decoder, surface, 1);
		return;      

	}
	Fatal(_("video/vdpau: pixel format %d not supported\n"),video_ctx->pix_fmt);
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	CUVID hw decoder
///
static void *CuvidGetHwAccelContext(CuvidDecoder * decoder)
{
    int ret,n;
    unsigned int device_count,version;
    CUdevice device;

	Debug(3, "Initializing cuvid hwaccel thread ID:%ld\n",(long int)syscall(186));
//turn NULL;
	if (decoder->cuda_ctx) {
		Debug(3,"schon passiert\n");
		return NULL;
	}
	
    checkCudaErrors(cuInit(0)); 

    checkCudaErrors(cuGLGetDevices(&device_count, &device, 1, CU_GL_DEVICE_LIST_ALL));

    if (decoder->cuda_ctx) {
		cuCtxDestroy (decoder->cuda_ctx);
        decoder->cuda_ctx = NULL;
    }

    checkCudaErrors(cuCtxCreate(&decoder->cuda_ctx, (unsigned int) CU_CTX_SCHED_BLOCKING_SYNC, (CUdevice) 0));
    
    if (decoder->cuda_ctx == NULL)
      Fatal(_("Kein Cuda device gefunden"));

    cuCtxGetApiVersion(decoder->cuda_ctx,&version);
	Debug(3, "***********CUDA API Version %d\n",version);

	return NULL;
}


///
///	Render video surface to output surface.
///
///	@param decoder	CUVID hw decoder
///	@param level	video surface level 0 = bottom
///
static void CuvidMixVideo(CuvidDecoder * decoder, int level)
{
	int current;
	VdpRect video_src_rect;
	VdpRect dst_rect;
	VdpRect dst_video_rect;
	int w = decoder->InputWidth;
	int h = decoder->InputHeight;
	int y;
	GLint texLoc;
	
	size_t nSize = 0;
	static uint32_t lasttime = 0;
		
#ifdef USE_AUTOCROP
	// FIXME: can move to render frame
	CuvidCheckAutoCrop(decoder);
#endif

	if (level) {
		dst_rect.x0 = decoder->VideoX;	// video window output (clip)
		dst_rect.y0 = decoder->VideoY;
		dst_rect.x1 = decoder->VideoX + decoder->VideoWidth;
		dst_rect.y1 = decoder->VideoY + decoder->VideoHeight;
	} else {
		dst_rect.x0 = 0;		// complete window (clip)
		dst_rect.y0 = 0;
		dst_rect.x1 = VideoWindowWidth;
		dst_rect.y1 = VideoWindowHeight;
	}

	video_src_rect.x0 = decoder->CropX;	// video source (crop)
	video_src_rect.y0 = decoder->CropY;
	video_src_rect.x1 = decoder->CropX + decoder->CropWidth;
	video_src_rect.y1 = decoder->CropY + decoder->CropHeight;

	dst_video_rect.x0 = decoder->OutputX;	// video output (scale)
	dst_video_rect.y0 = decoder->OutputY;
	dst_video_rect.x1 = decoder->OutputX + decoder->OutputWidth;
	dst_video_rect.y1 = decoder->OutputY + decoder->OutputHeight;

	current = decoder->SurfacesRb[decoder->SurfaceRead];

	// Render Progressive frame and simple interlaced

	y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
	if (y <0 )
		y = 0;
	glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);

	if (gl_prog == 0)
		gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
	
	glUseProgram(gl_prog);
	texLoc = glGetUniformLocation(gl_prog, "texture0");
	glUniform1i(texLoc, 0);
	texLoc = glGetUniformLocation(gl_prog, "texture1");
	glUniform1i(texLoc, 1);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current*2+0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current*2+1]);	
	
	render_pass_quad(0);
	
	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);

	Debug(4, "video/vdpau: yy video surface %p displayed\n", current, decoder->SurfaceRead);
}

///
///	Create and display a black empty surface.
///
///	@param decoder	CUVID hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///	decoder->Output.. isn't correct setup for radio stations
///
static void CuvidBlackSurface(CuvidDecoder * decoder)
{

	VdpRect source_rect;
	VdpRect output_rect;
	glClear(GL_COLOR_BUFFER_BIT);
return;
#if 0
	source_rect.x0 = 0;
	source_rect.y0 = 0;
	source_rect.x1 = 0;
	source_rect.y1 = 0;

	// FIXME: what happens with PIP?
	if (0) {
		// FIXME: wrong for radio channels
		output_rect.x0 = decoder->OutputX;	// video output (scale)
		output_rect.y0 = decoder->OutputY;
		output_rect.x1 = decoder->OutputX + decoder->OutputWidth;
		output_rect.y1 = decoder->OutputY + decoder->OutputHeight;
	} else {
		output_rect.x0 = decoder->VideoX;
		output_rect.y0 = decoder->VideoY;
		output_rect.x1 = decoder->VideoWidth;
		output_rect.y1 = decoder->VideoHeight;
	}

	// FIXME: double buffered osd disabled
	// CuvidOsdSurfaceIndex always 0 and only 0 valid
#ifdef USE_BITMAP
	status =
	CuvidOutputSurfaceRenderBitmapSurface(CuvidSurfacesRb
	[CuvidOutputSurfaceIndex], &output_rect,
	CuvidOsdBitmapSurface[CuvidOsdSurfaceIndex], &source_rect, NULL, NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
		CuvidGetErrorString(status));
	}
#else
	status =
	CuvidOutputSurfaceRenderOutputSurface(CuvidSurfacesRb
	[CuvidOutputSurfaceIndex], &output_rect,
	CuvidOsdOutputSurface[CuvidOsdSurfaceIndex], &source_rect, NULL, NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
		CuvidGetErrorString(status));
	}
#endif
#endif
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidAdvanceDecoderFrame(CuvidDecoder * decoder)
{
	// next surface, if complete frame is displayed (1 -> 0)
	if (decoder->SurfaceField) {
		int filled;

		// FIXME: this should check the caller
		// check decoder, if new surface is available
		// need 2 frames for progressive
		// need 4 frames for interlaced
			filled = atomic_read(&decoder->SurfacesFilled);
			if (filled <=  1 + 2 * decoder->Interlaced) {
				// keep use of last surface
				++decoder->FramesDuped;
				// FIXME: don't warn after stream start, don't warn during pause
//				printf("video: display buffer empty, duping frame (%d/%d) %d\n",
//				decoder->FramesDuped, decoder->FrameCounter,
//				VideoGetBuffers(decoder->Stream));
				return;
			}
			decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
			atomic_dec(&decoder->SurfacesFilled);
			decoder->SurfaceField = !decoder->Interlaced;
			return;
	}
	// next field
	decoder->SurfaceField = 1;
}

///
///	Display a video frame.
///
static void CuvidDisplayFrame(void)
{

	uint64_t first_time, diff, akt_time;
	static uint64_t last_time = 0;
	int i;
	static unsigned int Count;
	int filled;
	CuvidDecoder *decoder;


	glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);	
	CuvidDecoders[0]->Frameproc = (float)(GetusTicks()-last_time)/1000000.0;
//	printf("Time used %2.2f\n",CuvidDecoders[0]->Frameproc);
    glXWaitVideoSyncSGI (2, (Count + 1) % 2, &Count);   // wait for previous frame to swap
	last_time = GetusTicks();
	
	glClear(GL_COLOR_BUFFER_BIT);
	//
	//	Render videos into output
	//
	///

	for (i = 0; i < CuvidDecoderN; ++i) {

		decoder = CuvidDecoders[i];
		decoder->FramesDisplayed++;
		decoder->StartCounter++;

		filled = atomic_read(&decoder->SurfacesFilled);
		// need 1 frame for progressive, 3 frames for interlaced	
		if (filled < 1 + 2 * decoder->Interlaced) { 
			// FIXME: rewrite MixVideo to support less surfaces
			if ((VideoShowBlackPicture && !decoder->TrickSpeed) || (VideoShowBlackPicture && decoder->Closing < -300)) {
				CuvidBlackSurface(decoder);
				CuvidMessage(3, "video/cuvid: black surface displayed\n");
			}
			continue;
		}
		CuvidMixVideo(decoder, i);
		if (i==0 && decoder->grab) {   // Grab frame 
			get_RGB(decoder);
			decoder->grab = 0;
		}
	}
	//
	   
	//	add osd to surface
	//
	if (OsdShown) {		
#ifndef USE_OPENGLOSD
		glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
		GlxRenderTexture(OsdGlTextures[OsdIndex], 0,0, VideoWindowWidth, VideoWindowHeight);		
#else
		pthread_mutex_lock(&OSDMutex);
		glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext );	
		glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
		GlxCheck();
//		printf("osd %d %dx%d\n",OSDtexture,VideoWindowWidth, VideoWindowHeight);
		GlxRenderTexture(OSDtexture, 0,0, VideoWindowWidth, VideoWindowHeight);
		pthread_mutex_unlock(&OSDMutex);
#endif
//		glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
		glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
	}

	glXGetVideoSyncSGI (&Count);    // get current frame
	glXSwapBuffers(XlibDisplay, VideoWindow);
	
	
	// FIXME: CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC, &CuvidFrameTime);
	for (i = 0; i < CuvidDecoderN; ++i) {
		// remember time of last shown surface
		CuvidDecoders[i]->FrameTime = CuvidFrameTime;
	}
//	xcb_flush(Connection);
}

///
///	Set CUVID decoder video clock.
///
///	@param decoder	CUVID hardware decoder
///	@param pts	audio presentation timestamp
///
void CuvidSetClock(CuvidDecoder * decoder, int64_t pts)
{
	decoder->PTS = pts;
}

///
///	Get CUVID decoder video clock.
///
///	@param decoder	CUVID hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t CuvidGetClock(const CuvidDecoder * decoder)
{
	// pts is the timestamp of the latest decoded frame
	if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
		return AV_NOPTS_VALUE;
	}
	// subtract buffered decoded frames
	if (decoder->Interlaced) {
		/*
		   Info("video: %s =pts field%d #%d\n",
		   Timestamp2String(decoder->PTS),
		   decoder->SurfaceField,
		   atomic_read(&decoder->SurfacesFilled));
		 */
		// 1 field is future, 2 fields are past, + 2 in driver queue
		return decoder->PTS - 20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled) - decoder->SurfaceField - 2 + 2);
	}
	// + 2 in driver queue
	return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled));  // +2
}

///
///	Set CUVID decoder closing stream flag.
///
///	@param decoder	CUVID decoder
///
static void CuvidSetClosing(CuvidDecoder * decoder)
{
	decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	CUVID decoder
///
static void CuvidResetStart(CuvidDecoder * decoder)
{
	decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder	CUVID decoder
///	@param speed	trick speed (0 = normal)
///
static void CuvidSetTrickSpeed(CuvidDecoder * decoder, int speed)
{
	decoder->TrickSpeed = speed;
	decoder->TrickCounter = speed;
	if (speed) {
		decoder->Closing = 0;
	}
}

///
///	Get CUVID decoder statistics.
///
///	@param decoder		CUVID decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void CuvidGetStats(CuvidDecoder * decoder, int *missed, int *duped,
	int *dropped, int *counter, float *frametime)
{
	*missed = decoder->FramesMissed;
	*duped = decoder->FramesDuped;
	*dropped = decoder->FramesDropped;
	*counter = decoder->FrameCounter;
	*frametime = decoder->Frameproc;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	CUVID hw decoder
///
static void CuvidSyncDecoder(CuvidDecoder * decoder)
{
	int err;
	int filled;
	int64_t audio_clock;
	int64_t video_clock;

	err = 0;
	video_clock = CuvidGetClock(decoder);
	filled = atomic_read(&decoder->SurfacesFilled);

	if (!decoder->SyncOnAudio) {
		audio_clock = AV_NOPTS_VALUE;
		// FIXME: 60Hz Mode
		goto skip_sync;
	}
	audio_clock = AudioGetClock();

	// 60Hz: repeat every 5th field
	if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
		if (audio_clock == (int64_t) AV_NOPTS_VALUE || video_clock == (int64_t) AV_NOPTS_VALUE) {
			goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
		goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
		goto skip_sync;
	}
	}
	// TrickSpeed
	if (decoder->TrickSpeed) {
		if (decoder->TrickCounter--) {
			goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
	}
	// at start of new video stream, soft or hard sync video to audio
	if (!VideoSoftStartSync && decoder->StartCounter < VideoSoftStartFrames
		&& video_clock != (int64_t) AV_NOPTS_VALUE
		&& (audio_clock == (int64_t) AV_NOPTS_VALUE
		|| video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
		err = CuvidMessage(4, "video: initial slow down video, frame %d\n",decoder->StartCounter);
		goto out;
	}

	if (decoder->SyncCounter && decoder->SyncCounter--) {
		goto skip_sync;
	}

	if (audio_clock != (int64_t) AV_NOPTS_VALUE
		&& video_clock != (int64_t) AV_NOPTS_VALUE) {
		// both clocks are known
		int diff;

		diff = video_clock - audio_clock - VideoAudioDelay;
		diff = (decoder->LastAVDiff + diff) / 2;
		decoder->LastAVDiff = diff;
//printf("diff %d filled %d\n",diff/90,filled);
		if (abs(diff) > 5000 * 90) {	// more than 5s
			err = CuvidMessage(2, "video: audio/video difference too big\n");
		} else if (diff > 100 * 90) {
			// FIXME: this quicker sync step, did not work with new code!
			err = CuvidMessage(4, "video: slow down video, duping frame\n");
			++decoder->FramesDuped;
			decoder->SyncCounter = 1;
			goto out;
		} else if (diff > 55 * 90) {
			err = CuvidMessage(3, "video: slow down video, duping frame\n");
			++decoder->FramesDuped;
			decoder->SyncCounter = 1;
			goto out;
		} else if (diff < -25 * 90 && filled > 3 + 2 * decoder->Interlaced) {
		    err = CuvidMessage(3, "video: speed up video, droping frame\n");
			++decoder->FramesDropped;
			CuvidAdvanceDecoderFrame(decoder);
	//	    filled = atomic_read(&decoder->SurfacesFilled);
//			Debug(3,"hinter drop frame filled %d\n",atomic_read(&decoder->SurfacesFilled));
			decoder->SyncCounter = 1;
		}
#if defined(DEBUG) || defined(AV_INFO)
		if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
			Debug(3, "video/cuvid: synced after %d frames %dms\n",
			decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
			Info("video/cuvid: synced after %d frames\n",decoder->StartCounter);
#endif
			decoder->StartCounter += 1000;
		}
#endif
	}

  skip_sync:
	// check if next field is available
//JOJO    if (decoder->SurfaceField && filled <= 1 + 2 * decoder->Interlaced) {

	if (decoder->SurfaceField && filled < 1 + 2 * decoder->Interlaced) {
		if (filled < 1 + 2 * decoder->Interlaced) {
			++decoder->FramesDuped;
#if 0
			// FIXME: don't warn after stream start, don't warn during pause
			err = CuvidMessage(1,_("video: decoder buffer empty, duping frame (%d/%d) %d v-buf\n"), decoder->FramesDuped,	decoder->FrameCounter, VideoGetBuffers(decoder->Stream));
			// some time no new picture or black video configured
			if (decoder->Closing < -300 || (VideoShowBlackPicture && decoder->Closing)) {
				// clear ring buffer to trigger black picture
				atomic_set(&decoder->SurfacesFilled, 0);
			}
#endif
		}	
//	Debug(3,"filled zu klein %d  Field %d Interlaced %d\n",filled,decoder->SurfaceField,decoder->Interlaced);
//	goto out;
	}

	CuvidAdvanceDecoderFrame(decoder);
out:
#if 0
	// defined(DEBUG) || defined(AV_INFO)
	// debug audio/video sync
	if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
		if (!err) {
			CuvidMessage(0, NULL);
		}
		Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d%+d v-buf\n",
			Timestamp2String(video_clock),
			abs((video_clock - audio_clock) / 90) <
			8888 ? ((video_clock - audio_clock) / 90) : 8888,
			AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
			VideoGetBuffers(decoder->Stream),
			decoder->Interlaced ? 2 * atomic_read(&decoder->SurfacesFilled)
			- decoder->SurfaceField : atomic_read(&decoder->SurfacesFilled),
			CuvidOutputSurfaceQueued);
		if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
			CuvidPrintFrames(decoder);
		}
	}
#endif
	return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void CuvidSyncFrame(void)
{
	int i;

	//
	//	Sync video decoder to audio
	//
	for (i = 0; i < CuvidDecoderN; ++i) {
		CuvidSyncDecoder(CuvidDecoders[i]);
	}
}

///
///	Sync and display surface.
///
static void CuvidSyncDisplayFrame(void)
{
	CuvidDisplayFrame();
	CuvidSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CuvidSyncRenderFrame(CuvidDecoder * decoder,
	const AVCodecContext * video_ctx, const AVFrame * frame)
{
	// FIXME: temp debug
	if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
		Debug(3, "video: render frame pts %s\n",
		Timestamp2String(frame->pkt_pts));
	}
#ifdef DEBUG
	if (!atomic_read(&decoder->SurfacesFilled)) {
		Debug(4, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
	}
#endif

	// if video output buffer is full, wait and display surface.
	// loop for interlace
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
		Fatal("video/cuvid: this code part shouldn't be used\n");
		return;
	}

	if (!decoder->Closing) {
		VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
	}
	CuvidRenderFrame(decoder, video_ctx, frame);
}


///
///	Set CUVID background color.
///
///	@param rgba	32 bit RGBA color.
///
static void CuvidSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set CUVID video mode.
///
static void CuvidSetVideoMode(void)
{
	int i;

Debug(3,"Set video mode %dx%d\n",VideoWindowWidth,VideoWindowHeight);

	if (GlxEnabled) {
		GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxThreadContext);
	}

	for (i = 0; i < CuvidDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
		CuvidDecoders[i]->VideoX = 0;
		CuvidDecoders[i]->VideoY = 0;
		CuvidDecoders[i]->VideoWidth = VideoWindowWidth;
		CuvidDecoders[i]->VideoHeight = VideoWindowHeight;
		CuvidUpdateOutput(CuvidDecoders[i]);
		
	}
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a CUVID display.
///
static void CuvidDisplayHandlerThread(void)
{
	int i;
	int err=0;
	int allfull;
	int decoded;
	int filled;
	struct timespec nowtime;
	CuvidDecoder *decoder;

	allfull = 1;
	decoded = 0;
	pthread_mutex_lock(&VideoLockMutex);
	for (i = 0; i < CuvidDecoderN; ++i) {

		decoder = CuvidDecoders[i];
		//
		// fill frame output ring buffer
		//
		filled = atomic_read(&decoder->SurfacesFilled);

//if (filled <= 1 +  2 * decoder->Interlaced) {
		if (filled < 5) {
			// FIXME: hot polling
			// fetch+decode or reopen
			allfull = 0;
			err = VideoDecodeInput(decoder->Stream);
		} else {
			err = VideoPollInput(decoder->Stream);
		}
		// decoder can be invalid here
		if (err) {
			// nothing buffered?
			if (err == -1 && decoder->Closing) {
				decoder->Closing--;
				if (!decoder->Closing) {
					Debug(3, "video/cuvid: closing eof\n");
					decoder->Closing = -1;
				}
			}
			continue;
		}
		decoded = 1;
	}
	pthread_mutex_unlock(&VideoLockMutex);

	if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
		usleep(1 * 100);
	}
	// all decoder buffers are full
	// and display is not preempted
	// speed up filling display queue, wait on display queue empty
	if (!allfull) {
		clock_gettime(CLOCK_MONOTONIC, &nowtime);
		// time for one frame over?
		if ((nowtime.tv_sec - CuvidFrameTime.tv_sec) * 1000 * 1000 * 1000 +
			(nowtime.tv_nsec - CuvidFrameTime.tv_nsec) < 15 * 1000 * 1000) {
			return;
		}
	}

	pthread_mutex_lock(&VideoLockMutex);
	CuvidSyncDisplayFrame();
	pthread_mutex_unlock(&VideoLockMutex);
	
}

#else

#define CuvidDisplayHandlerThread	NULL

#endif

#endif

///
///	Set video output position.
///
///	@param decoder	CUVID hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void CuvidSetOutputPosition(CuvidDecoder * decoder, int x, int y,
	int width, int height)
{
	Debug(3, "video/cuvid: output %dx%d%+d%+d\n", width, height, x, y);

	decoder->VideoX = x;
	decoder->VideoY = y;
	decoder->VideoWidth = width;
	decoder->VideoHeight = height;

	// next video pictures are automatic rendered to correct position
}

//----------------------------------------------------------------------------
//	CUVID OSD
//----------------------------------------------------------------------------

static const uint8_t OsdZeros[4096 * 2160 * 4];	///< 0 for clear osd


///
///	CUVID module.
///
static const VideoModule CuvidModule = {
	.Name = "cuvid",
	.Enabled = 1,
	.NewHwDecoder =	(VideoHwDecoder * (*const)(VideoStream *)) CuvidNewHwDecoder,
	.DelHwDecoder = (void (*const) (VideoHwDecoder *))CuvidDelHwDecoder,
	.GetSurface = (unsigned (*const) (VideoHwDecoder *, const AVCodecContext *))CuvidGetVideoSurface,
    .ReleaseSurface = (void (*const) (VideoHwDecoder *, unsigned))CuvidReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Cuvid_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))CuvidSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))	CuvidGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))CuvidSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))CuvidGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))CuvidSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))CuvidResetStart,
    .SetTrickSpeed = (void (*const) (const VideoHwDecoder *, int))CuvidSetTrickSpeed,
    .GrabOutput = CuvidGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, float *))CuvidGetStats,
    .SetBackground = CuvidSetBackground,
    .SetVideoMode = CuvidSetVideoMode,
  //  .ResetAutoCrop = CuvidResetAutoCrop,
    .DisplayHandlerThread = CuvidDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
//    .OsdClear = CuvidOsdClear,
//    .OsdDrawARGB = CuvidOsdDrawARGB,
//    .OsdInit = CuvidOsdInit,
//    .OsdExit = CuvidOsdExit,
    .Exit = CuvidExit,
    .Init = CuvidGlxInit,
};

#endif

//----------------------------------------------------------------------------
//	NOOP
//----------------------------------------------------------------------------

///
///	Allocate new noop decoder.
///
///	@param stream	video stream
///
///	@returns always NULL.
///
static VideoHwDecoder *NoopNewHwDecoder(
    __attribute__ ((unused)) VideoStream * stream)
{
    return NULL;
}

///
///	Release a surface.
///
///	Can be called while exit.
///
///	@param decoder	noop hw decoder
///	@param surface	surface no longer used
///
static void NoopReleaseSurface(
    __attribute__ ((unused)) VideoHwDecoder * decoder, __attribute__ ((unused))
    unsigned surface)
{
}

///
///	Set noop background color.
///
///	@param rgba	32 bit RGBA color.
///
static void NoopSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Noop initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void NoopOsdInit( __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height)
{
}

///
///	Draw OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void NoopOsdDrawARGB( __attribute__ ((unused))
    int xi, __attribute__ ((unused))
    int yi, __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height, __attribute__ ((unused))
    int pitch, __attribute__ ((unused))
    const uint8_t * argb, __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y)
{
}

///
///	Noop setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns always true.
///
static int NoopInit(const char *display_name)
{
    Info("video/noop: noop driver running on display '%s'\n", display_name);
    return 1;
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a noop display.
///
static void NoopDisplayHandlerThread(void)
{
    // avoid 100% cpu use
    usleep(20 * 1000);
#if 0
    // this can't be canceled
    if (XlibDisplay) {
	XEvent event;

	XPeekEvent(XlibDisplay, &event);
    }
#endif
}

#else

#define NoopDisplayHandlerThread	NULL

#endif

///
///	Noop void function.
///
static void NoopVoid(void)
{
}

///
///	Noop video module.
///
static const VideoModule NoopModule = {
    .Name = "noop",
    .Enabled = 1,
    .NewHwDecoder = NoopNewHwDecoder,
#if 0
    // can't be called:
    .DelHwDecoder = NoopDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))NoopGetSurface,
#endif
    .ReleaseSurface = NoopReleaseSurface,
#if 0
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Noop_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))NoopSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	DummyGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))NoopSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))NoopGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))NoopSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))NoopResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))NoopSetTrickSpeed,
    .GrabOutput = NoopGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, float *))NoopGetStats,
#endif
    .SetBackground = NoopSetBackground,
    .SetVideoMode = NoopVoid,
    .ResetAutoCrop = NoopVoid,
    .DisplayHandlerThread = NoopDisplayHandlerThread,
    .OsdClear = NoopVoid,
    .OsdDrawARGB = NoopOsdDrawARGB,
    .OsdInit = NoopOsdInit,
    .OsdExit = NoopVoid,
    .Init = NoopInit,
    .Exit = NoopVoid,
};

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///	@todo I use glTexImage2D to clear the texture, are there faster and
///	better ways to clear a texture?
///
void VideoOsdClear(void)
{
    VideoThreadLock();

    VideoUsedModule->OsdClear();

    OsdDirtyX = OsdWidth;		// reset dirty area
    OsdDirtyY = OsdHeight;
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
    OsdShown = 0;

    VideoThreadUnlock();
}

///
///	Draw an OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
void VideoOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    VideoThreadLock();
    // update dirty area
    if (x < OsdDirtyX) {
	if (OsdDirtyWidth) {
	    OsdDirtyWidth += OsdDirtyX - x;
	}
	OsdDirtyX = x;
    }
    if (y < OsdDirtyY) {
	if (OsdDirtyHeight) {
	    OsdDirtyHeight += OsdDirtyY - y;
	}
	OsdDirtyY = y;
    }
    if (x + width > OsdDirtyX + OsdDirtyWidth) {
	OsdDirtyWidth = x + width - OsdDirtyX;
    }
    if (y + height > OsdDirtyY + OsdDirtyHeight) {
	OsdDirtyHeight = y + height - OsdDirtyY;
    }
    Debug(4, "video: osd dirty %dx%d%+d%+d -> %dx%d%+d%+d\n", width, height, x,
	y, OsdDirtyWidth, OsdDirtyHeight, OsdDirtyX, OsdDirtyY);

    VideoUsedModule->OsdDrawARGB(xi, yi, width, height, pitch, argb, x, y);
    OsdShown = 1;

    VideoThreadUnlock();
}

#ifdef USE_OPENGLOSD
void ActivateOsd(GLuint texture, int x, int y, int xsize, int ysize) {
	OsdShown = 1;
	OSDtexture = texture;
	OSDx = x;
	OSDy = y;
	OSDxsize = xsize;
	OSDysize = ysize;
}
#endif



///
///	Get OSD size.
///
///	@param[out] width	OSD width
///	@param[out] height	OSD height
///
void VideoGetOsdSize(int *width, int *height)
{
    *width = 1920;
    *height = 1080;			// unknown default
    if (OsdWidth && OsdHeight) {
		*width = OsdWidth;
		*height = OsdHeight;
    }
}

///	Set OSD Size.
///
///	@param width	OSD width
///	@param height	OSD height
///
void VideoSetOsdSize(int width, int height)
{

    if (OsdConfigWidth != width || OsdConfigHeight != height) {
		VideoOsdExit();
		OsdConfigWidth = width;
		OsdConfigHeight = height;
		VideoOsdInit();
    }
}

///
///	Set the 3d OSD mode.
///
///	@param mode	OSD mode (0=off, 1=SBS, 2=Top Bottom)
///
void VideoSetOsd3DMode(int mode)
{
    Osd3DMode = mode;
}

///
///	Setup osd.
///
///	FIXME: looking for BGRA, but this fourcc isn't supported by the
///	drawing functions yet.
///
void VideoOsdInit(void)
{
    if (OsdConfigWidth && OsdConfigHeight) {
		OsdWidth = OsdConfigWidth;
		OsdHeight = OsdConfigHeight;
    } else {
		OsdWidth = VideoWindowWidth;
		OsdHeight = VideoWindowHeight;
    }

    VideoThreadLock();
    VideoUsedModule->OsdInit(OsdWidth, OsdHeight);
    VideoThreadUnlock();
    VideoOsdClear();
}

///
///	Cleanup OSD.
///
void VideoOsdExit(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdExit();
    VideoThreadUnlock();
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
}

//----------------------------------------------------------------------------
//	Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int, const char *);

///
///	Handle XLib I/O Errors.
///
///	@param display	display with i/o error
///
static int VideoIOErrorHandler( __attribute__ ((unused)) Display * display)
{

    Error(_("video: fatal i/o error\n"));
    // should be called from VideoThread
    if (VideoThread && VideoThread == pthread_self()) {
	Debug(3, "video: called from video thread\n");
		VideoUsedModule = &NoopModule;
		XlibDisplay = NULL;
		VideoWindow = XCB_NONE;
#ifdef USE_VIDEO_THREAD
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_cond_destroy(&VideoWakeupCond);
		pthread_mutex_destroy(&VideoLockMutex);
		pthread_mutex_destroy(&VideoMutex);
		VideoThread = 0;
		pthread_exit("video thread exit");
#endif
    }
    do {
	sleep(1000);
    } while (1);			// let other threads running

    return -1;
}

///
///	Handle X11 events.
///
///	@todo	Signal WmDeleteMessage to application.
///
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;
    const char *keynam;
    char buf[64];
    char letter[64];
    int letter_len;
    uint32_t values[1];

    VideoThreadLock();
    XNextEvent(XlibDisplay, &event);
    VideoThreadUnlock();
    switch (event.type) {
	case ClientMessage:
	    Debug(3, "video/event: ClientMessage\n");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		Debug(3, "video/event: wm-delete-message\n");
		FeedKeyPress("XKeySym", "Close", 0, 0, NULL);
	    }
	    break;

	case MapNotify:
	    Debug(3, "video/event: MapNotify\n");
	    // �wm workaround
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, &VideoBlankCursor);
	    VideoThreadUnlock();
	    VideoBlankTick = 0;
	    break;
	case Expose:
	    //Debug(3, "video/event: Expose\n");
	    break;
	case ReparentNotify:
	    Debug(3, "video/event: ReparentNotify\n");
	    break;
	case ConfigureNotify:
	    //Debug(3, "video/event: ConfigureNotify\n");
	    VideoSetVideoMode(event.xconfigure.x, event.xconfigure.y,
		event.xconfigure.width, event.xconfigure.height);
	    break;
	case ButtonPress:
	    VideoSetFullscreen(-1);
	    break;
	case KeyPress:
	    VideoThreadLock();
	    letter_len =
		XLookupString(&event.xkey, letter, sizeof(letter) - 1, &keysym,
		NULL);
	    VideoThreadUnlock();
	    if (letter_len < 0) {
		letter_len = 0;
	    }
	    letter[letter_len] = '\0';
	    if (keysym == NoSymbol) {
		Warning(_("video/event: No symbol for %d\n"),
		    event.xkey.keycode);
		break;
	    }
	    VideoThreadLock();
	    keynam = XKeysymToString(keysym);
	    VideoThreadUnlock();
	    // check for key modifiers (Alt/Ctrl)
	    if (event.xkey.state & (Mod1Mask | ControlMask)) {
		if (event.xkey.state & Mod1Mask) {
		    strcpy(buf, "Alt+");
		} else {
		    buf[0] = '\0';
		}
		if (event.xkey.state & ControlMask) {
		    strcat(buf, "Ctrl+");
		}
		strncat(buf, keynam, sizeof(buf) - 10);
		keynam = buf;
	    }
	    FeedKeyPress("XKeySym", keynam, 0, 0, letter);
	    break;
	case KeyRelease:
	    break;
	case MotionNotify:
	    values[0] = XCB_NONE;
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, values);
	    VideoThreadUnlock();
	    VideoBlankTick = GetMsTicks();
	    break;
	default:
#if 0
	    if (XShmGetEventBase(XlibDisplay) + ShmCompletion == event.type) {
		// printf("ShmCompletion\n");
	    }
#endif
	    Debug(3, "Unsupported event type %d\n", event.type);
	    break;
    }
}

///
///	Poll all x11 events.
///
void VideoPollEvent(void)
{
    // hide cursor, after xx ms
    if (VideoBlankTick && VideoWindow != XCB_NONE && VideoBlankTick + 200 < GetMsTicks()) {
		VideoThreadLock();
		xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, &VideoBlankCursor);
		VideoThreadUnlock();
		VideoBlankTick = 0;
    }
    while (XlibDisplay) {
		VideoThreadLock();
		if (!XPending(XlibDisplay)) {
			VideoThreadUnlock();
			break;
		}
		VideoThreadUnlock();
		VideoEvent();
    }
}
#ifdef USE_OPENGLOSD
void VideoSetVideoEventCallback(void (*videoEventCallback)(void))
{
    VideoEventCallback = videoEventCallback;
}
#endif
//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

#ifdef USE_VIDEO_THREAD

///
///	Lock video thread.
///
static void VideoThreadLock(void)
{
    if (VideoThread) {
		if (pthread_mutex_lock(&VideoLockMutex)) {
			Error(_("video: can't lock thread\n"));
		}
    }
}

///
///	Unlock video thread.
///
static void VideoThreadUnlock(void)
{
    if (VideoThread) {
		if (pthread_mutex_unlock(&VideoLockMutex)) {
			Error(_("video: can't unlock thread\n"));
		}
	}
}

///
///	Video render thread.
///
static void *VideoDisplayHandlerThread(void *dummy)
{
	prctl(PR_SET_NAME,"cuvid video",0,0,0);
	

    if (GlxEnabled) {
		Debug(3, "video/glx: thread context %p <-> %p\n",glXGetCurrentContext(), GlxThreadContext);
		Debug(3, "video/glx: context %p <-> %p\n", glXGetCurrentContext(),GlxContext);

		GlxThreadContext = glXCreateContext(XlibDisplay, GlxVisualInfo, GlxSharedContext,GL_TRUE);
		if (!GlxThreadContext) {
			Error(_("video/glx: can't create glx context\n"));
			return NULL;
		}
		// set glx context
		GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxThreadContext);
    }


    for (;;) {
	// fix dead-lock with CuvidExit
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		VideoPollEvent();

		VideoUsedModule->DisplayHandlerThread();
    }

    return dummy;
}

///
///	Initialize video threads.
///
static void VideoThreadInit(void)
{

    glXMakeCurrent(XlibDisplay, None, NULL);

    pthread_mutex_init(&VideoMutex, NULL);
    pthread_mutex_init(&VideoLockMutex, NULL);
	pthread_mutex_init(&OSDMutex, NULL);
    pthread_cond_init(&VideoWakeupCond, NULL);
    pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
}

///
///	Exit and cleanup video threads.
///
static void VideoThreadExit(void)
{
    if (VideoThread) {
		void *retval;

		Debug(3, "video: video thread canceled\n");
		//VideoThreadLock();
		// FIXME: can't cancel locked
		if (pthread_cancel(VideoThread)) {
			Error(_("video: can't queue cancel video display thread\n"));
		}
		//VideoThreadUnlock();
		if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
			Error(_("video: can't cancel video display thread\n"));
		}
		VideoThread = 0;
		pthread_cond_destroy(&VideoWakeupCond);
		pthread_mutex_destroy(&VideoLockMutex);
		pthread_mutex_destroy(&VideoMutex);
		pthread_mutex_destroy(&OSDMutex);
    }

}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoDisplayWakeup(void)
{
    if (!XlibDisplay) {			// not yet started
		return;
    }

    if (!VideoThread) {			// start video thread, if needed
		VideoThreadInit();
    }
}

#endif

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Table of all video modules.
///
static const VideoModule *VideoModules[] = {

    &CuvidModule,
    &NoopModule
};

///
///	Video hardware decoder
///
struct _video_hw_decoder_
{
    union
    {
		CuvidDecoder Cuvid;		///< cuvid decoder structure
    };
};

///
///	Allocate new video hw decoder.
///
///	@param stream	video stream
///
///	@returns a new initialized video hardware decoder.
///
VideoHwDecoder *VideoNewHwDecoder(VideoStream * stream)
{
    VideoHwDecoder *hw;

    VideoThreadLock();
    hw = VideoUsedModule->NewHwDecoder(stream);
    VideoThreadUnlock();

    return hw;
}

///
///	Destroy a video hw decoder.
///
///	@param hw_decoder	video hardware decoder
///
void VideoDelHwDecoder(VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
#ifdef DEBUG
		if (!pthread_equal(pthread_self(), VideoThread)) {
			Debug(3, "video: should only be called from inside the thread\n");
		}
#endif
		// only called from inside the thread
		//VideoThreadLock();
		VideoUsedModule->DelHwDecoder(hw_decoder);
		//VideoThreadUnlock();
    }
}

///
///	Get a free hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface or invalid surface
///
unsigned VideoGetSurface(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx)
{
    return VideoUsedModule->GetSurface(hw_decoder, video_ctx);
}

///
///	Release a hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param surface		surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * hw_decoder, unsigned surface)
{
    // FIXME: must be guarded against calls, after VideoExit
    VideoUsedModule->ReleaseSurface(hw_decoder, surface);
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param fmt		is the list of formats which are supported by
///				the codec, it is terminated by -1 as 0 is a
///				valid format, the formats are ordered by
///				quality.
///
enum AVPixelFormat Video_get_format(VideoHwDecoder * hw_decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
#ifdef DEBUG
    int ms_delay;

    // FIXME: use frame time
    ms_delay = (1000 * video_ctx->time_base.num * video_ctx->ticks_per_frame)
	/ video_ctx->time_base.den;

    Debug(3, "video: ready %s %2dms/frame %dms\n",
	Timestamp2String(VideoGetClock(hw_decoder)), ms_delay,
	GetMsTicks() - VideoSwitch);
#endif

    return VideoUsedModule->get_format(hw_decoder, video_ctx, fmt);
}

///
///	Display a ffmpeg frame
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
#if 0
    fprintf(stderr, "video: render frame pts %s closing %d\n",
	Timestamp2String(frame->pkt_pts), hw_decoder->Cuvid.Closing);
#endif
    if (frame->repeat_pict && !VideoIgnoreRepeatPict) {
		Warning(_("video: repeated pict %d found, but not handled\n"), frame->repeat_pict);
    }
    VideoUsedModule->RenderFrame(hw_decoder, video_ctx, frame);
}

///
///	Get hwaccel context for ffmpeg.
///
///	FIXME: new ffmpeg supports cuvid hw context
///
///	@param hw_decoder	video hardware decoder (must be VA-API)
///
void *VideoGetHwAccelContext(VideoHwDecoder * hw_decoder)
{
    return VideoUsedModule->GetHwAccelContext(hw_decoder);
}



///
///	Set video clock.
///
///	@param hw_decoder	video hardware decoder
///	@param pts		audio presentation timestamp
///
void VideoSetClock(VideoHwDecoder * hw_decoder, int64_t pts)
{
    Debug(3, "video: set clock %s\n", Timestamp2String(pts));
    if (hw_decoder) {
		VideoUsedModule->SetClock(hw_decoder, pts);
    }
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t VideoGetClock(const VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
	return VideoUsedModule->GetClock(hw_decoder);
    }
    return AV_NOPTS_VALUE;
}

///
///	Set closing stream flag.
///
///	@param hw_decoder	video hardware decoder
///
void VideoSetClosing(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: set closing\n");
    VideoUsedModule->SetClosing(hw_decoder);
    // clear clock to avoid further sync
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Reset start of frame counter.
///
///	@param hw_decoder	video hardware decoder
///
void VideoResetStart(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: reset start\n");
    VideoUsedModule->ResetStart(hw_decoder);
    // clear clock to trigger new video stream
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Set trick play speed.
///
///	@param hw_decoder	video hardware decoder
///	@param speed		trick speed (0 = normal)
///
void VideoSetTrickSpeed(VideoHwDecoder * hw_decoder, int speed)
{
    Debug(3, "video: set trick-speed %d\n", speed);
    VideoUsedModule->SetTrickSpeed(hw_decoder, speed);
}

///
///	Grab full screen image.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug(3, "video: grab\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
		uint8_t *data;
		uint8_t *rgb;
		char buf[64];
		int i;
		int n;
		int scale_width;
		int scale_height;
		int x;
		int y;
		double src_x;
		double src_y;
		double scale_x;
		double scale_y;

		scale_width = *width;
		scale_height = *height;
		n = 0;
		data = VideoUsedModule->GrabOutput(size, width, height);
		if (data == NULL)
			return NULL;

		if (scale_width <= 0) {
			scale_width = *width;
		}
		if (scale_height <= 0) {
			scale_height = *height;
		}
		// hardware didn't scale for us, use simple software scaler
		if (scale_width != *width && scale_height != *height) {
			if (write_header) {
				n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n",
					scale_width, scale_height);
			}
			rgb = malloc(scale_width * scale_height * 3 + n);
			if (!rgb) {
				Error(_("video: out of memory\n"));
				free(data);
				return NULL;
			}
			*size = scale_width * scale_height * 3 + n;
			memcpy(rgb, buf, n);	// header

			scale_x = (double)*width / scale_width;
			scale_y = (double)*height / scale_height;

			src_y = 0.0;
			for (y = 0; y < scale_height; y++) {
				int o;

				src_x = 0.0;
				o = (int)src_y **width;

				for (x = 0; x < scale_width; x++) {
					i = 4 * (o + (int)src_x);

					rgb[n + (x + y * scale_width) * 3 + 0] = data[i + 2];
					rgb[n + (x + y * scale_width) * 3 + 1] = data[i + 1];
					rgb[n + (x + y * scale_width) * 3 + 2] = data[i + 0];

					src_x += scale_x;
				}

				src_y += scale_y;
			}

			*width = scale_width;
			*height = scale_height;

			// grabed image of correct size convert BGRA -> RGB
		} else {
			if (write_header) {
				n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", *width,
					*height);
			}
			rgb = malloc(*width * *height * 3 + n);
			if (!rgb) {
				Error(_("video: out of memory\n"));
				free(data);
				return NULL;
			}
			memcpy(rgb, buf, n);	// header

			for (i = 0; i < *size / 4; ++i) {	// convert bgra -> rgb
				rgb[n + i * 3 + 0] = data[i * 4 + 2];
				rgb[n + i * 3 + 1] = data[i * 4 + 1];
				rgb[n + i * 3 + 2] = data[i * 4 + 0];
			}

			*size = *width * *height * 3 + n;
		}
		free(data);

		return rgb;
    } else
#endif
    {
		Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    (void)write_header;
    return NULL;
}

///
///	Grab image service.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrabService(int *size, int *width, int *height)
{
    Debug(3, "video: grab service\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
		return VideoUsedModule->GrabOutput(size, width, height);
    } else
#endif
    {
		Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    return NULL;
}

///
///	Get decoder statistics.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VideoGetStats(VideoHwDecoder * hw_decoder, int *missed, int *duped,
    int *dropped, int *counter, float *frametime)
{
    VideoUsedModule->GetStats(hw_decoder, missed, duped, dropped, counter, frametime);
}

///
///	Get decoder video stream size.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void VideoGetVideoSize(VideoHwDecoder * hw_decoder, int *width, int *height,
    int *aspect_num, int *aspect_den)
{
    *width = 1920;
    *height = 1080;
    *aspect_num = 16;
    *aspect_den = 9;
    // FIXME: test to check if working, than make module function

    if (VideoUsedModule == &CuvidModule) {
		*width = hw_decoder->Cuvid.InputWidth;
		*height = hw_decoder->Cuvid.InputHeight;
		av_reduce(aspect_num, aspect_den,
			hw_decoder->Cuvid.InputWidth * hw_decoder->Cuvid.InputAspect.num,
			hw_decoder->Cuvid.InputHeight * hw_decoder->Cuvid.InputAspect.den,
			1024 * 1024);
    }


}

#ifdef USE_SCREENSAVER

//----------------------------------------------------------------------------
//	DPMS / Screensaver
//----------------------------------------------------------------------------

///
///	Suspend X11 screen saver.
///
///	@param connection	X11 connection to enable/disable screensaver
///	@param suspend		True suspend screensaver,
///				false enable screensaver
///
static void X11SuspendScreenSaver(xcb_connection_t * connection, int suspend)
{
    const xcb_query_extension_reply_t *query_extension_reply;

    query_extension_reply =
	xcb_get_extension_data(connection, &xcb_screensaver_id);
    if (query_extension_reply && query_extension_reply->present) {
		xcb_screensaver_query_version_cookie_t cookie;
		xcb_screensaver_query_version_reply_t *reply;

		Debug(3, "video: screen saver extension present\n");

		cookie =
			xcb_screensaver_query_version_unchecked(connection,
			XCB_SCREENSAVER_MAJOR_VERSION, XCB_SCREENSAVER_MINOR_VERSION);
		reply = xcb_screensaver_query_version_reply(connection, cookie, NULL);
		if (reply
			&& (reply->server_major_version >= XCB_SCREENSAVER_MAJOR_VERSION)
			&& (reply->server_minor_version >= XCB_SCREENSAVER_MINOR_VERSION)
			) {
			xcb_screensaver_suspend(connection, suspend);
		}
		free(reply);
    }
}

///
///	DPMS (Display Power Management Signaling) extension available.
///
///	@param connection	X11 connection to check for DPMS
///
static int X11HaveDPMS(xcb_connection_t * connection)
{
    static int have_dpms = -1;
    const xcb_query_extension_reply_t *query_extension_reply;

    if (have_dpms != -1) {		// already checked
		return have_dpms;
    }

    have_dpms = 0;
    query_extension_reply = xcb_get_extension_data(connection, &xcb_dpms_id);
    if (query_extension_reply && query_extension_reply->present) {
		xcb_dpms_get_version_cookie_t cookie;
		xcb_dpms_get_version_reply_t *reply;
		int major;
		int minor;

		Debug(3, "video: dpms extension present\n");

		cookie =
			xcb_dpms_get_version_unchecked(connection, XCB_DPMS_MAJOR_VERSION,
			XCB_DPMS_MINOR_VERSION);
		reply = xcb_dpms_get_version_reply(connection, cookie, NULL);
		// use locals to avoid gcc warning
		major = XCB_DPMS_MAJOR_VERSION;
		minor = XCB_DPMS_MINOR_VERSION;
		if (reply && (reply->server_major_version >= major)
			&& (reply->server_minor_version >= minor)
			) {
			have_dpms = 1;
		}
		free(reply);
    }
    return have_dpms;
}

///
///	Disable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to disable DPMS
///
static void X11DPMSDisable(xcb_connection_t * connection)
{
    if (X11HaveDPMS(connection)) {
	xcb_dpms_info_cookie_t cookie;
	xcb_dpms_info_reply_t *reply;

	cookie = xcb_dpms_info_unchecked(connection);
	reply = xcb_dpms_info_reply(connection, cookie, NULL);
	if (reply) {
	    if (reply->state) {
		Debug(3, "video: dpms was enabled\n");
		xcb_dpms_disable(connection);	// monitor powersave off
	    }
	    free(reply);
	}
	DPMSDisabled = 1;
    }
}

///
///	Reenable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to enable DPMS
///
static void X11DPMSReenable(xcb_connection_t * connection)
{
    if (DPMSDisabled && X11HaveDPMS(connection)) {
	xcb_dpms_enable(connection);	// monitor powersave on
	xcb_dpms_force_level(connection, XCB_DPMS_DPMS_MODE_ON);
	DPMSDisabled = 0;
    }
}

#else

    /// dummy function: Suspend X11 screen saver.
#define X11SuspendScreenSaver(connection, suspend)
    /// dummy function: Disable X11 DPMS.
#define X11DPMSDisable(connection)
    /// dummy function: Reenable X11 DPMS.
#define X11DPMSReenable(connection)

#endif

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Create main window.
///
///	@param parent	parent of new window
///	@param visual	visual of parent
///	@param depth	depth of parent
///
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual,
    uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;
    xcb_pixmap_t pixmap;
    xcb_cursor_t cursor;

    Debug(3, "video: visual %#0x depth %d\n", visual, depth);

    // Color map
    VideoColormap = xcb_generate_id(Connection);
    xcb_create_colormap(Connection, XCB_COLORMAP_ALLOC_NONE, VideoColormap,
	parent, visual);

    values[0] = 0;
    values[1] = 0;
    values[2] =
	XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
	XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
	XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
	XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    values[3] = VideoColormap;
    VideoWindow = xcb_generate_id(Connection);
    xcb_create_window(Connection, depth, VideoWindow, parent, VideoWindowX,
	VideoWindowY, VideoWindowWidth, VideoWindowHeight, 0,
	XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
	XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK |
	XCB_CW_COLORMAP, values);

    // define only available with xcb-utils-0.3.8
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,	sizeof("softhdcuvid") - 1, "softhdcuvid");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,	sizeof("softhdcuvid") - 1, "softhdcuvid");
#endif
    // define only available with xcb-utils-0.3.6
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, sizeof("softhdcuvid") - 1, "softhdcuvid");
    xcb_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, sizeof("softhdcuvid") - 1, "softhdcuvid");
#endif

    // FIXME: size hints

    // register interest in the delete window message
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0, sizeof("WM_DELETE_WINDOW") - 1, "WM_DELETE_WINDOW"),NULL))) {
		WmDeleteWindowAtom = reply->atom;
		free(reply);
		if ((reply =
			xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection,0, sizeof("WM_PROTOCOLS") - 1, "WM_PROTOCOLS"),NULL))) {
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
			xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1,&WmDeleteWindowAtom);
#endif
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
			xcb_set_wm_protocols(Connection, reply->atom, VideoWindow, 1,&WmDeleteWindowAtom);
#endif
			free(reply);
		}
    }

    //
    //	prepare fullscreen.
    //
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE") - 1, "_NET_WM_STATE"), NULL))) {
		NetWmState = reply->atom;
		free(reply);
    }
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE_FULLSCREEN") - 1,
		    "_NET_WM_STATE_FULLSCREEN"), NULL))) {
		NetWmStateFullscreen = reply->atom;
		free(reply);
    }

    xcb_map_window(Connection, VideoWindow);

    //
    //	hide cursor
    //
    pixmap = xcb_generate_id(Connection);
    xcb_create_pixmap(Connection, 1, pixmap, parent, 1, 1);
    cursor = xcb_generate_id(Connection);
    xcb_create_cursor(Connection, cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1,1);

    values[0] = cursor;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, values);
    VideoCursorPixmap = pixmap;
    VideoBlankCursor = cursor;
    VideoBlankTick = 0;
}

///
///	Set video device.
///
///	Currently this only choose the driver.
///
void VideoSetDevice(const char *device)
{
    VideoDriverName = device;
}

///
///	Get video driver name.
///
///	@returns name of current video driver.
///
const char *VideoGetDriverName(void)
{
    if (VideoUsedModule) {
	return VideoUsedModule->Name;
    }
    return "";
}

///
///	Set video geometry.
///
///	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
int VideoSetGeometry(const char *geometry)
{
    XParseGeometry(geometry, &VideoWindowX, &VideoWindowY, &VideoWindowWidth,
	&VideoWindowHeight);

    return 0;
}

///
///	Set 60hz display mode.
///
///	Pull up 50 Hz video for 60 Hz display.
///
///	@param onoff	enable / disable the 60 Hz mode.
///
void VideoSet60HzMode(int onoff)
{
    Video60HzMode = onoff;
}

///
///	Set soft start audio/video sync.
///
///	@param onoff	enable / disable the soft start sync.
///
void VideoSetSoftStartSync(int onoff)
{
    VideoSoftStartSync = onoff;
}

///
///	Set show black picture during channel switch.
///
///	@param onoff	enable / disable black picture.
///
void VideoSetBlackPicture(int onoff)
{
    VideoShowBlackPicture = onoff;
}

///
///	Set brightness adjustment.
///
///	@param brightness	between -1000 and 1000.
///				0 represents no modification
///
void VideoSetBrightness(int brightness)
{
    (void)brightness;
}

///
///	Set contrast adjustment.
///
///	@param contrast		between 0 and 10000.
///				1000 represents no modification
///
void VideoSetContrast(int contrast)
{
    (void)contrast;
}

///
///	Set saturation adjustment.
///
///	@param saturation	between 0 and 10000.
///				1000 represents no modification
///
void VideoSetSaturation(int saturation)
{
    (void)saturation;
}

///
///	Set hue adjustment.
///
///	@param hue	between -PI*1000 and PI*1000.
///			0 represents no modification
///
void VideoSetHue(int hue)
{
    (void)hue;
}

///
///	Set video output position.
///
///	@param hw_decoder	video hardware decoder
///	@param x		video output x coordinate OSD relative
///	@param y		video output y coordinate OSD relative
///	@param width		video output width
///	@param height		video output height
///
void VideoSetOutputPosition(VideoHwDecoder * hw_decoder, int x, int y,
    int width, int height)
{
    if (!OsdWidth || !OsdHeight) {
	return;
    }
    if (!width || !height) {
		// restore full size
		width = VideoWindowWidth;
		height = VideoWindowHeight;
	} else {
		// convert OSD coordinates to window coordinates
		x = (x * VideoWindowWidth) / OsdWidth;
		width = (width * VideoWindowWidth) / OsdWidth;
		y = (y * VideoWindowHeight) / OsdHeight;
		height = (height * VideoWindowHeight) / OsdHeight;
    }

    // FIXME: add function to module class

    if (VideoUsedModule == &CuvidModule) {
		// check values to be able to avoid
		// interfering with the video thread if possible

		if (x == hw_decoder->Cuvid.VideoX && y == hw_decoder->Cuvid.VideoY
			&& width == hw_decoder->Cuvid.VideoWidth
			&& height == hw_decoder->Cuvid.VideoHeight) {
			// not necessary...
			return;
		}
		VideoThreadLock();
		CuvidSetOutputPosition(&hw_decoder->Cuvid, x, y, width, height);
		CuvidUpdateOutput(&hw_decoder->Cuvid);
		VideoThreadUnlock();
    }


    (void)hw_decoder;
}

///
///	Set video window position.
///
///	@param x	window x coordinate
///	@param y	window y coordinate
///	@param width	window width
///	@param height	window height
///
///	@note no need to lock, only called from inside the video thread
///
void VideoSetVideoMode( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, int width, int height)
{
    Debug(4, "video: %s %dx%d%+d%+d\n", __FUNCTION__, width, height, x, y);

    if ((unsigned)width == VideoWindowWidth	&& (unsigned)height == VideoWindowHeight) {
		return;				// same size nothing todo
    }
#ifdef USE_OPENGLOSD
    if (VideoEventCallback)
        VideoEventCallback();
#endif	
    VideoOsdExit();  

    VideoThreadLock();
    VideoWindowWidth = width;
    VideoWindowHeight = height;
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();
    VideoOsdInit();
}

///
///	Set 4:3 video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSet4to3DisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (Video4to3ZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == Video4to3ZoomMode) {
		return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    Video4to3ZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Set other video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSetOtherDisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (VideoOtherZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == VideoOtherZoomMode) {
		return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    VideoOtherZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Send fullscreen message to window.
///
///	@param onoff	-1 toggle, true turn on, false turn off
///
void VideoSetFullscreen(int onoff)
{
    if (XlibDisplay) {			// needs running connection
		xcb_client_message_event_t event;

		memset(&event, 0, sizeof(event));
		event.response_type = XCB_CLIENT_MESSAGE;
		event.format = 32;
		event.window = VideoWindow;
		event.type = NetWmState;
		if (onoff < 0) {
			event.data.data32[0] = XCB_EWMH_WM_STATE_TOGGLE;
		} else if (onoff) {
			event.data.data32[0] = XCB_EWMH_WM_STATE_ADD;
		} else {
			event.data.data32[0] = XCB_EWMH_WM_STATE_REMOVE;
		}
		event.data.data32[1] = NetWmStateFullscreen;

		xcb_send_event(Connection, XCB_SEND_EVENT_DEST_POINTER_WINDOW,
			DefaultRootWindow(XlibDisplay),
			XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (void *)&event);
		Debug(3, "video/x11: send fullscreen message %x %x\n",
			event.data.data32[0], event.data.data32[1]);
    }
}

///
///	Set deinterlace mode.
///
void VideoSetDeinterlace(int mode[VideoResolutionMax])
{
    VideoDeinterlace[0] = mode[0];
    VideoDeinterlace[1] = mode[1];
    VideoDeinterlace[2] = mode[2];
    VideoDeinterlace[3] = mode[3];
    VideoDeinterlace[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set skip chroma deinterlace on/off.
///
void VideoSetSkipChromaDeinterlace(int onoff[VideoResolutionMax])
{
    VideoSkipChromaDeinterlace[0] = onoff[0];
    VideoSkipChromaDeinterlace[1] = onoff[1];
    VideoSkipChromaDeinterlace[2] = onoff[2];
    VideoSkipChromaDeinterlace[3] = onoff[3];
    VideoSkipChromaDeinterlace[4] = onoff[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set inverse telecine on/off.
///
void VideoSetInverseTelecine(int onoff[VideoResolutionMax])
{
    VideoInverseTelecine[0] = onoff[0];
    VideoInverseTelecine[1] = onoff[1];
    VideoInverseTelecine[2] = onoff[2];
    VideoInverseTelecine[3] = onoff[3];
    VideoInverseTelecine[4] = onoff[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set denoise level (0 .. 1000).
///
void VideoSetDenoise(int level[VideoResolutionMax])
{
    VideoDenoise[0] = level[0];
    VideoDenoise[1] = level[1];
    VideoDenoise[2] = level[2];
    VideoDenoise[3] = level[3];
    VideoDenoise[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set sharpness level (-1000 .. 1000).
///
void VideoSetSharpen(int level[VideoResolutionMax])
{
    VideoSharpen[0] = level[0];
    VideoSharpen[1] = level[1];
    VideoSharpen[2] = level[2];
    VideoSharpen[3] = level[3];
    VideoSharpen[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set scaling mode.
///
///	@param mode	table with VideoResolutionMax values
///
void VideoSetScaling(int mode[VideoResolutionMax])
{
    VideoScaling[0] = mode[0];
    VideoScaling[1] = mode[1];
    VideoScaling[2] = mode[2];
    VideoScaling[3] = mode[3];
    VideoScaling[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set cut top and bottom.
///
///	@param pixels table with VideoResolutionMax values
///
void VideoSetCutTopBottom(int pixels[VideoResolutionMax])
{
    VideoCutTopBottom[0] = pixels[0];
    VideoCutTopBottom[1] = pixels[1];
    VideoCutTopBottom[2] = pixels[2];
    VideoCutTopBottom[3] = pixels[3];
    VideoCutTopBottom[4] = pixels[4];
    // FIXME: update output
}

///
///	Set cut left and right.
///
///	@param pixels	table with VideoResolutionMax values
///
void VideoSetCutLeftRight(int pixels[VideoResolutionMax])
{
    VideoCutLeftRight[0] = pixels[0];
    VideoCutLeftRight[1] = pixels[1];
    VideoCutLeftRight[2] = pixels[2];
    VideoCutLeftRight[3] = pixels[3];
    VideoCutLeftRight[4] = pixels[4];
    // FIXME: update output
}

///
///	Set studio levels.
///
///	@param onoff	flag on/off
///
void VideoSetStudioLevels(int onoff)
{
    VideoStudioLevels = onoff;
}

///
///	Set background color.
///
///	@param rgba	32 bit RGBA color.
///
void VideoSetBackground(uint32_t rgba)
{
    VideoBackground = rgba;		// saved for later start
    VideoUsedModule->SetBackground(rgba);
}

///
///	Set audio delay.
///
///	@param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

///
///	Set auto-crop parameters.
///
void VideoSetAutoCrop(int interval, int delay, int tolerance)
{
#ifdef USE_AUTOCROP
    AutoCropInterval = interval;
    AutoCropDelay = delay;
    AutoCropTolerance = tolerance;

    VideoThreadLock();
    VideoUsedModule->ResetAutoCrop();
    VideoThreadUnlock();
#else
    (void)interval;
    (void)delay;
    (void)tolerance;
#endif
}

///
///	Set EnableDPMSatBlackScreen
///
///	Currently this only choose the driver.
///
void SetDPMSatBlackScreen(int enable)
{
#ifdef USE_SCREENSAVER
    EnableDPMSatBlackScreen = enable;
#endif
}

///
///	Raise video window.
///
int VideoRaiseWindow(void)
{
    static const uint32_t values[] = { XCB_STACK_MODE_ABOVE };

    xcb_configure_window(Connection, VideoWindow, XCB_CONFIG_WINDOW_STACK_MODE,	values);

    return 1;
}

///
///	Initialize video output module.
///
///	@param display_name	X11 display name
///
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t const *screen;

    if (XlibDisplay) {			// allow multiple calls
		Debug(3, "video: x11 already setup\n");
	return;
    }
#ifdef USE_GLX
    if (!XInitThreads()) {
		Error(_("video: Can't initialize X11 thread support on '%s'\n"),display_name);
	}
#endif
    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name && !(display_name = getenv("DISPLAY"))) {
		// if no environment variable, use :0.0 as default display name
		display_name = ":0.0";
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
		Error(_("video: Can't connect to X11 server on '%s'\n"), display_name);
		// FIXME: we need to retry connection
	return;
    }

    // Register error handler
    XSetIOErrorHandler(VideoIOErrorHandler);

    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
		Error(_("video: Can't convert XLIB display to XCB connection\n"));
		VideoExit();
	return;
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
#ifdef xcb_USE_GLX
    xcb_prefetch_extension_data(Connection, &xcb_glx_id);
#endif
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
#ifdef USE_SCREENSAVER
    xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
#endif
    //xcb_prefetch_extension_data(Connection, &xcb_shm_id);
    //xcb_prefetch_extension_data(Connection, &xcb_xv_id);

    // Get the requested screen number
    screen_nr = DefaultScreen(XlibDisplay);
    screen_iter = xcb_setup_roots_iterator(xcb_get_setup(Connection));
    for (i = 0; i < screen_nr; ++i) {
		xcb_screen_next(&screen_iter);
    }
    screen = screen_iter.data;
    VideoScreen = screen;

    //
    //	Default window size
    //
    if (!VideoWindowHeight) {
		if (VideoWindowWidth) {
			VideoWindowHeight = (VideoWindowWidth * 9) / 16;
		} else {			// default to fullscreen
			VideoWindowHeight = screen->height_in_pixels;
			VideoWindowWidth = screen->width_in_pixels;
	//***********************************************************************************************
#if DEBUG_no
         if (strcmp(":0.0",display_name) == 0) {
			VideoWindowHeight = 1080;
			VideoWindowWidth = 1920;
		 }
#endif
		}
    }
    if (!VideoWindowWidth) {
		VideoWindowWidth = (VideoWindowHeight * 16) / 9;
    }
    //
    //	prepare opengl
    //
#ifdef USE_GLX
    // FIXME: module selected below
    if (0) {

		GlxInit();
		// FIXME: use root window?
		VideoCreateWindow(screen->root, GlxVisualInfo->visualid, GlxVisualInfo->depth);
		GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxContext);
    } else
#endif

		//
		// Create output window
		//
		if (1) {				// FIXME: use window mode
			VideoCreateWindow(screen->root, screen->root_visual, screen->root_depth);
		} else {
			// FIXME: support embedded mode
			VideoWindow = screen->root;

			// FIXME: VideoWindowHeight VideoWindowWidth
		}

    Debug(3, "video: window prepared\n");

    //
    //	prepare hardware decoder CUVID
    //
    for (i = 0; i < (int)(sizeof(VideoModules) / sizeof(*VideoModules)); ++i) {
		// FIXME: support list of drivers and include display name
		// use user device or first working enabled device driver
		if ((VideoDriverName
				&& !strcasecmp(VideoDriverName, VideoModules[i]->Name))
				|| (!VideoDriverName && VideoModules[i]->Enabled)) {
			if (VideoModules[i]->Init(display_name)) {
				VideoUsedModule = VideoModules[i];
				goto found;
			}
		}
    }
    Error(_("video: '%s' output module isn't supported\n"), VideoDriverName);
    VideoUsedModule = &NoopModule;

  found:
    // FIXME: make it configurable from gui
    if (getenv("NO_MPEG_HW")) {
		VideoHardwareDecoder = 1;
    }
    if (getenv("NO_HW")) {
		VideoHardwareDecoder = 0;
    }
    // disable x11 screensaver
    X11SuspendScreenSaver(Connection, 1);
    X11DPMSDisable(Connection);

    //xcb_prefetch_maximum_request_length(Connection);
    xcb_flush(Connection);

    // I would like to start threads here, but this produces:
    // [xcb] Unknown sequence number while processing queue
    // [xcb] Most likely this is a multi-threaded client and XInitThreads
    // has not been called
    //VideoPollEvent();
    //VideoThreadInit();
}

///
///	Cleanup video output module.
///
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
		return;
    }
    //
    //	Reenable screensaver / DPMS.
    //
    X11DPMSReenable(Connection);
    X11SuspendScreenSaver(Connection, 0);


    VideoUsedModule->Exit();
    VideoUsedModule = &NoopModule;

	
#ifdef USE_VIDEO_THREAD
    VideoThreadExit();   // destroy all mutexes 
#endif
#ifdef USE_GLX
    if (GlxEnabled) {
		GlxExit();       // delete all contexts
    }
#endif
    //
    //	FIXME: cleanup.
    //
    //RandrExit();
	
    //
    //	X11/xcb cleanup
    //
	if (VideoWindow != XCB_NONE) {
		xcb_destroy_window(Connection, VideoWindow);
		VideoWindow = XCB_NONE;
    }
    if (VideoColormap != XCB_NONE) {
		xcb_free_colormap(Connection, VideoColormap);
		VideoColormap = XCB_NONE;
    }
    if (VideoBlankCursor != XCB_NONE) {
		xcb_free_cursor(Connection, VideoBlankCursor);
		VideoBlankCursor = XCB_NONE;
    }
    if (VideoCursorPixmap != XCB_NONE) {
		xcb_free_pixmap(Connection, VideoCursorPixmap);
		VideoCursorPixmap = XCB_NONE;
    }
    xcb_flush(Connection);
    if (XlibDisplay) {
		if (XCloseDisplay(XlibDisplay)) {
			Error(_("video: error closing display\n"));
		}
		XlibDisplay = NULL;
		Connection = 0;
    }
	

}

int GlxInitopengl() {
	
	while (GlxSharedContext == NULL || GlxContext == NULL) {
		sleep(1);					// wait until Init from video thread is ready
//		printf("GlxConext %p\n",GlxSharedContext);
	}

	OSDcontext = glXCreateContext(XlibDisplay, GlxVisualInfo, GlxSharedContext,GL_TRUE);
	if (!OSDcontext) {
		Debug(3,"video/osd: can't create glx context\n");
		return 0;
	}
	Debug(3,"Create OSD GLX context\n");
	glXMakeCurrent(XlibDisplay, VideoWindow, OSDcontext);
#if 0
	glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
    glDepthRange(-1.0, 1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glClearDepth(1.0);
    GlxCheck();

//	if (glewInit())
//		Fatal(_("glewinit failed\n"));

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
    GlxCheck();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);		// setup 2d drawing
#endif
	return 1;
	
}

