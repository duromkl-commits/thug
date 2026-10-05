// THUG-Vita — serveur de debug TCP.
//
// Remplace la boucle « deposer inject.txt par FTP -> attendre le sondage ->
// ecrire un BMP sur la carte -> le rapatrier par FTP ». Chaque etape de cette
// boucle coutait des secondes, et le FTP de la console ne sert qu'un client a
// la fois. Ici, une seule connexion TCP persistante : les touches arrivent a
// la frame suivante, la capture part directement de la memoire, compressee.
//
// Protocole : une commande texte par ligne, voir vita_dbgsrv.c (commande
// « help »). Client cote Mac : vita/tools/vctl.py.
//
// Trois points d'ancrage dans le moteur, tous sur le thread principal :
//   - vita_dbgsrv_frame()        juste avant vglSwapBuffers (p_nx.cpp)
//   - vita_dbgsrv_take_command() et vita_dbgsrv_pad()   dans read_data
//   - vita_dbgsrv_set_busy()     idem, apres l'injection
// Le thread serveur ne touche JAMAIS a l'etat du moteur : il depose des
// demandes, le thread principal les execute.

#ifndef VITA_DBGSRV_H
#define VITA_DBGSRV_H

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_DBGSRV_PORT 9990

void vita_dbgsrv_start(void);

// Une fois par image presentee, AVANT le swap : compte les images et sert
// les demandes de capture.
void vita_dbgsrv_frame(void);

// Rend 1 et copie la prochaine commande texte en attente (a donner a
// l'analyseur d'injection), 0 s'il n'y en a pas.
int vita_dbgsrv_take_command(char *out, int size);

// Manette forcee par « hold » : rend 1 si un maintien est en cours.
typedef struct VitaDbgPad {
    unsigned int  buttons;      // bits SCE_CTRL_*
    int           has_sticks;   // 0 : ne pas toucher aux sticks reels
    unsigned char lx, ly, rx, ry;
} VitaDbgPad;
int vita_dbgsrv_pad(VitaDbgPad *out);

// Une sequence de touches injectees est-elle en cours de rejeu ?
void vita_dbgsrv_set_busy(int busy);

#ifdef __cplusplus
}
#endif

#endif // VITA_DBGSRV_H
