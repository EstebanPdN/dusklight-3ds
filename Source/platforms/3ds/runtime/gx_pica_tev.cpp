#include "gx_pica_tev.hpp"

namespace dusk::native::gx {
namespace {
using Source = PicaTevSource;
using Function = PicaTevFunction;

bool inputs(const std::uint8_t (&actual)[4], std::uint8_t a, std::uint8_t b,
            std::uint8_t c, std::uint8_t d) {
    return actual[0] == a && actual[1] == b && actual[2] == c && actual[3] == d;
}
bool operation(const TevOperation& value) {
    return value.operation == 0 && value.bias == 0 && value.scale == 0 &&
           value.clamp == 1 && value.output == 0;
}
bool order(const TevStage& value, std::uint8_t coordinate, std::uint8_t texture,
           std::uint8_t channel) {
    return value.coordinate == coordinate && value.texture == texture && value.channel == channel;
}
PicaTevChannel replace(Source source) {
    return {Function::replace, {source, Source::previous, Source::previous}};
}
PicaTevChannel modulate(Source a, Source b) {
    return {Function::modulate, {a, b, Source::previous}};
}
bool fail(PicaTevError value, PicaTevError* error) {
    if (error) *error = value;
    return false;
}
}

const char* picaTevErrorText(PicaTevError error) {
    switch (error) {
    case PicaTevError::none: return "none";
    case PicaTevError::stageCount: return "PICA TEV requires one or two stages; untextured requires one";
    case PicaTevError::operation: return "PICA TEV requires ADD, zero bias, scale one, clamp, PREV output";
    case PicaTevError::order: return "PICA TEV texture coordinate, map or raster channel is unsupported";
    case PicaTevError::inputs: return "PICA TEV input equation is outside the original J2D subset";
    case PicaTevError::nonzeroBlack: return "PICA TEV tint requires black REG0 RGBA zero";
    }
    return "unknown PICA TEV error";
}

bool planPicaTev(const DrawPacket& packet, PicaTevPlan& output, PicaTevError* error) {
    output = {};
    if (error) *error = PicaTevError::none;
    if (packet.tevCount < 1 || packet.tevCount > 2 || (!packet.textured && packet.tevCount != 1))
        return fail(PicaTevError::stageCount, error);
    for (unsigned i = 0; i < packet.tevCount; ++i) {
        if (!operation(packet.tev[i].colorOp) || !operation(packet.tev[i].alphaOp))
            return fail(PicaTevError::operation, error);
    }

    PicaTevPlan candidate{};
    const auto& base = packet.tev[0];
    if (!packet.textured) {
        if (!order(base, 255, 255, 4)) return fail(PicaTevError::order, error);
        if (!inputs(base.color, 15, 15, 15, 10) || !inputs(base.alpha, 7, 7, 7, 5))
            return fail(PicaTevError::inputs, error);
        candidate.stages[0] = {replace(Source::primary), replace(Source::primary), {}};
    } else {
        if (!order(base, 0, 0, 255)) return fail(PicaTevError::order, error);
        const bool textureAlpha = inputs(base.alpha, 4, 7, 7, 7);
        const bool registerAlpha = inputs(base.alpha, 3, 7, 7, 7);
        if (!inputs(base.color, 8, 15, 15, 15) || (!textureAlpha && !registerAlpha))
            return fail(PicaTevError::inputs, error);
        candidate.stages[0] = {replace(Source::texture0),
            replace(textureAlpha ? Source::texture0 : Source::constant),
            registerAlpha ? packet.registers[3] : Rgba{}};

        if (packet.tevCount == 2) {
            const auto& extra = packet.tev[1];
            const bool tint = inputs(extra.color, 2, 4, 0, 15) && inputs(extra.alpha, 1, 2, 0, 7);
            const bool raster = inputs(extra.color, 15, 0, 10, 15) && inputs(extra.alpha, 7, 0, 5, 7);
            if (!tint && !raster) return fail(PicaTevError::inputs, error);
            if (!order(extra, 255, 255, tint ? 255 : 4)) return fail(PicaTevError::order, error);
            if (tint) {
                const auto& black = packet.registers[1];
                if (black.r || black.g || black.b || black.a)
                    return fail(PicaTevError::nonzeroBlack, error);
                candidate.stages[1] = {modulate(Source::previous, Source::constant),
                                      modulate(Source::previous, Source::constant), packet.registers[2]};
            } else {
                candidate.stages[1] = {modulate(Source::previous, Source::primary),
                                      modulate(Source::previous, Source::primary), {}};
            }
        }
    }
    candidate.count = packet.tevCount;
    output = candidate;
    return true;
}
}
