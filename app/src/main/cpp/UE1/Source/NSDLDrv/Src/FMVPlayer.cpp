/*=============================================================================
	FMVPlayer.cpp: KHG FMV (Indeo 5 / IV50 AVI) playback on Android via FFmpeg.

	See FMVPlayer.h. Stateful single-clip player split into open/advance/draw/close so the
	video can be composited UNDER the live engine HUD: the playavi handler drives a blocking
	loop (advance + UGameEngine::Draw); UGameEngine::Draw calls UE1FMVDrawActiveFrame() between
	the world and the HUD so KHG's KlingonHUDIntroNull overlay renders on top.

	Pipeline: avformat (avi demux) -> avcodec (indeo5 video, adpcm_ms audio)
	          video: sws_scale yuv410p -> RGBA, held until its PTS is due, then GL texture
	          audio: swresample -> S16 stereo -> SDL audio device (queued); audio = master clock
	          sync : a decoded video frame is uploaded when pendingPts <= audio playback clock
=============================================================================*/

#include <SDL.h>
#include <android/log.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

#include "FMVPlayer.h"

#define FMV_LOG(...) ((void)0)   // [KHG] diagnostic logging pulled (was KHG_FMV per-frame spam)
#define FMV_ERR(...) __android_log_print( ANDROID_LOG_ERROR, "KHG_FMV", __VA_ARGS__ )   // keep real failures

#define ARRAY_COUNT_FMV 48   // decoded-video frame-queue capacity (must match FMVState::vq[] size)

