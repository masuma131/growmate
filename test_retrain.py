import requests

# Replace this with your actual URL
url = "https://us-central1-growmate-455421.cloudfunctions.net/retrain_model"

try:
    response = requests.post(url)
    print("Status Code:", response.status_code)
    print("Response JSON:")
    print(response.json())
except Exception as e:
    print("Error occurred:", e)
