BASE		= ..
OS 			:= $(shell uname)

ifeq ($(OS),Darwin)
LINKSTATIC	=
LDFLAGS 	+= -Wl,-dead_strip
else
LINKSTATIC 	= -static
LDFLAGS 	+= -Wl,--gc-sections
endif

TOOLS		= ./tools
SRC			= ./squeeze2cast
SQUEEZETINY	= ./squeezetiny
JANSSON		= $(BASE)/jansson
NANOPB		= $(BASE)/nanopb
MDNSSD		= $(BASE)/mdnssd
VALGRIND	= $(BASE)/valgrind
UPNP 		= $(BASE)/libupnp/1.6.19

DEFINES 	= -D_FILE_OFFSET_BITS=64 -DPB_FIELD_16BIT -DHAVE_STDINT_H -DRESAMPLE

CFLAGS 		+= -fdata-sections -ffunction-sections 

vpath %.c $(TOOLS):$(SRC):$(SQUEEZETINY):$(JANSSON)/src:$(NANOPB):$(MDNSSD)

INCLUDE = -I. \
		  -I$(BASE)\
		  -I$(VALGRIND)/memcheck -I$(VALGRIND)/include \
		  -I$(SQUEEZETINY) \
		  -I$(TOOLS) \
		  -I$(SRC) -I$(SRC)/inc \
		  -I$(UPNP)/ixml/inc -I$(UPNP)/upnp/inc -I$(UPNP)/build/inc -I$(UPNP)/threadutil/inc \
		  -I$(JANSSON)/src \
		  -I$(NANOPB) \
		  -I$(MDNSSD) \
		  -I$(BASE)/flac/include\
		  -I$(BASE)/faad2/include\
		  -I$(BASE)/libmad\
		  -I$(BASE)/libvorbis/include -I$(BASE)/libogg/include\
		  -I$(BASE)/soxr/src
		  		  
DEPS	= $(SQUEEZETINY)/squeezedefs.h
				  
SOURCES = slimproto.c buffer.c tinyutils.c output_http.c main.c \
		  stream.c decode.c pcm.c flac_thru.c thru.c process.c resample.c \
		  util_common.c cast_util.c util.c log_util.c \
		  castcore.c cast_parse.c castmessage.pb.c squeeze2cast.c \
		  pb_common.c pb_decode.c pb_encode.c dump.c error.c hashtable.c strconv.c \
		  hashtable_seed.c load.c memory.c pack_unpack.c strbuffer.c utf.c value.c \
	  	  mdnssd-min.c
		
SOURCES_LIBS = flac.c mad.c faad.c vorbis.c output.c
		
OBJECTS 		= $(patsubst %.c,$(OBJ)/%.o,$(SOURCES)) $(patsubst %.c,$(OBJ)/%.o,$(SOURCES_LIBS)) 
OBJECTS_STATIC 	= $(patsubst %.c,$(OBJ)/%.o,$(SOURCES)) $(patsubst %.c,$(OBJ)/%-static.o,$(SOURCES_LIBS)) 

LIBRARY 		= $(OBJ)/libixml.a  $(OBJ)/libsoxr.a $(OBJ)/libshine.a
LIBRARY_STATIC 	= $(LIBRARY) $(OBJ)/libfaad.a $(OBJ)/libFLAC.a $(OBJ)/libmad.a $(OBJ)/libvorbisfile.a $(OBJ)/libvorbis.a $(OBJ)/libogg.a 

all: $(EXECUTABLE) $(EXECUTABLE_STATIC)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -o $@

$(EXECUTABLE_STATIC): $(OBJECTS_STATIC)
	$(CC) $(OBJECTS_STATIC) $(LIBRARY_STATIC) $(LDFLAGS) $(LINKSTATIC) -o $@	

$(OBJECTS): $(DEPS) | bin $(OBJ) 

$(OBJ):
	@mkdir -p $@
	
bin:	
	@mkdir -p bin

$(OBJ)/%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

$(OBJ)/%-static.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DLINKALL $(INCLUDE) $< -c -o $(OBJ)/$*-static.o	
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(OBJECTS_STATIC) $(EXECUTABLE_STATIC) 

