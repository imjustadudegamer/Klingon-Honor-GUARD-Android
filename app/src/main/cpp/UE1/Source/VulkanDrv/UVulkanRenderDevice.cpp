/*=============================================================================
	UVulkanRenderDevice.cpp: KHG Android Vulkan render device (build-219 URenderDevice).

	Phase 4: the UT99VulkanDrv manager architecture (bindless scene + OpenGL 1:1 math) adapted to the
	219 vtable and our Android surface lifecycle. The manager classes own all Vulkan resources; this
	device drives them. Offscreen Scene render -> Present pass -> swapchain. Shaders are precompiled
	SPIR-V (SceneShaders.h). No glslang on Android; no bloom (Bloom=0, 1:1).

	Two-way merge: the rendering (Init managers, Lock/Unlock scene pass, DrawComplexSurface/Gouraud/Tile,
	bindless batch, present) is UT99's; the surface/instance/device bring-up and the sleep/resume guard
	are KHG's Android pieces (vkCreateAndroidSurfaceKHR from the SDL ANativeWindow).
=============================================================================*/

#include "Precomp.h"
#include "UVulkanRenderDevice.h"
#include "CachedTexture.h"
#include <cmath>
#include <math.h>     // global ::pow/::tan/::sqrt/::expf (NDK <cmath> may keep them in std only)
#include <cstring>    // memcpy
#include <stdexcept>

// [KHG] Surface via vkCreateAndroidSurfaceKHR (not SDL_Vulkan_*): SDL doesn't treat this port's window as a Vulkan window, so build the surface straight from the ANativeWindow.
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#include <android/native_window.h>

// IMPLEMENT_CLASS + CurrentFrameIndex live in VulkanDrv.cpp.


/*-----------------------------------------------------------------------------
	Construction / config.
-----------------------------------------------------------------------------*/

UVulkanRenderDevice::UVulkanRenderDevice()
{
	// URenderDevice base flags (219 base members only; hardware z-buffer device, mirror the GLES driver).
	SpanBased = 0;
	FrameBuffered = 1;
	SupportsFogMaps = 1;
	SupportsDistanceFog = 0;
	VolumetricLighting = 1;
	ShinySurfaces = 1;
	Coronas = 1;
	HighDetailActors = 1;
	NoVolumetricBlend = 1;

	UseVSync = 0;          // Android: present mode chosen by the swapchain; no vsync toggle
	AntialiasMode = 0;
	UsePrecache = 1;
	GammaMode = 0;
	GammaOffset = GammaOffsetRed = GammaOffsetGreen = GammaOffsetBlue = 0.0f;
	LinearBrightness = 128; // neutral
	Contrast = 128;         // neutral
	Saturation = 255;       // neutral
	GrayFormula = 1;
	Hdr = 0;
	HdrScale = 128;
	Bloom = 0;              // [KHG] no bloom (not 1:1)
	BloomAmount = 128;
	LODBias = 0.0f;
	LightMode = 0;          // [KHG] GL parity: no actor *1.5 (flag 32), no lightmap*2 disable (flag 64)
	GlideGamma = 1;         // [KHG] 3dfx Glide look: present gamma = 0.5+1.5*Brightness; 0 = old flat Brightness*2.0
	GammaCorrectScreenshots = 1;
	VkDeviceIndex = 0;
	VkDebug = 0;
	VkExclusiveFullscreen = 0;
}

void UVulkanRenderDevice::InternalClassInitializer( UClass* Class )
{
	guard(UVulkanRenderDevice::InternalClassInitializer);
	new(Class, "UseVSync",         RF_Public) UBoolProperty ( CPP_PROPERTY(UseVSync),         "Options", CPF_Config );
	new(Class, "LinearBrightness", RF_Public) UByteProperty ( CPP_PROPERTY(LinearBrightness), "Options", CPF_Config );
	new(Class, "Contrast",         RF_Public) UByteProperty ( CPP_PROPERTY(Contrast),         "Options", CPF_Config );
	new(Class, "Saturation",       RF_Public) UByteProperty ( CPP_PROPERTY(Saturation),       "Options", CPF_Config );
	new(Class, "LODBias",          RF_Public) UFloatProperty( CPP_PROPERTY(LODBias),          "Options", CPF_Config );
	new(Class, "GlideGamma",       RF_Public) UBoolProperty ( CPP_PROPERTY(GlideGamma),       "Options", CPF_Config );
	new(Class, "VkDeviceIndex",    RF_Public) UIntProperty  ( CPP_PROPERTY(VkDeviceIndex),    "Options", CPF_Config );
	new(Class, "VkDebug",          RF_Public) UBoolProperty ( CPP_PROPERTY(VkDebug),          "Options", CPF_Config );
	unguard;
}

// ZVulkan host hooks (the library declares these; the consumer defines them). 219 ANSI variant.
void VulkanPrintLog( const char* typestr, const std::string& msg )
{
	debugf( NAME_Log, "VulkanDrv[%s]: %s", typestr, msg.c_str() );
}
void VulkanError( const char* text )
{
	throw std::runtime_error( text );
}

/*-----------------------------------------------------------------------------
	Android surface lifecycle.
-----------------------------------------------------------------------------*/

// [KHG] The ANativeWindow backing the SDL window; NULL while backgrounded (no SurfaceView surface), exactly when we must not touch Vulkan presentation.
ANativeWindow* UVulkanRenderDevice::GetCurrentNativeWindow()
{
	SDL_Window* win = (SDL_Window*)WindowHandle;
	if( !win )
		win = Viewport ? (SDL_Window*)Viewport->GetWindow() : NULL;
	if( !win )
		return NULL;
	SDL_SysWMinfo wm; SDL_VERSION( &wm.version );
	if( !SDL_GetWindowWMInfo( win, &wm ) )
		return NULL;
	return wm.info.android.window;
}

// [KHG] Sleep/resume: a new ANativeWindow on resume invalidates the old VkSurface; rebuild the surface and point Device->Surface at it (swapchain Create reads it). Returns 0 if still backgrounded so the caller skips the frame.
UBOOL UVulkanRenderDevice::RecreateAndroidSurface()
{
	guard(UVulkanRenderDevice::RecreateAndroidSurface);
	if( !Instance || !Device )
		return 0;

	ANativeWindow* awin = GetCurrentNativeWindow();
	if( !awin )
		return 0;

	vkDeviceWaitIdle( Device->device );

	VkAndroidSurfaceCreateInfoKHR sci = {};
	sci.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	sci.window = awin;
	VkSurfaceKHR surfaceHandle = VK_NULL_HANDLE;
	VkResult sr = vkCreateAndroidSurfaceKHR( Instance->Instance, &sci, NULL, &surfaceHandle );
	if( sr != VK_SUCCESS )
	{
		debugf( NAME_Warning, "VulkanDrv: resume vkCreateAndroidSurfaceKHR failed: %d", (INT)sr );
		return 0;
	}

	auto newSurface = std::make_shared<VulkanSurface>( Instance, surfaceHandle );
	// [KHG] Keep the old surface alive until RecreateSwapChainForResume destroys the swapchain built on it, else freeing the VkSurface while its VkSwapchainKHR lives is a use-after-free.
	RetiredSurface = Surface;
	Device->Surface = newSurface;   // swapchain Create() reads device->Surface->Surface
	Surface = newSurface;
	BoundWindow = awin;
	return 1;
	unguard;
}

// [KHG] After a resume surface swap the swapchain still points at the old surface but isn't flagged Lost, so force a rebuild against the new surface here (mirrors SubmitCommands' present-time recreate).
void UVulkanRenderDevice::RecreateSwapChainForResume()
{
	guard(UVulkanRenderDevice::RecreateSwapChainForResume);
	if( !Commands || !Commands->SwapChain )
		return;

	vkDeviceWaitIdle( Device->device );

	int w = 0, h = 0;
	SDL_GetWindowSize( (SDL_Window*)WindowHandle, &w, &h );
	if( w <= 0 || h <= 0 ) { w = Viewport->SizeX; h = Viewport->SizeY; }

	Framebuffers->DestroySwapChainFramebuffers();
	Commands->SwapChain->Create( w, h, UseVSync ? 2 : 3, UseVSync ? true : false, Hdr ? true : false, false );
	Framebuffers->CreateSwapChainFramebuffers();
	RetiredSurface.reset();   // [KHG] the old swapchain (built on the retired surface) has now been retired/destroyed inside Create(); safe to free the old VkSurface
	unguard;
}

// [KHG Mali diag] Breadcrumb logger for the Vulkan bring-up. Writes each sub-step to THREE sinks so a
// non-technical reporter can retrieve it: (1) logcat "KHGBoot", (2) the engine log Unreal.log (unbuffered,
// survives a hard SIGSEGV), and (3) a dedicated, obvious file <OBB>/Unreal/KHG_vulkan_boot.log that is
// fflush+fclose'd every line (so the LAST line before a driver segfault names the exact failing step).
#ifdef PLATFORM_ANDROID
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static void KHGBoot( const char* Fmt, ... )
{
	char Buf[1200];
	va_list ap; va_start( ap, Fmt );
	vsnprintf( Buf, sizeof(Buf), Fmt, ap );
	va_end( ap );
	__android_log_write( ANDROID_LOG_ERROR, "KHGBoot", Buf );
	debugf( NAME_Log, "[KHGBoot] %s", Buf );
	const char* Root = getenv( "UE1_ANDROID_ROOT" );
	char Path[1400];
	snprintf( Path, sizeof(Path), "%s/KHG_vulkan_boot.log", (Root && Root[0]) ? Root : "." );
	FILE* F = fopen( Path, "a" );
	if( F ) { fprintf( F, "%s\n", Buf ); fflush( F ); fclose( F ); }
}
#else
static void KHGBoot( const char*, ... ) {}
#endif

