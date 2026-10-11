#include "vita_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

// Deux canaux, volontairement redondants :
//
//   1. sceClibPrintf -> canal de debug du kernel -> plugin Cat-A-Log -> TCP.
//      Temps réel, pratique, mais dépend d'un plugin dont l'état sur la
//      console n'est pas sous notre contrôle (la première tentative de boot
//      n'a produit AUCUNE ligne alors que le main tournait).
//
//   2. ux0:data/thug/thug.log, écrit par sceIo et flushé à chaque ligne.
//      Ne dépend d'aucun plugin, se récupère par FTP après coup. C'est le
//      canal de vérité quand le canal 1 est muet ou que le crash est brutal.
//
// Le coût du flush systématique est assumé : un log bufferisé perdu dans un
// crash est exactement le log qu'on voulait lire.

#define LOG_DIR  "ux0:data/thug"
#define LOG_PATH LOG_DIR "/thug.log"

static SceUID    s_fd = -1;
static SceUInt64 s_t0 = 0;

// Compteur de lignes : c'est le pouls du programme, lu par le chien de garde.
static volatile unsigned int s_lines = 0;

/* Tampon d'ecriture : 16 Ko, soit ~200 lignes par acces SD au lieu d'un par
 * ligne. */
static char s_buf[16384];
static int  s_buf_used = 0;

/* Copie circulaire pour le serveur de debug. Le journal est ecrit par
 * plusieurs threads (audio, chien de garde) : l'anneau est donc protege,
 * contrairement au tampon ci-dessus qui ne l'a jamais ete. */
/* 2 Mo : a 256 Ko, les traces QB d'un chargement chassaient en quelques
 * secondes les lignes qu'on cherchait (confirmation d'« unlock », demarrage
 * des cinematiques). */
#define RING_SIZE (2 * 1024 * 1024)
static char                  s_ring[RING_SIZE];
static volatile unsigned int s_ring_total = 0;   /* octets ecrits depuis le debut */
static SceUID                s_ring_mtx   = -1;

int vita_log_ring_read(unsigned int *pos, char *out, int max)
{
    if (s_ring_mtx < 0)
        return 0;
    sceKernelLockMutex(s_ring_mtx, 1, NULL);
    unsigned int total = s_ring_total;
    unsigned int p     = *pos;
    if (p > total)
        p = total;                              /* « la fin » */
    if (total - p > RING_SIZE)
        p = total - RING_SIZE;                  /* ecrase : plus ancien dispo */
    int n = (int)(total - p);
    if (n > max)
        n = max;
    for (int i = 0; i < n; ++i)
        out[i] = s_ring[(p + i) % RING_SIZE];
    sceKernelUnlockMutex(s_ring_mtx, 1);
    *pos = p + n;
    return n;
}

void vita_log_flush(void)
{
    if (s_fd >= 0 && s_buf_used > 0)
    {
        sceIoWrite(s_fd, s_buf, s_buf_used);
        s_buf_used = 0;
    }
}

void vita_log_init(void)
{
    s_t0 = sceKernelGetProcessTimeWide();
    s_ring_mtx = sceKernelCreateMutex("thug_log_ring", 0, 0, NULL);

    sceIoMkdir(LOG_DIR, 0777);
    // Troncature à l'ouverture : le log d'un run ne se mélange pas au
    // précédent, sinon on debugge le run d'avant sans le savoir.
    s_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

// ---------------------------------------------------------------------------
// Chien de garde.
//
// Un moteur fige sans plantage ne laisse RIEN : pas de psp2core, donc pas de
// pile, donc rien a lire. Or le dump de la Vita contient le PC de TOUS les
// threads. Il suffit donc qu'un thread annexe se saborde pour obtenir la
// position exacte du thread principal.
//
// Declenchement : aucune ligne de log ecrite depuis WATCHDOG_TIMEOUT_S.
// C'est un OUTIL DE DEBUG, pas un mecanisme de production -- il transforme
// deliberement un gel silencieux en plantage lisible.
// ---------------------------------------------------------------------------
#define WATCHDOG_TIMEOUT_S 45

static int vita_watchdog_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;

    unsigned int last  = 0;
    int          still = 0;

    // Controle positif : sans cette ligne, l'absence de reaction du chien de
    // garde serait ambigue (thread mort ? ou rien a signaler ?).
    vita_log_printf("WDOG", "chien de garde demarre (seuil %d s)",
                    WATCHDOG_TIMEOUT_S);

    for (;;) {
        sceKernelDelayThread(5 * 1000 * 1000);

        // Empeche la mise en veille automatique.
        //
        // Sans cela, la console suspend le PROCESSUS ENTIER pendant les longs
        // chargements sans interaction. Vu du PC c'est indiscernable d'un
        // blocage : le log s'arrete net, son horodatage vient du temps
        // processus qui n'avance plus, et meme ce thread-ci cesse de tourner.
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);

        unsigned int now = s_lines;
        if (now != last) {
            last  = now;
            still = 0;
            continue;
        }

        still += 5;
        if (still < WATCHDOG_TIMEOUT_S)
            continue;

        vita_log_printf("WDOG", "aucune ligne depuis %d s -- dump provoque",
                        still);
        // Le dump qui suit contiendra le PC du thread principal : c'est tout
        // l'interet de la manoeuvre.
        *(volatile int *)0 = 0;
    }
    return 0;
}

