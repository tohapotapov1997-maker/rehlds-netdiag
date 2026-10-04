#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <dlfcn.h>

#include "extdll.h"
#include "meta_api.h"
#include "rehlds_api.h"
#include "rehlds_interfaces.h"
#include "com_delta_packet.h"
#include "IMessageManager.h"

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = nullptr;
meta_globals_t *gpMetaGlobals = nullptr;
gamedll_funcs_t *gpGamedllFuncs = nullptr;
mutil_funcs_t *gpMetaUtilFuncs = nullptr;

IRehldsApi *g_RehldsApi = nullptr;
const RehldsFuncs_t *g_RehldsFuncs = nullptr;
IRehldsHookchains *g_RehldsHookchains = nullptr;
IRehldsServerStatic *g_RehldsSvs = nullptr;
IMessageManager *g_MessageManager = nullptr;

static FILE *g_log = nullptr;

struct ClientSnap
{
    int packet_entities = 0;
    int msg_before = 0;
    int msg_after = 0;
    int msg_delta = 0;
    int packet_type = 0;
};

static ClientSnap g_snap[33];

enum TraceKind
{
    TRACE_MESSAGE = 1,
    TRACE_ENGINE_GAP = 2,
    TRACE_SOURCE = 3
};

enum TraceSource
{
    SOURCE_NONE = 0,
    SOURCE_NETCMD = 1,
    SOURCE_START_SOUND = 2,
    SOURCE_EMIT_SOUND2 = 3,
    SOURCE_RELIABLE_EVENT = 4,
    SOURCE_BUILD_SOUND_MSG = 5
};

static int g_trace_source = SOURCE_NONE;
static int g_trace_source_arg = 0;
static int g_trace_source_ref = 0;
static int g_trace_source_extra = 0;

static const int SOUND_SAMPLE_MAX = 512;
static const int SOUND_SAMPLE_LEN = 96;
static char g_sound_samples[SOUND_SAMPLE_MAX][SOUND_SAMPLE_LEN];
static int g_sound_sample_count = 1; // 0 = unknown / empty

static int intern_sound_sample(const char *sample)
{
    if (!sample || !sample[0])
        return 0;

    for (int i = 1; i < g_sound_sample_count; ++i)
    {
        if (std::strncmp(g_sound_samples[i], sample, SOUND_SAMPLE_LEN) == 0)
            return i;
    }

    if (g_sound_sample_count >= SOUND_SAMPLE_MAX)
        return 0;

    const int id = g_sound_sample_count++;
    std::strncpy(g_sound_samples[id], sample, SOUND_SAMPLE_LEN - 1);
    g_sound_samples[id][SOUND_SAMPLE_LEN - 1] = '\0';
    return id;
}

static const char *sound_sample_name(int id)
{
    if (id <= 0 || id >= g_sound_sample_count)
        return "";
    return g_sound_samples[id];
}

static const int SOUND_GUARD_HEADROOM = 800;
static unsigned int g_soundguard_skipped_calls[33] = {};
static unsigned int g_soundguard_skipped_bytes[33] = {};
static std::time_t g_soundguard_last_log[33] = {};

static bool is_footstep_sample(const char *sample)
{
    if (!sample)
        return false;

    return std::strcmp(sample, "player/pl_step1.wav") == 0 ||
           std::strcmp(sample, "player/pl_step2.wav") == 0 ||
           std::strcmp(sample, "player/pl_step3.wav") == 0 ||
           std::strcmp(sample, "player/pl_step4.wav") == 0;
}

static const char *trace_source_name(int source, int arg)
{
    if (source == SOURCE_NETCMD)
        return arg == 8 ? "NETCMD_VOICE" : "NETCMD";
    if (source == SOURCE_START_SOUND)
        return "SV_START_SOUND";
    if (source == SOURCE_EMIT_SOUND2)
        return "SV_EMIT_SOUND2";
    if (source == SOURCE_RELIABLE_EVENT)
        return "EV_RELIABLE_EVENT";
    if (source == SOURCE_BUILD_SOUND_MSG)
        return "PF_BUILD_SOUND_MSG";
    return "UNKNOWN";
}

static const int TRACE_CAP = 2048;
static const int TRACE_CYCLES = 8;

struct TraceEntry
{
    unsigned int seq = 0;
    int kind = 0;
    int msg_id = -1;
    int dest = -1;
    int params = 0;
    int before = 0;
    int after = 0;
    int delta = 0;
    int source = SOURCE_NONE;
    int source_arg = 0;
    int source_ref = 0;
    int source_extra = 0;

    int msg_recipient = -1;
    int msg_p0_type = -1;
    int msg_p0_int = 0;
    int msg_p1_type = -1;
    int msg_p1_int = 0;
};

struct TraceCycle
{
    TraceEntry entries[TRACE_CAP];
    int head = 0;
    int count = 0;
    int start_seen = -1;
    int last_seen = -1;
    int high_water = -1;
    int total_msg_bytes = 0;
    int total_gap_bytes = 0;
    int total_source_bytes = 0;
    unsigned int cycle_no = 0;
};

struct ClientTrace
{
    TraceCycle cycles[TRACE_CYCLES];
    int active = 0;
    unsigned int next_cycle_no = 1;
};

static ClientTrace g_trace[33];
static unsigned int g_trace_seq = 0;

// Per-client per-datagram-cycle message flood diagnostics.
// This is intentionally diagnostics-only: it never blocks user messages.
static const int MSG_FLOOD_SLOTS = 32;

struct MsgFloodEntry
{
    bool used = false;
    int msg_id = -1;
    int dest = -1;
    int recipient = -1;
    int params = 0;
    int p0_type = -1;
    int p0_int = 0;
    int p1_type = -1;
    int p1_int = 0;
    unsigned int calls = 0;
    unsigned int bytes = 0;
};

struct MsgFloodClient
{
    unsigned int cycle_no = 0;
    MsgFloodEntry entries[MSG_FLOOD_SLOTS];
};

static MsgFloodClient g_msg_flood[33];

// Forward declaration: msg_flood_track() logs before the full definition below.
static void log_line(const char *fmt, ...);

