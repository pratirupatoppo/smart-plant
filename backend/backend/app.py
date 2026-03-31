import json
import os
import re
import sqlite3
import argparse
import traceback
from datetime import datetime, timedelta, timezone

import joblib
import jwt
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix
from sklearn.model_selection import train_test_split

from flask import Flask, jsonify, request
from flask_cors import CORS
from werkzeug.security import check_password_hash, generate_password_hash
import requests
import threading
import time
from croniter import croniter

try:
    from pywebpush import webpush, WebPushException
    WEBPUSH_AVAILABLE = True
except ImportError:
    WEBPUSH_AVAILABLE = False
    print("[WARN] pywebpush not installed. Push notifications disabled.")


app = Flask(__name__)
CORS(app)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_NAME = os.path.join(BASE_DIR, "smartplant.db")
MODEL_PATH = os.path.join(BASE_DIR, "model.pkl")
DATA_PATH = os.path.join(BASE_DIR, "irrigation_prediction.csv")
VALID_ZONES = {"A", "B", "C"}
MODEL_BUNDLE = None
JWT_SECRET = os.getenv("SMARTPLANT_JWT_SECRET", "smartplant-dev-secret-change-me-2026")
JWT_ALGORITHM = "HS256"
JWT_EXP_HOURS = 24
ESP_DEVICE_KEY = os.getenv("SMARTPLANT_ESP_DEVICE_KEY", "")
REQUIRE_ESP_DEVICE_KEY = os.getenv("SMARTPLANT_REQUIRE_ESP_DEVICE_KEY", "true").strip().lower() in {"1", "true", "yes", "on"}
MOISTURE_THRESHOLD = 35
MOISTURE_HYSTERESIS_MARGIN = 8

# ─── VAPID / Web Push ────────────────────────────────────────
VAPID_PRIVATE_KEY = os.getenv("SMARTPLANT_VAPID_PRIVATE_KEY", "J4whsTjM7C5mDl-1AmFgg1dGLzOk8fqs25v7iRb4ORY")
VAPID_PUBLIC_KEY = os.getenv("SMARTPLANT_VAPID_PUBLIC_KEY", "BLoO9XYn9Tp6fi5gp42Dq67dX5tvwf4qArBK6UTYkM3R5rIL1aer5TzkP8PZipoJk8Cw3-KBRdI8TzDMDc952pg")
VAPID_CLAIMS_EMAIL = os.getenv("SMARTPLANT_VAPID_EMAIL", "mailto:smartplant@example.com")
PUSH_ENABLED = bool(WEBPUSH_AVAILABLE and VAPID_PRIVATE_KEY and VAPID_PUBLIC_KEY)

if REQUIRE_ESP_DEVICE_KEY and not ESP_DEVICE_KEY:
    
    # ─── Weather API Configuration ────────────────────────────────────────
    WEATHER_API_URL = os.getenv("SMARTPLANT_WEATHER_API_URL", "https://api.openweathermap.org/data/2.5/weather")
    WEATHER_API_KEY = os.getenv("SMARTPLANT_WEATHER_API_KEY", "")
    raise RuntimeError("SMARTPLANT_ESP_DEVICE_KEY must be set when SMARTPLANT_REQUIRE_ESP_DEVICE_KEY is enabled")


def train_model_from_csv(data_path=DATA_PATH, model_out=MODEL_PATH):
    df = pd.read_csv(data_path)
    df = df[df["Crop_Type"].astype(str).str.strip().str.lower() == "rice"].copy()
    if df.empty:
        raise ValueError("No rows found for Crop_Type == Rice. Check dataset values.")

    features = [
        "Soil_Moisture",
        "Temperature_C",
        "Humidity",
        "Rainfall_mm",
        "Sunlight_Hours",
        "Wind_Speed_kmh",
    ]

    for c in features + ["Irrigation_Need"]:
        if c not in df.columns:
            raise ValueError(f"Missing column in dataset: {c}")

    df["Irrigation_Need"] = df["Irrigation_Need"].astype(str).str.strip().str.title()
    valid = {"Low", "Medium", "High"}
    df = df[df["Irrigation_Need"].isin(valid)].copy()

    label_to_int = {"Low": 0, "Medium": 1, "High": 2}
    int_to_label = {0: "Low", 1: "Medium", 2: "High"}

    X = df[features].astype(float)
    y = df["Irrigation_Need"].map(label_to_int).astype(int)

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    model = RandomForestClassifier(n_estimators=300, random_state=42, class_weight="balanced")
    model.fit(X_train, y_train)

    pred = model.predict(X_test)
    print("Accuracy:", accuracy_score(y_test, pred))
    print("\nConfusion Matrix:\n", confusion_matrix(y_test, pred))
    print("\nClassification Report:\n", classification_report(y_test, pred, target_names=["Low", "Medium", "High"]))

    bundle = {
        "model": model,
        "features": features,
        "int_to_label": int_to_label,
    }
    joblib.dump(bundle, model_out)
    print("\nSaved model bundle to:", model_out)


