\set ON_ERROR_STOP on
\pset pager off
\timing on
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;

-- Pass -v run_id=... to profile a specific completed run. Otherwise use the
-- largest completed run. All queries use the same snapshot.
\if :{?run_id}
\else
 SELECT id AS run_id FROM runs WHERE data->>'status'='completed'
 ORDER BY (data->'summary'->>'total_apps')::bigint DESC NULLS LAST, id LIMIT 1 \gset
\endif
SELECT id, data->'summary'->>'total_apps' AS apps,
       data->'summary'->>'normalized_rows' AS normalized,
       data->'summary'->>'comparison_rows' AS comparisons,
       data->'summary'->>'facts' AS facts
FROM runs WHERE id=:'run_id' AND data->>'status'='completed';

-- Form the same literal IN list constructed by Jobs::selectRows. Vary the
-- selected apps and run for different data distributions.
SELECT string_agg(quote_literal(app_id), ',' ORDER BY app_id) AS batch
FROM (SELECT app_id FROM application_segments WHERE run_id=:'run_id'
      ORDER BY app_id LIMIT 100) selected \gset

\echo normalized / 100 application batch / previous comparison stage
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON)
SELECT data FROM normalized_values
WHERE run_id=:'run_id' AND app_id IN (:batch) ORDER BY app_id;

\echo comparison / 100 application batch
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON)
SELECT data FROM comparison_values
WHERE run_id=:'run_id' AND app_id IN (:batch) ORDER BY app_id;

\echo comparison / whole run / previous native aggregation stage
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON)
SELECT data FROM comparison_values WHERE run_id=:'run_id' ORDER BY app_id;

\echo facts / first page
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON)
SELECT data FROM aggregate_facts WHERE run_id=:'run_id' ORDER BY fact_id::bigint LIMIT 100;

\echo membership / first fact drill-down
SELECT fact_id AS fact FROM aggregate_facts WHERE run_id=:'run_id'
ORDER BY fact_id::bigint LIMIT 1 \gset
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON)
SELECT app_id FROM aggregate_members WHERE run_id=:'run_id' AND fact_id=:'fact'
ORDER BY app_id LIMIT 100;

\echo relation and index sizes (bytes; entire relations, across all runs)
SELECT relname,n_live_tup,last_autoanalyze,
       pg_relation_size(relid) AS heap_bytes,pg_indexes_size(relid) AS index_bytes,
       pg_total_relation_size(relid) AS total_bytes
FROM pg_stat_user_tables
WHERE relname IN ('normalized_values','comparison_values','application_segments','aggregate_facts','aggregate_members')
ORDER BY relname;

\echo membership coverage and duplicate representation (this run)
SELECT sum(jsonb_array_length(data->'APP_IDS')) AS embedded_app_ids,
       sum(octet_length((data->'APP_IDS')::text)) AS embedded_app_ids_json_text_bytes,
       (SELECT count(*) FROM aggregate_members WHERE run_id=:'run_id') AS normalized_members
FROM aggregate_facts WHERE run_id=:'run_id';
ROLLBACK;