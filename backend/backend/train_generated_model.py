"""
SmartPlant — Synthetic Dataset Generator & Random Forest Trainer
═══════════════════════════════════════════════════════════════════
Generates a realistic 10,000-sample irrigation dataset for Rice/Paddy
with temporal patterns, sensor noise, and diurnal cycles.
Then trains a RandomForestClassifier and saves the model bundle.

Usage:
    python train_generated_model.py
"""

import math
import os
import random

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix
from sklearn.model_selection import train_test_split

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_PATH = os.path.join(BASE_DIR, "irrigation_prediction.csv")
MODEL_PATH = os.path.join(BASE_DIR, "model.pkl")


def generate_smartplant_data(num_samples=10000):
    """
    Generate realistic synthetic sensor data for Rice/Paddy irrigation.

    Simulates:
      - Diurnal temperature/humidity cycles (hotter midday, cooler night)
      - Soil moisture that decays over time and jumps on watering/rain events
      - Sensor noise (±1-3% jitter) to mimic real hardware
      - Correlated environmental factors
      - Balanced class distribution for Low/Medium/High irrigation need
    """
    random.seed(42)
    np.random.seed(42)

    rows = []

    # Simulate multiple "days" of data with temporal progression
    hours_per_day = 24
    total_hours = num_samples  # One sample per simulated hour
    soil_moisture = random.uniform(20.0, 80.0)  # Start with varied moisture

    for hour_idx in range(total_hours):
        # ─── Diurnal cycle (0-23 hour of day) ─────────────────────
        hour_of_day = hour_idx % hours_per_day
        day_fraction = hour_of_day / 24.0

        # Temperature follows a sine curve: peak at ~14:00, min at ~05:00
        temp_base = 30.0  # Mean temperature for tropical rice climate
        temp_amplitude = random.uniform(6.0, 10.0)  # Daily swing
        temp_phase = (day_fraction - 0.583) * 2 * math.pi  # Peak at 14:00
        temp_raw = temp_base + temp_amplitude * math.sin(temp_phase)
        # Add random day-to-day variation
        temp_raw += random.gauss(0, 1.5)
        # Seasonal variation (some days hotter)
        if random.random() < 0.15:
            temp_raw += random.uniform(3, 7)  # Heat wave
        temp = round(max(18.0, min(45.0, temp_raw)), 1)

        # Humidity inversely correlates with temperature
        hum_base = 68.0
        hum_swing = random.uniform(10.0, 20.0)
        hum_raw = hum_base - hum_swing * math.sin(temp_phase) + random.gauss(0, 5)
        # High rainfall days have higher humidity
        humidity = round(max(25.0, min(95.0, hum_raw)), 1)

        # ─── Rainfall ─────────────────────────────────────────────
        # Most hours have no rain; some have bursts
        if random.random() < 0.08:  # 8% chance of rain in any hour
            rainfall = round(random.uniform(1.0, 25.0), 1)
            humidity = round(min(95.0, humidity + random.uniform(5, 15)), 1)
        else:
            rainfall = 0.0

        # ─── Soil moisture dynamics ───────────────────────────────
        # Decay: moisture drops over time (evaporation, plant uptake)
        evap_rate = 0.5 + (temp - 25) * 0.06  # Higher temp = faster drying
        wind_decay = random.uniform(0.0, 0.25)  # Wind helps evaporation
        soil_moisture -= (evap_rate + wind_decay)

        # Rain adds moisture
        if rainfall > 0:
            soil_moisture += rainfall * random.uniform(0.8, 1.5)

        # Simulated watering events — rare, to keep moisture varied
        if soil_moisture < 5:
            if random.random() < 0.3:
                soil_moisture += random.uniform(15, 30)

        # Natural capillary rise at night (slower)
        if hour_of_day >= 20 or hour_of_day <= 5:
            soil_moisture += random.uniform(0.0, 0.5)

        # Add sensor noise (capacitive sensor jitter)
        sensor_noise = random.gauss(0, 1.5)
        soil_reading = round(max(0.0, min(100.0, soil_moisture + sensor_noise)), 1)

        # Clamp actual moisture
        soil_moisture = max(0.0, min(100.0, soil_moisture))

        # ─── Sunlight hours (daily average, estimate) ─────────────
        if hour_of_day >= 6 and hour_of_day <= 18:
            sunlight = round(random.uniform(4.0, 11.0), 1)
        else:
            sunlight = round(random.uniform(0.0, 1.0), 1)
        # Rainy days have less sunlight
        if rainfall > 5:
            sunlight = round(max(0.0, sunlight - random.uniform(2, 5)), 1)

        # ─── Wind speed ───────────────────────────────────────────
        wind_base = random.uniform(1.0, 8.0)
        wind_gust = random.uniform(0, 6) if random.random() < 0.2 else 0
        wind = round(max(0.0, min(25.0, wind_base + wind_gust + random.gauss(0, 1.5))), 1)

        # ─── Irrigation need label ────────────────────────────────
        # Based on multiple factors with realistic decision boundaries
        irrigation_score = 0.0

        # Primary factor: soil moisture (widened ranges)
        if soil_reading < 20:
            irrigation_score += 3.0
        elif soil_reading < 35:
            irrigation_score += 2.0
        elif soil_reading < 50:
            irrigation_score += 1.0
        elif soil_reading < 65:
            irrigation_score += 0.3
        elif soil_reading > 80:
            irrigation_score -= 0.5

        # Temperature stress
        if temp > 38:
            irrigation_score += 1.5
        elif temp > 35:
            irrigation_score += 0.8
        elif temp > 32:
            irrigation_score += 0.3

        # Low humidity increases need
        if humidity < 40:
            irrigation_score += 0.8
        elif humidity < 50:
            irrigation_score += 0.3
        elif humidity > 80:
            irrigation_score -= 0.3  # Less evaporation

        # Wind increases evaporation
        if wind > 12:
            irrigation_score += 0.5
        elif wind > 8:
            irrigation_score += 0.2

        # Recent rainfall reduces need
        if rainfall > 10:
            irrigation_score -= 2.0
        elif rainfall > 5:
            irrigation_score -= 1.0
        elif rainfall > 1:
            irrigation_score -= 0.3

        # Strong sunlight increases need slightly
        if sunlight > 9:
            irrigation_score += 0.3

        # Add random noise to labels (real-world ambiguity)
        irrigation_score += random.gauss(0, 0.4)

        # Map score to label (lowered thresholds for better balance)
        if irrigation_score >= 2.0:
            irrigation_need = "High"
        elif irrigation_score >= 0.8:
            irrigation_need = "Medium"
        else:
            irrigation_need = "Low"

        rows.append([
            "Rice",
            soil_reading,
            temp,
            humidity,
            rainfall,
            sunlight,
            wind,
            irrigation_need,
        ])

    df = pd.DataFrame(
        rows,
        columns=[
            "Crop_Type",
            "Soil_Moisture",
            "Temperature_C",
            "Humidity",
            "Rainfall_mm",
            "Sunlight_Hours",
            "Wind_Speed_kmh",
            "Irrigation_Need",
        ],
    )
    df.to_csv(DATA_PATH, index=False)
    return df


