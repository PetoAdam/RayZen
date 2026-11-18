#ifndef RAYZEN_MATERIAL_SETTINGS_H
#define RAYZEN_MATERIAL_SETTINGS_H

#include <filesystem>

#include "Scene.h"

namespace rayzen {
bool loadMaterialSettings(const std::filesystem::path& path, Scene& scene);
bool saveMaterialSettings(const std::filesystem::path& path, const Scene& scene);
}

#endif
