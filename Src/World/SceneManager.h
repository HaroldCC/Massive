/**
 * @file SceneManager.h
 * @brief 场景生命周期管理
 *
 * MVP 阶段只做常驻场景预创建：WorldServer 启动时根据配置加载所有常驻场景。
 * 副本/战场管理器后续按需添加。
 *
 * 每个 Scene 持有独立的 EnTT registry + AOI + ScriptComponentStorage。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"

namespace MMO
{

    /**
     * @brief 常驻场景配置（从 world.toml 读取）
     */
    struct SceneConfig
    {
        uint32       id   = 0;
        std::string  name;
        std::string  navmeshPath;          // 空字符串表示无 NavMesh
        float        gridSize     = 50.0f;
        float        viewRadiusXZ = 100.0f;
        float        viewRadiusY  = 15.0f;
        float        layerHeight  = 0.0f;  // 0 = 单层
    };

    /**
     * @brief 场景管理器
     *
     * 所有场景以 unique_ptr<Scene> 形式持有，key = sceneId。
     * WorldServer::Init 阶段调用 LoadPersistentScenes 预创建常驻场景。
     */
    class SceneManager
    {
    public:
        SceneManager() = default;

        /**
         * @brief 预创建所有常驻场景
         * @param configs  场景配置列表
         * @return true 全部创建成功
         */
        bool LoadPersistentScenes(const std::vector<SceneConfig> &configs);

        /**
         * @brief 获取场景
         * @param sceneId  Scene ID
         * @return Scene*，不存在返回 nullptr
         */
        ECS::Scene *GetScene(uint32 sceneId);

        /**
         * @brief 获取默认场景（第一个常驻场景）
         */
        ECS::Scene *GetDefaultScene()
        {
            if (_scenes.empty())
            {
                return nullptr;
            }
            return _scenes.begin()->second.get();
        }

        /**
         * @brief 创建新场景（副本/战场用）
         * @param config  场景配置
         * @return Scene*，创建失败返回 nullptr
         */
        ECS::Scene *CreateScene(const SceneConfig &config);

        /**
         * @brief 销毁场景
         * @param sceneId  要销毁的 Scene ID
         */
        void DestroyScene(uint32 sceneId);

        /**
         * @brief 场景数量
         */
        size_t Count() const
        {
            return _scenes.size();
        }

    private:
        std::unordered_map<uint32, std::unique_ptr<ECS::Scene>> _scenes;
    };

} // namespace MMO
