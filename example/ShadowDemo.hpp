#pragma once
#include "sim/Renderer.hpp"
#include <array>
#include <iosfwd>

enum class ShadowPreset { Colonnade, ContactStudy, OffscreenCaster };
const char* shadowPresetName(ShadowPreset);

struct ShadowScene
{
    sim::Scene scene;
    uint32_t movingCaster{};
    explicit ShadowScene(ShadowPreset);
    float motionAmplitude = 1.8f;
    sim::SceneSnapshot snapshot(unsigned frame, bool moving);
};

sim::PerspectiveCamera shadowCamera(ShadowPreset, float offset = 0);
struct ShadowTiming
{
    double first{}, median{}, p95{}, submitMedian{};
};
using ShadowTimings = std::array<std::array<ShadowTiming, 3>, 2>;
ShadowTimings measureShadows(sim::Renderer&, ShadowPreset, sim::RenderOptions);
void printShadowTimings(std::ostream&, const ShadowTimings&, bool rayQuery);
