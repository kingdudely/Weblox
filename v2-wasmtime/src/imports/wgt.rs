//! Generated enum index -> wgpu value tables (index 0 is empty).
pub fn tex_format(i: u32) -> Option<wgpu::TextureFormat> {
    Some(match i {
        1 => wgpu::TextureFormat::R8Unorm,
        2 => wgpu::TextureFormat::R8Snorm,
        3 => wgpu::TextureFormat::R8Uint,
        4 => wgpu::TextureFormat::R8Sint,
        5 => wgpu::TextureFormat::R16Unorm,
        6 => wgpu::TextureFormat::R16Snorm,
        7 => wgpu::TextureFormat::R16Uint,
        8 => wgpu::TextureFormat::R16Sint,
        9 => wgpu::TextureFormat::R16Float,
        10 => wgpu::TextureFormat::Rg8Unorm,
        11 => wgpu::TextureFormat::Rg8Snorm,
        12 => wgpu::TextureFormat::Rg8Uint,
        13 => wgpu::TextureFormat::Rg8Sint,
        14 => wgpu::TextureFormat::R32Float,
        15 => wgpu::TextureFormat::R32Uint,
        16 => wgpu::TextureFormat::R32Sint,
        17 => wgpu::TextureFormat::Rg16Unorm,
        18 => wgpu::TextureFormat::Rg16Snorm,
        19 => wgpu::TextureFormat::Rg16Uint,
        20 => wgpu::TextureFormat::Rg16Sint,
        21 => wgpu::TextureFormat::Rg16Float,
        22 => wgpu::TextureFormat::Rgba8Unorm,
        23 => wgpu::TextureFormat::Rgba8UnormSrgb,
        24 => wgpu::TextureFormat::Rgba8Snorm,
        25 => wgpu::TextureFormat::Rgba8Uint,
        26 => wgpu::TextureFormat::Rgba8Sint,
        27 => wgpu::TextureFormat::Bgra8Unorm,
        28 => wgpu::TextureFormat::Bgra8UnormSrgb,
        29 => wgpu::TextureFormat::Rgb10a2Uint,
        30 => wgpu::TextureFormat::Rgb10a2Unorm,
        31 => wgpu::TextureFormat::Rg11b10Ufloat,
        32 => wgpu::TextureFormat::Rgb9e5Ufloat,
        33 => wgpu::TextureFormat::Rg32Float,
        34 => wgpu::TextureFormat::Rg32Uint,
        35 => wgpu::TextureFormat::Rg32Sint,
        36 => wgpu::TextureFormat::Rgba16Unorm,
        37 => wgpu::TextureFormat::Rgba16Snorm,
        38 => wgpu::TextureFormat::Rgba16Uint,
        39 => wgpu::TextureFormat::Rgba16Sint,
        40 => wgpu::TextureFormat::Rgba16Float,
        41 => wgpu::TextureFormat::Rgba32Float,
        42 => wgpu::TextureFormat::Rgba32Uint,
        43 => wgpu::TextureFormat::Rgba32Sint,
        44 => wgpu::TextureFormat::Stencil8,
        45 => wgpu::TextureFormat::Depth16Unorm,
        46 => wgpu::TextureFormat::Depth24Plus,
        47 => wgpu::TextureFormat::Depth24PlusStencil8,
        48 => wgpu::TextureFormat::Depth32Float,
        49 => wgpu::TextureFormat::Depth32FloatStencil8,
        50..=63 => return None, // placeholder; filled below
        64 => wgpu::TextureFormat::Etc2Rgb8Unorm,
        65 => wgpu::TextureFormat::Etc2Rgb8UnormSrgb,
        66 => wgpu::TextureFormat::Etc2Rgb8A1Unorm,
        67 => wgpu::TextureFormat::Etc2Rgb8A1UnormSrgb,
        68 => wgpu::TextureFormat::Etc2Rgba8Unorm,
        69 => wgpu::TextureFormat::Etc2Rgba8UnormSrgb,
        70 => wgpu::TextureFormat::EacR11Unorm,
        71 => wgpu::TextureFormat::EacR11Snorm,
        72 => wgpu::TextureFormat::EacRg11Unorm,
        73 => wgpu::TextureFormat::EacRg11Snorm,
        74 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B4x4, channel: wgpu::AstcChannel::Unorm },
        75 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B4x4, channel: wgpu::AstcChannel::UnormSrgb },
        76 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B5x4, channel: wgpu::AstcChannel::Unorm },
        77 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B5x4, channel: wgpu::AstcChannel::UnormSrgb },
        78 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B5x5, channel: wgpu::AstcChannel::Unorm },
        79 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B5x5, channel: wgpu::AstcChannel::UnormSrgb },
        80 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B6x5, channel: wgpu::AstcChannel::Unorm },
        81 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B6x5, channel: wgpu::AstcChannel::UnormSrgb },
        82 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B6x6, channel: wgpu::AstcChannel::Unorm },
        83 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B6x6, channel: wgpu::AstcChannel::UnormSrgb },
        84 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x5, channel: wgpu::AstcChannel::Unorm },
        85 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x5, channel: wgpu::AstcChannel::UnormSrgb },
        86 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x6, channel: wgpu::AstcChannel::Unorm },
        87 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x6, channel: wgpu::AstcChannel::UnormSrgb },
        88 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x8, channel: wgpu::AstcChannel::Unorm },
        89 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B8x8, channel: wgpu::AstcChannel::UnormSrgb },
        90 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x5, channel: wgpu::AstcChannel::Unorm },
        91 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x5, channel: wgpu::AstcChannel::UnormSrgb },
        92 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x6, channel: wgpu::AstcChannel::Unorm },
        93 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x6, channel: wgpu::AstcChannel::UnormSrgb },
        94 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x8, channel: wgpu::AstcChannel::Unorm },
        95 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x8, channel: wgpu::AstcChannel::UnormSrgb },
        96 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x10, channel: wgpu::AstcChannel::Unorm },
        97 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B10x10, channel: wgpu::AstcChannel::UnormSrgb },
        98 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B12x10, channel: wgpu::AstcChannel::Unorm },
        99 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B12x10, channel: wgpu::AstcChannel::UnormSrgb },
        100 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B12x12, channel: wgpu::AstcChannel::Unorm },
        101 => wgpu::TextureFormat::Astc { block: wgpu::AstcBlock::B12x12, channel: wgpu::AstcChannel::UnormSrgb },
        _ => return None,
    })
}

