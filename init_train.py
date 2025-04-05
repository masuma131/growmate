import pandas as pd
import firebase_admin
from firebase_admin import credentials, db
from sklearn.ensemble import RandomForestRegressor
import pickle
import os
from dotenv import load_dotenv
from datetime import datetime

load_dotenv()

# Initialize Firebase
cred = credentials.Certificate(os.getenv("CERTIFICATE"))
firebase_admin.initialize_app(cred, {
    'databaseURL': os.getenv("DATABASE_URL")
})

def get_firebase_data():
    """Get all data from Firebase sensor_data node with timestamp processing"""
    ref = db.reference('initial_training_logs')
    data = ref.get()
    
    records = []
    for record_id, record in data.items():
        try:
            records.append({
                'temperature': float(record['temperature']),
                'humidity': float(record['humidity']),
                'light': float(record['light']),
                'moisture_before': float(record['moisture_before']),
                'predicted_time': float(record['predicted_time']),,
                'moisture_after': float(record['moisture_after'])
            })
        except (KeyError, ValueError) as e:
            print(f"Skipping record {record_id}: {str(e)}")
            continue
            
    return pd.DataFrame(records)

def train_model(df):
    """Train Random Forest model with final moisture around 50%"""
    # Filter data for training
    filtered = df[
        (df['moisture_after'] >= 47) & 
        (df['moisture_after'] <= 53)
    ].copy()
    
    features = ['temperature', 'humidity', 'light', 'moisture_before']
    X = filtered[features]
    y = filtered['watering_duration']
    
    model = RandomForestRegressor(
        n_estimators=150,
        max_depth=7,
        min_samples_split=5,
        random_state=42
    )
    model.fit(X, y)

    model_info = {
        'model': model,
        'feature_names': features,
        'training_date': datetime.now().isoformat(),
        'records_used': len(filtered)
    }

    return model_info

# Main execution
if __name__ == '__main__':
    print("Fetching data from Firebase...")
    df = get_firebase_data()
    
    if len(df) == 0:
        print("Error: No data fetched from Firebase")
    else:
        print(f"Retrieved {len(df)} records")
        print("Training model...")
        model_info = train_model(df)
        
        # Save model with metadata
        with open('base_model.pkl', 'wb') as f:
            pickle.dump(model_info, f)
            
        print(f"Model trained on {model_info['records_used']} records")
        print("Features used:", model_info['feature_names'])