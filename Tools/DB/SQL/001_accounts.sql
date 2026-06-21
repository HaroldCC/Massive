CREATE TABLE IF NOT EXISTS accounts
(
    account_id    SERIAL PRIMARY KEY,
    username      VARCHAR(255) NOT NULL UNIQUE,
    password_hash VARCHAR(96)  NOT NULL,
    email         VARCHAR(255),
    ban_until     TIMESTAMPTZ DEFAULT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_login_at TIMESTAMPTZ
);
