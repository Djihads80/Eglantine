// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// AMD Boost - Combined AMD FidelityFX enhancement suite
// Implements FSR 1 EASU upscaling + RCAS sharpening + optional denoising

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; };

uniform float ScaleFactor <
    ui_type = "slider";
    ui_label = "Scale Factor";
    ui_tooltip = "Upscaling factor (1.0 = native, 1.5 = 1.5x, 2.0 = 2x). Higher values require more performance.";
    ui_min = 1.0; ui_max = 4.0; ui_step = 0.1;
> = 1.5;

uniform float Sharpness <
    ui_type = "slider";
    ui_label = "Sharpness";
    ui_tooltip = "RCAS sharpening strength. Higher values increase edge contrast.";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.5;

uniform bool EnableDenoise <
    ui_type = "checkbox";
    ui_label = "Enable Denoising";
    ui_tooltip = "Apply a light denoising pass before upscaling to reduce artifacts.";
> = false;

uniform float DenoiseStrength <
    ui_type = "slider";
    ui_label = "Denoise Strength";
    ui_tooltip = "Strength of the denoising filter. Only applies when denoising is enabled.";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.3;

uniform bool EnableUpscaling <
    ui_type = "checkbox";
    ui_label = "Enable Upscaling";
    ui_tooltip = "Enable FSR 1 EASU upscaling. Disable for sharpening only.";
> = true;

#define FFX_FSR1_EASU_MAX_TAPS 5

// EASU constants - packed into uniforms
uniform float4 EASUCon0;
uniform float4 EASUCon1;
uniform float4 EASUCon2;
uniform float4 EASUCon3;

// RCAS constants
uniform float4 RCASCon;

// Compute input size from output size and scale factor
float2 GetInputSize(float2 outputSize, float scale)
{
    return outputSize / scale;
}

// EASU: Edge-Adaptive Spatial Upsampling
float4 EASU(float2 uv, float2 inputSize, float2 outputSize)
{
    float2 inputViewportInPixels = outputSize;
    float2 outputSizeInPixels = outputSize;
    float2 inputSizeInPixels = inputSize;

    // Compute constants
    float2 con0 = EASUCon0.xy;
    float2 con1 = EASUCon1.xy;
    float2 con2 = EASUCon2.xy;
    float2 con3 = EASUCon3.xy;

    // Sample positions
    float2 uv_input = uv * con0 + con1;
    
    // 5-tap sampling pattern
    float2 offsets[5];
    offsets[0] = float2(0.0, 0.0);
    offsets[1] = float2(con2.x, con2.y);
    offsets[2] = float2(-con2.x, con2.y);
    offsets[3] = float2(con2.x, -con2.y);
    offsets[4] = float2(-con2.x, -con2.y);

    float4 samples[5];
    for (int i = 0; i < 5; i++)
    {
        float2 sample_uv = uv_input + offsets[i] * con3.xy;
        samples[i] = tex2D(BackBuffer, saturate(sample_uv));
    }

    // Compute gradients
    float3 c = samples[0].rgb;
    float3 n = samples[1].rgb;
    float3 s = samples[2].rgb;
    float3 e = samples[3].rgb;
    float3 w = samples[4].rgb;

    // Luminance weights
    float3 luma_weights = float3(0.299, 0.587, 0.114);
    
    float luma_c = dot(c, luma_weights);
    float luma_n = dot(n, luma_weights);
    float luma_s = dot(s, luma_weights);
    float luma_e = dot(e, luma_weights);
    float luma_w = dot(w, luma_weights);

    // Gradient computation
    float grad_ns = abs(luma_n - luma_s);
    float grad_ew = abs(luma_e - luma_w);

    // Direction
    float2 dir = float2(grad_ew, grad_ns);
    float dir_length = length(dir) + 1e-6;
    dir /= dir_length;

    // Weights
    float len = length(dir * float2(grad_ew, grad_ns));
    float weight = 1.0 / (len + 1e-6);

    // Reconstruct
    float3 result = c;
    result += (n + s - 2.0 * c) * dir.y * weight;
    result += (e + w - 2.0 * c) * dir.x * weight;

    return float4(saturate(result), 1.0);
}

