// THUG-Vita — serveur de debug TCP. Voir vita_dbgsrv.h pour la raison d'etre.
//
// Commandes (une par ligne, reponse sur une ligne sauf mention) :
//
//   ping                      -> ok
//   state                     -> STATE frame=.. fps=.. t_ms=.. busy=.. hold=..
//   shot [full|half|quarter]  -> IMG w h 1 zlen\n + zlib(RGB888, haut en bas)
//   burst N PERIODE_MS [taille] -> IMG w h N zlen\n + zlib(N images collees)
//   hold MS tok...            -> ok       maintient boutons/sticks MS ms
//                                tok : croix rond carre triangle haut bas
//                                gauche droite start select l1 r1
//                                (alias anglais : cross circle square up down
//                                left right l r) et lx= ly= rx= ry= dans -1..1
//   idle [MS]                 -> ok | timeout   attend la fin des injections
//   frames N [MS]             -> ok | timeout   attend N images presentees
//   log POS                   -> LOG newpos len\n + octets du journal depuis POS
//                                (POS = -1 : rend seulement la fin courante)
//   help                      -> liste
//   <autre>                   -> ok queued : transmis tel quel a l'analyseur
//                                d'inject.txt (touches, unlock, zoom, ...)

#include "vita_dbgsrv.h"
#include "vita_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <vitaGL.h>
#include <zlib.h>

#define SHOT_W      960
#define SHOT_H      544
#define NET_MEM     ( 1024 * 1024 )
#define BURST_MAX   16

// --- etat partage ----------------------------------------------------------

static volatile unsigned int s_frames = 0;
static volatile float        s_frame_ms = 0.0f;
static volatile int          s_busy = 0;

// File de commandes : UN producteur (serveur), UN consommateur (principal).
// Indices volatils + barrieres : pas besoin de verrou.
#define CMD_Q   8
#define CMD_LEN 512
static char         s_cmdq[CMD_Q][CMD_LEN];
static volatile int s_cmd_head = 0;    // ecrit par le serveur
static volatile int s_cmd_tail = 0;    // ecrit par le principal

// Maintien de manette. Petit, lu une fois par image : un mutex suffit.
static SceUID s_mtx = -1;
static struct {
    unsigned int  buttons;
    int           has_sticks;
    unsigned char lx, ly, rx, ry;
    SceUInt64     until;
} s_hold;

// Capture : 0 libre, 1 demandee, 3 en lecture, 2 prete.
static volatile int  s_shot_state = 0;
static unsigned char *s_shot_rgba = NULL;   // SHOT_W*SHOT_H*4, alloue au 1er usage

// --- cote thread principal -------------------------------------------------

void vita_dbgsrv_frame(void)
{
    static SceUInt64 s_prev = 0;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (s_prev) {
        float ms = (float)(now - s_prev) / 1000.0f;
        s_frame_ms = (s_frame_ms == 0.0f) ? ms : (s_frame_ms * 0.9f + ms * 0.1f);
    }
    s_prev = now;
    ++s_frames;

    // Lecture du tampon arriere juste avant sa presentation : c'est ce qui
    // sera a l'ecran. Seul le thread qui possede le contexte GL peut le faire.
    if (__sync_bool_compare_and_swap(&s_shot_state, 1, 3)) {
        glReadPixels(0, 0, SHOT_W, SHOT_H, GL_RGBA, GL_UNSIGNED_BYTE, s_shot_rgba);
        __sync_synchronize();
        s_shot_state = 2;
    }
}

int vita_dbgsrv_take_command(char *out, int size)
{
    if (s_cmd_tail == s_cmd_head)
        return 0;
    __sync_synchronize();
    strncpy(out, s_cmdq[s_cmd_tail], size - 1);
    out[size - 1] = 0;
    __sync_synchronize();
    s_cmd_tail = (s_cmd_tail + 1) % CMD_Q;
    return 1;
}

int vita_dbgsrv_pad(VitaDbgPad *out)
{
    if (s_mtx < 0)
        return 0;
    int active = 0;
    sceKernelLockMutex(s_mtx, 1, NULL);
    if (s_hold.until && sceKernelGetProcessTimeWide() < s_hold.until) {
        out->buttons    = s_hold.buttons;
        out->has_sticks = s_hold.has_sticks;
        out->lx = s_hold.lx; out->ly = s_hold.ly;
        out->rx = s_hold.rx; out->ry = s_hold.ry;
        active = 1;
    }
    sceKernelUnlockMutex(s_mtx, 1);
    return active;
}

void vita_dbgsrv_set_busy(int busy)
{
    s_busy = busy;
}

// --- utilitaires reseau -----------------------------------------------------