// Instance + Android surface + bindless device + the nine managers.
UBOOL UVulkanRenderDevice::BringUpVulkan()
{
	guard(UVulkanRenderDevice::BringUpVulkan);
	KHGBoot( "================ KHG Vulkan bring-up ================" );
	KHGBoot( "BringUpVulkan: ENTER" );

	SDL_Window* win = (SDL_Window*)Viewport->GetWindow();
	if( !win )
	{
		debugf( NAME_Warning, "VulkanDrv: no SDL window" );
		return 0;
	}
	WindowHandle = win;

	// Instance with the Android surface extensions; ZVulkan's VulkanInstance ctor runs volk init so vkCreateAndroidSurfaceKHR is live afterwards.
	VulkanInstanceBuilder ib;
	ib.ApiVersionsToTry( { VK_API_VERSION_1_1, VK_API_VERSION_1_0 } );
	ib.RequireExtension( VK_KHR_SURFACE_EXTENSION_NAME );
	ib.RequireExtension( VK_KHR_ANDROID_SURFACE_EXTENSION_NAME );
	ib.OptionalExtension( VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME ); // HDR colorspace query (Hdr=0)
	if( VkDebug )
		ib.DebugLayer( true );
	KHGBoot( "step 1: create VkInstance (api 1.1 -> 1.0)" );
	Instance = ib.Create();
	KHGBoot( "step 1: VkInstance OK" );

	// Pull the native window from SDL and make the surface ourselves.
	SDL_SysWMinfo wm; SDL_VERSION( &wm.version );
	if( !SDL_GetWindowWMInfo( win, &wm ) )
	{
		debugf( NAME_Warning, "VulkanDrv: SDL_GetWindowWMInfo failed: %s", SDL_GetError() );
		return 0;
	}
	ANativeWindow* awin = wm.info.android.window;
	if( !awin )
	{
		debugf( NAME_Warning, "VulkanDrv: no ANativeWindow" );
		return 0;
	}
	debugf( NAME_Log, "VulkanDrv: ANativeWindow=%p %dx%d", (void*)awin,
		ANativeWindow_getWidth( awin ), ANativeWindow_getHeight( awin ) );
	KHGBoot( "step 2: ANativeWindow %dx%d, creating VkAndroidSurfaceKHR",
		ANativeWindow_getWidth( awin ), ANativeWindow_getHeight( awin ) );

	VkAndroidSurfaceCreateInfoKHR sci = {};
	sci.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	sci.window = awin;
	VkSurfaceKHR surfaceHandle = VK_NULL_HANDLE;
	VkResult sr = vkCreateAndroidSurfaceKHR( Instance->Instance, &sci, NULL, &surfaceHandle );
	if( sr != VK_SUCCESS )
	{
		debugf( NAME_Warning, "VulkanDrv: vkCreateAndroidSurfaceKHR failed: %d", (INT)sr );
		return 0;
	}
	Surface = std::make_shared<VulkanSurface>( Instance, surfaceHandle );
	BoundWindow = awin;

	// Bindless device: RequireExtension(DESCRIPTOR_INDEXING) so an absent EXT rejects the device (GLES fallback), vs Optional which would false-positive the bindless check then crash.
	auto deviceBuilder = VulkanDeviceBuilder();
	deviceBuilder.Surface( Surface );
	deviceBuilder.RequireExtension( VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME );
	deviceBuilder.OptionalDescriptorIndexing();
	deviceBuilder.SelectDevice( VkDeviceIndex );
	KHGBoot( "step 3: create VkDevice (require descriptor_indexing)" );
	Device = deviceBuilder.Create( Instance );
	KHGBoot( "step 3: VkDevice OK" );

	// [KHG Mali diag] Dump the EXACT driver identity + descriptor-indexing caps/limits. This is what
	// distinguishes the crash hypotheses: if descriptorBindingSampledImageUpdateAfterBind is enabled=0,
	// or a UAB limit is < the 16536-wide bindless array we build, an old Mali (r32p1) segfaults building
	// the descriptor pool/set. On Adreno the feature is 1 and the limits are huge (so nothing changes).
	{
		VkPhysicalDevice pd = Device->PhysicalDevice.Device;
		VkPhysicalDeviceDriverProperties drv = {};
		drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		VkPhysicalDeviceProperties2 p2 = {};
		p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		p2.pNext = &drv;
		if( vkGetPhysicalDeviceProperties2 )
			vkGetPhysicalDeviceProperties2( pd, &p2 );
		const auto& P    = Device->PhysicalDevice.Properties.Properties;
		const auto& DIp  = Device->PhysicalDevice.Properties.DescriptorIndexing;
		const auto& DIs  = Device->PhysicalDevice.Features.DescriptorIndexing;   // supported
		const auto& DIe  = Device->EnabledFeatures.DescriptorIndexing;           // enabled at vkCreateDevice
		KHGBoot( "GPU '%s' api %u.%u.%u drvVer=0x%08x vendor=0x%04x device=0x%04x",
			P.deviceName, VK_VERSION_MAJOR(P.apiVersion), VK_VERSION_MINOR(P.apiVersion),
			VK_VERSION_PATCH(P.apiVersion), P.driverVersion, P.vendorID, P.deviceID );
		KHGBoot( "Driver name='%s' info='%s'", drv.driverName, drv.driverInfo );
		KHGBoot( "DI supported: partialBound=%d runtimeArr=%d nonUniform=%d varCount=%d sampledImgUAB=%d",
			(int)DIs.descriptorBindingPartiallyBound, (int)DIs.runtimeDescriptorArray,
			(int)DIs.shaderSampledImageArrayNonUniformIndexing, (int)DIs.descriptorBindingVariableDescriptorCount,
			(int)DIs.descriptorBindingSampledImageUpdateAfterBind );
		KHGBoot( "DI enabled:   partialBound=%d runtimeArr=%d nonUniform=%d varCount=%d sampledImgUAB=%d",
			(int)DIe.descriptorBindingPartiallyBound, (int)DIe.runtimeDescriptorArray,
			(int)DIe.shaderSampledImageArrayNonUniformIndexing, (int)DIe.descriptorBindingVariableDescriptorCount,
			(int)DIe.descriptorBindingSampledImageUpdateAfterBind );
		KHGBoot( "Limits: maxPerStageSampledImg=%u maxPerStageUAB_SampledImg=%u maxSetUAB_SampledImg=%u maxUAB_all=%u (bindless ceiling=16536)",
			P.limits.maxPerStageDescriptorSampledImages, DIp.maxPerStageDescriptorUpdateAfterBindSampledImages,
			DIp.maxDescriptorSetUpdateAfterBindSampledImages, DIp.maxUpdateAfterBindDescriptorsInAllPools );
	}

	bool supportsBindless =
		Device->EnabledFeatures.DescriptorIndexing.descriptorBindingPartiallyBound &&
		Device->EnabledFeatures.DescriptorIndexing.runtimeDescriptorArray &&
		Device->EnabledFeatures.DescriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
	if( !supportsBindless )
	{
		debugf( NAME_Warning, "VulkanDrv: GPU lacks bindless textures -> GLES fallback" );
		KHGBoot( "ABORT: GPU lacks required bindless features (partialBound/runtimeArray/nonUniform) -> return 0" );
		return 0;
	}

	const auto& props = Device->PhysicalDevice.Properties.Properties;
	debugf( NAME_Log, "VulkanDrv: device '%s' api %d.%d.%d maxTex=%d", props.deviceName,
		VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
		props.limits.maxImageDimension2D );

	// Construct the managers; CommandBufferManager builds the swapchain object (its images are created lazily on first present).
	KHGBoot( "step 4: construct BufferManager" );        Buffers.reset( new BufferManager( this ) );
	KHGBoot( "step 4: construct CommandBufferManager (builds swapchain)" ); Commands.reset( new CommandBufferManager( this ) );
	KHGBoot( "step 4: construct SamplerManager" );       Samplers.reset( new SamplerManager( this ) );
	KHGBoot( "step 4: construct TextureManager" );       Textures.reset( new TextureManager( this ) );
	KHGBoot( "step 4: construct ShaderManager" );        Shaders.reset( new ShaderManager( this ) );
	KHGBoot( "step 4: construct UploadManager" );        Uploads.reset( new UploadManager( this ) );
	KHGBoot( "step 4: construct DescriptorSetManager (bindless UpdateAfterBind pool <-- prime suspect)" ); DescriptorSets.reset( new DescriptorSetManager( this ) );
	KHGBoot( "step 4: construct RenderPassManager (eager bloom pipeline)" ); RenderPasses.reset( new RenderPassManager( this ) );
	KHGBoot( "step 4: construct FramebufferManager (eager present pipeline)" ); Framebuffers.reset( new FramebufferManager( this ) );

	KHGBoot( "BringUpVulkan: ALL 9 MANAGERS OK -> return 1 (SUCCESS - Vulkan is up)" );
	return 1;
	unguard;
}

