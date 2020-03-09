-- ===================================================================
-- create FDW objects
-- ===================================================================

CREATE EXTENSION pxf_fdw;

DROP ROLE IF EXISTS pxf_fdw_user;
CREATE ROLE pxf_fdw_user;

-- ===================================================================
-- Validation for WRAPPER options
-- ===================================================================

--
-- Foreign-data wrapper creation fails if protocol option is not provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator;

--
-- Foreign-data wrapper creation fails if protocol option is empty
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol '' );

--
-- Foreign-data wrapper creation fails if resource option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( resource '/invalid/option/for/wrapper' );

--
-- Foreign-data wrapper creation fails if header option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', header 'TRUE' );

--
-- Foreign-data wrapper creation fails if delimiter option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', delimiter ' ' );

--
-- Foreign-data wrapper creation fails if quote option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', quote '`' );

--
-- Foreign-data wrapper creation fails if escape option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', escape '\' );

--
-- Foreign-data wrapper creation fails if null option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', null '' );

--
-- Foreign-data wrapper creation fails if encoding option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', encoding 'UTF-8' );

--
-- Foreign-data wrapper creation fails if newline option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', newline 'CRLF' );

--
-- Foreign-data wrapper creation fails if fill_missing_fields option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', fill_missing_fields '' );

--
-- Foreign-data wrapper creation fails if force_null option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', force_null 'true' );

--
-- Foreign-data wrapper creation fails if force_not_null option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', force_not_null 'true' );

--
-- Foreign-data wrapper creation fails if reject_limit option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', reject_limit '5' );

--
-- Foreign-data wrapper creation fails if reject_limit_type option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', reject_limit_type 'rows' );

--
-- Foreign-data wrapper creation fails if log_errors option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', log_errors 'true' );

--
-- Foreign-data wrapper creation fails if config option is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', config '/foo/bar' );

--
-- Foreign-data wrapper creation fails if negative pxf_port number is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_port '-1' );

--
-- Foreign-data wrapper creation fails if out of range pxf_port number is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_port '65536' );

--
-- Foreign-data wrapper creation fails if non numeric pxf_port number is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_port 'foo' );

--
-- Foreign-data wrapper creation fails when an invalid pxf_protocol is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_protocol 'foo' );

--
-- Foreign-data wrapper creation succeeds when protocol is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', mpp_execute 'all segments' );

--
-- Foreign-data wrapper creation succeeds when valid pxf_port is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_with_port
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_port '7007' );

--
-- Foreign-data wrapper creation succeeds when pxf_host is provided
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_with_host
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_host 'foobar.com' );

--
-- Foreign-data wrapper creation succeeds when pxf_protocol is HTTP
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_unsecure
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_protocol 'HTTP' );

--
-- Foreign-data wrapper creation succeeds when pxf_protocol is https
--
CREATE FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_secure
    HANDLER pxf_fdw_handler
    VALIDATOR pxf_fdw_validator
    OPTIONS ( protocol 'pxf_fdw_test', pxf_protocol 'https' );

--
-- Foreign-data wrapper alteration fails when protocol is dropped
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( DROP protocol );

--
-- Foreign-data wrapper alteration fails if protocol option is empty
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( SET protocol '' );

--
-- Foreign-data wrapper alteration fails if resource option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD resource '/invalid/option/for/wrapper' );

--
-- Foreign-data wrapper alteration fails if header option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD header 'TRUE' );

--
-- Foreign-data wrapper alteration fails if delimiter option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD delimiter ' ' );

--
-- Foreign-data wrapper alteration fails if quote option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD quote '`' );

--
-- Foreign-data wrapper alteration fails if escape option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD escape '\' );

--
-- Foreign-data wrapper alteration fails if null option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD null '' );

--
-- Foreign-data wrapper alteration fails if encoding option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD encoding 'UTF-8' );

--
-- Foreign-data wrapper alteration fails if newline option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD newline 'CRLF' );

--
-- Foreign-data wrapper alteration fails if fill_missing_fields option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD fill_missing_fields '' );

--
-- Foreign-data wrapper alteration fails if force_null option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD force_null 'true' );

--
-- Foreign-data wrapper alteration fails if force_not_null option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD force_not_null 'true' );

--
-- Foreign-data wrapper alteration fails if reject_limit option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD reject_limit '5' );

--
-- Foreign-data wrapper alteration fails if reject_limit_type option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD reject_limit_type 'rows' );

--
-- Foreign-data wrapper alteration fails if log_errors option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD log_errors 'true' );

--
-- Foreign-data wrapper alteration fails if config option is added
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD config '/foo/bar' );

--
-- Foreign-data wrapper alteration fails if negative pxf_port number is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_port '-1' );

--
-- Foreign-data wrapper alteration fails if out of range pxf_port number is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_port '65536' );

--
-- Foreign-data wrapper alteration fails if non numeric pxf_port number is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_port 'foo' );

--
-- Foreign-data wrapper alteration succeeds if valid port number is provide
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_port '8080' );

--
-- Foreign-data wrapper alteration fails if an invalid port number is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( SET pxf_port '80808' );

--
-- Foreign-data wrapper alteration succeeds if a new valid port number is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( SET pxf_port '7777' );

--
-- Foreign-data wrapper alteration succeeds when the pxf_port option is dropped
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( DROP pxf_port );

--
-- Foreign-data wrapper alteration succeeds when pxf_host is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_host 'test.com' );

--
-- Foreign-data wrapper alteration succeeds when pxf_host is set
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_with_host
    OPTIONS ( SET pxf_host 'bar.com' );

--
-- Foreign-data wrapper alteration succeeds when pxf_host is dropped
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_with_host
    OPTIONS ( DROP pxf_host );

--
-- Foreign-data wrapper alteration fails when an invalid pxf_protocol value is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_protocol '12345' );

--
-- Foreign-data wrapper alteration succeeds when the pxf_protocol option is provided
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw
    OPTIONS ( ADD pxf_protocol 'hTTpS' );

--
-- Foreign-data wrapper alteration succeeds when pxf_protocol is set
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_secure
    OPTIONS ( SET pxf_protocol 'http' );

--
-- Foreign-data wrapper alteration succeeds when pxf_protocol is dropped
--
ALTER FOREIGN DATA WRAPPER pxf_fdw_test_pxf_fdw_secure
    OPTIONS ( DROP pxf_protocol );
