@module develop
// =============================================================================
// develop.glsl - Unified develop shader (lens correction + crop + LUT)
// =============================================================================
// Single-pass GPU shader that replaces CPU lens correction with GPU remap.
// Handles: distortion, chromatic aberration, vignetting, DefaultCrop, LUT.
// =============================================================================

// Vertex shader
@vs vs
in vec2 position;
in vec2 texcoord0;
out vec2 uv;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = texcoord0;
}
@end

// Fragment shader
@fs fs_develop
layout(binding=0) uniform texture2D srcTex;
layout(binding=0) uniform sampler srcSmp;
layout(binding=1) uniform texture3D lutTex;
layout(binding=1) uniform sampler lutSmp;
layout(binding=2) uniform texture2D lensLutTex;
layout(binding=2) uniform sampler lensLutSmp;
layout(binding=3) uniform texture2D vigTex;
layout(binding=3) uniform sampler vigSmp;

layout(binding=0) uniform fs_develop_params {
    float lutSize;
    float lutBlend;
    float lensEnabled;
    float autoScale;
    vec4 cropRect;       // (originX, originY, sizeX, sizeY) in normalized UV
    vec2 opticalCenter;  // optical center in source UV
    float invDiag;       // 1 / diagonal_half (pixel space)
    float vigEnabled;
    vec2 imageSize;      // source image size in pixels
    float exposure;      // EV stops (-3 to +3), default 0
    float wbTemp;        // temperature shift (-1 to +1), default 0
    float wbTint;        // tint shift (-1 to +1), default 0
    float _pad1;
};

in vec2 uv;
out vec4 frag_color;

void main() {
    vec3 color;

    if (lensEnabled > 0.5) {
        // 1. Output UV -> intermediate image position
        vec2 cropCenter = cropRect.xy + cropRect.zw * 0.5;
        vec2 pos = cropRect.xy + uv * cropRect.zw;
        vec2 centered = (pos - cropCenter) / autoScale;

        // 2. Radius -> 1D LUT lookup for distortion+TCA factors
        vec2 fromOptical = (cropCenter + centered) - opticalCenter;
        float r = length(fromOptical * imageSize) * invDiag;
        r = clamp(r, 0.0, 1.0);
        vec3 factors = texture(sampler2D(lensLutTex, lensLutSmp), vec2(r, 0.5)).rgb;

        // 3. Per-channel UV remap (distortion + chromatic aberration)
        vec2 uvR = opticalCenter + fromOptical * factors.r;
        vec2 uvG = opticalCenter + fromOptical * factors.g;
        vec2 uvB = opticalCenter + fromOptical * factors.b;
        color = vec3(texture(sampler2D(srcTex, srcSmp), uvR).r,
                     texture(sampler2D(srcTex, srcSmp), uvG).g,
                     texture(sampler2D(srcTex, srcSmp), uvB).b);

        // 4. Vignetting correction
        if (vigEnabled > 0.5) {
            vec2 vigUV = cropCenter + centered;
            float gain = texture(sampler2D(vigTex, vigSmp), vigUV).r;
            color *= gain;
        }
    } else {
        // Simple crop (no distortion)
        vec2 texUV = cropRect.xy + uv * cropRect.zw;
        color = texture(sampler2D(srcTex, srcSmp), texUV).rgb;
    }

    // 5. Exposure (EV stops)
    if (exposure != 0.0) {
        color *= pow(2.0, exposure);
    }

    // 6. White balance (simplified RGB multiplier)
    if (wbTemp != 0.0 || wbTint != 0.0) {
        color.r *= 1.0 + wbTemp * 0.2;
        color.b *= 1.0 - wbTemp * 0.2;
        color.g *= 1.0 - wbTint * 0.2;
    }

    // 7. Camera profile LUT
    if (lutBlend > 0.0) {
        float scale = (lutSize - 1.0) / lutSize;
        float offset = 0.5 / lutSize;
        vec3 lutCoord = clamp(color, 0.0, 1.0) * scale + offset;
        vec3 lutColor = texture(sampler3D(lutTex, lutSmp), lutCoord).rgb;
        color = mix(color, lutColor, lutBlend);
    }

    frag_color = vec4(color, 1.0);
}
@end

@program develop vs fs_develop
