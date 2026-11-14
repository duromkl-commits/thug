#!/usr/bin/env python3
"""Writes clean-skaters.ps1 (+ .bat): deletes skater model folders and bail voice
files the current exe no longer lists (scrapped skaters, past and present).

    gen_cleanup.py <out dir> <voice_banks.py output dir>

Only touches our own folders: data/models/<the skater folders> and
data/voices/*.txt bail files + data/voices/own. Keep list = s_gta in
custom_music.cpp and skater_roster.py."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', 'src', 'custom_music.cpp')).read()
src = src[src.index('s_gta[] = {'):]
src = src[:src.index('\n};')]
keep = {}
for nom, dos in re.findall(r'^\s*\{ "([^"]+)", "[^"]*", \d+(?:, "([^"]+)")?', src, re.M):
    keep.setdefault(dos or 'gta', set()).add(nom.lower())
sys.path.insert(0, here)
from skater_roster import R  # noqa: E402
voices = sorted(k.lower() for k in R)
vdir = sys.argv[2]
own = sorted({l.split('/')[1].lower() for f in os.listdir(vdir) if f.endswith('.txt')
              for l in open(os.path.join(vdir, f)) if l.startswith('own/')})
# every skater folder we ever shipped
roots = sorted(set(keep) | {'gta', 'rdr', 'thps2', 'cinc', 'rtcw', 'kingpin', 'maxpayne', 'quake', 'quake2', 'hrot',
                            'rethawed/thps1', 'rethawed/thps3', 'rethawed/thps4', 'rethawed/other', 'rethawed/mods'})
L = ['# Deletes scrapped skaters (models + bail voices) from THUG. Put this next to thug.exe and run clean-skaters.bat.',
     'param([switch]$Yes)', '$ErrorActionPreference = "Stop"',
     '$data = Join-Path $PSScriptRoot "data"',
     'if (-not (Test-Path (Join-Path $data "models"))) { Write-Host "No data\\models next to this script. Put it in the game folder (next to thug.exe)."; exit 1 }',
     '$keep = @{']
for r in roots:
    L.append('  "%s" = @(%s)' % (r, ','.join('"%s"' % k for k in sorted(keep.get(r, ())))))
L += ['}', '$voices = @(%s)' % ','.join('"%s"' % v for v in voices),
      '$kv = @{}; foreach ($v in $voices) { $kv[$v] = 1 }',
      '$ko = @{}; foreach ($v in @(%s)) { $ko[$v] = 1 }' % ','.join('"%s"' % o for o in own),
      '$gone = New-Object System.Collections.ArrayList',
      'foreach ($r in $keep.Keys) {',
      '  $d = Join-Path $data ("models\\" + ($r -replace "/", "\\"))',
      '  if (-not (Test-Path $d)) { continue }',
      '  $k = @{}; foreach ($n in $keep[$r]) { $k[$n] = 1 }',
      '  foreach ($f in Get-ChildItem -LiteralPath $d -Directory) {',
      '    if ($r -eq "rethawed") { continue }',
      '    if (-not $k.ContainsKey($f.Name.ToLower())) { [void]$gone.Add($f.FullName) }',
      '  }',
      '}',
      '$vd = Join-Path $data "voices"',
      'if (Test-Path $vd) {',
      '  foreach ($f in Get-ChildItem -LiteralPath $vd -File -Filter *.txt) {',
      '    $first = Get-Content -LiteralPath $f.FullName -TotalCount 1',
      '    if ($first -like "*bail voice clips*" -and -not $kv.ContainsKey($f.BaseName.ToLower())) { [void]$gone.Add($f.FullName) }',
      '  }',
      '  $own = Join-Path $vd "own"',
      '  if (Test-Path $own) { foreach ($f in Get-ChildItem -LiteralPath $own -Directory) { if (-not $ko.ContainsKey($f.Name.ToLower())) { [void]$gone.Add($f.FullName) } } }',
      '}',
      'if ($gone.Count -eq 0) { Write-Host "Nothing to clean."; exit 0 }',
      '$gone | ForEach-Object { Write-Host $_ }',
      'Write-Host ""; Write-Host ("{0} folders/files above will be deleted." -f $gone.Count)',
      'if (-not $Yes) { $a = Read-Host "Delete them? (y/n)"; if ($a -ne "y") { Write-Host "Nothing deleted."; exit 0 } }',
      'foreach ($p in $gone) { Remove-Item -LiteralPath $p -Recurse -Force }',
      'foreach ($r in $keep.Keys) { $d = Join-Path $data ("models\\" + ($r -replace "/", "\\")); if ((Test-Path $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) { Remove-Item -LiteralPath $d -Force } }',
      '$rt = Join-Path $data "models\\rethawed"; if ((Test-Path $rt) -and -not (Get-ChildItem -LiteralPath $rt -Force)) { Remove-Item -LiteralPath $rt -Force }',
      'Write-Host "Done."']
out = sys.argv[1]
open(os.path.join(out, 'clean-skaters.ps1'), 'w', newline='\r\n').write('\n'.join(L) + '\n')
open(os.path.join(out, 'clean-skaters.bat'), 'w', newline='\r\n').write(
    '@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean-skaters.ps1"\r\npause\r\n')
print('kept models', sum(len(v) for v in keep.values()), 'voices', len(voices), 'roots', len(roots))
