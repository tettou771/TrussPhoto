// =============================================================================
// GuidedFilterMPS.mm - GPU-accelerated guided filter via Metal Performance Shaders
// =============================================================================
// Uses MPSImageGuidedFilter for edge-preserving noise reduction.
// Operates in YCbCr space: Y broadcast to RGB as guidance for chroma channels.
//
// Cache strategy: GPU filters run ONCE per image with fixed parameters.
// Slider changes only re-run the cheap CPU blend (original ↔ filtered).
// =============================================================================

#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

using namespace std;

// Forward declare sokol Metal device accessor
extern "C" const void* sg_mtl_device(void);

namespace tp {

namespace {

struct MPSState {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    // GPU textures (recreated when image size changes)
    id<MTLTexture> chY = nil;     // R32Float: luma source
    id<MTLTexture> chCb = nil;    // R32Float: chroma blue source
    id<MTLTexture> chCr = nil;    // R32Float: chroma red source
    id<MTLTexture> outY = nil;    // R32Float: filtered luma
    id<MTLTexture> outCb = nil;   // R32Float: filtered chroma blue
    id<MTLTexture> outCr = nil;   // R32Float: filtered chroma red
    id<MTLTexture> guide = nil;   // RGBA32Float: Y broadcast to (Y,Y,Y,1)
    id<MTLTexture> coeff = nil;   // RGBA32Float: regression coefficients
    int texW = 0, texH = 0;

    // Original channels (from YCbCr split)
    vector<float> Y, Cb, Cr;
    vector<float> guideRow;       // single row for guidance upload

