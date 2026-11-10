#!/bin/bash
# Se ejecuta una sola vez, cuando el volumen de Postgres está vacío.
# Crea las bases y usuarios a partir de las variables del archivo .env.
set -e

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname postgres <<-EOSQL
  CREATE ROLE n8n            LOGIN PASSWORD '${N8N_DB_PASSWORD}';
  CREATE ROLE monitor_app    LOGIN PASSWORD '${MONITOR_DB_PASSWORD}';
  CREATE ROLE grafana_lector LOGIN PASSWORD '${GRAFANA_DB_PASSWORD}';

  CREATE DATABASE n8n     OWNER n8n;
  CREATE DATABASE monitor OWNER monitor_app;
EOSQL

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname monitor <<-EOSQL
  REVOKE ALL ON SCHEMA public FROM PUBLIC;
  GRANT ALL ON SCHEMA public TO monitor_app;
EOSQL