namespace {

// [KHG] The FMV is presented entirely through the Vulkan render device (UGameEngine::Draw ->
// RenDev->DrawTile, fed by the CPU buffers G.rgba and gOverlayRGBA). The old GLES2 blitter/composite
// path has been removed as part of the OpenGL teardown — decode stays, present is Vulkan-only.

// ---- single active clip --------------------------------------------------------------

struct FMVState
{
	bool             active=false;
	SDL_Window*      window=NULL;
	AVFormatContext* fmt=NULL;
	AVCodecContext*  vctx=NULL;
	AVCodecContext*  actx=NULL;
	SwsContext*      sws=NULL;
	SwrContext*      swr=NULL;
	SDL_AudioDeviceID adev=0;
	bool             ownAudioSubsys=false;
	AVPacket*        pkt=NULL;
	AVFrame*         frm=NULL;
	uint8_t*         rgba=NULL;          // staging buffer for the pending/just-shown frame
	int              vidW=0, vidH=0, vIdx=-1, aIdx=-1;
	int              outRate=22050, outCh=2, bytesPerSample=4;
	long long        totalAudioBytes=0;
	double           timeBase=0.0;
	double           pendingPts=0.0;
	bool             hasPending=false;   // rgba holds a decoded frame not yet uploaded
	bool             demuxEof=false;
	// Decoded-video frame queue. We drain the decoder after EVERY video packet (so it can never back up
	// and drop frames -> the freeze), buffering the decoded frames here, then present them by PTS against
	// the audio clock (so the video can never run ahead -> no desync). Holds AVFrame refs (YUV), scaled
	// to BGRA only at display time.
	AVFrame*         vq[48] = {};
	int              vqHead=0, vqTail=0, vqCount=0;
	Uint32           startTicks=0;
	// Audio-clock liveness: if the SDL audio device never drains (e.g. the platform output is owned by
	// the game's own audio engine), the audio clock stays at 0 and playback would freeze. Detect that
	// and fall back to the wall clock so the movie always progresses and ends.
	double           lastAudioClock=-1.0;
	Uint32           lastAudioMoveTicks=0;
	bool             audioStuck=false;
};

static FMVState G;

static double AudioClock()
{
	if( G.adev==0 || G.bytesPerSample<=0 ) return -1.0; // signal: use wall clock
	long long queued = (long long)SDL_GetQueuedAudioSize( G.adev );
	long long played = G.totalAudioBytes - queued; if( played<0 ) played=0;
	return (double)played / (double)( G.outRate * G.bytesPerSample );
}

static double NowClock()
{
	double c = AudioClock();
	if( c < 0.0 )
		return ( SDL_GetTicks() - G.startTicks ) / 1000.0;   // no audio device -> wall clock

	// Audio device present: confirm it is actually draining. If the clock hasn't advanced for ~300ms of
	// wall time the device isn't playing -> switch to wall clock (and mark audioStuck for the end test).
	Uint32 now = SDL_GetTicks();
	if( c > G.lastAudioClock + 1e-4 )
	{
		G.lastAudioClock = c;
		G.lastAudioMoveTicks = now;
		G.audioStuck = false;
	}
	else if( G.lastAudioMoveTicks != 0 && ( now - G.lastAudioMoveTicks ) > 300 )
	{
		G.audioStuck = true;
	}
	if( G.lastAudioMoveTicks == 0 )
		G.lastAudioMoveTicks = now;   // first sample
	if( G.audioStuck )
		return ( now - G.startTicks ) / 1000.0;
	return c;
}

#ifdef PLATFORM_ANDROID
// Native-direct gamepad buttons bypass SDL events, so SDL_PollEvent never sees them. Poll a
// button-down sequence published by NSDLViewport so the controller can skip cutscenes too.
extern "C" int UE1AndroidNativeControllerButtonDownSeqV141();
static int    gCtrlSkipBaseline  = 0;
static Uint32 gCtrlSkipForTicks  = 0xFFFFFFFFu;
#endif

static const Uint32 kSkipGraceMs = 400; // ignore skip input for the first 400ms of a clip

static bool PollSkip()
{
	SDL_Event ev; bool skip=false, quit=false;
	while( SDL_PollEvent( &ev ) )
	{
		switch( ev.type )
		{
			case SDL_QUIT: quit=true; break;
			// [KHG] Skip only on the Esc-equivalent: keyboard Esc, or the controller START button.
			// (The Thor's native-direct gamepad START is handled via the seq bridge below; this case
			// also covers any controller SDL delivers as a real SDL_CONTROLLERBUTTON.)
			case SDL_KEYDOWN:
				if( ev.key.keysym.sym == SDLK_ESCAPE ) skip=true;
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_START ) skip=true;
				break;
			// [KHG] Tap / click to skip. A screen tap is the natural way to dismiss a cutscene on a
			// touch device, so honour SDL_FINGERDOWN (real touch) and SDL_MOUSEBUTTONDOWN (mouse, plus
			// any touch SDL synthesises as a click). Still gated by the kSkipGraceMs grace window below,
			// so the tap that launched the app / kicked off the intro cannot instantly skip the clip.
			case SDL_FINGERDOWN:
			case SDL_MOUSEBUTTONDOWN:
				skip=true;
				break;
			default: break;
		}
	}
#ifdef PLATFORM_ANDROID
	// Gamepad (native-direct) skip: re-baseline on each new clip (startTicks changes), then any
	// controller button-down since the clip began counts as a skip (still gated by the grace window).
	if( gCtrlSkipForTicks != G.startTicks ) { gCtrlSkipForTicks = G.startTicks; gCtrlSkipBaseline = UE1AndroidNativeControllerButtonDownSeqV141(); }
	if( UE1AndroidNativeControllerButtonDownSeqV141() != gCtrlSkipBaseline ) skip = true;
#endif
	if( quit ) return true; // a real quit always ends the clip
	// Grace window: a stray input event delivered right as the clip starts (e.g. the tap/button that
	// launched the app or kicked off the intro) must not instantly skip the movie. UE1FMVOpen also
	// flushes the pre-queued events; this covers anything delivered in the first few frames.
	if( skip && ( SDL_GetTicks() - G.startTicks ) < kSkipGraceMs )
		return false;
	return skip;
}

static double FramePts( const AVFrame* f )
{
	int64_t ts = ( f->best_effort_timestamp != AV_NOPTS_VALUE ) ? f->best_effort_timestamp
	           : ( f->pts != AV_NOPTS_VALUE ? f->pts : 0 );
	return ts * G.timeBase;
}