// Resolve protocol service IDs and registered user-message IDs to readable names.
// GoldSrc service IDs occupy 0..63; user messages start at 64 on ReHLDS.
static const char *message_name(int msg_id)
{
    static const char *svc_names[64] = {
        "svc_bad", "svc_nop", "svc_disconnect", "svc_event",
        "svc_version", "svc_setview", "svc_sound", "svc_time",
        "svc_print", "svc_stufftext", "svc_setangle", "svc_serverinfo",
        "svc_lightstyle", "svc_updateuserinfo", "svc_deltadescription", "svc_clientdata",
        "svc_stopsound", "svc_pings", "svc_particle", "svc_damage",
        "svc_spawnstatic", "svc_event_reliable", "svc_spawnbaseline", "svc_temp_entity",
        "svc_setpause", "svc_signonnum", "svc_centerprint", "svc_killedmonster",
        "svc_foundsecret", "svc_spawnstaticsound", "svc_intermission", "svc_finale",
        "svc_cdtrack", "svc_restore", "svc_cutscene", "svc_weaponanim",
        "svc_decalname", "svc_roomtype", "svc_addangle", "svc_newusermsg",
        "svc_packetentities", "svc_deltapacketentities", "svc_choke", "svc_resourcelist",
        "svc_newmovevars", "svc_resourcerequest", "svc_customization", "svc_crosshairangle",
        "svc_soundfade", "svc_filetxferfailed", "svc_hltv", "svc_director",
        "svc_voiceinit", "svc_voicedata", "svc_sendextrainfo", "svc_timescale",
        "svc_resourcelocation", "svc_sendcvarvalue", "svc_sendcvarvalue2", "svc_exec",
        "svc_reserve60", "svc_reserve61", "svc_reserve62", "svc_reserve63"
    };

    if (msg_id >= 0 && msg_id < 64)
        return svc_names[msg_id];

    if (gpMetaUtilFuncs && gpMetaUtilFuncs->pfnGetUserMsgName)
    {
        int size = 0;
        const char *name = gpMetaUtilFuncs->pfnGetUserMsgName(PLID, msg_id, &size);
        if (name && name[0])
            return name;
    }

    return "unknown";
}

static const char *message_origin_hint(int msg_id)
{
    switch (msg_id)
    {
        case 17: return "ReHLDS:SV_EmitPings";
        case 23: return "GoldSrc:TempEntity producer (GameDLL/AMXX/engine)";
        default:
            return (msg_id >= 64) ? "registered user message (GameDLL/AMXX/plugin)"
                                  : "GoldSrc/ReHLDS service message";
    }
}

static const char *temp_entity_name(int code)
{
    switch (code)
    {
        case 0: return "TE_BEAMPOINTS";
        case 1: return "TE_BEAMENTPOINT";
        case 2: return "TE_GUNSHOT";
        case 3: return "TE_EXPLOSION";
        case 4: return "TE_TAREXPLOSION";
        case 5: return "TE_SMOKE";
        case 6: return "TE_TRACER";
        case 7: return "TE_LIGHTNING";
        case 8: return "TE_BEAMENTS";
        case 9: return "TE_SPARKS";
        case 10: return "TE_LAVASPLASH";
        case 11: return "TE_TELEPORT";
        case 12: return "TE_EXPLOSION2";
        case 13: return "TE_BSPDECAL";
        case 14: return "TE_IMPLOSION";
        case 15: return "TE_SPRITETRAIL";
        case 17: return "TE_SPRITE";
        case 18: return "TE_BEAMSPRITE";
        case 19: return "TE_BEAMTORUS";
        case 20: return "TE_BEAMDISK";
        case 21: return "TE_BEAMCYLINDER";
        case 22: return "TE_BEAMFOLLOW";
        case 23: return "TE_GLOWSPRITE";
        case 24: return "TE_BEAMRING";
        case 25: return "TE_STREAK_SPLASH";
        case 27: return "TE_DLIGHT";
        case 28: return "TE_ELIGHT";
        case 29: return "TE_TEXTMESSAGE";
        case 30: return "TE_LINE";
        case 31: return "TE_BOX";
        case 99: return "TE_KILLBEAM";
        case 100: return "TE_LARGEFUNNEL";
        case 101: return "TE_BLOODSTREAM";
        case 102: return "TE_SHOWLINE";
        case 103: return "TE_BLOOD";
        case 104: return "TE_DECAL";
        case 105: return "TE_FIZZ";
        case 106: return "TE_MODEL";
        case 107: return "TE_EXPLODEMODEL";
        case 108: return "TE_BREAKMODEL";
        case 109: return "TE_GUNSHOTDECAL";
        case 110: return "TE_SPRITE_SPRAY";
        case 111: return "TE_ARMOR_RICOCHET";
        case 112: return "TE_PLAYERDECAL";
        case 113: return "TE_BUBBLES";
        case 114: return "TE_BUBBLETRAIL";
        case 115: return "TE_BLOODSPRITE";
        case 116: return "TE_WORLDDECAL";
        case 117: return "TE_WORLDDECALHIGH";
        case 118: return "TE_DECALHIGH";
        case 119: return "TE_PROJECTILE";
        case 120: return "TE_SPRAY";
        case 121: return "TE_PLAYERSPRITES";
        case 122: return "TE_PARTICLEBURST";
        case 123: return "TE_FIREFIELD";
        case 124: return "TE_PLAYERATTACHMENT";
        case 125: return "TE_KILLPLAYERATTACHMENTS";
        case 126: return "TE_MULTIGUNSHOT";
        case 127: return "TE_USERTRACER";
        default: return "";
    }
}

static bool msg_flood_log_point(unsigned int calls)
{
    return calls == 25 || calls == 50 || calls == 100 ||
           calls == 200 || calls == 400 || calls == 800 ||
           (calls > 800 && (calls % 500) == 0);
}

