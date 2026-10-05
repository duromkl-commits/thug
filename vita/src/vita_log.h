// THUG-Vita — logs réseau.
//
// Choix : printf vers le canal de debug du kernel, capté par Cat-A-Log
// (plugin console, envoie en TCP vers le PC, port réglé dans les Réglages).
// Le brief mentionnait debugnet (UDP) : libdebugnet n'est PAS dans le vitasdk
// installé, et la chaîne Cat-A-Log est déjà éprouvée sur cette console
// (projet 9mm). Si Cat-A-Log se révèle trop lossy sous forte charge de log,
// l'alternative est d'installer libdebugnet via vdpm et de rerouter VLOG.
//
// Tout log est préfixé par son sous-système : [SYS] [GFX] [QB] [SND] [NET].

#ifndef VITA_LOG_H
#define VITA_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void vita_log_init(void);
void vita_log_shutdown(void);

/* Demarre le chien de garde : transforme un gel silencieux en psp2core
 * lisible. Outil de debug, a retirer quand le moteur tournera. */
void vita_log_start_watchdog(void);
// Vide le tampon d ecriture sur la carte. Appele a la fermeture et par le
// chien de garde -- sans lui, les dernieres lignes seraient perdues.
void vita_log_flush( void );

void vita_log_printf(const char *sys, const char *fmt, ...);

// Copie du journal en memoire, lue par le serveur de debug (vita_dbgsrv.c)
// pour suivre le log en direct sans passer par le FTP.
// *pos : position en octets depuis le debut du run ; 0xFFFFFFFF = « la fin ».
// Rend le nombre d'octets copies et avance *pos. Si *pos a ete ecrase par le
// tampon circulaire, la lecture repart du plus ancien octet disponible.
int vita_log_ring_read(unsigned int *pos, char *out, int max);

#define VLOG(sys, ...) vita_log_printf(sys, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // VITA_LOG_H
