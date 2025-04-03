import functions_framework
import firebase_admin
from firebase_admin import credentials, db
from sklearn.ensemble import RandomForestRegressor
import pickle
import pandas as pd
from google.cloud import storage
import logging
from datetime import datetime
import os
import numpy as np

# Configuration
TARGET_MOISTURE = 50  # Target moisture level
MIN_DURATION = 5      # Minimum watering duration (seconds)
MAX_DURATION = 600    # Maximum watering duration (seconds)

# Set up logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger()

# Initialize Firebase
try:
    if not firebase_admin._apps:
        cred = credentials.Certificate(os.getenv("CERTIFICATE"))
        firebase_admin.initialize_app(cred, {
            'databaseURL': os.getenv("DATABASE_URL")
        })
except Exception as e:
    logger.error(f"Firebase initialization failed: {str(e)}")


def get_training_data():
    """Fetch complete training data from Firebase with sensor logs and predictions"""
    # Get sensor logs (initial conditions)
    sensor_ref = db.reference('initial_training_logs')
    sensor_data = sensor_ref.get() or {}
    
    # Get prediction logs (with outcomes)
    pred_ref = db.reference('training_logs')
    pred_data = pred_ref.get() or {}
    
    records = []
    
    # Match sensor logs with their corresponding predictions
    for pred_id, pred in pred_data.items():
        try:
            # Find the original sensor data this prediction was based on
            sensor_id = pred['sensor_log_id']
            if sensor_id not in sensor_data:
                continue
                
            sensor = sensor_data[sensor_id]
            
            records.append({
                'moisture_before': float(sensor['moisture_before']),
                'temperature': float(sensor['temperature']),
                'humidity': float(sensor['humidity']),
                'light': float(sensor['light']),
                'predicted_time': float(pred['predicted_watering_time']),
                'moisture_after': float(pred['moisture_after']),
            })
            
        except (KeyError, ValueError, TypeError) as e:
            logger.warning(f"Skipping record {pred_id}: {str(e)}")
            continue
            
    return pd.DataFrame(records)

def calculate_optimal_durations(df):
    """Calculate what duration would have achieved exactly TARGET_MOISTURE"""
    # Only use records where watering was actually performed
    df = df[df['predicted_time'] > 0].copy()
    
    if len(df) == 0:
        return df
    
    # Calculate moisture change rate (% per second)
    df['moisture_change_per_sec'] = (df['moisture_after'] - df['moisture_before']) / df['predicted_time']
    
    # Filter out unrealistic rates (0.01 to 1.0 %/second)
    valid = df[(df['moisture_change_per_sec'] > 0.01) & 
               (df['moisture_change_per_sec'] < 1.0) &
               (df['moisture_after'] > df['moisture_before'])]
    
    # Calculate optimal duration for TARGET_MOISTURE
    valid['required_increase'] = np.maximum(TARGET_MOISTURE - valid['moisture_before'], 0)
    valid['optimal_duration'] = valid['required_increase'] / valid['moisture_change_per_sec']
    
    # Apply duration constraints
    valid['optimal_duration'] = valid['optimal_duration'].clip(MIN_DURATION, MAX_DURATION)
    
    return valid

def train_model(df):
    """Train RandomForest model on optimal durations"""
    features = ['moisture_before', 'temperature', 'humidity', 'light']
    X = df[features]
    y = df['optimal_duration']
    
    model = RandomForestRegressor(
        n_estimators=150,
        max_depth=7,
        min_samples_split=5,
        random_state=42,
        min_samples_leaf=2
    )
    model.fit(X, y)
    return model, features

def save_model(model, features, record_count):
    """Save model to Google Cloud Storage"""
    model_info = {
        'model': model,
        'feature_names': features,
        'training_date': datetime.now().isoformat(),
        'records_used': record_count,
        'target_moisture': TARGET_MOISTURE,
        'min_duration': MIN_DURATION,
        'max_duration': MAX_DURATION
    }
    
    # Save to temporary file
    with open('/tmp/model.pkl', 'wb') as f:
        pickle.dump(model_info, f)
    
    # Upload to GCS
    storage_client = storage.Client()
    bucket = storage_client.bucket(os.getenv("BUCKET"))
    blob = bucket.blob('optimized_model.pkl')
    blob.upload_from_filename('/tmp/model.pkl')
    logger.info(f"Model saved to GCS with {record_count} records")

@functions_framework.http
def retrain_model(request):
    """HTTP Cloud Function to retrain the watering model"""
    try:
        logger.info("Starting model retraining process")
        
        # 1. Fetch and prepare training data
        df = get_training_data()
        if len(df) == 0:
            logger.error("No training data available")
            return {"status": "error", "message": "No training data available"}, 400
        
        # 2. Calculate optimal durations
        training_df = calculate_optimal_durations(df)
        if len(training_df) == 0:
            logger.error("No valid records after filtering")
            return {"status": "error", "message": "No valid records after filtering"}, 400
        
        # 3. Train new model
        model, features = train_model(training_df)
        
        # 4. Save model
        save_model(model, features, len(training_df))
        
        logger.info(f"Successfully retrained on {len(training_df)} records")
        return {
            "status": "success",
            "records_used": len(training_df),
            "average_optimal_duration": round(training_df['optimal_duration'].mean(), 1),
            "target_moisture": TARGET_MOISTURE
        }, 200
        
    except Exception as e:
        logger.error(f"Retraining failed: {str(e)}", exc_info=True)
        return {
            "status": "error",
            "message": str(e)
        }, 500