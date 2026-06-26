CREATE TABLE IF NOT EXISTS players
(
    player_id      BIGSERIAL PRIMARY KEY,
    account_id     INTEGER NOT NULL REFERENCES accounts(account_id),
    name           VARCHAR(64) NOT NULL,
    level          INTEGER NOT NULL DEFAULT 1,
    class_type     SMALLINT NOT NULL DEFAULT 0,
    race_type      SMALLINT NOT NULL DEFAULT 0,
    gender_type    SMALLINT NOT NULL DEFAULT 0,
    experience     BIGINT NOT NULL DEFAULT 0,
    gold           BIGINT NOT NULL DEFAULT 0,
    map_id         INTEGER NOT NULL DEFAULT 0,
    position_x     DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    position_y     DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    position_z     DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    hp             INTEGER NOT NULL DEFAULT 100,
    mp             INTEGER NOT NULL DEFAULT 50,
    max_hp         INTEGER NOT NULL DEFAULT 100,
    max_mp         INTEGER NOT NULL DEFAULT 50,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_login_at  TIMESTAMPTZ,
    last_logout_at TIMESTAMPTZ,
    delete_flag    BOOLEAN NOT NULL DEFAULT false
);