def db_conn():
    con = sqlite3.connect(DB_NAME)
    con.row_factory = sqlite3.Row
    return con


def ensure_database():
    con = sqlite3.connect(DB_NAME)
    cur = con.cursor()
    cur.execute("PRAGMA foreign_keys = ON")

    def has_column(table_name, column_name):
        cur.execute(f"PRAGMA table_info({table_name})")
        cols = [r[1] for r in cur.fetchall()]
        return column_name in cols

    def ensure_column(table_name, column_name, definition):
        if not has_column(table_name, column_name):
            cur.execute(f"ALTER TABLE {table_name} ADD COLUMN {column_name} {definition}")

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS users (
            user_id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            email TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
        """
    )

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS plants (
            plant_id TEXT PRIMARY KEY,
            plant_name TEXT,
            crop_type TEXT DEFAULT 'Paddy',
            zone_id TEXT NOT NULL,
            owner_user_id INTEGER NOT NULL,
            planted_date TEXT,
            qr_data TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (owner_user_id) REFERENCES users(user_id) ON DELETE CASCADE
        )
        """
    )

    ensure_column("plants", "plant_name", "TEXT")
    ensure_column("plants", "crop_type", "TEXT DEFAULT 'Paddy'")
    ensure_column("plants", "zone_id", "TEXT")
    ensure_column("plants", "owner_user_id", "INTEGER")
    ensure_column("plants", "planted_date", "TEXT")
    ensure_column("plants", "qr_data", "TEXT")
    ensure_column("plants", "created_at", "TIMESTAMP DEFAULT CURRENT_TIMESTAMP")

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS sensor_logs (
            log_id INTEGER PRIMARY KEY AUTOINCREMENT,
            zone_id TEXT NOT NULL,
            soil_moisture REAL NOT NULL,
            temperature_c REAL,
            humidity REAL,
            rainfall_mm REAL,
            sunlight_hours REAL,
            wind_speed_kmh REAL,
            source TEXT DEFAULT 'manual',
            ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
        """
    )

    ensure_column("sensor_logs", "temperature_c", "REAL")
    ensure_column("sensor_logs", "humidity", "REAL")
    ensure_column("sensor_logs", "rainfall_mm", "REAL")
    ensure_column("sensor_logs", "sunlight_hours", "REAL")
    ensure_column("sensor_logs", "wind_speed_kmh", "REAL")
    ensure_column("sensor_logs", "source", "TEXT DEFAULT 'manual'")

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS notifications (
            notif_id INTEGER PRIMARY KEY AUTOINCREMENT,
            owner_user_id INTEGER NOT NULL,
            plant_id TEXT,
            irrigation_need TEXT,
            message TEXT NOT NULL,
            status TEXT DEFAULT 'unread',
            created_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (owner_user_id) REFERENCES users(user_id) ON DELETE CASCADE,
            FOREIGN KEY (plant_id) REFERENCES plants(plant_id) ON DELETE CASCADE
        )
        """
    )

    ensure_column("notifications", "plant_id", "TEXT")
    ensure_column("notifications", "irrigation_need", "TEXT")
    ensure_column("notifications", "status", "TEXT DEFAULT 'unread'")

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS motor_state (
            zone_id TEXT PRIMARY KEY,
            is_on INTEGER NOT NULL DEFAULT 0,
            last_reason TEXT,
            last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
        """
    )

    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS push_subscriptions (
            sub_id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER NOT NULL,
            endpoint TEXT NOT NULL UNIQUE,
            p256dh TEXT NOT NULL,
            auth TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE
        )
        """
    )

    for zone in ["A", "B", "C"]:
        cur.execute(
            "INSERT OR IGNORE INTO motor_state (zone_id, is_on, last_reason) VALUES (?, 0, 'startup')",
            (zone,),
        )

    con.commit()
    con.close()