    // Cached filtered results (GPU runs once, blend uses these)
    vector<float> filtY, filtCb, filtCr;
    bool cacheValid = false;
    float fpR = 0, fpG = 0, fpB = 0;  // fingerprint: first pixel RGB
};

static MPSState s;

id<MTLTexture> makeR32Float(int w, int h) {
    auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                   width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;
    return [s.device newTextureWithDescriptor:desc];
}

id<MTLTexture> makeRGBA32Float(int w, int h) {
    auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                   width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;
    return [s.device newTextureWithDescriptor:desc];
}

bool ensureState(int w, int h) {
    if (!s.device) {
        const void* ptr = sg_mtl_device();
        if (!ptr) return false;
        s.device = (__bridge id<MTLDevice>)ptr;
        s.queue = [s.device newCommandQueue];
    }
    if (s.texW != w || s.texH != h) {
        @autoreleasepool {
            s.chY = makeR32Float(w, h);
            s.chCb = makeR32Float(w, h);
            s.chCr = makeR32Float(w, h);
            s.outY = makeR32Float(w, h);
            s.outCb = makeR32Float(w, h);
            s.outCr = makeR32Float(w, h);
            s.guide = makeRGBA32Float(w, h);
            s.coeff = makeRGBA32Float(w, h);
            s.texW = w;
            s.texH = h;
            s.cacheValid = false;
        }
    }
    return s.chY != nil;
}

} // anonymous namespace

bool guidedDenoiseMPS(float* rgbaData, int w, int h,
                      float chromaStrength, float lumaStrength, int radius) {
    if (!ensureState(w, h)) return false;

    auto t0 = chrono::steady_clock::now();
    int N = w * h;

    // Radii
    int longEdge = max(w, h);
    int lumaRadius = (radius > 0) ? radius : max(1, (int)round(5.0 * longEdge / 7000.0));
    int chromaRadius = (int)round(lumaRadius * 1.6);  // wider for demosaic-correlated chroma noise
    int chromaKd = chromaRadius * 2 + 1;
    int lumaKd = lumaRadius * 2 + 1;

    // Resize buffers (no-op after first call with same size)
    s.Y.resize(N);  s.Cb.resize(N);  s.Cr.resize(N);
    s.filtY.resize(N);  s.filtCb.resize(N);  s.filtCr.resize(N);
    s.guideRow.resize(w * 4);

    // Check cache: compare first pixel as fingerprint
    bool cacheHit = s.cacheValid
        && s.fpR == rgbaData[0] && s.fpG == rgbaData[1] && s.fpB == rgbaData[2];

    if (cacheHit) {
        // Cache hit — skip split, upload, GPU. Just do the YCbCr split for originals.
        // (needed for blend, but cheaper than full pipeline)
        for (int i = 0; i < N; i++) {
            float r = rgbaData[i * 4 + 0];
            float g = rgbaData[i * 4 + 1];
            float b = rgbaData[i * 4 + 2];
            s.Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b;
            s.Cb[i] = -0.169f * r - 0.331f * g + 0.500f * b;
            s.Cr[i] =  0.500f * r - 0.419f * g - 0.081f * b;
        }
    } else {
        // Cache miss — full pipeline: split + upload + GPU + readback
        NSUInteger r32Row = w * sizeof(float);
        NSUInteger rgbaRow = w * 4 * sizeof(float);

        // CPU: RGBA → YCbCr + upload guidance row by row
        for (int row = 0; row < h; row++) {
            int base = row * w;
            for (int x = 0; x < w; x++) {
                int i = base + x;
                float r = rgbaData[i * 4 + 0];
                float g = rgbaData[i * 4 + 1];
                float b = rgbaData[i * 4 + 2];
                float y = 0.299f * r + 0.587f * g + 0.114f * b;
                s.Y[i]  = y;
                s.Cb[i] = -0.169f * r - 0.331f * g + 0.500f * b;
                s.Cr[i] =  0.500f * r - 0.419f * g - 0.081f * b;
                s.guideRow[x * 4 + 0] = y;
                s.guideRow[x * 4 + 1] = y;
                s.guideRow[x * 4 + 2] = y;
                s.guideRow[x * 4 + 3] = 1.0f;
            }
            MTLRegion rowRegion = MTLRegionMake2D(0, row, w, 1);
            [s.guide replaceRegion:rowRegion mipmapLevel:0
                         withBytes:s.guideRow.data() bytesPerRow:rgbaRow];
        }

        // Upload source textures
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        [s.chY replaceRegion:region mipmapLevel:0 withBytes:s.Y.data() bytesPerRow:r32Row];
        [s.chCb replaceRegion:region mipmapLevel:0 withBytes:s.Cb.data() bytesPerRow:r32Row];
        [s.chCr replaceRegion:region mipmapLevel:0 withBytes:s.Cr.data() bytesPerRow:r32Row];

        // GPU: all three channels with fixed strong eps
        @autoreleasepool {
            id<MTLCommandBuffer> cmdBuf = [s.queue commandBuffer];

            // Chroma: Y-guided, fixed eps (slider controls blend, not filter strength)
            float chromaEps = 0.3f * 3.0f;
            MPSImageGuidedFilter* chromaGf = [[MPSImageGuidedFilter alloc]
                initWithDevice:s.device kernelDiameter:chromaKd];
            chromaGf.epsilon = chromaEps;

            [chromaGf encodeRegressionToCommandBuffer:cmdBuf
                                        sourceTexture:s.chCb
                                      guidanceTexture:s.guide
                                       weightsTexture:nil
                           destinationCoefficientsTexture:s.coeff];
            [chromaGf encodeReconstructionToCommandBuffer:cmdBuf
                                         guidanceTexture:s.guide
                                      coefficientsTexture:s.coeff
                                       destinationTexture:s.outCb];

            [chromaGf encodeRegressionToCommandBuffer:cmdBuf
                                        sourceTexture:s.chCr
                                      guidanceTexture:s.guide
                                       weightsTexture:nil
                           destinationCoefficientsTexture:s.coeff];
            [chromaGf encodeReconstructionToCommandBuffer:cmdBuf
                                         guidanceTexture:s.guide
                                      coefficientsTexture:s.coeff
                                       destinationTexture:s.outCr];

            // Luma: self-guided (Y guides Y)
            float lumaEps = 0.03f * 3.0f;
            MPSImageGuidedFilter* lumaGf = [[MPSImageGuidedFilter alloc]
                initWithDevice:s.device kernelDiameter:lumaKd];
            lumaGf.epsilon = lumaEps;

            [lumaGf encodeRegressionToCommandBuffer:cmdBuf
                                       sourceTexture:s.chY
                                     guidanceTexture:s.guide
                                      weightsTexture:nil
                          destinationCoefficientsTexture:s.coeff];
            [lumaGf encodeReconstructionToCommandBuffer:cmdBuf
                                        guidanceTexture:s.guide
                                     coefficientsTexture:s.coeff
                                      destinationTexture:s.outY];

            [cmdBuf commit];
            [cmdBuf waitUntilCompleted];

            if (cmdBuf.status == MTLCommandBufferStatusError) {
                fprintf(stderr, "[MPS] Command buffer error: %s\n",
                        cmdBuf.error.localizedDescription.UTF8String);
                return false;
            }
        }

        // Read back filtered channels into cache
        [s.outCb getBytes:s.filtCb.data() bytesPerRow:r32Row fromRegion:region mipmapLevel:0];
        [s.outCr getBytes:s.filtCr.data() bytesPerRow:r32Row fromRegion:region mipmapLevel:0];
        [s.outY  getBytes:s.filtY.data()  bytesPerRow:r32Row fromRegion:region mipmapLevel:0];

        // Update cache fingerprint
        s.fpR = rgbaData[0];
        s.fpG = rgbaData[1];
        s.fpB = rgbaData[2];
        s.cacheValid = true;
    }

    auto t1 = chrono::steady_clock::now();

    // CPU: blend original ↔ filtered, then YCbCr → RGBA
    float ct = chromaStrength;
    float lt = lumaStrength;
    for (int i = 0; i < N; i++) {
        float y  = s.Y[i]  + lt * (s.filtY[i]  - s.Y[i]);
        float cb = s.Cb[i] + ct * (s.filtCb[i] - s.Cb[i]);
        float cr = s.Cr[i] + ct * (s.filtCr[i] - s.Cr[i]);
        rgbaData[i * 4 + 0] = y + 1.402f * cr;
        rgbaData[i * 4 + 1] = y - 0.344f * cb - 0.714f * cr;
        rgbaData[i * 4 + 2] = y + 1.772f * cb;
        // alpha unchanged
    }

    auto t2 = chrono::steady_clock::now();

    auto ms = [](auto a, auto b) {
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };
    fprintf(stderr, "[MPS GuidedFilter] %dx%d chroma_kd=%d luma_kd=%d %s | %lldms + blend=%lldms\n",
            w, h, chromaKd, lumaKd,
            cacheHit ? "CACHED" : "GPU",
            ms(t0, t1), ms(t1, t2));

    return true;
}

} // namespace tp
