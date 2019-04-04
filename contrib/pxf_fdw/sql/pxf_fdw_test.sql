-- start_matchsubs
--
-- # create a match/subs expression
--
-- m/ERROR:.*invalid foreign table option\. The \'protocol\' option cannot be defined for PXF foreign tables.*/
-- s/ERROR:.*invalid foreign table option\. The \'protocol\' option cannot be defined for PXF foreign tables.*/ERROR:  invalid foreign table option. The 'protocol' option cannot be defined for PXF foreign tables/
--
-- end_matchsubs

CREATE EXTENSION IF NOT EXISTS pxf_fdw;

CREATE SERVER demo
    FOREIGN DATA WRAPPER pxf_fdw;

-- Foreign table creation fails if `protocol` option is provided
CREATE FOREIGN TABLE invalid_protocol_option( login_time interval )
    SERVER demo
    OPTIONS ( protocol 'hdfs' );

CREATE ROLE pxf_fdw_user;

-- User mapping creation fails if `protocol` option is provided
CREATE USER MAPPING FOR pxf_fdw_user
    SERVER demo
    OPTIONS ( protocol 'hdfs' );

-- Succeeds when protocol is not provided
CREATE USER MAPPING FOR pxf_fdw_user
    SERVER demo;

-- Altering a user mapping  fails if `protocol` option is added
ALTER USER MAPPING FOR pxf_fdw_user
    SERVER demo
    OPTIONS ( protocol 'hdfs' );

CREATE FOREIGN TABLE pxf_foreign_table( login_time interval )
    SERVER demo;

-- Altering a foreign table fails if `protocol` option is added
ALTER FOREIGN TABLE pxf_foreign_table
    OPTIONS (ADD protocol 'hdfs');

DROP USER MAPPING FOR pxf_fdw_user SERVER demo;
DROP FOREIGN TABLE pxf_foreign_table;
DROP SERVER demo;