def _normalize_state(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value > 0
    if isinstance(value, str):
        return value.strip().lower() in {"on", "true", "1"}
    return False


def should_irrigate(soil_moisture, ml_prediction=None, current_state=False):
    try:
        soil_moisture = float(soil_moisture)
    except Exception:
        soil_moisture = None

    state_on = _normalize_state(current_state)
    ml = str(ml_prediction or "").strip().title()

    if soil_moisture is None:
        if ml in ["Medium", "High"]:
            return True
        return state_on

    on_threshold = MOISTURE_THRESHOLD
    off_threshold = MOISTURE_THRESHOLD + MOISTURE_HYSTERESIS_MARGIN

    if not state_on:
        if soil_moisture < on_threshold:
            return True
        if ml in ["Medium", "High"]:
            return True
        return False

    if ml == "High":
        return True

    if soil_moisture >= off_threshold and ml not in ["Medium", "High"]:
        return False

    if soil_moisture < on_threshold:
        return True

    return state_on


def require_zone(zone):
    return (zone or "").upper() in VALID_ZONES


def to_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def normalize_prediction(pred):
    p = str(pred or "").strip().title()
    return p if p in {"Low", "Medium", "High"} else "Low"


def load_model_any():
    global MODEL_BUNDLE
    if MODEL_BUNDLE is not None:
        return MODEL_BUNDLE
    if not os.path.exists(MODEL_PATH):
        return None
    MODEL_BUNDLE = joblib.load(MODEL_PATH)
    return MODEL_BUNDLE


def fallback_predict(soil_moisture):
    soil = to_float(soil_moisture)
    if soil < 25:
        return "High"
    if soil < 45:
        return "Medium"
    return "Low"


def predict_label(bundle_or_model, soil, temp, hum, rain, sun, wind):
    if bundle_or_model is None:
        return fallback_predict(soil)

    try:
        if isinstance(bundle_or_model, dict) and "model" in bundle_or_model:
            model = bundle_or_model["model"]
            features = bundle_or_model.get("features", [])
            int_to_label = bundle_or_model.get("int_to_label", {})

            fmap = {
                "Soil_Moisture": soil,
                "Temperature_C": temp,
                "Humidity": hum,
                "Rainfall_mm": rain,
                "Sunlight_Hours": sun,
                "Wind_Speed_kmh": wind,
            }
            X = [[to_float(fmap.get(f)) for f in features]]
            pred_raw = model.predict(X)[0]
            try:
                pred_raw = int_to_label.get(int(pred_raw), "Low")
            except (ValueError, TypeError):
                pass
            return normalize_prediction(pred_raw)

        X = [[to_float(soil), to_float(temp), to_float(hum), to_float(rain), to_float(sun), to_float(wind)]]
        return normalize_prediction(bundle_or_model.predict(X)[0])
    except Exception:
        return fallback_predict(soil)


def get_latest_sensor(zone_id):
    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        SELECT * FROM sensor_logs
        WHERE zone_id=?
        ORDER BY ts DESC
        LIMIT 1
        """,
        (zone_id,),
    )
    row = cur.fetchone()
    con.close()
    return dict(row) if row else None


def set_motor_state(zone_id, is_on, reason):
    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        INSERT INTO motor_state (zone_id, is_on, last_reason)
        VALUES (?, ?, ?)
        ON CONFLICT(zone_id)
        DO UPDATE SET is_on=excluded.is_on, last_reason=excluded.last_reason, last_updated=CURRENT_TIMESTAMP
        """,
        (zone_id, 1 if is_on else 0, reason),
    )
    con.commit()
    con.close()


def get_motor_state(zone_id):
    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT is_on, last_reason FROM motor_state WHERE zone_id=?", (zone_id,))
    row = cur.fetchone()
    con.close()
    if not row:
        return False, "not-set"
    return bool(row["is_on"]), (row["last_reason"] or "unknown")