static void msg_flood_track(
    int slot, unsigned int cycle_no, int delta,
    int msg_id, int dest, int recipient, int params,
    int p0_type, int p0_int, int p1_type, int p1_int)
{
    if (slot < 1 || slot > 32 || delta <= 0)
        return;

    MsgFloodClient &fc = g_msg_flood[slot];
    if (fc.cycle_no != cycle_no)
    {
        fc.cycle_no = cycle_no;
        for (int i = 0; i < MSG_FLOOD_SLOTS; ++i)
            fc.entries[i] = MsgFloodEntry();
    }

    int found = -1;
    int free_slot = -1;

    for (int i = 0; i < MSG_FLOOD_SLOTS; ++i)
    {
        MsgFloodEntry &e = fc.entries[i];
        if (!e.used)
        {
            if (free_slot < 0) free_slot = i;
            continue;
        }

        if (e.msg_id == msg_id && e.dest == dest && e.recipient == recipient &&
            e.params == params && e.p0_type == p0_type && e.p0_int == p0_int &&
            e.p1_type == p1_type && e.p1_int == p1_int)
        {
            found = i;
            break;
        }
    }

    if (found < 0)
    {
        if (free_slot < 0)
            return;

        found = free_slot;
        MsgFloodEntry &e = fc.entries[found];
        e.used = true;
        e.msg_id = msg_id;
        e.dest = dest;
        e.recipient = recipient;
        e.params = params;
        e.p0_type = p0_type;
        e.p0_int = p0_int;
        e.p1_type = p1_type;
        e.p1_int = p1_int;
    }

    MsgFloodEntry &e = fc.entries[found];
    ++e.calls;
    e.bytes += static_cast<unsigned int>(delta);

    if (msg_flood_log_point(e.calls))
    {
        IGameClient *gc = g_RehldsSvs ? g_RehldsSvs->GetClient(slot - 1) : nullptr;
        const char *msgname = message_name(msg_id);
        const char *origin = message_origin_hint(msg_id);
        const char *te = (msg_id == 23 && p0_type >= 0) ? temp_entity_name(p0_int) : "";
        log_line(
            "[MSG-FLOOD] id=%d name=\"%s\" cycle=%u msg_id=%d msg_name=\"%s\" "
            "origin_hint=\"%s\" dest=%d recipient=%d params=%d "
            "p0_type=%d p0=%d te_name=\"%s\" p1_type=%d p1=%d calls=%u bytes=%u avg=%.2f",
            slot,
            (gc && gc->GetName()) ? gc->GetName() : "",
            cycle_no, msg_id, msgname, origin, dest, recipient, params,
            p0_type, p0_int, te, p1_type, p1_int,
            e.calls, e.bytes, e.calls ? (double)e.bytes / (double)e.calls : 0.0
        );
    }
}

static void log_line(const char *fmt, ...)
{
    if (!g_log)
        g_log = std::fopen("addons/rehlds_netdiag/rehlds_netdiag.log", "a");

    if (!g_log)
        g_log = std::fopen("cstrike/addons/rehlds_netdiag/rehlds_netdiag.log", "a");

    if (!g_log)
        return;

    std::time_t now = std::time(nullptr);
    std::tm *lt = std::localtime(&now);
    char stamp[64] = "unknown-time";

    if (lt)
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", lt);

    std::fprintf(g_log, "[%s] ", stamp);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);

    std::fputc('\n', g_log);
    std::fflush(g_log);
}

static bool contains_overflow_marker(const char *text)
{
    if (!text)
        return false;

    return std::strstr(text, "SZ_GetSpace: overflow on ") != nullptr ||
           std::strstr(text, "WARNING: datagram overflowed for ") != nullptr ||
           std::strstr(text, "WARNING: msg overflowed for ") != nullptr;
}

static void cycle_begin(TraceCycle &c, int current, unsigned int cycle_no)
{
    c.head = 0;
    c.count = 0;
    c.start_seen = current;
    c.last_seen = current;
    c.high_water = current;
    c.total_msg_bytes = 0;
    c.total_gap_bytes = 0;
    c.total_source_bytes = 0;
    c.cycle_no = cycle_no;
}

static TraceCycle &active_cycle(ClientTrace &t)
{
    return t.cycles[t.active];
}

static void trace_rotate_cycle(int slot, int current)
{
    if (slot < 1 || slot > 32)
        return;

    ClientTrace &t = g_trace[slot];
    t.active = (t.active + 1) % TRACE_CYCLES;
    cycle_begin(t.cycles[t.active], current, t.next_cycle_no++);
}

static void trace_ensure_started(int slot, int current)
{
    if (slot < 1 || slot > 32)
        return;

    ClientTrace &t = g_trace[slot];
    TraceCycle &c = active_cycle(t);

    if (c.last_seen < 0)
        cycle_begin(c, current, t.next_cycle_no++);
}

static void trace_add(
    int slot, int kind, int msg_id, int dest, int params,
    int before, int after,
    int source = SOURCE_NONE,
    int source_arg = 0,
    int source_ref = 0,
    int source_extra = 0,
    int msg_recipient = -1,
    int msg_p0_type = -1,
    int msg_p0_int = 0,
    int msg_p1_type = -1,
    int msg_p1_int = 0)
{
    if (slot < 1 || slot > 32 || after <= before)
        return;

    ClientTrace &t = g_trace[slot];
    TraceCycle &c = active_cycle(t);

    if (c.last_seen < 0)
        cycle_begin(c, before, t.next_cycle_no++);

    TraceEntry &e = c.entries[c.head];

    e.seq = ++g_trace_seq;
    e.kind = kind;
    e.msg_id = msg_id;
    e.dest = dest;
    e.params = params;
    e.before = before;
    e.after = after;
    e.delta = after - before;
    e.source = source;
    e.source_arg = source_arg;
    e.source_ref = source_ref;
    e.source_extra = source_extra;
    e.msg_recipient = msg_recipient;
    e.msg_p0_type = msg_p0_type;
    e.msg_p0_int = msg_p0_int;
    e.msg_p1_type = msg_p1_type;
    e.msg_p1_int = msg_p1_int;

    c.head = (c.head + 1) % TRACE_CAP;
    if (c.count < TRACE_CAP)
        ++c.count;

    c.last_seen = after;
    if (after > c.high_water)
        c.high_water = after;

    if (kind == TRACE_MESSAGE)
        c.total_msg_bytes += e.delta;
    else if (kind == TRACE_SOURCE)
        c.total_source_bytes += e.delta;
    else if (kind == TRACE_ENGINE_GAP)
        c.total_gap_bytes += e.delta;
}