pub fn topology(i: u32) -> Option<wgpu::PrimitiveTopology> {
    Some(match i {
        1 => wgpu::PrimitiveTopology::PointList,
        2 => wgpu::PrimitiveTopology::LineList,
        3 => wgpu::PrimitiveTopology::LineStrip,
        4 => wgpu::PrimitiveTopology::TriangleList,
        5 => wgpu::PrimitiveTopology::TriangleStrip,
        _ => return None,
    })
}

pub fn index_format(i: u32) -> Option<wgpu::IndexFormat> {
    Some(match i { 1 => wgpu::IndexFormat::Uint16, 2 => wgpu::IndexFormat::Uint32, _ => return None })
}

pub fn front_face(i: u32) -> Option<wgpu::FrontFace> {
    Some(match i { 1 => wgpu::FrontFace::Ccw, 2 => wgpu::FrontFace::Cw, _ => return None })
}

pub fn cull_mode(i: u32) -> Option<wgpu::Face> {
    match i { 1 => None, 2 => Some(wgpu::Face::Front), 3 => Some(wgpu::Face::Back), _ => None }
}

pub fn blend_op(i: u32) -> Option<wgpu::BlendOperation> {
    Some(match i {
        1 => wgpu::BlendOperation::Add,
        2 => wgpu::BlendOperation::Subtract,
        3 => wgpu::BlendOperation::ReverseSubtract,
        4 => wgpu::BlendOperation::Min,
        5 => wgpu::BlendOperation::Max,
        _ => return None,
    })
}

pub fn blend_factor(i: u32) -> Option<wgpu::BlendFactor> {
    Some(match i {
        1 => wgpu::BlendFactor::Zero,
        2 => wgpu::BlendFactor::One,
        3 => wgpu::BlendFactor::Src,
        4 => wgpu::BlendFactor::OneMinusSrc,
        5 => wgpu::BlendFactor::SrcAlpha,
        6 => wgpu::BlendFactor::OneMinusSrcAlpha,
        7 => wgpu::BlendFactor::Dst,
        8 => wgpu::BlendFactor::OneMinusDst,
        9 => wgpu::BlendFactor::DstAlpha,
        10 => wgpu::BlendFactor::OneMinusDstAlpha,
        11 => wgpu::BlendFactor::SrcAlphaSaturated,
        12 => wgpu::BlendFactor::Constant,
        13 => wgpu::BlendFactor::OneMinusConstant,
        14 => wgpu::BlendFactor::Src1,
        15 => wgpu::BlendFactor::OneMinusSrc1,
        16 => wgpu::BlendFactor::Src1Alpha,
        17 => wgpu::BlendFactor::OneMinusSrc1Alpha,
        _ => return None,
    })
}

