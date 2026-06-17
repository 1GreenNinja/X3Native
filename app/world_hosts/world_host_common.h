#pragma once
// Shared include surface for the extracted --world host TUs (#28 deep split).
// These are the engine + app headers the lifted host bodies reference. Kept in
// one place so every host TU has the identical environment the inline body had.

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include "../host_context.h"
#include "../world_hosts.h"

#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