static int send_all(int s, const void *p, int n)
{
    const char *c = (const char *)p;
    while (n > 0) {
        int r = sceNetSend(s, c, n, 0);
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

static int send_str(int s, const char *str)
{
    return send_all(s, str, (int)strlen(str));
}

static SceUInt64 now_us(void)
{
    return sceKernelGetProcessTimeWide();
}

static int hold_active(void)
{
    VitaDbgPad p;
    return vita_dbgsrv_pad(&p);
}

// --- capture ----------------------------------------------------------------

// Demande une image au thread principal et attend qu'elle soit lue.
static int grab(unsigned int timeout_ms)
{
    if (!s_shot_rgba) {
        s_shot_rgba = (unsigned char *)malloc(SHOT_W * SHOT_H * 4);
        if (!s_shot_rgba)
            return -1;
    }
    s_shot_state = 1;
    SceUInt64 limit = now_us() + (SceUInt64)timeout_ms * 1000;
    for (;;) {
        if (s_shot_state == 2) {
            __sync_synchronize();
            s_shot_state = 0;
            return 0;
        }
        if (now_us() > limit) {
            // Retire la demande, sauf si la lecture a deja commence.
            if (__sync_bool_compare_and_swap(&s_shot_state, 1, 0))
                return -1;
        }
        sceKernelDelayThread(2000);
    }
}

// RGBA de bas en haut (glReadPixels) -> RGB de haut en bas, reduit par
// moyenne de blocs step x step : le texte des menus reste lisible en demi.
static void convert(unsigned char *dst, int step)
{
    const int w = SHOT_W / step, h = SHOT_H / step, n = step * step;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < step; ++dy) {
                const unsigned char *p = s_shot_rgba
                    + ((SHOT_H - 1 - (y * step + dy)) * SHOT_W + x * step) * 4;
                for (int dx = 0; dx < step; ++dx, p += 4) {
                    r += p[0]; g += p[1]; b += p[2];
                }
            }
            *dst++ = (unsigned char)(r / n);
            *dst++ = (unsigned char)(g / n);
            *dst++ = (unsigned char)(b / n);
        }
    }
}

static int parse_step(const char *arg)
{
    if (arg && !strncmp(arg, "half", 4))    return 2;
    if (arg && !strncmp(arg, "quarter", 7)) return 4;
    return 1;
}

static void do_shots(int s, int count, int period_ms, int step)
{
    if (count < 1) count = 1;
    if (count > BURST_MAX) count = BURST_MAX;

    const int w = SHOT_W / step, h = SHOT_H / step;
    const int one = w * h * 3;
    unsigned char *raw = (unsigned char *)malloc((size_t)one * count);
    if (!raw) {
        send_str(s, "err memoire\n");
        return;
    }

    SceUInt64 t0 = now_us();
    int got = 0;
    for (int i = 0; i < count; ++i) {
        if (i) {
            SceUInt64 due = t0 + (SceUInt64)i * period_ms * 1000;
            SceUInt64 t = now_us();
            if (due > t)
                sceKernelDelayThread((SceUInt)(due - t));
        }
        if (grab(3000) < 0)
            break;
        convert(raw + (size_t)one * got, step);
        ++got;
    }
    if (!got) {
        free(raw);
        // Aucune image presentee en 3 s : chargement, gel ou ecran hors
        // s_plat_post_render. La capture USB (udcd_uvc) voit ces cas-la.
        send_str(s, "err pas d'image presentee (chargement ou gel ?)\n");
        return;
    }

    uLongf zlen = compressBound((uLong)one * got);
    unsigned char *z = (unsigned char *)malloc(zlen);
    if (!z || compress2(z, &zlen, raw, (uLong)one * got, 1) != Z_OK) {
        free(raw); free(z);
        send_str(s, "err compression\n");
        return;
    }
    free(raw);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "IMG %d %d %d %lu\n", w, h, got, (unsigned long)zlen);
    if (send_str(s, hdr) == 0)
        send_all(s, z, (int)zlen);
    free(z);
}

// --- maintien ---------------------------------------------------------------

