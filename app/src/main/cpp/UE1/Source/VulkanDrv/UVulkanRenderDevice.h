/*=============================================================================
	UVulkanRenderDevice.h: KHG Android Vulkan render device (build-219 URenderDevice).

	Phase 4: UT99VulkanDrv manager architecture (bindless scene + OpenGL 1:1 math), adapted to the 219
	vtable and our Android surface lifecycle. The manager classes (Buffer/Command/Descriptor/Framebuffer/
	RenderPass/Sampler/Shader/Texture/Upload/SceneTextures) are owned here and drive all rendering.
	Shaders are precompiled SPIR-V (SceneShaders.h); no glslang on Android.
=============================================================================*/
#pragma once

#include "Precomp.h"
#include "CommandBufferManager.h"
#include "BufferManager.h"
#include "DescriptorSetManager.h"
#include "FramebufferManager.h"
#include "RenderPassManager.h"
#include "SamplerManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "UploadManager.h"
#include "vec.h"
#include "mat.h"
#include <array>

class CachedTexture;
struct ANativeWindow;   // android/native_window.h (included in the .cpp)

class DLL_EXPORT UVulkanRenderDevice : public URenderDevice
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UVulkanRenderDevice, URenderDevice, CLASS_Config)

	UVulkanRenderDevice();
	static void InternalClassInitializer( UClass* Class );

	// --- 219 URenderDevice contract (exact signatures from Engine/Inc/UnRenDev.h) ---
	virtual UBOOL Init( UViewport* InViewport );
	virtual void  Exit();
	virtual void  Flush();
	virtual UBOOL Exec( const char* Cmd, FOutputDevice* Out );
	virtual void  Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize );
	virtual void  Unlock( UBOOL Blit );
	virtual void  DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet );
	virtual void  DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span );
	virtual void  DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags );
	virtual void  Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 );
	virtual void  Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 );
	virtual void  ClearZ( FSceneNode* Frame );
	virtual void  PushHit( const BYTE* Data, INT Count );
	virtual void  PopHit( INT Count, UBOOL bForce );
	virtual void  GetStats( char* Result );
	virtual void  ReadPixels( FColor* Pixels );
	virtual void  EndFlash();

	// --- Device + managers (the UT99 architecture) ---
	std::shared_ptr<VulkanInstance>          Instance;   // [KHG] kept alive (Android surface)
	std::shared_ptr<VulkanSurface>           Surface;    // [KHG] Android surface
	std::shared_ptr<VulkanSurface>           RetiredSurface; // [KHG] holds the pre-resume surface alive until the old swapchain built on it is destroyed (avoids freeing a VkSurface still referenced by a live VkSwapchainKHR)
	std::shared_ptr<VulkanDevice>            Device;

	std::unique_ptr<CommandBufferManager>    Commands;
	std::unique_ptr<SamplerManager>          Samplers;
	std::unique_ptr<TextureManager>          Textures;
	std::unique_ptr<BufferManager>           Buffers;
	std::unique_ptr<ShaderManager>           Shaders;
	std::unique_ptr<UploadManager>           Uploads;
	std::unique_ptr<DescriptorSetManager>    DescriptorSets;
	std::unique_ptr<RenderPassManager>       RenderPasses;
	std::unique_ptr<FramebufferManager>      Framebuffers;

	// --- Config (defaults set in ctor; managers read these). Bloom/HDR OFF for 1:1. ---
	BITFIELD UseVSync;
	FLOAT    GammaOffset, GammaOffsetRed, GammaOffsetGreen, GammaOffsetBlue;
	BYTE     LinearBrightness, Contrast, Saturation;
	INT      GrayFormula;
	BITFIELD Hdr;
	BYTE     HdrScale;
	BITFIELD Bloom;
	BYTE     BloomAmount;
	FLOAT    LODBias;
	BYTE     AntialiasMode;
	BYTE     GammaMode;
	BYTE     LightMode;
	BITFIELD GammaCorrectScreenshots;
	INT      VkDeviceIndex;
	BITFIELD VkDebug;
	BITFIELD VkExclusiveFullscreen;

	void  RunBloomPass();
	void  BloomStep( VulkanCommandBuffer* cmdbuffer, VulkanPipeline* pipeline, VulkanDescriptorSet* input, VulkanFramebuffer* output, int width, int height, const BloomPushConstants& pushconstants );
	static float ComputeBlurGaussian( float n, float theta );
	static void  ComputeBlurSamples( int sampleCount, float blurAmount, float* sampleWeights );
	void  DrawPresentTexture( int width, int height );
	PresentPushConstants GetPresentPushConstants();

	struct
	{
		int ComplexSurfaces = 0, GouraudPolygons = 0, Tiles = 0, DrawCalls = 0, Uploads = 0, RectUploads = 0;
	} Stats;

	int GetSettingsMultisample() { return 0; }   // [KHG] no MSAA on the mobile path
	// NOTE: Viewport is inherited from URenderDevice (219 base) — do NOT redeclare it here (shadowing
	// would desync the engine's view of the device). SpanBased/FrameBuffered/SupportsFogMaps/etc. are
	// likewise base members, set in the ctor.

