/* contrib/pxf_fdw/pxf_fdw--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION s3_fdw" to load this file. \quit

-- create wrapper with handler
CREATE OR REPLACE FUNCTION pxf_fdw_handler()
RETURNS fdw_handler
AS 'pxf_fdw'
LANGUAGE C STRICT;

CREATE FUNCTION pxf_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER pxf_fdw
VALIDATOR pxf_fdw_validator
HANDLER pxf_fdw_handler;

-- CREATE FOREIGN DATA WRAPPER pxf_s3_fdw
-- VALIDATOR pxf_fdw_validator
-- HANDLER pxf_fdw_handler
-- OPTIONS (protocol 's3');

-- CREATE FUNCTION file_fdw_validator(text[], oid)
-- RETURNS void
-- AS 'MODULE_PATHNAME'
-- LANGUAGE C STRICT;