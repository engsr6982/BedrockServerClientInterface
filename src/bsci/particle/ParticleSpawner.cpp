#include "bsci/particle/ParticleSpawner.h"
#include "BedrockServerClientInterface.h"

#include <ll/api/memory/Hook.h>
#include <ll/api/memory/Memory.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/service/GamingStatus.h>
#include <ll/api/thread/ServerThreadExecutor.h>

#include <mc/deps/core/string/HashedString.h>
#include <mc/deps/core/threading/Threading.h>
#include <mc/legacy/ActorUniqueID.h>
#include <mc/network/packet/SpawnParticleEffectPacket.h>
#include <mc/platform/threading/AssignedThread.h>
#include <mc/util/MolangMemberArray.h>
#include <mc/util/MolangMemberVariable.h>
#include <mc/util/MolangScriptArg.h>
#include <mc/util/MolangVariable.h>
#include <mc/util/MolangVariableMap.h>
#include <mc/util/MolangStruct_RGBA.h>
#include <mc/util/MolangStruct_XY.h>
#include <mc/util/MolangStruct_XYZ.h>
#include <mc/util/MolangVariableSettings.h>
#include <mc/util/Timer.h>
#include <mc/world/Minecraft.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/util/MolangOffsetPlaceholder.h>

#include <parallel_hashmap/phmap.h>

template <class K, class V, size_t N = 4, class M = std::shared_mutex>
using ph_flat_hash_map = phmap::parallel_flat_hash_map<
    K,
    V,
    phmap::priv::hash_default_hash<K>,
    phmap::priv::hash_default_eq<K>,
    phmap::priv::Allocator<phmap::priv::Pair<const K, V>>,
    N,
    M>;

MolangVariableMap::MolangVariableMap(MolangVariableMap const& rhs) {
    mMapFromVariableIndexToVariableArrayOffset = rhs.mMapFromVariableIndexToVariableArrayOffset;
    mVariables                                 = {};
    for (auto& ptr : *rhs.mVariables) {
        mVariables->push_back(std::make_unique<MolangVariable>(*ptr));
    }
    mHasPublicVariables = rhs.mHasPublicVariables;
}

namespace bsci {
std::unique_ptr<GeometryGroup> GeometryGroup::createDefault() {
    return std::make_unique<ParticleSpawner>();
}

static void addTime(MolangVariableMap& var) {
    auto& config = BedrockServerClientInterface::getInstance().getConfig().particle;
    var.setMolangVariable(
        "variable.bsci_particle_lifetime",
        (float)(config.extraTime + ((64.0 / 20.0) / (double)config.tablePerTick))
    );
}
static void addSize(MolangVariableMap& var, Vec2 const& size) {
    var.setMolangVariable(
        "variable.bsci_particle_size",
        MolangMemberArray{MolangStruct_XY{}, size * 0.5f}
    );
}
static void addTint(MolangVariableMap& var, mce::Color const& tint) {
    var.setMolangVariable(
        "variable.bsci_particle_tint",
        MolangMemberArray{MolangStruct_RGBA{}, tint}
    );
}
static void addDirection(MolangVariableMap& var, Vec3 const& dir) {
    var.setMolangVariable(
        "variable.bsci_particle_direction",
        MolangMemberArray{MolangStruct_XYZ{}, dir}
    );
}
struct ParticleSpawner::Impl {
    struct Hook;
    size_t                                                                 id{};
    ph_flat_hash_map<GeoId, std::unique_ptr<SpawnParticleEffectPacket>, 6> geoPackets;
    ph_flat_hash_map<GeoId, std::vector<GeoId>>                            geoGroup;

