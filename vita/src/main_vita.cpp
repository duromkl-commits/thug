// THUG-Vita — Palier 0 : squelette.
//
// Ce fichier ne touche PAS au moteur. Son seul rôle est de prouver la chaîne
// outillage de bout en bout :
//   cmake+vitasdk -> eboot.bin -> VPK -> FTP -> vitacompanion launch -> logs
// Il sera remplacé par le vrai point d'entrée du moteur au palier 1.

#include <stdio.h>
#include <stdlib.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>

#include <vitaGL.h>

#include "vita_log.h"

// Réglages mémoire calqués sur un port qui boote réellement sur CETTE console
// (9mm). Le premier jet mettait sceLibcHeapSize à 128 Mio : ces 128 Mio sont
// réservés SÉPARÉMENT du heap newlib, et affamaient vitaGL au point qu'il
// crashait en allouant la texture de son splashscreen.
// sceLibcHeapSize ne sert qu'à SceLibc, dont on ne fait presque rien : 4 Mio.
int sceLibcHeapSize          = 4 * 1024 * 1024;
int _newlib_heap_size_user   = 192 * 1024 * 1024;
// Le moteur fait de grosses allocations sur pile (parsing QB). On monte la
// stack du thread principal maintenant plutôt que de diagnostiquer un crash
// silencieux au palier 2.
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

int main(int argc, char *argv[])
{
    vita_log_init();

    VLOG("SYS", "THUG-Vita — palier 0 (squelette)");
    VLOG("SYS", "build %s %s", __DATE__, __TIME__);

    // 444 MHz = le maximum accessible par l'API officielle (au-delà il faut un
    // plugin type PSVshell). Ce n'est pas de l'overclock : Sony sous-cadence à
    // 333 MHz pour la batterie.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    VLOG("SYS", "horloges: ARM=%d BUS=%d GPU=%d",
         scePowerGetArmClockFrequency(),
         scePowerGetBusClockFrequency(),
         scePowerGetGpuClockFrequency());

    // -- Sonde de diagnostic ------------------------------------------------
    // Deux questions qu'on ne peut PAS trancher depuis le Mac : le serveur FTP
    // de la console ne supporte pas les requêtes range et sature dès qu'on
    // l'interroge en boucle. On les pose donc au système lui-même.

    SceKernelFreeMemorySizeInfo mem;
    mem.size = sizeof(mem);
    if (sceKernelGetFreeMemorySize(&mem) == 0) {
        VLOG("SYS", "RAM libre — MAIN=%d Ko CDRAM=%d Ko PHYCONT=%d Ko",
             mem.size_user / 1024, mem.size_cdram / 1024,
             mem.size_phycont / 1024);
    } else {
        VLOG("SYS", "sceKernelGetFreeMemorySize a echoue");
    }

    // libshacccg.suprx : requis par vitaGL pour compiler du GLSL au runtime.
    // Non redistribuable, il s'extrait de sa propre console. Sa présence n'a
    // jamais été confirmée mécaniquement — on la teste ici.
    static const char *kShaccPaths[] = {
        "ur0:data/libshacccg.suprx",
        "ux0:data/libshacccg.suprx",
        "ur0:data/external/libshacccg.suprx",
        "ux0:data/external/libshacccg.suprx",
        "ur0:tai/libshacccg.suprx",
    };
    int shacc_found = 0;
    for (unsigned i = 0; i < sizeof(kShaccPaths) / sizeof(kShaccPaths[0]); ++i) {
        SceUID f = sceIoOpen(kShaccPaths[i], SCE_O_RDONLY, 0);
        if (f >= 0) {
            SceOff sz = sceIoLseek(f, 0, SCE_SEEK_END);
            sceIoClose(f);
            VLOG("SYS", "libshacccg TROUVE: %s (%lld o)", kShaccPaths[i],
                 (long long)sz);
            shacc_found = 1;
        } else {
            VLOG("SYS", "libshacccg absent: %s (0x%08X)", kShaccPaths[i],
                 (unsigned)f);
        }
    }
    if (!shacc_found)
        VLOG("SYS", "AUCUN libshacccg — la compilation GLSL runtime echouera");

    // vglInitExtended plutôt que vglInit : c'est la forme utilisée par les
    // ports qui tournent sur cette console. legacy_pool_size=0 — le pipeline
    // immédiat (glBegin/glEnd) ne nous servira pas, le moteur soumet des
    // buffers. MSAA désactivé : on veut le chemin le plus simple au palier 0.
    // PIÈGE : la valeur de retour de vglInit* N'EST PAS un statut de succès.
    // Dans vgl.c, elle vaut `res_fallback`, initialisé à GL_FALSE et passé à
    // GL_TRUE uniquement quand la résolution demandée a dû être RABAISSÉE :
    //
    //     GLboolean res_fallback = GL_FALSE;
    //     if (width > max_w || height > max_h) { ...; res_fallback = GL_TRUE; }
    //     return res_fallback;
    //
    // Donc GL_FALSE = tout va bien, GL_TRUE = résolution rognée. Tester
    // `== GL_FALSE` comme une erreur (ce que faisait le premier jet) tue le
    // process sur le chemin nominal. Le port 9mm ignore purement et
    // simplement ce retour — c'est pour ça qu'il boote.
    VLOG("GFX", "vglInitExtended(0, 960x544, 6 Mo, no MSAA)...");
    GLboolean res_fallback = vglInitExtended(0, 960, 544, 6 * 1024 * 1024,
                                             SCE_GXM_MULTISAMPLE_NONE);
    VLOG("GFX", "vglInit rendu -> %d (%s)", (int)res_fallback,
         res_fallback ? "resolution RABAISSEE" : "resolution demandee accordee");

    VLOG("GFX", "GL_VERSION=%s", (const char *)glGetString(GL_VERSION));
    VLOG("GFX", "GL_RENDERER=%s", (const char *)glGetString(GL_RENDERER));

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    // Violet franc : impossible à confondre avec un écran noir (crash) ou
    // avec le fond de LiveArea.
    glClearColor(0.45f, 0.05f, 0.65f, 1.0f);

    VLOG("GFX", "boucle de rendu — START pour quitter");

    SceCtrlData pad;
    unsigned int frame = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) {
            VLOG("SYS", "START pressé — sortie propre");
            break;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);

        // Un battement par seconde : prouve que la boucle vit, sans noyer le
        // log.
        if ((++frame % 60) == 0)
            VLOG("SYS", "frame %u", frame);
    }

    // Pas de vglEnd()/vglTerm() : cette version de vitaGL n'expose aucune
    // fonction de terminaison (vérifié au nm sur libvitaGL.a). On sort du
    // process, le kernel récupère tout.
    VLOG("SYS", "au revoir");
    vita_log_shutdown();

    sceKernelExitProcess(0);
    return 0;
}
