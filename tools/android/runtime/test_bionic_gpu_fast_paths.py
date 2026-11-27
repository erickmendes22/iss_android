"""Exercise production GPU page tracking across readback modes and empty regions."""
from pathlib import Path
import unittest
import test_bionic_resource_reuse as reuse
from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]


class GpuFastPathTests(unittest.TestCase):
    def test_gpu_dirty_summary_tracks_partial_clears_in_all_readback_modes(self):
        source = (ROOT / 'port/gpu/shadps4/video_core/buffer_cache/region_manager.h').read_text()
        bodies = '\n'.join(method(source, signature) for signature in (
            '    template <Type type, bool enable>\n    void ChangeRegionState(',
            '    template <Type type>\n    [[nodiscard]] bool IsRegionModified(',
            '    void NoteGpuBitsSet()', '    void NoteGpuBitsCleared()'))
        reuse.ResourceReuseTests().run_cpp(r'''
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdint>
using u64=uint64_t;using s64=int64_t;
#define RENDERER_TRACE
namespace Common {u64 DivCeil(u64 a,u64 b){return (a+b-1)/b;}}
enum class Type{CPU,GPU};
enum class GpuReadbacksMode{Disabled,Relaxed,Precise};
struct {GpuReadbacksMode mode;auto GetReadbacksMode(){return mode;}} EmulatorSettings;
struct RegionBits {
    std::bitset<32> bits;
    mutable unsigned scans=0;
    void SetRange(size_t a,size_t b){for(;a<b;++a)bits.set(a);}
    void UnsetRange(size_t a,size_t b){for(;a<b;++a)bits.reset(a);}
    bool AllInRange(size_t a,size_t b)const{for(;a<b;++a)if(!bits[a])return false;return true;}
    bool Any()const{return bits.any();}
    bool AnyInRange(size_t a,size_t b)const{++scans;for(;a<b;++a)if(bits[a])return true;return false;}
};
struct RegionManager {
    static constexpr size_t TRACKER_BYTES_PER_PAGE=4096,NUM_PAGES_PER_REGION=32;
    u64 cpu_addr=0;
    RegionBits cpu,gpu;
    std::atomic<bool> cpu_any{true},gpu_any{false};
    unsigned protections=0;
    static size_t SanitizeAddress(size_t a){return size_t(std::max<s64>(s64(a),0));}
    template<Type t> auto& GetRegionBits(){if constexpr(t==Type::GPU)return gpu;else return cpu;}
    template<Type t> const auto& GetRegionBits()const{if constexpr(t==Type::GPU)return gpu;else return cpu;}
    void CountWriteFaults(size_t,size_t){}void NoteCpuBitsSet(){}void NoteCpuBitsCleared(){}
    static inline bool hybrid_read_protection=false;
    static bool HybridReadProtectionEnabled(){return hybrid_read_protection && EmulatorSettings.GetReadbacksMode()==GpuReadbacksMode::Relaxed;}
    template<bool,bool> void UpdateProtection(){++protections;}
''' + bodies + r'''
};
int main(){
    for(auto mode:{GpuReadbacksMode::Disabled,GpuReadbacksMode::Relaxed,GpuReadbacksMode::Precise}){
        EmulatorSettings.mode=mode;
        RegionManager r;
        assert(!r.IsRegionModified<Type::GPU>(0,4096));assert(r.gpu.scans==0);
        r.ChangeRegionState<Type::GPU,true>(4096,8192);
        assert(r.IsRegionModified<Type::GPU>(4096,4096));
        assert(!r.IsRegionModified<Type::GPU>(0,4096));
        r.ChangeRegionState<Type::GPU,false>(4096,4096);
        assert(r.gpu_any.load());assert(r.IsRegionModified<Type::GPU>(8192,4096));
        r.ChangeRegionState<Type::GPU,false>(8192,4096);
        assert(!r.gpu_any.load());auto scans=r.gpu.scans;
        assert(!r.IsRegionModified<Type::GPU>(0,32*4096));assert(r.gpu.scans==scans);
        r.ChangeRegionState<Type::GPU,true>(31*4096,4096);
        assert(r.IsRegionModified<Type::GPU>(31*4096,4096));
        r.ChangeRegionState<Type::GPU,false>(31*4096,4096);
        assert(!r.gpu_any.load());
        assert(r.protections==(mode==GpuReadbacksMode::Precise?5u:0u));
    }
    // The experimental hybrid protects GPU-dirty reads without switching the mode
    // or disabling Relaxed's asynchronous readback publication.
    EmulatorSettings.mode=GpuReadbacksMode::Relaxed;
    RegionManager::hybrid_read_protection=true;
    RegionManager hybrid;
    hybrid.ChangeRegionState<Type::GPU,true>(4096,8192);
    hybrid.ChangeRegionState<Type::GPU,true>(4096,8192);
    hybrid.ChangeRegionState<Type::GPU,false>(4096,8192);
    hybrid.ChangeRegionState<Type::GPU,false>(4096,8192);
    assert(hybrid.protections==2u);
    RegionManager::hybrid_read_protection=false;
}
''')


if __name__ == '__main__':
    unittest.main()