    void sendParticleImmediately(SpawnParticleEffectPacket& pkt) {
        if (BedrockServerClientInterface::getInstance().getConfig().particle.delayUndate) {
            return;
        }
        ll::thread::ServerThreadExecutor::getDefault().execute([pkt] {
            pkt.sendTo(*pkt.mPos, pkt.mVanillaDimensionId);
        });
    }
};

static std::recursive_mutex          listMutex;
static std::vector<ParticleSpawner*> list;
static std::atomic_bool              hasInstance{false};
static size_t                        listtick;

LL_TYPE_INSTANCE_HOOK(
    ParticleSpawner::Impl::Hook,
    HookPriority::Normal,
    Minecraft,
    &Minecraft::update,
    bool
) {
    if (mSimTimer.mTicks && ll::getGamingStatus() == ll::GamingStatus::Running) {
        if (hasInstance) {
            std::lock_guard l{listMutex};
            listtick++;
            for (auto s : list) {
                s->tick({listtick});
            }
        }
    }
    return origin();
}

void ParticleSpawner::tick(Tick const& tick) {
    auto const tablePerTick =
        BedrockServerClientInterface::getInstance().getConfig().particle.tablePerTick;
    auto begin = (tick.tickID % (64 / tablePerTick)) * tablePerTick;
    for (size_t i = 0; i < tablePerTick; i++) {
        impl->geoPackets.with_submap(begin + i, [&](auto& map) {
            for (auto& [id, pkt] : map) {
                if (pkt)
                    pkt->sendTo(*pkt->mPos, pkt->mVanillaDimensionId); // tick must in server thread
            }
        });
    }
}

ParticleSpawner::ParticleSpawner() : impl(std::make_unique<Impl>()) {
    static ll::memory::HookRegistrar<ParticleSpawner::Impl::Hook> reg;
    std::lock_guard                                               l{listMutex};
    hasInstance = true;
    impl->id    = list.size();
    list.push_back(this);
}
ParticleSpawner::~ParticleSpawner() {
    std::lock_guard l{listMutex};
    list.back()->impl->id = impl->id;
    std::swap(list[impl->id], list.back());
    list.pop_back();
    hasInstance = !list.empty();
}

GeometryGroup::GeoId ParticleSpawner::particle(
    DimensionType      dim,
    Vec3 const&        pos,
    std::string const& name,
    MolangVariableMap  var
) const {
    addTime(var);
    auto packet =
        std::make_unique<SpawnParticleEffectPacket>(pos, name, (uchar)dim, std::move(var));
    impl->sendParticleImmediately(*packet);
    auto id = GeometryGroup::getNextGeoId();
    impl->geoPackets.try_emplace(id, std::move(packet));
    return id;
}

GeometryGroup::GeoId ParticleSpawner::line(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> thickness
) {
    if (begin == end) return {};
    MolangVariableMap var;
    addSize(
        var,
        {begin.distanceTo(end),
         thickness.value_or(
             BedrockServerClientInterface::getInstance().getConfig().particle.defaultThickness
         )}
    );
    addDirection(var, (end - begin).normalize());
    addTint(var, color);
    return particle(
        dim,
        (begin + end) * 0.5f,
        color.a == 1 ? "bsci:line" : "bsci:blend_line",
        std::move(var)
    );
}

GeometryGroup::GeoId ParticleSpawner::point(
    DimensionType        dim,
    Vec3 const&          pos,
    mce::Color const&    color,
    std::optional<float> radius
) {
    MolangVariableMap var;
    addSize(
        var,
        {radius.value_or(
            BedrockServerClientInterface::getInstance().getConfig().particle.defaultPointRadius
        )}
    );
    addTint(var, color);
    return particle(dim, pos, color.a == 1 ? "bsci:point" : "bsci:blend_point", std::move(var));
}

bool ParticleSpawner::remove(GeoId id) {
    if (id.value == 0) {
        return false;
    }
    if (!impl->geoGroup.erase_if(id, [this](auto&& iter) {
            for (auto& subId : iter.second) {
                impl->geoPackets.erase(subId);
            }
            return true;
        })) {
        return impl->geoPackets.erase(id);
    }
    return true;
}

GeometryGroup::GeoId ParticleSpawner::merge(std::span<GeoId> ids) {
    if (ids.empty()) {
        return {0};
    }
    auto               id = GeometryGroup::getNextGeoId();
    std::vector<GeoId> res;
    res.reserve(ids.size());
    for (auto const& sid : ids) {
        if (!impl->geoGroup.erase_if(sid, [this, &res](auto&& iter) {
                res.append_range(std::move(iter.second));
                return true;
            })) {
            res.push_back(sid);
        }
    }
    impl->geoGroup.try_emplace(id, std::move(res));
    return id;
}

bool ParticleSpawner::shift(GeoId id, Vec3 const& v) {
    if (!impl->geoGroup.modify_if(id, [this, &v](auto&& i) {
            for (auto& subId : i.second) {
                impl->geoPackets.modify_if(subId, [this, &v](auto&& iter) {
                    *iter.second->mPos += v;
                    if (iter.second) impl->sendParticleImmediately(*iter.second);
                });
            }
        })) {
        return impl->geoPackets.modify_if(id, [this, &v](auto&& iter) {
            *iter.second->mPos += v;
            if (iter.second) impl->sendParticleImmediately(*iter.second);
        });
    }
    return true;
}

} // namespace bsci
