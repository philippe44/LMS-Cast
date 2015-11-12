# Cross compile support - create a Makefile which defines these three variables and then includes this Makefile...
#CC = arm-linux-gnueabi-gcc
CFLAGS  ?= -Wall -fPIC -ggdb -O2 $(OPTS) $(INCLUDE) $(DEFINES)
LDFLAGS ?= -lpthread -lssl -lcrypto -ldl -lm -lrt -L. 
# for LD debug -s
EXECUTABLE ?= squeeze2cast-x86
EXECUTABLE_STATIC ?= squeeze2cast-x86-static

squeezetiny_dir = squeezetiny
squeezecast_dir = squeeze2cast
nanopb_dir = ../../nanopb
jansson_dir = ../../jansson/src

build_dir	= ./bin

LIBRARY = libupnp.a libixml.a libthreadutil.a

DEFINES = -DNO_CODEC -D_FILE_OFFSET_BITS=64 -DPB_FIELD_16BIT -DHAVE_STDINT_H
#-D_LARGE_FILE_SOURCE

INCLUDE = -I../../libupnp/1.6.19/threadutil/inc\
          -I../../libupnp/1.6.19/ixml/inc\
          -I../../libupnp/1.6.19/upnp/inc\
          -I../../libupnp/1.6.19/build/inc\
		  -I../../jansson/src\
		  -I../../nanopb\
		  -I$(squeezetiny_dir)\
          -I$(squeezecast_dir)/inc

SOURCES = \
		$(squeezetiny_dir)/slimproto.c $(squeezetiny_dir)/buffer.c \
        $(squeezetiny_dir)/stream.c $(squeezetiny_dir)/utils.c \
		$(squeezetiny_dir)/output_mr.c $(squeezetiny_dir)/decode.c \
        $(squeezetiny_dir)/main.c $(squeezetiny_dir)/util_common.c \
		$(squeezecast_dir)/cast_util.c \
      	$(squeezecast_dir)/util.c $(squeezecast_dir)/webserver.c \
		$(squeezecast_dir)/castcore.c $(squeezecast_dir)/castmessage.pb.c \
        $(squeezecast_dir)/squeeze2cast.c\
		$(nanopb_dir)/pb_common.c $(nanopb_dir)/pb_decode.c $(nanopb_dir)/pb_encode.c\
		$(jansson_dir)/dump.c $(jansson_dir)/error.c $(jansson_dir)/hashtable.c $(jansson_dir)/hashtable_seed.c \
		$(jansson_dir)/load.c $(jansson_dir)/memory.c $(jansson_dir)/pack_unpack.c $(jansson_dir)/strbuffer.c \
		$(jansson_dir)/strconv.c $(jansson_dir)/utf.c $(jansson_dir)/value.c

DEPS             = $(squeezetiny_dir)/squeezelite.h $(squeezetiny_dir)/slimproto.h\
                   $(squeezetiny_dir)/squeezedefs.h $(squeezetiny_dir)/squeezeitf.h

UNAME            = $(shell uname -s)

# add optional sources
ifneq (,$(findstring $(OPT_DSD), $(CFLAGS)))
	SOURCES += $(SOURCES_DSD)
endif
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	SOURCES += $(SOURCES_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	SOURCES += $(SOURCES_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_VIS), $(CFLAGS)))
	SOURCES += $(SOURCES_VIS)
endif

# add optional link options
ifneq (,$(findstring $(OPT_LINKALL), $(CFLAGS)))
	LDFLAGS += $(LINKALL)
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	LDFLAGS += $(LINKALL_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	LDFLAGS += $(LINKALL_RESAMPLE)
endif
else
# if not LINKALL and linux add LINK_LINUX
ifeq ($(UNAME), Linux)
	LDFLAGS += $(LINK_LINUX)
endif
endif

OBJECTS = $(SOURCES:.c=.o)

all: $(EXECUTABLE) $(EXECUTABLE_STATIC)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -o $(build_dir)/$@

$(EXECUTABLE_STATIC): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -static -o $(build_dir)/$@

$(OBJECTS): $(DEPS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@


clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

