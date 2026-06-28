-- Active: 1778556472858@@127.0.0.1@5432@massive
-- seed.sql — Massive 开发环境初始数据
-- 用法: psql -U postgres -d massive -f Tools/DB/seed.sql

CREATE DATABASE massive;

-- ═══ 建表 ═══

CREATE TABLE IF NOT EXISTS accounts (
    account_id   SERIAL PRIMARY KEY,
    username     VARCHAR(32)  NOT NULL UNIQUE,
    password_hash VARCHAR(96) NOT NULL,  -- hex(salt(16B) + hash(32B))
    email        VARCHAR(128) NOT NULL DEFAULT '',
    ban_until    TIMESTAMPTZ  NOT NULL DEFAULT to_timestamp(0),
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS players (
    player_id    BIGSERIAL PRIMARY KEY,
    account_id   INTEGER      NOT NULL REFERENCES accounts(account_id),
    name         VARCHAR(32)  NOT NULL DEFAULT '',
    level        INTEGER      NOT NULL DEFAULT 1,
    class_type   SMALLINT     NOT NULL DEFAULT 0,
    race_type    SMALLINT     NOT NULL DEFAULT 0,
    gender_type  SMALLINT     NOT NULL DEFAULT 0,
    experience   BIGINT       NOT NULL DEFAULT 0,
    gold         BIGINT       NOT NULL DEFAULT 0,
    map_id       INTEGER      NOT NULL DEFAULT 0,
    position_x   DOUBLE PRECISION NOT NULL DEFAULT 0,
    position_y   DOUBLE PRECISION NOT NULL DEFAULT 0,
    position_z   DOUBLE PRECISION NOT NULL DEFAULT 0,
    hp           INTEGER      NOT NULL DEFAULT 100,
    mp           INTEGER      NOT NULL DEFAULT 100,
    max_hp       INTEGER      NOT NULL DEFAULT 100,
    max_mp       INTEGER      NOT NULL DEFAULT 100,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
    last_login_at  TIMESTAMPTZ,
    last_logout_at TIMESTAMPTZ,
    delete_flag  BOOLEAN      NOT NULL DEFAULT FALSE
);

-- ═══ Seed 测试账号 ═══
-- 密码: test123
-- argon2id hash: hex(salt(16B) + hash(32B)) → 96 hex chars
-- 参数: 2 iterations, 64 MiB memory, 1 parallelism

INSERT INTO accounts (username, password_hash) VALUES
    ('testuser0', '79581171f246799d0abed93d7aa7bcf40e041a6141514dd3706d00596d9cc19fa7ac2d1f29b4349fddb9e3673017f421'),
    ('testuser1', '79581171f246799d0abed93d7aa7bcf40e041a6141514dd3706d00596d9cc19fa7ac2d1f29b4349fddb9e3673017f421'),
    ('testuser2', '79581171f246799d0abed93d7aa7bcf40e041a6141514dd3706d00596d9cc19fa7ac2d1f29b4349fddb9e3673017f421'),
    ('testuser3', '79581171f246799d0abed93d7aa7bcf40e041a6141514dd3706d00596d9cc19fa7ac2d1f29b4349fddb9e3673017f421'),
    ('testuser4', '79581171f246799d0abed93d7aa7bcf40e041a6141514dd3706d00596d9cc19fa7ac2d1f29b4349fddb9e3673017f421')
ON CONFLICT (username) DO NOTHING;