// Drain EVERY frame the decoder currently has ready into the queue (moving the ref, no copy). This is
// the fix for the freeze: the decoder is fully drained after each packet so it never backs up and drops
// frames. Returns the number drained. Drops the oldest queued frame if the queue is somehow full.
static int DrainVideoToQueue()
{
	int n = 0;
	while( avcodec_receive_frame( G.vctx, G.frm ) == 0 )
	{
		AVFrame* f = av_frame_alloc();
		if( !f ) { av_frame_unref( G.frm ); break; }
		av_frame_move_ref( f, G.frm );   // take ownership of this frame's buffers; G.frm reset for next
		if( G.vqCount >= (int)ARRAY_COUNT_FMV )
		{
			av_frame_free( &G.vq[G.vqHead] );
			G.vqHead = ( G.vqHead + 1 ) % (int)ARRAY_COUNT_FMV;
			G.vqCount--;
		}
		G.vq[G.vqTail] = f;
		G.vqTail = ( G.vqTail + 1 ) % (int)ARRAY_COUNT_FMV;
		G.vqCount++;
		n++;
	}
	return n;
}

// Present the newest queued frame whose PTS is due (<= clk). Older due frames are dropped without
// scaling (catch-up). Frames with PTS in the future stay queued. Scales the chosen frame into G.rgba
// (which the Vulkan present reads via UE1FMVGetFrameBGRA). Returns true if a frame was shown.
static bool ShowDueVideoFrame( double clk )
{
	AVFrame* due = NULL; double duePts = 0.0;
	while( G.vqCount > 0 )
	{
		AVFrame* f = G.vq[G.vqHead];
		double pts = FramePts( f );
		if( pts > clk + 0.005 ) break;       // not due yet -> keep it (and all later) queued
		if( due ) av_frame_free( &due );     // an earlier due frame we skipped past (catch-up)
		due = f; duePts = pts;
		G.vqHead = ( G.vqHead + 1 ) % (int)ARRAY_COUNT_FMV;
		G.vqCount--;
	}
	if( !due ) return false;
	uint8_t* planes[4] = { G.rgba, NULL, NULL, NULL };
	int      stride[4] = { G.vidW*4, 0, 0, 0 };
	sws_scale( G.sws, due->data, due->linesize, 0, G.vidH, planes, stride );
	G.pendingPts = duePts;
	av_frame_free( &due );
	return true;
}

// ---- decorated console-frame overlay (buffer/buildup/breakdn) compositing -------------
// KHG's briefing/comm clips are a character in a hex mask on a flat dark surround; the ornate
// Klingon console frame is a SEPARATE video: buildup.avi assembles it, breakdn.avi removes it.
// We retain buildup's final frame (the assembled console, with an empty black hex window) as
// gOverlayTex and composite the content clip INTO that window.
//
// MASK = the window itself (NOT the character silhouette). At retain time we flood-fill the
// buildup frame's central pure-black window (bounded by its bright gold ring) and bake it into the
// overlay's ALPHA (0 = window, 255 = console border). The shader then shows the content clip
// exactly where the window is cut. This is faithful to the original (which keyed the HEX, not the
// silhouette) and avoids the two dead ends: (1) guessing an SDF hexagon's centre/shape -- the real
// window is an irregular flat-top hex, widest BELOW centre; and (2) colour-keying the content's
// surround -- which fails because the Klingon's black hair is as dark as the surround and gets
// punched out (the "messed-up colours" bug). Measured window (640x480): centre ~(0.50,0.47),
// x[162..474] y[125..325].
// Composite (framed Y/C) state. The decorated console frame is composited in the Vulkan present path
// (UGameEngine::Draw): the content clip is hex-cut and the retained overlay (gOverlayRGBA, baked window
// alpha) is drawn masked on top. No GL textures/shaders here anymore.
static bool gComposite = false;

} // namespace

// ---- public API ----------------------------------------------------------------------

extern "C" int UE1FMVIsActive( void ) { return G.active ? 1 : 0; }

// ---- static splash still (timed, skippable) -------------------------------------------
// Present a fixed BGRA image (top-down, w*h*4) through the SAME device-agnostic path as the FMV
// (UGameEngine::Draw -> UE1FMVPresentFrame -> UE1FMVGetFrameBGRA -> RenDev->DrawTile), for `seconds`,
// skippable after PollSkip's grace window. Used for the retail MicroProse launcher splash shown AFTER
// the intro movie. The caller drives the present loop exactly like playavi:
//   while( UE1FMVAdvance() ) { Engine->Draw( Viewport ); SDL_Delay( 2 ); }  UE1FMVClose();
static bool   gStill = false;
static double gStillSeconds = 0.0;