static void trace_sync_before(int slot, int current)
{
    if (slot < 1 || slot > 32)
        return;

    trace_ensure_started(slot, current);

    ClientTrace &t = g_trace[slot];
    TraceCycle &c = active_cycle(t);

    if (current < c.last_seen)
    {
        trace_rotate_cycle(slot, current);
        return;
    }

    if (current > c.last_seen)
    {
        if (g_trace_source != SOURCE_NONE)
            trace_add(
                slot, TRACE_SOURCE, -1, -1, 0,
                c.last_seen, current,
                g_trace_source, g_trace_source_arg,
                g_trace_source_ref, g_trace_source_extra
            );
        else
            trace_add(slot, TRACE_ENGINE_GAP, -1, -1, 0, c.last_seen, current);
    }
}

static void trace_sync_after(
    int slot, int before, int after, int msg_id, int dest, int params,
    int msg_recipient, int p0_type, int p0_int, int p1_type, int p1_int)
{
    if (slot < 1 || slot > 32)
        return;

    trace_ensure_started(slot, before);

    if (after < before)
    {
        trace_rotate_cycle(slot, after);
        return;
    }

    if (after > before)
        trace_add(
            slot, TRACE_MESSAGE, msg_id, dest, params, before, after,
            SOURCE_NONE, 0, 0, 0,
            msg_recipient, p0_type, p0_int, p1_type, p1_int
        );
    else
        active_cycle(g_trace[slot]).last_seen = after;
}

static const TraceCycle *find_best_recent_cycle(int slot)
{
    if (slot < 1 || slot > 32)
        return nullptr;

    ClientTrace &t = g_trace[slot];
    const TraceCycle *best = nullptr;

    for (int i = 0; i < TRACE_CYCLES; ++i)
    {
        const TraceCycle &c = t.cycles[i];
        if (c.last_seen < 0)
            continue;

        if (!best ||
            c.high_water > best->high_water ||
            (c.high_water == best->high_water && c.cycle_no > best->cycle_no))
        {
            best = &c;
        }
    }

    return best;
}

