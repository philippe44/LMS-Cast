# LMS-to-Cast bridge
Allows ChromeCast players to be used by Logitech Media Server, as a normal Logitech SqueezeBox. 

This project is now part of LMS 3rd parties repositories. Support thread is [here](https://forums.slimdevices.com/showthread.php?104614-Announce-CastBridge-integrate-Chromecast-players-with-LMS-(squeeze2cast))

Please see [here](https://github.com/philippe44/cross-compiling/blob/master/README.md#organizing-submodules--packages) to know how to rebuild my apps in general 

Otherwise, you can just get the source code and pre-built binaries:
```
cd ~
git clone http://github.com/philippe44/lms-cast
cd ~/lms-cast
git submodule update --init
```
and build doing:
```
cd ~/lms-cast/application
make

Binary releases are [here](https://sourceforge.net/projects/lms-plugins-philippe44/files/) as well
