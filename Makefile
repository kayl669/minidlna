# $Id$
# MiniDLNA project
# http://sourceforge.net/projects/minidlna/
# (c) 2008-2009 Justin Maggard
# for use with GNU Make
# To install use :
# $ DESTDIR=/dummyinstalldir make install
# or :
# $ INSTALLPREFIX=/usr/local make install
# or :
# $ make install
#

DESTDIR=/cygdrive/z/

$(warning useful commands)
$(warning make)
$(warning make VERSION=NAS)
$(warning make VERSION=NAS install-nas)
$(warning make distclean)

VERSION = NORMAL
ifneq (,$(findstring NORMAL,$(VERSION)))
OS_NAME = ${shell uname -s}
OS_VERSION = ${shell uname -r}
else
$(warning Building NAS version)
OS_NAME = Linux
OS_VERSION = 2.6.22.7
endif

# set, if you want to include thumbnail creation support
# (requires libffmpegthumbnailer library)
CREATE_THUMBNAILS=yes

RM = rm -f
INSTALL = install

INSTALLPREFIX ?= $(DESTDIR)/usr
SBININSTALLDIR = $(INSTALLPREFIX)/sbin
ETCINSTALLDIR = $(DESTDIR)/etc

BASEOBJS = minidlna.o upnphttp.o upnpdescgen.o upnpsoap.o \
           upnpreplyparse.o minixml.o \
           getifaddr.o process.o upnpglobalvars.o \
           options.o minissdp.o uuid.o upnpevents.o \
           sql.o utils.o metadata.o scanner.o inotify.o \
           tivo_utils.o tivo_beacon.o tivo_commands.o \
           tagutils/tagutils.o \
           playlist.o image_utils.o albumart.o log.o containers.o clients.o

ALLOBJS = $(BASEOBJS) $(LNXOBJS)

LIBS = -lpthread -lexif -ljpeg -lsqlite3 -lavformat -lavutil -lavcodec -lid3tag -lFLAC -logg -lvorbis -lz

ifneq (,$(findstring CYGWIN,$(OS_NAME)))
  LIBS += -lintl -lws2_32 -liphlpapi
endif
ifeq (,$(findstring CYGWIN,$(OS_NAME)))
  LIBS += -ljack -lx264 -lrtmp -lgnutls -lbz2 -lxvidcore -lvpx -lvorbisenc -ltheoraenc -ltheoradec -lspeex -lschroedinger-1.0 -lopenjpeg -lmp3lame -lgsm -ldirac_encoder -lva -lgcrypt -ltasn1 -lorc-0.4 -lgpg-error
endif

ifneq (,$(findstring CYGWIN,$(OS_NAME)))
CFLAGS = -Wall -g -O3 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
	 -I/usr/include/ \
	 -I/usr/include/libavutil \
     -I/usr/include/libavcodec \
     -I/usr/include/libavformat \
	 -DSTATIC
LDFLAGS = -L/usr/lib
CC = gcc
endif

ifneq (,$(findstring Linux,$(OS_NAME)))
CFLAGS = -Wall -g -O3 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
    -Iusr/include \
    -Iusr/include/libavutil \
    -Iusr/include/libavcodec \
    -Iusr/include/libavformat

LDFLAGS =-Lusr/lib
CC = arm-none-linux-gnueabi-gcc
endif

ifneq ($(CREATE_THUMBNAILS),no)
  LIBS += -lffmpegthumbnailer
ifeq (,$(findstring CYGWIN,$(OS_NAME)))
  LIBS += -lswscale -lpng12
endif
  CFLAGS += -DTHUMBNAIL_CREATION_SUPPORT
endif

TESTUPNPDESCGENOBJS = testupnpdescgen.o upnpdescgen.o

EXECUTABLES = minidlna testupnpdescgen

.PHONY:	all clean distclean install depend

all:	$(EXECUTABLES)

clean-ex:
	$(RM) $(EXECUTABLES)
clean: clean-ex
	$(RM) $(ALLOBJS)
	$(RM) $(EXECUTABLES)
	$(RM) testupnpdescgen.o

distclean: clean
	$(RM) config.h

install-nas: clean-ex minidlna
	@cp -u minidlna $(DESTDIR)
	@cp -u png_sm.png $(DESTDIR)
	@cp -u png_lrg.png $(DESTDIR)
	@cp -u jpeg_sm.jpg $(DESTDIR)
	@cp -u jpeg_lrg.jpg $(DESTDIR)
	@cp -u chapter.jpg $(DESTDIR)

