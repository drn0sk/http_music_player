.DELETE_ON_ERROR:

.PHONY : all clean install install_dirs install-strip uninstall

CFLAGS = -g -O
BUILD_DIR = build

INSTALL = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA = $(INSTALL) -m 644
prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
datarootdir = $(prefix)/share
datadir = $(datarootdir)
localstatedir = $(prefix)/var

HTML_FILE = $(datadir)/http_music_player/http_music_player.html
PATHS_CACHE = $(localstatedir)/cache/http_music_player/paths.cache

all : $(BUILD_DIR)/http_music_player

$(BUILD_DIR)/http_music_player : http_music_player.c | $(BUILD_DIR)
	$(CC)	-pthread -lmagic -lhttp \
		$(if $(HTML_FILE),-D'HTML_FILE="$(DESTDIR)$(HTML_FILE)"') \
		$(if $(PATHS_CACHE),-D'PATHS_CACHE="$(DESTDIR)$(PATHS_CACHE)"') \
		$(if $(LOGFILE),-D'LOGFILE="$(DESTDIR)$(LOGFILE)"') \
		$(CPPFLAGS) $(CFLAGS) $(LDFLAGS) http_music_player.c -o $(BUILD_DIR)/http_music_player

$(BUILD_DIR) :
	-mkdir $(BUILD_DIR)

clean :
	-rm -r $(BUILD_DIR)

install : all | install_dirs
	$(INSTALL_PROGRAM) $(if $(STRIP),-s) $(BUILD_DIR)/http_music_player $(DESTDIR)$(bindir)/
	$(INSTALL) http_music_player.html $(DESTDIR)$(HTML_FILE)

install_dirs :
	$(INSTALL) -d $(DESTDIR)$(bindir)/ $(dir $(DESTDIR)$(HTML_FILE)) $(dir $(DESTDIR)$(PATHS_CACHE)) $(dir $(DESTDIR)$(LOGFILE))

install-strip : STRIP = 1
install-strip : install

uninstall :
	-rm $(DESTDIR)$(bindir)/http_music_player
	-rm -r $(dir $(DESTDIR)$(HTML_FILE)) $(dir $(DESTDIR)$(PATHS_CACHE)) $(dir $(DESTDIR)$(LOGFILE))
