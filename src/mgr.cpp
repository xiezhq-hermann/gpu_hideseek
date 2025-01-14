#include "mgr.hpp"
#include "sim.hpp"

#include <madrona/utils.hpp>
#include <madrona/importer.hpp>
#include <madrona/physics_assets.hpp>
#include <madrona/tracing.hpp>
#include <madrona/mw_cpu.hpp>

#include <charconv>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef MADRONA_CUDA_SUPPORT
#include <madrona/mw_gpu.hpp>
#include <madrona/cuda_utils.hpp>
#endif

using namespace madrona;
using namespace madrona::math;
using namespace madrona::phys;
using namespace madrona::py;

namespace GPUHideSeek {

struct Manager::Impl {
    Config cfg;
    PhysicsLoader physicsLoader;
    EpisodeManager *episodeMgr;

    static inline Impl * init(const Config &cfg);
};

struct Manager::CPUImpl : Manager::Impl {
    using TaskGraphT =
        TaskGraphExecutor<Engine, Sim, GPUHideSeek::Config, WorldInit>;

    TaskGraphT cpuExec;
};

#ifdef MADRONA_CUDA_SUPPORT
struct Manager::CUDAImpl : Manager::Impl {
    MWCudaExecutor mwGPU;
};
#endif

static void loadPhysicsObjects(PhysicsLoader &loader)
{
    DynArray<RigidBodyMetadata> metadatas(0);
    DynArray<AABB> aabbs(0);
    DynArray<CollisionPrimitive> prims(0);

    { // Sphere: (0)
        metadatas.push_back({
            .invInertiaTensor = { 2.5f, 2.5f, 2.5f },
            .invMass = 1.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        aabbs.push_back({
            .pMin = { -1, -1, -1 },
            .pMax = { 1, 1, 1 },
        });

        prims.push_back({
            .type = CollisionPrimitive::Type::Sphere,
            .sphere = {
                .radius = 1.f,
            },
        });
    }

    { // Plane: (1)
        metadatas.push_back({
            .invInertiaTensor = { 0.f, 0.f, 0.f },
            .invMass = 0.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        aabbs.push_back({
            .pMin = { -FLT_MAX, -FLT_MAX, -FLT_MAX },
            .pMax = { FLT_MAX, FLT_MAX, 0 },
        });

        prims.push_back({
            .type = CollisionPrimitive::Type::Plane,
            .plane = {},
        });
    }

    { // Cube: (2)
        metadatas.push_back({
            .invInertiaTensor = { 1.5f, 1.5f, 1.5f },
            .invMass = 1.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        PhysicsLoader::LoadedHull cube_hull = loader.loadHullFromDisk(
            (std::filesystem::path(DATA_DIR) / "cube_collision.obj").c_str());

        aabbs.push_back(cube_hull.aabb);

        prims.push_back({
            .type = CollisionPrimitive::Type::Hull,
            .hull = {
                .halfEdgeMesh = cube_hull.collisionMesh,
            },
        });
    }

    { // Wall: (3)
        metadatas.push_back({
            .invInertiaTensor = { 0.f, 0.f, 0.f },
            .invMass = 0.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        PhysicsLoader::LoadedHull wall_hull = loader.loadHullFromDisk(
            (std::filesystem::path(DATA_DIR) / "wall_collision.obj").c_str());

        aabbs.push_back(wall_hull.aabb);

        prims.push_back({
            .type = CollisionPrimitive::Type::Hull,
            .hull = {
                .halfEdgeMesh = wall_hull.collisionMesh,
            },
        });
    }

    { // Cylinder: (4)
        metadatas.push_back({
            .invInertiaTensor = { 0.f, 0.f, 1.f }, // FIXME
            .invMass = 1.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        PhysicsLoader::LoadedHull cylinder_hull = loader.loadHullFromDisk(
            (std::filesystem::path(DATA_DIR) /
             "cylinder_collision.obj").c_str());

        aabbs.push_back(cylinder_hull.aabb);

        prims.push_back({
            .type = CollisionPrimitive::Type::Hull,
            .hull = {
                .halfEdgeMesh = cylinder_hull.collisionMesh,
            },
        });
    }

    { // Ramp (5)
        metadatas.push_back({
            .invInertiaTensor = { 1.5f, 1.5f, 1.5f }, // FIXME
            .invMass = 1.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        PhysicsLoader::LoadedHull ramp_hull = loader.loadHullFromDisk(
            (std::filesystem::path(DATA_DIR) /
             "ramp_collision.obj").c_str());

        aabbs.push_back(ramp_hull.aabb);

        prims.push_back({
            .type = CollisionPrimitive::Type::Hull,
            .hull = {
                .halfEdgeMesh = ramp_hull.collisionMesh,
            },
        });
    }

    { // Elongated box (6)
        float width = 8;
        float height = 2;
        float depth = 1.5;

        metadatas.push_back({
            .invInertiaTensor = 12.f / Vector3 {
                height * height + depth * depth,
                height * height  + width * width,
                width * width + depth * depth,
            },
            .invMass = 1.f,
            .muS = 0.5f,
            .muD = 0.5f,
        });

        PhysicsLoader::LoadedHull elongated_box_hull = loader.loadHullFromDisk(
            (std::filesystem::path(DATA_DIR) /
             "elongated_collision.obj").c_str());

        aabbs.push_back(elongated_box_hull.aabb);

        prims.push_back({
            .type = CollisionPrimitive::Type::Hull,
            .hull = {
                .halfEdgeMesh = elongated_box_hull.collisionMesh,
            },
        });
    }

    loader.loadObjects(metadatas.data(), aabbs.data(),
                       prims.data(), metadatas.size());
}

Manager::Impl * Manager::Impl::init(const Config &cfg)
{
    HostEventLogging(HostEvent::initStart);
    DynArray<imp::ImportedObject> imported_renderer_objs(0);

    {
        auto sphere_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "sphere.obj").c_str());

        if (!sphere_obj.has_value()) {
            FATAL("Failed to load sphere");
        }

        imported_renderer_objs.emplace_back(std::move(*sphere_obj));
    }

    {
        auto plane_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "plane.obj").c_str());

        if (!plane_obj.has_value()) {
            FATAL("Failed to load plane");
        }

        imported_renderer_objs.emplace_back(std::move(*plane_obj));
    }

    {
        auto cube_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "cube_render.obj").c_str());

        if (!cube_obj.has_value()) {
            FATAL("Failed to load cube");
        }

        imported_renderer_objs.emplace_back(std::move(*cube_obj));
    }

    {
        auto wall_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "wall_render.obj").c_str());

        if (!wall_obj.has_value()) {
            FATAL("Failed to load wall");
        }

        imported_renderer_objs.emplace_back(std::move(*wall_obj));
    }

    {
        auto cylinder_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "cylinder_render.obj").c_str());

        if (!cylinder_obj.has_value()) {
            FATAL("Failed to load cylinder");
        }

        imported_renderer_objs.emplace_back(std::move(*cylinder_obj));
    }

    {
        auto ramp_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "ramp_render.obj").c_str());

        if (!ramp_obj.has_value()) {
            FATAL("Failed to load ramp");
        }

        imported_renderer_objs.emplace_back(std::move(*ramp_obj));
    }

    {
        auto ramp_obj = imp::ImportedObject::importObject(
            (std::filesystem::path(DATA_DIR) / "elongated_render.obj").c_str());

        if (!ramp_obj.has_value()) {
            FATAL("Failed to load elongated");
        }

        imported_renderer_objs.emplace_back(std::move(*ramp_obj));
    }

    DynArray<imp::SourceObject> renderer_objects(0);

    for (const auto &imported_obj : imported_renderer_objs) {
        renderer_objects.push_back(imp::SourceObject {
            imported_obj.meshes,
        });
    }

    GPUHideSeek::Config app_cfg {
        cfg.enableRender,
    };

    switch (cfg.execMode) {
    case ExecMode::CUDA: {
#ifdef MADRONA_CUDA_SUPPORT
        EpisodeManager *episode_mgr = 
            (EpisodeManager *)cu::allocGPU(sizeof(EpisodeManager));
        REQ_CUDA(cudaMemset(episode_mgr, 0, sizeof(EpisodeManager)));

        PhysicsLoader phys_loader(PhysicsLoader::StorageType::CUDA, 10);
        loadPhysicsObjects(phys_loader);

        ObjectManager *phys_obj_mgr = &phys_loader.getObjectManager();

        HeapArray<WorldInit> world_inits(cfg.numWorlds);

        for (int64_t i = 0; i < (int64_t)cfg.numWorlds; i++) {
            world_inits[i] = WorldInit {
                episode_mgr,
                phys_obj_mgr,
                cfg.minEntitiesPerWorld,
                cfg.maxEntitiesPerWorld,
            };
        }

        MWCudaExecutor mwgpu_exec({
            .worldInitPtr = world_inits.data(),
            .numWorldInitBytes = sizeof(WorldInit),
            .userConfigPtr = &app_cfg,
            .numUserConfigBytes = sizeof(GPUHideSeek::Config),
            .numWorldDataBytes = sizeof(Sim),
            .worldDataAlignment = alignof(Sim),
            .numWorlds = cfg.numWorlds,
            .maxViewsPerWorld = consts::maxAgents,
            .numExportedBuffers = 16,
            .gpuID = (uint32_t)cfg.gpuID,
            .cameraMode = cfg.enableRender ? 
                render::CameraMode::Perspective :
                render::CameraMode::None,
            .renderWidth = cfg.renderWidth,
            .renderHeight = cfg.renderHeight,
        }, {
            "",
            { GPU_HIDESEEK_SRC_LIST },
            { GPU_HIDESEEK_COMPILE_FLAGS },
            cfg.debugCompile ? CompileConfig::OptMode::Debug :
                CompileConfig::OptMode::LTO,
            CompileConfig::Executor::TaskGraph,
        });

        if (cfg.enableRender) {
            mwgpu_exec.loadObjects(renderer_objects);
        }

        HostEventLogging(HostEvent::initEnd);
        return new CUDAImpl {
            { 
                cfg,
                std::move(phys_loader),
                episode_mgr,
            },
            std::move(mwgpu_exec),
        };
#else
        FATAL("Madrona was not compiled with CUDA support");
#endif
    } break;
    case ExecMode::CPU: {
        EpisodeManager *episode_mgr = new EpisodeManager {};

        PhysicsLoader phys_loader(PhysicsLoader::StorageType::CPU, 10);
        loadPhysicsObjects(phys_loader);

        ObjectManager *phys_obj_mgr = &phys_loader.getObjectManager();

        HeapArray<WorldInit> world_inits(cfg.numWorlds);

        for (int64_t i = 0; i < (int64_t)cfg.numWorlds; i++) {
            world_inits[i] = WorldInit {
                episode_mgr,
                phys_obj_mgr,
                cfg.minEntitiesPerWorld,
                cfg.maxEntitiesPerWorld,
            };
        }

        auto cpu_impl = new CPUImpl {
            { 
                cfg,
                std::move(phys_loader),
                episode_mgr,
            },
            CPUImpl::TaskGraphT {
                ThreadPoolExecutor::Config {
                    .numWorlds = cfg.numWorlds,
                    .maxViewsPerWorld = consts::maxAgents,
                    .maxInstancesPerWorld = 1024,
                    .renderWidth = cfg.renderWidth,
                    .renderHeight = cfg.renderHeight,
                    .maxObjects = 50,
                    .numExportedBuffers = 16,
                    .cameraMode = cfg.enableRender ?
                        render::CameraMode::Perspective :
                        render::CameraMode::None,
                    .renderGPUID = cfg.gpuID,
                },
                app_cfg,
                world_inits.data()
            },
        };

        if (cfg.enableRender) {
            cpu_impl->cpuExec.loadObjects(renderer_objects);
        }

        HostEventLogging(HostEvent::initEnd);

        return cpu_impl;
    } break;
    default: __builtin_unreachable();
    }
}

MADRONA_EXPORT Manager::Manager(const Config &cfg)
    : impl_(Impl::init(cfg))
{}

MADRONA_EXPORT Manager::~Manager() {
    switch (impl_->cfg.execMode) {
    case ExecMode::CUDA: {
#ifdef MADRONA_CUDA_SUPPORT
        delete static_cast<CUDAImpl *>(impl_);
#endif
    } break;
    case ExecMode::CPU : {
        delete static_cast<CPUImpl *>(impl_);
    } break;
    }
#ifdef MADRONA_TRACING
    FinalizeLogging("/tmp/");
#endif
}

MADRONA_EXPORT void Manager::step()
{
    switch (impl_->cfg.execMode) {
    case ExecMode::CUDA: {
#ifdef MADRONA_CUDA_SUPPORT
        static_cast<CUDAImpl *>(impl_)->mwGPU.run();
#endif
    } break;
    case ExecMode::CPU: {
        static_cast<CPUImpl *>(impl_)->cpuExec.run();
    } break;
    }
}

MADRONA_EXPORT Tensor Manager::resetTensor() const
{
    return exportStateTensor(0, Tensor::ElementType::Int32,
                             {impl_->cfg.numWorlds, 3});
}

MADRONA_EXPORT Tensor Manager::doneTensor() const
{
    return exportStateTensor(1, Tensor::ElementType::Int32,
                             {impl_->cfg.numWorlds, 1});
}

MADRONA_EXPORT madrona::py::Tensor Manager::prepCounterTensor() const
{
    return exportStateTensor(2, Tensor::ElementType::Int32,
                             {impl_->cfg.numWorlds, 1});
}

MADRONA_EXPORT Tensor Manager::actionTensor() const
{
    return exportStateTensor(3, Tensor::ElementType::Int32,
                             {impl_->cfg.numWorlds, consts::maxAgents, 5});
}

MADRONA_EXPORT Tensor Manager::rewardTensor() const
{
    return exportStateTensor(4, Tensor::ElementType::Float32,
                             {impl_->cfg.numWorlds, consts::maxAgents, 1});
}

MADRONA_EXPORT Tensor Manager::agentTypeTensor() const
{
    return exportStateTensor(5, Tensor::ElementType::Int32,
                             {impl_->cfg.numWorlds, consts::maxAgents, 1});
}

MADRONA_EXPORT Tensor Manager::agentMaskTensor() const
{
    return exportStateTensor(6, Tensor::ElementType::Float32,
                             {impl_->cfg.numWorlds, consts::maxAgents, 1});
}


MADRONA_EXPORT madrona::py::Tensor Manager::agentDataTensor() const
{
    return exportStateTensor(7, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxAgents - 1,
                                 4,
                             });
}

