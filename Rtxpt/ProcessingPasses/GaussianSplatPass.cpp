/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "GaussianSplatPass.h"

#include "../GPUSort/GPUSort.h"
#include "../SampleCommon/RenderTargets.h"

#include <donut/core/log.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <donut/shaders/view_cb.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

using namespace donut;
using namespace donut::math;

namespace
{
    constexpr float kSH_C0 = 0.28209479177387814f;
    constexpr std::array<float, 15> kRdfToRubShFlip = {
        -1.0f, -1.0f, 1.0f, -1.0f, 1.0f,
         1.0f, -1.0f, 1.0f, -1.0f, 1.0f,
        -1.0f, -1.0f, 1.0f, -1.0f, 1.0f
    };

    enum class PlyFormat
    {
        Ascii,
        BinaryLittleEndian,
        Unsupported
    };

    enum class PlyScalarType
    {
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Float32,
        Float64,
        Invalid
    };

    struct PlyProperty
    {
        std::string name;
        PlyScalarType type = PlyScalarType::Invalid;
        bool isList = false;
        PlyScalarType listCountType = PlyScalarType::Invalid;
    };

    struct PlyElement
    {
        std::string name;
        uint64_t count = 0;
        std::vector<PlyProperty> properties;
    };

    struct RawGaussianSplat
    {
        float position[3] = {};
        float scale[3] = { 1.0f, 1.0f, 1.0f };
        float rotation[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; // w, x, y, z
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float alpha = 1.0f;
    };

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }

    std::string Trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();

        size_t first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
            ++first;

        if (first)
            value.erase(0, first);