pub fn compare(i: u32) -> Option<wgpu::CompareFunction> {
    Some(match i {
        1 => wgpu::CompareFunction::Never,
        2 => wgpu::CompareFunction::Less,
        3 => wgpu::CompareFunction::Equal,
        4 => wgpu::CompareFunction::LessEqual,
        5 => wgpu::CompareFunction::Greater,
        6 => wgpu::CompareFunction::NotEqual,
        7 => wgpu::CompareFunction::GreaterEqual,
        8 => wgpu::CompareFunction::Always,
        _ => return None,
    })
}

pub fn vertex_format(i: u32) -> Option<wgpu::VertexFormat> {
    Some(match i {
        1 => wgpu::VertexFormat::Uint8,
        2 => wgpu::VertexFormat::Uint8x2,
        3 => wgpu::VertexFormat::Uint8x4,
        4 => wgpu::VertexFormat::Sint8,
        5 => wgpu::VertexFormat::Sint8x2,
        6 => wgpu::VertexFormat::Sint8x4,
        7 => wgpu::VertexFormat::Unorm8,
        8 => wgpu::VertexFormat::Unorm8x2,
        9 => wgpu::VertexFormat::Unorm8x4,
        10 => wgpu::VertexFormat::Snorm8,
        11 => wgpu::VertexFormat::Snorm8x2,
        12 => wgpu::VertexFormat::Snorm8x4,
        13 => wgpu::VertexFormat::Uint16,
        14 => wgpu::VertexFormat::Uint16x2,
        15 => wgpu::VertexFormat::Uint16x4,
        16 => wgpu::VertexFormat::Sint16,
        17 => wgpu::VertexFormat::Sint16x2,
        18 => wgpu::VertexFormat::Sint16x4,
        19 => wgpu::VertexFormat::Unorm16,
        20 => wgpu::VertexFormat::Unorm16x2,
        21 => wgpu::VertexFormat::Unorm16x4,
        22 => wgpu::VertexFormat::Snorm16,
        23 => wgpu::VertexFormat::Snorm16x2,
        24 => wgpu::VertexFormat::Snorm16x4,
        25 => wgpu::VertexFormat::Float16,
        26 => wgpu::VertexFormat::Float16x2,
        27 => wgpu::VertexFormat::Float16x4,
        28 => wgpu::VertexFormat::Float32,
        29 => wgpu::VertexFormat::Float32x2,
        30 => wgpu::VertexFormat::Float32x3,
        31 => wgpu::VertexFormat::Float32x4,
        32 => wgpu::VertexFormat::Uint32,
        33 => wgpu::VertexFormat::Uint32x2,
        34 => wgpu::VertexFormat::Uint32x3,
        35 => wgpu::VertexFormat::Uint32x4,
        36 => wgpu::VertexFormat::Sint32,
        37 => wgpu::VertexFormat::Sint32x2,
        38 => wgpu::VertexFormat::Sint32x3,
        39 => wgpu::VertexFormat::Sint32x4,
        40 => wgpu::VertexFormat::Unorm10_10_10_2,
        41 => wgpu::VertexFormat::Unorm8x4Bgra,
        _ => return None,
    })
}

pub fn step_mode(i: u32) -> Option<wgpu::VertexStepMode> {
    Some(match i { 1 => wgpu::VertexStepMode::Vertex, 2 => wgpu::VertexStepMode::Instance, _ => return None })
}

pub fn stencil_op(i: u32) -> Option<wgpu::StencilOperation> {
    Some(match i {
        1 => wgpu::StencilOperation::Keep,
        2 => wgpu::StencilOperation::Zero,
        3 => wgpu::StencilOperation::Replace,
        4 => wgpu::StencilOperation::Invert,
        5 => wgpu::StencilOperation::IncrementClamp,
        6 => wgpu::StencilOperation::DecrementClamp,
        7 => wgpu::StencilOperation::IncrementWrap,
        8 => wgpu::StencilOperation::DecrementWrap,
        _ => return None,
    })
}

