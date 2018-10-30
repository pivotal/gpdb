CREATE SERVER s3server
FOREIGN DATA WRAPPER pxf_fdw
OPTIONS (protocol 's3');

CREATE USER MAPPING FOR gpadmin
SERVER s3server
OPTIONS ("fs.s3a.awsAccessKeyId" 'your access key', "fs.s3a.awsSecretAccessKey" 'your secret key');

CREATE FOREIGN TABLE s3_table (first TEXT, last TEXT)
SERVER s3server
OPTIONS (location 'pxf://tmp/tmp') ;