def parse_manual_override(reason):
    txt = str(reason or "")
    m = re.search(r"manual-toggle\|until:(\d+)", txt)
    if not m:
        return False, None
    until_ts = int(m.group(1))
    now_ts = int(datetime.now(timezone.utc).timestamp())
    return now_ts <= until_ts, until_ts


def create_notification(owner_user_id, plant_id, irrigation_need, message):
    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        INSERT INTO notifications (owner_user_id, plant_id, irrigation_need, message, status)
        VALUES (?, ?, ?, ?, 'unread')
        """,
        (owner_user_id, plant_id, irrigation_need, message),
    )
    con.commit()
    con.close()


def notify_zone_owners(zone_id, irrigation_need, message):
    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        SELECT owner_user_id, plant_id
        FROM plants
        WHERE zone_id=?
        """,
        (zone_id,),
    )
    rows = cur.fetchall()
    con.close()

    for r in rows:
        create_notification(r["owner_user_id"], r["plant_id"], irrigation_need, message)
        # Send Web Push notification
        send_push_to_user(r["owner_user_id"], irrigation_need, message)


def send_push_to_user(user_id, irrigation_need, message):
    """Send Web Push notification to all subscribed devices for a user."""
    if not PUSH_ENABLED:
        return

    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT endpoint, p256dh, auth FROM push_subscriptions WHERE user_id=?", (user_id,))
    subs = cur.fetchall()
    con.close()

    if not subs:
        return

    # Determine notification icon/title based on irrigation need
    need = str(irrigation_need or "").strip().title()
    if need == "High":
        title = "🚨 Plant Alert — Soil is DRY!"
        icon_emoji = "😢"
    elif need == "Low":
        title = "🌱 Plant Happy — Moisture OK!"
        icon_emoji = "😊"
    else:
        title = "💧 SmartPlant Update"
        icon_emoji = "💧"

    payload = json.dumps({
        "title": title,
        "body": message,
        "icon": "./icon.svg",
        "badge": "./icon.svg",
        "tag": f"smartplant-{need.lower()}",
        "data": {
            "url": "/",
            "irrigation_need": need,
        }
    })

    stale_endpoints = []

    for sub in subs:
        subscription_info = {
            "endpoint": sub["endpoint"],
            "keys": {
                "p256dh": sub["p256dh"],
                "auth": sub["auth"],
            }
        }
        try:
            webpush(
                subscription_info=subscription_info,
                data=payload,
                vapid_private_key=VAPID_PRIVATE_KEY,
                vapid_claims={"sub": VAPID_CLAIMS_EMAIL},
            )
        except WebPushException as e:
            status_code = getattr(e, 'response', None)
            if status_code and hasattr(status_code, 'status_code') and status_code.status_code in (404, 410):
                stale_endpoints.append(sub["endpoint"])
            else:
                print(f"[PUSH] Error sending to {sub['endpoint'][:50]}...: {e}")
        except Exception as e:
            print(f"[PUSH] Unexpected error: {e}")

    # Clean up stale subscriptions
    if stale_endpoints:
        con = db_conn()
        cur = con.cursor()
        for ep in stale_endpoints:
            cur.execute("DELETE FROM push_subscriptions WHERE endpoint=?", (ep,))
        con.commit()
        con.close()


def create_access_token(user_id, email):
    payload = {
        "sub": str(user_id),
        "email": email,
        "exp": datetime.now(timezone.utc) + timedelta(hours=JWT_EXP_HOURS),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)


def get_auth_user_id():
    auth = request.headers.get("Authorization", "")
    if not auth.startswith("Bearer "):
        return None, "missing bearer token"

    token = auth.split(" ", 1)[1].strip()
    if not token:
        return None, "missing bearer token"

    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return int(payload.get("sub")), None
    except jwt.ExpiredSignatureError:
        return None, "token expired"
    except Exception:
        return None, "invalid token"


def require_auth_user():
    user_id, err = get_auth_user_id()
    if err:
        return None, (jsonify({"error": err}), 401)
    return user_id, None


