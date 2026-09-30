#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>

#include "extdll.h"
#include "meta_api.h"
#include "rehlds_api.h"
#include "interface.h"

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals = nullptr;
meta_globals_t *gpMetaGlobals = nullptr;
gamedll_funcs_t *gpGamedllFuncs = nullptr;
mutil_funcs_t *gpMetaUtilFuncs = nullptr;

IRehldsApi *g_RehldsApi = nullptr;
const RehldsFuncs_t *g_RehldsFuncs = nullptr;
IRehldsHookchains *g_RehldsHookchains = nullptr;
IRehldsServerStatic *g_RehldsSvs = nullptr;

static FILE *g_log = nullptr;

struct ClientSnap
{
    int packet_entities;
    int msg_before;
    int msg_after;
    int msg_delta;
    int packet_type;

    ClientSnap()
        : packet_entities(0),
          msg_before(0),
          msg_after(0),
          msg_delta(0),
          packet_type(0)
    {}
};

static ClientSnap g_snap[33];

static void log_line(const char *fmt, ...)
{
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

    log_line(
        "[CLIENT] reason=\"%s\" id=%d name=\"%s\" "
        "connected=%d active=%d spawned=%d fake=%d fully=%d "
        "choke=%d delta_seq=%d loss=%.3f latency=%.3f "
        "datagram=%d/%d flags=0x%x "
        "reliable=%d/%d flags=0x%x "
        "packet_entities=%d packet_msg_before=%d packet_msg_after=%d "
        "packet_msg_delta=%d packet_type=%d "
        "next_msg=%.6f next_interval=%.6f send_state=%d skip_state=%d",
        reason ? reason : "",
        id + 1,
        gc->GetName() ? gc->GetName() : "",
        gc->IsConnected() ? 1 : 0,
        gc->IsActive() ? 1 : 0,
        gc->IsSpawned() ? 1 : 0,
        gc->IsFakeClient() ? 1 : 0,
        gc->IsFullyConnected() ? 1 : 0,
        gc->GetChokeCount(),
        gc->GetDeltaSequence(),
        gc->GetPacketLoss(),
        gc->GetLatency(),
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
        gc->GetNextMessageTime(),
        gc->GetNextMessageIntervalTime(),
        gc->GetSendMessageState() ? 1 : 0,
        gc->GetSkipMessageState() ? 1 : 0
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
        if (!gc || !gc->IsConnected() || gc->IsFakeClient())
            continue;

        dump_client(gc, reason);
    }

    log_line("##################################################");
}

static int Hook_SV_CreatePacketEntities(
    IRehldsHook_SV_CreatePacketEntities *chain,
    sv_delta_t type,
    IGameClient *client,
    packet_entities_t *to,
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

static void Hook_Con_Printf(IRehldsHook_Con_Printf *chain, const char *text)
{
    if (contains_overflow_marker(text))
        dump_all_clients(text);

    chain->callNext(text);
}

static bool init_rehlds_api()
{
#ifdef _WIN32
    const char *engineNames[] = { "swds.dll", "sw.dll", "hw.dll" };
    CSysModule *engineModule = nullptr;
    for (const char *name : engineNames)
    {
        engineModule = Sys_GetModuleHandle(name);
        if (engineModule)
            break;
    }
#else
    CSysModule *engineModule = Sys_GetModuleHandle("engine_i486.so");
#endif

    if (!engineModule)
    {
        log_line("[ERROR] Cannot find ReHLDS engine module");
        return false;
    }

    CreateInterfaceFn factory = Sys_GetFactory(engineModule);
    if (!factory)
    {
        log_line("[ERROR] Cannot get engine interface factory");
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

    if (minor < REHLDS_API_VERSION_MINOR)
    {
        log_line("[WARN] ReHLDS API minor older than build headers: plugin=%d server=%d",
                 REHLDS_API_VERSION_MINOR, minor);
        // Continue: the functions used by this plugin are long-established API calls.
    }

    g_RehldsFuncs = g_RehldsApi->GetFuncs();
    g_RehldsHookchains = g_RehldsApi->GetHookchains();
    g_RehldsSvs = g_RehldsApi->GetServerStatic();

    if (!g_RehldsFuncs || !g_RehldsHookchains || !g_RehldsSvs)
    {
        log_line("[ERROR] ReHLDS API returned null funcs/hookchains/serverstatic");
        return false;
    }

    log_line("[API] ReHLDS API %d.%d initialized", major, minor);
    return true;
}

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "ReHLDS NetDiag",
    "1.1",
    __DATE__,
    "OpenAI diagnostic build",
    "https://github.com/rehlds/ReHLDS",
    "REHLDS_NETDIAG",
    PT_STARTUP,
    PT_ANYTIME
};

C_DLLEXPORT int Meta_Query(
    const char *interfaceVersion,
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
    g_RehldsHookchains->Con_Printf()->registerHook(&Hook_Con_Printf);

    log_line("[START] ReHLDS NetDiag 1.1 attached");
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason)
{
    if (g_RehldsHookchains)
    {
        g_RehldsHookchains->SV_CreatePacketEntities()->unregisterHook(&Hook_SV_CreatePacketEntities);
        g_RehldsHookchains->Con_Printf()->unregisterHook(&Hook_Con_Printf);
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