extern "C" int UE1FMVShowStill( const unsigned char* bgra, int w, int h, double seconds )
{
	if( G.active ) UE1FMVClose();
	if( !bgra || w <= 0 || h <= 0 ) return 0;
	G = FMVState();
	gComposite    = false;                       // plain full-screen, no hex cut
	gStill        = true;
	gStillSeconds = ( seconds > 0.0 ) ? seconds : 2.5;
	G.vidW = w; G.vidH = h;
	G.rgba = (uint8_t*)av_malloc( (size_t)w * h * 4 );
	if( !G.rgba ) { gStill = false; return 0; }
	SDL_memcpy( G.rgba, bgra, (size_t)w * h * 4 );
	// Drop the input event that launched us so PollSkip doesn't instantly skip (also a 400ms grace).
	SDL_PumpEvents();
	SDL_FlushEvents( SDL_FIRSTEVENT, SDL_LASTEVENT );
	G.startTicks = SDL_GetTicks();
	G.active = true;
	FMV_LOG( "splash still %dx%d for %.1fs", w, h, gStillSeconds );
	return 1;
}

// Device-agnostic present hook: expose the current decoded frame (BGRA8, top-down) so the engine can
// blit it through RenDev->DrawTile (works on Vulkan, which has no GL context). Returns 1 if a frame is
// available. Buffer is valid until the next UE1FMVAdvance / UE1FMVClose.
//
// [KHG] Composite (framed Y/C) clips are NOT cut here. The content is presented FULL/opaque and the
// retained console-frame overlay (gOverlayRGBA) — whose central window was flood-filled to alpha 0 in
// BakeWindowAlpha — is drawn masked ON TOP, so the overlay's exact, measured window does ALL the masking.
// (The old hardcoded kHexX/kHexY polygon cut here was redundant with that window, and when the two shapes
// disagreed it produced the off-center character / black-ring-at-the-window-edge artifacts — removed.)
extern "C" int UE1FMVGetFrameBGRA( const unsigned char** outData, int* outW, int* outH )
{
	if( !G.active || !G.rgba || G.vidW <= 0 || G.vidH <= 0 )
		return 0;
	if( outData ) *outData = G.rgba;
	if( outW )    *outW = G.vidW;
	if( outH )    *outH = G.vidH;
	return 1;
}

extern "C" void UE1FMVBlackout( void )
{
	// No-op: the GL swap-clear path was removed with the OpenGL teardown. Under Vulkan the screen is
	// blacked out through the render device (see UE1AndroidPlayIntroFMVOnce in SDLLaunch.cpp).
}

