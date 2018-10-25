-- create wrapper with handler
CREATE OR REPLACE FUNCTION pxf_fdw_handler ()
RETURNS fdw_handler
AS 'pxf_fdw'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER pxf_fdw
HANDLER pxf_fdw_handler;

