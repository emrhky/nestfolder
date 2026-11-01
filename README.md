# nestfolder

Folders inside folders on the PS Vita home screen (LiveArea).

The stock Vita lets you group apps into folders, but a folder can't hold another
folder. nestfolder adds that: you can put a folder inside a folder, open it, and
manage it like any other folder.

> **Supports retail firmware 3.60–3.74 and devkit/testkit firmware
> 3.600.011–3.730.011, including the mapped internal SceShell variants.**

## Features

- Open a folder inside another folder (two levels). Circle or tapping outside goes back.
- Drop a folder onto another folder in edit mode.
- Move apps into an inner folder, rearrange them, rename the folder.
- The outer folder's icon shows the inner folder, and icons update without a restart.

## Install

1. In HENkaku settings, turn on **Enable Unsafe Homebrew**.
2. Install `nestfolder.vpk` from the [Releases](../../releases) page with VitaShell and open it.
3. Choose **Install / update**. The Vita restarts when it's done.

On first start the installer backs up your `app.db` and `config.txt` to
`ux0:data/nestfolder/first_backup/`. Every action shows what it will change and
asks for confirmation first.

**To remove it,** use **Uninstall** in the same app. It moves your nested folders
back to the home screen before removing the plugin. Don't just delete the plugin
by hand: without it, touching a nested folder freezes the home screen.

If something goes wrong, hold **L** while turning the Vita on to skip plugins, or
use **Restore first backup** in the installer. Logs are in
`ux0:data/nestfolder/log.txt`, please attach it when reporting a bug.

## Known limits

- Moving an app out of an inner folder sends it to the home screen, not to the outer folder.
- After dropping an app onto an inner folder, its new icon shows the next time you open the outer folder.

## How it works

nestfolder is a taiHEN plugin that hooks into SceShell, the Vita's home screen.
The Shell normally closes the open folder before opening another one, which
breaks when the second folder is inside the first. The plugin steps in at those
points, keeps the outer folder alive, and teaches the Shell to accept a folder
as a folder's content. It also gives each folder a unique ID, refreshes nested
folder icons and draws them into the outer folder's icon.

Before hooking anything, the plugin matches the loaded Shell to a known profile
using both segment sizes and stable code signatures. It then validates every
private hook site, so it never patches something it does not recognise.

The installer is a small Vita app that copies the plugin, edits `config.txt` and
updates `app.db` (the home screen database) safely, with a backup.

This project was made with heavy help from an AI assistant (Claude by Anthropic).
I decided what it should do and tested every build on my own Vita; the code and
the reverse engineering of the Shell were done together with the AI.

## Build

Needs [VitaSDK](https://vitasdk.org).

```
cmake -S . -B build
cmake --build build
```

GitHub Actions builds every push and publishes a release for every `v*` tag.

## License

See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Not affiliated with Sony. PlayStation, PS Vita and LiveArea are trademarks of
Sony Interactive Entertainment. Use at your own risk.