static void dump_cycle_entries(int slot, const char *label, const TraceCycle &c)
{
    log_line(
        "[TRACE-%s-SUMMARY] id=%d cycle=%u start=%d final=%d high_water=%d entries=%d "
        "message_bytes=%d source_bytes=%d engine_gap_bytes=%d accounted=%d",
        label, slot, c.cycle_no, c.start_seen, c.last_seen, c.high_water, c.count,
        c.total_msg_bytes, c.total_source_bytes, c.total_gap_bytes,
        c.total_msg_bytes + c.total_source_bytes + c.total_gap_bytes
    );


    struct SoundAgg
    {
        int sample_id;
        int channel;
        int calls;
        int bytes;
    };

    SoundAgg sounds[64];
    int sound_count = 0;
    int sound_calls = 0;
    int sound_bytes = 0;

    for (int n = 0; n < c.count; ++n)
    {
        int idx = (c.head - c.count + n + TRACE_CAP) % TRACE_CAP;
        const TraceEntry &e = c.entries[idx];

        if (e.kind != TRACE_SOURCE || e.source != SOURCE_START_SOUND)
            continue;

        ++sound_calls;
        sound_bytes += e.delta;

        int found = -1;
        for (int i = 0; i < sound_count; ++i)
        {
            if (sounds[i].sample_id == e.source_ref &&
                sounds[i].channel == e.source_arg)
            {
                found = i;
                break;
            }
        }

        if (found < 0 && sound_count < 64)
        {
            found = sound_count++;
            sounds[found].sample_id = e.source_ref;
            sounds[found].channel = e.source_arg;
            sounds[found].calls = 0;
            sounds[found].bytes = 0;
        }

        if (found >= 0)
        {
            ++sounds[found].calls;
            sounds[found].bytes += e.delta;
        }
    }

    if (sound_calls > 0)
    {
        log_line(
            "[TRACE-%s-SOUND-SUMMARY] id=%d cycle=%u unique=%d calls=%d bytes=%d",
            label, slot, c.cycle_no, sound_count, sound_calls, sound_bytes
        );

        // Sort the small local table by attributed bytes, descending.
        for (int i = 0; i < sound_count; ++i)
        {
            int best = i;
            for (int j = i + 1; j < sound_count; ++j)
            {
                if (sounds[j].bytes > sounds[best].bytes)
                    best = j;
            }

            if (best != i)
            {
                SoundAgg tmp = sounds[i];
                sounds[i] = sounds[best];
                sounds[best] = tmp;
            }
        }

        const int top = sound_count < 16 ? sound_count : 16;
        for (int i = 0; i < top; ++i)
        {
            log_line(
                "[TRACE-%s-SOUND-TOP] id=%d cycle=%u rank=%d sample=\"%s\" "
                "sample_id=%d channel=%d calls=%d bytes=%d",
                label, slot, c.cycle_no, i + 1,
                sound_sample_name(sounds[i].sample_id),
                sounds[i].sample_id,
                sounds[i].channel,
                sounds[i].calls,
                sounds[i].bytes
            );
        }
    }

    struct MsgAgg
    {
        int msg_id;
        int dest;
        int recipient;
        int params;
        int p0_type;
        int p0_int;
        int p1_type;
        int p1_int;
        int calls;
        int bytes;
    };

    MsgAgg msgs[96];
    int msg_count = 0;
    int msg_calls = 0;
    int msg_bytes = 0;

    for (int n = 0; n < c.count; ++n)
    {
        int idx = (c.head - c.count + n + TRACE_CAP) % TRACE_CAP;
        const TraceEntry &e = c.entries[idx];

        if (e.kind != TRACE_MESSAGE)
            continue;

        ++msg_calls;
        msg_bytes += e.delta;

        int found = -1;
        for (int i = 0; i < msg_count; ++i)
        {
            if (msgs[i].msg_id == e.msg_id &&
                msgs[i].dest == e.dest &&
                msgs[i].recipient == e.msg_recipient &&
                msgs[i].params == e.params &&
                msgs[i].p0_type == e.msg_p0_type &&
                msgs[i].p0_int == e.msg_p0_int &&
                msgs[i].p1_type == e.msg_p1_type &&
                msgs[i].p1_int == e.msg_p1_int)
            {
                found = i;
                break;
            }
        }

        if (found < 0 && msg_count < 96)
        {
            found = msg_count++;
            msgs[found].msg_id = e.msg_id;
            msgs[found].dest = e.dest;
            msgs[found].recipient = e.msg_recipient;
            msgs[found].params = e.params;
            msgs[found].p0_type = e.msg_p0_type;
            msgs[found].p0_int = e.msg_p0_int;
            msgs[found].p1_type = e.msg_p1_type;
            msgs[found].p1_int = e.msg_p1_int;
            msgs[found].calls = 0;
            msgs[found].bytes = 0;
        }

        if (found >= 0)
        {
            ++msgs[found].calls;
            msgs[found].bytes += e.delta;
        }
    }

    if (msg_calls > 0)
    {
        log_line(
            "[TRACE-%s-MSG-SUMMARY] id=%d cycle=%u unique=%d calls=%d bytes=%d",
            label, slot, c.cycle_no, msg_count, msg_calls, msg_bytes
        );

        for (int i = 0; i < msg_count; ++i)
        {
            int best = i;
            for (int j = i + 1; j < msg_count; ++j)
            {
                if (msgs[j].bytes > msgs[best].bytes)
                    best = j;
            }

            if (best != i)
            {
                MsgAgg tmp = msgs[i];
                msgs[i] = msgs[best];
                msgs[best] = tmp;
            }
        }

        const int top_msgs = msg_count < 20 ? msg_count : 20;
        for (int i = 0; i < top_msgs; ++i)
        {
            const char *msgname = message_name(msgs[i].msg_id);
            const char *origin = message_origin_hint(msgs[i].msg_id);
            const char *te = (msgs[i].msg_id == 23 && msgs[i].p0_type >= 0)
                ? temp_entity_name(msgs[i].p0_int) : "";
            log_line(
                "[TRACE-%s-MSG-TOP] id=%d cycle=%u rank=%d msg_id=%d msg_name=\"%s\" "
                "origin_hint=\"%s\" dest=%d recipient=%d params=%d "
                "p0_type=%d p0=%d te_name=\"%s\" p1_type=%d p1=%d calls=%d bytes=%d",
                label, slot, c.cycle_no, i + 1,
                msgs[i].msg_id, msgname, origin, msgs[i].dest, msgs[i].recipient,
                msgs[i].params,
                msgs[i].p0_type, msgs[i].p0_int, te,
                msgs[i].p1_type, msgs[i].p1_int,
                msgs[i].calls, msgs[i].bytes
            );
        }
    }

    for (int n = 0; n < c.count; ++n)
    {
        int idx = (c.head - c.count + n + TRACE_CAP) % TRACE_CAP;
        const TraceEntry &e = c.entries[idx];

        if (e.kind == TRACE_MESSAGE)
        {
            const char *msgname = message_name(e.msg_id);
            const char *origin = message_origin_hint(e.msg_id);
            const char *te = (e.msg_id == 23 && e.msg_p0_type >= 0)
                ? temp_entity_name(e.msg_p0_int) : "";
            log_line(
                "[TRACE-%s] id=%d cycle=%u seq=%u kind=MSG msg_id=%d msg_name=\"%s\" "
                "origin_hint=\"%s\" dest=%d recipient=%d params=%d "
                "p0_type=%d p0=%d te_name=\"%s\" p1_type=%d p1=%d before=%d after=%d delta=%d",
                label, slot, c.cycle_no, e.seq,
                e.msg_id, msgname, origin, e.dest, e.msg_recipient, e.params,
                e.msg_p0_type, e.msg_p0_int, te, e.msg_p1_type, e.msg_p1_int,
                e.before, e.after, e.delta
            );
        }
        else if (e.kind == TRACE_SOURCE)
        {
            if (e.source == SOURCE_START_SOUND ||
                e.source == SOURCE_EMIT_SOUND2 ||
                e.source == SOURCE_BUILD_SOUND_MSG)
            {
                log_line(
                    "[TRACE-%s] id=%d cycle=%u seq=%u kind=SOURCE source=%s source_id=%d "
                    "channel=%d sample_id=%d sample=\"%s\" extra=%d "
                    "before=%d after=%d delta=%d",
                    label, slot, c.cycle_no, e.seq,
                    trace_source_name(e.source, e.source_arg),
                    e.source,
                    e.source_arg,
                    e.source_ref,
                    sound_sample_name(e.source_ref),
                    e.source_extra,
                    e.before, e.after, e.delta
                );
            }
            else
            {
                log_line(
                    "[TRACE-%s] id=%d cycle=%u seq=%u kind=SOURCE source=%s source_id=%d "
                    "arg=%d ref=%d extra=%d before=%d after=%d delta=%d",
                    label, slot, c.cycle_no, e.seq,
                    trace_source_name(e.source, e.source_arg),
                    e.source, e.source_arg, e.source_ref, e.source_extra,
                    e.before, e.after, e.delta
                );
            }
        }
        else
        {
            log_line(
                "[TRACE-%s] id=%d cycle=%u seq=%u kind=ENGINE_GAP before=%d after=%d delta=%d",
                label, slot, c.cycle_no, e.seq, e.before, e.after, e.delta
            );
        }
    }
}

static void dump_trace_for_client(IGameClient *gc)
{
    if (!gc)
        return;

    const int id = gc->GetId();
    const int slot = id + 1;

    if (id < 0 || id >= 32)
        return;

    sizebuf_t *datagram = gc->GetDatagram();
    const int current = datagram ? datagram->cursize : -1;

    if (current >= 0)
        trace_sync_before(slot, current);

    ClientTrace &t = g_trace[slot];
    TraceCycle &cur = active_cycle(t);
    const TraceCycle *best = find_best_recent_cycle(slot);

    log_line(
        "[TRACE-STATE] id=%d name=\"%s\" current_datagram=%d active_cycle=%u active_high_water=%d "
        "best_cycle=%u best_high_water=%d",
        slot,
        gc->GetName() ? gc->GetName() : "",
        current,
        cur.cycle_no,
        cur.high_water,
        best ? best->cycle_no : 0,
        best ? best->high_water : -1
    );

    if (best)
        dump_cycle_entries(slot, "BEST", *best);

    if (cur.last_seen >= 0 && (!best || cur.cycle_no != best->cycle_no))
        dump_cycle_entries(slot, "CURRENT", cur);
}

