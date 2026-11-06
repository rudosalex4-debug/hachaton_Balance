CREATE TABLE IF NOT EXISTS users(username text PRIMARY KEY, salt text NOT NULL, password_hash text NOT NULL);
CREATE TABLE IF NOT EXISTS sessions(token_hash text PRIMARY KEY, username text NOT NULL REFERENCES users, expires timestamptz NOT NULL);
CREATE TABLE IF NOT EXISTS datasets(id text PRIMARY KEY, owner text REFERENCES users, data jsonb NOT NULL);
CREATE TABLE IF NOT EXISTS procedures(id text PRIMARY KEY, owner text REFERENCES users, data jsonb NOT NULL);
CREATE TABLE IF NOT EXISTS run_configs(id text PRIMARY KEY, owner text REFERENCES users, data jsonb NOT NULL);
CREATE TABLE IF NOT EXISTS runs(id text PRIMARY KEY, owner text NOT NULL REFERENCES users, data jsonb NOT NULL);
CREATE TABLE IF NOT EXISTS run_logs(id bigserial PRIMARY KEY, run_id text NOT NULL REFERENCES runs ON DELETE CASCADE, data jsonb NOT NULL);
CREATE INDEX IF NOT EXISTS logs_run ON run_logs(run_id,id);
CREATE TABLE IF NOT EXISTS normalized_values(run_id text NOT NULL REFERENCES runs ON DELETE CASCADE, app_id text NOT NULL, source text NOT NULL CHECK(source IN ('BEFORE','AFTER')), path text NOT NULL, array_key text NOT NULL, data jsonb NOT NULL, PRIMARY KEY(run_id,app_id,source,path,array_key));
CREATE INDEX IF NOT EXISTS normalized_path ON normalized_values(run_id,path,source);
CREATE TABLE IF NOT EXISTS comparison_values(run_id text NOT NULL REFERENCES runs ON DELETE CASCADE, app_id text NOT NULL, path text NOT NULL, array_key text NOT NULL, diff_flag integer NOT NULL CHECK(diff_flag IN (0,1)), change_type text NOT NULL CHECK(change_type IN ('SAME','VALUE_CHANGED','ATTRIBUTE_NEW','ATTRIBUTE_LOST')), data jsonb NOT NULL, PRIMARY KEY(run_id,app_id,path,array_key));
CREATE INDEX IF NOT EXISTS comparison_path ON comparison_values(run_id,path,diff_flag);
CREATE INDEX IF NOT EXISTS comparison_change ON comparison_values(run_id,change_type,app_id);
CREATE INDEX IF NOT EXISTS comparison_diff ON comparison_values(run_id,diff_flag,app_id);
CREATE TABLE IF NOT EXISTS application_segments(run_id text NOT NULL REFERENCES runs ON DELETE CASCADE, app_id text NOT NULL, data jsonb NOT NULL, PRIMARY KEY(run_id,app_id));
CREATE TABLE IF NOT EXISTS aggregate_facts(run_id text NOT NULL REFERENCES runs ON DELETE CASCADE, fact_id text NOT NULL, data jsonb NOT NULL, PRIMARY KEY(run_id,fact_id));
CREATE INDEX IF NOT EXISTS fact_path ON aggregate_facts(run_id,(data->>'PATH'));
CREATE TABLE IF NOT EXISTS aggregate_members(run_id text NOT NULL, fact_id text NOT NULL, app_id text NOT NULL, PRIMARY KEY(run_id,fact_id,app_id), FOREIGN KEY(run_id,fact_id) REFERENCES aggregate_facts(run_id,fact_id) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS aggregate_member_app ON aggregate_members(run_id,app_id);

CREATE TABLE IF NOT EXISTS run_check_results(
 run_id text NOT NULL REFERENCES runs ON DELETE CASCADE,
 row_id integer NOT NULL,
 check_id text NOT NULL,
 result text NOT NULL CHECK(result IN ('PASS','FAIL')),
 confidence text NOT NULL CHECK(confidence IN ('HIGH','LOW')),
 data jsonb NOT NULL,
 PRIMARY KEY(run_id,row_id)
);
CREATE INDEX IF NOT EXISTS check_results_filter ON run_check_results(run_id,result,confidence,check_id);
CREATE INDEX IF NOT EXISTS check_results_procedure ON run_check_results(run_id,check_id,row_id);
