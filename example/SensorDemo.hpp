#pragma once
#include "sim/Renderer.hpp"

class SensorDemo
{
  public:
    explicit SensorDemo(sim::Renderer&);
    ~SensorDemo();
    void draw(bool smoke, unsigned frame);
    sim::Frame capture();
    void verify();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
