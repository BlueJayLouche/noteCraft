// The CLAP side of every effect: one adapter, the factory and the entry functions. clap-wrapper builds the
// VST3 from the same code (src/entry.cpp exports the entry).
#include "effects.hpp"
#include "generators.hpp"
#include "timing.hpp"
#include "transforms.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace notecraft {
namespace {

const char* const features[] = {CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr};

// "C4" is key 60.
void keyName(int key, char* text, std::size_t capacity)
{
    std::snprintf(text, capacity, "%s%d", kNoteNames[key % 12], key / 12 - 1);
}

bool sameText(const char* a, const char* b)
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return *a == *b;
}

bool stepped(Unit u) { return u != Unit::Percent && u != Unit::Ms; }

template <class E>
class Plugin {
public:
    static constexpr std::size_t N = E::params.size();

    explicit Plugin(const clap_host_t* host) : host_(host)
    {
        plugin_.desc = &descriptor;
        plugin_.plugin_data = this;
        plugin_.init = [](const clap_plugin_t*) { return true; };
        plugin_.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        plugin_.activate = [](const clap_plugin_t* p, double rate, std::uint32_t, std::uint32_t) {
            self(p).engine_.activate(rate);
            return true;
        };
        plugin_.deactivate = [](const clap_plugin_t* p) { self(p).engine_.reset(); };
        plugin_.start_processing = [](const clap_plugin_t*) { return true; };
        plugin_.stop_processing = [](const clap_plugin_t*) {};
        plugin_.reset = [](const clap_plugin_t* p) { self(p).engine_.reset(); };
        plugin_.process = [](const clap_plugin_t* p, const clap_process_t* process) -> clap_process_status {
            auto& s = self(p);
            s.engine_.process(*process, s.values_);
            return CLAP_PROCESS_CONTINUE;
        };
        plugin_.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            if (id == nullptr) return nullptr;
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &paramsExt;
            if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &stateExt;
            if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePortsExt;
            return nullptr;
        };
        plugin_.on_main_thread = [](const clap_plugin_t*) {};
        for (std::size_t i = 0; i < N; ++i) {
            values_[i].store(E::params[i].defaultValue, std::memory_order_relaxed);
        }
    }

    const clap_plugin_t* clap() const { return &plugin_; }

    static inline const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT, E::id, E::name, "NoteCraft", "", "", "", "0.1.1", E::description, features,
    };

