-- 004_add_provision_step.sql
-- Adds provision_step to sessions for tracking async provisioning progress.
ALTER TABLE sessions ADD COLUMN IF NOT EXISTS provision_step VARCHAR(32) DEFAULT NULL;