static unsigned int button_bit(const char *t)
{
    static const struct { const char *n; unsigned int b; } tab[] = {
        { "croix", SCE_CTRL_CROSS },    { "cross", SCE_CTRL_CROSS },
        { "rond", SCE_CTRL_CIRCLE },    { "circle", SCE_CTRL_CIRCLE },
        { "carre", SCE_CTRL_SQUARE },   { "square", SCE_CTRL_SQUARE },
        { "triangle", SCE_CTRL_TRIANGLE },
        { "haut", SCE_CTRL_UP },        { "up", SCE_CTRL_UP },
        { "bas", SCE_CTRL_DOWN },       { "down", SCE_CTRL_DOWN },
        { "gauche", SCE_CTRL_LEFT },    { "left", SCE_CTRL_LEFT },
        { "droite", SCE_CTRL_RIGHT },   { "right", SCE_CTRL_RIGHT },
        { "start", SCE_CTRL_START },    { "select", SCE_CTRL_SELECT },
        { "l1", SCE_CTRL_LTRIGGER },    { "l", SCE_CTRL_LTRIGGER },
        { "r1", SCE_CTRL_RTRIGGER },    { "r", SCE_CTRL_RTRIGGER },
        /* pave arriere (issue #30), memes bits que p_siodev.cpp */
        { "l2", 0x01000000 },           { "r2", 0x02000000 },
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); ++i)
        if (!strcmp(t, tab[i].n))
            return tab[i].b;
    return 0;
}

static unsigned char stick(const char *v)
{
    float f = (float)atof(v);
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;
    int i = 128 + (int)(f * 127.0f);
    return (unsigned char)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

static void do_hold(int s, char *args)
{
    unsigned int buttons = 0;
    int has_sticks = 0;
    unsigned char lx = 128, ly = 128, rx = 128, ry = 128;

    char *tok = strtok(args, " \t");
    int ms = tok ? atoi(tok) : 0;
    while ((tok = strtok(NULL, " \t"))) {
        if      (!strncmp(tok, "lx=", 3)) { lx = stick(tok + 3); has_sticks = 1; }
        else if (!strncmp(tok, "ly=", 3)) { ly = stick(tok + 3); has_sticks = 1; }
        else if (!strncmp(tok, "rx=", 3)) { rx = stick(tok + 3); has_sticks = 1; }
        else if (!strncmp(tok, "ry=", 3)) { ry = stick(tok + 3); has_sticks = 1; }
        else {
            unsigned int b = button_bit(tok);
            if (!b) {
                char e[96];
                snprintf(e, sizeof(e), "err touche inconnue : %s\n", tok);
                send_str(s, e);
                return;
            }
            buttons |= b;
        }
    }

    sceKernelLockMutex(s_mtx, 1, NULL);
    s_hold.buttons    = buttons;
    s_hold.has_sticks = has_sticks;
    s_hold.lx = lx; s_hold.ly = ly; s_hold.rx = rx; s_hold.ry = ry;
    s_hold.until      = ms > 0 ? now_us() + (SceUInt64)ms * 1000 : 0;
    sceKernelUnlockMutex(s_mtx, 1);
    send_str(s, "ok\n");
}

// --- journal ----------------------------------------------------------------

static void do_log(int s, const char *arg)
{
    static char buf[64 * 1024];
    long long want = arg ? atoll(arg) : 0;
    unsigned int pos = (want < 0) ? 0xFFFFFFFFu : (unsigned int)want;
    int n = vita_log_ring_read(&pos, buf, sizeof(buf));
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "LOG %u %d\n", pos, n);
    if (send_str(s, hdr) == 0 && n > 0)
        send_all(s, buf, n);
}

// --- boucle -----------------------------------------------------------------

