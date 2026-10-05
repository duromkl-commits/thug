#!/usr/bin/env python3
"""Genere la LiveArea et l'ecran de lancement depuis le dump du jeu.

    THUG_DATA=<dossier data/ de l'ISO Xbox USA> python3 vita/livearea/generer.py

Les visuels reprennent le VRAI logo et l'illustration du jeu (accord de
l'humain, 2026-10-05), avec une etiquette "VITA" ajoutee. Ce sont donc des
donnees du jeu : les fichiers produits sont IGNORES par git (.gitignore) et
regeneres a partir du dump de chacun. Seuls ce script, livearea.html et
template.xml sont versionnes.

Sources (dans le dossier data/ de l'ISO Xbox USA) :
  images/loadscrn_ngc.img.xbx              ecran-titre : logo orange, texte legal
  images/loadscrn_mainmenu.img.xbx         illustration du menu (etoile, skateur)
  images/Miscellaneous/xbox_icon_title.xbx badge rond du tableau de bord Xbox

Sorties :
  vita/sce_sys/icon0.png                      128x128  bulle
  vita/sce_sys/pic0.png                       960x544  zoom de lancement (systeme)
  vita/sce_sys/livearea/contents/bg.png       840x500  fond de la LiveArea (style a1)
  vita/sce_sys/livearea/contents/startup.png  280x158  porte
  vita/boot/ecran.bin                         l'image de pic0 en RGBA brut, affichee
                                              des main() (vita/src/vita_ecran_boot.c)

PNG 8 bits INDEXES, sans transparence, non entrelaces (sinon 0x8010113D a
l'installation) ; bits=8 force et verifie dans l'en-tete : Pillow reduit la
profondeur d'une image a peu de couleurs. Dependances : Pillow, numpy,
opencv-python (effacement du texte legal), playwright + Chromium, reseau pour
la police Big Shoulders Stencil Display (OFL) au rendu.
"""
import os
import shutil
import struct
import sys
import tempfile
import zlib

import numpy as np
from PIL import Image, ImageFilter

ICI = os.path.dirname(os.path.abspath(__file__))
VITA = os.path.normpath(os.path.join(ICI, ".."))
DATA = os.environ.get("THUG_DATA", "")	# dossier data/ de l'ISO Xbox USA
SCE = os.path.join(VITA, "sce_sys")
SORTIES = {
    "icon0":   (os.path.join(SCE, "icon0.png"), (128, 128)),
    "pic0":    (os.path.join(SCE, "pic0.png"), (960, 544)),
    "bg":      (os.path.join(SCE, "livearea", "contents", "bg.png"), (840, 500)),
    "startup": (os.path.join(SCE, "livearea", "contents", "startup.png"), (280, 158)),
}
BOOT = os.path.join(VITA, "boot", "ecran.bin")


# --- decodage des formats du jeu -------------------------------------------

def morton(w, h):
    """Ordre de desentrelacement : unswizzle() de Code/Gfx/Vita/p_NxTexture.cpp."""
    idx = np.zeros(w * h, dtype=np.int64)
    for pixel in range(w * h):
        hw, hh, x, y, bx, by, bit = w // 2, h // 2, 0, 0, 1, 1, 1
        while hw or hh:
            if hw:
                hw //= 2
                if pixel & bit:
                    x |= bx
                bx *= 2
                bit *= 2
            if hh:
                hh //= 2
                if pixel & bit:
                    y |= by
                by *= 2
                bit *= 2
        idx[pixel] = x + y * w
    return idx