extern "C" int UE1FMVOpen( const char* FullPath, SDL_Window* Window )
{
	if( G.active ) UE1FMVClose();
	if( !FullPath || !Window ) return 0;
	FMV_LOG( "open '%s'", FullPath );
	G = FMVState();
	G.window = Window;

	if( avformat_open_input( &G.fmt, FullPath, NULL, NULL ) != 0 ) { FMV_ERR("open_input failed: %s", FullPath); UE1FMVClose(); return 0; }
	if( avformat_find_stream_info( G.fmt, NULL ) < 0 )             { FMV_ERR("find_stream_info failed");        UE1FMVClose(); return 0; }

	for( unsigned i=0; i<G.fmt->nb_streams; ++i )
	{
		AVCodecParameters* p = G.fmt->streams[i]->codecpar;
		if( p->codec_type==AVMEDIA_TYPE_VIDEO && G.vIdx<0 ) G.vIdx=(int)i;
		else if( p->codec_type==AVMEDIA_TYPE_AUDIO && G.aIdx<0 ) G.aIdx=(int)i;
	}
	if( G.vIdx<0 ) { FMV_ERR("no video stream"); UE1FMVClose(); return 0; }

	AVStream* vst = G.fmt->streams[G.vIdx];
	const AVCodec* vdec = avcodec_find_decoder( vst->codecpar->codec_id );
	G.vctx = avcodec_alloc_context3( vdec );
	avcodec_parameters_to_context( G.vctx, vst->codecpar );
	if( !vdec || avcodec_open2( G.vctx, vdec, NULL ) < 0 ) { FMV_ERR("video codec open failed"); UE1FMVClose(); return 0; }
	G.vidW = G.vctx->width; G.vidH = G.vctx->height;
	G.timeBase = av_q2d( vst->time_base );

	// Decode to BGRA: the device-agnostic present (UnGame.cpp -> RenDev->DrawTile -> GetTexture) treats a
	// non-palette texture as BGRA and swaps to RGBA, so BGRA here yields correct colours under Vulkan.
	G.sws = sws_getContext( G.vidW, G.vidH, G.vctx->pix_fmt, G.vidW, G.vidH, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL,NULL,NULL );
	G.rgba = (uint8_t*)av_malloc( (size_t)G.vidW*G.vidH*4 );
	if( !G.sws || !G.rgba ) { FMV_ERR("sws/alloc failed"); UE1FMVClose(); return 0; }

	// Audio (optional).
	if( G.aIdx>=0 )
	{
		AVStream* ast = G.fmt->streams[G.aIdx];
		const AVCodec* adec = avcodec_find_decoder( ast->codecpar->codec_id );
		if( adec )
		{
			G.actx = avcodec_alloc_context3( adec );
			avcodec_parameters_to_context( G.actx, ast->codecpar );
			if( avcodec_open2( G.actx, adec, NULL ) < 0 ) { avcodec_free_context(&G.actx); G.actx=NULL; }
		}
		if( G.actx )
		{
			G.outRate = G.actx->sample_rate>0 ? G.actx->sample_rate : 22050;
			G.bytesPerSample = G.outCh * (int)sizeof(int16_t);
			AVChannelLayout outLayout; av_channel_layout_default( &outLayout, G.outCh );
			if( swr_alloc_set_opts2( &G.swr, &outLayout, AV_SAMPLE_FMT_S16, G.outRate,
					&G.actx->ch_layout, G.actx->sample_fmt, G.actx->sample_rate, 0, NULL ) < 0
				|| swr_init( G.swr ) < 0 )
			{
				FMV_ERR("swr init failed; no audio"); if(G.swr) swr_free(&G.swr); avcodec_free_context(&G.actx); G.actx=NULL;
			}
		}
		if( G.actx )
		{
			if( SDL_WasInit( SDL_INIT_AUDIO )==0 )
			{
				if( SDL_InitSubSystem( SDL_INIT_AUDIO )==0 ) G.ownAudioSubsys=true;
				else FMV_ERR("SDL_InitSubSystem(AUDIO): %s", SDL_GetError());
			}
			SDL_AudioSpec want; SDL_zero(want);
			want.freq=G.outRate; want.format=AUDIO_S16SYS; want.channels=(Uint8)G.outCh; want.samples=1024;
			G.adev = SDL_OpenAudioDevice( NULL, 0, &want, NULL, 0 );
			if( G.adev==0 ) FMV_ERR("OpenAudioDevice: %s", SDL_GetError());
			else SDL_PauseAudioDevice( G.adev, 0 );
		}
	}

	// Presentation is Vulkan-only: each decoded frame is presented via RenDev->DrawTile (UnGame.cpp).
	// The GL blit path was removed with the OpenGL teardown.
	FMV_LOG("presenting FMV via RenDev->DrawTile (Vulkan)");

	G.pkt = av_packet_alloc();
	G.frm = av_frame_alloc();
	// Drop any input events queued before playback (the tap/button that launched the app or started
	// this clip) so PollSkip on the first advance doesn't instantly skip the movie.
	SDL_PumpEvents();
	SDL_FlushEvents( SDL_FIRSTEVENT, SDL_LASTEVENT );
	G.startTicks = SDL_GetTicks();
	G.active = true;
	FMV_LOG( "ready: %dx%d  audio=%s %dHz", G.vidW, G.vidH, G.adev?"on":"off", G.outRate );
	return 1;
}