def require_esp_device_key():
    if not REQUIRE_ESP_DEVICE_KEY:
        return None

    if not ESP_DEVICE_KEY:
        return jsonify({"error": "server missing device key configuration"}), 503

    provided = request.headers.get("X-Device-Key", "").strip()
    if not provided:
        data = request.json or {}
        provided = str(data.get("device_key") or "").strip()

    if provided != ESP_DEVICE_KEY:
        return jsonify({"error": "invalid device key"}), 401
    return None


ensure_database()


@app.get("/api/health")
def health():
    return jsonify({"status": "ok", "model_loaded": bool(load_model_any())})


@app.post("/api/auth/register")
def register():
    data = request.json or {}
    name = (data.get("name") or "").strip()
    email = (data.get("email") or "").strip().lower()
    password = data.get("password") or ""

    if not name or not email or not password:
        return jsonify({"error": "missing fields"}), 400
    if not re.match(r"^[^@\s]+@[^@\s]+\.[^@\s]+$", email):
        return jsonify({"error": "invalid email"}), 400
    if len(password) < 6:
        return jsonify({"error": "password must be at least 6 characters"}), 400

    con = db_conn()
    cur = con.cursor()
    try:
        cur.execute(
            "INSERT INTO users (name, email, password_hash) VALUES (?, ?, ?)",
            (name, email, generate_password_hash(password)),
        )
        con.commit()
        user_id = cur.lastrowid
        token = create_access_token(user_id, email)
        return jsonify({"message": "registered", "user_id": user_id, "name": name, "email": email, "token": token}), 201
    except sqlite3.IntegrityError:
        return jsonify({"error": "email already exists"}), 409
    finally:
        con.close()


@app.post("/api/auth/login")
def login():
    data = request.json or {}
    email = (data.get("email") or "").strip().lower()
    password = data.get("password") or ""
    if not email or not password:
        return jsonify({"error": "missing fields"}), 400

    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT * FROM users WHERE email=?", (email,))
    user = cur.fetchone()
    con.close()

    if not user:
        return jsonify({"error": "user not found"}), 404
    if not check_password_hash(user["password_hash"], password):
        return jsonify({"error": "wrong password"}), 401

    token = create_access_token(user["user_id"], user["email"])
    return jsonify({"user_id": user["user_id"], "name": user["name"], "email": user["email"], "token": token})


@app.post("/api/plants/add")
def add_plant():
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    plant_id = (data.get("plant_id") or "").strip().upper()
    plant_name = (data.get("plant_name") or "").strip() or None
    zone_id = (data.get("zone_id") or "").strip().upper()
    owner_user_id = data.get("owner_user_id")
    qr_data = (data.get("qr_data") or f"PLANT:{plant_id}").strip()

    if not plant_id or not owner_user_id or not require_zone(zone_id):
        return jsonify({"error": "missing or invalid fields"}), 400
    if int(owner_user_id) != int(auth_user_id):
        return jsonify({"error": "forbidden user mismatch"}), 403

    con = db_conn()
    cur = con.cursor()
    try:
        cur.execute(
            """
            INSERT INTO plants (plant_id, plant_name, crop_type, zone_id, owner_user_id, qr_data)
            VALUES (?, ?, 'Paddy', ?, ?, ?)
            """,
            (plant_id, plant_name, zone_id, owner_user_id, qr_data),
        )
        con.commit()
        return jsonify({"message": "plant added", "plant_id": plant_id}), 201
    except sqlite3.IntegrityError:
        return jsonify({"error": "plant already exists"}), 409
    finally:
        con.close()


@app.get("/api/plants/mine")
def list_my_plants():
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        SELECT plant_id, plant_name, zone_id, crop_type, created_at
        FROM plants
        WHERE owner_user_id=?
        ORDER BY created_at DESC, plant_id DESC
        """,
        (auth_user_id,),
    )
    rows = [dict(r) for r in cur.fetchall()]
    con.close()
    return jsonify(rows)


@app.post("/api/sensors/log")
def save_sensor_log():
    _, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    zone_id = (data.get("zone_id") or "").strip().upper()
    if not require_zone(zone_id):
        return jsonify({"error": "invalid zone"}), 400

    soil = to_float(data.get("soil_moisture"))
    temp = to_float(data.get("temperature_c"))
    hum = to_float(data.get("humidity"))
    rain = to_float(data.get("rainfall_mm"))
    sun = to_float(data.get("sunlight_hours"))
    wind = to_float(data.get("wind_speed_kmh"))

    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        INSERT INTO sensor_logs (zone_id, soil_moisture, temperature_c, humidity, rainfall_mm, sunlight_hours, wind_speed_kmh, source)
        VALUES (?, ?, ?, ?, ?, ?, ?, 'manual')
        """,
        (zone_id, soil, temp, hum, rain, sun, wind),
    )
    con.commit()
    con.close()

    return jsonify({"message": "sensor log saved", "zone_id": zone_id})


