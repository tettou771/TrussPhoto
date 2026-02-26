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
    float wbR;           // WB red multiplier (default 1.0)
    float wbG;           // WB green multiplier (default 1.0)
    float wbB;           // WB blue multiplier (default 1.0)
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

    // 5–7. Exposure + WB + Tone — all in linear light (single linearize pass)
    // Lr processes in linear ProPhoto RGB; we approximate in linear sRGB.
    // Lr uses Local Laplacian for HL/SH which we can't do in a pixel shader,
    // so we approximate with a global parametric curve.
    {
        color = pow(max(color, 0.0), vec3(2.2));  // sRGB → linear

        // Exposure (linear gain, §7)
        if (exposure != 0.0) {
            color *= pow(2.0, exposure);
        }

        // White balance (physically-based RGB multiplier from Bradford CAT)
        // Default (1,1,1) = no correction. Computed on CPU from color temperature.
        color *= vec3(wbR, wbG, wbB);

        // Contrast: power-curve around 18% gray pivot (linear 0.18)
        // f(x) = pivot * pow(x / pivot, p)  — identity when p=1
        // p > 1 steepens (more contrast), p < 1 flattens (less contrast)
        if (contrast != 0.0) {
            float c = contrast / 100.0;
            float p = 1.0 + c * 1.5;  // range: -0.5 to 2.5
            float pivot = 0.18;
            color = pivot * pow(max(color, vec3(0.00001)) / pivot, vec3(p));
        }

        // Blacks: black-point curve bend (levels-style remap)
        // Negative: crush (push dark pixels toward 0)
        // Positive: lift (raise the floor)
        if (blacks != 0.0) {
            float b = blacks / 100.0;
            if (b < 0.0) {
                float thresh = -b * 0.06;  // at -100: 6% threshold in linear
                color = max((color - thresh) / (1.0 - thresh), 0.0);
            } else {
                float lift = b * 0.06;
                color = color * (1.0 - lift) + lift;
            }
        }

        // Whites: levels-style white-point adjustment (global, symmetric to Blacks)
        if (whites != 0.0) {
            float w = whites / 100.0;
            if (w > 0.0) {
                // Extend: push white point out (brighten highlights globally)
                color = color / (1.0 - w * 0.06);
            } else {
                // Compress: pull white point in (darken highlights globally)
                color = color * (1.0 + w * 0.06);
            }
        }

        // Highlights / Shadows: zone-based adjustment via luminance ratio.
        // Computed after Blacks/Whites so zone masks see the adjusted image.
        // Zone ranges from Lr §9.3 in linear space.
        float lum = clamp(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        float adj = 0.0;

        // Highlights: 0.65–0.95 linear (ramp up)
        if (highlights != 0.0) {
            float mask = smoothstep(0.55, 0.90, lum);
            adj += highlights / 100.0 * 0.25 * mask;
        }

        // Shadows: bell-shaped — lifts shadow detail while preserving
        // true blacks. Rises from 0.02, peaks at ~0.08–0.20, fades by 0.40.
        if (shadows != 0.0) {
            float mask = smoothstep(0.02, 0.08, lum) * (1.0 - smoothstep(0.20, 0.40, lum));
            adj += shadows / 100.0 * 0.30 * mask;
        }

        // Apply via luminance ratio with soft additive fallback for darks
        if (adj != 0.0) {
            float lumPost = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float target = max(lumPost + adj, 0.0);
            float blend = smoothstep(0.02, 0.10, lumPost);
            vec3 addResult = max(color + adj, 0.0);
            vec3 ratResult = (lumPost > 0.0001) ? color * (target / lumPost) : addResult;
            color = mix(addResult, ratResult, blend);
        }

        // --- Back to sRGB gamma ---
        color = pow(max(color, 0.0), vec3(1.0 / 2.2));
    }

    // 8. Color adjustments (Saturation, Vibrance) — in sRGB/perceptual space
    {
        float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));

        // Saturation: uniform linear mix (§14)
        if (saturation != 0.0) {
            float s = saturation / 100.0;
            color = mix(vec3(gray), color, 1.0 + s);
        }

        // Vibrance: non-linear saturation boost with skin tone protection (§13)
        if (vibrance != 0.0) {
            float maxC = max(color.r, max(color.g, color.b));
            float minC = min(color.r, min(color.g, color.b));
            float range = maxC - minC;
            float sat = (maxC > 0.001) ? range / maxC : 0.0;

            // g(S) = (1-S)^gamma — low saturation gets more boost (§13.1)
            float weight = pow(1.0 - sat, 1.5);

            // h(H) — skin tone protection: reduce effect near hue ≈ 20° (§13.1)
            float skinProtect = 1.0;
            if (range > 0.001) {
                float hue;
                if (maxC == color.r) {
                    hue = mod((color.g - color.b) / range, 6.0) / 6.0;
                } else if (maxC == color.g) {
                    hue = ((color.b - color.r) / range + 2.0) / 6.0;
                } else {
                    hue = ((color.r - color.g) / range + 4.0) / 6.0;
                }
                // Gaussian centered at 20° (0.055), sigma 15° (0.042)
                float d = abs(hue - 0.055);
                d = min(d, 1.0 - d);  // circular distance
                skinProtect = 1.0 - 0.6 * exp(-(d * d) / (2.0 * 0.042 * 0.042));
            }

            float v = vibrance / 100.0;
            color = mix(vec3(gray), color, 1.0 + v * weight * skinProtect);
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