def img_xbx(chemin):
    """.img.xbx : LoadVitaImgInto (p_NxTexture.cpp). Stocke de bas en haut."""
    d = open(chemin, "rb").read()
    _, _, w, h, prof, _ = struct.unpack_from("<6I", d, 0)
    ow, oh = struct.unpack_from("<HH", d, 24)
    ps, = struct.unpack_from("<I", d, 28)
    bpp = {0x00: 32, 0x02: 16, 0x13: 8}[prof]
    o, n = 32 + ps, w * h
    brut = np.frombuffer(d[o:o + n * bpp // 8], dtype=np.uint8).reshape(n, bpp // 8)
    if not (w & (w - 1)) and not (h & (h - 1)):
        out = np.zeros_like(brut)
        out[morton(w, h)] = brut
        brut = out
    if bpp == 8:
        pal = np.frombuffer(d[32:32 + ps], dtype=np.uint8).reshape(-1, 4)[brut[:, 0]]
        rgba = np.stack([pal[:, 2], pal[:, 1], pal[:, 0], pal[:, 3]], 1)
    elif bpp == 32:
        rgba = np.stack([brut[:, 2], brut[:, 1], brut[:, 0], brut[:, 3]], 1)
    else:
        v = brut[:, 0].astype(np.uint16) | (brut[:, 1].astype(np.uint16) << 8)
        rgba = np.stack([((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
                         (v & 31) * 255 // 31, np.where(v & 0x8000, 255, 0)], 1).astype(np.uint8)
    im = Image.fromarray(rgba.reshape(h, w, 4).astype(np.uint8))
    if ow and oh and ow <= w and oh <= h:
        im = im.crop((0, 0, ow, oh))
    return im.transpose(Image.FLIP_TOP_BOTTOM).convert("RGB")


def xpr_dxt1(chemin):
    """Texture XPR0 de la Xbox, DXT1 128x128 (xbox_icon_title.xbx)."""
    d = open(chemin, "rb").read()
    assert d[:4] == b"XPR0"
    entete, = struct.unpack_from("<I", d, 8)
    fmt, = struct.unpack_from("<I", d, 0x18)
    assert (fmt >> 8) & 0xFF == 0x0C, "pas du DXT1"
    w, h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
    src, out, o = d[entete:], np.zeros((h, w, 3), np.uint8), 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from("<HHI", src, o)
            o += 8

            def rgb(c):
                return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                                 (c & 31) * 255 // 31])
            p0, p1 = rgb(c0), rgb(c1)
            cols = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3] if c0 > c1 else \
                   [p0, p1, (p0 + p1) // 2, np.zeros(3, int)]
            for i in range(16):
                out[by + i // 4, bx + i % 4] = cols[(bits >> (2 * i)) & 3]
    return Image.fromarray(out)


def effacer_texte_legal(im):
    """Inpainting du bloc de mentions legales de loadscrn_ngc (gris ~98 sur ~28)."""
    import cv2
    a = cv2.cvtColor(np.asarray(im), cv2.COLOR_RGB2BGR)
    v = cv2.cvtColor(a, cv2.COLOR_BGR2HSV)[:, :, 2]
    zone = np.zeros(v.shape, np.uint8)
    cv2.rectangle(zone, (40, 316), (605, 408), 255, -1)
    m = cv2.bitwise_and(((v > 55) * 255).astype(np.uint8), zone)
    m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=2)
    b = cv2.inpaint(a, m, 7, cv2.INPAINT_TELEA)
    return Image.fromarray(cv2.cvtColor(b, cv2.COLOR_BGR2RGB))


# --- ecriture PNG ------------------------------------------------------------

def ecrire_png8(q, chemin):
    """PNG 8 bits indexe dans la forme EXACTE des LiveArea qui s'importent
    (VitaShell, gtasa_vita) : IHDR, PLTE de 256 entrees, UN SEUL IDAT, filtre 0
    sur chaque ligne, zlib 9, IEND. Pillow coupe IDAT tous les 64 Ko : notre
    bg.png en avait 3 et la LiveArea n'etait pas importee (ur0:appmeta ne
    recevait que icon0.png)."""
    w, h = q.size
    pal = (q.getpalette() or [])[:768]
    pal += [0] * (768 - len(pal))
    px = q.tobytes()
    brut = b"".join(b"\x00" + px[y * w:(y + 1) * w] for y in range(h))

    def bloc(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(chemin, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(bloc(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)))
        f.write(bloc(b"PLTE", bytes(pal)))
        f.write(bloc(b"IDAT", zlib.compress(brut, 9)))
        f.write(bloc(b"IEND", b""))


# --- verification ------------------------------------------------------------

def verifier(chemin, taille):
    with open(chemin, "rb") as f:
        d = f.read(33)
    w, h, prof, typ, _, _, entrel = struct.unpack(">IIBBBBB", d[16:29])
    ok = d[:8] == b"\x89PNG\r\n\x1a\n" and (w, h) == taille and prof == 8 and typ == 3 and entrel == 0
    ok = ok and open(chemin, "rb").read().count(b"IDAT") == 1
    print("%-48s %dx%d prof %d type %d entrel %d %7d o  %s" % (
        os.path.relpath(chemin, VITA), w, h, prof, typ, entrel, os.path.getsize(chemin),
        "OK" if ok else "!! REFUSE"))
    return ok


def main():
    from playwright.sync_api import sync_playwright
    if not DATA or not os.path.isdir(DATA):
        sys.exit("THUG_DATA=<dossier data/ de l'ISO Xbox USA> python3 vita/livearea/generer.py")
    tmp = tempfile.mkdtemp()
    L = Image.LANCZOS
    carte = effacer_texte_legal(img_xbx(os.path.join(DATA, "images", "loadscrn_ngc.img.xbx")))
    carte.resize((960, 672), L).filter(ImageFilter.UnsharpMask(1.0, 55, 2)).save(os.path.join(tmp, "src_pic0.png"))
    # Porte : plaque du logo (x85-570, y60-300) et, derriere, la texture bleue
    # de la carte (bande sans logo du haut, agrandie et assombrie).
    carte.crop((85, 60, 570, 300)).resize((222, 110), L).filter(
        ImageFilter.UnsharpMask(0.8, 40, 2)).save(os.path.join(tmp, "src_porte_logo.png"))
    fond = carte.crop((0, 300, 640, 448)).resize((280, 158), L).filter(ImageFilter.GaussianBlur(1.5))
    Image.blend(fond, Image.new("RGB", fond.size, (0, 0, 0)), 0.35).save(os.path.join(tmp, "src_porte_fond.png"))
    img_xbx(os.path.join(DATA, "images", "loadscrn_mainmenu.img.xbx")).resize((928, 650), L).filter(
        ImageFilter.UnsharpMask(1.2, 50, 2)).save(os.path.join(tmp, "src_bg.png"))
    xpr_dxt1(os.path.join(DATA, "images", "Miscellaneous", "xbox_icon_title.xbx")).save(
        os.path.join(tmp, "src_icon0.png"))
    shutil.copy(os.path.join(ICI, "livearea.html"), tmp)

    with sync_playwright() as p:
        nav = p.chromium.launch()
        page = nav.new_page(viewport={"width": 1100, "height": 1700}, device_scale_factor=1)
        page.goto("file://" + os.path.join(tmp, "livearea.html"))
        page.wait_for_timeout(2500)
        if not page.evaluate("document.fonts.check(\"900 40px 'Big Shoulders Stencil Display'\")"):
            sys.exit("police non chargee (reseau ?)")
        for nom in SORTIES:
            page.locator("#" + nom).screenshot(path=os.path.join(tmp, nom + ".png"))
        nav.close()

    tout_ok = True
    for nom, (chemin, taille) in SORTIES.items():
        im = Image.open(os.path.join(tmp, nom + ".png")).convert("RGB")
        assert im.size == taille, (nom, im.size)
        os.makedirs(os.path.dirname(chemin), exist_ok=True)
        ecrire_png8(im.quantize(colors=256, method=Image.Quantize.FASTOCTREE), chemin)
        tout_ok &= verifier(chemin, taille)
        if nom == "pic0":
            # Brut et non compresse : lu d'un bloc au demarrage en ~0,17 s,
            # la decompression zlib en coutait 0,7 (mesure console).
            brut = im.convert("RGBA").tobytes()
            os.makedirs(os.path.dirname(BOOT), exist_ok=True)
            with open(BOOT, "wb") as f:
                f.write(b"THBR" + struct.pack("<HHI", 960, 544, len(brut)) + brut)
            print("%-48s %d o (RGBA brut)" % (os.path.relpath(BOOT, VITA), os.path.getsize(BOOT)))
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if tout_ok else 1)


if __name__ == "__main__":
    main()
