#pragma once
#include "sim/Renderer.hpp"
sim::Scene courtyard(const std::filesystem::path& asset = {});
sim::Scene primitives();
sim::PerspectiveCamera galleryCamera(float azimuth, float elevation, float distance);
