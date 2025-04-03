import firebase_admin
from firebase_admin import credentials, db
import pandas as pd

# Initialize Firebase (replace with your service account key)
cred = credentials.Certificate("sep600-proje-firebase-adminsdk-fbsvc-e65e14e3d8.json")
firebase_admin.initialize_app(cred, {
    'databaseURL': 'https://sep600-proje-default-rtdb.firebaseio.com/'
})

# Prepare your data
data = {
    'timestamp': pd.to_datetime([
        '2025-03-19 10:24:00', '2025-03-19 16:49:39', '2025-03-19 16:56:54',
        '2025-03-19 16:57:16', '2025-03-19 16:58:53', '2025-03-19 17:58:54',
        '2025-03-19 19:05:01', '2025-03-22 16:00:45', '2025-03-22 17:00:46',
        '2025-03-22 18:54:15', '2025-03-23 00:50:07', '2025-03-23 00:50:46',
        '2025-03-23 00:55:55', '2025-03-23 12:30:00', '2025-03-23 15:45:00'
    ]).astype(str).tolist(),
    'temp': [24.2, 22.5, 21.0, 20.9, 20.7, 19.9, 18.3, 23.8, 23.0, 21.0, 20.2, 20.2, 20.2, 25.0, 26.5],
    'humidity': [61.9, 63.0, 67.9, 68.1, 69.0, 73.0, 79.7, 63.5, 65.1, 75.1, 74.4, 74.5, 74.8, 60.0, 58.0],
    'light': [19.2, 0.0, 15.0, 11.7, 35.0, 23.3, 0.0, 0.0, 29.2, 5.8, 2.5, 2.5, 5.0, 50.0, 70.0],
    'init_moisture': [38.7, 35.2, 36.5, 37.1, 34.8, 39.5, 32.1, 37.6, 36.2, 33.9, 31.8, 32.5, 34.1, 39.9, 37.3],
    'watering_duration': [10, 15, 12, 8, 18, 9, 20, 11, 14, 16, 22, 19, 15, 7, 13],
    'final_moisture': [48.5, 51.8, 49.2, 45.0, 52.5, 47.3, 50.2, 48.9, 50.5, 49.8, 53.0, 51.2, 49.5, 46.0, 50.1]
}

# Convert to list of records
records = []
for i in range(len(data['timestamp'])):
    record = {key: data[key][i] for key in data}
    records.append(record)

# Upload to Firebase
ref = db.reference('sensor_data')  # Change 'sensor_data' to your desired path
for record in records:
    ref.push().set(record)

print("Data uploaded successfully!")