extern "C" int UE1FMVAdvance( void )
{
	if( !G.active ) return 0;

	// Static splash: re-present the same frame until the timer expires or the user skips. No decode.
	if( gStill )
	{
		if( PollSkip() ) { FMV_LOG("splash skipped"); return 0; }
		double wall = ( SDL_GetTicks() - G.startTicks ) / 1000.0;
		if( wall >= gStillSeconds ) { FMV_LOG("splash ended (%.1fs)", wall); return 0; }
		return 1;
	}

	if( PollSkip() ) { FMV_LOG("skipped"); return 0; }

	double clk = NowClock();

	// Hard safety: never let the play-loop run past the clip's nominal duration (+margin), regardless of
	// the audio device draining or the A/V clock. Guarantees the intro always ends and the menu appears.
	{
		double wall   = ( SDL_GetTicks() - G.startTicks ) / 1000.0;
		double durSec = ( G.fmt && G.fmt->duration > 0 ) ? (double)G.fmt->duration / (double)AV_TIME_BASE : 0.0;
		if( durSec > 0.0 && wall > durSec + 1.5 )
		{
			FMV_LOG( "ended (wall cap %.1f > dur %.1f)", wall, durSec );
			return 0;
		}
	}

	{
		static int dbg = 0;
		if( ( dbg++ % 15 ) == 0 )
			FMV_LOG( "adv clk=%.2f audclk=%.2f queued=%u total=%lld eof=%d vq=%d ppts=%.2f stuck=%d",
				clk, AudioClock(), (unsigned)( G.adev ? SDL_GetQueuedAudioSize(G.adev) : 0 ), G.totalAudioBytes,
				(int)G.demuxEof, G.vqCount, G.pendingPts, (int)G.audioStuck );
	}

	// Present the queued frame whose PTS is due against the audio clock (keeps video in sync).
	ShowDueVideoFrame( clk );
	G.hasPending = ( G.vqCount > 0 );

	// Pump: keep ~0.4s of audio queued and ~12 video frames (~0.8s) buffered ahead in the queue.
	const Uint32 audioTarget = (Uint32)( G.outRate * G.bytesPerSample * 4 / 10 ); // ~0.4s
	int guard = 0;
	while( guard++ < 256 )
	{
		bool needAudio = ( G.adev && SDL_GetQueuedAudioSize( G.adev ) < audioTarget );
		bool needVideo = ( G.vqCount < 12 );
		if( !needAudio && !needVideo ) break;
		if( G.demuxEof ) break;

		if( av_read_frame( G.fmt, G.pkt ) < 0 ) { G.demuxEof = true; avcodec_send_packet(G.vctx,NULL); if(G.actx) avcodec_send_packet(G.actx,NULL); break; }

		if( G.pkt->stream_index == G.vIdx )
		{
			// ALWAYS drain after sending so the decoder can never back up (the freeze cause).
			if( avcodec_send_packet( G.vctx, G.pkt )==0 )
				DrainVideoToQueue();
		}
		else if( G.actx && G.pkt->stream_index == G.aIdx )
		{
			if( avcodec_send_packet( G.actx, G.pkt )==0 )
			{
				while( avcodec_receive_frame( G.actx, G.frm )==0 )
				{
					uint8_t* obuf=NULL;
					int outSamples = swr_get_out_samples( G.swr, G.frm->nb_samples );
					if( av_samples_alloc( &obuf, NULL, G.outCh, outSamples, AV_SAMPLE_FMT_S16, 0 ) >= 0 )
					{
						int got = swr_convert( G.swr, &obuf, outSamples, (const uint8_t**)G.frm->data, G.frm->nb_samples );
						if( got>0 && G.adev ) { int b=got*G.bytesPerSample; SDL_QueueAudio(G.adev,obuf,(Uint32)b); G.totalAudioBytes+=b; }
						av_freep( &obuf );
					}
				}
			}
		}
		av_packet_unref( G.pkt );
	}

	// After EOF, flush any frames still buffered inside the decoder into the queue.
	if( G.demuxEof )
		DrainVideoToQueue();

	// Ended when demux is done, the video queue is empty, and audio has drained (or device is dead).
	bool audioDrained = ( G.adev==0 ) || ( SDL_GetQueuedAudioSize( G.adev )==0 ) || G.audioStuck;
	if( G.demuxEof && G.vqCount==0 && audioDrained )
	{
		FMV_LOG("ended (audioStuck=%d)", (int)G.audioStuck );
		return 0;
	}
	return 1;
}

