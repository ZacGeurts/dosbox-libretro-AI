# DOSBOX-Grok3-AI<BR />

# Currently down for updates. Overwrote some code before uploading. Expect segfault or worse while this message is up.

This is a test. I currently recommend either DOSBOX Svn or DOSBOX Pure (or vice versa)<BR />
Why not try it with those? I want something old to update. This for fun, nothing much else.<BR />
If anything comes of this project then it will likely be incorporated by those with the talent.<BR />
Up to 300% speed update so far? Enabling dynrec is a massive jump and was not enabled by default on core downloader for Linux. It shares similat speed if reconpiled with dynrec. The old one you need to add -std=c++11 or 14 to the build options due to older code. This one should build fine on newer systems without modifying the makefile. I opened a ticket on the original dosbox-libretro to see if fixed build options can mainline into the build.<BR />
If you want to build the original with your system capabilities then add CXX += -std=c++14 somewhere near the top of your Makefile.libretro<BR />
The buildbot is not using dynrec so it is not (seeming) faster yet other than that change.<BR />
I expect like a 5%-15% speed increase when it is all said and done. You want dynrec on Windows or Linux x32 x64 as it is the 300% speed increase (likely translates to 300% reduction in complexity hitting your processor).
<BR />
My next goal is to go from a peak of 980fps to over 1000fps because who wouldn't want to break the number?<BR />
I cleaned up all the warnings so there may actually be a slight speed hit atm with warning checks.<BR />
Is it faster or not? Unnuno. Depends if you already have dynrec enabled and can run profiling on it to prove one way or the other.<BR />
The niceity is it builds with C++ 17 now and probably 20 without pages of warnings and errors.<BR />
I will be eventually getting more information on what is being achieved. Still a new project.<BR />
For now I just fixed a couple bugs from moving to the new libretro-common and am beginning to just have the AI rewrite files.<BR />
So I hit some bugs that I did not fix properly and am going through and updating libretro-common issues again now that I have other fixes in place. Shoulde be updated and uploaded soon(tm).<BR />
Press space bar to run the emulator full speed. <BR />
You can view the framerate and other setting in Config - On screen Notifications - Notification Visibility <BR />
One of the updates changed endian so PowerPC is unlikely to work any longer with this build. <BR />
This was built with C++17 and is likely to require it due to memory updates.<BR />
If you can test it on a mobile platform please open a ticket letting everyone know how it went.<BR />

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
