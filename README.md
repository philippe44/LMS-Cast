# LMS-to-Cast

Allows Chromecast devices to be used in Logitech Media Server.
Pre-packaged versions for Windows (XP and above), Linux (x86, x64 and ARM) and OSX can be found here https://sourceforge.net/projects/lms-to-cast/ and here https://sourceforge.net/projects/lms-plugins-philippe44/

See support thread here: http://forums.slimdevices.com/showthread.php?104614-Announce-CastBridge-integrate-Chromecast-players-with-LMS-(squeeze2cast)&p=835640&viewfull=1#post835640

# To re-compile, use makefiles and following libraries/packages
 - pthread for Windows: https://www.sourceware.org/pthreads-win32
 - libupnp: https://sourceforge.net/projects/pupnp
 - nanopb: https://github.com/nanopb/nanopb
 - jansson: https://github.com/akheron/jansson
 - mDNS-SD (my fork) https://github.com/philippe44/mDNS-SD (use fork v2)
 - ALAC codec: https://github.com/macosforge/alac
 - faad2: http://www.audiocoding.com/
 - libmad: https://www.underbit.com/products/mad/
 - libflac: https://xiph.org/flac/
 - libsoxr: https://sourceforge.net/p/soxr/wiki/Home/
 - libogg & libvorbis: https://xiph.org/vorbis/
 - shine: https://github.com/philippe44/shine

