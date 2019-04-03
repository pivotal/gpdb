CREATE EXTENSION IF NOT EXISTS pxf_fdw;

CREATE SERVER demoserver
FOREIGN DATA WRAPPER pxf_fdw
OPTIONS (protocol 'demo');

DROP ROLE IF EXISTS pxf_fdw_user;
CREATE ROLE pxf_fdw_user;
GRANT USAGE ON FOREIGN SERVER demoserver TO pxf_fdw_user;

CREATE USER MAPPING FOR pxf_fdw_user
SERVER demoserver
OPTIONS ("fs.s3a.awsAccessKeyId" 'your access key', "fs.s3a.awsSecretAccessKey" 'your secret key');

SET ROLE pxf_fdw_user;
CREATE FOREIGN TABLE demo_table (first TEXT, last TEXT)
SERVER demoserver
OPTIONS (format 'text', location 'pxf://tmp/tmp?FRAGMENTER=org.greenplum.pxf.api.examples.DemoFragmenter&ACCESSOR=org.greenplum.pxf.api.examples.DemoAccessor&RESOLVER=org.greenplum.pxf.api.examples.DemoTextResolver');


DROP FOREIGN TABLE demo_table;
DROP USER MAPPING FOR pxf_fdw_user SERVER demoserver;
RESET ROLE;
REVOKE USAGE ON FOREIGN SERVER demoserver FROM pxf_fdw_user;
DROP ROLE pxf_fdw_user;
DROP SERVER demoserver;