        return value;
    }

    PlyScalarType ParseScalarType(const std::string& token)
    {
        const std::string type = ToLower(token);

        if (type == "char" || type == "int8")
            return PlyScalarType::Int8;
        if (type == "uchar" || type == "uint8" || type == "uint8_t")
            return PlyScalarType::UInt8;
        if (type == "short" || type == "int16")
            return PlyScalarType::Int16;
        if (type == "ushort" || type == "uint16")
            return PlyScalarType::UInt16;
        if (type == "int" || type == "int32")
            return PlyScalarType::Int32;
        if (type == "uint" || type == "uint32")
            return PlyScalarType::UInt32;
        if (type == "float" || type == "float32")
            return PlyScalarType::Float32;
        if (type == "double" || type == "float64")
            return PlyScalarType::Float64;

        return PlyScalarType::Invalid;
    }

    size_t ScalarTypeSize(PlyScalarType type)
    {
        switch (type)
        {
        case PlyScalarType::Int8:
        case PlyScalarType::UInt8:
            return 1;
        case PlyScalarType::Int16:
        case PlyScalarType::UInt16:
            return 2;
        case PlyScalarType::Int32:
        case PlyScalarType::UInt32:
        case PlyScalarType::Float32:
            return 4;
        case PlyScalarType::Float64:
            return 8;
        default:
            return 0;
        }
    }

    bool ReadScalarBinary(std::istream& stream, PlyScalarType type, double& value)
    {
        switch (type)
        {
        case PlyScalarType::Int8:
        {
            int8_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt8:
        {
            uint8_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Int16:
        {
            int16_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt16:
        {
            uint16_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Int32:
        {
            int32_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt32:
        {
            uint32_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Float32:
        {
            float v = 0.0f;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Float64:
        {
            double v = 0.0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = v;
            return bool(stream);
        }
        default:
            return false;
        }
    }

    bool ParseScalarAscii(std::istream& stream, PlyScalarType type, double& value)
    {
        std::string token;
        if (!(stream >> token))
            return false;

        try
        {
            switch (type)
            {
            case PlyScalarType::Float32:
            case PlyScalarType::Float64:
                value = std::stod(token);
                return true;
            case PlyScalarType::Int8:
            case PlyScalarType::Int16:
            case PlyScalarType::Int32:
                value = double(std::stoll(token));
                return true;
            case PlyScalarType::UInt8:
            case PlyScalarType::UInt16:
            case PlyScalarType::UInt32:
                value = double(std::stoull(token));
                return true;
            default:
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
    }

    int FindProperty(const std::vector<PlyProperty>& properties, const char* name)
    {
        for (size_t index = 0; index < properties.size(); ++index)
        {
            if (!properties[index].isList && properties[index].name == name)
                return int(index);
        }

        return -1;
    }

    int FindFirstProperty(const std::vector<PlyProperty>& properties, const std::initializer_list<const char*> names)
    {
        for (const char* name : names)
        {
            int index = FindProperty(properties, name);
            if (index >= 0)
                return index;
        }

        return -1;
    }

    bool SkipElementRowBinary(std::istream& stream, const PlyElement& element)
    {
        for (const PlyProperty& property : element.properties)
        {
            if (!property.isList)
            {
                stream.seekg(static_cast<std::streamoff>(ScalarTypeSize(property.type)), std::ios::cur);
                if (!stream)
                    return false;
                continue;
            }

            double countValue = 0.0;
            if (!ReadScalarBinary(stream, property.listCountType, countValue))
                return false;

            const auto count = static_cast<uint64_t>(std::max(0.0, countValue));
            stream.seekg(static_cast<std::streamoff>(count * ScalarTypeSize(property.type)), std::ios::cur);
            if (!stream)
                return false;
        }

        return true;
    }

    float Clamp01(float value)
    {
        return std::min(1.0f, std::max(0.0f, value));
    }

    float Sigmoid(float value)
    {
        return 1.0f / (1.0f + std::exp(-value));
    }

    void NormalizeQuaternion(float rotation[4])
    {
        float lengthSquared = rotation[0] * rotation[0] + rotation[1] * rotation[1] +
            rotation[2] * rotation[2] + rotation[3] * rotation[3];

        if (lengthSquared <= std::numeric_limits<float>::epsilon())
        {
            rotation[0] = 1.0f;
            rotation[1] = rotation[2] = rotation[3] = 0.0f;
            return;
        }

        float invLength = 1.0f / std::sqrt(lengthSquared);
        rotation[0] *= invLength;
        rotation[1] *= invLength;
        rotation[2] *= invLength;
        rotation[3] *= invLength;
    }

    SimpleViewConstants FromPlanarViewConstants(const PlanarViewConstants& view)
    {
        SimpleViewConstants ret;
        ret.matWorldToView = view.matWorldToView;
        ret.matViewToClip = view.matViewToClip;
        ret.matWorldToClipNoOffset = view.matWorldToClipNoOffset;
        ret.matClipToWorldNoOffset = view.matClipToWorldNoOffset;
        ret.matWorldToClip = view.matWorldToClip;
        ret.clipToWindowBias = view.clipToWindowBias;
        ret.clipToWindowScale = view.clipToWindowScale;
        ret.viewportOrigin = view.viewportOrigin;
        ret.viewportSize = view.viewportSize;
        ret.viewportSizeInv = view.viewportSizeInv;
        ret.pixelOffset = view.pixelOffset;
        return ret;
    }

    GaussianSplatData ConvertToGpuSplat(const RawGaussianSplat& raw, bool convertRdfToDonut)
    {
        float rotation[4] = { raw.rotation[0], raw.rotation[1], raw.rotation[2], raw.rotation[3] };
        float position[3] = { raw.position[0], raw.position[1], raw.position[2] };

        if (convertRdfToDonut)
        {
            position[1] = -position[1];
            position[2] = -position[2];
            rotation[2] = -rotation[2];
            rotation[3] = -rotation[3];
        }

        NormalizeQuaternion(rotation);

        const float w = rotation[0];
        const float x = rotation[1];
        const float y = rotation[2];
        const float z = rotation[3];

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        const float r00 = 1.0f - 2.0f * (yy + zz);
        const float r01 = 2.0f * (xy - wz);
        const float r02 = 2.0f * (xz + wy);
        const float r10 = 2.0f * (xy + wz);
        const float r11 = 1.0f - 2.0f * (xx + zz);
        const float r12 = 2.0f * (yz - wx);
        const float r20 = 2.0f * (xz - wy);
        const float r21 = 2.0f * (yz + wx);
        const float r22 = 1.0f - 2.0f * (xx + yy);

        const float sx2 = raw.scale[0] * raw.scale[0];
        const float sy2 = raw.scale[1] * raw.scale[1];
        const float sz2 = raw.scale[2] * raw.scale[2];

        float cov00 = r00 * r00 * sx2 + r01 * r01 * sy2 + r02 * r02 * sz2;
        float cov01 = r00 * r10 * sx2 + r01 * r11 * sy2 + r02 * r12 * sz2;
        float cov02 = r00 * r20 * sx2 + r01 * r21 * sy2 + r02 * r22 * sz2;
        float cov11 = r10 * r10 * sx2 + r11 * r11 * sy2 + r12 * r12 * sz2;
        float cov12 = r10 * r20 * sx2 + r11 * r21 * sy2 + r12 * r22 * sz2;
        float cov22 = r20 * r20 * sx2 + r21 * r21 * sy2 + r22 * r22 * sz2;

        GaussianSplatData splat = {};
        splat.centerOpacity = float4(position[0], position[1], position[2], Clamp01(raw.alpha));
        splat.covariance0 = float4(cov00, cov01, cov02, cov11);
        splat.covariance1 = float4(cov12, cov22, 0.0f, 0.0f);
        splat.color = float4(Clamp01(raw.color[0]), Clamp01(raw.color[1]), Clamp01(raw.color[2]), 0.0f);

        return splat;
    }

    bool LoadPlyFile(
        const std::filesystem::path& fileName,
        bool convertRdfToDonut,
        std::vector<GaussianSplatData>& splats,
        std::vector<float4>& shCoefficients,
        uint32_t& shDegree)
    {
        std::ifstream file(fileName, std::ios::binary);
        if (!file)
        {
            log::error("Failed to open Gaussian splat PLY file: %s", fileName.string().c_str());
            return false;
        }

        std::string line;
        if (!std::getline(file, line) || Trim(line) != "ply")
        {
            log::error("Invalid Gaussian splat PLY header: %s", fileName.string().c_str());
            return false;
        }

        PlyFormat format = PlyFormat::Unsupported;
        std::vector<PlyElement> elements;
        PlyElement* currentElement = nullptr;

        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty())
                continue;

            std::istringstream parser(line);
            std::string keyword;
            parser >> keyword;

            if (keyword == "end_header")
                break;

            if (keyword == "comment" || keyword == "obj_info")
                continue;

            if (keyword == "format")
            {
                std::string formatToken;
                parser >> formatToken;
                formatToken = ToLower(formatToken);

                if (formatToken == "ascii")
                    format = PlyFormat::Ascii;
                else if (formatToken == "binary_little_endian")
                    format = PlyFormat::BinaryLittleEndian;
                else
                    format = PlyFormat::Unsupported;

                continue;
            }

            if (keyword == "element")
            {
                PlyElement element;
                parser >> element.name >> element.count;
                elements.push_back(std::move(element));
                currentElement = &elements.back();
                continue;
            }

            if (keyword == "property" && currentElement)
            {
                std::string typeToken;
                parser >> typeToken;

                PlyProperty property;
                if (typeToken == "list")
                {
                    std::string countTypeToken;
                    std::string valueTypeToken;
                    parser >> countTypeToken >> valueTypeToken >> property.name;
                    property.isList = true;
                    property.listCountType = ParseScalarType(countTypeToken);
                    property.type = ParseScalarType(valueTypeToken);
                }
                else
                {
                    parser >> property.name;
                    property.type = ParseScalarType(typeToken);
                }

                if (property.type == PlyScalarType::Invalid ||
                    (property.isList && property.listCountType == PlyScalarType::Invalid))
                {
                    log::error("Unsupported PLY property type in %s", fileName.string().c_str());
                    return false;
                }

                currentElement->properties.push_back(std::move(property));
            }
        }

        if (format == PlyFormat::Unsupported)
        {
            log::error("Unsupported PLY format in %s", fileName.string().c_str());
            return false;
        }

        auto vertexElementIt = std::find_if(elements.begin(), elements.end(),
            [](const PlyElement& element) { return element.name == "vertex"; });

        if (vertexElementIt == elements.end() || vertexElementIt->count == 0)
        {
            log::error("Gaussian splat PLY has no vertex element: %s", fileName.string().c_str());
            return false;
        }

        const PlyElement& vertexElement = *vertexElementIt;
        const auto& properties = vertexElement.properties;

        const int xIndex = FindProperty(properties, "x");
        const int yIndex = FindProperty(properties, "y");
        const int zIndex = FindProperty(properties, "z");
        const int opacityIndex = FindProperty(properties, "opacity");
        const int scale0Index = FindProperty(properties, "scale_0");
        const int scale1Index = FindProperty(properties, "scale_1");
        const int scale2Index = FindProperty(properties, "scale_2");
        const int rot0Index = FindProperty(properties, "rot_0");
        const int rot1Index = FindProperty(properties, "rot_1");
        const int rot2Index = FindProperty(properties, "rot_2");
        const int rot3Index = FindProperty(properties, "rot_3");
        const int fdc0Index = FindProperty(properties, "f_dc_0");
        const int fdc1Index = FindProperty(properties, "f_dc_1");
        const int fdc2Index = FindProperty(properties, "f_dc_2");
        const int redIndex = FindFirstProperty(properties, { "red", "r", "diffuse_red" });
        const int greenIndex = FindFirstProperty(properties, { "green", "g", "diffuse_green" });
        const int blueIndex = FindFirstProperty(properties, { "blue", "b", "diffuse_blue" });

        std::array<int, 45> fRestIndices;
        fRestIndices.fill(-1);
        uint32_t fRestCount = 0;
        for (uint32_t index = 0; index < uint32_t(fRestIndices.size()); ++index)
        {
            std::string propertyName = "f_rest_" + std::to_string(index);
            fRestIndices[index] = FindProperty(properties, propertyName.c_str());
            if (fRestIndices[index] >= 0)
                ++fRestCount;
        }

        if (fRestCount >= 45)
            shDegree = 3;
        else if (fRestCount >= 24)
            shDegree = 2;
        else if (fRestCount >= 9)
            shDegree = 1;
        else
            shDegree = 0;

        const bool hasRequired3dgsProperties =
            xIndex >= 0 && yIndex >= 0 && zIndex >= 0 &&
            opacityIndex >= 0 &&
            scale0Index >= 0 && scale1Index >= 0 && scale2Index >= 0 &&
            rot0Index >= 0 && rot1Index >= 0 && rot2Index >= 0 && rot3Index >= 0 &&
            ((fdc0Index >= 0 && fdc1Index >= 0 && fdc2Index >= 0) ||
             (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0));

        if (!hasRequired3dgsProperties)
        {
            log::error("PLY file does not contain the expected 3DGS attributes: %s", fileName.string().c_str());
            return false;
        }

        splats.clear();
        splats.reserve(size_t(vertexElement.count));
        shCoefficients.clear();
        shCoefficients.reserve(size_t(vertexElement.count) * GAUSSIAN_SPLAT_SH_FLOAT4_COUNT);

        std::vector<double> values(properties.size(), 0.0);

        for (const PlyElement& element : elements)
        {
            if (element.name != "vertex")
            {
                for (uint64_t row = 0; row < element.count; ++row)
                {
                    if (format == PlyFormat::Ascii)
                    {
                        std::getline(file, line);
                    }
                    else if (!SkipElementRowBinary(file, element))
                    {
                        log::error("Failed while skipping PLY element in %s", fileName.string().c_str());
                        return false;
                    }
                }
                continue;
            }

            for (uint64_t row = 0; row < element.count; ++row)
            {
                if (format == PlyFormat::Ascii)
                {
                    if (!std::getline(file, line))
                    {
                        log::error("Unexpected end of PLY vertex data: %s", fileName.string().c_str());
                        return false;
                    }

                    std::istringstream rowParser(line);
                    for (size_t propertyIndex = 0; propertyIndex < properties.size(); ++propertyIndex)
                    {
                        const PlyProperty& property = properties[propertyIndex];

                        if (property.isList)
                        {
                            double countValue = 0.0;
                            if (!ParseScalarAscii(rowParser, property.listCountType, countValue))
                                return false;
                            for (uint64_t i = 0; i < static_cast<uint64_t>(std::max(0.0, countValue)); ++i)
                            {
                                double ignored = 0.0;
                                if (!ParseScalarAscii(rowParser, property.type, ignored))
                                    return false;
                            }
                        }
                        else if (!ParseScalarAscii(rowParser, property.type, values[propertyIndex]))
                        {
                            log::error("Failed to parse PLY vertex row in %s", fileName.string().c_str());
                            return false;
                        }
                    }
                }
                else
                {
                    for (size_t propertyIndex = 0; propertyIndex < properties.size(); ++propertyIndex)
                    {
                        const PlyProperty& property = properties[propertyIndex];

                        if (property.isList)
                        {
                            double countValue = 0.0;
                            if (!ReadScalarBinary(file, property.listCountType, countValue))
                                return false;
                            file.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(std::max(0.0, countValue)) * ScalarTypeSize(property.type)), std::ios::cur);
                            if (!file)
                                return false;
                        }
                        else if (!ReadScalarBinary(file, property.type, values[propertyIndex]))
                        {
                            log::error("Failed to read PLY vertex row in %s", fileName.string().c_str());
                            return false;
                        }
                    }
                }

                RawGaussianSplat raw = {};
                raw.position[0] = float(values[xIndex]);
                raw.position[1] = float(values[yIndex]);
                raw.position[2] = float(values[zIndex]);

                raw.scale[0] = std::exp(float(values[scale0Index]));
                raw.scale[1] = std::exp(float(values[scale1Index]));
                raw.scale[2] = std::exp(float(values[scale2Index]));

                raw.rotation[0] = float(values[rot0Index]);
                raw.rotation[1] = float(values[rot1Index]);
                raw.rotation[2] = float(values[rot2Index]);
                raw.rotation[3] = float(values[rot3Index]);

                raw.alpha = Sigmoid(float(values[opacityIndex]));

                if (fdc0Index >= 0)
                {
                    raw.color[0] = 0.5f + kSH_C0 * float(values[fdc0Index]);
                    raw.color[1] = 0.5f + kSH_C0 * float(values[fdc1Index]);
                    raw.color[2] = 0.5f + kSH_C0 * float(values[fdc2Index]);
                }
                else
                {
                    raw.color[0] = float(values[redIndex]) / 255.0f;
                    raw.color[1] = float(values[greenIndex]) / 255.0f;
                    raw.color[2] = float(values[blueIndex]) / 255.0f;
                }

                splats.push_back(ConvertToGpuSplat(raw, convertRdfToDonut));

                std::array<float, GAUSSIAN_SPLAT_SH_FLOAT4_COUNT * 4> packedSh = {};
                if (shDegree > 0)
                {
                    const uint32_t coefficientsPerChannel = shDegree == 3 ? 15u : (shDegree == 2 ? 8u : 3u);
                    const uint32_t coefficientCount = coefficientsPerChannel * 3u;

                    for (uint32_t coeff = 0; coeff < coefficientsPerChannel; ++coeff)
                    {
                        const float coordinateFlip = convertRdfToDonut ? kRdfToRubShFlip[coeff] : 1.0f;
                        for (uint32_t rgb = 0; rgb < 3; ++rgb)
                        {
                            const uint32_t sourceCoeff = rgb * coefficientsPerChannel + coeff;
                            if (sourceCoeff < coefficientCount && fRestIndices[sourceCoeff] >= 0)
                                packedSh[coeff * 3 + rgb] = float(values[fRestIndices[sourceCoeff]]) * coordinateFlip;
                        }
                    }
                }

                for (uint32_t i = 0; i < GAUSSIAN_SPLAT_SH_FLOAT4_COUNT; ++i)
                {
                    shCoefficients.push_back(float4(
                        packedSh[i * 4 + 0],
                        packedSh[i * 4 + 1],
                        packedSh[i * 4 + 2],
                        packedSh[i * 4 + 3]));
                }
            }
        }

        log::info("Loaded %zu Gaussian splats from %s (SH degree %u)", splats.size(), fileName.string().c_str(), shDegree);
        return !splats.empty();
    }
}

GaussianSplatPass::GaussianSplatPass(
    nvrhi::IDevice* device,
    std::shared_ptr<donut::engine::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
{
    m_constantBuffer = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GaussianSplatConstants), "GaussianSplatConstants", 16));

    nvrhi::BindingLayoutDesc renderLayoutDesc;
    renderLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    renderLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(3)
    };
    m_renderBindingLayout = m_device->createBindingLayout(renderLayoutDesc);

    nvrhi::BindingLayoutDesc sortLayoutDesc;
    sortLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    sortLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(0)
    };
    m_sortKeyBindingLayout = m_device->createBindingLayout(sortLayoutDesc);
}

