#include "../runtime/gx_pica_tev.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace dusk::native::gx;
namespace {
using Color = std::array<double, 4>;
std::size_t comparisons = 0, rejections = 0;

Color normalized(Rgba color) {
    return {color.r / 255.0, color.g / 255.0, color.b / 255.0, color.a / 255.0};
}

// Independent numeric GX interpreter: no recognition of the planner's patterns.
// Computes normalized real arithmetic, deliberately not undocumented GPU rounding.
double gxInput(unsigned id, bool alpha, unsigned component, const Color& texture,
               const Color& raster, const std::array<Color, 4>& regs) {
    if (alpha) {
        if (id < 4) return regs[id][3];
        if (id == 4) return texture[3];
        if (id == 5) return raster[3];
        if (id == 7) return 0;
    } else {
        if (id < 8) return regs[id / 2][id & 1 ? 3 : component];
        if (id == 8) return texture[component];
        if (id == 9) return texture[3];
        if (id == 10) return raster[component];
        if (id == 11) return raster[3];
        if (id == 12) return 1;
        if (id == 13) return .5;
        if (id == 15) return 0;
    }
    std::abort(); // KONST/other selectors are outside these admitted fixtures.
}

Color evaluateGx(const DrawPacket& packet, const Color& texture, const Color& raster) {
    std::array<Color, 4> regs{};
    for (unsigned i = 0; i < 4; ++i) regs[i] = normalized(packet.registers[i]);
    for (unsigned i = 0; i < packet.tevCount; ++i) {
        const auto& stage = packet.tev[i];
        Color result{};
        for (unsigned c = 0; c < 4; ++c) {
            const bool alpha = c == 3;
            const auto* ids = alpha ? stage.alpha : stage.color;
            const auto& op = alpha ? stage.alphaOp : stage.colorOp;
            assert(op.operation == 0 && op.bias == 0 && op.scale == 0 && op.clamp == 1);
            const double a = gxInput(ids[0], alpha, c, texture, raster, regs);
            const double b = gxInput(ids[1], alpha, c, texture, raster, regs);
            const double factor = gxInput(ids[2], alpha, c, texture, raster, regs);
            const double d = gxInput(ids[3], alpha, c, texture, raster, regs);
            result[c] = std::clamp(d + a * (1.0 - factor) + b * factor, 0.0, 1.0);
        }
        // All accepted fixtures write both channels to PREV, simultaneously.
        assert(stage.colorOp.output == 0 && stage.alphaOp.output == 0);
        regs[0] = result;
    }
    return regs[0];
}

Color evaluatePica(const PicaTevPlan& plan, const Color& texture, const Color& raster) {
    Color previous{};
    assert(plan.count > 0 && plan.count <= plan.stages.size());
    for (unsigned i = 0; i < plan.count; ++i) {
        const auto& stage = plan.stages[i];
        const Color constant = normalized(stage.constant);
        auto select = [&](PicaTevSource source, unsigned component) {
            switch (source) {
            case PicaTevSource::primary: return raster[component];
            case PicaTevSource::texture0: return texture[component];
            case PicaTevSource::previous: return previous[component];
            case PicaTevSource::constant: return constant[component];
            }
            std::abort();
        };
        Color result{};
        for (unsigned c = 0; c < 4; ++c) {
            const auto& channel = c == 3 ? stage.alpha : stage.rgb;
            const double a = select(channel.sources[0], c);
            switch (channel.function) {
            case PicaTevFunction::replace: result[c] = a; break;
            case PicaTevFunction::modulate: result[c] = a * select(channel.sources[1], c); break;
            }
            result[c] = std::clamp(result[c], 0.0, 1.0);
        }
        previous = result;
    }
    return previous;
}

DrawPacket packet(unsigned mode, bool registerAlpha = false) {
    DrawPacket result{};
    result.textured = mode != 0;
    result.tevCount = mode > 1 ? 2 : 1;
    const TevOperation add{0, 0, 0, 1, 0};
    result.tev[0] = mode == 0
        ? TevStage{{15, 15, 15, 10}, {7, 7, 7, 5}, add, add, 255, 255, 4, 0, 0}
        : TevStage{{8, 15, 15, 15}, {std::uint8_t(registerAlpha ? 3 : 4), 7, 7, 7},
                   add, add, 0, 0, 255, 0, 0};
    if (mode == 2)
        result.tev[1] = {{2, 4, 0, 15}, {1, 2, 0, 7}, add, add, 255, 255, 255, 0, 0};
    if (mode == 3)
        result.tev[1] = {{15, 0, 10, 15}, {7, 0, 5, 7}, add, add, 255, 255, 4, 0, 0};
    return result;
}

void compare(const DrawPacket& input, Rgba texture, Rgba raster) {
    PicaTevPlan plan{};
    PicaTevError error = PicaTevError::inputs;
    assert(planPicaTev(input, plan, &error) && error == PicaTevError::none);
    assert(plan.count == input.tevCount && plan.count <= 6);
    const auto expected = evaluateGx(input, normalized(texture), normalized(raster));
    const auto actual = evaluatePica(plan, normalized(texture), normalized(raster));
    for (unsigned c = 0; c < 4; ++c) assert(std::abs(expected[c] - actual[c]) < 1e-12);
    ++comparisons;
}

void assertReset(const PicaTevPlan& plan) {
    assert(plan.count == 0);
    for (const auto& stage : plan.stages) {
        assert(stage.rgb.function == PicaTevFunction{} && stage.alpha.function == PicaTevFunction{});
        for (auto source : stage.rgb.sources) assert(source == PicaTevSource{});
        for (auto source : stage.alpha.sources) assert(source == PicaTevSource{});
        assert(!stage.constant.r && !stage.constant.g && !stage.constant.b && !stage.constant.a);
    }
}

void reject(const DrawPacket& input, PicaTevError expected) {
    PicaTevPlan plan{};
    assert(planPicaTev(packet(3, true), plan)); // Poison with a previous successful plan.
    PicaTevError error = PicaTevError::none;
    assert(!planPicaTev(input, plan, &error) && error == expected);
    assertReset(plan);
    assert(picaTevErrorText(error) && *picaTevErrorText(error));
    assert(!planPicaTev(input, plan, nullptr));
    assertReset(plan);
    ++rejections;
}
}