install:	minidlna
	$(INSTALL) -d $(SBININSTALLDIR)
	$(INSTALL) minidlna $(SBININSTALLDIR)

install-conf:
	$(INSTALL) -d $(ETCINSTALLDIR)
	$(INSTALL) --mode=0644 minidlna.conf $(ETCINSTALLDIR)

minidlna:	$(BASEOBJS) $(LNXOBJS)
	@echo Linking $@
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BASEOBJS) $(LNXOBJS) $(LIBS)

testupnpdescgen:	$(TESTUPNPDESCGENOBJS)
	@echo Linking $@
	@$(CC) $(CFLAGS) -o $@ $(TESTUPNPDESCGENOBJS)

config.h:	genconfig.sh
	./genconfig.sh '${OS_NAME}' '${OS_VERSION}'

depend:	config.h
	makedepend -f$(MAKEFILE_LIST) -Y \
	$(ALLOBJS:.o=.c) $(TESTUPNPDESCGENOBJS:.o=.c) 2>/dev/null

# DO NOT DELETE

minidlna.o: config.h upnpglobalvars.h minidlnatypes.h
minidlna.o: upnphttp.h upnpdescgen.h minidlnapath.h getifaddr.h upnpsoap.h
minidlna.o: options.h minissdp.h process.h upnpevents.h log.h
upnphttp.o: config.h upnphttp.h upnpdescgen.h minidlnapath.h upnpsoap.h
upnphttp.o: upnpevents.h image_utils.h sql.h log.h
upnpdescgen.o: config.h upnpdescgen.h minidlnapath.h upnpglobalvars.h
upnpdescgen.o: minidlnatypes.h log.h
upnpsoap.o: config.h upnpglobalvars.h minidlnatypes.h log.h utils.h sql.h
upnpsoap.o: upnphttp.h upnpsoap.h upnpreplyparse.h getifaddr.h log.h
upnpreplyparse.o: upnpreplyparse.h minixml.h log.h
minixml.o: minixml.h
getifaddr.o: getifaddr.h log.h
process.o: process.h config.h log.h
upnpglobalvars.o: config.h upnpglobalvars.h
upnpglobalvars.o: minidlnatypes.h
options.o: options.h config.h upnpglobalvars.h
options.o: minidlnatypes.h
minissdp.o: config.h minidlnapath.h upnphttp.h
minissdp.o: upnpglobalvars.h minidlnatypes.h minissdp.h log.h
upnpevents.o: config.h upnpevents.h minidlnapath.h upnpglobalvars.h
upnpevents.o: minidlnatypes.h upnpdescgen.h log.h uuid.h
uuid.o: uuid.h
testupnpdescgen.o: config.h upnpdescgen.h
upnpdescgen.o: config.h upnpdescgen.h minidlnapath.h upnpglobalvars.h
upnpdescgen.o: minidlnatypes.h
scanner.o: upnpglobalvars.h metadata.h utils.h sql.h scanner.h log.h playlist.h
metadata.o: upnpglobalvars.h metadata.h albumart.h utils.h sql.h log.h
albumart.o: upnpglobalvars.h albumart.h utils.h image_utils.h sql.h log.h
tagutils/tagutils.o: tagutils/tagutils-asf.c tagutils/tagutils-flc.c tagutils/tagutils-plist.c tagutils/tagutils-misc.c
tagutils/tagutils.o: tagutils/tagutils-aac.c tagutils/tagutils-asf.h tagutils/tagutils-flc.h tagutils/tagutils-mp3.c tagutils/tagutils-wav.c
tagutils/tagutils.o: tagutils/tagutils-ogg.c tagutils/tagutils-aac.h tagutils/tagutils.h tagutils/tagutils-mp3.h tagutils/tagutils-ogg.h log.h
playlist.o: playlist.h
inotify.o: inotify.h playlist.h
image_utils.o: image_utils.h
tivo_utils.o: config.h tivo_utils.h
tivo_beacon.o: config.h tivo_beacon.h tivo_utils.h
tivo_commands.o: config.h tivo_commands.h tivo_utils.h utils.h
utils.o: utils.h
sql.o: sql.h
log.o: log.h
containers.o: containers.h
clients.o: clients.h

.SUFFIXES: .c .o

.c.o:
	@echo Compiling $*.c
	@$(CC) $(CFLAGS) -o $@ -c $< && exit 0;\
		echo "The following command failed:" 1>&2;\
		echo "$(CC) $(CFLAGS) -o $@ -c $<";\
		$(CC) $(CFLAGS) -o $@ -c $< &>/dev/null
