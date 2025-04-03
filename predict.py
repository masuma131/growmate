import functions_framework
import pickle
import numpy as np
from google.cloud import storage
from datetime import datetime
import traceback

import os
from dotenv import load_dotenv

load_dotenv()


storage_client = storage.Client()
bucket = storage_client.bucket(os.getenv("BUCKET"))

@functions_framework.http
def predict_watering(request):
    try:
        # Load model with metadata
        with open("base_model.pkl", "rb") as f:
            model_info = pickle.load(f)
            model = model_info['model']
            expected_features = model_info['feature_names']
        
        # Get and validate input
        request_json = request.get_json()
        if not request_json:
            return {"error": "No JSON payload"}, 400
        
        # Prepare features in EXACT same order as training
        features = [
            float(request_json["temperature"]),
            float(request_json["humidity"]),
            float(request_json["light"]),
            float(request_json["moisture_before"]),
        ]
        
        # Make prediction
        duration = model.predict([features])[0]
        
        return {"watering_duration": float(duration)}
            # "features_used": expected_features,
            # "status": "success"
        
    except Exception as e:
        return {"error": str(e)}, 500