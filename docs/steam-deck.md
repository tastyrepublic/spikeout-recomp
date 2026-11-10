# Spikeout on the Steam Deck

The Steam Deck runs SteamOS, a Linux, so it uses the Linux download. Spikeout is a native Linux program: it doesn't
need Proton.

> [!NOTE]
> **Nobody has tried this on a real Steam Deck yet.** The Linux download is built with Valve's own Steam Runtime SDK
> (what Steam games for Linux are built with), so it should run, but these steps are written from how SteamOS
> works, not from a Deck in hand. If something here is wrong or doesn't work, please tell us: Esc > Game > Report a
> problem in the game, or [open an issue](https://github.com/tastyrepublic/spikeout-recomp/issues).

## What you need

- A Steam Deck (LCD or OLED), and its Desktop Mode for the first set-up (about 10 minutes).
- Your own Spikeout: Final Edition ROM set, `spikeofe.zip`, from your own arcade board. No game files come with the
  download, and this project can't help you find them.
- The Linux download from the [Releases page](https://github.com/tastyrepublic/spikeout-recomp/releases):
  `spikeout-<version>-linux-x64.tar.gz`.

## 1. Switch to Desktop Mode

Press the **Steam** button > **Power** > **Switch to Desktop**. A mouse pointer follows the right trackpad (a click:
press the right trackpad or R2). Press **Steam + X** to bring up the on-screen keyboard when you need to type.

## 2. Download and unpack

1. Open a web browser in Desktop Mode (Firefox or Chrome, from the Discover store if you have none) and download
   `spikeout-<version>-linux-x64.tar.gz` from the Releases page. It goes to your **Downloads** folder.
   (You can also copy it from a PC with a USB stick or a network share.)
2. Open **Dolphin** (the file manager, the folder icon on the taskbar) and go to Downloads.
3. Right-click the download > **Extract** > **Extract archive here**. You get a folder named like
   `spikeout-<version>-linux-x64`. Move it where you want to keep the game, for example a `Games` folder in your
   home folder.

Unpack it on the Deck itself: unpacked on Windows and copied over, the program can lose its permission to run.

## 3. Put your ROM set on the Deck

Copy your `spikeofe.zip` to the Deck, for example into the same `Games` folder (keep it zipped: the game reads the
zip itself). Don't put it inside the game's folder if you'd like to replace that folder with a newer version later;
next to it is fine.

## 4. Start it once in Desktop Mode

1. In the game's folder, double-click **spikeout**. (If nothing happens: right-click it > Properties > Permissions,
   tick **Is executable**, then try again.)
2. The game asks for the ROM set: click **CHOOSE THE ROM SET** and pick your `spikeofe.zip`. It is checked, and
   remembered from then on.
3. **The first start translates the game** from your ROM set into code for the Deck (this is how the download can
   come without any game files). It shows **PREPARING THE GAME** with a bar; on a Deck expect about half a minute to a
   minute or two, once. The result is kept, and later starts take a moment only. A new version translates it again
   once.
4. When the attract mode runs, it works. Close the window.

Do the ROM set choice here in Desktop Mode: the game's file chooser is the system's own window, which Game Mode may
not show well.

## 5. Add it to Steam

1. Open **Steam** in Desktop Mode (the icon on the desktop or taskbar).
2. **Games** > **Add a Non-Steam Game to My Library...** > **Browse...**
3. Go to the game's folder, set the file type at the bottom to **All Files**, select **spikeout**, **Open**, then
   tick it in the list and click **Add Selected Programs**.
4. In your Library, right-click the new **spikeout** entry > **Properties**:
   - Rename it to **Spikeout** if you like.
   - Under **Compatibility**, leave **"Force the use of a specific Steam Play compatibility tool"** **off**: it's a
     Linux game; Proton would only get in the way.
5. Optional: the icon. It's **`spikeout.png`** in the game's folder: in Properties, click the empty icon square next
   to the name and pick it. (For the Library's big pictures: right-click the game > Manage > Set custom artwork; any
   picture you like.) Downloads before 0.1.0-preview3 don't have it: save
   [`assets/icon/icon.png`](../assets/icon/icon.png) from this repository instead.
6. Optional, for Desktop Mode: run **`add-to-menu.sh`** in the game's folder (double-click it, or in a terminal
   `./add-to-menu.sh`) to put Spikeout in the desktop's application menu with its icon. Run it again if you move
   the folder.

Then go back: the **Return to Gaming Mode** icon on the desktop.

## 6. Play in Game Mode

Find **Spikeout** in your Library under **Non-Steam**, and press **Play**.

**Controls.** The Deck's controls reach the game as a gamepad (an Xbox-style pad, through Steam Input):

| On the Deck | In the game |
| --- | --- |
| D-pad or left stick | Move |
| A | Beat |
| X | Charge |
| B | Jump |
| Y | Shift |
| Menu (☰, right of the screen) | Start |
| View (⧉, left of the screen) | Coin |
| **View + Menu together** | The game's menu (the Esc menu) |

The Steam button stays Steam's own, so the game's menu is **View + Menu pressed together**. Change any of the
buttons in the game's menu, under Controls, or in Steam's controller settings for the game (the controller icon on
its page). If the pad does nothing at all, check that Steam's controller layout for Spikeout is a **Gamepad** layout
(the game's page > the controller icon > the layout: a gamepad template, not "Keyboard and mouse").

**The picture.** The Deck's screen is 16:10 (1280 x 800). In the game's menu, **Display > Picture shape**:
**16:10** fills the screen with the widescreen picture; **Arcade** keeps the original 4:3 with bars at the sides.
Game Mode shows the game full screen by itself.

**Free play** is the arcade's own setting: menu > Game > Arcade settings (COIN/CREDIT). Otherwise press View for a
coin.

## Online play on the Deck

Network play works over the Deck's Wi-Fi like on a PC: menu > Network play. A few things to know:

- The Deck has a 4-core processor. Running every player's cabinet (every board) with 3 or 4 players is a lot for it;
  the game notices and switches the Deck to **one board** by itself (its own cabinet only). The seat list on the right
  shows **ALL BOARDS** or **1 BOARD** for each player (see the
  [README's Online play section](../README.md#online-play)).
- Network play is still being tested: expect some lag and stutters, more with players far away. A wired connection
  (a USB-C dock with Ethernet) helps.
- On battery, a game with others drains it faster than playing alone.

## Updating to a new version

Download the new `linux-x64.tar.gz`, unpack it in Desktop Mode, and replace the old game folder with the new one
(same place, same name), so Steam's shortcut still points at it. Your settings, rankings and the ROM set's location
are kept: they're in `~/.local/share/spikeout-recomp/Spikeout`, not in the game's folder. The first start of the new
version translates the game again, once.

## If something goes wrong

- **It doesn't start from Game Mode:** start it once in Desktop Mode (step 4) to see any message. Check that Proton is
  off for it (step 5).
- **The pad doesn't work:** see the Controls part above (a Gamepad layout in Steam).
- **Anything else:** in the game, **menu > Game > Report a problem**. It collects the game's log and settings (never
  the ROM files). Log in to GitHub there to send it directly, or save it as a zip and attach it to
  [an issue](https://github.com/tastyrepublic/spikeout-recomp/issues). The log is also at
  `~/.local/share/spikeout-recomp/Spikeout/spikeout.log`.
