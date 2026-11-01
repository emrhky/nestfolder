# Changelog

## 1.0.3

- Added support for retail firmware 3.60 through 3.74.
- Added support for devkit and testkit firmware 3.600.011 through 3.730.011,
  plus the mapped internal CEX, DEX and devtool SceShell variants.
- SceShell layouts are selected by segment sizes and stable code signatures;
  every private hook site is still validated before its feature is enabled.

## 1.0.2

- Nested folder icons now refresh without a restart. When you change the contents
  of an inner folder and go back, its icon is updated right away.
- The outer folder's icon now shows a small version of the inner folder's icon,
  like it does for apps. Can be turned off with `ux0:data/nestfolder/nosubicon`.
- Installer: Uninstall also clears the titleIds the plugin gave to folders
  (`FLDR` + 5 digits), after moving nested folders back to the home screen.
- Installer app (VPK): install / update, give titleIds to old folders,
  uninstall (moves nested folders to the home screen first), restore first backup.
  Detailed confirmation screen before every action, 5 s cancellable countdown,
  automatic restart.
- One-time backup of `app.db` and `config.txt` in `ux0:data/nestfolder/first_backup/`.