private:
    static Plugin& self(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }

    // Named functions, not lambdas: the MSVC of Visual Studio 18 misreads (and then crashes on) lambdas that initialize a static
    // member of a class template.
    static std::uint32_t paramsCount(const clap_plugin_t*) { return static_cast<std::uint32_t>(N); }

    static bool paramsInfo(const clap_plugin_t*, std::uint32_t index, clap_param_info_t* info)
    {
        if (index >= N) return false;
        const Param& p = E::params[index];
        *info = clap_param_info_t{};
        info->id = index;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | (stepped(p.unit) ? CLAP_PARAM_IS_STEPPED : 0) |
                      (p.unit == Unit::Choice ? CLAP_PARAM_IS_ENUM : 0);
        std::snprintf(info->name, sizeof info->name, "%s", p.name);
        info->min_value = p.min;
        info->max_value = p.max;
        info->default_value = p.defaultValue;
        return true;
    }

    static bool paramsValue(const clap_plugin_t* plugin, clap_id id, double* value)
    {
        if (id >= N) return false;
        *value = get(self(plugin).values_, id);
        return true;
    }

    static bool paramsValueToText(const clap_plugin_t*, clap_id id, double value, char* text, std::uint32_t capacity)
    {
        if (id >= N || capacity == 0 || !std::isfinite(value)) return false;
        const Param& p = E::params[id];
        const long step = std::lround(std::clamp(value, p.min, p.max));
        switch (p.unit) {
        case Unit::Percent: std::snprintf(text, capacity, "%.0f %%", value * 100.0); break;
        case Unit::Ms: std::snprintf(text, capacity, "%.1f ms", value); break;
        case Unit::Semitones: std::snprintf(text, capacity, p.min < 0.0 ? "%+ld st" : "%ld st", step); break;
        case Unit::Number: std::snprintf(text, capacity, "%ld", step); break;
        case Unit::Choice: std::snprintf(text, capacity, "%s", p.labels[step - std::lround(p.min)]); break;
        case Unit::Key: keyName(static_cast<int>(step), text, capacity); break;
        }
        return true;
    }

    static bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* value)
    {
        if (id >= N || text == nullptr) return false;
        const Param& p = E::params[id];
        if (p.unit == Unit::Choice || p.unit == Unit::Key) { // a label or a key name
            for (long i = std::lround(p.min); i <= std::lround(p.max); ++i) {
                char key[8] = "";
                if (p.unit == Unit::Key) keyName(static_cast<int>(i), key, sizeof key);
                if (sameText(p.unit == Unit::Choice ? p.labels[i - std::lround(p.min)] : key, text)) {
                    *value = static_cast<double>(i);
                    return true;
                }
            }
        }
        char* end = nullptr;
        const double v = std::strtod(text, &end);
        if (end == text || !std::isfinite(v)) return false;
        *value = std::clamp(p.unit == Unit::Percent ? v / 100.0 : v, p.min, p.max);
        return true;
    }

    static void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*)
    {
        const std::uint32_t count = in->size(in);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (const clap_event_header_t* h = in->get(in, i)) {
                applyParam(h, self(plugin).values_, E::params);
            }
        }
    }

    static inline const clap_plugin_params_t paramsExt = {
        &paramsCount, &paramsInfo, &paramsValue, &paramsValueToText, &paramsTextToValue, &paramsFlush,
    };

    // State: a count, then (id, value) pairs, native byte order (every target is little-endian).
    // Unknown ids are skipped and values clamped, so an older or newer state still loads.
    static constexpr std::size_t kStateMax = 4 + 64 * 12;

    static bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream)
    {
        unsigned char bytes[4 + N * 12];
        const auto count = static_cast<std::uint32_t>(N);
        std::memcpy(bytes, &count, 4);
        for (std::uint32_t i = 0; i < N; ++i) {
            const double v = get(self(plugin).values_, i);
            std::memcpy(bytes + 4 + i * 12, &i, 4);
            std::memcpy(bytes + 8 + i * 12, &v, 8);
        }
        std::size_t done = 0;
        while (done < sizeof bytes) {
            const std::int64_t n = stream->write(stream, bytes + done, sizeof bytes - done);
            if (n <= 0) return false;
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    static bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream)
    {
        unsigned char bytes[kStateMax];
        std::size_t size = 0;
        while (size < sizeof bytes) {
            const std::int64_t n = stream->read(stream, bytes + size, sizeof bytes - size);
            if (n < 0) return false;
            if (n == 0) break;
            size += static_cast<std::size_t>(n);
        }
        std::uint32_t count = 0;
        if (size < 4) return false;
        std::memcpy(&count, bytes, 4);
        if (count > 64 || size < 4 + std::size_t{count} * 12) return false;
        auto& values = self(plugin).values_;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t id = 0;
            double v = 0.0;
            std::memcpy(&id, bytes + 4 + i * 12, 4);
            std::memcpy(&v, bytes + 8 + i * 12, 8);
            if (id < N && std::isfinite(v)) {
                values[id].store(std::clamp(v, E::params[id].min, E::params[id].max), std::memory_order_relaxed);
            }
        }
        const clap_host_t* host = self(plugin).host_;
        const auto* hostParams = host != nullptr && host->get_extension != nullptr
            ? static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS))
            : nullptr;
        if (hostParams != nullptr && hostParams->rescan != nullptr) {
            hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        }
        return true;
    }

    static inline const clap_plugin_state_t stateExt = {&stateSave, &stateLoad};

    static std::uint32_t notePortsCount(const clap_plugin_t*, bool) { return 1; }

    static bool notePortsInfo(const clap_plugin_t*, std::uint32_t index, bool isInput, clap_note_port_info_t* info)
    {
        if (index != 0) return false;
        *info = clap_note_port_info_t{};
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof info->name, "%s", isInput ? "Notes in" : "Notes out");
        return true;
    }

    static inline const clap_plugin_note_ports_t notePortsExt = {&notePortsCount, &notePortsInfo};

    const clap_host_t* host_;
    clap_plugin_t plugin_{};
    Values<N> values_{};
    E engine_;
};

template <class E>
const clap_plugin_t* make(const clap_host_t* host)
{
    auto* p = new (std::nothrow) Plugin<E>(host);
    return p != nullptr ? p->clap() : nullptr;
}

template <class... E>
struct Registry {
    static inline const clap_plugin_descriptor_t* const descriptors[] = {&Plugin<E>::descriptor...};

    static const clap_plugin_t* create(const clap_host_t* host, const char* id)
    {
        const clap_plugin_t* made = nullptr;
        ((made == nullptr && std::strcmp(id, E::id) == 0 ? (made = make<E>(host), 0) : 0), ...);
        return made;
    }
};

using All = Registry<VelocityCurve, Humanize, Strum, NoteEcho, NoteLength, Scale, Chord, NoteFilter, RandomPitch,
                     Arpeggiator, Latch>;

const clap_plugin_factory_t factory = {
    [](const clap_plugin_factory_t*) { return static_cast<std::uint32_t>(std::size(All::descriptors)); },
    [](const clap_plugin_factory_t*, std::uint32_t index) -> const clap_plugin_descriptor_t* {
        return index < std::size(All::descriptors) ? All::descriptors[index] : nullptr;
    },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
        if (id == nullptr || host == nullptr || !clap_version_is_compatible(host->clap_version)) return nullptr;
        return All::create(host, id);
    },
};

} // namespace
} // namespace notecraft

bool notecraft_entry_init(const char*) { return true; }
void notecraft_entry_deinit() {}
const void* notecraft_entry_get_factory(const char* id)
{
    return id != nullptr && std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &notecraft::factory : nullptr;
}