void UVulkanRenderDevice::ShutdownVulkan()
{
	if( Device )
		vkDeviceWaitIdle( Device->device );

	Framebuffers.reset();
	RenderPasses.reset();
	DescriptorSets.reset();
	Uploads.reset();
	Shaders.reset();
	Buffers.reset();
	Textures.reset();
	Samplers.reset();
	Commands.reset();

	Device.reset();
	Surface.reset();
	Instance.reset();
	BoundWindow = nullptr;
	Initialized = false;
}

/*-----------------------------------------------------------------------------
	URenderDevice contract (219).
-----------------------------------------------------------------------------*/

UBOOL UVulkanRenderDevice::Init( UViewport* InViewport )
{
	guard(UVulkanRenderDevice::Init);
	Viewport = InViewport;

	try
	{
		if( !BringUpVulkan() )
		{
			ShutdownVulkan();
			return 0;
		}
	}
	catch( const std::exception& e )
	{
		debugf( NAME_Warning, "VulkanDrv: init failed (%s) -> GLES fallback", e.what() );
		ShutdownVulkan();
		return 0;
	}
	catch( ... )
	{
		debugf( NAME_Warning, "VulkanDrv: init failed (unknown) -> GLES fallback" );
		ShutdownVulkan();
		return 0;
	}

	Initialized = true;
	debugf( NAME_Log, "VulkanDrv: initialised (%dx%d)", Viewport->SizeX, Viewport->SizeY );
	return 1;
	unguard;
}

void UVulkanRenderDevice::Exit()
{
	guard(UVulkanRenderDevice::Exit);
	ShutdownVulkan();
	unguard;
}

void UVulkanRenderDevice::SubmitAndWait( bool present, int presentWidth, int presentHeight, bool presentFullscreen )
{
	DescriptorSets->UpdateBindlessSet();
	Commands->SubmitCommands( present, presentWidth, presentHeight, presentFullscreen );

	Batch.SceneIndexStart = 0;
	SceneVertexPositions[CurrentFrameIndex] = 0;
	SceneIndexPositions[CurrentFrameIndex] = 0;
}

void UVulkanRenderDevice::Flush()
{
	guard(UVulkanRenderDevice::Flush);

	if( IsLocked )
	{
		DrawBatch( Commands->GetDrawCommands() );
		Commands->GetDrawCommands()->endRenderPass();
		SubmitAndWait( false, 0, 0, false );

		ClearTextureCache();

		auto cmdbuffer = Commands->GetDrawCommands();

		VkAccessFlags srcColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkAccessFlags dstColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkAccessFlags srcDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkAccessFlags dstDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		PipelineBarrier()
			.AddImage(Textures->Scene->ColorBuffer.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
			.AddImage(Textures->Scene->HitBuffer.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
			.AddImage(Textures->Scene->DepthBuffer.get(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, srcDepthAccess, dstDepthAccess, VK_IMAGE_ASPECT_DEPTH_BIT)
			.Execute(cmdbuffer, srcStages, dstStages);

		RenderPassBegin()
			.RenderPass(RenderPasses->Scene.RenderPassContinue.get())
			.Framebuffer(Framebuffers->SceneFramebuffer.get())
			.RenderArea(0, 0, Textures->Scene->Width, Textures->Scene->Height)
			.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f)
			.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f)
			.AddClearDepthStencil(1.0f, 0)
			.Execute(cmdbuffer);

		VkBuffer vertexBuffers[] = { Buffers->SceneVertexBuffers[CurrentFrameIndex]->buffer };
		VkDeviceSize offsets[] = { 0 };
		cmdbuffer->bindVertexBuffers(0, 1, vertexBuffers, offsets);
		cmdbuffer->bindIndexBuffer(Buffers->SceneIndexBuffers[CurrentFrameIndex]->buffer, 0, VK_INDEX_TYPE_UINT32);
	}
	else
	{
		ClearTextureCache();
	}

	// [KHG] 219's URenderDevice has no PrecacheOnFlip; precache is driven by PrecacheTexture calls serviced in DrawComplexSurface/Tile, so nothing to flag here.

	unguard;
}

UBOOL UVulkanRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	return 0;
}

void UVulkanRenderDevice::Lock( FPlane InFlashScale, FPlane InFlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
	guard(UVulkanRenderDevice::Lock);
	if( !Initialized )
		return;

	// [KHG] Android sleep/resume: no native window means backgrounded, so skip the frame (IsLocked stays false); on a new window, rebuild the surface and swapchain before rendering.
	ANativeWindow* awin = GetCurrentNativeWindow();
	if( !awin )
		return;
	if( awin != BoundWindow )
	{
		if( !RecreateAndroidSurface() )
			return;
		RecreateSwapChainForResume();
	}

	HitData = InHitData;
	HitSize = InHitSize;
	FlashScale = InFlashScale;
	FlashFog = InFlashFog;
	pushconstants.hitIndex = 0;
	ForceHitIndex = -1;
	CurrentFrame = nullptr;   // force UpdateSceneNode to recompute the projection this frame


	try
	{
		// If the frame textures no longer match the window, recreate them along with the scene pass.
		if( !Textures->Scene || Textures->Scene->Width != Viewport->SizeX || Textures->Scene->Height != Viewport->SizeY || Textures->Scene->Multisample != GetSettingsMultisample() )
		{
			// [KHG] idle both in-flight frames before freeing/recreating the scene images/pass/pipelines: they bypass the per-frame DeleteList, so the other frame may still be using them (use-after-free).
			vkDeviceWaitIdle( Device->device );
			Framebuffers->DestroySceneFramebuffer();
			Textures->Scene.reset();
			Textures->Scene.reset( new SceneTextures( this, Viewport->SizeX, Viewport->SizeY, GetSettingsMultisample() ) );
			RenderPasses->CreateRenderPass();
			RenderPasses->CreatePipelines();
			Framebuffers->CreateSceneFramebuffer();
			DescriptorSets->UpdateFrameDescriptors();
		}

		auto cmdbuffer = Commands->GetDrawCommands();

		VkAccessFlags srcColorAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkAccessFlags dstColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkAccessFlags srcDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkAccessFlags dstDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		PipelineBarrier()
			.AddImage(Textures->Scene->ColorBuffer.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
			.AddImage(Textures->Scene->HitBuffer.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
			.AddImage(Textures->Scene->DepthBuffer.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, srcDepthAccess, dstDepthAccess, VK_IMAGE_ASPECT_DEPTH_BIT)
			.Execute(cmdbuffer, srcStages, dstStages);

		RenderPassBegin()
			.RenderPass(RenderPasses->Scene.RenderPass.get())
			.Framebuffer(Framebuffers->SceneFramebuffer.get())
			.RenderArea(0, 0, Textures->Scene->Width, Textures->Scene->Height)
			.AddClearColor(ScreenClear.X, ScreenClear.Y, ScreenClear.Z, ScreenClear.W)
			.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f)
			.AddClearDepthStencil(1.0f, 0)
			.Execute(cmdbuffer);

		VkBuffer vertexBuffers[] = { Buffers->SceneVertexBuffers[CurrentFrameIndex]->buffer };
		VkDeviceSize offsets[] = { 0 };
		cmdbuffer->bindVertexBuffers(0, 1, vertexBuffers, offsets);
		cmdbuffer->bindIndexBuffer(Buffers->SceneIndexBuffers[CurrentFrameIndex]->buffer, 0, VK_INDEX_TYPE_UINT32);

		IsLocked = true;
	}
	catch( const std::exception& e )
	{
		debugf( NAME_Warning, "VulkanDrv: Lock failed: %s", e.what() );
	}

	unguard;
}

void UVulkanRenderDevice::FlushDrawBatchAndWait()
{
	DrawBatch( Commands->GetDrawCommands() );
	Commands->GetDrawCommands()->endRenderPass();
	SubmitAndWait( false, 0, 0, false );

	auto drawcommands = Commands->GetDrawCommands();

	VkAccessFlags srcColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	VkAccessFlags dstColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	VkAccessFlags srcDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	VkAccessFlags dstDepthAccess = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

	PipelineBarrier()
		.AddImage(Textures->Scene->ColorBuffer.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
		.AddImage(Textures->Scene->HitBuffer.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
		.AddImage(Textures->Scene->DepthBuffer.get(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, srcDepthAccess, dstDepthAccess, VK_IMAGE_ASPECT_DEPTH_BIT)
		.Execute(drawcommands, srcStages, dstStages);

	RenderPassBegin()
		.RenderPass(RenderPasses->Scene.RenderPassContinue.get())
		.Framebuffer(Framebuffers->SceneFramebuffer.get())
		.RenderArea(0, 0, Textures->Scene->Width, Textures->Scene->Height)
		.Execute(drawcommands);

	VkBuffer vertexBuffers[] = { Buffers->SceneVertexBuffers[CurrentFrameIndex]->buffer };
	VkDeviceSize offsets[] = { 0 };
	drawcommands->bindVertexBuffers(0, 1, vertexBuffers, offsets);
	drawcommands->bindIndexBuffer(Buffers->SceneIndexBuffers[CurrentFrameIndex]->buffer, 0, VK_INDEX_TYPE_UINT32);
	drawcommands->setViewport(0, 1, &viewportdesc);
}

void UVulkanRenderDevice::Unlock( UBOOL Blit )
{
	guard(UVulkanRenderDevice::Unlock);
	if( !Initialized || !IsLocked )
		return;


	try
	{
		DrawBatch( Commands->GetDrawCommands() );
		Commands->GetDrawCommands()->endRenderPass();

		BlitSceneToPostprocess();
		if( Bloom )
			RunBloomPass();

		int windowWidth = 0, windowHeight = 0;
		SDL_GetWindowSize( (SDL_Window*)WindowHandle, &windowWidth, &windowHeight );
		if( windowWidth <= 0 || windowHeight <= 0 ) { windowWidth = Viewport->SizeX; windowHeight = Viewport->SizeY; }

		Stats.ComplexSurfaces = Stats.GouraudPolygons = Stats.Tiles = Stats.DrawCalls = 0;

		SubmitAndWait( Blit ? true : false, windowWidth, windowHeight, false );

		Batch.Pipeline = nullptr;

		if( Samplers->LODBias != LODBias )
		{
			// [KHG] idle the GPU before recreating samplers: the just-submitted frame's bindless set still references the old VkSamplers that CreateSceneSamplers frees.
			vkDeviceWaitIdle( Device->device );
			DescriptorSets->ClearCache();
			Textures->ClearAllBindlessIndexes();
			Samplers->CreateSceneSamplers();
		}

		if( HitData )
		{
			int width = Viewport->HitXL;
			int height = Viewport->HitYL;
			int hit = 0;
			const int32_t* data = (const int32_t*)Textures->Scene->StagingHitBuffer->Map(0, width * height * sizeof(int32_t));
			if( data )
			{
				for( int y = 0; y < height; y++ )
				{
					const INT* line = data + y * width;
					for( int x = 0; x < width; x++ )
						hit = std::max(hit, line[x]);
				}
				Textures->Scene->StagingHitBuffer->Unmap();
			}
			hit--;
			hit = std::max(hit, ForceHitIndex);

			if( hit >= 0 && hit < (int)HitQueries.size() )
			{
				const HitQuery& query = HitQueries[hit];
				memcpy(HitData, HitBuffer.data() + query.Start, query.Count);
				*HitSize = query.Count;
			}
			else
			{
				*HitSize = 0;
			}
		}

		HitQueryStack.clear();
		HitQueries.clear();
		HitBuffer.clear();
		HitData = nullptr;
		HitSize = nullptr;

		IsLocked = false;
	}
	catch( std::exception& e )
	{
		debugf( NAME_Warning, "VulkanDrv: Unlock failed: %s", e.what() );
		IsLocked = false;
	}

	unguard;
}

void UVulkanRenderDevice::DrawBatch( VulkanCommandBuffer* cmdbuffer )
{
	size_t SceneIndexPos = SceneIndexPositions[CurrentFrameIndex];
	size_t icount = SceneIndexPos - Batch.SceneIndexStart;
	if( icount > 0 )
	{
		if( viewportdesc.minDepth != Batch.Pipeline->MinDepth || viewportdesc.maxDepth != Batch.Pipeline->MaxDepth )
		{
			viewportdesc.minDepth = Batch.Pipeline->MinDepth;
			viewportdesc.maxDepth = Batch.Pipeline->MaxDepth;
			cmdbuffer->setViewport(0, 1, &viewportdesc);
		}

		auto layout = RenderPasses->Scene.BindlessPipelineLayout.get();
		cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, Batch.Pipeline->Pipeline.get());
		cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, DescriptorSets->GetBindlessSet());
		cmdbuffer->pushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ScenePushConstants), &pushconstants);
		cmdbuffer->drawIndexed(icount, 1, Batch.SceneIndexStart, 0, 0);
		Batch.SceneIndexStart = SceneIndexPos;
		Stats.DrawCalls++;
	}
}