MADRONA_EXPORT madrona::py::Tensor Manager::boxDataTensor() const
{
    return exportStateTensor(8, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxBoxes,
                                 7,
                             });
}

MADRONA_EXPORT madrona::py::Tensor Manager::rampDataTensor() const
{
    return exportStateTensor(9, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxRamps,
                                 5,
                             });
}

MADRONA_EXPORT madrona::py::Tensor Manager::visibleAgentsMaskTensor() const
{
    return exportStateTensor(10, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxAgents - 1,
                                 1,
                             });
}

MADRONA_EXPORT madrona::py::Tensor Manager::visibleBoxesMaskTensor() const
{
    return exportStateTensor(11, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxBoxes,
                                 1,
                             });
}

MADRONA_EXPORT madrona::py::Tensor Manager::visibleRampsMaskTensor() const
{
    return exportStateTensor(12, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 consts::maxRamps,
                                 1,
                             });
}

MADRONA_IMPORT madrona::py::Tensor Manager::globalPositionsTensor() const
{
    return exportStateTensor(13, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxBoxes + consts::maxRamps +
                                     consts::maxAgents,
                                 2,
                             });
}

MADRONA_EXPORT Tensor Manager::depthTensor() const
{
    void *dev_ptr = nullptr;
    Optional<int> gpu_id = impl_->cfg.gpuID;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        dev_ptr = static_cast<CUDAImpl *>(impl_)->mwGPU.
            depthObservations();
#endif
    } else {
        dev_ptr = static_cast<CPUImpl *>(impl_)->cpuExec.
            depthObservations();
    }

    return Tensor(dev_ptr, Tensor::ElementType::Float32,
                     {impl_->cfg.numWorlds, consts::maxAgents,
                      impl_->cfg.renderHeight,
                      impl_->cfg.renderWidth, 1}, gpu_id);
}

