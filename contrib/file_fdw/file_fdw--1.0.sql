/* contrib/file_fdw/file_fdw--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION s3_fdw" to load this file. \quit

CREATE FUNCTION pxf_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION file_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER s3_fdw
  HANDLER file_fdw_handler
  VALIDATOR file_fdw_validator
  OPTIONS ( option 's3' [, ... ];