int main() {
    const std::array<Rgba, 8> edges{{{0, 0, 0, 0}, {255, 255, 255, 255}, {255, 0, 127, 1},
        {1, 254, 128, 127}, {17, 34, 51, 128}, {128, 127, 1, 254}, {254, 1, 255, 17}, {0, 255, 0, 255}}};
    for (unsigned mode = 0; mode < 4; ++mode) for (bool registerAlpha : {false, true}) {
        if (mode == 0 && registerAlpha) continue;
        for (const auto texture : edges) for (const auto raster : edges) for (const auto reg : edges) {
            auto input = packet(mode, registerAlpha);
            input.registers[2] = reg; input.registers[3] = reg;
            compare(input, texture, raster);
        }
    }
    std::mt19937 random(0x50494341);
    auto color = [&]() {
        return Rgba{std::uint8_t(random()), std::uint8_t(random()), std::uint8_t(random()), std::uint8_t(random())};
    };
    for (unsigned sample = 0; sample < 4096; ++sample) {
        const auto texture = color(), raster = color();
        for (unsigned mode = 0; mode < 4; ++mode) for (bool registerAlpha : {false, true}) {
            if (mode == 0 && registerAlpha) continue;
            auto input = packet(mode, registerAlpha);
            for (auto& reg : input.registers) reg = color();
            for (auto& reg : input.konst) reg = color();
            if (mode == 2) input.registers[1] = {};
            // Unused konst selectors do not affect these equations.
            for (auto& stage : input.tev) {
                stage.konstColor = std::uint8_t(random()); stage.konstAlpha = std::uint8_t(random());
            }
            compare(input, texture, raster);
        }
    }

    // Distinct constant values verify both register indexing and shared RGBA use.
    auto tint = packet(2, true);
    tint.registers[2] = {11, 33, 55, 77}; tint.registers[3] = {222, 111, 99, 137};
    PicaTevPlan plan{};
    assert(planPicaTev(tint, plan));
    assert(plan.stages[0].rgb.sources[0] == PicaTevSource::texture0);
    assert(plan.stages[0].alpha.sources[0] == PicaTevSource::constant);
    assert(plan.stages[0].constant.a == 137);
    assert(plan.stages[1].rgb.sources[1] == PicaTevSource::constant);
    assert(plan.stages[1].alpha.sources[1] == PicaTevSource::constant);
    assert(plan.stages[1].constant.r == 11 && plan.stages[1].constant.g == 33 &&
           plan.stages[1].constant.b == 55 && plan.stages[1].constant.a == 77);
    compare(tint, {77, 199, 47, 53}, {222, 111, 66, 88});

    for (unsigned count : {0u, 3u, 255u}) {
        auto input = packet(1); input.tevCount = count;
        reject(input, PicaTevError::stageCount);
    }
    auto noTextureTwo = packet(3); noTextureTwo.textured = false;
    reject(noTextureTwo, PicaTevError::stageCount);

    constexpr std::array<std::uint8_t TevOperation::*, 5> fields{
        &TevOperation::operation, &TevOperation::bias, &TevOperation::scale,
        &TevOperation::clamp, &TevOperation::output};
    for (unsigned mode = 0; mode < 4; ++mode) for (bool registerAlpha : {false, true}) {
        if (mode == 0 && registerAlpha) continue;
        const auto original = packet(mode, registerAlpha);
        for (unsigned s = 0; s < original.tevCount; ++s) {
            for (bool alpha : {false, true}) for (const auto field : fields) {
                auto input = original;
                auto& op = alpha ? input.tev[s].alphaOp : input.tev[s].colorOp;
                op.*field ^= 0x80;
                reject(input, PicaTevError::operation);
            }
            for (bool alpha : {false, true}) for (unsigned arg = 0; arg < 4; ++arg) {
                auto input = original;
                (alpha ? input.tev[s].alpha : input.tev[s].color)[arg] ^= 0x80;
                reject(input, PicaTevError::inputs);
            }
            for (auto field : {&TevStage::coordinate, &TevStage::texture, &TevStage::channel}) {
                auto input = original; input.tev[s].*field ^= 0x80;
                reject(input, PicaTevError::order);
            }
        }
    }
    for (auto field : {&Rgba::r, &Rgba::g, &Rgba::b, &Rgba::a}) {
        for (std::uint8_t value : {1, 128, 255}) {
            auto input = tint; input.registers[1].*field = value;
            reject(input, PicaTevError::nonzeroBlack);
        }
    }
    // Valid GX equations outside this deliberately bounded translation still reject.
    auto altered = packet(1); altered.tev[0].color[0] = 2; // REG0 RGB replace.
    reject(altered, PicaTevError::inputs);
    altered = packet(1); altered.tev[0].alpha[0] = 2; // REG1 alpha replace.
    reject(altered, PicaTevError::inputs);
    altered = packet(2); altered.tev[1].color[2] = 14; // KONST interpolation.
    reject(altered, PicaTevError::inputs);
    assert(picaTevErrorText(static_cast<PicaTevError>(255)));

    std::printf("PASS: %zu independent GX/PICA RGBA comparisons; %zu strict rejection cases.\n",
                comparisons, rejections);
    std::puts("Normalized real arithmetic only; no GX/PICA fixed-point rounding, GPU or hardware claim.");
}