// RCAS: Radeon Contrast Adaptive Sharpening
float4 RCAS(float2 uv, float2 inputSize, float2 outputSize)
{
    float2 texel = float2(1.0 / inputSize.x, 1.0 / inputSize.y);
    
    float3 c = tex2D(BackBuffer, uv).rgb;
    float3 n = tex2D(BackBuffer, uv + float2(0.0, -texel.y)).rgb;
    float3 s = tex2D(BackBuffer, uv + float2(0.0, texel.y)).rgb;
    float3 e = tex2D(BackBuffer, uv + float2(texel.x, 0.0)).rgb;
    float3 w = tex2D(BackBuffer, uv + float2(-texel.x, 0.0)).rgb;

    float3 luma_weights = float3(0.299, 0.587, 0.114);
    
    float luma_c = dot(c, luma_weights);
    float luma_n = dot(n, luma_weights);
    float luma_s = dot(s, luma_weights);
    float luma_e = dot(e, luma_weights);
    float luma_w = dot(w, luma_weights);

    float grad_n = abs(luma_n - luma_c);
    float grad_s = abs(luma_s - luma_c);
    float grad_e = abs(luma_e - luma_c);
    float grad_w = abs(luma_w - luma_c);

    float grad_ns = max(grad_n, grad_s);
    float grad_ew = max(grad_e, grad_w);

    float2 dir = float2(grad_ew, grad_ns);
    float dir_len = length(dir) + 1e-6;
    dir /= dir_len;

    float sharpness = RCASCon.x;
    float weight = sharpness * (1.0 - smoothstep(0.0, 0.5, dir_len));

    float3 result = c;
    result += (n + s - 2.0 * c) * dir.y * weight;
    result += (e + w - 2.0 * c) * dir.x * weight;

    return float4(saturate(result), 1.0);
}

// Simple denoise (bilateral-like)
float4 Denoise(float2 uv, float2 inputSize)
{
    float2 texel = float2(1.0 / inputSize.x, 1.0 / inputSize.y);
    float3 center = tex2D(BackBuffer, uv).rgb;
    
    float3 sum = center;
    float total_weight = 1.0;
    
    float3 luma_weights = float3(0.299, 0.587, 0.114);
    float center_luma = dot(center, luma_weights);
    
    // 4-tap cross pattern
    float2 offsets[4] = {
        float2(-texel.x, 0.0),
        float2(texel.x, 0.0),
        float2(0.0, -texel.y),
        float2(0.0, texel.y)
    };
    
    for (int i = 0; i < 4; i++)
    {
        float3 sample = tex2D(BackBuffer, uv + offsets[i]).rgb;
        float sample_luma = dot(sample, luma_weights);
        float diff = abs(sample_luma - center_luma);
        float weight = exp(-diff * DenoiseStrength * 50.0);
        
        sum += sample * weight;
        total_weight += weight;
    }
    
    return float4(sum / total_weight, 1.0);
}

void VS_PostProcess(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2(float(id & 2), float((id & 1) << 1));
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PS_AMDBoost(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 outputSize = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    float2 inputSize = GetInputSize(outputSize, ScaleFactor);
    
    float4 color = tex2D(BackBuffer, uv);
    
    // Apply denoising first if enabled
    if (EnableDenoise)
    {
        color = Denoise(uv, outputSize);
    }
    
    // Apply upscaling if enabled
    if (EnableUpscaling && ScaleFactor > 1.0)
    {
        color = EASU(uv, inputSize, outputSize);
    }
    
    // Apply RCAS sharpening
    if (Sharpness > 0.0)
    {
        color = RCAS(uv, EnableUpscaling ? inputSize : outputSize, outputSize);
    }
    
    return color;
}

technique AMDBoost <
    ui_label = "AMD Boost";
    ui_tooltip = "AMD FidelityFX Boost: Combines FSR 1 upscaling (EASU), RCAS sharpening, and optional denoising for enhanced image quality.";
>
{
    pass
    {
        VertexShader = VS_PostProcess;
        PixelShader = PS_AMDBoost;
    }
}