static void dump_client(IGameClient *gc, const char *reason)
{
    if (!gc)
        return;

    const int id = gc->GetId();
    if (id < 0 || id >= 32)
        return;

    sizebuf_t *datagram = gc->GetDatagram();
    INetChan *nc = gc->GetNetChan();
    sizebuf_t *reliable = nc ? nc->GetMessageBuf() : nullptr;
    const ClientSnap &s = g_snap[id + 1];

    const netadr_t *adr = nc ? nc->GetRemoteAdr() : nullptr;

    log_line(
        "[CLIENT] reason=\"%s\" id=%d name=\"%s\" "
        "connected=%d active=%d spawned=%d "
        "datagram=%d/%d flags=0x%x "
        "reliable=%d/%d flags=0x%x "
        "packet_entities=%d packet_msg_before=%d packet_msg_after=%d "
        "packet_msg_delta=%d packet_type=%d "
        "soundguard_skipped_calls=%u soundguard_skipped_bytes=%u "
        "adr_type=%d adr_port=%u",
        reason ? reason : "",
        id + 1,
        gc->GetName() ? gc->GetName() : "",
        gc->IsConnected() ? 1 : 0,
        gc->IsActive() ? 1 : 0,
        gc->IsSpawned() ? 1 : 0,
        datagram ? datagram->cursize : -1,
        datagram ? datagram->maxsize : -1,
        datagram ? datagram->flags : 0,
        reliable ? reliable->cursize : -1,
        reliable ? reliable->maxsize : -1,
        reliable ? reliable->flags : 0,
        s.packet_entities,
        s.msg_before,
        s.msg_after,
        s.msg_delta,
        s.packet_type,
        g_soundguard_skipped_calls[id + 1],
        g_soundguard_skipped_bytes[id + 1],
        adr ? static_cast<int>(adr->type) : -1,
        adr ? adr->port : 0
    );
}

static void dump_all_clients(const char *reason)
{
    if (!g_RehldsSvs)
        return;

    log_line("################ ENGINE NET EVENT ################");
    log_line("[EVENT] %s", reason ? reason : "");

    const int maxClients = g_RehldsSvs->GetMaxClients();
    for (int i = 0; i < maxClients && i < 32; ++i)
    {
        IGameClient *gc = g_RehldsSvs->GetClient(i);
        if (!gc || !gc->IsConnected())
            continue;

        dump_client(gc, reason);
    }

    for (int i = 0; i < maxClients && i < 32; ++i)
    {
        IGameClient *gc = g_RehldsSvs->GetClient(i);
        if (!gc || !gc->IsConnected())
            continue;

        sizebuf_t *datagram = gc->GetDatagram();
        const char *name = gc->GetName() ? gc->GetName() : "";

        const bool near_full = datagram && datagram->maxsize > 0 &&
            datagram->cursize >= (datagram->maxsize - 256);

        const bool named = reason && name[0] && std::strstr(reason, name) != nullptr;

        if (near_full || named)
            dump_trace_for_client(gc);
    }

    log_line("##################################################");
}

static int Hook_SV_CreatePacketEntities(
    IRehldsHook_SV_CreatePacketEntities *chain,
    sv_delta_t type,
    IGameClient *client,
    struct packet_entities_s *to,
    sizebuf_t *msg)
{
    int id = client ? client->GetId() : -1;
    int before = msg ? msg->cursize : 0;

    int result = chain->callNext(type, client, to, msg);

    int after = msg ? msg->cursize : 0;

    if (id >= 0 && id < 32)
    {
        ClientSnap &s = g_snap[id + 1];
        s.packet_entities = to ? to->num_entities : 0;
        s.msg_before = before;
        s.msg_after = after;
        s.msg_delta = after - before;
        s.packet_type = static_cast<int>(type);
    }

    return result;
}

static void Hook_UserMessage(IVoidHookChain<IMessage *> *chain, IMessage *msg)
{
    int before[32];
    bool valid[32];

    for (int i = 0; i < 32; ++i)
    {
        before[i] = 0;
        valid[i] = false;
    }

    if (g_RehldsSvs)
    {
        const int maxClients = g_RehldsSvs->GetMaxClients();

        for (int i = 0; i < maxClients && i < 32; ++i)
        {
            IGameClient *gc = g_RehldsSvs->GetClient(i);
            if (!gc || !gc->IsConnected())
                continue;

            sizebuf_t *datagram = gc->GetDatagram();
            if (!datagram)
                continue;

            trace_sync_before(i + 1, datagram->cursize);
            before[i] = datagram->cursize;
            valid[i] = true;
        }
    }

    const int msg_id = msg ? msg->getId() : -1;
    const int dest = msg ? static_cast<int>(msg->getDest()) : -1;
    const int params = msg ? msg->getParamCount() : 0;

    int msg_recipient = -1;
    int p0_type = -1;
    int p0_int = 0;
    int p1_type = -1;
    int p1_int = 0;

    if (msg)
    {
        edict_t *msg_edict = msg->getEdict();
        if (msg_edict && g_engfuncs.pfnIndexOfEdict)
            msg_recipient = g_engfuncs.pfnIndexOfEdict(msg_edict);

        if (params > 0)
        {
            p0_type = static_cast<int>(msg->getParamType(0));
            if (msg->getParamType(0) != IMessage::ParamType::String)
                p0_int = msg->getParamInt(0);
        }

        if (params > 1)
        {
            p1_type = static_cast<int>(msg->getParamType(1));
            if (msg->getParamType(1) != IMessage::ParamType::String)
                p1_int = msg->getParamInt(1);
        }
    }

    chain->callNext(msg);

    if (!g_RehldsSvs)
        return;

    const int maxClients = g_RehldsSvs->GetMaxClients();

    for (int i = 0; i < maxClients && i < 32; ++i)
    {
        if (!valid[i])
            continue;

        IGameClient *gc = g_RehldsSvs->GetClient(i);
        if (!gc || !gc->IsConnected())
            continue;

        sizebuf_t *datagram = gc->GetDatagram();
        if (!datagram)
            continue;

        trace_sync_after(
            i + 1, before[i], datagram->cursize,
            msg_id, dest, params,
            msg_recipient, p0_type, p0_int, p1_type, p1_int
        );

        const int delta = datagram->cursize - before[i];
        if (delta > 0)
        {
            const TraceCycle &cycle = active_cycle(g_trace[i + 1]);
            msg_flood_track(
                i + 1, cycle.cycle_no, delta,
                msg_id, dest, msg_recipient, params,
                p0_type, p0_int, p1_type, p1_int
            );
        }
    }
}