// [KHG] 219 has no SetSceneNode: recompute the projection per frame. Forward-Z frustum (zero_positive_w) to match UT99's Scene pipelines (depth clear 1.0, COMPARE_OP_LESS); FOV from live FovAngle, falling back to DesiredFOV if out of range.
void UVulkanRenderDevice::UpdateSceneNode( FSceneNode* Frame )
{
	guardSlow(UVulkanRenderDevice::UpdateSceneNode);

	auto commands = Commands->GetDrawCommands();
	DrawBatch( commands );

	CurrentFrame = Frame;
	Aspect = Frame->FY / Frame->FX;

	float fov = ( Viewport && Viewport->Actor ) ? Viewport->Actor->FovAngle : 90.0f;
	if( fov < 1.0f || fov > 170.0f )
	{
		float dfov = ( Viewport && Viewport->Actor ) ? Viewport->Actor->DesiredFOV : 90.0f;
		fov = ( dfov >= 1.0f && dfov <= 170.0f ) ? dfov : 90.0f;
	}
	RProjZ = (float)tan( fov * (PI / 360.0) );   // tan(FovAngle/2)
	RFX2 = 2.0f * RProjZ / Frame->FX;
	RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;

	viewportdesc = {};
	viewportdesc.x = Frame->XB;
	viewportdesc.y = Frame->YB;
	viewportdesc.width = Frame->X;
	viewportdesc.height = Frame->Y;
	viewportdesc.minDepth = 0.1f;
	viewportdesc.maxDepth = 1.0f;
	commands->setViewport(0, 1, &viewportdesc);

	pushconstants.objectToProjection = mat4::frustum( -RProjZ, RProjZ, -Aspect * RProjZ, Aspect * RProjZ, 1.0f, 32768.0f, handedness::left, clipzrange::zero_positive_w );
	pushconstants.nearClip = vec4( Frame->NearClip.X, Frame->NearClip.Y, Frame->NearClip.Z, -Frame->NearClip.W );

	unguardSlow;
}

void UVulkanRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guardSlow(UVulkanRenderDevice::DrawComplexSurface);
	if( !IsLocked )
		return;
	if( Frame != CurrentFrame )
		UpdateSceneNode( Frame );

	DWORD PolyFlags = ApplyPrecedenceRules( Surface.PolyFlags );

	// [KHG] 219 FTextureInfo has no UTexture backpointer, so mask purely off PolyFlags like the 219 GL driver (dropping the lean device's CacheID->UTexture mask-forcing hack).
	CachedTexture* tex = Textures->GetTexture(Surface.Texture, (PolyFlags & PF_Masked) != 0);
	CachedTexture* lightmap = Textures->GetTexture(Surface.LightMap, false);
	CachedTexture* macrotex = Textures->GetTexture(Surface.MacroTexture, false);
	CachedTexture* detailtex = Textures->GetTexture(Surface.DetailTexture, false);
	CachedTexture* fogmap = (Surface.FogMap && Surface.FogMap->Mips[0] && Surface.FogMap->Mips[0]->DataPtr) ? Textures->GetTexture(Surface.FogMap, false) : nullptr;

	if( Surface.DetailTexture && Surface.FogMap ) detailtex = nullptr;

	float UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	float VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;

	float UPan = tex ? UDot + Surface.Texture->Pan.X : 0.0f;
	float VPan = tex ? VDot + Surface.Texture->Pan.Y : 0.0f;
	float UMult = tex ? GetUMult(*Surface.Texture) : 0.0f;
	float VMult = tex ? GetVMult(*Surface.Texture) : 0.0f;
	float LMUPan = lightmap ? UDot + Surface.LightMap->Pan.X - 0.5f * Surface.LightMap->UScale : 0.0f;
	float LMVPan = lightmap ? VDot + Surface.LightMap->Pan.Y - 0.5f * Surface.LightMap->VScale : 0.0f;
	float LMUMult = lightmap ? GetUMult(*Surface.LightMap) : 0.0f;
	float LMVMult = lightmap ? GetVMult(*Surface.LightMap) : 0.0f;
	float MacroUPan = macrotex ? UDot + Surface.MacroTexture->Pan.X : 0.0f;
	float MacroVPan = macrotex ? VDot + Surface.MacroTexture->Pan.Y : 0.0f;
	float MacroUMult = macrotex ? GetUMult(*Surface.MacroTexture) : 0.0f;
	float MacroVMult = macrotex ? GetVMult(*Surface.MacroTexture) : 0.0f;
	float DetailUPan = UPan;
	float DetailVPan = VPan;
	float DetailUMult = detailtex ? GetUMult(*Surface.DetailTexture) : 0.0f;
	float DetailVMult = detailtex ? GetVMult(*Surface.DetailTexture) : 0.0f;

	uint32_t flags = 0;
	if( lightmap ) flags |= 1;
	if( macrotex ) flags |= 2;
	if( detailtex && !fogmap ) flags |= 4;
	if( fogmap ) flags |= 8;
	if( LightMode == 1 ) flags |= 64;

	if( fogmap ) // if Surface.FogMap exists, use instead of detail texture
	{
		detailtex = fogmap;
		DetailUPan = UDot + Surface.FogMap->Pan.X - 0.5f * Surface.FogMap->UScale;
		DetailVPan = VDot + Surface.FogMap->Pan.Y - 0.5f * Surface.FogMap->VScale;
		DetailUMult = GetUMult(*Surface.FogMap);
		DetailVMult = GetVMult(*Surface.FogMap);
	}

	SetPipeline( RenderPasses->GetPipeline(PolyFlags) );

	ivec4 textureBinds = GetTextureIndexes(PolyFlags, tex, lightmap, macrotex, detailtex);
	vec4 color(1.0f);

	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		auto pts = Poly->Pts;
		uint32_t vcount = Poly->NumPts;
		if( vcount < 3 ) continue;

		uint32_t icount = (vcount - 2) * 3;
		auto alloc = ReserveVertices(vcount, icount);
		if( alloc.vptr )
		{
			SceneVertex* vptr = alloc.vptr;
			uint32_t* iptr = alloc.iptr;
			uint32_t vpos = alloc.vpos;

			for( uint32_t i = 0; i < vcount; i++ )
			{
				FVector point = pts[i]->Point;
				FLOAT u = Facet.MapCoords.XAxis | point;
				FLOAT v = Facet.MapCoords.YAxis | point;

				vptr->Flags = flags;
				vptr->Position.x = point.X;
				vptr->Position.y = point.Y;
				vptr->Position.z = point.Z;
				vptr->TexCoord.s = (u - UPan) * UMult;
				vptr->TexCoord.t = (v - VPan) * VMult;
				vptr->TexCoord2.s = (u - LMUPan) * LMUMult;
				vptr->TexCoord2.t = (v - LMVPan) * LMVMult;
				vptr->TexCoord3.s = (u - MacroUPan) * MacroUMult;
				vptr->TexCoord3.t = (v - MacroVPan) * MacroVMult;
				vptr->TexCoord4.s = (u - DetailUPan) * DetailUMult;
				vptr->TexCoord4.t = (v - DetailVPan) * DetailVMult;
				vptr->Color = color;
				vptr->TextureBinds = textureBinds;
				vptr++;
			}

			for( uint32_t i = vpos + 2; i < vpos + vcount; i++ )
			{
				*(iptr++) = vpos;
				*(iptr++) = i - 1;
				*(iptr++) = i;
			}

			UseVertices(vcount, icount);
		}
	}

	Stats.ComplexSurfaces++;

	unguardSlow;
}

void UVulkanRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span )
{
	guardSlow(UVulkanRenderDevice::DrawGouraudPolygon);
	if( !IsLocked || NumPts < 3 ) return; // NumPts < 3 can apparently happen
	if( Frame != CurrentFrame )
		UpdateSceneNode( Frame );

	PolyFlags = ApplyPrecedenceRules( PolyFlags );

	SetPipeline( RenderPasses->GetPipeline(PolyFlags) );

	CachedTexture* tex = Textures->GetTexture(&Info, !!(PolyFlags & PF_Masked));
	ivec4 textureBinds = GetTextureIndexes(PolyFlags, tex);

	float UMult = GetUMult(Info);
	float VMult = GetVMult(Info);
	int flags = (PolyFlags & (PF_RenderFog | PF_Translucent | PF_Modulated)) == PF_RenderFog ? 16 : 0;

	if( (PolyFlags & (PF_Translucent | PF_Modulated)) == 0 && LightMode == 2 ) flags |= 32;

	auto alloc = ReserveVertices(NumPts, (NumPts - 2) * 3);
	if( alloc.vptr )
	{
		SceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		if( PolyFlags & PF_Modulated )
		{
			SceneVertex* vertex = vptr;
			for( INT i = 0; i < NumPts; i++ )
			{
				FTransTexture* P = Pts[i];
				vertex->Flags = flags;
				vertex->Position.x = P->Point.X;
				vertex->Position.y = P->Point.Y;
				vertex->Position.z = P->Point.Z;
				vertex->TexCoord.s = P->U * UMult;
				vertex->TexCoord.t = P->V * VMult;
				vertex->TexCoord2.s = P->Fog.X;
				vertex->TexCoord2.t = P->Fog.Y;
				vertex->TexCoord3.s = P->Fog.Z;
				vertex->TexCoord3.t = P->Fog.W;
				vertex->TexCoord4.s = 0.0f;
				vertex->TexCoord4.t = 0.0f;
				vertex->Color.r = 1.0f;
				vertex->Color.g = 1.0f;
				vertex->Color.b = 1.0f;
				vertex->Color.a = 1.0f;
				vertex->TextureBinds = textureBinds;
				vertex++;
			}
		}
		else
		{
			SceneVertex* vertex = vptr;
			for( INT i = 0; i < NumPts; i++ )
			{
				FTransTexture* P = Pts[i];
				vertex->Flags = flags;
				vertex->Position.x = P->Point.X;
				vertex->Position.y = P->Point.Y;
				vertex->Position.z = P->Point.Z;
				vertex->TexCoord.s = P->U * UMult;
				vertex->TexCoord.t = P->V * VMult;
				vertex->TexCoord2.s = P->Fog.X;
				vertex->TexCoord2.t = P->Fog.Y;
				vertex->TexCoord3.s = P->Fog.Z;
				vertex->TexCoord3.t = P->Fog.W;
				vertex->TexCoord4.s = 0.0f;
				vertex->TexCoord4.t = 0.0f;
				vertex->Color.r = P->Light.X;
				vertex->Color.g = P->Light.Y;
				vertex->Color.b = P->Light.Z;
				vertex->Color.a = 1.0f;
				vertex->TextureBinds = textureBinds;
				vertex++;
			}
		}

		uint32_t vstart = vpos;
		uint32_t vcount = NumPts;
		for( uint32_t i = vstart + 2; i < vstart + vcount; i++ )
		{
			*(iptr++) = vstart;
			*(iptr++) = i - 1;
			*(iptr++) = i;
		}

		UseVertices(NumPts, (NumPts - 2) * 3);
	}

	Stats.GouraudPolygons++;

	unguardSlow;
}

void UVulkanRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags )
{
	guardSlow(UVulkanRenderDevice::DrawTile);
	if( !IsLocked )
		return;
	if( Frame != CurrentFrame )
		UpdateSceneNode( Frame );


	PolyFlags = ApplyPrecedenceRules( PolyFlags );

	// TODO(KHG): FMV/ScriptedTexture tiles wrap 8-bit BGRA as TEXF_RGB32 and route through the lightmap <<1 uploader (double-brightens); if FMV blows out, give realtime tiles a non-doubling path. (7-bit lightmaps need the <<1.)
	CachedTexture* tex = Textures->GetTexture(&Info, (PolyFlags & PF_Masked) != 0);  // 219: no UTexture backpointer
	float UMult = tex ? GetUMult(Info) : 0.0f;
	float VMult = tex ? GetVMult(Info) : 0.0f;
	float u0 = U * UMult;
	float v0 = V * VMult;
	float u1 = (U + UL) * UMult;
	float v1 = (V + VL) * VMult;
	bool clamp = (u0 >= 0.0f && u1 <= 1.00001f && v0 >= 0.0f && v1 <= 1.00001f);

	SetPipeline( RenderPasses->GetPipeline(PolyFlags) );
	ivec4 textureBinds = GetTextureIndexes(PolyFlags, tex, clamp);

	float r, g, b, a;
	if( PolyFlags & PF_Modulated )
	{
		r = 1.0f; g = 1.0f; b = 1.0f;
	}
	else
	{
		r = Color.X; g = Color.Y; b = Color.Z;
	}
	a = 1.0f;

	if( Textures->Scene->Multisample > 1 )
	{
		XL = std::floor(X + XL + 0.5f);
		YL = std::floor(Y + YL + 0.5f);
		X = std::floor(X + 0.5f);
		Y = std::floor(Y + 0.5f);
		XL = XL - X;
		YL = YL - Y;
	}

	auto alloc = ReserveVertices(4, 6);
	if( alloc.vptr )
	{
		SceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		vptr[0] = { 0, vec3(RFX2 * Z * (X - Frame->FX2),      RFY2 * Z * (Y - Frame->FY2),      Z), vec2(u0, v0), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec4(r, g, b, a), textureBinds };
		vptr[1] = { 0, vec3(RFX2 * Z * (X + XL - Frame->FX2), RFY2 * Z * (Y - Frame->FY2),      Z), vec2(u1, v0), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec4(r, g, b, a), textureBinds };
		vptr[2] = { 0, vec3(RFX2 * Z * (X + XL - Frame->FX2), RFY2 * Z * (Y + YL - Frame->FY2), Z), vec2(u1, v1), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec4(r, g, b, a), textureBinds };
		vptr[3] = { 0, vec3(RFX2 * Z * (X - Frame->FX2),      RFY2 * Z * (Y + YL - Frame->FY2), Z), vec2(u0, v1), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec4(r, g, b, a), textureBinds };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;
		iptr[2] = vpos + 2;
		iptr[3] = vpos;
		iptr[4] = vpos + 2;
		iptr[5] = vpos + 3;

		UseVertices(4, 6);
	}

	Stats.Tiles++;

	unguardSlow;
}

vec4 UVulkanRenderDevice::ApplyInverseGamma( vec4 color )
{
	if( Viewport->IsOrtho() )
		return color;
	// [KHG] 3dfx Glide gamma ramp: present gamma = 0.5+1.5*Brightness; GlideGamma=0 restores the flat Brightness*2.0.
	float brightness = Clamp( (float)( GlideGamma ? (0.5 + 1.5 * Viewport->Client->Brightness) : (Viewport->Client->Brightness * 2.0) ), 0.05f, 2.99f );
	float gammaRed = Max( brightness + GammaOffset + GammaOffsetRed, 0.001f );
	float gammaGreen = Max( brightness + GammaOffset + GammaOffsetGreen, 0.001f );
	float gammaBlue = Max( brightness + GammaOffset + GammaOffsetBlue, 0.001f );
	return vec4( pow(color.r, gammaRed), pow(color.g, gammaGreen), pow(color.b, gammaBlue), color.a );
}

void UVulkanRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{
	guard(UVulkanRenderDevice::Draw2DLine);
	if( !IsLocked )
		return;
	if( Frame != CurrentFrame )
		UpdateSceneNode( Frame );

	SetPipeline( RenderPasses->GetLinePipeline(false) );
	ivec4 textureBinds = GetTextureIndexes(PF_Highlighted, nullptr);
	vec4 color = ApplyInverseGamma( vec4(Color.X, Color.Y, Color.Z, 1.0f) );

	auto alloc = ReserveVertices(2, 2);
	if( alloc.vptr )
	{
		SceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		vptr[0] = { 0, vec3(RFX2 * P1.Z * (P1.X - Frame->FX2), RFY2 * P1.Z * (P1.Y - Frame->FY2), P1.Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };
		vptr[1] = { 0, vec3(RFX2 * P2.Z * (P2.X - Frame->FX2), RFY2 * P2.Z * (P2.Y - Frame->FY2), P2.Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;

		UseVertices(2, 2);
	}

	unguard;
}

void UVulkanRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )
{
	guard(UVulkanRenderDevice::Draw2DPoint);
	if( !IsLocked )
		return;
	if( Frame != CurrentFrame )
		UpdateSceneNode( Frame );

	const FLOAT Z = 1.0f;   // 219 Draw2DPoint has no Z param; points are 2D overlay
	SetPipeline( RenderPasses->GetPointPipeline(false) );
	ivec4 textureBinds = GetTextureIndexes(PF_Highlighted, nullptr);
	vec4 color = ApplyInverseGamma( vec4(Color.X, Color.Y, Color.Z, 1.0f) );

	auto alloc = ReserveVertices(4, 6);
	if( alloc.vptr )
	{
		SceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		vptr[0] = { 0, vec3(RFX2 * Z * (X1 - Frame->FX2 - 0.5f), RFY2 * Z * (Y1 - Frame->FY2 - 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };
		vptr[1] = { 0, vec3(RFX2 * Z * (X2 - Frame->FX2 + 0.5f), RFY2 * Z * (Y1 - Frame->FY2 - 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };
		vptr[2] = { 0, vec3(RFX2 * Z * (X2 - Frame->FX2 + 0.5f), RFY2 * Z * (Y2 - Frame->FY2 + 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };
		vptr[3] = { 0, vec3(RFX2 * Z * (X1 - Frame->FX2 - 0.5f), RFY2 * Z * (Y2 - Frame->FY2 + 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color, textureBinds };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;
		iptr[2] = vpos + 2;
		iptr[3] = vpos;
		iptr[4] = vpos + 2;
		iptr[5] = vpos + 3;

		UseVertices(4, 6);
	}

	unguard;
}

void UVulkanRenderDevice::ClearZ( FSceneNode* Frame )
{
	guard(UVulkanRenderDevice::ClearZ);
	if( !IsLocked )
		return;

	DrawBatch( Commands->GetDrawCommands() );

	VkClearAttachment attachment = {};
	VkClearRect rect = {};
	attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	attachment.clearValue.depthStencil.depth = 1.0f;   // forward-Z: far = 1
	rect.layerCount = 1;
	rect.rect.extent.width = Textures->Scene->Width;
	rect.rect.extent.height = Textures->Scene->Height;
	Commands->GetDrawCommands()->clearAttachments(1, &attachment, 1, &rect);
	unguard;
}

void UVulkanRenderDevice::PushHit( const BYTE* Data, INT Count )
{
	guard(UVulkanRenderDevice::PushHit);
	if( Count <= 0 ) return;
	HitQueryStack.insert(HitQueryStack.end(), Data, Data + Count);
	SetHitLocation();
	unguard;
}

void UVulkanRenderDevice::PopHit( INT Count, UBOOL bForce )
{
	guard(UVulkanRenderDevice::PopHit);
	if( bForce )
		ForceHitIndex = HitQueries.size() - 1;
	HitQueryStack.resize(HitQueryStack.size() - Count);
	SetHitLocation();
	unguard;
}

void UVulkanRenderDevice::SetHitLocation()
{
	DrawBatch( Commands->GetDrawCommands() );

	if( !HitQueryStack.empty() )
	{
		INT index = HitQueries.size();
		HitQuery query;
		query.Start = HitBuffer.size();
		query.Count = HitQueryStack.size();
		HitQueries.push_back(query);
		HitBuffer.insert(HitBuffer.end(), HitQueryStack.begin(), HitQueryStack.end());
		pushconstants.hitIndex = index + 1;
	}
	else
	{
		pushconstants.hitIndex = 0;
	}
}

void UVulkanRenderDevice::GetStats( char* Result )
{
	guard(UVulkanRenderDevice::GetStats);
	if( Result )
		Result[0] = 0;
	unguard;
}

void UVulkanRenderDevice::EndFlash()
{
	guard(UVulkanRenderDevice::EndFlash);
	if( !IsLocked )
		return;
	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
	{
		vec4 color( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.0f - Min(FlashScale.X * 2.0f, 1.0f) );
		vec2 zero2(0.0f);
		ivec4 zero4(0);

		DrawBatch( Commands->GetDrawCommands() );
		pushconstants.objectToProjection = mat4::identity();
		pushconstants.nearClip = vec4(0.0f, 0.0f, 0.0f, 1.0f);

		SetPipeline( RenderPasses->GetEndFlashPipeline() );

		auto alloc = ReserveVertices(4, 6);
		if( alloc.vptr )
		{
			SceneVertex* vptr = alloc.vptr;
			uint32_t* iptr = alloc.iptr;
			uint32_t vpos = alloc.vpos;

			vptr[0] = { 0, vec3(-1.0f, -1.0f, 0.0f), zero2, zero2, zero2, zero2, color, zero4 };
			vptr[1] = { 0, vec3(1.0f, -1.0f, 0.0f),  zero2, zero2, zero2, zero2, color, zero4 };
			vptr[2] = { 0, vec3(1.0f,  1.0f, 0.0f),  zero2, zero2, zero2, zero2, color, zero4 };
			vptr[3] = { 0, vec3(-1.0f,  1.0f, 0.0f), zero2, zero2, zero2, zero2, color, zero4 };

			iptr[0] = vpos;
			iptr[1] = vpos + 1;
			iptr[2] = vpos + 2;
			iptr[3] = vpos;
			iptr[4] = vpos + 2;
			iptr[5] = vpos + 3;

			UseVertices(4, 6);
		}

		DrawBatch( Commands->GetDrawCommands() );
		if( CurrentFrame )
			UpdateSceneNode( CurrentFrame );
	}
	unguard;
}

void UVulkanRenderDevice::ClearTextureCache()
{
	DescriptorSets->ClearCache();
	Textures->ClearCache();
	Uploads->ClearCache();
}

void UVulkanRenderDevice::BlitSceneToPostprocess()
{
	auto buffers = Textures->Scene.get();
	auto cmdbuffer = Commands->GetDrawCommands();

	// [KHG perf] Fast path: when there is no MSAA resolve to do (SceneSamples==1), no bloom, and no
	// hit-test readback this frame, the present pass can sample the scene ColorBuffer directly. Skip the
	// full-frame ColorBuffer->PPImage[0] blit entirely — that blit is a wasted read+write of the whole
	// framebuffer every frame, especially costly on tile-based (Mali) GPUs. Pixels are identical:
	// PPImage[0] was only ever a 1:1 VK_FILTER_NEAREST copy of ColorBuffer (same R16G16B16A16_SFLOAT
	// format). Bloom is not part of retail KHG and is off by default, so this is the normal path.
	// Correct-by-construction: the scene render pass leaves ColorBuffer in COLOR_ATTACHMENT_OPTIMAL; we
	// move it to SHADER_READ_ONLY for the present sample; the next frame's Lock barrier uses
	// oldLayout=UNDEFINED, so leaving ColorBuffer in SHADER_READ_ONLY here is safe.
	PresentFromColorBuffer = ( buffers->SceneSamples == VK_SAMPLE_COUNT_1_BIT && !Bloom && !HitData );
	if( PresentFromColorBuffer )
	{
		PipelineBarrier()
			.AddImage(
				buffers->ColorBuffer.get(),
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		return;
	}

	PipelineBarrier barrer0;
	VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrer0.AddImage(
		buffers->ColorBuffer.get(),
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT);
	if( HitData )
	{
		barrer0.AddImage(
			buffers->HitBuffer.get(),
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
		barrer0.AddImage(
			buffers->PPHitBuffer.get(),
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT);
		srcStageMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
	}
	barrer0.AddImage(
		buffers->PPImage[0].get(),
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT);
	barrer0.Execute(
		Commands->GetDrawCommands(),
		srcStageMask,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	if( buffers->SceneSamples != VK_SAMPLE_COUNT_1_BIT )
	{
		VkImageResolve resolve = {};
		resolve.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.srcSubresource.layerCount = 1;
		resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.dstSubresource.layerCount = 1;
		resolve.extent = { (uint32_t)buffers->ColorBuffer->width, (uint32_t)buffers->ColorBuffer->height, 1 };
		cmdbuffer->resolveImage(
			buffers->ColorBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PPImage[0]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &resolve);
		if( HitData )
		{
			cmdbuffer->resolveImage(
				buffers->HitBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				buffers->PPHitBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &resolve);
		}
	}
	else
	{
		auto colorBuffer = buffers->ColorBuffer.get();
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { colorBuffer->width, colorBuffer->height, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { colorBuffer->width, colorBuffer->height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		cmdbuffer->blitImage(
			colorBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PPImage[0]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_NEAREST);
		if( HitData )
		{
			VkImageCopy copy = {};
			copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.srcSubresource.layerCount = 1;
			copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.dstSubresource.layerCount = 1;
			copy.extent = { (uint32_t)colorBuffer->width, (uint32_t)colorBuffer->height, (uint32_t)1 };
			cmdbuffer->copyImage(
				buffers->HitBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				buffers->PPHitBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &copy);
		}
	}

	PipelineBarrier barrier1;
	barrier1.AddImage(
		buffers->PPImage[0].get(),
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT);
	if( HitData )
	{
		barrier1.AddImage(
			buffers->PPHitBuffer.get(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
	}
	barrier1.Execute(
		Commands->GetDrawCommands(),
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		HitData ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	if( HitData )
	{
		VkBufferImageCopy copy = {};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset = { (int32_t)Viewport->HitX, (int32_t)Viewport->HitY, (int32_t)0 };
		copy.imageExtent = { (uint32_t)Viewport->HitXL, (uint32_t)Viewport->HitYL, (uint32_t)1 };
		cmdbuffer->copyImageToBuffer(buffers->PPHitBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers->StagingHitBuffer->buffer, 1, &copy);

		PipelineBarrier()
			.AddBuffer(buffers->StagingHitBuffer.get(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
	}
}

void UVulkanRenderDevice::ReadPixels( FColor* Pixels )
{
	guard(UVulkanRenderDevice::ReadPixels);

	auto cmdbuffer = Commands->GetDrawCommands();
	DrawBatch( cmdbuffer );

	// [KHG perf] The present fast path (BlitSceneToPostprocess) leaves PPImage[0] unpopulated, so copy the
	// scene ColorBuffer into PPImage[0] here for the readback. Rare path (SHOT command only). ColorBuffer
	// is in SHADER_READ_ONLY from the last fast-path present; leave it in TRANSFER_SRC (next Lock uses UNDEFINED).
	if( PresentFromColorBuffer )
	{
		auto buffers = Textures->Scene.get();
		PipelineBarrier()
			.AddImage(buffers->ColorBuffer.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.AddImage(buffers->PPImage[0].get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkImageBlit blit = {};
		blit.srcOffsets[1] = { buffers->ColorBuffer->width, buffers->ColorBuffer->height, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[1] = { buffers->ColorBuffer->width, buffers->ColorBuffer->height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		cmdbuffer->blitImage(buffers->ColorBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers->PPImage[0]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

		PipelineBarrier()
			.AddImage(buffers->PPImage[0].get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	if( GammaCorrectScreenshots )
	{
		PresentPushConstants pushconstants = GetPresentPushConstants();

		bool ActiveHdr = false;
		int presentShader = 0;
		if( ActiveHdr ) presentShader |= 1;
		if( GammaMode == 1 ) presentShader |= 2;
		if( pushconstants.Brightness != 0.0f || pushconstants.Contrast != 1.0f || pushconstants.Saturation != 1.0f ) presentShader |= (Clamp(GrayFormula, 0, 2) + 1) << 2;

		VkViewport viewport = {};
		viewport.width = Textures->Scene->Width;
		viewport.height = Textures->Scene->Height;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor = {};
		scissor.extent.width = Textures->Scene->Width;
		scissor.extent.height = Textures->Scene->Height;

		VkAccessFlags srcColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkAccessFlags dstColorAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		PipelineBarrier()
			.AddImage(Textures->Scene->PPImage[1].get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcColorAccess, dstColorAccess)
			.Execute(cmdbuffer, srcStages, dstStages);

		RenderPassBegin()
			.RenderPass(RenderPasses->Postprocess.RenderPass.get())
			.Framebuffer(Framebuffers->PPImageFB[1].get())
			.RenderArea(0, 0, Textures->Scene->Width, Textures->Scene->Height)
			.AddClearColor(0.0f, 0.0f, 0.0f, 1.0f)
			.Execute(cmdbuffer);

		cmdbuffer->setViewport(0, 1, &viewport);
		cmdbuffer->setScissor(0, 1, &scissor);
		cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, RenderPasses->Present.ScreenshotPipeline[presentShader].get());
		cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, RenderPasses->Present.PipelineLayout.get(), 0, DescriptorSets->GetPresentSet());
		cmdbuffer->pushConstants(RenderPasses->Present.PipelineLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPushConstants), &pushconstants);
		cmdbuffer->draw(6, 1, 0, 0);

		cmdbuffer->endRenderPass();

		PipelineBarrier()
			.AddImage(Textures->Scene->PPImage[1].get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	auto srcimage = Textures->Scene->PPImage[GammaCorrectScreenshots ? 1 : 0].get();

	int w = Viewport->SizeX;
	int h = Viewport->SizeY;
	void* data = Pixels;

	auto dstimage = ImageBuilder()
		.Format(VK_FORMAT_B8G8R8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("ReadPixelsDstImage")
		.Create(Device.get());

	PipelineBarrier()
		.AddImage(srcimage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.AddImage(dstimage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageBlit blit = {};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { srcimage->width, srcimage->height, 1 };
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = 1;
	blit.dstOffsets[0] = { 0, 0, 0 };
	blit.dstOffsets[1] = { dstimage->width, dstimage->height, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = 0;
	blit.dstSubresource.layerCount = 1;
	cmdbuffer->blitImage(
		srcimage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstimage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);

	PipelineBarrier()
		.AddImage(srcimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
		.AddImage(dstimage.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("ReadPixelsStaging")
		.Create(Device.get());

	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cmdbuffer->copyImageToBuffer(dstimage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1, &region);

	SubmitAndWait( false, 0, 0, false );
	// [KHG] SubmitAndWait doesn't block (it submits + rotates the frame), so idle the GPU before mapping the staging buffer and before the local dstimage/staging are destroyed at return (else garbage / use-after-free).
	vkDeviceWaitIdle( Device->device );

	uint8_t* pixels = (uint8_t*)staging->Map(0, w * h * 4);
	memcpy(data, pixels, w * h * 4);
	staging->Unmap();

	unguard;
}

/*-----------------------------------------------------------------------------
	Bloom (compiled but disabled by config — Bloom=0; KHG has no bloom).
-----------------------------------------------------------------------------*/

void UVulkanRenderDevice::RunBloomPass()
{
	float blurAmount = 0.6f + BloomAmount * (1.9f / 255.0f);
	BloomPushConstants pushconstants;
	ComputeBlurSamples(7, blurAmount, pushconstants.SampleWeights);

	auto cmdbuffer = Commands->GetDrawCommands();

	PipelineBarrier()
		.AddImage(Textures->Scene->BloomBlurLevels[0].VTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	BloomStep(cmdbuffer, RenderPasses->Bloom.Extract.get(), DescriptorSets->GetBloomPPImageSet(),
		Framebuffers->BloomBlurLevels[0].VTextureFB.get(),
		Textures->Scene->BloomBlurLevels[0].Width, Textures->Scene->BloomBlurLevels[0].Height, pushconstants);

	for( int i = 0; i < NumBloomLevels - 1; i++ )
	{
		PipelineBarrier()
			.AddImage(Textures->Scene->BloomBlurLevels[i].HTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
			.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		BloomStep(cmdbuffer, RenderPasses->Bloom.BlurVertical.get(), DescriptorSets->GetBloomVTextureSet(i),
			Framebuffers->BloomBlurLevels[i].HTextureFB.get(),
			Textures->Scene->BloomBlurLevels[i].Width, Textures->Scene->BloomBlurLevels[i].Height, pushconstants);

		PipelineBarrier()
			.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
			.AddImage(Textures->Scene->BloomBlurLevels[i].HTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		BloomStep(cmdbuffer, RenderPasses->Bloom.BlurHorizontal.get(), DescriptorSets->GetBloomHTextureSet(i),
			Framebuffers->BloomBlurLevels[i].VTextureFB.get(),
			Textures->Scene->BloomBlurLevels[i].Width, Textures->Scene->BloomBlurLevels[i].Height, pushconstants);

		PipelineBarrier()
			.AddImage(Textures->Scene->BloomBlurLevels[i + 1].VTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
			.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		BloomStep(cmdbuffer, RenderPasses->Bloom.Scale.get(), DescriptorSets->GetBloomVTextureSet(i),
			Framebuffers->BloomBlurLevels[i + 1].VTextureFB.get(),
			Textures->Scene->BloomBlurLevels[i + 1].Width, Textures->Scene->BloomBlurLevels[i + 1].Height, pushconstants);
	}

	for( int i = NumBloomLevels - 1; i >= 0; i-- )
	{
		PipelineBarrier()
			.AddImage(Textures->Scene->BloomBlurLevels[i].HTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
			.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		BloomStep(cmdbuffer, RenderPasses->Bloom.BlurVertical.get(), DescriptorSets->GetBloomVTextureSet(i),
			Framebuffers->BloomBlurLevels[i].HTextureFB.get(),
			Textures->Scene->BloomBlurLevels[i].Width, Textures->Scene->BloomBlurLevels[i].Height, pushconstants);

		PipelineBarrier()
			.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
			.AddImage(Textures->Scene->BloomBlurLevels[i].HTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		BloomStep(cmdbuffer, RenderPasses->Bloom.BlurHorizontal.get(), DescriptorSets->GetBloomHTextureSet(i),
			Framebuffers->BloomBlurLevels[i].VTextureFB.get(),
			Textures->Scene->BloomBlurLevels[i].Width, Textures->Scene->BloomBlurLevels[i].Height, pushconstants);

		if( i > 0 )
		{
			PipelineBarrier()
				.AddImage(Textures->Scene->BloomBlurLevels[i - 1].VTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
				.AddImage(Textures->Scene->BloomBlurLevels[i].VTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
				.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

			BloomStep(cmdbuffer, RenderPasses->Bloom.Scale.get(), DescriptorSets->GetBloomVTextureSet(i),
				Framebuffers->BloomBlurLevels[i - 1].VTextureFB.get(),
				Textures->Scene->BloomBlurLevels[i - 1].Width, Textures->Scene->BloomBlurLevels[i - 1].Height, pushconstants);
		}
	}

	PipelineBarrier()
		.AddImage(Textures->Scene->PPImage[0].get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.AddImage(Textures->Scene->BloomBlurLevels[0].VTexture.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	BloomStep(cmdbuffer, RenderPasses->Bloom.Combine.get(), DescriptorSets->GetBloomVTextureSet(0),
		Framebuffers->PPImageFB[0].get(),
		Textures->Scene->Width, Textures->Scene->Height, pushconstants);

	PipelineBarrier()
		.AddImage(Textures->Scene->PPImage[0].get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void UVulkanRenderDevice::BloomStep( VulkanCommandBuffer* cmdbuffer, VulkanPipeline* pipeline, VulkanDescriptorSet* input, VulkanFramebuffer* output, int width, int height, const BloomPushConstants& pushconstants )
{
	RenderPassBegin()
		.RenderPass(pipeline != RenderPasses->Bloom.Combine.get() ? RenderPasses->Postprocess.RenderPass.get() : RenderPasses->Postprocess.RenderPassCombine.get())
		.Framebuffer(output)
		.RenderArea(0, 0, width, height)
		.AddClearColor(0.0f, 0.0f, 0.0f, 1.0f)
		.Execute(cmdbuffer);

	VkViewport viewport = {};
	viewport.width = (float)width;
	viewport.height = (float)height;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor = {};
	scissor.extent.width = width;
	scissor.extent.height = height;

	cmdbuffer->setViewport(0, 1, &viewport);
	cmdbuffer->setScissor(0, 1, &scissor);
	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, RenderPasses->Bloom.PipelineLayout.get(), 0, input);
	cmdbuffer->pushConstants(RenderPasses->Bloom.PipelineLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPushConstants), &pushconstants);
	cmdbuffer->draw(6, 1, 0, 0);

	cmdbuffer->endRenderPass();
}

float UVulkanRenderDevice::ComputeBlurGaussian( float n, float theta )
{
	return (float)((1.0f / sqrt(2 * 3.14159265359f * theta)) * expf(-(n * n) / (2.0f * theta * theta)));
}

void UVulkanRenderDevice::ComputeBlurSamples( int sampleCount, float blurAmount, float* sampleWeights )
{
	sampleWeights[0] = ComputeBlurGaussian(0, blurAmount);
	float totalWeights = sampleWeights[0];
	for( int i = 0; i < sampleCount / 2; i++ )
	{
		float weight = ComputeBlurGaussian(i + 1.0f, blurAmount);
		sampleWeights[i * 2 + 1] = weight;
		sampleWeights[i * 2 + 2] = weight;
		totalWeights += weight * 2;
	}
	for( int i = 0; i < sampleCount; i++ )
		sampleWeights[i] /= totalWeights;
}

/*-----------------------------------------------------------------------------
	Present.
-----------------------------------------------------------------------------*/

PresentPushConstants UVulkanRenderDevice::GetPresentPushConstants()
{
	PresentPushConstants pushconstants;
	pushconstants.HdrScale = 0.8f + HdrScale * (3.0f / 255.0f);
	if( Viewport->IsOrtho() )
	{
		pushconstants.GammaCorrection = { 1.0f };
		pushconstants.Contrast = 1.0f;
		pushconstants.Saturation = 1.0f;
		pushconstants.Brightness = 0.0f;
	}
	else
	{
		// [KHG] 3dfx Glide gamma ramp: present gamma = 0.5+1.5*Brightness; GlideGamma=0 restores the flat Brightness*2.0.
		float brightness = Clamp( (float)( GlideGamma ? (0.5 + 1.5 * Viewport->Client->Brightness) : (Viewport->Client->Brightness * 2.0) ), 0.05f, 2.99f );

		if( GammaMode == 0 )
		{
			float invGammaRed = 1.0f / Max(brightness + GammaOffset + GammaOffsetRed, 0.001f);
			float invGammaGreen = 1.0f / Max(brightness + GammaOffset + GammaOffsetGreen, 0.001f);
			float invGammaBlue = 1.0f / Max(brightness + GammaOffset + GammaOffsetBlue, 0.001f);
			pushconstants.GammaCorrection = vec4(invGammaRed, invGammaGreen, invGammaBlue, 0.0f);
		}
		else
		{
			float invGammaRed = (GammaOffset + GammaOffsetRed + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetRed + 1.0f) : 1.0f;
			float invGammaGreen = (GammaOffset + GammaOffsetGreen + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetGreen + 1.0f) : 1.0f;
			float invGammaBlue = (GammaOffset + GammaOffsetBlue + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetBlue + 1.0f) : 1.0f;
			pushconstants.GammaCorrection = vec4(invGammaRed, invGammaGreen, invGammaBlue, brightness);
		}

		if( Contrast >= 128 )
			pushconstants.Contrast = 1.0f + (Contrast - 128) / 127.0f * 3.0f;
		else
			pushconstants.Contrast = Max(Contrast / 128.0f, 0.1f);

		pushconstants.Saturation = 1.0f - 2.0f * (255 - Saturation) / 255.0f;

		if( LinearBrightness >= 128 )
			pushconstants.Brightness = (LinearBrightness - 128) / 127.0f * 1.8f;
		else
			pushconstants.Brightness = (128 - LinearBrightness) / 128.0f * -1.8f;
	}
	return pushconstants;
}

void UVulkanRenderDevice::DrawPresentTexture( int width, int height )
{
	PresentPushConstants pushconstants = GetPresentPushConstants();

	bool ActiveHdr = (Commands->SwapChain->Format().colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) ? 1 : 0;

	int presentShader = 0;
	if( ActiveHdr ) presentShader |= 1;
	if( GammaMode == 1 ) presentShader |= 2;
	if( pushconstants.Brightness != 0.0f || pushconstants.Contrast != 1.0f || pushconstants.Saturation != 1.0f ) presentShader |= (Clamp(GrayFormula, 0, 2) + 1) << 2;

	float scale = std::min( width / (float)Viewport->SizeX, height / (float)Viewport->SizeY );
	int letterboxWidth = (int)std::round(Viewport->SizeX * scale);
	int letterboxHeight = (int)std::round(Viewport->SizeY * scale);
	int letterboxX = (width - letterboxWidth) / 2;
	int letterboxY = (height - letterboxHeight) / 2;

	VkViewport viewport = {};
	viewport.x = letterboxX;
	viewport.y = letterboxY;
	viewport.width = letterboxWidth;
	viewport.height = letterboxHeight;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor = {};
	scissor.offset.x = letterboxX;
	scissor.offset.y = letterboxY;
	scissor.extent.width = letterboxWidth;
	scissor.extent.height = letterboxHeight;

	auto cmdbuffer = Commands->GetDrawCommands();


	PipelineBarrier()
		.AddImage(Commands->SwapChain->GetImage(Commands->PresentImageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	RenderPassBegin()
		.RenderPass(RenderPasses->Present.RenderPass.get())
		.Framebuffer(Framebuffers->GetSwapChainFramebuffer())
		.RenderArea(0, 0, Commands->SwapChain->Width(), Commands->SwapChain->Height())
		.AddClearColor(0.0f, 0.0f, 0.0f, 1.0f)
		.Execute(cmdbuffer);
	cmdbuffer->setViewport(0, 1, &viewport);
	cmdbuffer->setScissor(0, 1, &scissor);
	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, RenderPasses->Present.Pipeline[presentShader].get());
	// [KHG perf] Sample ColorBuffer directly when the blit was skipped (see BlitSceneToPostprocess), else PPImage[0].
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, RenderPasses->Present.PipelineLayout.get(), 0, PresentFromColorBuffer ? DescriptorSets->GetPresentSetColorBuffer() : DescriptorSets->GetPresentSet());
	cmdbuffer->pushConstants(RenderPasses->Present.PipelineLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPushConstants), &pushconstants);
	cmdbuffer->draw(6, 1, 0, 0);
	cmdbuffer->endRenderPass();

	PipelineBarrier()
		.AddImage(Commands->SwapChain->GetImage(Commands->PresentImageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}