MADRONA_EXPORT Tensor Manager::rgbTensor() const
{
    void *dev_ptr = nullptr;
    Optional<int> gpu_id = impl_->cfg.gpuID;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        dev_ptr = static_cast<CUDAImpl *>(impl_)->mwGPU.
            rgbObservations();
#endif
    } else {
        dev_ptr = static_cast<CPUImpl *>(impl_)->cpuExec.
            rgbObservations();
    }

    return Tensor(dev_ptr, Tensor::ElementType::UInt8,
                  {impl_->cfg.numWorlds, consts::maxAgents,
                   impl_->cfg.renderHeight,
                   impl_->cfg.renderWidth, 4}, gpu_id);
}


MADRONA_EXPORT madrona::py::Tensor Manager::lidarTensor() const
{
    return exportStateTensor(14, Tensor::ElementType::Float32,
                             {
                                 impl_->cfg.numWorlds,
                                 consts::maxAgents,
                                 30,
                             });
}

Tensor Manager::exportStateTensor(int64_t slot,
                                  Tensor::ElementType type,
                                  Span<const int64_t> dimensions) const
{
    void *dev_ptr = nullptr;
    Optional<int> gpu_id = Optional<int>::none();
    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        dev_ptr =
            static_cast<CUDAImpl *>(impl_)->mwGPU.getExported(slot);
        gpu_id = impl_->cfg.gpuID;
#endif
    } else {
        dev_ptr = static_cast<CPUImpl *>(impl_)->cpuExec.getExported(slot);
    }

    return Tensor(dev_ptr, type, dimensions, gpu_id);
}


}
