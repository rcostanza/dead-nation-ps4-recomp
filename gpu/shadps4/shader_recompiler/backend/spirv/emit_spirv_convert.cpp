// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U16, value);
}

Id ExtractS16(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.S16, value);
}

Id ExtractU8(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U8, value);
}

Id ExtractS8(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.S8, value);
}
} // Anonymous namespace

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
}

// deadnation: GCN's v_cvt_i32_f32 and v_cvt_u32_f32 are defined for every input: NaN converts to
// 0 and values past the range clamp to its ends. OpConvertFToS/U leave those undefined, and NVIDIA
// returns huge values: a particle emitter in Blood on the Ice turned a NaN size into a loop count
// of millions, and the GPU timed out. The conversions saturate as GCN does.
static Id SaturatingConvertF32(EmitContext& ctx, Id value, bool is_signed) {
    const f32 low = is_signed ? -2147483648.0f : 0.0f;
    const f32 high = is_signed ? 2147483520.0f : 4294967040.0f; // largest floats below 2^31, 2^32
    const f32 overflow = is_signed ? 2147483648.0f : 4294967296.0f;
    const Id clamped = ctx.OpFClamp(ctx.F32[1], value, ctx.ConstF32(low), ctx.ConstF32(high));
    Id result = is_signed ? ctx.OpConvertFToS(ctx.U32[1], clamped)
                          : ctx.OpConvertFToU(ctx.U32[1], clamped);
    const Id saturated = ctx.ConstU32(is_signed ? 0x7fffffffu : 0xffffffffu);
    result = ctx.OpSelect(ctx.U32[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[1], value,
                                                                  ctx.ConstF32(overflow)),
                          saturated, result);
    return ctx.OpSelect(ctx.U32[1], ctx.OpIsNan(ctx.U1[1], value), ctx.u32_zero_value, result);
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    return SaturatingConvertF32(ctx, value, true);
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return SaturatingConvertF32(ctx, value, false);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertF16F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F16[1], value);
}

Id EmitConvertF32F16(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertU16U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U16, value);
}

Id EmitConvertU32U16(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertU8U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U8, value);
}

Id EmitConvertU32U8(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id EmitConvertS32S8(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], value);
}

Id EmitConvertS32S16(EmitContext& ctx, Id value) {
    return ctx.OpSConvert(ctx.U32[1], value);
}

} // namespace Shader::Backend::SPIRV
