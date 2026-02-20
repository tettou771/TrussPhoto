// =============================================================================
// PhotoExporter.mm - Metal readback for JPEG export
// =============================================================================
// Do NOT include PhotoExporter.h here — it has `using namespace tc;` which
// causes Rect ambiguity with macOS CarbonCore Rect via Metal.h.
// Follow the same pattern as tcFbo_mac.mm.
// =============================================================================

#include <TrussC.h>

#ifdef __APPLE__
#import <Metal/Metal.h>

using namespace trussc;

namespace PhotoExporter {

bool readFboPixels(sg_image fboImg, int w, int h, Pixels& outPixels) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)sg_mtl_device();
    if (!device) {
        logError() << "[Export] Failed to get Metal device";
        return false;
    }

    sg_mtl_image_info info = sg_mtl_query_image_info(fboImg);
    id<MTLTexture> srcTex = (__bridge id<MTLTexture>)info.tex[info.active_slot];
    if (!srcTex) {
        logError() << "[Export] Failed to get source MTLTexture";
        return false;
    }

    // FBO is RGBA32F (16 bytes/pixel). Copy to Shared texture for CPU readback.
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
        width:w height:h mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;

    id<MTLTexture> dstTex = [device newTextureWithDescriptor:desc];
    if (!dstTex) {
        logError() << "[Export] Failed to create destination texture";
        return false;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];

    [blit copyFromTexture:srcTex
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(w, h, 1)
                 toTexture:dstTex
          destinationSlice:0
          destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];

    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    // Read RGBA32F and convert to U8
    NSUInteger bytesPerRow = w * 16; // 4 floats * 4 bytes
    std::vector<float> raw(w * h * 4);
    [dstTex getBytes:raw.data()
         bytesPerRow:bytesPerRow
          fromRegion:MTLRegionMake2D(0, 0, w, h)
         mipmapLevel:0];

    outPixels.allocate(w, h, 4);
    unsigned char* dst = outPixels.getData();
    for (int i = 0; i < w * h * 4; i++) {
        float v = raw[i];
        // Clamp 0-1 then quantize to 8-bit
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        dst[i] = (unsigned char)(v * 255.f + 0.5f);
    }

    return true;
}

} // namespace PhotoExporter

#endif // __APPLE__