void GaussianSplatPass::SetGpuSort(std::shared_ptr<GPUSort> gpuSort)
{
    m_gpuSort = std::move(gpuSort);
}

bool GaussianSplatPass::LoadFromFile(const std::filesystem::path& fileName, bool convertRdfToDonut)
{
    const std::string extension = ToLower(fileName.extension().string());
    if (extension != ".ply")
    {
        log::error("Unsupported Gaussian splat file extension '%s'. This pass currently supports 3DGS .ply files.", extension.c_str());
        return false;
    }

    std::vector<GaussianSplatData> loadedSplats;
    std::vector<float4> loadedShCoefficients;
    uint32_t loadedShDegree = 0;
    if (!LoadPlyFile(fileName, convertRdfToDonut, loadedSplats, loadedShCoefficients, loadedShDegree))
        return false;

    m_splats = std::move(loadedSplats);
    m_shCoefficients = std::move(loadedShCoefficients);
    m_splatCount = uint32_t(m_splats.size());
    m_shDegree = loadedShDegree;

    if (m_shCoefficients.empty())
        m_shCoefficients.push_back(float4(0.0f, 0.0f, 0.0f, 0.0f));

    nvrhi::BufferDesc splatBufferDesc;
    splatBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(GaussianSplatData);
    splatBufferDesc.structStride = sizeof(GaussianSplatData);
    splatBufferDesc.debugName = "GaussianSplatDataBuffer";
    splatBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    splatBufferDesc.keepInitialState = true;
    m_splatBuffer = m_device->createBuffer(splatBufferDesc);

    nvrhi::BufferDesc shBufferDesc;
    shBufferDesc.byteSize = uint64_t(m_shCoefficients.size()) * sizeof(float4);
    shBufferDesc.structStride = sizeof(float4);
    shBufferDesc.debugName = "GaussianSplatSHBuffer";
    shBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    shBufferDesc.keepInitialState = true;
    m_shBuffer = m_device->createBuffer(shBufferDesc);

    nvrhi::BufferDesc uintBufferDesc;
    uintBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(uint32_t);
    uintBufferDesc.format = nvrhi::Format::R32_UINT;
    uintBufferDesc.canHaveTypedViews = true;
    uintBufferDesc.canHaveUAVs = true;
    uintBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    uintBufferDesc.keepInitialState = true;
    uintBufferDesc.debugName = "GaussianSplatSortedIndexBuffer";
    m_indexBuffer = m_device->createBuffer(uintBufferDesc);

    uintBufferDesc.debugName = "GaussianSplatSortKeyBuffer";
    m_sortKeyBuffer = m_device->createBuffer(uintBufferDesc);

    nvrhi::BufferDesc sortControlDesc;
    sortControlDesc.byteSize = sizeof(uint32_t);
    sortControlDesc.debugName = "GaussianSplatSortControlBuffer";
    sortControlDesc.initialState = nvrhi::ResourceStates::Common;
    sortControlDesc.keepInitialState = true;
    m_sortControlBuffer = m_device->createBuffer(sortControlDesc);

    m_renderBindingSet = nullptr;
    m_sortKeyBindingSet = nullptr;
    m_sourceFileName = fileName.string();
    m_splatUploadPending = true;

    return true;
}