@app.post("/api/esp/sensor")
def esp_sensor():
    esp_err = require_esp_device_key()
    if esp_err:
        return esp_err

    data = request.json or {}
    zone_id = (data.get("zone_id") or "").strip().upper()
    if not require_zone(zone_id):
        return jsonify({"error": "invalid zone"}), 400

    soil = to_float(data.get("soil_moisture"))
    temp = to_float(data.get("temperature_c"), 28.0)
    hum = to_float(data.get("humidity"), 60.0)
    rain = to_float(data.get("rainfall_mm"), 0.0)
    sun = to_float(data.get("sunlight_hours"), 6.0)
    wind = to_float(data.get("wind_speed_kmh"), 3.0)

    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        INSERT INTO sensor_logs (zone_id, soil_moisture, temperature_c, humidity, rainfall_mm, sunlight_hours, wind_speed_kmh, source)
        VALUES (?, ?, ?, ?, ?, ?, ?, 'esp')
        """,
        (zone_id, soil, temp, hum, rain, sun, wind),
    )
    con.commit()
    con.close()

    model = load_model_any()
    prediction = predict_label(model, soil, temp, hum, rain, sun, wind)
    current_state, current_reason = get_motor_state(zone_id)
    manual_active, manual_until = parse_manual_override(current_reason)

    if manual_active:
        should_water = current_state
        reason = current_reason
    else:
        should_water = should_irrigate(soil, prediction, current_state)
        reason = f"auto:{prediction}|soil:{soil}"

    set_motor_state(zone_id, should_water, reason)

    if (not manual_active) and should_water and not current_state:
        notify_zone_owners(
            zone_id,
            prediction,
            f"Zone {zone_id}: Soil is dry ({soil:.1f}). Motor turned ON automatically.",
        )
    elif (not manual_active) and (not should_water) and current_state:
        notify_zone_owners(
            zone_id,
            "Low",
            f"Zone {zone_id}: Moisture recovered ({soil:.1f}). Motor turned OFF automatically.",
        )

    return jsonify(
        {
            "zone_id": zone_id,
            "soil_moisture": soil,
            "prediction": prediction,
            "motor_on": should_water,
            "reason": reason,
            "manual_override_active": manual_active,
            "manual_override_until": manual_until,
        }
    )


@app.get("/api/sensors/latest/<zone>")
def latest(zone):
    _, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    zone = (zone or "").upper()
    if not require_zone(zone):
        return jsonify({"error": "invalid zone"}), 400

    row = get_latest_sensor(zone)
    if not row:
        return jsonify({"error": "no data"}), 404

    motor_on, motor_reason = get_motor_state(zone)
    row["motor_on"] = motor_on
    row["motor_reason"] = motor_reason
    return jsonify(row)


@app.get("/api/predict/live/<zone>")
def predict_live(zone):
    _, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    zone = (zone or "").upper()
    if not require_zone(zone):
        return jsonify({"error": "invalid zone"}), 400

    latest_sensor = get_latest_sensor(zone)
    if not latest_sensor:
        return jsonify({"error": "no data"}), 404

    prediction = predict_label(
        load_model_any(),
        latest_sensor.get("soil_moisture"),
        latest_sensor.get("temperature_c"),
        latest_sensor.get("humidity"),
        latest_sensor.get("rainfall_mm"),
        latest_sensor.get("sunlight_hours"),
        latest_sensor.get("wind_speed_kmh"),
    )
    prediction = normalize_prediction(prediction)

    motor_on, motor_reason = get_motor_state(zone)
    manual_active, manual_until = parse_manual_override(motor_reason)
    return jsonify(
        {
            "zone_id": zone,
            "prediction": prediction,
            "motor_on": motor_on,
            "motor_reason": motor_reason,
            "manual_override_active": manual_active,
            "manual_override_until": manual_until,
            "latest_sensor": latest_sensor,
        }
    )


@app.post("/api/predict/irrigation")
def predict_irrigation():
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    plant_id = (data.get("plant_id") or "").strip().upper()
    zone_id = (data.get("zone_id") or "").strip().upper()
    owner_user_id = data.get("owner_user_id")

    if not plant_id or not owner_user_id or not require_zone(zone_id):
        return jsonify({"error": "missing or invalid fields"}), 400
    if int(owner_user_id) != int(auth_user_id):
        return jsonify({"error": "forbidden user mismatch"}), 403

    latest_sensor = get_latest_sensor(zone_id)
    if not latest_sensor:
        return jsonify({"error": "no sensor logs for this zone"}), 404

    prediction = predict_label(
        load_model_any(),
        latest_sensor.get("soil_moisture"),
        latest_sensor.get("temperature_c"),
        latest_sensor.get("humidity"),
        latest_sensor.get("rainfall_mm"),
        latest_sensor.get("sunlight_hours"),
        latest_sensor.get("wind_speed_kmh"),
    )
    prediction = normalize_prediction(prediction)

    current_state, _ = get_motor_state(zone_id)
    should_water = should_irrigate(latest_sensor.get("soil_moisture"), prediction, current_state)
    set_motor_state(zone_id, should_water, f"predict:{prediction}")

    notif_created = False
    if prediction in {"Medium", "High"}:
        create_notification(
            owner_user_id,
            plant_id,
            prediction,
            f"Plant {plant_id} in Zone {zone_id}: Irrigation need is {prediction}.",
        )
        notif_created = True

    return jsonify(
        {
            "prediction": prediction,
            "notification_created": notif_created,
            "motor_on": should_water,
            "latest_sensor": latest_sensor,
        }
    )

@app.post("/api/predict/manual")
def predict_manual():
    data = request.json or {}
    soil = data.get("soil_moisture")
    temp = data.get("temperature_c")
    hum = data.get("humidity")

    if soil is None or temp is None or hum is None:
        return jsonify({"error": "Missing sensor values"}), 400

    # Pass None for rainfall, sunlight, wind so they default to 0.0
    prediction = predict_label(
        load_model_any(),
        soil,
        temp,
        hum,
        None,
        None,
        None,
    )
    prediction = normalize_prediction(prediction)

    return jsonify({"prediction": prediction})


@app.get("/api/notifications/<int:user_id>")
def notifications(user_id):
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err
    if int(user_id) != int(auth_user_id):
        return jsonify({"error": "forbidden user mismatch"}), 403

    con = db_conn()
    cur = con.cursor()
    cur.execute(
        """
        SELECT * FROM notifications
        WHERE owner_user_id=?
        ORDER BY created_time DESC, notif_id DESC
        LIMIT 100
        """,
        (user_id,),
    )
    rows = cur.fetchall()
    con.close()
    return jsonify([dict(r) for r in rows])


@app.post("/api/notifications/<int:notif_id>/read")
def mark_notification_read(notif_id):
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT owner_user_id FROM notifications WHERE notif_id=?", (notif_id,))
    row = cur.fetchone()
    if not row:
        con.close()
        return jsonify({"error": "notification not found"}), 404
    if int(row["owner_user_id"]) != int(auth_user_id):
        con.close()
        return jsonify({"error": "forbidden user mismatch"}), 403

    cur.execute("UPDATE notifications SET status='read' WHERE notif_id=?", (notif_id,))
    con.commit()
    con.close()
    return jsonify({"message": "notification marked read", "notif_id": notif_id})


@app.post("/api/notifications/read-all")
def mark_all_notifications_read():
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    con = db_conn()
    cur = con.cursor()
    cur.execute("UPDATE notifications SET status='read' WHERE owner_user_id=?", (auth_user_id,))
    changed = cur.rowcount
    con.commit()
    con.close()
    return jsonify({"message": "all notifications marked read", "updated": changed})


@app.post("/api/motor/toggle")
def toggle_motor():
    _, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    zone_id = (data.get("zone_id") or "A").strip().upper()
    desired = data.get("is_on")
    if not require_zone(zone_id):
        return jsonify({"error": "invalid zone"}), 400

    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT is_on FROM motor_state WHERE zone_id=?", (zone_id,))
    row = cur.fetchone()
    current = bool(row["is_on"]) if row else False
    next_state = (not current) if desired is None else bool(desired)
    con.close()

    manual_until = int(datetime.now(timezone.utc).timestamp()) + 120
    set_motor_state(zone_id, next_state, f"manual-toggle|until:{manual_until}")
    return jsonify({
        "zone_id": zone_id,
        "motor_on": next_state,
        "manual_override_active": True,
        "manual_override_until": manual_until,
    })


@app.get("/api/motor/status/<zone>")
def motor_status(zone):
    _, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    zone = (zone or "").upper()
    if not require_zone(zone):
        return jsonify({"error": "invalid zone"}), 400

    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT * FROM motor_state WHERE zone_id=?", (zone,))
    row = cur.fetchone()
    con.close()

    if not row:
        return jsonify({"zone_id": zone, "motor_on": False, "last_reason": "not-set"})
    out = dict(row)
    out["motor_on"] = bool(out.pop("is_on", 0))
    return jsonify(out)


# ═══════════════════════════════════════════════════════════════
# WEB PUSH ENDPOINTS
# ═══════════════════════════════════════════════════════════════

@app.get("/api/push/vapid-key")
def get_vapid_key():
    """Return the public VAPID key for frontend push subscription."""
    return jsonify({
        "publicKey": VAPID_PUBLIC_KEY,
        "enabled": PUSH_ENABLED,
    })


@app.post("/api/push/subscribe")
def push_subscribe():
    """Save a push subscription for the authenticated user."""
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    endpoint = (data.get("endpoint") or "").strip()
    keys = data.get("keys") or {}
    p256dh = (keys.get("p256dh") or "").strip()
    auth = (keys.get("auth") or "").strip()

    if not endpoint or not p256dh or not auth:
        return jsonify({"error": "missing subscription fields"}), 400

    con = db_conn()
    cur = con.cursor()
    try:
        cur.execute(
            """
            INSERT INTO push_subscriptions (user_id, endpoint, p256dh, auth)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(endpoint)
            DO UPDATE SET user_id=excluded.user_id, p256dh=excluded.p256dh, auth=excluded.auth
            """,
            (auth_user_id, endpoint, p256dh, auth),
        )
        con.commit()
        return jsonify({"message": "subscribed", "push_enabled": PUSH_ENABLED})
    except Exception as e:
        return jsonify({"error": str(e)}), 500
    finally:
        con.close()


@app.post("/api/push/unsubscribe")
def push_unsubscribe():
    """Remove a push subscription."""
    auth_user_id, auth_err = require_auth_user()
    if auth_err:
        return auth_err

    data = request.json or {}
    endpoint = (data.get("endpoint") or "").strip()
    if not endpoint:
        return jsonify({"error": "missing endpoint"}), 400

    con = db_conn()
    cur = con.cursor()
    cur.execute(
        "DELETE FROM push_subscriptions WHERE endpoint=? AND user_id=?",
        (endpoint, auth_user_id),
    )
    deleted = cur.rowcount
    con.commit()
    con.close()
    return jsonify({"message": "unsubscribed", "deleted": deleted})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SmartPlant backend")
    parser.add_argument("--train-model", action="store_true", help="Train and save model.pkl from irrigation_prediction.csv")
    args = parser.parse_args()

    if PUSH_ENABLED:
        print("[PUSH] Web Push notifications ENABLED")
    else:
        reasons = []
        if not WEBPUSH_AVAILABLE:
            reasons.append("pywebpush not installed")
        if not VAPID_PRIVATE_KEY:
            reasons.append("SMARTPLANT_VAPID_PRIVATE_KEY not set")
        if not VAPID_PUBLIC_KEY:
            reasons.append("SMARTPLANT_VAPID_PUBLIC_KEY not set")
        print(f"[PUSH] Web Push notifications DISABLED ({', '.join(reasons)})")

    if args.train_model:
        train_model_from_csv()
    else:
        app.run(host="0.0.0.0", port=5000, debug=True)