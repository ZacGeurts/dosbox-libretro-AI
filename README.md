# DOSBOX-Grok3-AI<BR />
Up to 300% speed update so far. <BR />
The buildbot may not be using dynrec so it may only be like 30% update.<BR />
I cleaned up all the warnings so there may actually be a slight speed hit while updating.<BR />
Is it faster or not? Unnuno. Probably and maybe by a lot. WIP and updates will be coming soon.<BR />
(Barring foul play)<BR />
I will be eventually getting more information on what is being achieved. Still a new project.<BR />
Press space bar to run the emulator full speed. <BR />
You can view the framerate and other setting in Config - On screen Notifications - Notification Visibility <BR />
One of the updates changed endian so PowerPC is unlikely to work any longer with this build. <BR />
This was built with C++17 and is likely to require it due to memory updates.<BR />
If you can test it on a mobile platform please open a ticket letting everyone know how it went.<BR />
This is a test. I currently recommend either DOSBOX Svn or DOSBOX Pure (or vice versa)<BR />

<img src="Screenshot from 2025-04-11 14-45-25.png"></img>

libretro wrapper for DOSBox

* To use you can either load an exe/com/bat file or a *.conf file.
* If loading exe/com/bat the system directory will be searched for a 'dosbox.conf' file to load. If one isn't available default values will be used. This mode is equivalent to running a DOSBox binary with the specified file as the command line argument.
* If loading a conf file DOSBox will be loaded with the options in the config file. This mode is useful if you just want to be dumped at a command prompt, but can also be used to load a game by putting commands in the autoexec section.
* To be useful the frontend will need to have keyboard+mouse support, and all keyboard shortcuts need to be remapped.

Unsupported features:

* Physical CD-ROMs, CD images are supported.
* The key mapper, key remapping does not work.

TODO:

* Joysticks need more testing. Flightsticks types are not implemented yet.

Building:

* To build for your current platform "cd */libretro-dosbox (ENTER) make (ENTER)"

Notes:

* There seems to be no trivial way to have the DOSBox core return periodically, so libco is used to enter/exit the emulator loop. This actually works better than one would expect.
* There is no serialization support, it's not supported by DOSBox.
* DOSBox uses 'wall time' for timing, frontend fast forward and slow motion features will have no effect.
* To use MIDI you need MT32_CONTROL.ROM and MT32_PCM.ROM in the system directory of RetroArch.Then set:
[midi]
mpu401=intelligent
mididevice=mt32