extern "C" void UE1FMVDrawActiveFrame( void )
{
	// No-op: the GLES present path was removed with the OpenGL teardown. The active frame is presented
	// by UGameEngine::Draw via RenDev->DrawTile (content + decorated overlay), reading G.rgba /
	// gOverlayRGBA. This stub remains only so the Engine-side extern "C" reference still links.
}

extern "C" void UE1FMVClose( void )
{
	if( G.adev ) { SDL_PauseAudioDevice(G.adev,1); SDL_ClearQueuedAudio(G.adev); SDL_CloseAudioDevice(G.adev); }
	if( G.ownAudioSubsys ) SDL_QuitSubSystem( SDL_INIT_AUDIO );
	if( G.rgba ) av_free( G.rgba );
	if( G.sws ) sws_freeContext( G.sws );
	if( G.frm ) av_frame_free( &G.frm );
	if( G.pkt ) av_packet_free( &G.pkt );
	if( G.swr ) swr_free( &G.swr );
	if( G.actx ) avcodec_free_context( &G.actx );
	if( G.vctx ) avcodec_free_context( &G.vctx );
	if( G.fmt ) avformat_close_input( &G.fmt );
	// Free any frames still in the video queue (FMVState() reset below only nulls the pointers).
	while( G.vqCount > 0 )
	{
		av_frame_free( &G.vq[G.vqHead] );
		G.vqHead = ( G.vqHead + 1 ) % ARRAY_COUNT_FMV;
		G.vqCount--;
	}
	bool was = G.active;
	gStill = false; gStillSeconds = 0.0;
	G = FMVState();
	if( was ) FMV_LOG("closed");
}

