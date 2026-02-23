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
    float contrast;      // -100 to +100, default 0
    float highlights;    // -100 to +100, default 0
    float shadows;       // -100 to +100, default 0
    float whites;        // -100 to +100, default 0
    float blacks;        // -100 to +100, default 0
    float vibrance;      // -100 to +100, default 0
    float saturation;    // -100 to +100, default 0
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

    // 5. Exposure (linear light — Lr applies exposure as linear gain)
    if (exposure != 0.0) {
        color = pow(max(color, 0.0), vec3(2.2));   // sRGB → linear
        color *= pow(2.0, exposure);                // EV gain
        color = pow(color, vec3(1.0 / 2.2));        // linear → sRGB
    }

    // 6. White balance (simplified RGB multiplier)
    if (wbTemp != 0.0 || wbTint != 0.0) {
        color.r *= 1.0 + wbTemp * 0.2;
        color.b *= 1.0 - wbTemp * 0.2;
        color.g *= 1.0 - wbTint * 0.2;
    }

    // 7. Tone adjustments (Contrast, Highlights, Shadows, Whites, Blacks)
    // Lr uses Local Laplacian Filters (image-adaptive) which we can't do
    // in a pixel shader, so we approximate with a global parametric curve.
    // Lr processes in linear ProPhoto RGB; we process in sRGB gamma.
    // Zone ranges from Lr §9.3 (linear) are converted to sRGB equivalents.
    {
        float lum = clamp(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);

        // Contrast: S-curve via smoothstep blend (pivot at 18% gray ≈ sRGB 0.46)
        if (contrast != 0.0) {
            float c = contrast / 100.0;
            vec3 clamped = clamp(color, 0.0, 1.0);
            // Remap so pivot (0.46) maps to 0.5, apply smoothstep S-curve, remap back
            vec3 norm = clamp(clamped * (0.5 / 0.46), 0.0, 1.0);  // 0.46 → 0.5
            vec3 scurve = norm * norm * (3.0 - 2.0 * norm);        // Hermite S-curve
            scurve = scurve * (0.46 / 0.5);                        // 0.5 → 0.46
            color = mix(color, scurve + (color - clamped), c * 0.5);
        }

        // Accumulate luminance adjustment from zone masks.
        // Lr §9.3 linear ranges → sRGB: pow(x, 1/2.2)
        //   Shadows  0.05–0.35 lin → ~0.25–0.62 sRGB
        //   Highlights 0.65–0.95 lin → ~0.82–0.98 sRGB
        //   Blacks   0.00–0.10 lin → ~0.00–0.35 sRGB
        //   Whites   0.90–1.00 lin → ~0.95–1.00 sRGB
        float adj = 0.0;

        // Highlights: ramp up in bright zone (sRGB 0.70–0.95)
        if (highlights != 0.0) {
            float mask = smoothstep(0.70, 0.95, lum);
            adj += highlights / 100.0 * 0.25 * mask;
        }

        // Shadows: bell-shaped mask — lifts shadow detail but preserves
        // true blacks (which the Blacks slider controls).
        // Rises from 0.03, peaks at ~0.15–0.35, falls to 0 at ~0.60.
        if (shadows != 0.0) {
            float mask = smoothstep(0.03, 0.15, lum) * (1.0 - smoothstep(0.35, 0.60, lum));
            adj += shadows / 100.0 * 0.30 * mask;
        }

        // Whites: narrow top end — white point adjustment (sRGB 0.92–1.0)
        if (whites != 0.0) {
            float mask = smoothstep(0.92, 1.0, lum);
            adj += whites / 100.0 * 0.12 * mask;
        }

        // Blacks: narrow bottom — black point adjustment (sRGB 0.0–0.10)
        if (blacks != 0.0) {
            float mask = 1.0 - smoothstep(0.0, 0.10, lum);
            adj += blacks / 100.0 * 0.08 * mask;
        }

        // Apply adjustment preserving color ratios.
        // Soft blend: additive for dark pixels, ratio for bright pixels.
        if (adj != 0.0) {
            float lumPost = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float target = max(lumPost + adj, 0.0);
            float blend = smoothstep(0.05, 0.20, lumPost);
            vec3 addResult = max(color + adj, 0.0);
            vec3 ratResult = (lumPost > 0.001) ? color * (target / lumPost) : addResult;
            color = mix(addResult, ratResult, blend);
        }
    }

    // 8. Color adjustments (Saturation, Vibrance)
    {
        float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));

        // Saturation: uniform linear mix
        if (saturation != 0.0) {
            float s = saturation / 100.0;
            color = mix(vec3(gray), color, 1.0 + s);
        }

        // Vibrance: weighted by inverse saturation + skin tone protection
        if (vibrance != 0.0) {
            float maxC = max(color.r, max(color.g, color.b));
            float minC = min(color.r, min(color.g, color.b));
            float sat = (maxC > 0.001) ? (maxC - minC) / maxC : 0.0;
            float weight = 1.0 - sat;
            float v = vibrance / 100.0;
            color = mix(vec3(gray), color, 1.0 + v * weight);
        }
    }

    // 9. Camera profile LUT
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