static void sync_all_datagrams()
{
    if (!g_RehldsSvs) return;
    const int maxClients = g_RehldsSvs->GetMaxClients();
    for (int i = 0; i < maxClients && i < 32; ++i)
    {
        IGameClient *gc = g_RehldsSvs->GetClient(i);
        if (!gc || !gc->IsConnected()) continue;
        sizebuf_t *datagram = gc->GetDatagram();
        if (!datagram) continue;
        trace_sync_before(i + 1, datagram->cursize);
    }
}

struct SourceGuard
{
    int old_source;
    int old_arg;
    int old_ref;
    int old_extra;

    SourceGuard(int source, int arg, int ref = 0, int extra = 0)
        : old_source(g_trace_source),
          old_arg(g_trace_source_arg),
          old_ref(g_trace_source_ref),
          old_extra(g_trace_source_extra)
    {
        sync_all_datagrams();
        g_trace_source = source;
        g_trace_source_arg = arg;
        g_trace_source_ref = ref;
        g_trace_source_extra = extra;
    }

    ~SourceGuard()
    {
        sync_all_datagrams();
        g_trace_source = old_source;
        g_trace_source_arg = old_arg;
        g_trace_source_ref = old_ref;
        g_trace_source_extra = old_extra;
    }
};

static void Hook_HandleNetCommand(IRehldsHook_HandleNetCommand *chain, IGameClient *client, uint8 opcode)
{
    SourceGuard guard(SOURCE_NETCMD, static_cast<int>(opcode));
    chain->callNext(client, opcode);
}

static void Hook_SV_StartSound(IRehldsHook_SV_StartSound *chain, int recipients, edict_t *entity, int channel, const char *sample, int volume, float attenuation, int flags, int pitch)
{
    const int sample_id = intern_sound_sample(sample);
    const bool footstep = is_footstep_sample(sample);

    // Important: SV_StartSound writes to the engine multicast buffer first and
    // then fans the sound out to clients. The old v1.9 guard tried to rewind
    // an individual client datagram AFTER callNext(), which is too late: the
    // overflow can already have happened inside callNext().
    //
    // ReHLDS exposes no per-client recipient mask in this hook (recipients is
    // only the engine's 0/1 mode), so the safe pre-write protection is to drop
    // only footstep sounds while at least one connected client's datagram is
    // close to full. This is deliberately narrow and temporary: non-footstep
    // sounds and all game messages continue normally.
    if (footstep && g_RehldsSvs)
    {
        const int maxClients = g_RehldsSvs->GetMaxClients();
        int pressured_slot = 0;
        int pressured_cur = 0;
        int pressured_max = 0;

        for (int i = 0; i < maxClients && i < 32; ++i)
        {
            IGameClient *gc = g_RehldsSvs->GetClient(i);
            if (!gc || !gc->IsConnected())
                continue;

            sizebuf_t *datagram = gc->GetDatagram();
            if (!datagram || datagram->maxsize <= 0)
                continue;

            const int threshold = datagram->maxsize - SOUND_GUARD_HEADROOM;
            if (datagram->cursize >= threshold)
            {
                pressured_slot = i + 1;
                pressured_cur = datagram->cursize;
                pressured_max = datagram->maxsize;
                break;
            }
        }

        if (pressured_slot > 0)
        {
            ++g_soundguard_skipped_calls[pressured_slot];
            // Ordinary CS footstep sound writes observed in our trace are 13 B.
            // Keep this counter as an estimate because we now block pre-write.
            g_soundguard_skipped_bytes[pressured_slot] += 13;

            const std::time_t now = std::time(nullptr);
            if (g_soundguard_last_log[pressured_slot] != now)
            {
                g_soundguard_last_log[pressured_slot] = now;
                IGameClient *gc = g_RehldsSvs->GetClient(pressured_slot - 1);
                log_line(
                    "[SOUND-GUARD-PRE] pressured_id=%d name=\"%s\" sample=\"%s\" channel=%d "
                    "datagram=%d/%d action=drop_footstep_global total_calls=%u est_total_bytes=%u",
                    pressured_slot,
                    (gc && gc->GetName()) ? gc->GetName() : "",
                    sample ? sample : "",
                    channel,
                    pressured_cur, pressured_max,
                    g_soundguard_skipped_calls[pressured_slot],
                    g_soundguard_skipped_bytes[pressured_slot]
                );
            }

            return;
        }
    }

    SourceGuard guard(SOURCE_START_SOUND, channel, sample_id, recipients);
    chain->callNext(recipients, entity, channel, sample, volume, attenuation, flags, pitch);
}

static bool Hook_SV_EmitSound2(IRehldsHook_SV_EmitSound2 *chain, edict_t *entity, IGameClient *receiver, int channel, const char *sample, float volume, float attenuation, int flags, int pitch, int emitFlags, const float *pOrigin)
{
    const int sample_id = intern_sound_sample(sample);
    SourceGuard guard(SOURCE_EMIT_SOUND2, channel, sample_id, emitFlags);
    return chain->callNext(entity, receiver, channel, sample, volume, attenuation, flags, pitch, emitFlags, pOrigin);
}

static void Hook_EV_PlayReliableEvent(IRehldsHook_EV_PlayReliableEvent *chain, IGameClient *client, int entindex, unsigned short eventindex, float delay, struct event_args_s *pargs)
{
    SourceGuard guard(SOURCE_RELIABLE_EVENT, static_cast<int>(eventindex));
    chain->callNext(client, entindex, eventindex, delay, pargs);
}

static void Hook_PF_BuildSoundMsg_I(IRehldsHook_PF_BuildSoundMsg_I *chain, edict_t *entity, int channel, const char *sample, float volume, float attenuation, int flags, int pitch, int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
    const int sample_id = intern_sound_sample(sample);
    SourceGuard guard(SOURCE_BUILD_SOUND_MSG, channel, sample_id, msg_dest);
    chain->callNext(entity, channel, sample, volume, attenuation, flags, pitch, msg_dest, msg_type, pOrigin, ed);
}

