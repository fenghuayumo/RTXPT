#ifndef __HYBRID_GAUSSIAN_SHADOW_HLSLI__
#define __HYBRID_GAUSSIAN_SHADOW_HLSLI__

static const float kGaussianShadowExtent = 8.0f;

float3x3 HybridGaussian_LoadCovariance(GaussianSplatData splat, float splatScale)
{
    float scale2 = splatScale * splatScale;
    return float3x3(
        splat.covariance0.x * scale2, splat.covariance0.y * scale2, splat.covariance0.z * scale2,
        splat.covariance0.y * scale2, splat.covariance0.w * scale2, splat.covariance1.x * scale2,
        splat.covariance0.z * scale2, splat.covariance1.x * scale2, splat.covariance1.y * scale2);
}

bool HybridGaussian_Invert3x3(float3x3 m, out float3x3 invM)
{
    float a = m[0][0], b = m[0][1], c = m[0][2];
    float d = m[1][0], e = m[1][1], f = m[1][2];
    float g = m[2][0], h = m[2][1], i = m[2][2];

    float A = e * i - f * h;
    float B = c * h - b * i;
    float C = b * f - c * e;
    float D = f * g - d * i;
    float E = a * i - c * g;
    float F = c * d - a * f;
    float G = d * h - e * g;
    float H = b * g - a * h;
    float I = a * e - b * d;

    float det = a * A + b * D + c * G;
    if (abs(det) < 1e-12f)
    {
        invM = 0.0f;
        return false;
    }

    float invDet = 1.0f / det;
    invM = float3x3(
        A * invDet, B * invDet, C * invDet,
        D * invDet, E * invDet, F * invDet,
        G * invDet, H * invDet, I * invDet);
    return true;
}

float HybridGaussian_QuadraticForm(float3x3 m, float3 v)
{
    return dot(v, mul(m, v));
}

bool HybridGaussian_IntersectSplat(RayDesc ray, GaussianSplatData splat, float splatScale, float alphaThreshold, out float hitT)
{
    hitT = 0.0f;

    if (splat.centerOpacity.w <= alphaThreshold)
        return false;

    float3x3 invCov;
    if (!HybridGaussian_Invert3x3(HybridGaussian_LoadCovariance(splat, max(splatScale, 1e-4f)), invCov))
        return false;

    float3 localOrigin = ray.Origin - splat.centerOpacity.xyz;
    float3 localDir = ray.Direction;

    float a = HybridGaussian_QuadraticForm(invCov, localDir);
    float b = 2.0f * dot(localDir, mul(invCov, localOrigin));
    float c = HybridGaussian_QuadraticForm(invCov, localOrigin) - kGaussianShadowExtent;

    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f || abs(a) < 1e-12f)
        return false;

    float root = sqrt(discriminant);
    float invDenom = 0.5f / a;
    float t0 = (-b - root) * invDenom;
    float t1 = (-b + root) * invDenom;
    float t = (t0 >= ray.TMin) ? t0 : t1;

    if (t < ray.TMin || t > ray.TMax)
        return false;

    float3 p = localOrigin + localDir * t;
    float density = exp(-0.5f * HybridGaussian_QuadraticForm(invCov, p)) * splat.centerOpacity.w;
    if (density <= alphaThreshold)
        return false;

    hitT = t;
    return true;
}

bool HybridGaussian_TraceGaussianShadow(
    RaytracingAccelerationStructure gaussianBVH,
    StructuredBuffer<GaussianSplatData> splats,
    uint splatCount,
    RayDesc ray,
    float splatScale,
    float alphaThreshold)
{
    if (splatCount == 0)
        return false;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
    rayQuery.TraceRayInline(gaussianBVH, RAY_FLAG_NONE, 0xff, ray);

    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            uint splatIndex = rayQuery.CandidatePrimitiveIndex();
            float hitT = 0.0f;
            if (splatIndex < splatCount
                && HybridGaussian_IntersectSplat(ray, splats[splatIndex], splatScale, alphaThreshold, hitT))
            {
                rayQuery.CommitProceduralPrimitiveHit(hitT);
            }
        }
    }

    return rayQuery.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT;
}

bool HybridGaussian_TraceMeshShadow(RaytracingAccelerationStructure meshBVH, RayDesc ray)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
    rayQuery.TraceRayInline(meshBVH, RAY_FLAG_FORCE_OPAQUE, 0xff, ray);

    while (rayQuery.Proceed())
    {
    }

    return rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#endif
