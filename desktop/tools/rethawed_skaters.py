#!/usr/bin/env python3
"""ReTHAWed (THAW PC mod) skater models Marcus wanted (notes 2026-11-13):
THPS1, THPS3 and the THPS4 ones we lacked, plus Trey Kincaid, J. Jonah
Jameson and Joel Jewett.

    rethawed_skaters.py <thug data dir> <out data dir> <THAW data/mod/Models dir>

converts them all with thaw_to_thug.py into models/rethawed/<game>/<key>/.
Keys, names and the folder each lands in are listed here once;
custom_music.cpp's s_gta and skater_roster.py carry matching lines."""

import os
import sys

# (THAW model path, game folder, key, display name, female[, scale])
L = [
    ('newPeds/thps1/thps1_alien', 'thps1', 'rt_thps1_alien', 'THPS1 Alien', 0),
    ('newPeds/thps1/thps1_carrera', 'thps1', 'rt_thps1_carrera', 'THPS1 Private Carrera', 1),
    ('newPeds/thps1/thps1_joel', 'thps1', 'rt_thps1_joel', 'Joel Jewett', 0),
    ('newPeds/thps1/thps1_officer', 'thps1', 'rt_thps1_officer', 'THPS1 Officer Dick', 0),
    ('newPeds/thps1/thps1_rasta', 'thps1', 'rt_thps1_rasta', 'THPS1 Rasta', 0),
    ('newPeds/thps1/thps1_skater', 'thps1', 'rt_thps1_skater', 'THPS1 Skater', 0),
    ('newPros/thps1/thps1_burnquist', 'thps1', 'rt_thps1_burnquist', 'THPS1 Bob Burnquist', 0),
    ('newPros/thps1/thps1_campbell', 'thps1', 'rt_thps1_campbell', 'THPS1 Kareem Campbell', 0),
    ('newPros/thps1/thps1_glifberg', 'thps1', 'rt_thps1_glifberg', 'THPS1 Rune Glifberg', 0),
    ('newPros/thps1/thps1_hawk_dlx', 'thps1', 'rt_thps1_hawk_dlx', 'THPS1 Tony Hawk A', 0),
    ('newPros/thps1/thps1_hawk_psx', 'thps1', 'rt_thps1_hawk_psx', 'THPS1 Tony Hawk B', 0),
    ('newPros/thps1/thps1_lasek', 'thps1', 'rt_thps1_lasek', 'THPS1 Bucky Lasek', 0),
    ('newPros/thps1/thps1_muska', 'thps1', 'rt_thps1_muska', 'THPS1 Chad Muska', 0),
    ('newPros/thps1/thps1_reynolds', 'thps1', 'rt_thps1_reynolds', 'THPS1 Andrew Reynolds', 0),
    ('newPros/thps1/thps1_rowley', 'thps1', 'rt_thps1_rowley', 'THPS1 Geoff Rowley', 0),
    ('newPros/thps1/thps1_steamer', 'thps1', 'rt_thps1_steamer', 'THPS1 Elissa Steamer', 1),
    ('newPros/thps1/thps1_thomas', 'thps1', 'rt_thps1_thomas', 'THPS1 Jamie Thomas', 0),

    ('newPeds/thps3/THPS3Bam', 'thps3', 'rt_thps3_bam', 'THPS3 Bam Margera A', 0),
    ('newPeds/thps3/THPS3Bam_alt', 'thps3', 'rt_thps3_bam_alt', 'THPS3 Bam Margera B', 0),
    ('newPeds/thps3/thps3_bam_psx', 'thps3', 'rt_thps3_bam_psx', 'THPS3 Bam Margera C', 0),
    ('newPeds/thps3/THPS3Cab', 'thps3', 'rt_thps3_cab', 'THPS3 Steve Caballero A', 0),
    ('newPeds/thps3/thps3_cab_psx', 'thps3', 'rt_thps3_cab_psx', 'THPS3 Steve Caballero B', 0),
    ('newPeds/thps3/THPS3Hawk', 'thps3', 'rt_thps3_hawk', 'THPS3 Tony Hawk A', 0),
    ('newPros/thps3/psx_thps3_hawk', 'thps3', 'rt_thps3_hawk_psx', 'THPS3 Tony Hawk B', 0),
    ('newPros/thps3/psx_thps3_hawk_beta', 'thps3', 'rt_thps3_hawk_beta', 'THPS3 Tony Hawk C', 0),
    ('newPeds/thps3/THPS3Kareem', 'thps3', 'rt_thps3_kareem', 'THPS3 Kareem Campbell A', 0),
    ('newPeds/thps3/thps3_campbell_psx', 'thps3', 'rt_thps3_campbell_psx', 'THPS3 Kareem Campbell B', 0),
    ('newPeds/thps3/THPS3Koston', 'thps3', 'rt_thps3_koston', 'THPS3 Eric Koston A', 0),
    ('newPeds/thps3/thps3_koston_psx', 'thps3', 'rt_thps3_koston_psx', 'THPS3 Eric Koston B', 0),
    ('newPeds/thps3/THPS3Lasek', 'thps3', 'rt_thps3_lasek', 'THPS3 Bucky Lasek A', 0),
    ('newPeds/thps3/thps3_lasek_psx', 'thps3', 'rt_thps3_lasek_psx', 'THPS3 Bucky Lasek B', 0),
    ('newPeds/thps3/THPS3Mullen', 'thps3', 'rt_thps3_mullen', 'THPS3 Rodney Mullen A', 0),
    ('newPeds/thps3/thps3_mullen_psx', 'thps3', 'rt_thps3_mullen_psx', 'THPS3 Rodney Mullen B', 0),
    ('newPeds/thps3/THPS3Muska', 'thps3', 'rt_thps3_muska', 'THPS3 Chad Muska A', 0),
    ('newPeds/thps3/thps3_muska_psx', 'thps3', 'rt_thps3_muska_psx', 'THPS3 Chad Muska B', 0),
    ('newPeds/thps3/THPS3Reynolds', 'thps3', 'rt_thps3_reynolds', 'THPS3 Andrew Reynolds A', 0),
    ('newPeds/thps3/thps3_reynolds_psx', 'thps3', 'rt_thps3_reynolds_psx', 'THPS3 Andrew Reynolds B', 0),
    ('newPeds/thps3/THPS3Rowley', 'thps3', 'rt_thps3_rowley', 'THPS3 Geoff Rowley A', 0),
    ('newPeds/thps3/THPS3Rowley_Beta', 'thps3', 'rt_thps3_rowley_beta', 'THPS3 Geoff Rowley B', 0),
    ('newPeds/thps3/thps3_rowley_psx', 'thps3', 'rt_thps3_rowley_psx', 'THPS3 Geoff Rowley C', 0),
    ('newPeds/thps3/THPS3Rune', 'thps3', 'rt_thps3_rune', 'THPS3 Rune Glifberg A', 0),
    ('newPeds/thps3/thps3_glifberg_psx', 'thps3', 'rt_thps3_glifberg_psx', 'THPS3 Rune Glifberg B', 0),
    ('newPeds/thps3/THPS3Steamer', 'thps3', 'rt_thps3_steamer', 'THPS3 Elissa Steamer A', 1),
    ('newPeds/thps3/thps3_steamer_psx', 'thps3', 'rt_thps3_steamer_psx', 'THPS3 Elissa Steamer B', 1),
    ('newPeds/thps3/THPS3Thomas', 'thps3', 'rt_thps3_thomas', 'THPS3 Jamie Thomas A', 0),
    ('newPeds/thps3/thps3_thomas_psx', 'thps3', 'rt_thps3_thomas_psx', 'THPS3 Jamie Thomas B', 0),
    ('newPeds/THPS3_LABum', 'thps3', 'rt_thps3_labum', 'THPS3 LA Bum', 0),
    ('newPeds/thps3/si_Pedestrian_A', 'thps3', 'rt_thps3_ped_a', 'THPS3 Pedestrian A', 0),
    ('newPeds/thps3/si_Pedestrian_B', 'thps3', 'rt_thps3_ped_b', 'THPS3 Pedestrian B', 0),
    ('newPeds/thps3/si_Pedestrian_C', 'thps3', 'rt_thps3_ped_c', 'THPS3 Pedestrian C', 0),
    ('newPeds/thps3/thps3_carrera', 'thps3', 'rt_thps3_carrera', 'THPS3 Private Carrera A', 1),
    ('newPeds/thps3/thps3_carrera_psx', 'thps3', 'rt_thps3_carrera_psx', 'THPS3 Private Carrera B', 1),
    ('newPeds/thps3/thps3_officer_dick', 'thps3', 'rt_thps3_officer_dick', 'THPS3 Officer Dick A', 0),
    ('newPeds/thps3/thps3_officer_dick_psx', 'thps3', 'rt_thps3_officer_dick_psx', 'THPS3 Officer Dick B', 0),
    ('newPeds/thps3/thps3_pirate', 'thps3', 'rt_thps3_pirate', 'THPS3 Pirate', 0),
    ('newPeds/thps3/thps3_rasta', 'thps3', 'rt_thps3_rasta', 'THPS3 Rasta', 0),
    ('newPeds/thps3/thps3_slater', 'thps3', 'rt_thps3_slater', 'THPS3 Kelly Slater', 0),
    ('newPeds/thps3/thps3_thinman', 'thps3', 'rt_thps3_thinman', 'THPS3 Thin Man A', 0),
    ('newPeds/thps3/thps3_thinman_beta', 'thps3', 'rt_thps3_thinman_beta', 'THPS3 Thin Man B', 0),
    ('newPeds/thps3_wolverine', 'thps3', 'rt_thps3_wolverine', 'THPS3 Wolverine A', 0),
    ('newPeds/thps3/thps3_wolverine_psx', 'thps3', 'rt_thps3_wolverine_psx', 'THPS3 Wolverine B', 0),
    ('newPeds/thps3_demoness', 'thps3', 'rt_thps3_demoness', 'THPS3 Demoness A', 1),
    ('newPeds/thps3_demoness_inferno', 'thps3', 'rt_thps3_demoness_inferno', 'THPS3 Demoness B', 1),
    ('newPeds/thps3_doomguy', 'thps3', 'rt_thps3_doomguy', 'THPS3 Doom Guy', 0),
    ('newPeds/thps3_eyeball_bride', 'thps3', 'rt_thps3_eyeball_bride', 'THPS3 Eyeball Bride', 1),
    ('newPeds/thps3_maul', 'thps3', 'rt_thps3_maul', 'THPS3 Darth Maul', 0),
    ('newPeds/thps3_neversoft_guy', 'thps3', 'rt_thps3_neversoft_guy', 'THPS3 Neversoft Guy', 0),
    ('newPeds/thps3_ollie', 'thps3', 'rt_thps3_ollie', 'THPS3 Ollie the Magic Bum', 0),
    ('newPeds/thps3_skater_x', 'thps3', 'rt_thps3_skater_x', 'THPS3 Skater X', 0),

    ('newPeds/thps4/thps4_burnquist', 'thps4', 'rt_thps4_burnquist', 'THPS4 Bob Burnquist', 0),
    ('newPeds/thps4/thps4_caballero', 'thps4', 'rt_thps4_caballero', 'THPS4 Steve Caballero', 0),
    ('newPeds/thps4/thps4_campbell', 'thps4', 'rt_thps4_campbell', 'THPS4 Kareem Campbell', 0),
    ('newPeds/thps4/thps4_glifberg', 'thps4', 'rt_thps4_glifberg', 'THPS4 Rune Glifberg', 0),
    ('newPeds/thps4/thps4_hawk', 'thps4', 'rt_thps4_hawk', 'THPS4 Tony Hawk A', 0),
    ('newPeds/thps4/thps4_hawk_beta', 'thps4', 'rt_thps4_hawk_beta', 'THPS4 Tony Hawk B', 0),
    ('newPeds/thps4/thps4_koston', 'thps4', 'rt_thps4_koston', 'THPS4 Eric Koston', 0),
    ('newPeds/thps4/thps4_lasek', 'thps4', 'rt_thps4_lasek', 'THPS4 Bucky Lasek', 0),
    ('newPeds/thps4/thps4_margera', 'thps4', 'rt_thps4_margera', 'THPS4 Bam Margera', 0),
    ('newPeds/thps4/thps4_mullen', 'thps4', 'rt_thps4_mullen', 'THPS4 Rodney Mullen', 0),
    ('newPeds/thps4/thps4_muska', 'thps4', 'rt_thps4_muska', 'THPS4 Chad Muska', 0),
    ('newPeds/thps4/thps4_reynolds', 'thps4', 'rt_thps4_reynolds', 'THPS4 Andrew Reynolds', 0),
    ('newPeds/thps4/thps4_rowley', 'thps4', 'rt_thps4_rowley', 'THPS4 Geoff Rowley', 0),
    ('newPeds/thps4/thps4_steamer', 'thps4', 'rt_thps4_steamer', 'THPS4 Elissa Steamer', 1),
    ('newPeds/thps4/thps4_thomas', 'thps4', 'rt_thps4_thomas', 'THPS4 Jamie Thomas', 0),
    ('newPeds/thps4/thps4_vallely', 'thps4', 'rt_thps4_vallely', 'THPS4 Mike Vallely', 0),
    ('newPeds/thps4/thps4_chef', 'thps4', 'rt_thps4_chef', 'Chef', 0),
    ('newPeds/thps4/thps4_clown_a', 'thps4', 'rt_thps4_clown_a', 'THPS4 Clown A', 0, 0.7),
    ('newPeds/thps4/thps4_clown_a_hd', 'thps4', 'rt_thps4_clown_a_hd', 'THPS4 Clown B', 0),
    ('newPeds/thps4/thps4_clown_b', 'thps4', 'rt_thps4_clown_b', 'THPS4 Clown C', 0, 0.7),
    ('newPeds/thps4/thps4_daisy', 'thps4', 'rt_thps4_daisy', 'THPS4 Daisy', 1),
    ('newPeds/thps4/thps4_eddie', 'thps4', 'rt_thps4_eddie', 'THPS4 Eddie', 0),
    ('newPeds/thps4/thps4_jango', 'thps4', 'rt_thps4_jango', 'THPS4 Jango Fett', 0),
    ('newPeds/thps4/thps4_kenny', 'thps4', 'rt_thps4_kenny', 'THPS4 Kenny', 0),

    ('newPeds/bruce', 'other', 'rt_trey_kincaid', 'Trey Kincaid', 0),
    ('newPeds/sm2000/sm2000_jameson', 'other', 'rt_jjj', 'J. Jonah Jameson', 0),
]

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import thaw_to_thug
    data, out, models = sys.argv[1:4]
    for path, game, key, name, fem, *_ in L:
        nv, nt, a, b = thaw_to_thug.convert(data, out, os.path.join(models, path), 'rethawed/' + game, key)
        print('%-28s %5d v %5d t  h %.1f' % (key, nv, nt, b[1] - a[1]))
