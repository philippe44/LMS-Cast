# LMS-to-Cast

Allows Chromecast devices to be used in Logitech Media Server.
Pre-packaged versions for Windows (XP and above), Linux (x86, x64 and ARM) and OSX can be found here https://sourceforge.net/projects/lms-to-cast/ and here https://sourceforge.net/projects/lms-plugins-philippe44/

See support thread here: http://forums.slimdevices.com/showthread.php?104614-Announce-CastBridge-integrate-Chromecast-players-with-LMS-(squeeze2cast)&p=835640&viewfull=1#post835640

=============================================

To re-compile, use makefile (Linux only, need some mods for OSX and Windows) using:
https://github.com/nanopb/nanopb
https://github.com/akheron/jansson
https://github.com/philippe44/mDNS-SD (use fork v2)
https://sourceforge.net/projects/pupnp (there are a few patches to apply first against 1.6.19)
===========================================================================
MINIMUM TO READ FOR ADVANCED USE
===========================================================================
If you do not want to RTFM, at least do the following:
- put the application on a LOCAL disk where you have read/write access
- launch it with "-i config.xml" on the command line, and wait 30s till it exits
- launch the application without parameters (type exit to stop it)
- if it does not work try again with firewall disabled 
- ALWAYS start with a very low volume in LMS (some players might play very loud)

Really, have a look at the user guide in /doc

===========================================================================
WHAT VERSION TO CHOOSE
===========================================================================
Download CastBridge.zip and in /Bin, there are binaries for Windows, Linux 
(x86 and ARM) and OSX. 

===========================================================================
ADVANCED
===========================================================================
REBUILDING IS NOT NEEDED, but if you want to rebuild, here is what should be 
done. This can be built for Linux, OSX or Windows. My main Windows environment 
is C++ builder, so I've not made a proper makefile. The Linux/OSX makefile is
very hardcoded.

DEPENDENCIES
 - pthread (pthread_win32 on Windows) and dynamically linked
 - libupnp, statically linked
 - libflac, faad, libmad, alac, libsoxr, libogg, libvorbis
 
LIBUPNP
 - Under Win32, libupnp is build with the following defines
        WIN32
        UPNP_STATIC_LIB
        UPNP_USE_BCBPP (for C++ builder)
- Under Linux : N/A
- It is recommended to make the following changes to config.h (find the 
existing corresponding lines)
	#define MAX_JOBS_TOTAL 200 
	#define GENA_NOTIFICATION_SENDING_TIMEOUT 5
	#define GENA_NOTIFICATION_ANSWERING_TIMEOUT 5

===========================================================================
Main application for Windows XP and above
 - For compilation, use following defines
        WIN32
        UPNP_STATIC_LIB
        UPNP_USE_BCBPP (for C++ builder)
  - For runtime
        On C++ builder, requires cc32160mt.dll
        pthread.dll for all compilers

===========================================================================
Main application for Linux and OSX
 - For compilation, use following defines
        _FILE_OFFSET_BITS=64
	
 


