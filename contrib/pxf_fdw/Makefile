EXTENSION    = pxf_fdw
EXTVERSION   = 1.0
MODULE_big   = pxf_fdw
OBJS         = pxf_fdw.o
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
PGXS := $(shell $(PG_CONFIG) --pgxs)
REGRESS      = pxf_fdw_test pxf_fdw_test2

include $(PGXS)

PG_TEST_VERSION ?= $(MAJORVERSION)
REGRESS_OPTS = --inputdir=sql --load-language=plpgsql --dbname=regression

