-- 005_add_user_eip.sql
-- Adds Elastic IP tracking to users for stable relay addresses.
-- Each user gets one EIP that persists across relay provision cycles.

ALTER TABLE users ADD COLUMN IF NOT EXISTS eip_allocation_id TEXT;
ALTER TABLE users ADD COLUMN IF NOT EXISTS eip_public_ip INET;