static void Hook_Con_Printf(IRehldsHook_Con_Printf *chain, const char *text)
{
    if (contains_overflow_marker(text))
        dump_all_clients(text);

    chain->callNext(text);
}

typedef void *(*CreateInterfaceFnLocal)(const char *, int *);

static bool init_rehlds_api()
{
    void *engineModule = dlopen("engine_i486.so", RTLD_NOW | RTLD_NOLOAD);

    if (!engineModule)
        engineModule = dlopen("./engine_i486.so", RTLD_NOW | RTLD_NOLOAD);

    if (!engineModule)
    {
        log_line("[ERROR] Cannot find loaded engine_i486.so: %s", dlerror() ? dlerror() : "unknown");
        return false;
    }

    auto factory = reinterpret_cast<CreateInterfaceFnLocal>(
        dlsym(engineModule, "CreateInterface")
    );

    if (!factory)
    {
        log_line("[ERROR] Cannot resolve CreateInterface: %s", dlerror() ? dlerror() : "unknown");
        return false;
    }

    int retCode = 0;
    g_RehldsApi = reinterpret_cast<IRehldsApi*>(
        factory(VREHLDS_HLDS_API_VERSION, &retCode)
    );

    if (!g_RehldsApi)
    {
        log_line("[ERROR] ReHLDS API not found ret=%d", retCode);
        return false;
    }

    const int major = g_RehldsApi->GetMajorVersion();
    const int minor = g_RehldsApi->GetMinorVersion();

    if (major != REHLDS_API_VERSION_MAJOR)
    {
        log_line("[ERROR] ReHLDS API major mismatch: plugin=%d server=%d",
                 REHLDS_API_VERSION_MAJOR, major);
        return false;
    }

    g_RehldsFuncs = g_RehldsApi->GetFuncs();
    g_RehldsHookchains = g_RehldsApi->GetHookchains();
    g_RehldsSvs = g_RehldsApi->GetServerStatic();
    g_MessageManager = g_RehldsApi->GetMessageManager();

    if (!g_RehldsFuncs || !g_RehldsHookchains || !g_RehldsSvs || !g_MessageManager)
    {
        log_line("[ERROR] ReHLDS API returned null funcs/hookchains/serverstatic/messagemanager");
        return false;
    }

    if (g_MessageManager->getMajorVersion() != MESSAGEMNGR_VERSION_MAJOR)
    {
        log_line("[ERROR] MessageManager major mismatch: plugin=%d server=%d",
                 MESSAGEMNGR_VERSION_MAJOR, g_MessageManager->getMajorVersion());
        return false;
    }

    log_line("[API] ReHLDS API %d.%d initialized", major, minor);
    return true;
}

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "ReHLDS NetDiag",
    "2.0-source-diag",
    __DATE__,
    "OpenAI diagnostic build",
    "https://github.com/rehlds/ReHLDS",
    "REHLDS_NETDIAG",
    PT_STARTUP,
    PT_ANYTIME
};

C_DLLEXPORT int Meta_Query(
    char *interfaceVersion,
    plugin_info_t **plinfo,
    mutil_funcs_t *pMetaUtilFuncs)
{
    *plinfo = &Plugin_info;
    gpMetaUtilFuncs = pMetaUtilFuncs;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(
    PLUG_LOADTIME now,
    META_FUNCTIONS *pFunctionTable,
    meta_globals_t *pMGlobals,
    gamedll_funcs_t *pGamedllFuncs)
{
    gpMetaGlobals = pMGlobals;
    gpGamedllFuncs = pGamedllFuncs;

    if (pFunctionTable)
        std::memset(pFunctionTable, 0, sizeof(*pFunctionTable));

    if (!init_rehlds_api())
        return FALSE;

    g_RehldsHookchains->SV_CreatePacketEntities()->registerHook(&Hook_SV_CreatePacketEntities);
    g_RehldsHookchains->HandleNetCommand()->registerHook(&Hook_HandleNetCommand);
    g_RehldsHookchains->SV_StartSound()->registerHook(&Hook_SV_StartSound);
    g_RehldsHookchains->SV_EmitSound2()->registerHook(&Hook_SV_EmitSound2);
    g_RehldsHookchains->EV_PlayReliableEvent()->registerHook(&Hook_EV_PlayReliableEvent);
    g_RehldsHookchains->PF_BuildSoundMsg_I()->registerHook(&Hook_PF_BuildSoundMsg_I);
    g_RehldsHookchains->Con_Printf()->registerHook(&Hook_Con_Printf);

    for (int msg_id = 0; msg_id < 256; ++msg_id)
        g_MessageManager->registerHook(msg_id, &Hook_UserMessage);

    log_line("[START] ReHLDS NetDiag 2.0-source-diag attached; protocol names + TempEntity decoding + flood source hints enabled");
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason)
{
    if (g_RehldsHookchains)
    {
        g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&Hook_SV_CreatePacketEntities);
        g_RehldsHookchains->HandleNetCommand()->unregisterHook(&Hook_HandleNetCommand);
        g_RehldsHookchains->SV_StartSound()->unregisterHook(&Hook_SV_StartSound);
        g_RehldsHookchains->SV_EmitSound2()->unregisterHook(&Hook_SV_EmitSound2);
        g_RehldsHookchains->EV_PlayReliableEvent()->unregisterHook(&Hook_EV_PlayReliableEvent);
        g_RehldsHookchains->PF_BuildSoundMsg_I()->unregisterHook(&Hook_PF_BuildSoundMsg_I);
        g_RehldsHookchains->Con_Printf()->unregisterHook(&Hook_Con_Printf);
    }

    if (g_MessageManager)
    {
        for (int msg_id = 0; msg_id < 256; ++msg_id)
            g_MessageManager->unregisterHook(msg_id, &Hook_UserMessage);
    }

    log_line("[STOP] ReHLDS NetDiag detached");

    if (g_log)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }

    return TRUE;
}

C_DLLEXPORT void GiveFnptrsToDll(
    enginefuncs_t *pengfuncsFromEngine,
    globalvars_t *pGlobals)
{
    if (pengfuncsFromEngine)
        std::memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(g_engfuncs));

    gpGlobals = pGlobals;
}
