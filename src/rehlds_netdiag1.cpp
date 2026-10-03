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

static const int TRACE_CAP = 512;
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
    int before, int after, int source = SOURCE_NONE, int source_arg = 0)
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
            trace_add(slot, TRACE_SOURCE, -1, -1, 0, c.last_seen, current, g_trace_source, g_trace_source_arg);
        else
            trace_add(slot, TRACE_ENGINE_GAP, -1, -1, 0, c.last_seen, current);
    }
}

static void trace_sync_after(int slot, int before, int after, int msg_id, int dest, int params)
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
        trace_add(slot, TRACE_MESSAGE, msg_id, dest, params, before, after);
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

    for (int n = 0; n < c.count; ++n)
    {
        int idx = (c.head - c.count + n + TRACE_CAP) % TRACE_CAP;
        const TraceEntry &e = c.entries[idx];

        if (e.kind == TRACE_MESSAGE)
        {
            log_line(
                "[TRACE-%s] id=%d cycle=%u seq=%u kind=MSG msg_id=%d dest=%d params=%d before=%d after=%d delta=%d",
                label, slot, c.cycle_no, e.seq, e.msg_id, e.dest, e.params, e.before, e.after, e.delta
            );
        }
        else if (e.kind == TRACE_SOURCE)
        {
            log_line(
                "[TRACE-%s] id=%d cycle=%u seq=%u kind=SOURCE source=%s source_id=%d arg=%d before=%d after=%d delta=%d",
                label, slot, c.cycle_no, e.seq, trace_source_name(e.source, e.source_arg), e.source, e.source_arg,
                e.before, e.after, e.delta
            );
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

        trace_sync_after(i + 1, before[i], datagram->cursize, msg_id, dest, params);
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
    SourceGuard(int source, int arg) : old_source(g_trace_source), old_arg(g_trace_source_arg)
    {
        sync_all_datagrams();
        g_trace_source = source;
        g_trace_source_arg = arg;
    }
    ~SourceGuard()
    {
        sync_all_datagrams();
        g_trace_source = old_source;
        g_trace_source_arg = old_arg;
    }
};

static void Hook_HandleNetCommand(IRehldsHook_HandleNetCommand *chain, IGameClient *client, uint8 opcode)
{
    SourceGuard guard(SOURCE_NETCMD, static_cast<int>(opcode));
    chain->callNext(client, opcode);
}

static void Hook_SV_StartSound(IRehldsHook_SV_StartSound *chain, int recipients, edict_t *entity, int channel, const char *sample, int volume, float attenuation, int flags, int pitch)
{
    SourceGuard guard(SOURCE_START_SOUND, channel);
    chain->callNext(recipients, entity, channel, sample, volume, attenuation, flags, pitch);
}

static bool Hook_SV_EmitSound2(IRehldsHook_SV_EmitSound2 *chain, edict_t *entity, IGameClient *receiver, int channel, const char *sample, float volume, float attenuation, int flags, int pitch, int emitFlags, const float *pOrigin)
{
    SourceGuard guard(SOURCE_EMIT_SOUND2, channel);
    return chain->callNext(entity, receiver, channel, sample, volume, attenuation, flags, pitch, emitFlags, pOrigin);
}

static void Hook_EV_PlayReliableEvent(IRehldsHook_EV_PlayReliableEvent *chain, IGameClient *client, int entindex, unsigned short eventindex, float delay, struct event_args_s *pargs)
{
    SourceGuard guard(SOURCE_RELIABLE_EVENT, static_cast<int>(eventindex));
    chain->callNext(client, entindex, eventindex, delay, pargs);
}

static void Hook_PF_BuildSoundMsg_I(IRehldsHook_PF_BuildSoundMsg_I *chain, edict_t *entity, int channel, const char *sample, float volume, float attenuation, int flags, int pitch, int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
    SourceGuard guard(SOURCE_BUILD_SOUND_MSG, channel);
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
    "1.7",
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

    log_line("[START] ReHLDS NetDiag 1.7 attached; low-level source attribution enabled");
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