void GaussianSplatPass::CreateBindingSets(const RenderTargets& renderTargets)
{
    if (!m_splatBuffer || !m_shBuffer || !m_indexBuffer || !m_sortKeyBuffer)
        return;

    nvrhi::BindingSetDesc renderBindingSetDesc;
    renderBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_SRV(1, m_indexBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_shBuffer),
        nvrhi::BindingSetItem::Texture_SRV(3, renderTargets.Depth)
    };
    m_renderBindingSet = m_device->createBindingSet(renderBindingSetDesc, m_renderBindingLayout);

    nvrhi::BindingSetDesc sortKeyBindingSetDesc;
    sortKeyBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_sortKeyBuffer, nvrhi::Format::R32_UINT)
    };
    m_sortKeyBindingSet = m_device->createBindingSet(sortKeyBindingSetDesc, m_sortKeyBindingLayout);
}

void GaussianSplatPass::CreatePipeline(const RenderTargets& renderTargets)
{
    if (!HasSplats())
        return;

    m_vertexShader = m_shaderFactory->CreateShader("app/ProcessingPasses/GaussianSplatRaster.hlsl", "vs_main", nullptr, nvrhi::ShaderType::Vertex);
    m_pixelShader = m_shaderFactory->CreateShader("app/ProcessingPasses/GaussianSplatRaster.hlsl", "ps_main", nullptr, nvrhi::ShaderType::Pixel);

    std::vector<donut::engine::ShaderMacro> sortKeyMacros = {
        donut::engine::ShaderMacro({ "GAUSSIAN_SPLAT_SORT_KEYS", "1" })
    };
    m_sortKeyShader = m_shaderFactory->CreateShader("app/ProcessingPasses/GaussianSplatRaster.hlsl", "cs_sort_keys", &sortKeyMacros, nvrhi::ShaderType::Compute);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_renderBindingLayout };
    pipelineDesc.VS = m_vertexShader;
    pipelineDesc.PS = m_pixelShader;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::BlendState::RenderTarget alphaBlend;
    alphaBlend.blendEnable = true;
    alphaBlend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    alphaBlend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    alphaBlend.srcBlendAlpha = nvrhi::BlendFactor::One;
    alphaBlend.destBlendAlpha = nvrhi::BlendFactor::One;
    pipelineDesc.renderState.blendState.targets[0] = alphaBlend;

    m_renderPipeline = m_device->createGraphicsPipeline(
        pipelineDesc,
        renderTargets.OutputFramebuffer->GetFramebuffer(nvrhi::AllSubresources));

    nvrhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.bindingLayouts = { m_sortKeyBindingLayout };
    computePipelineDesc.CS = m_sortKeyShader;
    m_sortKeyPipeline = m_device->createComputePipeline(computePipelineDesc);

    CreateBindingSets(renderTargets);
}

