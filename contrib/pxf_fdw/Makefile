EXTENSION    = pxf_fdw
EXTVERSION   = 1.0
MODULE_big   = pxf_fdw
OBJS         = src/pxfbridge.o src/pxfheaders.o src/pxffragment.o src/pxfuriparser.o src/pxfutils.o src/libchurl.o pxf_fdw.o
DOCS         = $(wildcard *.md)

BUILD_DIR = $(shell pwd)

LOADLOCAL=pxf_fdw--1.0.sql

all: $(LOADLOCAL) $(EXTENSION)--$(EXTVERSION).sql

install:

clobber:: clean 
	-rm pxf_fdw--1.0.sql

# backwards compatibility: loading the library from the build directory
$(LOADLOCAL) : $(LOADLOCAL).in
	cat $< | sed 's@BUILD_DIR@$(BUILD_DIR)@' > $@

$(EXTENSION)--$(EXTVERSION).sql : $(EXTENSION).sql
	@cp $< $@

DATA_built = $(EXTENSION)--$(EXTVERSION).sql
# DATA= $(EXTENSION).sql
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
REGRESS      = pxf_fdw_test4

ifdef USE_PGXS
	include $(PGXS)
else
	top_builddir = ../..
	include $(top_builddir)/src/Makefile.global
	include $(top_builddir)/contrib/contrib-global.mk
endif

SHLIB_LINK += -lcurl

PG_TEST_VERSION ?= $(MAJORVERSION)
REGRESS_OPTS = --inputdir=sql --load-language=plpgsql --dbname=regression
