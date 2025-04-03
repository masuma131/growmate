# AI-Based Automated Plant Watering System

## System Overview

This project implements an intelligent, automated watering system that:
1. Predicts optimal watering durations using environmental conditions
2. Continuously learns from watering outcomes
3. Maintains optimal soil moisture (target: 40%)

## Key Components

### Data Flow
- **Sensor Logs**: Initial conditions before watering
  ```json
  {
    "moisture_before": 35.2,
    "temperature": 24.5,
    "humidity": 60.1,
    "light": 850.3,
    "timestamp": "2023-11-15 14:30:00"
  }
  ```

- **Prediction Logs**: Watering actions and outcomes
  ```json
  {
    "sensor_log_id": "log_123456",
    "predicted_watering_time": 45.2,
    "moisture_after": 42.8,
    "timestamp": "2023-11-15 15:00:30",
    "watering_executed": true
  }
  ```

## Core Functions

### 1. Initial Model Training (`train.py`)

**Purpose**: Create the baseline model using historical data

**Usage**:
```bash
python train.py
```

**Input**:
- Firebase `sensor_data` collection with:
  - `temp`, `humidity`, `light`, `init_moisture`
  - `watering_duration`, `final_moisture`

**Output**:
- `base_model.pkl` with trained RandomForest model

### 2. Prediction Function (`predict.py`)

**Endpoint**: `POST /predict_watering`

**Input**:
```json
{
  "temp": 24.5,
  "humidity": 60.1,
  "light": 850.3,
  "init_moisture": 35.2
}
```

**Output**:
```json
{
  "watering_duration": 45.2
}
```

**Deployment**:
```bash
gcloud functions deploy predict_watering \
  --runtime python39 \
  --trigger-http \
  --allow-unauthenticated \
  --memory=256MB \
  --timeout=30s \
  --region=us-central1
```

### 3. Retraining Function (`retrain_model.py`)

**Endpoint**: `POST /retrain_model`

**Trigger**: Automatically after collecting new data

**Process**:
1. Pairs sensor logs with outcomes
2. Calculates optimal durations to reach 40% moisture
3. Retrains RandomForest model
4. Saves updated model to GCS

**Deployment**:
```bash
gcloud functions deploy retrain_model \
  --runtime python39 \
  --trigger-http \
  --allow-unauthenticated \
  --memory=512MB \
  --timeout=540s \
  --region=us-central1
```

## Development Setup

### Requirements
1. Python 3.9+
2. Firebase project with:
   - `sensor_logs` collection
   - `prediction_logs` collection
3. Google Cloud Storage bucket
4. Environment variables:
   ```
   DATABASE_URL=your-firebase-db-url
   BUCKET=your-gcs-bucket
   CERTIFICATE=path/to/firebase_credentials.json
   ```

### Installation
```bash
pip install -r requirements.txt
```

### Testing Locally
```bash
# Test prediction
python3 -m pip install -r requirements.txt
python3 predict.py

# Test retraining
python3 retrain_model.py
```

## Architecture Diagram

```
[Sensors] → [Sensor Logs] → [Predict] → [Watering Action]
               ↓                      ↓
          [Prediction Logs] ← [Moisture Reading]
                     ↓
               [Retrain Model]
                     ↓
              [Updated Model]
```

## Error Handling

All functions return structured responses:
```json
{
  "status": "success|error",
  "message": "Detailed information",
  "data": {}  // context-specific
}
```

## Monitoring

Check Cloud Function logs:
```bash
gcloud functions logs read [FUNCTION_NAME] --region=us-central1
```

## Contribution Guidelines

1. Use the established data formats
2. Maintain consistent feature ordering
3. Add tests for new functionality
4. Document changes in the README
5. Verify Firebase/GCS permissions

## Troubleshooting

Common issues:
- **Firebase auth errors**: Verify credential file permissions
- **Model loading failures**: Check GCS bucket access
- **Prediction mismatches**: Ensure feature order matches training
- **Timeout errors**: Increase function timeout duration