pub fn address_mode(i: u32) -> Option<wgpu::AddressMode> {
    Some(match i { 1 => wgpu::AddressMode::ClampToEdge, 2 => wgpu::AddressMode::Repeat, 3 => wgpu::AddressMode::MirrorRepeat, _ => return None })
}

pub fn filter_mode(i: u32) -> Option<wgpu::FilterMode> {
    Some(match i { 1 => wgpu::FilterMode::Nearest, 2 => wgpu::FilterMode::Linear, _ => return None })
}

pub fn mipmap_filter(i: u32) -> Option<wgpu::MipmapFilterMode> {
    Some(match i { 1 => wgpu::MipmapFilterMode::Nearest, 2 => wgpu::MipmapFilterMode::Linear, _ => return None })
}

pub fn texture_dimension(i: u32) -> Option<wgpu::TextureDimension> {
    Some(match i { 1 => wgpu::TextureDimension::D1, 2 => wgpu::TextureDimension::D2, 3 => wgpu::TextureDimension::D3, _ => return None })
}

pub fn view_dimension(i: u32) -> Option<wgpu::TextureViewDimension> {
    Some(match i {
        1 => wgpu::TextureViewDimension::D1,
        2 => wgpu::TextureViewDimension::D2,
        3 => wgpu::TextureViewDimension::D2Array,
        4 => wgpu::TextureViewDimension::Cube,
        5 => wgpu::TextureViewDimension::CubeArray,
        6 => wgpu::TextureViewDimension::D3,
        _ => return None,
    })
}

pub fn texture_aspect(i: u32) -> Option<wgpu::TextureAspect> {
    Some(match i { 1 => wgpu::TextureAspect::All, 2 => wgpu::TextureAspect::StencilOnly, 3 => wgpu::TextureAspect::DepthOnly, _ => return None })
}

pub fn alpha_mode(i: u32) -> Option<wgpu::CompositeAlphaMode> {
    Some(match i {
        1 => wgpu::CompositeAlphaMode::Opaque,
        2 => wgpu::CompositeAlphaMode::PreMultiplied,
        3 => wgpu::CompositeAlphaMode::PostMultiplied,
        4 => wgpu::CompositeAlphaMode::Inherit,
        _ => return None,
    })
}

pub fn store_op(i: u32) -> Option<wgpu::StoreOp> {
    Some(match i { 1 => wgpu::StoreOp::Store, 2 => wgpu::StoreOp::Discard, _ => return None })
}

/// Reverse of `tex_format`: generated enum index for a wgpu format.
pub fn tex_format_index(f: wgpu::TextureFormat) -> Option<u32> {
    (1..=101u32).find(|&i| tex_format(i) == Some(f))
}

/// WebGPU FeatureName index -> wgpu feature flag (the JS table is 1-based).
pub fn feature(i: u32) -> Option<wgpu::Features> {
    Some(match i {
        1 => wgpu::Features::empty(),
        2 => wgpu::Features::DEPTH_CLIP_CONTROL,
        3 => wgpu::Features::DEPTH32FLOAT_STENCIL8,
        4 => wgpu::Features::TEXTURE_COMPRESSION_BC,
        5 => wgpu::Features::TEXTURE_COMPRESSION_BC_SLICED_3D,
        6 => wgpu::Features::TEXTURE_COMPRESSION_ETC2,
        7 => wgpu::Features::TEXTURE_COMPRESSION_ASTC,
        8 => wgpu::Features::TEXTURE_COMPRESSION_ASTC_SLICED_3D,
        9 => wgpu::Features::TIMESTAMP_QUERY,
        10 => wgpu::Features::INDIRECT_FIRST_INSTANCE,
        11 => wgpu::Features::SHADER_F16,
        12 => wgpu::Features::RG11B10UFLOAT_RENDERABLE,
        13 => wgpu::Features::BGRA8UNORM_STORAGE,
        14 => wgpu::Features::FLOAT32_FILTERABLE,
        15 => wgpu::Features::FLOAT32_BLENDABLE,
        16 => wgpu::Features::CLIP_DISTANCES,
        17 => wgpu::Features::DUAL_SOURCE_BLENDING,
        18 => wgpu::Features::SUBGROUP,
        21 => wgpu::Features::PRIMITIVE_INDEX,
        _ => return None,
    })
}