// Mark the console frame's cut window in the overlay's alpha (0 = window, 255 = border) by
// flood-filling the central pure-black region of buildup's final frame, bounded by its bright gold
// ring. Robust to the irregular window shape. If the flood is implausible (no seed / leaked through
// a dark gap), fall back to the measured window hexagon polygon.
static void BakeWindowAlpha( uint8_t* ov, int w, int h )
{
	const int N = w*h;
	for( int i=0; i<N; ++i ) ov[(size_t)i*4+3] = 255;   // default: opaque border everywhere
	uint8_t* vis   = (uint8_t*)calloc( (size_t)N, 1 );
	int*     stack = (int*)malloc( (size_t)N * sizeof(int) );
	int count = 0;
	#define FMV_LUMI(p) ( (77*(int)(p)[0] + 150*(int)(p)[1] + 29*(int)(p)[2]) >> 8 ) // ~Rec601, /256
	const int TH = 18; // window black is ~lum 2; gold ring is bright -> 18 cleanly separates
	if( vis && stack )
	{
		// Seed: nearest dark pixel to centre (the window straddles centre).
		int seed = -1;
		for( int r=0; r<60 && seed<0; ++r )
			for( int dy=-r; dy<=r && seed<0; ++dy )
				for( int dx=-r; dx<=r; ++dx )
				{
					int x=w/2+dx, y=h/2+dy;
					if( x<0||y<0||x>=w||y>=h ) continue;
					if( FMV_LUMI( ov+((size_t)y*w+x)*4 ) < TH ) { seed=y*w+x; break; }
				}
		if( seed>=0 )
		{
			int sp=0; stack[sp++]=seed; vis[seed]=1;
			while( sp>0 )
			{
				int idx=stack[--sp]; ++count; ov[(size_t)idx*4+3]=0; // alpha 0 -> window
				int x=idx%w, y=idx/w;
				static const int nb[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
				for( int k=0;k<4;++k )
				{
					int nx=x+nb[k][0], ny=y+nb[k][1];
					if( nx<0||ny<0||nx>=w||ny>=h ) continue;
					int nidx=ny*w+nx;
					if( vis[nidx] ) continue;
					if( FMV_LUMI( ov+(size_t)nidx*4 ) < TH ) { vis[nidx]=1; stack[sp++]=nidx; }
				}
			}
		}
	}
	// Fallback: measured window hexagon (UV verts, x/640,y/480) if flood failed or leaked.
	if( count < N/100 || count > N*45/100 )
	{
		FMV_ERR( "overlay window flood implausible (count=%d/%d) -> polygon fallback", count, N );
		for( int i=0;i<N;++i ) ov[(size_t)i*4+3]=255;
		static const float hx[6] = { 0.3625f, 0.6313f, 0.7313f, 0.6672f, 0.3281f, 0.2594f };
		static const float hy[6] = { 0.260f,  0.260f,  0.531f,  0.677f,  0.677f,  0.531f  };
		for( int y=0;y<h;++y ) for( int x=0;x<w;++x )
		{
			float u=(x+0.5f)/w, v=(y+0.5f)/h; int inside=0;
			for( int a=0,b=5; a<6; b=a++ )
				if( ((hy[a]>v)!=(hy[b]>v)) && (u < (hx[b]-hx[a])*(v-hy[a])/(hy[b]-hy[a])+hx[a]) ) inside=!inside;
			if( inside ) ov[((size_t)y*w+x)*4+3]=0;
		}
	}
	#undef FMV_LUMI
	free( vis ); free( stack );
	FMV_LOG( "overlay window mask baked (%d px cut of %d)", count, N );
}

// Persistent CPU copy of the retained console-frame overlay (BGRA, border alpha=255, window alpha=0),
// for the Vulkan present path which has no GL overlay texture. Survives until ReleaseOverlay.
static uint8_t* gOverlayRGBA = NULL;
static int      gOverlayCpuW = 0, gOverlayCpuH = 0;

// Expose the decorated console-frame overlay (BGRA8, top-down; alpha = 255 on the ornate border, 0 in
// the hex window) so the engine can draw it OVER the hex-cut content clip via RenDev->DrawTile (masked):
// the window's alpha-0 lets the character show through, the border composites the Klingon frame on top.
extern "C" int UE1FMVGetOverlayBGRA( const unsigned char** outData, int* outW, int* outH )
{
	if( !gOverlayRGBA || gOverlayCpuW<=0 || gOverlayCpuH<=0 ) return 0;
	if( outData ) *outData = gOverlayRGBA;
	if( outW )    *outW = gOverlayCpuW;
	if( outH )    *outH = gOverlayCpuH;
	return 1;
}

extern "C" void UE1FMVRetainAsOverlay( void )
{
	// Snapshot the current decoded frame (buildup.avi's final = the assembled console frame) and
	// bake its cut window into alpha so the composite shader knows where to show the content clip.
	if( !G.active || !G.rgba || G.vidW<=0 || G.vidH<=0 ) { FMV_ERR("retain overlay: no frame"); return; }
	const int w=G.vidW, h=G.vidH;
	uint8_t* ov = (uint8_t*)malloc( (size_t)w*h*4 );
	if( !ov ) { FMV_ERR("retain overlay: oom"); return; }
	memcpy( ov, G.rgba, (size_t)w*h*4 );
	BakeWindowAlpha( ov, w, h );
	// Keep a persistent CPU copy for the Vulkan present (DrawTile) path.
	if( gOverlayRGBA ) free( gOverlayRGBA );
	gOverlayRGBA = (uint8_t*)malloc( (size_t)w*h*4 );
	if( gOverlayRGBA ) { memcpy( gOverlayRGBA, ov, (size_t)w*h*4 ); gOverlayCpuW=w; gOverlayCpuH=h; }
	free( ov );
	FMV_LOG( "overlay (console frame) retained %dx%d", gOverlayCpuW, gOverlayCpuH );
}

extern "C" void UE1FMVSetComposite( int On ) { gComposite = ( On != 0 ); }
extern "C" int  UE1FMVIsComposite( void ) { return gComposite ? 1 : 0; }

extern "C" void UE1FMVReleaseOverlay( void )
{
	if( gOverlayRGBA ) { free( gOverlayRGBA ); gOverlayRGBA = NULL; }
	gOverlayCpuW = gOverlayCpuH = 0;
	gComposite = false;
}
