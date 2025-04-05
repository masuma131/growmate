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
from google.cloud import secretmanager

# Configuration
TARGET_MOISTURE = 50  # Your target moisture level
MIN_DURATION = 5      # Minimum watering duration (seconds)
MAX_DURATION = 600    # Maximum watering duration (seconds)

# Set up logging
logging.basicConfig(level=logging.DEBUG)  # Set to DEBUG to capture all logs
logger = logging.getLogger()

def get_firebase_credentials_from_secret():
    """Fetch Firebase credentials from Google Secret Manager"""
    project_id = os.getenv("GCP_PROJECT") or os.getenv("GOOGLE_CLOUD_PROJECT")
    secret_name = "firebase-adminsdk-cert"  # Secret name

    client = secretmanager.SecretManagerServiceClient()
    name = f"projects/{project_id}/secrets/{secret_name}/versions/latest"
    response = client.access_secret_version(name=name)
    secret_content = response.payload.data.decode("UTF-8")

    # Write to a temporary file
    temp_path = "/tmp/firebase_adminsdk.json"
    with open(temp_path, "w") as f:
        f.write(secret_content)
    logger.debug(f"Firebase credentials written to {temp_path}")
    return credentials.Certificate(temp_path)

def initialize_firebase():
    """Ensure Firebase is initialized properly"""
    if not firebase_admin._apps:
        cred = get_firebase_credentials_from_secret()
        firebase_admin.initialize_app(cred, {
            'databaseURL': os.getenv("DATABASE_URL")
        })
        logger.info("Firebase app initialized successfully")
    else:
        logger.info("Firebase app already initialized")
        
def get_training_data():
    """Fetch combined training data from Firebase (initial + ongoing logs)"""
    def parse_data_from_ref(ref_path):
        ref = db.reference(ref_path)
        data = ref.get()
        records = []
        
        if not data:
            return records

        for record_id, record in data.items():
            try:
                temperature = float(record['temperature'])
                humidity = float(record['humidity'])
                light = float(record['light'])
                moisture_before = float(record['moisture_before'])
                predicted_time = float(record.get('predicted_time', 0))
                moisture_after = float(record['moisture_after'])

                if predicted_time > 0:
                    records.append({
                        'temperature': temperature,
                        'humidity': humidity,
                        'light': light,
                        'moisture_before': moisture_before,
                        'predicted_time': predicted_time,
                        'moisture_after': moisture_after
                    })
            except Exception as e:
                print(f"Error in record {record_id}: {e}")
        
        print(f"Fetched {len(records)} records from {ref_path}")
        return records

    initial_records = parse_data_from_ref('initial_training_logs')
    new_records = parse_data_from_ref('training_logs')

    all_records = initial_records + new_records
    return pd.DataFrame(all_records)

def calculate_optimal_durations(df):
    """Calculate what duration would have achieved exactly TARGET_MOISTURE"""
    # Only use records where watering was actually performed
    df = df[df['predicted_time'] > 0].copy()
    
    if len(df) == 0:
        logger.debug("No records with predicted time > 0")
        return df
    
    # Calculate moisture change rate (% per second)
    df['moisture_change_per_sec'] = (df['moisture_after'] - df['moisture_before']) / df['predicted_time']
    
    # Optional: Filter out unrealistic rates (0.01 to 1.0 %/second)
    valid = df[(df['moisture_change_per_sec'] > 0.01) & 
               (df['moisture_change_per_sec'] < 1.0) &
               (df['moisture_after'] > df['moisture_before'])]
    
    logger.debug(f"Filtered down to {len(valid)} valid records")
    
    # Calculate optimal duration for TARGET_MOISTURE
    valid['required_increase'] = np.maximum(TARGET_MOISTURE - valid['moisture_before'], 0)
    valid['optimal_duration'] = valid['required_increase'] / valid['moisture_change_per_sec']
    
    # Apply duration constraints
    valid['optimal_duration'] = valid['optimal_duration'].clip(MIN_DURATION, MAX_DURATION)
    
    logger.debug(f"Calculated optimal durations for {len(valid)} valid records")
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
    logger.debug("Model training completed")
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
    with open('base_modelv2.pkl', 'wb') as f:
        pickle.dump(model_info, f)
    
    # Upload to GCS
    storage_client = storage.Client()
    bucket = storage_client.bucket(os.getenv("BUCKET"))
    blob = bucket.blob('base_model.pkl')
    blob.upload_from_filename('base_modelv2.pkl')
    logger.info(f"Model saved to GCS with {record_count} records")

@functions_framework.http
def retrain_model(request):
    """HTTP Cloud Function to retrain the watering model"""
    try:
        logger.debug("Starting retraining process")
        initialize_firebase()
        
        # 1. Fetch and prepare retrain data
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