private:
	// --- Android surface lifecycle (preserved from the lean device) ---
	UBOOL BringUpVulkan();             // instance + Android surface + device + managers
	UBOOL RecreateAndroidSurface();    // sleep/resume: rebuild VkSurface from the current ANativeWindow
	void  RecreateSwapChainForResume();// resume: force the CommandBufferManager swapchain onto the new surface
	ANativeWindow* GetCurrentNativeWindow();
	void  ShutdownVulkan();

	bool           Initialized = false;   // device + managers up
	void*          WindowHandle = nullptr;// SDL_Window* backing the surface
	ANativeWindow* BoundWindow  = nullptr;// the ANativeWindow our current VkSurface was built from

	// --- Scene projection (219 has no SetSceneNode): recompute per Frame from FSceneNode ---
	void  UpdateSceneNode( FSceneNode* Frame );

	void ClearTextureCache();
	void BlitSceneToPostprocess();

	// --- Bindless batch/vertex system (from UT99) ---
	struct VertexReserveInfo { SceneVertex* vptr; uint32_t* iptr; uint32_t vpos; };

	VertexReserveInfo ReserveVertices( size_t vcount, size_t icount )
	{
		size_t& vpos = SceneVertexPositions[CurrentFrameIndex];
		size_t& ipos = SceneIndexPositions[CurrentFrameIndex];
		if( vpos + vcount > (size_t)BufferManager::SceneVertexBufferSize || ipos + icount > (size_t)BufferManager::SceneIndexBufferSize )
		{
			if( vcount > (size_t)BufferManager::SceneVertexBufferSize || icount > (size_t)BufferManager::SceneIndexBufferSize )
				return { nullptr, nullptr, 0 };
			FlushDrawBatchAndWait();
		}
		return { Buffers->SceneVerticesArray[CurrentFrameIndex] + vpos, Buffers->SceneIndexesArray[CurrentFrameIndex] + ipos, (uint32_t)vpos };
	}
	void UseVertices( size_t vcount, size_t icount )
	{
		SceneVertexPositions[CurrentFrameIndex] += vcount;
		SceneIndexPositions[CurrentFrameIndex]  += icount;
	}
	void FlushDrawBatchAndWait();
	void DrawBatch( VulkanCommandBuffer* cmdbuffer );
	void SubmitAndWait( bool present, int presentWidth, int presentHeight, bool presentFullscreen );
	vec4 ApplyInverseGamma( vec4 color );

	void SetPipeline( PipelineState* pipeline )
	{
		if( pipeline != Batch.Pipeline )
		{
			DrawBatch( Commands->GetDrawCommands() );
			Batch.Pipeline = pipeline;
		}
	}
	ivec4 GetTextureIndexes( DWORD PolyFlags, CachedTexture* tex, bool clamp = false )
	{
		return ivec4( DescriptorSets->GetTextureArrayIndex( PolyFlags, tex, clamp ), 0, 0, 0 );
	}
	ivec4 GetTextureIndexes( DWORD PolyFlags, CachedTexture* tex, CachedTexture* lightmap, CachedTexture* macrotex, CachedTexture* detailtex )
	{
		if( DescriptorSets->IsTextureArrayFull() )
		{
			FlushDrawBatchAndWait();
			DescriptorSets->ClearCache();
			Textures->ClearAllBindlessIndexes();
		}
		ivec4 b;
		b.x = DescriptorSets->GetTextureArrayIndex( PolyFlags, tex );
		b.y = DescriptorSets->GetTextureArrayIndex( 0, macrotex );
		b.z = DescriptorSets->GetTextureArrayIndex( 0, detailtex );
		b.w = DescriptorSets->GetTextureArrayIndex( 0, lightmap );
		return b;
	}

	VkViewport viewportdesc = {};
	UBOOL  UsePrecache = 0;
	FPlane FlashScale, FlashFog;
	FSceneNode* CurrentFrame = nullptr;
	float Aspect = 0, RProjZ = 0, RFX2 = 0, RFY2 = 0;
	bool  IsLocked = false;

	struct { size_t SceneIndexStart = 0; PipelineState* Pipeline = nullptr; } Batch;
	ScenePushConstants pushconstants;

	std::array<size_t, MAX_FRAMES_IN_FLIGHT> SceneVertexPositions = { 0 };
	std::array<size_t, MAX_FRAMES_IN_FLIGHT> SceneIndexPositions  = { 0 };

	// --- Hit testing ---
	struct HitQuery { INT Start = 0; INT Count = 0; };
	BYTE* HitData = nullptr;
	INT*  HitSize = nullptr;
	std::vector<BYTE>     HitQueryStack;
	std::vector<HitQuery> HitQueries;
	std::vector<BYTE>     HitBuffer;
	int      ForceHitIndex = -1;
	HitQuery ForceHit;
	void SetHitLocation();
};

inline float GetUMult( const FTextureInfo& Info ) { return 1.0f / ( Info.UScale * Info.USize ); }
inline float GetVMult( const FTextureInfo& Info ) { return 1.0f / ( Info.VScale * Info.VSize ); }

inline DWORD ApplyPrecedenceRules( DWORD PolyFlags )
{
	if( !( PolyFlags & ( PF_Translucent | PF_Modulated ) ) )
		PolyFlags |= PF_Occlude;
	else if( PolyFlags & PF_Translucent )
		PolyFlags &= ~PF_Masked;
	return PolyFlags;
}