def train_model(df):
    """Train a RandomForestClassifier on the generated dataset."""
    features = [
        "Soil_Moisture",
        "Temperature_C",
        "Humidity",
        "Rainfall_mm",
        "Sunlight_Hours",
        "Wind_Speed_kmh",
    ]

    label_to_int = {"Low": 0, "Medium": 1, "High": 2}
    int_to_label = {0: "Low", 1: "Medium", 2: "High"}

    X = df[features].astype(float)
    y = df["Irrigation_Need"].map(label_to_int).astype(int)

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    model = RandomForestClassifier(
        n_estimators=300,
        random_state=42,
        class_weight="balanced",
        max_depth=20,
        min_samples_split=5,
        min_samples_leaf=2,
    )
    model.fit(X_train, y_train)

    pred = model.predict(X_test)
    acc = accuracy_score(y_test, pred)

    print(f"Accuracy: {acc:.4f}")
    print("\nConfusion Matrix:\n", confusion_matrix(y_test, pred))
    print(
        "\nClassification Report:\n",
        classification_report(y_test, pred, target_names=["Low", "Medium", "High"]),
    )

    bundle = {
        "model": model,
        "features": features,
        "int_to_label": int_to_label,
    }
    joblib.dump(bundle, MODEL_PATH)
    return acc


def main():
    print("=" * 60)
    print("  SmartPlant — Dataset Generator & Model Trainer")
    print("=" * 60)

    df = generate_smartplant_data(num_samples=10000)
    print(f"\nGenerated dataset: {len(df)} rows")
    print(f"Saved to: {DATA_PATH}")

    print("\nClass distribution:")
    print(df["Irrigation_Need"].value_counts())
    print(f"\nClass percentages:")
    print((df["Irrigation_Need"].value_counts(normalize=True) * 100).round(1))

    print("\nSensor ranges:")
    for col in ["Soil_Moisture", "Temperature_C", "Humidity", "Rainfall_mm", "Sunlight_Hours", "Wind_Speed_kmh"]:
        print(f"  {col}: {df[col].min():.1f} — {df[col].max():.1f} (mean: {df[col].mean():.1f})")

    print("\n" + "=" * 60)
    print("  Training Random Forest...")
    print("=" * 60)

    acc = train_model(df)
    print(f"\nModel saved to: {MODEL_PATH}")
    print(f"Final accuracy: {acc:.4f}")


if __name__ == "__main__":
    main()
