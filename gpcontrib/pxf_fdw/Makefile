EXTENSION    = pxf_fdw
EXTVERSION   = 1.0
MODULE_big   = pxf_fdw
OBJS         = src/pxf_fdw.o src/pxfuriparser.o src/libchurl.o src/pxfutils.o src/pxfheaders.o src/pxffragment.o src/gpdbwritableformatter.o src/pxffilters.o
DOCS         = $(wildcard *.md)

BUILD_DIR = $(shell pwd)

LOADLOCAL=pxf_fdw--1.0.sql

all: $(LOADLOCAL) $(EXTENSION)--$(EXTVERSION).sql

install:

clobber:: clean 
	-rm pxf_fdw--1.0.sql

$(EXTENSION)--$(EXTVERSION).sql : $(EXTENSION).sql
	@cp $< $@

DATA_built = $(EXTENSION)--$(EXTVERSION).sql
# DATA= $(EXTENSION).sql
PG_CONFIG ?= pg_config
REGRESS      = pxf_fdw_test4

ifdef USE_PGXS
	PGXS := $(shell pg_config --pgxs)
	include $(PGXS)
else
	top_builddir = ../..
	include $(top_builddir)/src/Makefile.global
	include $(top_builddir)/contrib/contrib-global.mk
endif

PG_TEST_VERSION ?= $(MAJORVERSION)
REGRESS_OPTS = --inputdir=sql --load-language=plpgsql --dbname=regression