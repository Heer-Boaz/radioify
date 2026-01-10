#pragma once

#include <mutex>

struct ID3D11Device;
class GpuAsciiRenderer;

GpuAsciiRenderer& sharedGpuRenderer();
std::recursive_mutex& getSharedGpuMutex();
ID3D11Device* getSharedGpuDevice();