void vita_log_start_watchdog(void)
{
#ifdef THUG_DESKTOP
    /* A PC debugger beats a provoked crash dump. */
    return;
#endif
    SceUID th = sceKernelCreateThread("thug_watchdog", vita_watchdog_thread,
                                      0x10000100, 0x4000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

void vita_log_shutdown(void)
{
    if (s_fd >= 0) {
        vita_log_flush();
    sceIoClose(s_fd);
        s_fd = -1;
    }
}

#ifdef THUG_DESKTOP
void desktop_io_flush(SceUID fd);
#endif

#ifdef THUG_RELEASE
/* Build public : seules les lignes utiles a un rapport de bug (demarrage,
 * erreurs "!!", plantages) ; le reste n'est meme pas formate. */
static int vita_log_retenu(const char *sys, const char *fmt)
{
    static const char *garder[] = { "SYS", "BOOT", "GFX", "WDOG", "ERR", "ASSERT", "CRASH", "LVL", "DSK", "PCM", "LOAD", NULL };
    if (fmt && fmt[0] == '!' && fmt[1] == '!')
        return 1;
    for (int i = 0; garder[i]; ++i)
        if (sys && !strcmp(sys, garder[i]))
            return 1;
    return 0;
}
#endif

void vita_log_printf(const char *sys, const char *fmt, ...)
{
#ifdef THUG_RELEASE
    if (!vita_log_retenu(sys, fmt))
        return;
#endif
    char body[512];
    char line[640];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SceUInt64 ms = (sceKernelGetProcessTimeWide() - s_t0) / 1000;
    int n = snprintf(line, sizeof(line), "[%5llu.%03llu] [%s] %s\n",
                     (unsigned long long)(ms / 1000),
                     (unsigned long long)(ms % 1000), sys, body);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 1)
        n = (int)sizeof(line) - 1;

    /* Ecriture TAMPONNEE.
     *
     * Chaque ligne coutait deux appels systeme : un sceClibPrintf vers le port
     * de debogage (que personne n'ecoute en general) et un sceIoWrite sur la
     * carte SD. Mesure pendant un chargement de niveau : 4009 lignes de traces
     * QB a elles seules, soit ~8000 appels systeme et autant d'acces SD --
     * plus de la moitie du temps de chargement passait la.
     *
     * On accumule donc, et on ne touche la carte que par blocs. Le tampon est
     * vide a la fermeture (vita_log_close) et des qu'il est plein ; en cas de
     * plantage on perd au pire les dernieres lignes, ce qui est le compromis
     * habituel -- et le chien de garde, lui, force un vidage. */
#ifdef THUG_DESKTOP
    /* Desktop: straight to the file, so a hung or killed game still leaves
     * its log (a PC write is cheap; the Vita's SD card was not). */
    if (s_fd >= 0)
    {
        sceIoWrite(s_fd, line, n);
        desktop_io_flush(s_fd);
    }
    else
#endif
    if (s_fd >= 0)
    {
        if (s_buf_used + n > (int)sizeof(s_buf))
        {
            sceIoWrite(s_fd, s_buf, s_buf_used);
            s_buf_used = 0;
        }
        if (n <= (int)sizeof(s_buf))
        {
            memcpy(s_buf + s_buf_used, line, n);
            s_buf_used += n;
        }
        else
        {
            sceIoWrite(s_fd, line, n);
        }
    }

#ifdef THUG_DESKTOP
    /* Desktop: the console window shows the log live. */
    fwrite(line, 1, n, stdout);
#endif

    if (s_ring_mtx >= 0)
    {
        sceKernelLockMutex(s_ring_mtx, 1, NULL);
        unsigned int t = s_ring_total;
        for (int i = 0; i < n; ++i)
            s_ring[(t + i) % RING_SIZE] = line[i];
        s_ring_total = t + n;
        sceKernelUnlockMutex(s_ring_mtx, 1);
    }

    ++s_lines;
}
