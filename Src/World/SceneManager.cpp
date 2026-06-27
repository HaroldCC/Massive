/**
 * @file SceneManager.cpp
 * @brief SceneManager 实现
 */

#include "World/SceneManager.h"

namespace MMO
{

    bool SceneManager::LoadPersistentScenes(const std::vector<SceneConfig> &configs)
    {
        for (const auto &cfg : configs)
        {
            auto scene = std::make_unique<ECS::Scene>(cfg.id);
            _scenes[cfg.id] = std::move(scene);

            Log::Info("SceneManager: scene {} ({}) loaded", cfg.id, cfg.name);
        }

        if (_scenes.empty())
        {
            Log::Warn("SceneManager: no persistent scenes configured");
            return false;
        }

        Log::Info("SceneManager: {} persistent scenes loaded", _scenes.size());
        return true;
    }

    ECS::Scene *SceneManager::GetScene(uint32 sceneId)
    {
        auto it = _scenes.find(sceneId);
        if (it == _scenes.end())
        {
            return nullptr;
        }
        return it->second.get();
    }

    ECS::Scene *SceneManager::CreateScene(const SceneConfig &config)
    {
        if (_scenes.find(config.id) != _scenes.end())
        {
            Log::Warn("SceneManager: scene {} already exists", config.id);
            return nullptr;
        }

        auto scene = std::make_unique<ECS::Scene>(config.id);
        auto *ptr  = scene.get();
        _scenes[config.id] = std::move(scene);

        Log::Info("SceneManager: scene {} ({}) created dynamically", config.id, config.name);
        return ptr;
    }

    void SceneManager::DestroyScene(uint32 sceneId)
    {
        auto it = _scenes.find(sceneId);
        if (it == _scenes.end())
        {
            Log::Warn("SceneManager: scene {} not found", sceneId);
            return;
        }

        _scenes.erase(it);
        Log::Info("SceneManager: scene {} destroyed", sceneId);
    }

} // namespace MMO