static void handle(int s, char *line)
{
    char *args = line;
    while (*args && *args != ' ' && *args != '\t')
        ++args;
    if (*args)
        *args++ = 0;
    while (*args == ' ' || *args == '\t')
        ++args;

    if (!strcmp(line, "ping")) {
        send_str(s, "ok\n");
    }
    else if (!strcmp(line, "state")) {
        char r[160];
        float ms = s_frame_ms;
        snprintf(r, sizeof(r), "STATE frame=%u fps=%.1f frame_ms=%.1f t_ms=%llu busy=%d hold=%d cmdq=%d\n",
                 s_frames, ms > 0.01f ? 1000.0f / ms : 0.0f, ms,
                 (unsigned long long)(now_us() / 1000), s_busy, hold_active(),
                 (s_cmd_head - s_cmd_tail + CMD_Q) % CMD_Q);
        send_str(s, r);
    }
    else if (!strcmp(line, "shot")) {
        do_shots(s, 1, 0, parse_step(*args ? args : NULL));
    }
    else if (!strcmp(line, "burst")) {
        char *a = strtok(args, " \t");
        char *b = a ? strtok(NULL, " \t") : NULL;
        char *c = b ? strtok(NULL, " \t") : NULL;
        do_shots(s, a ? atoi(a) : 4, b ? atoi(b) : 250, parse_step(c ? c : "half"));
    }
    else if (!strcmp(line, "hold")) {
        do_hold(s, args);
    }
    else if (!strcmp(line, "idle") || !strcmp(line, "frames")) {
        int is_frames = (line[0] == 'f');
        char *a = strtok(args, " \t");
        char *b = a ? strtok(NULL, " \t") : NULL;
        int n = is_frames ? (a ? atoi(a) : 1) : 0;
        int timeout = is_frames ? (b ? atoi(b) : 10000) : (a ? atoi(a) : 30000);
        unsigned int target = s_frames + (unsigned int)n;
        SceUInt64 limit = now_us() + (SceUInt64)timeout * 1000;
        int ok = 0;
        while (now_us() < limit) {
            if (is_frames) {
                if ((int)(s_frames - target) >= 0) { ok = 1; break; }
            } else {
                // Deux images sans rien en cours : une commande juste deposee
                // a eu le temps d'etre lue et de lancer sa sequence.
                static int calm;
                if (!s_busy && !hold_active() && s_cmd_head == s_cmd_tail)
                    ++calm;
                else
                    calm = 0;
                if (calm >= 3) { calm = 0; ok = 1; break; }
            }
            sceKernelDelayThread(16000);
        }
        send_str(s, ok ? "ok\n" : "timeout\n");
    }
    else if (!strcmp(line, "log")) {
        do_log(s, *args ? args : NULL);
    }
    else if (!strcmp(line, "help")) {
        send_str(s, "ok ping state shot burst hold idle frames log help | autre = inject.txt\n");
    }
    else {
        // Tout le reste passe par l'analyseur historique d'inject.txt : ses
        // commandes (touches, unlock, zoom, trace...) restent la reference.
        if (*args)
            line[strlen(line)] = ' ';       // recolle la ligne coupee plus haut
        int next = (s_cmd_head + 1) % CMD_Q;
        if (next == s_cmd_tail) {
            send_str(s, "err file pleine\n");
            return;
        }
        strncpy(s_cmdq[s_cmd_head], line, CMD_LEN - 1);
        s_cmdq[s_cmd_head][CMD_LEN - 1] = 0;
        __sync_synchronize();
        s_cmd_head = next;
        send_str(s, "ok queued\n");
    }
}

static void serve_client(int c)
{
    static char buf[2048];
    int used = 0;
    for (;;) {
        int r = sceNetRecv(c, buf + used, sizeof(buf) - 1 - used, 0);
        if (r <= 0)
            return;
        used += r;
        buf[used] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            if (nl > start && nl[-1] == '\r')
                nl[-1] = 0;
            if (*start)
                handle(c, start);
            start = nl + 1;
        }
        used -= (int)(start - buf);
        memmove(buf, start, used);
        if (used >= (int)sizeof(buf) - 1)
            used = 0;                       // ligne absurde : on jette
    }
}

static int server_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static char netmem[NET_MEM];

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p;
    p.memory = netmem;
    p.size   = NET_MEM;
    p.flags  = 0;
    int r = sceNetInit(&p);
    if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY) {
        VLOG("DBG", "!! sceNetInit 0x%08X : serveur de debug inactif", (unsigned)r);
        return 0;
    }
    sceNetCtlInit();

    int ls = sceNetSocket("thug_dbgsrv", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (ls < 0) {
        VLOG("DBG", "!! socket 0x%08X", (unsigned)ls);
        return 0;
    }
    int one = 1;
    sceNetSetsockopt(ls, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = sizeof(addr);
    addr.sin_family      = SCE_NET_AF_INET;
    addr.sin_port        = sceNetHtons(VITA_DBGSRV_PORT);
    addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    if ((r = sceNetBind(ls, (SceNetSockaddr *)&addr, sizeof(addr))) < 0
        || (r = sceNetListen(ls, 1)) < 0) {
        VLOG("DBG", "!! bind/listen 0x%08X", (unsigned)r);
        sceNetSocketClose(ls);
        return 0;
    }
    VLOG("DBG", "serveur de debug a l'ecoute, port %d", VITA_DBGSRV_PORT);

    for (;;) {
        SceNetSockaddrIn from;
        unsigned int len = sizeof(from);
        int c = sceNetAccept(ls, (SceNetSockaddr *)&from, &len);
        if (c < 0) {
            sceKernelDelayThread(100000);
            continue;
        }
        int nd = 1;
        sceNetSetsockopt(c, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nd, sizeof(nd));
        VLOG("DBG", "client de debug connecte");
        serve_client(c);
        sceNetSocketClose(c);
        VLOG("DBG", "client de debug parti");
    }
    return 0;
}

void vita_dbgsrv_start(void)
{
    s_mtx = sceKernelCreateMutex("thug_dbgsrv", 0, 0, NULL);
    SceUID th = sceKernelCreateThread("thug_dbgsrv", server_thread,
                                      0x10000100, 0x10000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
    else
        VLOG("DBG", "!! thread serveur 0x%08X", (unsigned)th);
}