void GaussianSplatPass::UploadSplatDataIfNeeded(nvrhi::ICommandList* commandList)
{
    if (!m_splatUploadPending || m_splats.empty())
        return;

    commandList->writeBuffer(m_splatBuffer, m_splats.data(), m_splats.size() * sizeof(GaussianSplatData));
    commandList->writeBuffer(m_shBuffer, m_shCoefficients.data(), m_shCoefficients.size() * sizeof(float4));
    m_splatUploadPending = false;
}

void GaussianSplatPass::SortSplats(nvrhi::ICommandList* commandList)
{
    if (!m_gpuSort || !m_sortKeyBindingSet || !m_sortKeyPipeline || !m_sortControlBuffer)
        return;

    {
        nvrhi::ComputeState state;
        state.pipeline = m_sortKeyPipeline;
        state.bindings = { m_sortKeyBindingSet };

        commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        commandList->setComputeState(state);
        commandList->dispatch((m_splatCount + 255u) / 256u, 1, 1);
    }

    commandList->writeBuffer(m_sortControlBuffer, &m_splatCount, sizeof(m_splatCount));

    commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_gpuSort->Sort(commandList, m_sortControlBuffer, 0, m_sortKeyBuffer, m_indexBuffer, m_splatCount, true);
}

void GaussianSplatPass::Render(
    nvrhi::ICommandList* commandList,
    const donut::engine::IView& view,
    const RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    if (!settings.enabled || !HasSplats() || !m_renderPipeline || !m_renderBindingSet || !m_gpuSort)
        return;

    commandList->beginMarker("GaussianSplats");

    UploadSplatDataIfNeeded(commandList);

    PlanarViewConstants planarView = {};
    view.FillPlanarViewConstants(planarView);

    GaussianSplatConstants constants = {};
    constants.view = FromPlanarViewConstants(planarView);
    const float3 cameraPosition = view.GetViewOrigin();
    constants.cameraPosition = float4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    constants.splatScale = settings.splatScale;
    constants.alphaScale = settings.alphaScale;
    constants.brightness = settings.brightness;
    constants.splatCount = m_splatCount;
    constants.alphaCullThreshold = settings.alphaCullThreshold;
    constants.shDegree = m_shDegree;
    constants.depthTest = settings.depthTest ? 1u : 0u;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    SortSplats(commandList);

    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(renderTargets.Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(renderTargets.OutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();

    nvrhi::GraphicsState state;
    state.pipeline = m_renderPipeline;
    state.bindings = { m_renderBindingSet };
    state.framebuffer = renderTargets.OutputFramebuffer->GetFramebuffer(nvrhi::AllSubresources);
    state.viewport = view.GetViewportState();
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args;
    args.vertexCount = m_splatCount * 6;
    args.instanceCount = 1;
    commandList->draw(args);

    commandList->endMarker();
}
